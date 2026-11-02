#pragma once

// What the inotify and FSEvents backends of directory_watcher share.

#include "directory_watcher.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace thin_io {

[[nodiscard]] std::string joinedPath(std::string_view directory, std::string_view name);
[[nodiscard]] bool isSameOrUnder(std::string_view path, std::string_view directory) noexcept;
[[nodiscard]] bool isExcluded(std::string_view relativePath, const std::vector<native_string>& excludedDirectories) noexcept;
// Without trailing separators
[[nodiscard]] std::vector<native_string> normalizedRelativeDirectories(std::vector<native_string> directories);

// Appends the event unless watch_options::excluded_directories drops it:
//   under an excluded directory - dropped;
//   a rename across an excluded directory's boundary - a creation or a removal;
//   an excluded directory's own modification or metadata change - dropped.
void appendUnlessExcluded(watch_event event, const std::vector<native_string>& excludedDirectories, std::vector<watch_event>& events);

// For both ends of a pipe: macOS has no pipe2(). Fails with errno set.
[[nodiscard]] bool setNonBlockingAndCloseOnExec(int descriptor) noexcept;
// poll() for input on one descriptor; false on timeout. A negative timeout waits indefinitely.
[[nodiscard]] filesystem_result<bool> waitUntilReadable(int descriptor, std::chrono::milliseconds timeout);
// Reads a non-blocking descriptor until it would block
void drainDescriptor(int descriptor) noexcept;

} // namespace thin_io
