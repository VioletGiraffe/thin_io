#include "catch_thin_io.hpp"

#include "buffered_reader.hpp"

#include <span>
#include <utility>
#include <vector>

using namespace thin_io;

static constexpr const char testFilePath[] = "buffered_reader_test.file";
// Spans two window refills and ends mid-window
static constexpr uint64_t testFileSize = 2 * buffered_reader::default_window_size + 12345;

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

	REQUIRE(!reader.open("buffered_reader_missing.file"));
	REQUIRE(!reader.is_open());
}

// Requires the state of a newly constructed reader, which opens and reads normally
static void requireMovedFrom(buffered_reader& reader)
{
	REQUIRE(!reader.is_open());
	REQUIRE(reader.pos() == 0u);
	std::byte byte{};
	REQUIRE(!reader.read(&byte, 1));
	REQUIRE(!reader.peek(1));

	REQUIRE(reader.open(testFilePath));
	requireReadAt(reader, 100, 8);
}

TEST_CASE("buffered_reader: moving", "[buffered_reader]")
{
	const TestFile testFile;
	buffered_reader source;
	REQUIRE(source.open(testFilePath));
	requireReadAt(source, 100, 8);

	SECTION("construction")
	{
		buffered_reader target{ std::move(source) };
		REQUIRE(target.pos() == 108u);
		requireReadAt(target, 0, 8);
		requireMovedFrom(source);
	}

	SECTION("assignment")
	{
		buffered_reader target;
		REQUIRE(target.open(testFilePath));
		target = std::move(source);
		REQUIRE(target.pos() == 108u);
		requireReadAt(target, 0, 8);
		requireMovedFrom(source);
	}
}

TEST_CASE("buffered_reader: close", "[buffered_reader]")
{
	const TestFile testFile;
	buffered_reader reader;
	REQUIRE(reader.open(testFilePath));
	requireReadAt(reader, 0, 8);

	reader.close();
	REQUIRE(!reader.is_open());
	reader.set_pos(0);
	std::byte byte{};
	REQUIRE(!reader.read(&byte, 1));
	REQUIRE(!reader.peek(1));

	REQUIRE(reader.open(testFilePath));
	requireReadAt(reader, 0, 8);
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
		requireReadAt(reader, buffered_reader::default_window_size + 700, 64);
		requireReadAt(reader, 100, 64);
		requireReadAt(reader, buffered_reader::default_window_size + 4096 - 10, 20); // straddles the end of the window read at 3
		requireReadAt(reader, 2 * buffered_reader::default_window_size + 9000, 100);
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
	requireReadAt(reader, 777, buffered_reader::default_window_size);
	requireReadAt(reader, 1, buffered_reader::default_window_size + 3);
}

TEST_CASE("buffered_reader: end of file", "[buffered_reader]")
{
	const TestFile testFile;
	buffered_reader reader;
	REQUIRE(reader.open(testFilePath));

	std::vector<std::byte> bytes(buffered_reader::default_window_size + 50);

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

	// Reopened for each read: a known end of file answers without reaching the file
	for (const uint64_t pos : { uint64_t{ 1 } << 63, ~uint64_t{ 1 } })
	{
		for (const size_t size : { size_t{ 50 }, bytes.size() })
		{
			REQUIRE(reader.open(testFilePath));
			requireReadAt(reader, 0, 8); // Leaves the Win32 file pointer at data
			reader.set_pos(pos);
			REQUIRE(reader.read(bytes.data(), size) == 0u);
			REQUIRE(reader.pos() == pos);
		}
	}
}

// Changes the file behind the reader: a reader that returns the old state issued no I/O for it
static void pwriteToTestFile(const std::span<const std::byte> bytes, const uint64_t pos)
{
	file writer;
	REQUIRE(writer.open(testFilePath, file::access_mode::ReadWrite, file::open_disposition::OpenExisting));
	REQUIRE(writer.pwrite(bytes.data(), bytes.size(), pos) == bytes.size());
	REQUIRE(writer.close());
}

