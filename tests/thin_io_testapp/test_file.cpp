#include "catch2/catch.hpp"

#include "file.hpp"

#include <memory.h>

using namespace thin_io;

static_assert((file::sharing_mode::ShareRead | file::sharing_mode::ShareWrite | file::sharing_mode::ShareDelete)
			  == static_cast<file::sharing_mode>(1 | 2 | 4));

#ifdef _WIN32
#define REQUIRE_LINUX(...) (void)0
#define REQUIRE_WIN(...) REQUIRE(__VA_ARGS__)
#else
#define REQUIRE_WIN(...) (void)0
#define REQUIRE_LINUX(...) REQUIRE(__VA_ARGS__)

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
// Actual on-disk allocation in bytes: ~0 for a sparse file, >= the logical size when the space is really reserved
static uint64_t allocatedSizeOnDisk(const char* path)
{
	struct stat st;
	return ::stat(path, &st) == 0 ? static_cast<uint64_t>(st.st_blocks) * 512u : 0;
}
#endif

TEST_CASE("basic file functionality", "[file]")
{
try {
	static constexpr const char testFilePath[] = "test.file";
	{
		file::delete_file(testFilePath);

		file f;
		REQUIRE(!f);
		REQUIRE(!f.is_open());
		REQUIRE(f.open("", file::access_mode::Read) == false);
		REQUIRE(f.open(testFilePath, file::access_mode::Read) == false);
		REQUIRE(f.is_open() == false);
		REQUIRE(f.close() == false);

		REQUIRE(f.open("test.file", file::access_mode::ReadWrite) == true);
		REQUIRE(f);
		REQUIRE(f.is_open());
		constexpr const char testString[]{ "The quick brown fox jumps over the lazy dog"};
		REQUIRE(f.write(testString, std::size(testString)) == std::size(testString));
		REQUIRE(f.close());
		REQUIRE(!f);
		REQUIRE(!f.is_open());

		REQUIRE(f.open("test.file", file::access_mode::Read) == true);
		std::string s;
		s.resize(std::size(testString));
		REQUIRE(f.read(s.data(), std::size(testString)) == std::size(testString));
		REQUIRE(::memcmp(s.data(), testString, s.size()) == 0);
	}

	// Testing for auto-closing the file on scope exit - deleting will fail if it's still open
	REQUIRE(file::delete_file(testFilePath));
}
catch (...) {
	FAIL("file must not throw!");
}
}

TEST_CASE("create-write-close", "[file]")
{
try {
	static constexpr const char testFilePath[] = "test.file";
	{
		file::delete_file(testFilePath);

		file f;
		REQUIRE(f.open("test.file", file::access_mode::Write) == true);
		REQUIRE(f);
		REQUIRE(f.is_open());

		constexpr const char testString[]{ "The quick brown fox jumps over the lazy dog"};
		REQUIRE(f.write(testString, std::size(testString)) == std::size(testString));
		REQUIRE(f.close());
		REQUIRE(!f);
		REQUIRE(!f.is_open());

		REQUIRE(f.open("test.file", file::access_mode::Read) == true);
		REQUIRE(f.size() == std::size(testString));
		std::string s;
		s.resize(std::size(testString));
		REQUIRE(f.read(s.data(), std::size(testString)) == std::size(testString));
		REQUIRE(::memcmp(s.data(), testString, s.size()) == 0);
	}

	// Testing for auto-closing the file on scope exit - deleting will fail if it's still open
	REQUIRE(file::delete_file(testFilePath));
}
catch (...) {
	FAIL("file must not throw!");
}
}

