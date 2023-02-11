#define CATCH_CONFIG_ENABLE_BENCHMARKING
#include "3rdparty/catch2/catch.hpp"

#include "file.hpp"

#include <memory.h>

using namespace thin_io;

#ifdef _WIN32
#define REQUIRE_LINUX(...) (void)0
#define REQUIRE_WIN(...) REQUIRE(__VA_ARGS__)
#else
#define REQUIRE_WIN(...) (void)0
#define REQUIRE_LINUX(...) REQUIRE(__VA_ARGS__)
#endif

TEST_CASE("basic file functionality", "[file]")
{
try {
	static constexpr const char testFilePath[] = "test.file";
	{
		file::deleteFile(testFilePath);

		file f;
		REQUIRE(f.open("", file::Read) == false);
		REQUIRE(f.open(testFilePath, file::Read) == false);
		REQUIRE(f.is_open() == false);
		REQUIRE(f.close() == false);

		REQUIRE(f.open("test.file", file::ReadWrite) == true);
		constexpr const char testString[]{ "The quick brown fox jumps over the lazy dog"};
		REQUIRE(f.write(testString, std::size(testString)) == std::size(testString));
		REQUIRE(f.close());

		REQUIRE(f.open("test.file", file::Read) == true);
		std::string s;
		s.resize(std::size(testString));
		REQUIRE(f.read(s.data(), std::size(testString)) == std::size(testString));
		REQUIRE(::memcmp(s.data(), testString, s.size()) == 0);
	}

	// Testing for auto-closing the file on scope exit - deleting will fail if it's still open
	REQUIRE(file::deleteFile(testFilePath));
}
catch (...) {
	FAIL("file must not throw!");
}
}

TEST_CASE("Navigating a file - read-only", "[file]")
{
try {
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	static constexpr const char testFilePath[] = "test.file";

	file::deleteFile(testFilePath);

	{
		file f;
		REQUIRE(f.open(testFilePath, file::Write));
		REQUIRE(f.write(testString, sizeof(testString)));
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.pos() == f.size());
		REQUIRE(f.atEnd());
		REQUIRE(f.close());
	}

	{
		file f;
		REQUIRE(f.open(testFilePath, file::Read));
		REQUIRE(f.pos() == 0);
		REQUIRE(f.atEnd() == false);
		REQUIRE(f.setPos(16));
		REQUIRE(f.pos() == 16);
		REQUIRE(f.atEnd() == false);

		char buf[sizeof(testString)] = {0};
		REQUIRE(f.read(buf, 3) == 3);
		REQUIRE(::memcmp(buf, "fox", 3) == 0);

		REQUIRE(f.setPos(sizeof(testString)));
		REQUIRE(f.pos() == sizeof(testString));
		REQUIRE(f.atEnd() == true);

		REQUIRE(f.setPos(40));
		REQUIRE(f.pos() == 40);
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.atEnd() == false);
		memset(buf, ' ', sizeof(buf));
		REQUIRE(f.read(buf, 4) == 4);
		REQUIRE(::memcmp(buf, "dog", 4 /* with null terminator */) == 0);
		REQUIRE(f.pos() == 44);
		REQUIRE(f.atEnd() == true);
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.close());
	}

	// Testing for auto-closing the file on scope exit - deleting will fail if it's still open
	REQUIRE(file::deleteFile(testFilePath));
}
catch (...) {
	FAIL("file must not throw!");
}
}

TEST_CASE("Navigating a file - write-only", "[file]")
{
try {
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	static constexpr const char testString2[] = "The quick brown dog jumps over the lazy dog";
	static constexpr const char testFilePath[] = "test.file";

	file::deleteFile(testFilePath);

	{
		file f;
		REQUIRE(f.open(testFilePath, file::Write));
		REQUIRE(f.write(testString, sizeof(testString)));
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.pos() == f.size());
		REQUIRE(f.atEnd());
		REQUIRE(f.setPos(16));
		REQUIRE(f.pos() == 16);
		REQUIRE(f.atEnd() == false);
		REQUIRE(f.write("dog", 3));
		REQUIRE(f.pos() == 16+3);
		REQUIRE(f.atEnd() == false);
		REQUIRE(f.close());
	}

	{
		file f;
		REQUIRE(f.open(testFilePath, file::Read));
		REQUIRE(f.pos() == 0);
		REQUIRE(f.atEnd() == false);

		char buf[sizeof(testString)] = {0};
		REQUIRE(f.read(buf, sizeof(testString)) == sizeof(testString));
		REQUIRE(::memcmp(buf, testString2, sizeof(testString)) == 0);

		REQUIRE(f.pos() == sizeof(testString));
		REQUIRE(f.atEnd() == true);
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.close());
	}

	// Testing for auto-closing the file on scope exit - deleting will fail if it's still open
	REQUIRE(file::deleteFile(testFilePath));
}
catch (...) {
	FAIL("file must not throw!");
}
}