TEST_CASE("buffered_reader: no I/O at a known end of file", "[buffered_reader]")
{
	const TestFile testFile;
	buffered_reader reader;
	REQUIRE(reader.open(testFilePath));
	std::vector<std::byte> bytes(buffered_reader::default_window_size + 50);

	SECTION("reads and peeks at or across it")
	{
		reader.set_pos(testFileSize - 10);
		REQUIRE(reader.read(bytes.data(), 50) == 10u);
		pwriteToTestFile(std::span{ bytes }.first(16), testFileSize);

		REQUIRE(reader.read(bytes.data(), 50) == 0u);
		REQUIRE(reader.read(bytes.data(), bytes.size()) == 0u);
		reader.set_pos(testFileSize - 10);
		const auto tail = reader.peek(50);
		REQUIRE(tail.has_value());
		REQUIRE(tail->size() == 10u);

		REQUIRE(reader.open(testFilePath));
		reader.set_pos(testFileSize - 10);
		REQUIRE(reader.read(bytes.data(), 50) == 26u);
	}

	SECTION("a read past it keeps the window")
	{
		requireReadAt(reader, 0, 8);
		// Far enough past the end for the window start, rounded down, to be past it too
		reader.set_pos(testFileSize + buffered_reader::default_window_size);
		REQUIRE(reader.read(bytes.data(), 50) == 0u);
		pwriteToTestFile(std::vector<std::byte>(8, std::byte{ 0xAA }), 0);
		requireReadAt(reader, 0, 8);
	}
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
		reader.set_pos(buffered_reader::default_window_size - 2);
		const auto peeked = reader.peek(buffered_reader::default_window_size);
		REQUIRE(peeked.has_value());
		REQUIRE(peeked->size() == buffered_reader::default_window_size);
		REQUIRE(matchesFileAt(*peeked, buffered_reader::default_window_size - 2));
	}

	SECTION("returns the whole request at an unaligned position")
	{
		reader.set_pos(100);
		const auto peeked = reader.peek(buffered_reader::default_window_size);
		REQUIRE(peeked.has_value());
		REQUIRE(peeked->size() == buffered_reader::default_window_size);
		REQUIRE(matchesFileAt(*peeked, 100));
	}

	SECTION("is limited to the window size and to the end of file")
	{
		const auto whole = reader.peek(buffered_reader::default_window_size * 2);
		REQUIRE(whole.has_value());
		REQUIRE(whole->size() == buffered_reader::default_window_size);

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

TEST_CASE("buffered_reader: window size", "[buffered_reader]")
{
	REQUIRE(buffered_reader{}.window_size() == buffered_reader::default_window_size);
	REQUIRE(buffered_reader{ 0 }.window_size() == 4096u);
	REQUIRE(buffered_reader{ 6000 }.window_size() == 4096u);
	REQUIRE(buffered_reader{ 8192 }.window_size() == 8192u);

	const TestFile testFile;
	buffered_reader reader{ 8192 };
	REQUIRE(reader.open(testFilePath));

	SECTION("small reads at scattered positions")
	{
		requireReadAt(reader, 5000, 16); // fills [4096, 16384)
		requireReadAt(reader, 16384 - 10, 20); // straddles its end
		requireReadAt(reader, 100000, 64);
		requireReadAt(reader, 4999, 16);
	}

	SECTION("peek is limited to it")
	{
		reader.set_pos(100);
		const auto peeked = reader.peek(3 * 8192);
		REQUIRE(peeked.has_value());
		REQUIRE(peeked->size() == 8192u);
		REQUIRE(matchesFileAt(*peeked, 100));
	}

	SECTION("reads at least as large as the window")
	{
		requireReadAt(reader, 777, 8192);
		requireReadAt(reader, 1, 3 * 8192 + 3);
	}

	SECTION("moving keeps it on both readers")
	{
		requireReadAt(reader, 100, 8);
		buffered_reader target{ std::move(reader) };
		REQUIRE(target.window_size() == 8192u);
		REQUIRE(reader.window_size() == 8192u);
		requireReadAt(target, 0, 8);
		requireMovedFrom(reader);
	}
}