TEST_CASE("open dispositions", "[file]")
{
	static constexpr char testFilePath[] = "test.file";
	static constexpr char originalContents[] = "existing contents";
	file::delete_file(testFilePath);

	file f;
	REQUIRE(!f.open(testFilePath, file::access_mode::Read, file::open_disposition::OpenExisting));
	REQUIRE(f.open(testFilePath, file::access_mode::ReadWrite, file::open_disposition::OpenOrCreate));
	REQUIRE(f.write(originalContents, sizeof(originalContents)) == sizeof(originalContents));
	REQUIRE(f.close());

	REQUIRE(f.open(testFilePath, file::access_mode::Write, file::open_disposition::OpenExisting));
	REQUIRE(f.size() == sizeof(originalContents));
	REQUIRE(!f.open(testFilePath, file::access_mode::Read, file::open_disposition::CreateOrTruncate));
	REQUIRE(!f.is_open());

	REQUIRE(!f.open(testFilePath, file::access_mode::Write, file::open_disposition::CreateNew));
	REQUIRE(!f.is_open());
	REQUIRE(f.open(testFilePath, file::access_mode::Read, file::open_disposition::OpenExisting));
	char contentsAfterFailedCreate[sizeof(originalContents)]{};
	REQUIRE(f.read(contentsAfterFailedCreate, sizeof(contentsAfterFailedCreate)) == sizeof(contentsAfterFailedCreate));
	REQUIRE(::memcmp(contentsAfterFailedCreate, originalContents, sizeof(originalContents)) == 0);
	REQUIRE(f.close());

	REQUIRE(f.open(testFilePath, file::access_mode::Write, file::open_disposition::OpenOrCreate));
	REQUIRE(f.size() == sizeof(originalContents));
	REQUIRE(f.close());

	REQUIRE(f.open(testFilePath, file::access_mode::Write, file::open_disposition::CreateOrTruncate));
	REQUIRE(f.size() == 0);
	REQUIRE(f.close());
	REQUIRE(file::delete_file(testFilePath));

	auto newFile = file::open_file(testFilePath, file::access_mode::Write, file::open_disposition::CreateNew);
	REQUIRE(newFile);
	REQUIRE(newFile.close());
#ifndef _WIN32
	struct stat createdFileInfo;
	REQUIRE(::stat(testFilePath, &createdFileInfo) == 0);
	REQUIRE((createdFileInfo.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0);
#endif
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("Navigating a file - read-only", "[file]")
{
try {
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	static constexpr const char testFilePath[] = "test.file";

	file::delete_file(testFilePath);

	{
		file f;
		REQUIRE(f.open(testFilePath, file::access_mode::Write));
		REQUIRE(f.write(testString, sizeof(testString)));
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.pos() == f.size());
		REQUIRE(f.at_end());
		REQUIRE(f.close());
	}

	{
		file f;
		REQUIRE(f.open(testFilePath, file::access_mode::Read));
		REQUIRE(f.pos() == 0);
		REQUIRE(f.at_end() == false);
		REQUIRE(f.set_pos(16));
		REQUIRE(f.pos() == 16);
		REQUIRE(f.at_end() == false);

		char buf[sizeof(testString)] = {0};
		REQUIRE(f.read(buf, 3) == 3);
		REQUIRE(::memcmp(buf, "fox", 3) == 0);

		REQUIRE(f.set_pos(sizeof(testString)));
		REQUIRE(f.pos() == sizeof(testString));
		REQUIRE(f.at_end() == true);

		REQUIRE(f.set_pos(40));
		REQUIRE(f.pos() == 40);
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.at_end() == false);
		memset(buf, ' ', sizeof(buf));
		REQUIRE(f.read(buf, 4) == 4);
		REQUIRE(::memcmp(buf, "dog", 4 /* with null terminator */) == 0);
		REQUIRE(f.pos() == 44);
		REQUIRE(f.at_end() == true);
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.close());
	}

	// Testing for auto-closing the file on scope exit - deleting will fail if it's still open
	REQUIRE(file::delete_file(testFilePath));
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

	file::delete_file(testFilePath);

	{
		file f;
		REQUIRE(f.open(testFilePath, file::access_mode::Write));
		REQUIRE(f.write(testString, sizeof(testString)));
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.pos() == f.size());
		REQUIRE(f.at_end());
		REQUIRE(f.set_pos(16));
		REQUIRE(f.pos() == 16);
		REQUIRE(f.at_end() == false);
		REQUIRE(f.write("dog", 3));
		REQUIRE(f.pos() == 16+3);
		REQUIRE(f.at_end() == false);
		REQUIRE(f.close());
	}

	{
		file f;
		REQUIRE(f.open(testFilePath, file::access_mode::Read));
		REQUIRE(f.pos() == 0);
		REQUIRE(f.at_end() == false);

		char buf[sizeof(testString)] = {0};
		REQUIRE(f.read(buf, sizeof(testString)) == sizeof(testString));
		REQUIRE(::memcmp(buf, testString2, sizeof(testString)) == 0);

		REQUIRE(f.pos() == sizeof(testString));
		REQUIRE(f.at_end() == true);
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.close());
	}

	// Testing for auto-closing the file on scope exit - deleting will fail if it's still open
	REQUIRE(file::delete_file(testFilePath));
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

	file::delete_file(testFilePath);

	{
		file f;
		REQUIRE(f.open(testFilePath, file::access_mode::Write));
		REQUIRE(f.write(testString, sizeof(testString)));
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.pos() == f.size());
		REQUIRE(f.at_end());
		REQUIRE(f.close());
	}

	{
		file f;
		REQUIRE(f.open(testFilePath, file::access_mode::ReadWrite));
		REQUIRE(f.pos() == 0);
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.at_end() == false);

		char buf[sizeof(testString)] = {0};
		REQUIRE(f.read(buf, sizeof(testString)) == sizeof(testString));
		REQUIRE(::memcmp(buf, testString, sizeof(testString)) == 0);

		REQUIRE(f.pos() == sizeof(testString));
		REQUIRE(f.at_end() == true);
		REQUIRE(f.size() == sizeof(testString));

		REQUIRE(f.set_pos(16));
		REQUIRE(f.pos() == 16);
		REQUIRE(f.at_end() == false);
		REQUIRE(f.size() == sizeof(testString));
		memset(buf, ' ', sizeof(buf));
		REQUIRE(f.read(buf, 3) == 3);

		REQUIRE(f.pos() == 19);
		REQUIRE(f.at_end() == false);
		REQUIRE(::memcmp(buf, "fox", 3) == 0);

		REQUIRE(f.set_pos(16));
		REQUIRE(f.pos() == 16);
		REQUIRE(f.at_end() == false);
		REQUIRE(f.size() == sizeof(testString));
		REQUIRE(f.write("dog", 3));
		REQUIRE(f.pos() == 16+3);
		REQUIRE(f.at_end() == false);
		REQUIRE(f.set_pos(16));
		REQUIRE(f.pos() == 16);
		REQUIRE(f.at_end() == false);
		REQUIRE(f.read(buf, 3) == 3);
		REQUIRE(f.pos() == 19);
		REQUIRE(f.at_end() == false);
		REQUIRE(::memcmp(buf, "dog", 3) == 0);

		REQUIRE(f.close());
	}

	// Testing for auto-closing the file on scope exit - deleting will fail if it's still open
	REQUIRE(file::delete_file(testFilePath));
}
catch (...) {
	FAIL("file must not throw!");
}
}

