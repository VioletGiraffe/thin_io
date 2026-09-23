#pragma once

#include "filesystem_error.hpp"
#include "filesystem_types.hpp"

#include <optional>
#include <vector>

namespace thin_io {

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
// A timestamp the filesystem does not keep at all is reported unset, so feeding the result straight back into
// set_times() transfers whatever the source has and the destination will accept.
// Returns nullopt on failure, in which case file::error_code() and file::text_for_last_error() report the reason.
[[nodiscard]] std::optional<entry_times> get_times(const char* path) noexcept;
#ifdef _WIN32
[[nodiscard]] std::optional<entry_times> get_times(const wchar_t* path) noexcept;
#endif

// Lists the immediate children of one directory. Returned names are native and relative to path; no entry is
// recursively traversed. A failure after enumeration has started fails the whole listing rather than returning a
// partial vector. With listing_detail::full, an entry that vanishes before its stat is left out, and one whose stat
// fails otherwise keeps only what the enumeration returned.
[[nodiscard]] filesystem_result<std::vector<directory_entry>> list_directory(const char* path, listing_detail detail = listing_detail::basic);
#ifdef _WIN32
[[nodiscard]] filesystem_result<std::vector<directory_entry>> list_directory(const wchar_t* path, listing_detail detail = listing_detail::basic);
#endif

// Reports one entry as list_directory() does with listing_detail::full, without opening it: works for entries that
// cannot be opened, such as a Windows paging file. A trailing separator is ignored.
// name is empty for a root. Otherwise it is the path's last component on POSIX, and the filesystem's spelling on Windows.
// Windows: wildcard characters in the name fail.
// Windows, parent not listable: name is the path's last component, and a reparse point must be openable.
[[nodiscard]] filesystem_result<directory_entry> get_directory_entry(const char* path);
#ifdef _WIN32
[[nodiscard]] filesystem_result<directory_entry> get_directory_entry(const wchar_t* path);
#endif

// Reads metadata for one filesystem entry. linkBehavior explicitly selects whether a symbolic link or reparse point
// is inspected itself or resolved to its target.
[[nodiscard]] filesystem_result<entry_metadata> get_entry_metadata(const char* path, link_behavior linkBehavior) noexcept;
#ifdef _WIN32
[[nodiscard]] filesystem_result<entry_metadata> get_entry_metadata(const wchar_t* path, link_behavior linkBehavior) noexcept;
#endif

// Reports space for the filesystem containing directoryPath. Symbolic links and reparse points are followed.
[[nodiscard]] filesystem_result<filesystem_space> get_filesystem_space(const char* directoryPath) noexcept;
#ifdef _WIN32
[[nodiscard]] filesystem_result<filesystem_space> get_filesystem_space(const wchar_t* directoryPath) noexcept;
#endif

} // namespace thin_io
