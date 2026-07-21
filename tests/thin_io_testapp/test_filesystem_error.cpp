#include "catch2/catch.hpp"

#include "filesystem_error.hpp"

#include <errno.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

using namespace thin_io;

namespace {

#ifdef _WIN32

[[nodiscard]] static filesystem_result<int> captureFailureBeforeCleanup(const HANDLE cleanupHandle)
{
	::SetLastError(ERROR_ACCESS_DENIED);
	const filesystem_error capturedError = capture_last_filesystem_error();
	::CloseHandle(cleanupHandle);
	::SetLastError(ERROR_FILE_NOT_FOUND);
	return std::unexpected{capturedError};
}

#else

[[nodiscard]] static filesystem_result<int> captureFailureBeforeCleanup(const int cleanupDescriptor)
{
	errno = EACCES;
	const filesystem_error capturedError = capture_last_filesystem_error();
	::close(cleanupDescriptor);
	errno = ENOENT;
	return std::unexpected{capturedError};
}

#endif

} // namespace

TEST_CASE("filesystem_result distinguishes an empty success from failure", "[filesystem-error]")
{
	const filesystem_result<std::vector<int>> emptySuccess = std::vector<int>{};
	REQUIRE(emptySuccess);
	CHECK(emptySuccess->empty());

	const filesystem_error error{ .native_code = 42 };
	const filesystem_result<std::vector<int>> failure = std::unexpected{error};
	REQUIRE_FALSE(failure);
	CHECK(failure.error() == error);
}

TEST_CASE("captured filesystem errors survive cleanup and later native calls", "[filesystem-error]")
{
#ifdef _WIN32
	const HANDLE cleanupHandle = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
	REQUIRE(cleanupHandle != nullptr);
	const filesystem_result<int> result = captureFailureBeforeCleanup(cleanupHandle);
	const filesystem_error expected{ .native_code = ERROR_ACCESS_DENIED };
	CHECK(::GetLastError() == ERROR_FILE_NOT_FOUND);
#else
	int descriptors[2];
	REQUIRE(::pipe(descriptors) == 0);
	const filesystem_result<int> result = captureFailureBeforeCleanup(descriptors[0]);
	const int laterNativeError = errno;
	::close(descriptors[1]);
	const filesystem_error expected{ .native_code = EACCES };
	CHECK(laterNativeError == ENOENT);
#endif

	REQUIRE_FALSE(result);
	CHECK(result.error() == expected);
}

TEST_CASE("filesystem error formatting retains the native code", "[filesystem-error]")
{
#ifdef _WIN32
	const filesystem_error error{ .native_code = ERROR_FILE_NOT_FOUND };
#else
	const filesystem_error error{ .native_code = ENOENT };
#endif

	const std::string text = format_filesystem_error(error);
	CHECK(text.starts_with(std::to_string(error.native_code)));
	CHECK(text.find(": ") != std::string::npos);
}

#ifdef _WIN32
TEST_CASE("filesystem error formatting handles a code with no description", "[filesystem-error]")
{
	// Codes beyond the int range have no std::system_category description, so only the number remains
	const filesystem_error error{ .native_code = 0xFFFF'FFFFu };
	CHECK(format_filesystem_error(error) == "4294967295");
}
#endif