#ifndef _WIN32
TEST_CASE("failed close relinquishes descriptor ownership", "[file]")
{
	static constexpr char testFilePath[] = "test.file";
	static constexpr char guardFilePath[] = "close-guard.file";
	file::delete_file(testFilePath);
	file::delete_file(guardFilePath);

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::Write));

	struct stat targetInfo;
	REQUIRE(::stat(testFilePath, &targetInfo) == 0);

	int ownedDescriptor = -1;
	for (int descriptor = 0; descriptor < 4096; ++descriptor)
	{
		struct stat descriptorInfo;
		if (::fstat(descriptor, &descriptorInfo) == 0 && descriptorInfo.st_dev == targetInfo.st_dev && descriptorInfo.st_ino == targetInfo.st_ino)
		{
			ownedDescriptor = descriptor;
			break;
		}
	}
	REQUIRE(ownedDescriptor >= 0);

	// Simulate close() reporting an error after the descriptor has ceased to belong to the file object.
	REQUIRE(::close(ownedDescriptor) == 0);
	REQUIRE(!f.close());
	REQUIRE(!f.is_open());

	int guardDescriptor = ::open(guardFilePath, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	REQUIRE(guardDescriptor >= 0);
	if (guardDescriptor != ownedDescriptor)
	{
		REQUIRE(::dup2(guardDescriptor, ownedDescriptor) == ownedDescriptor);
		REQUIRE(::close(guardDescriptor) == 0);
		guardDescriptor = ownedDescriptor;
	}

	REQUIRE(!f.close());
	REQUIRE(::fcntl(guardDescriptor, F_GETFD) != -1);
	REQUIRE(::close(guardDescriptor) == 0);

	REQUIRE(file::delete_file(testFilePath));
	REQUIRE(file::delete_file(guardFilePath));
}
#endif

TEST_CASE("resize", "[file]")
{
try {
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::delete_file(testFilePath);

	file f;

	REQUIRE(f.open("test.file", file::access_mode::ReadWrite) == true);
	REQUIRE(f.write(testString, std::size(testString)) == std::size(testString));
	REQUIRE(f.at_end());
	REQUIRE(f.size() == sizeof(testString));
	const auto positionBeforeResize = f.pos();
	REQUIRE(f.resize(3));
	REQUIRE(f.size() == 3);
	REQUIRE(f.pos() == positionBeforeResize);
	REQUIRE(f.at_end());
	char buf[sizeof(testString)] = {0};
	REQUIRE(f.set_pos(0));
	REQUIRE(!f.at_end());
	REQUIRE(f.read(buf, 3) == 3);
	REQUIRE(f.at_end());
	REQUIRE(::memcmp(buf, "The", 3) == 0);

	const auto positionBeforeSecondResize = f.pos();
	REQUIRE(f.resize(0));
	REQUIRE(f.size() == 0);
	REQUIRE(f.pos() == positionBeforeSecondResize);
	REQUIRE(f.at_end());
	REQUIRE(f.read(buf, 1) == 0);
	REQUIRE(!f.resize(UINT64_MAX)); // Larger than any native file size type can represent
	REQUIRE(f.size() == 0);
	REQUIRE(f.close());
	REQUIRE(!f.at_end());

	// Testing for auto-closing the file on scope exit - deleting will fail if it's still open
	REQUIRE(file::delete_file(testFilePath));
}
catch (...) {
	FAIL("file must not throw!");
}
}

