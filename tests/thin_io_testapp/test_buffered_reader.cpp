#include "catch_thin_io.hpp"

#include "buffered_reader.hpp"

#include <span>
#include <vector>

using namespace thin_io;

static constexpr const char testFilePath[] = "buffered_reader_test.file";
// Spans two window refills and ends mid-window
static constexpr uint64_t testFileSize = 2 * buffered_reader::window_size + 12345;

// A byte pattern that differs between nearby positions and between windows
static std::byte expectedByte(const uint64_t pos)
{
	return static_cast<std::byte>((pos * 7 + pos / 251) & 0xFF);
}

// Creates the test file for its lifetime; declared before the reader, so the file is closed before it is deleted
struct TestFile {
	TestFile();
	~TestFile() { file::delete_file(testFilePath); }
};

TestFile::TestFile()
{
	std::vector<std::byte> contents(testFileSize);
	for (uint64_t i = 0; i < testFileSize; ++i)
		contents[i] = expectedByte(i);

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::Write));
	REQUIRE(f.write(contents.data(), contents.size()) == contents.size());
	REQUIRE(f.close());
}

static bool matchesFileAt(const std::span<const std::byte> bytes, const uint64_t pos)
{
	for (uint64_t i = 0; i < bytes.size(); ++i)
	{
		if (bytes[i] != expectedByte(pos + i))
			return false;
	}
	return true;
}

// Reads size bytes at pos and checks them against the file's contents
static void requireReadAt(buffered_reader& reader, const uint64_t pos, const uint64_t size)
{
	reader.set_pos(pos);
	std::vector<std::byte> bytes(size);
	REQUIRE(reader.read(bytes.data(), size) == size);
	REQUIRE(matchesFileAt(bytes, pos));
	REQUIRE(reader.pos() == pos + size);
}

TEST_CASE("buffered_reader: open", "[buffered_reader]")
{
	{
		file::delete_file(testFilePath);
		buffered_reader reader;
		REQUIRE(!reader.is_open());
		REQUIRE(!reader.open(testFilePath));
		REQUIRE(!reader.is_open());
	}

	const TestFile testFile;
	buffered_reader reader;
	REQUIRE(reader.open(testFilePath));
	REQUIRE(reader.is_open());
	REQUIRE(reader.size() == testFileSize);
	REQUIRE(reader.pos() == 0u);

	reader.set_pos(100);
	REQUIRE(reader.open(testFilePath));
	REQUIRE(reader.pos() == 0u);
}

TEST_CASE("buffered_reader: small reads at scattered positions", "[buffered_reader]")
{
	const TestFile testFile;
	buffered_reader reader;
	REQUIRE(reader.open(testFilePath));

	SECTION("sequential reads across window boundaries")
	{
		std::vector<std::byte> bytes(1000);
		for (uint64_t pos = 0; pos + bytes.size() <= testFileSize; pos += bytes.size())
		{
			REQUIRE(reader.read(bytes.data(), bytes.size()) == bytes.size());
			REQUIRE(matchesFileAt(bytes, pos));
		}
	}

	SECTION("backward and forward seeks, within the window and outside it")
	{
		requireReadAt(reader, 5000, 16);
		requireReadAt(reader, 4999, 16);
		requireReadAt(reader, 3, 4);
		requireReadAt(reader, buffered_reader::window_size + 700, 64);
		requireReadAt(reader, 100, 64);
		requireReadAt(reader, buffered_reader::window_size - 10, 20); // straddles a window's end
		requireReadAt(reader, 2 * buffered_reader::window_size + 9000, 100);
		requireReadAt(reader, 0, 1);
	}
}

TEST_CASE("buffered_reader: reads at least as large as the window", "[buffered_reader]")
{
	const TestFile testFile;
	buffered_reader reader;
	REQUIRE(reader.open(testFilePath));

	// Partly served from the window the peek filled
	REQUIRE(reader.peek(3).has_value());
	requireReadAt(reader, 0, testFileSize);
	requireReadAt(reader, 777, buffered_reader::window_size);
	requireReadAt(reader, 1, buffered_reader::window_size + 3);
}

TEST_CASE("buffered_reader: end of file", "[buffered_reader]")
{
	const TestFile testFile;
	buffered_reader reader;
	REQUIRE(reader.open(testFilePath));

	std::vector<std::byte> bytes(buffered_reader::window_size + 50);

	reader.set_pos(testFileSize - 10);
	REQUIRE(reader.read(bytes.data(), 50) == 10u);
	REQUIRE(reader.pos() == testFileSize);
	REQUIRE(reader.read(bytes.data(), 50) == 0u);

	reader.set_pos(testFileSize - 10);
	REQUIRE(reader.read(bytes.data(), bytes.size()) == 10u);

	reader.set_pos(testFileSize + 1000);
	REQUIRE(reader.read(bytes.data(), 50) == 0u);
	REQUIRE(reader.read(bytes.data(), bytes.size()) == 0u);
	REQUIRE(reader.pos() == testFileSize + 1000);
}

TEST_CASE("buffered_reader: peek", "[buffered_reader]")
{
	const TestFile testFile;
	buffered_reader reader;
	REQUIRE(reader.open(testFilePath));

	SECTION("does not move the position")
	{
		reader.set_pos(300);
		const auto peeked = reader.peek(8);
		REQUIRE(peeked.has_value());
		REQUIRE(peeked->size() == 8u);
		REQUIRE(matchesFileAt(*peeked, 300));
		REQUIRE(reader.pos() == 300u);
		requireReadAt(reader, 300, 8);
	}

	SECTION("refills a window that holds only part of the request")
	{
		requireReadAt(reader, 0, 1);
		reader.set_pos(buffered_reader::window_size - 2);
		const auto peeked = reader.peek(8);
		REQUIRE(peeked.has_value());
		REQUIRE(peeked->size() == 8u);
		REQUIRE(matchesFileAt(*peeked, buffered_reader::window_size - 2));
	}

	SECTION("is limited to the window size and to the end of file")
	{
		const auto whole = reader.peek(buffered_reader::window_size * 2);
		REQUIRE(whole.has_value());
		REQUIRE(whole->size() == buffered_reader::window_size);

		reader.set_pos(testFileSize - 5);
		const auto tail = reader.peek(8);
		REQUIRE(tail.has_value());
		REQUIRE(tail->size() == 5u);

		reader.set_pos(testFileSize + 5);
		const auto pastEnd = reader.peek(8);
		REQUIRE(pastEnd.has_value());
		REQUIRE(pastEnd->empty());
	}
}
