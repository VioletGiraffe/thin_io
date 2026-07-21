#include "catch2/catch.hpp"

#include "file.hpp" // file::error_code
#include "fs.hpp"

#include <stdint.h>
#include <time.h>

using namespace thin_io;

#ifdef _WIN32

#include <Windows.h>

static bool createDirectory(const char* path) { return ::CreateDirectoryA(path, nullptr) != 0; }
static bool removeDirectory(const char* path) { return ::RemoveDirectoryA(path) != 0; }
static bool createDirectory(const wchar_t* path) { return ::CreateDirectoryW(path, nullptr) != 0; }
static bool removeDirectory(const wchar_t* path) { return ::RemoveDirectoryW(path) != 0; }

// Deliberately bypasses set_times(), so that get_times() can be checked against a value the platform itself produced
static bool setLastWriteNatively(const char* path, const SYSTEMTIME& utc)
{
	FILETIME fileTime;
	if (::SystemTimeToFileTime(&utc, &fileTime) == 0)
		return false;

	const HANDLE h = ::CreateFileA(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	const bool success = ::SetFileTime(h, nullptr, nullptr, &fileTime) != 0;
	::CloseHandle(h);
	return success;
}

#else

#include <errno.h>
#include <fcntl.h>    // AT_FDCWD
#include <sys/stat.h> // utimensat, UTIME_OMIT
#include <unistd.h>

static bool createDirectory(const char* path) { return ::mkdir(path, 0755) == 0; }
static bool removeDirectory(const char* path) { return ::rmdir(path) == 0; }

// Deliberately bypasses set_times(), so that get_times() can be checked against a value written by a bare syscall
static bool setLastWriteNatively(const char* path, const int64_t unixSeconds)
{
	timespec ts[2];
	ts[0].tv_sec = 0;
	ts[0].tv_nsec = UTIME_OMIT;
	ts[1].tv_sec = static_cast<time_t>(unixSeconds);
	ts[1].tv_nsec = 0;
	return ::utimensat(AT_FDCWD, path, ts, 0) == 0;
}

#endif

template <class Character>
static bool createEmptyFile(const Character* path)
{
	file f;
	return f.open(path, file::access_mode::Write, file::open_disposition::CreateOrTruncate) && f.close();
}

// get_times() converts out of the platform's own representation, so one test has to pin it against a value the platform
// itself produced: an epoch constant that is wrong but applied consistently in both directions would round-trip
// perfectly and go unnoticed. This is that test. Every test below verifies through get_times() on the strength of it -
// which is sound, because pinning the read direction and then round-tripping through it pins the write direction too.
TEST_CASE("get_times reads a known absolute instant", "[fs]")
{
	static constexpr char testFilePath[] = "get-times-epoch.file";
	file::delete_file(testFilePath);
	REQUIRE(createEmptyFile(testFilePath));

#ifdef _WIN32
	SYSTEMTIME utc{};
	utc.wYear = 2001;
	utc.wMonth = 9;
	utc.wDay = 9;
	utc.wHour = 1;
	utc.wMinute = 46;
	utc.wSecond = 40;
	REQUIRE(setLastWriteNatively(testFilePath, utc));
#else
	REQUIRE(setLastWriteNatively(testFilePath, 1'000'000'000));
#endif

	const auto times = get_times(testFilePath);
	REQUIRE(times);
	CHECK(times->last_write == timestamp{ .seconds = 1'000'000'000 }); // 2001-09-09 01:46:40 UTC

	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("get_times reports a newly created file and directory", "[fs]")
{
	static constexpr char testFilePath[] = "get-times-fresh.file";
	static constexpr char testDirectoryPath[] = "get-times-fresh-dir";
	file::delete_file(testFilePath);
	removeDirectory(testDirectoryPath);

	const auto createdNoEarlierThan = static_cast<int64_t>(::time(nullptr));
	REQUIRE(createEmptyFile(testFilePath));
	REQUIRE(createDirectory(testDirectoryPath));
	const auto createdNoLaterThan = static_cast<int64_t>(::time(nullptr));

	const char* const paths[] { testFilePath, testDirectoryPath };
	for (const char* path : paths)
	{
		INFO("path: " << path);

		const auto times = get_times(path);
		REQUIRE(times);
		REQUIRE(times->last_access);
		REQUIRE(times->last_write);
#if defined(_WIN32) || defined(__APPLE__)
		REQUIRE(times->creation); // Linux reports it only where the filesystem keeps one, so it cannot be required
#endif

		// Loose on purpose: the window is here to catch a timestamp landing in the wrong century, not to time the disk
		CHECK(times->last_write->seconds >= createdNoEarlierThan - 5);
		CHECK(times->last_write->seconds <= createdNoLaterThan + 5);
	}

	REQUIRE(file::delete_file(testFilePath));
	REQUIRE(removeDirectory(testDirectoryPath));
}

TEST_CASE("get_times follows a directory link to its target", "[fs][link]")
{
	static constexpr char targetPath[] = "get-times-link-target.dir";
	static constexpr char linkPath[] = "get-times-link.dir";
#ifdef _WIN32
	removeDirectory(linkPath); // A directory symbolic link is a directory entry
#else
	::unlink(linkPath);
#endif
	removeDirectory(targetPath);

	REQUIRE(createDirectory(targetPath));
	entry_times targetTimes;
	targetTimes.last_write = timestamp{ .seconds = 1'234'567'890 };
	REQUIRE(set_times(targetPath, targetTimes));

#ifdef _WIN32
	static constexpr DWORD directoryUnprivilegedCreate = 0x1 /*SYMBOLIC_LINK_FLAG_DIRECTORY*/ | 0x2 /*SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE*/;
	if (::CreateSymbolicLinkA(linkPath, targetPath, directoryUnprivilegedCreate) == 0)
	{
		WARN("Symbolic-link creation is unavailable; link-following times assertion skipped");
		REQUIRE(removeDirectory(targetPath));
		return;
	}
#else
	REQUIRE(::symlink(targetPath, linkPath) == 0);
#endif

	const auto times = get_times(linkPath);
	REQUIRE(times);
	CHECK(times->last_write == timestamp{ .seconds = 1'234'567'890 }); // The target's time, not the link entry's own

#ifdef _WIN32
	REQUIRE(removeDirectory(linkPath));
#else
	REQUIRE(::unlink(linkPath) == 0);
#endif
	REQUIRE(removeDirectory(targetPath));
}

TEST_CASE("get_times fails for a path that does not exist", "[fs]")
{
	static constexpr char missingPath[] = "get-times-no-such-entry.file";
	file::delete_file(missingPath);

	REQUIRE(!get_times(missingPath));

	const auto error = file::error_code();
#ifdef _WIN32
	CHECK(error == ERROR_FILE_NOT_FOUND);
#else
	CHECK(error == ENOENT);
#endif
}

template <class Character>
[[nodiscard]] static entry_times readTimes(const Character* path)
{
	const auto times = get_times(path);
	REQUIRE(times);
	return *times;
}

// Distinct values throughout, so that mixing two of the three up is a failure rather than a coincidence. The
// nanoseconds are whole multiples of 100 to survive the round trip through NTFS, whose resolution is 100 ns.
static constexpr timestamp testCreation { .seconds = 1'100'000'000, .nanoseconds = 500'000'000 };
static constexpr timestamp testLastAccess { .seconds = 1'234'567'890, .nanoseconds = 123'456'700 };
static constexpr timestamp testLastWrite { .seconds = 1'400'000'000, .nanoseconds = 987'654'300 };

[[nodiscard]] static entry_times allTestTimes()
{
	entry_times times;
	if constexpr (creation_time_settable)
		times.creation = testCreation;

	times.last_access = testLastAccess;
	times.last_write = testLastWrite;
	return times;
}

TEST_CASE("set_times round-trips every supported timestamp of a file", "[fs]")
{
	static constexpr char testFilePath[] = "set-times.file";
	file::delete_file(testFilePath);
	REQUIRE(createEmptyFile(testFilePath));

	REQUIRE(set_times(testFilePath, allTestTimes()));

	const entry_times actual = readTimes(testFilePath);
	CHECK(actual.last_access == testLastAccess);
	CHECK(actual.last_write == testLastWrite);
	if constexpr (creation_time_settable)
		CHECK(actual.creation == testCreation);

	REQUIRE(file::delete_file(testFilePath));
}

// The reason set_times() exists: QFile::setFileTime() cannot reach a directory, because Qt never opens one.
TEST_CASE("set_times sets the timestamps of a directory", "[fs]")
{
	static constexpr char testDirectoryPath[] = "set-times-dir";
	removeDirectory(testDirectoryPath);
	REQUIRE(createDirectory(testDirectoryPath));

	REQUIRE(set_times(testDirectoryPath, allTestTimes()));

	const entry_times actual = readTimes(testDirectoryPath);
	CHECK(actual.last_access == testLastAccess);
	CHECK(actual.last_write == testLastWrite);
	if constexpr (creation_time_settable)
		CHECK(actual.creation == testCreation);

	REQUIRE(removeDirectory(testDirectoryPath));
}

#ifdef _WIN32
TEST_CASE("native-wide timestamp APIs round-trip a Unicode file and directory", "[fs][windows]")
{
	static constexpr wchar_t testFilePath[] = L"set-times-\u0444\u0430\u0439\u043B.file";
	static constexpr wchar_t testDirectoryPath[] = L"set-times-\u043A\u0430\u0442\u0430\u043B\u043E\u0433";
	file::delete_file(testFilePath);
	removeDirectory(testDirectoryPath);
	REQUIRE(createEmptyFile(testFilePath));
	REQUIRE(createDirectory(testDirectoryPath));

	const wchar_t* const paths[] { testFilePath, testDirectoryPath };
	for (const wchar_t* path : paths)
	{
		REQUIRE(set_times(path, allTestTimes()));

		const entry_times actual = readTimes(path);
		CHECK(actual.creation == testCreation);
		CHECK(actual.last_access == testLastAccess);
		CHECK(actual.last_write == testLastWrite);
	}

	REQUIRE(file::delete_file(testFilePath));
	REQUIRE(removeDirectory(testDirectoryPath));
}
#endif

TEST_CASE("set_times leaves omitted timestamps untouched", "[fs]")
{
	static constexpr char testFilePath[] = "set-times-omit.file";
	file::delete_file(testFilePath);
	REQUIRE(createEmptyFile(testFilePath));
	REQUIRE(set_times(testFilePath, allTestTimes()));

	static constexpr timestamp newLastWrite { .seconds = 1'500'000'000, .nanoseconds = 111'111'100 };
	entry_times onlyLastWrite;
	onlyLastWrite.last_write = newLastWrite;
	REQUIRE(set_times(testFilePath, onlyLastWrite));

	const entry_times actual = readTimes(testFilePath);
	CHECK(actual.last_write == newLastWrite);
	CHECK(actual.last_access == testLastAccess);
	if constexpr (creation_time_settable)
		CHECK(actual.creation == testCreation);

	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("set_times with nothing requested succeeds and changes nothing", "[fs]")
{
	static constexpr char testFilePath[] = "set-times-empty.file";
	file::delete_file(testFilePath);
	REQUIRE(createEmptyFile(testFilePath));
	REQUIRE(set_times(testFilePath, allTestTimes()));

	REQUIRE(set_times(testFilePath, entry_times{}));

	const entry_times actual = readTimes(testFilePath);
	CHECK(actual.last_access == testLastAccess);
	CHECK(actual.last_write == testLastWrite);
	if constexpr (creation_time_settable)
		CHECK(actual.creation == testCreation);

	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("set_times accepts pre-1970 timestamps", "[fs]")
{
	static constexpr char testFilePath[] = "set-times-negative.file";
	file::delete_file(testFilePath);
	REQUIRE(createEmptyFile(testFilePath));

	// A positive nanosecond offset from a negative second is the case that a truncating division gets wrong
	static constexpr timestamp beforeTheEpoch { .seconds = -100'000'000, .nanoseconds = 500'000'000 }; // 1966-11-24 17:33:20 UTC
	entry_times requested;
	requested.last_write = beforeTheEpoch;
	REQUIRE(set_times(testFilePath, requested));

	CHECK(readTimes(testFilePath).last_write == beforeTheEpoch);

	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("set_times fails for a path that does not exist", "[fs]")
{
	static constexpr char missingPath[] = "set-times-no-such-entry.file";
	file::delete_file(missingPath);

	entry_times requested;
	requested.last_write = testLastWrite;
	REQUIRE(!set_times(missingPath, requested));

	const auto error = file::error_code();
#ifdef _WIN32
	CHECK(error == ERROR_FILE_NOT_FOUND);
#else
	CHECK(error == ENOENT);
#endif
}

#ifdef _WIN32
TEST_CASE("set_times rejects times outside the representable FILETIME range", "[fs]")
{
	static constexpr char testFilePath[] = "set-times-out-of-range.file";
	file::delete_file(testFilePath);
	REQUIRE(createEmptyFile(testFilePath));
	REQUIRE(set_times(testFilePath, allTestTimes()));

	entry_times requested;

	SECTION("before the FILETIME epoch of 1601") {
		requested.last_write = timestamp{ .seconds = -12'000'000'000 };
	}

	SECTION("too far in the future for the tick arithmetic") {
		requested.last_write = timestamp{ .seconds = INT64_MAX };
	}

	REQUIRE(!set_times(testFilePath, requested));
	CHECK(file::error_code() == ERROR_INVALID_PARAMETER);

	// The range check precedes opening the handle, so nothing can have been written
	const entry_times actual = readTimes(testFilePath);
	CHECK(actual.last_write == testLastWrite);
	CHECK(actual.last_access == testLastAccess);
	CHECK(actual.creation == testCreation);

	REQUIRE(file::delete_file(testFilePath));
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
TEST_CASE("set_times ignores a creation time the platform cannot write", "[fs]")
{
	static_assert(!creation_time_settable);

	static constexpr char testFilePath[] = "set-times-ignored-creation.file";
	file::delete_file(testFilePath);
	REQUIRE(createEmptyFile(testFilePath));

	// Requesting the impossible is not an error, and must not stop the other two from being applied
	entry_times requested;
	requested.creation = testCreation;
	requested.last_access = testLastAccess;
	requested.last_write = testLastWrite;
	REQUIRE(set_times(testFilePath, requested));

	const entry_times actual = readTimes(testFilePath);
	CHECK(actual.last_access == testLastAccess);
	CHECK(actual.last_write == testLastWrite);

	REQUIRE(file::delete_file(testFilePath));
}
#endif
