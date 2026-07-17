#pragma once

#include "filesystem_error.hpp"
#include "filesystem_types.hpp"

#include <optional>
#include <stdint.h>
#include <vector>

namespace thin_io {

// A point in time relative to the Unix epoch, mirroring struct timespec: nanoseconds is always a positive offset from
// seconds, including for pre-1970 times, where seconds is negative.
struct timestamp {
	int64_t seconds = 0;
	uint32_t nanoseconds = 0; // [0, 999'999'999]

	[[nodiscard]] bool operator==(const timestamp&) const noexcept = default;
};

// What a nullopt member means depends on the direction: set_times() leaves that timestamp untouched, get_times()
// reports that the platform or the filesystem does not provide it.
struct entry_times {
	std::optional<timestamp> creation;
	std::optional<timestamp> last_access;
	std::optional<timestamp> last_write;
};

// False on Linux, where the birth time is assigned by the kernel at inode creation and no API can change it -
// entry_times::creation is silently ignored there. Windows and Darwin can both set it.
inline constexpr bool creation_time_settable =
#if defined(_WIN32) || defined(__APPLE__)
	true;
#else
	false;
#endif

// Applies the requested timestamps to an existing file or directory, following links. Requesting nothing succeeds
// without touching the path at all, and so does requesting only a creation time where that isn't settable. Returns
// false on failure, in which case file::error_code() and file::text_for_last_error() report the native reason.
[[nodiscard]] bool set_times(const char* path, const entry_times& times) noexcept;
#ifdef _WIN32
[[nodiscard]] bool set_times(const wchar_t* path, const entry_times& times) noexcept;
#endif

// Reports the timestamps of an existing file or directory, following links. The access and modification times are
// always reported; the creation time is reported on Windows and Darwin, and on Linux only where the kernel and the
// filesystem provide it - note that this is wider than what creation_time_settable promises, which is about writing.
// A timestamp the filesystem does not keep at all is reported as nullopt rather than as an epoch value, so feeding the
// result straight back into set_times() transfers whatever the source has and the destination will accept.
// Returns nullopt on failure, in which case file::error_code() and file::text_for_last_error() report the reason.
[[nodiscard]] std::optional<entry_times> get_times(const char* path) noexcept;
#ifdef _WIN32
[[nodiscard]] std::optional<entry_times> get_times(const wchar_t* path) noexcept;
#endif

// Lists the immediate children of one directory. Returned names are native and relative to path; no entry is
// recursively traversed. A failure after enumeration has started fails the whole listing rather than returning a
// partial vector.
[[nodiscard]] filesystem_result<std::vector<directory_entry>> list_directory(const char* path);
#ifdef _WIN32
[[nodiscard]] filesystem_result<std::vector<directory_entry>> list_directory(const wchar_t* path);
#endif

} // namespace thin_io