TEST_CASE("resize and preallocate", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	file::delete_file(testFilePath);

	static constexpr uint64_t largeSize = 4u * 1024u * 1024u; // Large enough that block-size rounding is negligible

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::ReadWrite));
	REQUIRE(f.size() == 0);
	REQUIRE(f.pos() == 0);
	REQUIRE(!f.preallocate(largeSize)); // Preallocation cannot extend the logical file
	REQUIRE(f.size() == 0);
	REQUIRE(f.pos() == 0);

	static constexpr char preservedContent[] = "existing data";
	REQUIRE(f.write(preservedContent, sizeof(preservedContent)) == sizeof(preservedContent));
	const auto positionBeforeGrowth = f.pos();
	REQUIRE(f.resize(largeSize));
	REQUIRE(f.size() == largeSize);
	REQUIRE(f.pos() == positionBeforeGrowth);
	REQUIRE(f.preallocate(largeSize));
	REQUIRE(f.size() == largeSize);
	REQUIRE(f.pos() == positionBeforeGrowth);
	REQUIRE_LINUX(allocatedSizeOnDisk(testFilePath) >= largeSize); // Linux + macOS: real allocation, not a sparse file

	REQUIRE(f.set_pos(0));
	char contentAfterPreallocation[sizeof(preservedContent)]{};
	REQUIRE(f.read(contentAfterPreallocation, sizeof(contentAfterPreallocation)) == sizeof(contentAfterPreallocation));
	REQUIRE(::memcmp(contentAfterPreallocation, preservedContent, sizeof(preservedContent)) == 0);

	const auto positionBeforeShrink = f.pos();
	REQUIRE(f.resize(1024));
	REQUIRE(f.size() == 1024);
	REQUIRE(f.pos() == positionBeforeShrink);

	REQUIRE(f.close());
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("Empty files", "[file]")
{
try {
	static constexpr const char testFilePath[] = "test.file";
	file::delete_file(testFilePath);

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::Write));
	REQUIRE(f.is_open() == true);
	REQUIRE(f.pos() == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.at_end());
	REQUIRE(f.write("", 0) == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.at_end());
	REQUIRE(f.set_pos(0));
	REQUIRE(f.resize(0));
	REQUIRE(f.preallocate(0));
	REQUIRE(f.close());

	REQUIRE(f.open(testFilePath, file::access_mode::ReadWrite));
	REQUIRE(f.is_open() == true);
	REQUIRE(f.pos() == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.at_end());
	REQUIRE(f.write("", 0) == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.at_end());
	char buf[1];
	REQUIRE(f.read(buf, 0) == 0);
	REQUIRE(f.read(buf, 0).has_value());
	REQUIRE(f.read(buf, 1).has_value());
	REQUIRE(f.read(buf, 1) == 0);
	REQUIRE_WIN(f.read(buf, 10000000).has_value() == false);
	REQUIRE_LINUX(f.read(buf, 10000000).value() == 0);
	REQUIRE(f.pos() == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.at_end());
	REQUIRE(f.set_pos(100)); // Seeking past EOF is valid and does not change the logical size.
	REQUIRE(f.size() == 0);
	REQUIRE(f.at_end());
	REQUIRE(f.close());


	REQUIRE(f.open(testFilePath, file::access_mode::Read));
	REQUIRE(f.is_open() == true);
	REQUIRE(f.pos() == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.at_end());
	REQUIRE(f.read(buf, 0) == 0);
	REQUIRE(f.read(buf, 1) == 0);
	REQUIRE(f.size() == 0);
	REQUIRE(f.at_end());
	REQUIRE(f.set_pos(0));
	REQUIRE(f.set_pos(100));
	REQUIRE(f.size() == 0);
	REQUIRE(f.at_end());
	REQUIRE(f.close());

	REQUIRE(file::delete_file(testFilePath));
}
catch (...) {
	FAIL("file must not throw!");
}
}

bool createTestFile(const char* path, const char* contents, size_t size)
{
	file f;
	if (!f.open(path, file::access_mode::Write)) return false;
	if (f.write(contents, size) != size) return false;
	if (!f.close()) return false;
	return true;
}