TEST_CASE("Navigating a file - read+write", "[file]")
{
try {
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	static constexpr const char testFilePath[] = "test.file";

	file::deleteFile(testFilePath);

	{
		file f;
		REQUIRE(f.open(testFilePath, file::Write));
		REQUIRE(f.write(testString, sizeof(testString)));
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.pos() == f.size());
		REQUIRE(f.atEnd());
		REQUIRE(f.close());
	}

	{
		file f;
		REQUIRE(f.open(testFilePath, file::ReadWrite));
		REQUIRE(f.pos() == 0);
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.atEnd() == false);

		char buf[sizeof(testString)] = {0};
		REQUIRE(f.read(buf, sizeof(testString)) == sizeof(testString));
		REQUIRE(::memcmp(buf, testString, sizeof(testString)) == 0);

		REQUIRE(f.pos() == sizeof(testString));
		REQUIRE(f.atEnd() == true);
		REQUIRE(f.size() == sizeof(testString));

		REQUIRE(f.setPos(16));
		REQUIRE(f.pos() == 16);
		REQUIRE(f.atEnd() == false);
		REQUIRE(f.size() == sizeof(testString));
		memset(buf, ' ', sizeof(buf));
		REQUIRE(f.read(buf, 3) == 3);

		REQUIRE(f.pos() == 19);
		REQUIRE(f.atEnd() == false);
		REQUIRE(::memcmp(buf, "fox", 3) == 0);

		REQUIRE(f.setPos(16));
		REQUIRE(f.pos() == 16);
		REQUIRE(f.atEnd() == false);
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.write("dog", 3));
		REQUIRE(f.pos() == 16+3);
		REQUIRE(f.atEnd() == false);
		REQUIRE(f.setPos(16));
		REQUIRE(f.pos() == 16);
		REQUIRE(f.atEnd() == false);
		REQUIRE(f.read(buf, 3) == 3);
		REQUIRE(f.pos() == 19);
		REQUIRE(f.atEnd() == false);
		REQUIRE(::memcmp(buf, "dog", 3) == 0);

		REQUIRE(f.close());
	}

	// Testing for auto-closing the file on scope exit - deleting will fail if it's still open
	REQUIRE(file::deleteFile(testFilePath));
}
catch (...) {
	FAIL("file must not throw!");
}
}

TEST_CASE("truncate", "[file]")
{
try {
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::deleteFile(testFilePath);

	file f;

	REQUIRE(f.open("test.file", file::ReadWrite) == true);
	REQUIRE(f.write(testString, std::size(testString)) == std::size(testString));
	REQUIRE(f.atEnd());
	REQUIRE(f.size() == sizeof(testString));
	REQUIRE(f.truncate(3));
	//REQUIRE(f.atEnd()); - truncate does not change file pointer on Windows!
	REQUIRE(f.size() == 3);
	//REQUIRE(f.pos() == 3); - truncate does not change file pointer on Windows!
	char buf[sizeof(testString)] = {0};
	REQUIRE(f.setPos(0));
	REQUIRE(!f.atEnd());
	REQUIRE(f.read(buf, 3) == 3);
	REQUIRE(f.atEnd());
	REQUIRE(::memcmp(buf, "The", 3) == 0);

	REQUIRE(f.truncate(0));
	//REQUIRE(f.atEnd()); - truncate does not change file pointer on Windows!
	REQUIRE(f.size() == 0);
	//REQUIRE(f.pos() == 0);
	REQUIRE(f.read(buf, 1) == 0);
	REQUIRE(f.close());

	// Testing for auto-closing the file on scope exit - deleting will fail if it's still open
	REQUIRE(file::deleteFile(testFilePath));
}
catch (...) {
	FAIL("file must not throw!");
}
}

