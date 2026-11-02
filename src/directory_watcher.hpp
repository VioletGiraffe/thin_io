#pragma once

#include "filesystem_error.hpp"
#include "filesystem_types.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <stdint.h>
#include <vector>

namespace thin_io {

#ifdef _WIN32
using native_wait_handle = void*; // HANDLE
#else
using native_wait_handle = int; // fd
#endif

enum class watch_event_kind : uint8_t {
	created,
	removed,
	modified,         // Content or size; also metadata where the platform cannot tell them apart (Windows).
	metadata_changed, // Attributes, permissions, timestamps alone.
	renamed,          // old_path to path, both under the root.
	overflow,         // Events under path were lost; the watch goes on.
	stopped           // The watch has ended; no events follow.
};

enum class stop_reason : uint8_t {
	root_lost,       // The root path no longer resolves to the watched directory: the root was deleted, or the root or an ancestor moved.
	root_replaced,   // The root path resolves to a different directory.
	volume_released, // release() was called, or the volume was locked for eject, dismounted, unmounted or removed.
	failed           // Placing a watch or reading events failed; see error.
};

struct watch_event {
	watch_event_kind kind{};
	entry_kind entry = entry_kind::unknown; // Where the platform reports it.
	native_string path;                     // Relative to the root, with native separators; empty for the root itself.
	native_string old_path;                 // renamed only.
	stop_reason reason{};                   // stopped only.
	filesystem_error error{};               // stopped with failed only.
};

// A heuristic from the volume type: a server may never send notifications although the watch arms fine.
enum class watch_coverage : uint8_t {
	complete, // A local volume.
	partial   // A remote or virtual volume, or a root or ancestor that could not be watched: changes may go unreported.
};

struct watch_options {
	bool recursive = true;
	// Relative to the root. Nothing under them is reported or, on Linux, watched; nor is their own modification.
	std::vector<native_string> excluded_directories;
	double coalescing_latency_seconds = 0.05; // FSEvents only.
};

// What discard_events() drained.
struct discarded_events {
	bool any = false;                // Something was pending, the stop included. Exclusions are not applied.
	std::optional<watch_event> stop; // The stopped event, when the watch ended.
};

// Watches one directory, or the subtree under it, for changes.
// Not thread-safe: every call on one watcher comes from one thread at a time. It may be created on one thread and
// used from another; it holds no thread-local state.
// wait_handle() is level-triggered: signalled while notifications are pending. read_events() and discard_events()
// drain them all and leave it non-signalled.
// A notification may produce no event, such as a change in an excluded directory, or on Windows a directory added, removed or renamed beside the root.
// After the stopped event, no further events arrive and the watcher never re-arms itself: the caller re-arms by path.
// A directory that appears is reported as created. Its entries may be reported too, one made as the directory appears
// possibly twice: a caller that needs them lists the directory.
// Windows: the root's parent and every further ancestor cannot be renamed while the watch is held.
class directory_watcher {
public:
	// A relative root is resolved against the current directory once, here.
	[[nodiscard]] static filesystem_result<directory_watcher> watch(const native_char* root, watch_options options);
#ifdef _WIN32
	[[nodiscard]] static filesystem_result<directory_watcher> watch(const char* utf8Root, watch_options options);
#endif

	directory_watcher(directory_watcher&&) noexcept;
	directory_watcher& operator=(directory_watcher&&) noexcept;
	~directory_watcher();

	// Signalled while notifications are pending: an event HANDLE on Windows, a readable fd on POSIX.
	[[nodiscard]] native_wait_handle wait_handle() const noexcept;
	// Never blocks; empty when nothing is pending.
	[[nodiscard]] filesystem_result<std::vector<watch_event>> read_events();
	// Drains the pending notifications without building events, and re-arms.
	[[nodiscard]] filesystem_result<discarded_events> discard_events();
	// Blocks until notifications are pending or the timeout passes; a negative timeout waits indefinitely.
	[[nodiscard]] filesystem_result<bool> wait_for_events(std::chrono::milliseconds timeout);
	[[nodiscard]] watch_coverage coverage() const noexcept;
	// Takes effect for later changes; a directory leaving the list reports nothing for what it already holds.
	void set_excluded_directories(std::vector<native_string> directories);

	// Closes every handle; the watch then reports stopped with volume_released.
	void release() noexcept;

	struct impl;

private:
	explicit directory_watcher(std::unique_ptr<impl> watcherImpl) noexcept;

	std::unique_ptr<impl> _impl;
};

struct wait_result {
	bool woken = false;                    // The waker was signalled.
	std::vector<directory_watcher*> ready; // Every watcher with notifications pending when the wait returned.
};

class waker;

// Blocks until a watcher has notifications pending, the waker is signalled, or the timeout passes (an empty result).
// wakeUp may be null. A negative timeout waits indefinitely.
// The watchers belong to the waiting thread for the duration of the wait.
// Windows: at most 64 handles, so 63 watchers with a waker, 64 without.
[[nodiscard]] filesystem_result<wait_result> wait_for_any(std::span<directory_watcher* const> watchers, waker* wakeUp,
	std::chrono::milliseconds timeout);

// Wakes a thread blocked in wait_for_any(). signal() is safe from any thread.
class waker {
public:
	[[nodiscard]] static filesystem_result<waker> create();

	waker(waker&& other) noexcept;
	waker& operator=(waker&& other) noexcept;
	~waker();

	// Wakes the next wait_for_any() given this waker, which consumes the signal; signals before it coalesce.
	void signal() noexcept;

private:
#ifdef _WIN32
	explicit waker(native_wait_handle handle) noexcept;
#else
	waker(native_wait_handle readEnd, int writeEnd) noexcept;
#endif

	native_wait_handle _handle; // What wait_for_any() waits on: on POSIX, the read end of a pipe
#ifndef _WIN32
	int _writeEnd = -1;
#endif

	friend filesystem_result<wait_result> wait_for_any(std::span<directory_watcher* const> watchers, waker* wakeUp,
		std::chrono::milliseconds timeout);
};

} // namespace thin_io