TEST_CASE("pread", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::delete_file(testFilePath);

	REQUIRE(createTestFile(testFilePath, testString, sizeof(testString)));

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::Read));
	char buf[sizeof(testString)];
	REQUIRE(f.pread(buf, 5, 20) == 5);
	REQUIRE(::memcmp(buf, "jumps", 5) == 0);
	REQUIRE_WIN(f.pos() == 25); // The documented platform divergence: Win32 pread moves the file position
	REQUIRE_LINUX(f.pos() == 0);
	REQUIRE(f.pread(buf, 3, 0) == 3);
	REQUIRE(::memcmp(buf, "The", 3) == 0);
	REQUIRE(f.pread(buf, 4, 40) == 4);
	REQUIRE(::memcmp(buf, "dog", 4) == 0);
	REQUIRE(f.pread(buf, 10, 40) == 4); // A read crossing EOF is partial
	REQUIRE(f.pread(buf, 4, sizeof(testString)) == 0); // At EOF
	REQUIRE(f.pread(buf, 4, sizeof(testString) + 100) == 0); // Past EOF
	REQUIRE(f.close());

	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("pwrite", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::delete_file(testFilePath);

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::Write));
	REQUIRE(f.pwrite(testString, sizeof(testString), 0) == sizeof(testString));
	REQUIRE_WIN(f.pos() == sizeof(testString)); // The documented platform divergence: Win32 pwrite moves the file position
	REQUIRE_LINUX(f.pos() == 0);
	REQUIRE(f.pwrite("small", 5, 4) == 5);
	REQUIRE(f.pwrite("cat", 3, 40) == 3);
	REQUIRE(f.pwrite("!", 1, sizeof(testString) + 16) == 1); // Past EOF: extends the file, zero-filling the gap
	REQUIRE(f.close());

	REQUIRE(f.open(testFilePath, file::access_mode::Read));
	REQUIRE(f.size() == sizeof(testString) + 17);
	char buf[sizeof(testString)];
	REQUIRE(f.read(buf, sizeof(testString)) == sizeof(testString));
	REQUIRE(::memcmp(buf, "The small brown fox jumps over the lazy cat", sizeof(testString)) == 0);
	char tail[17];
	REQUIRE(f.read(tail, sizeof(tail)) == sizeof(tail));
	const char zeros[16] = {};
	CHECK(::memcmp(tail, zeros, sizeof(zeros)) == 0);
	CHECK(tail[16] == '!');
	REQUIRE(f.close());

	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("open on an already open file switches to the new file", "[file]")
{
	static constexpr const char firstPath[] = "test-first.file";
	static constexpr const char secondPath[] = "test-second.file";
	file::delete_file(firstPath);
	file::delete_file(secondPath);
	REQUIRE(createTestFile(firstPath, "first", 5));
	REQUIRE(createTestFile(secondPath, "second", 6));

	file f;
	REQUIRE(f.open(firstPath, file::access_mode::Read));
	REQUIRE(f.open(secondPath, file::access_mode::Read));
	char buf[6];
	REQUIRE(f.read(buf, 6) == 6);
	CHECK(::memcmp(buf, "second", 6) == 0);

	// The reopen must have closed the first handle - on Windows the deletion would fail otherwise
	REQUIRE(file::delete_file(firstPath));

	REQUIRE(f.close());
	REQUIRE(file::delete_file(secondPath));
}

