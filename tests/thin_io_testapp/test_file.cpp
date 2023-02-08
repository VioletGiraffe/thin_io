#define CATCH_CONFIG_ENABLE_BENCHMARKING
#include "3rdparty/catch2/catch.hpp"

#include "file.hpp"

#include <memory.h>

using namespace thin_io;

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
	REQUIRE(f.atEnd());
	REQUIRE(f.size() == 3);
	REQUIRE(f.pos() == 3);
	char buf[sizeof(testString)] = {0};
	REQUIRE(f.setPos(0));
	REQUIRE(!f.atEnd());
	REQUIRE(f.read(buf, 3) == 3);
	REQUIRE(f.atEnd());
	REQUIRE(::memcmp(buf, "The", 3) == 0);

	REQUIRE(f.truncate(0));
	REQUIRE(f.atEnd());
	REQUIRE(f.size() == 0);
	REQUIRE(f.pos() == 0);
	REQUIRE(f.read(buf, 1) == 0);
	REQUIRE(f.close());

	// Testing for auto-closing the file on scope exit - deleting will fail if it's still open
	REQUIRE(file::deleteFile(testFilePath));
}
catch (...) {
	FAIL("file must not throw!");
}
}
