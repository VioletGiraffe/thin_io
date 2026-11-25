#pragma once

#include "filesystem_error.hpp"
#include "filesystem_types.hpp"

#include <optional>
#include <vector>

namespace thin_io {

// Applies the requested timestamps to an existing file or directory, or with do_not_follow to a link itself. Requesting
// nothing succeeds without touching the path at all, and so does requesting only a creation time where that isn't
// settable. Returns false on failure, in which case file::error_code() and file::text_for_last_error() report the native reason.
[[nodiscard]] bool set_times(const char* path, const entry_times& times, link_behavior linkBehavior = link_behavior::follow) noexcept;
#ifdef _WIN32
[[nodiscard]] bool set_times(const wchar_t* path, const entry_times& times, link_behavior linkBehavior = link_behavior::follow) noexcept;
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

// Succeeds when list_directory() could start enumerating path: it is a directory, and the caller may list it.
// Reads no entries.
[[nodiscard]] filesystem_result<void> can_list_directory(const char* path) noexcept;
#ifdef _WIN32
[[nodiscard]] filesystem_result<void> can_list_directory(const wchar_t* path) noexcept;
#endif

// Reports one entry as list_directory() does with listing_detail::full, without opening it: works for entries that
// cannot be opened, such as a Windows paging file. A trailing separator is ignored.
// name is empty for a root. Otherwise it is the filesystem's spelling on Windows, and the path's last component on POSIX:
// - macOS: in the stored case where only ASCII case differs;
// - Linux has no call for the spelling: a case-insensitive volume, such as FAT, returns the name in the case asked for.
// Windows: wildcard characters in the name fail.
// Windows, parent not listable: name is the path's last component, and a reparse point must be openable.
[[nodiscard]] filesystem_result<directory_entry> get_directory_entry(const char* path);
#ifdef _WIN32
[[nodiscard]] filesystem_result<directory_entry> get_directory_entry(const wchar_t* path);
#endif

// Reports one entry itself, or with follow the target of a link, from the entry's own record: never from a directory listing.
// Cheaper than get_entry_metadata(): Windows opens no handle unless the entry is a reparse point.
// permissions is always set.
[[nodiscard]] filesystem_result<entry_status> get_entry_status(const char* path, link_behavior linkBehavior) noexcept;
#ifdef _WIN32
[[nodiscard]] filesystem_result<entry_status> get_entry_status(const wchar_t* path, link_behavior linkBehavior) noexcept;
#endif

// Reads metadata for one filesystem entry. linkBehavior explicitly selects whether a symbolic link or reparse point
// is inspected itself or resolved to its target.
[[nodiscard]] filesystem_result<entry_metadata> get_entry_metadata(const char* path, link_behavior linkBehavior) noexcept;
#ifdef _WIN32
[[nodiscard]] filesystem_result<entry_metadata> get_entry_metadata(const wchar_t* path, link_behavior linkBehavior) noexcept;
#endif

// Sets the permissions of an existing file or directory. do_not_follow never touches a link's target.
// do_not_follow on a link: Windows and Darwin change the link itself, Linux fails with EOPNOTSUPP.
// Linux, do_not_follow: requires /proc and glibc 2.32, otherwise fails with EOPNOTSUPP.
[[nodiscard]] filesystem_result<void> set_permissions(const char* path, file_permissions permissions, link_behavior linkBehavior) noexcept;
#ifdef _WIN32
[[nodiscard]] filesystem_result<void> set_permissions(const wchar_t* path, file_permissions permissions, link_behavior linkBehavior) noexcept;
#endif

// Creates one directory. Fails when the path already exists, or when its parent does not.
[[nodiscard]] filesystem_result<void> create_directory(const char* path) noexcept;
#ifdef _WIN32
[[nodiscard]] filesystem_result<void> create_directory(const wchar_t* path) noexcept;
#endif

// Renames the entry itself within one filesystem: a link is renamed, not its target, and nothing is ever copied.
// existing_destination::fail - one atomic call that fails when destination exists.
//   Windows: another name of the source itself, such as a case respelling or a hard link, does not count as existing.
//   POSIX: ENOSYS on a platform without such a call; a filesystem that lacks it reports EINVAL or ENOTSUP.
// existing_destination::replace - atomically replaces a file or a link at destination.
//   Windows: never replaces a directory.
//   POSIX: a directory also replaces an empty directory.
[[nodiscard]] filesystem_result<void> rename_entry(const char* source, const char* destination, existing_destination ifDestinationExists) noexcept;
#ifdef _WIN32
[[nodiscard]] filesystem_result<void> rename_entry(const wchar_t* source, const wchar_t* destination, existing_destination ifDestinationExists) noexcept;
#endif

// Removes the entry itself: a file, an empty directory, or a link of any kind, never the link's target.
// Fails on a non-empty directory, and on a Windows read-only file.
[[nodiscard]] filesystem_result<void> remove_entry(const char* path) noexcept;
#ifdef _WIN32
[[nodiscard]] filesystem_result<void> remove_entry(const wchar_t* path) noexcept;
#endif

// Reports what the link at path stores, never following it. A trailing separator is ignored.
// Fails on an entry that is not a link: EINVAL, or ERROR_NOT_A_REPARSE_POINT on Windows.
// Windows: fails with ERROR_NOT_SUPPORTED on any other kind of link, such as a volume mount point or a WSL symbolic link.
[[nodiscard]] filesystem_result<link_definition> read_link(const char* path);
#ifdef _WIN32
[[nodiscard]] filesystem_result<link_definition> read_link(const wchar_t* path);
#endif

// Creates a link at path, storing the definition's non-empty target verbatim. Fails when path exists.
// POSIX: only link_type::symbolic_link, otherwise EINVAL.
// Windows: a symbolic link needs Developer Mode or the symbolic-link privilege, without which it fails with
// ERROR_PRIVILEGE_NOT_HELD. A junction needs no privilege, but an absolute local target: ERROR_INVALID_PARAMETER otherwise.
// Windows: a junction starts as a directory, removed again when it cannot become one, such as on a filesystem without reparse points.
[[nodiscard]] filesystem_result<void> create_link(const char* path, const link_definition& definition) noexcept;
#ifdef _WIN32
[[nodiscard]] filesystem_result<void> create_link(const wchar_t* path, const link_definition& definition) noexcept;
#endif

// Reports space for the filesystem containing directoryPath. Symbolic links and reparse points are followed.
[[nodiscard]] filesystem_result<filesystem_space> get_filesystem_space(const char* directoryPath) noexcept;
#ifdef _WIN32
[[nodiscard]] filesystem_result<filesystem_space> get_filesystem_space(const wchar_t* directoryPath) noexcept;
#endif

} // namespace thin_io