TEST_CASE("fsync and fdatasync", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	file::delete_file(testFilePath);

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::Write));
	REQUIRE(f.write("data", 4) == 4);
	REQUIRE(f.fsync());
	REQUIRE(f.fdatasync());
	REQUIRE(f.close());

	REQUIRE(!f.fsync()); // Closed file
	REQUIRE(!f.fdatasync());
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("NoOsCaching - aligned unbuffered round-trip", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	file::delete_file(testFilePath);

	static constexpr size_t ioSize = 8192; // A multiple of any common sector size, as unbuffered I/O requires
	alignas(4096) static uint8_t writeBuffer[ioSize];
	alignas(4096) static uint8_t readBuffer[ioSize];
	for (size_t i = 0; i < ioSize; ++i)
		writeBuffer[i] = static_cast<uint8_t>(i * 37);

	file f;
	if (!f.open(testFilePath, file::access_mode::Write, file::sys_cache_mode::NoOsCaching))
	{
		WARN("The test filesystem does not support unbuffered I/O; NoOsCaching assertions skipped");
		return;
	}

	REQUIRE(f.write(writeBuffer, ioSize) == ioSize);
	REQUIRE(f.close());

	REQUIRE(f.open(testFilePath, file::access_mode::Read, file::sys_cache_mode::NoOsCaching));
	REQUIRE(f.size() == ioSize);
	REQUIRE(f.read(readBuffer, ioSize) == ioSize);
	CHECK(::memcmp(writeBuffer, readBuffer, ioSize) == 0);
	REQUIRE(f.close());
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("text_for_error produces a description", "[file]")
{
	CHECK(!file::text_for_error(2).empty()); // ERROR_FILE_NOT_FOUND and ENOENT both happen to be 2
}

TEST_CASE("write-read sharing", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::delete_file(testFilePath);

	file fw;
	REQUIRE(fw.open(testFilePath, file::access_mode::Write));
	REQUIRE(fw.write(testString, sizeof(testString)) == sizeof(testString));

	file fr;
	REQUIRE(fr.open(testFilePath, file::access_mode::Read));
	char buf[sizeof(testString)] = { 0 };
	REQUIRE(fr.read(buf, 1) == 1);
	REQUIRE(buf[0] == 'T');

	REQUIRE(fw.close());
	REQUIRE(fr.close());

	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("mmap - readonly", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::delete_file(testFilePath);

	static constexpr auto size = sizeof(testString);
	REQUIRE(createTestFile(testFilePath, testString, size));

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::Read));

	uint64_t offset = 0;
	SECTION("0 offset") {
		offset = 0;
	}

	SECTION("non-0 offset") {
		offset = 5;
	}

	auto* addr = f.mmap(file::mmap_access_mode::ReadOnly, offset, size - offset);
	REQUIRE(addr);

	std::byte buf[size];
	::memset(buf, 255, size);

	::memcpy(buf, addr, size - offset);
	REQUIRE(::memcmp(buf, testString + offset, size - offset) == 0);

	REQUIRE(f.unmap(addr));

	REQUIRE(f.close());

	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("mmap - write", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::delete_file(testFilePath);

	static constexpr auto size = sizeof(testString);
	REQUIRE(createTestFile(testFilePath, testString, 0));

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::ReadWrite));
	REQUIRE(f.resize(size));

	uint64_t offset = 0;
	SECTION("0 offset") {
		offset = 0;
	}
	SECTION("non-0 offset") {
		offset = 12;
	}

	auto* addr = f.mmap(file::mmap_access_mode::ReadWrite, offset, size - offset);
	REQUIRE(addr);

	::memcpy(addr, testString, size - offset);

	REQUIRE(f.close());

	f.open(testFilePath, file::access_mode::Read);
	char buf[size];
	::memset(buf, 255, size);

	REQUIRE(f.size() == size);
	REQUIRE(f.set_pos(offset));
	REQUIRE(f.read(buf, size - offset) == size - offset);
	REQUIRE(::memcmp(buf, testString, size - offset) == 0);
	REQUIRE(f.close());

	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("mmap - offset past the first page", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char marker[] = "marker";
	static constexpr uint64_t offset = 100 * 1024 + 15; // Past any supported page size, and not page-aligned
	file::delete_file(testFilePath);

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::ReadWrite, file::open_disposition::CreateOrTruncate));
	REQUIRE(f.resize(offset + sizeof(marker)));

	SECTION("read") {
		REQUIRE(f.pwrite(marker, sizeof(marker), offset) == sizeof(marker));
		auto* addr = f.mmap(file::mmap_access_mode::ReadOnly, offset, sizeof(marker));
		REQUIRE(addr);
		REQUIRE(::memcmp(addr, marker, sizeof(marker)) == 0);
		REQUIRE(f.unmap(addr));
	}

	SECTION("write") {
		auto* addr = f.mmap(file::mmap_access_mode::ReadWrite, offset, sizeof(marker));
		REQUIRE(addr);
		::memcpy(addr, marker, sizeof(marker));
		REQUIRE(f.unmap(addr));

		char buf[sizeof(marker)];
		::memset(buf, 0, sizeof(buf));
		REQUIRE(f.pread(buf, sizeof(marker), offset) == sizeof(marker));
		REQUIRE(::memcmp(buf, marker, sizeof(marker)) == 0);
	}

	REQUIRE(f.size() == offset + sizeof(marker)); // Mapping the tail of the file must not have grown it
	REQUIRE(f.close());
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("unmap - only a currently mapped address unmaps", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	static constexpr auto size = sizeof(testString);
	file::delete_file(testFilePath);
	REQUIRE(createTestFile(testFilePath, testString, size));

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::Read));
	auto* first = f.mmap(file::mmap_access_mode::ReadOnly, 0, size);
	auto* second = f.mmap(file::mmap_access_mode::ReadOnly, 0, size);
	REQUIRE(first);
	REQUIRE(second);

	CHECK(!f.unmap(nullptr));
	REQUIRE(f.unmap(first));
	CHECK(!f.unmap(first)); // No longer mapped
	CHECK(::memcmp(second, testString, size) == 0); // The second mapping is independent and still alive
	REQUIRE(f.unmap(second));

	REQUIRE(f.close());
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("mmap - the requested range must be non-empty and within the file", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	static constexpr auto size = sizeof(testString);
	file::delete_file(testFilePath);
	REQUIRE(createTestFile(testFilePath, testString, size));

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::ReadWrite));

	CHECK(f.mmap(file::mmap_access_mode::ReadWrite, 0, 0) == nullptr);
	CHECK(f.mmap(file::mmap_access_mode::ReadWrite, 0, size + 1) == nullptr);
	CHECK(f.mmap(file::mmap_access_mode::ReadWrite, size, 1) == nullptr);
	CHECK(f.mmap(file::mmap_access_mode::ReadOnly, size + 100, 1) == nullptr);
	REQUIRE(f.size() == size); // No failed request may have grown the file

	auto* addr = f.mmap(file::mmap_access_mode::ReadOnly, 0, size); // The full valid range still maps
	REQUIRE(addr);
	REQUIRE(f.unmap(addr));
	REQUIRE(f.close());
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("mmap - mappings survive moving the file object", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	static constexpr auto size = sizeof(testString);
	file::delete_file(testFilePath);
	REQUIRE(createTestFile(testFilePath, testString, size));

	file moved;
	void* addr = nullptr;
	{
		file original;
		REQUIRE(original.open(testFilePath, file::access_mode::ReadWrite));
		addr = original.mmap(file::mmap_access_mode::ReadWrite, 0, size);
		REQUIRE(addr);
		static_cast<char*>(addr)[0] = 'X';

		SECTION("move construction") {
			file constructed{std::move(original)};
			moved = std::move(constructed);
		}
		SECTION("move assignment") {
			moved = std::move(original);
		}
		REQUIRE(!original.is_open());
		REQUIRE(moved.is_open());
	} // The moved-from object is destroyed here; the mapping must remain owned by 'moved'

	REQUIRE(moved.unmap(addr));
	REQUIRE(moved.close());

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::Read));
	char buf[size];
	::memset(buf, 0, size);
	REQUIRE(f.read(buf, size) == size);
	REQUIRE(buf[0] == 'X');
	REQUIRE(::memcmp(buf + 1, testString + 1, size - 1) == 0);
	REQUIRE(f.close());
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("handle-based times round-trip", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::delete_file(testFilePath);
	REQUIRE(createTestFile(testFilePath, testString, sizeof(testString)));

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::ReadWrite));

	const auto initial = f.times();
	REQUIRE(initial);
	CHECK(initial->last_access);
	CHECK(initial->last_write);

	// All values are multiples of 100 ns so that NTFS can represent them exactly
	entry_times requested;
	if constexpr (creation_time_settable)
		requested.creation = timestamp{ .seconds = 1'400'000'000, .nanoseconds = 100'000'000 };
	requested.last_access = timestamp{ .seconds = 1'500'000'000, .nanoseconds = 250'000'000 };
	requested.last_write = timestamp{ .seconds = 1'600'000'000, .nanoseconds = 500'000'000 };
	REQUIRE(f.set_times(requested));

	auto actual = f.times();
	REQUIRE(actual);
	CHECK(actual->last_access == requested.last_access);
	CHECK(actual->last_write == requested.last_write);
	if constexpr (creation_time_settable)
		CHECK(actual->creation == requested.creation);

	// A nullopt member leaves the current value untouched
	entry_times writeOnly;
	writeOnly.last_write = timestamp{ .seconds = 1'700'000'000, .nanoseconds = 0 };
	REQUIRE(f.set_times(writeOnly));
	actual = f.times();
	REQUIRE(actual);
	CHECK(actual->last_write == writeOnly.last_write);
	CHECK(actual->last_access == requested.last_access);

	REQUIRE(f.close());
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("handle-based permissions round-trip", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	file::delete_file(testFilePath);
	REQUIRE(createTestFile(testFilePath, "x", 1));

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::ReadWrite));

	const auto initial = f.permissions();
	REQUIRE(initial);
#ifdef _WIN32
	CHECK(*initial == file_permissions{});
	REQUIRE(f.set_permissions(file_permissions{ .read_only = true, .hidden = true, .system = true }));
	const auto changed = f.permissions();
	REQUIRE(changed);
	CHECK(changed->read_only);
	CHECK(changed->hidden);
	CHECK(changed->system);
	REQUIRE(f.set_permissions(file_permissions{})); // Clear everything back so that the file can be deleted
	CHECK(f.permissions() == file_permissions{});
#else
	REQUIRE(f.set_permissions(file_permissions{ .mode = 0754 }));
	CHECK(f.permissions()->mode == 0754);
	REQUIRE(f.set_permissions(file_permissions{ .mode = 0100644 })); // File-type bits from a raw st_mode are masked away
	CHECK(f.permissions()->mode == 0644);
	REQUIRE(f.set_permissions(file_permissions{ .mode = 0600 }));
	CHECK(f.permissions()->mode == 0600);
#endif

	REQUIRE(f.close());
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("handle-based set_times - empty and unsupported requests succeed and change nothing", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	file::delete_file(testFilePath);
	REQUIRE(createTestFile(testFilePath, "x", 1));

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::ReadWrite));

	entry_times known;
	known.last_access = timestamp{ .seconds = 1'500'000'000, .nanoseconds = 250'000'000 };
	known.last_write = timestamp{ .seconds = 1'600'000'000, .nanoseconds = 500'000'000 };
	REQUIRE(f.set_times(known));
	const auto before = f.times();
	REQUIRE(before);

	REQUIRE(f.set_times(entry_times{})); // Nothing requested
	if constexpr (!creation_time_settable)
	{
		entry_times creationOnly;
		creationOnly.creation = timestamp{ .seconds = 1'400'000'000, .nanoseconds = 0 };
		REQUIRE(f.set_times(creationOnly)); // Silently ignored where the platform cannot write it
	}

	const auto after = f.times();
	REQUIRE(after);
	CHECK(after->creation == before->creation);
	CHECK(after->last_access == before->last_access);
	CHECK(after->last_write == before->last_write);

	REQUIRE(f.close());
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("handle-based metadata reads work on a read-only open", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	file::delete_file(testFilePath);
	REQUIRE(createTestFile(testFilePath, "x", 1));

	file f;
	REQUIRE(f.open(testFilePath, file::access_mode::Read));

	const auto times = f.times();
	REQUIRE(times);
	CHECK(times->last_write);
	REQUIRE(f.permissions());

	entry_times someTime;
	someTime.last_write = timestamp{ .seconds = 1'600'000'000, .nanoseconds = 0 };
	REQUIRE_WIN(!f.set_times(someTime)); // A read-only open carries no FILE_WRITE_ATTRIBUTES access

	REQUIRE(f.close());
	REQUIRE(f.open(testFilePath, file::access_mode::Write, file::open_disposition::OpenExisting));
	REQUIRE(f.times()); // Must work despite the write-only open having no read access
	REQUIRE(f.permissions());

	REQUIRE(f.close());
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("handle-based metadata calls on a closed file", "[file]")
{
	file f;
	CHECK(!f.times());
	CHECK(!f.permissions());

	entry_times someTime;
	someTime.last_write = timestamp{ .seconds = 1'600'000'000, .nanoseconds = 0 };
	CHECK(!f.set_times(someTime));
	CHECK(f.set_times(entry_times{})); // Requesting nothing succeeds without touching the handle, like the path-based contract

#ifdef _WIN32
	CHECK(!f.set_permissions(file_permissions{ .read_only = true }));
#else
	CHECK(!f.set_permissions(file_permissions{ .mode = 0644 }));
#endif
}

TEST_CASE("Factory method", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	static constexpr const char testString[] = "The quick brown fox jumps over the lazy dog";
	file::delete_file(testFilePath);

	auto f = file::open_file(testFilePath, file::access_mode::Write);
	REQUIRE(f);
	REQUIRE(f.write(testString, sizeof(testString)) == sizeof(testString));
	REQUIRE(f.close());
	REQUIRE(!f);

	f = file::open_file(testFilePath, file::access_mode::Read);
	REQUIRE(f);
	char buf[sizeof(testString)] = { 0 };
	REQUIRE(f.read(buf, sizeof(testString)) == sizeof(testString));
	REQUIRE(f);
	REQUIRE(f.close());
	REQUIRE(!f);
	REQUIRE(::memcmp(buf, testString, sizeof(testString)) == 0);

	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("Moving a file object", "[file]")
{
	static constexpr const char testFilePath[] = "test.file";
	file::delete_file(testFilePath);
	REQUIRE(createTestFile(testFilePath, "0", 0));

	auto f = file::open_file(testFilePath, file::access_mode::Read);
	REQUIRE(f);
	auto f2 = std::move(f);
	REQUIRE(f2);
	REQUIRE(!f);

	file f3;
	REQUIRE(!f3);

	f3 = std::move(f2);
	REQUIRE(f3);
	REQUIRE(!f2);

	REQUIRE(f3.close());
	REQUIRE(!f3);

	REQUIRE(file::delete_file(testFilePath));
}