TEST_CASE("Empty files", "[file]")
{
try {
	static constexpr const char testFilePath[] = "test.file";
	file::deleteFile(testFilePath);

	file f;
	REQUIRE(f.open(testFilePath, file::Write));
	REQUIRE(f.is_open() == true);
	REQUIRE(f.pos() == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.atEnd());
	REQUIRE(f.write("", 0) == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.atEnd());
	REQUIRE(f.setPos(0));
	REQUIRE(f.truncate(0));
	REQUIRE(f.close());

	REQUIRE(f.open(testFilePath, file::ReadWrite));
	REQUIRE(f.is_open() == true);
	REQUIRE(f.pos() == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.atEnd());
	REQUIRE(f.write("", 0) == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.atEnd());
	char buf[1];
	REQUIRE(f.read(buf, 0) == 0);
	REQUIRE(f.read(buf, 0).has_value());
	REQUIRE(f.read(buf, 1).has_value());
	REQUIRE(f.read(buf, 1) == 0);
	REQUIRE_WIN(f.read(buf, 10000000).has_value() == false);
	REQUIRE_LINUX(f.read(buf, 10000000).value() == 0);
	REQUIRE(f.pos() == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.atEnd());
	REQUIRE(f.setPos(100)); // The call succeeds - the file is read/write! Seek punches a hole.
	REQUIRE(f.size() == 0);
	REQUIRE(f.close());


	REQUIRE(f.open(testFilePath, file::Read));
	REQUIRE(f.is_open() == true);
	REQUIRE(f.pos() == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.atEnd());
	REQUIRE(f.read(buf, 0) == 0);
	REQUIRE(f.read(buf, 1) == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.atEnd());
	REQUIRE(f.setPos(0));
	REQUIRE(f.setPos(100));
	REQUIRE(f.size() == 0);
	//REQUIRE(f.atEnd());
	REQUIRE(f.close());

	REQUIRE(file::deleteFile(testFilePath));
}
catch (...) {
	FAIL("file must not throw!");
}
}

bool createTestFile(const char* path, const char* contents, size_t size)
{
	file f;
	if (!f.open(path, file::Write)) return false;
	if (f.write(contents, size) != size) return false;
	if (!f.close()) return false;
	return true;
}

TEST_CASE("pread", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::deleteFile(testFilePath);

	REQUIRE(createTestFile(testFilePath, testString, sizeof(testString)));

	file f;
	REQUIRE(f.open(testFilePath, file::Read));
	char buf[sizeof(testString)];
	REQUIRE(f.pread(buf, 5, 20) == 5);
	REQUIRE(::memcmp(buf, "jumps", 5) == 0);
	REQUIRE(f.pread(buf, 3, 0) == 3);
	REQUIRE(::memcmp(buf, "The", 3) == 0);
	REQUIRE(f.pread(buf, 4, 40) == 4);
	REQUIRE(::memcmp(buf, "dog", 4) == 0);
	REQUIRE(f.close());

	REQUIRE(file::deleteFile(testFilePath));
}

TEST_CASE("pwrite", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::deleteFile(testFilePath);

	file f;
	REQUIRE(f.open(testFilePath, file::Write));
	REQUIRE(f.pwrite(testString, sizeof(testString), 0) == sizeof(testString));
	REQUIRE(f.pwrite("small", 5, 4) == 5);
	REQUIRE(f.pwrite("cat", 3, 40) == 3);
	REQUIRE(f.close());

	REQUIRE(f.open(testFilePath, file::Read));
	char buf[sizeof(testString)];
	REQUIRE(f.read(buf, sizeof(testString)) == sizeof(testString));
	REQUIRE(::memcmp(buf, "The small brown fox jumps over the lazy cat", sizeof(testString)) == 0);
	REQUIRE(f.close());

	REQUIRE(file::deleteFile(testFilePath));
}

TEST_CASE("write-read sharing", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::deleteFile(testFilePath);

	file fw;
	REQUIRE(fw.open(testFilePath, file::Write));
	REQUIRE(fw.write(testString, sizeof(testString)) == sizeof(testString));

	file fr;
	REQUIRE(fr.open(testFilePath, file::Read));
	char buf[sizeof(testString)] = { 0 };
	REQUIRE(fr.read(buf, 1) == 1);
	REQUIRE(buf[0] == 'T');

	REQUIRE(fw.close());
	REQUIRE(fr.close());

	REQUIRE(file::deleteFile(testFilePath));
}

TEST_CASE("Factory method", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::deleteFile(testFilePath);

	auto f = file::create(testFilePath, file::Write);
	REQUIRE(f);
	REQUIRE(f.write(testString, sizeof(testString)) == sizeof(testString));
	REQUIRE(f.close());
	REQUIRE(!f);

	f = file::create(testFilePath, file::Read);
	REQUIRE(f);
	char buf[sizeof(testString)] = { 0 };
	REQUIRE(f.read(buf, sizeof(testString)) == sizeof(testString));
	REQUIRE(f);
	REQUIRE(f.close());
	REQUIRE(!f);
	REQUIRE(::memcmp(buf, testString, sizeof(testString)) == 0);

	REQUIRE(file::deleteFile(testFilePath));
}
