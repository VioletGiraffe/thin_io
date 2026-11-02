#include "directory_watcher.hpp"
#include "fs.hpp"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h> // realpath
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/vfs.h> // statfs
#include <unistd.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace thin_io {

namespace {

constexpr uint32_t treeMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_ATTRIB | IN_EXCL_UNLINK | IN_ONLYDIR;
constexpr uint32_t selfMask = IN_DELETE_SELF | IN_MOVE_SELF;

[[nodiscard]] int pollMilliseconds(const std::chrono::milliseconds timeout) noexcept
{
	if (timeout.count() < 0)
		return -1;
	return static_cast<int>(std::min<std::chrono::milliseconds::rep>(timeout.count(), INT_MAX));
}

// statfs magic numbers: NFS, SMB, SMB2, CIFS, FUSE (WSL's and others' virtual drives), 9P
[[nodiscard]] bool isRemoteOrVirtual(const char* const path) noexcept
{
	struct statfs info{};
	if (::statfs(path, &info) != 0)
		return true;

	switch (static_cast<uint32_t>(info.f_type))
	{
	case 0x6969u:
	case 0x517Bu:
	case 0xFE534D42u:
	case 0xFF534D42u:
	case 0x65735546u:
	case 0x01021997u:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] std::string joined(const std::string_view directory, const std::string_view name)
{
	if (directory.empty())
		return std::string{ name };
	std::string path{ directory };
	if (!path.ends_with('/'))
		path += '/';
	path += name;
	return path;
}

[[nodiscard]] bool isSameOrUnder(const std::string_view path, const std::string_view directory) noexcept
{
	return path.starts_with(directory) && (path.size() == directory.size() || path[directory.size()] == '/');
}

[[nodiscard]] bool isUnder(const std::string_view path, const std::string_view directory) noexcept
{
	return !directory.empty() && path.size() > directory.size() && isSameOrUnder(path, directory);
}

[[nodiscard]] std::vector<native_string> normalizedRelativeDirectories(std::vector<native_string> directories)
{
	for (native_string& directory : directories)
	{
		while (directory.ends_with('/'))
			directory.pop_back();
	}
	return directories;
}

} // namespace

struct directory_watcher::impl {
	impl() = default;
	~impl();

	impl(const impl&) = delete;
	impl& operator=(const impl&) = delete;

	[[nodiscard]] filesystem_result<void> arm(const char* rootPath, watch_options options);
	[[nodiscard]] std::optional<watch_event> drain(std::vector<watch_event>* events, bool& any);
	void processBuffer(size_t bytes, std::vector<watch_event>* events);
	void processTreeEvent(const inotify_event& nativeEvent, std::vector<watch_event>* events);
	void flushMoveSource(std::vector<watch_event>* events);
	// Places a watch on a directory under the root, and on the directories under it. events: the entries found, as created.
	[[nodiscard]] filesystem_result<void> watchSubtree(const std::string& relativeDirectory, std::vector<watch_event>* events);
	void unwatchSubtree(std::string_view relativeDirectory) noexcept;
	void moveWatchedPaths(std::string_view from, std::string_view to);
	[[nodiscard]] bool isExcluded(std::string_view relativePath) const noexcept;
	void appendUnlessExcluded(watch_event event, std::vector<watch_event>* events) const;
	[[nodiscard]] std::optional<stop_reason> rootLossReason() const;
	void stopWatch(stop_reason reason, filesystem_error error) noexcept;
	void discardQueue() noexcept;

	int inotifyFd = -1;
	std::string root; // Absolute, as given: the root's identity is checked through it
	std::string realRoot; // The directory the watches are placed on; its ancestors are watched too
	bool recursive = true;
	std::vector<native_string> excludedDirectories;
	std::optional<entry_identity> rootId;
	watch_coverage coverage = watch_coverage::complete;
	int rootWatch = -1;
	std::unordered_map<int, std::string> treeWatches; // Directory relative to the root, empty for the root itself
	std::unordered_set<int> ancestorWatches;

	struct move_source {
		uint32_t cookie = 0;
		std::string path;
		bool isDirectory = false;
	};
	std::optional<move_source> moveSource; // An IN_MOVED_FROM waiting for its IN_MOVED_TO

	std::optional<watch_event> pendingStop;
	bool stopDelivered = false;
	alignas(inotify_event) std::array<char, 64 * 1024> buffer;
};

directory_watcher::impl::~impl()
{
	if (inotifyFd >= 0)
		::close(inotifyFd);
}

filesystem_result<void> directory_watcher::impl::arm(const char* const rootPath, watch_options options)
{
	recursive = options.recursive;
	excludedDirectories = normalizedRelativeDirectories(std::move(options.excluded_directories));

	root = rootPath;
	if (!root.starts_with('/'))
	{
		char* const currentDirectory = ::getcwd(nullptr, 0);
		if (currentDirectory == nullptr) [[unlikely]]
			return std::unexpected{ capture_last_filesystem_error() };
		root = joined(currentDirectory, root);
		::free(currentDirectory);
	}
	while (root.size() > 1 && root.ends_with('/'))
		root.pop_back();

	inotifyFd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotifyFd < 0) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };

	// Read before the root's watch is placed: a replacement in between then fails the check at the end.
	const auto metadata = get_entry_metadata(root.c_str(), link_behavior::follow);
	if (!metadata) [[unlikely]]
		return std::unexpected{ metadata.error() };
	if (metadata->attributes.kind != entry_kind::directory) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = ENOTDIR } };
	rootId = metadata->identity;

	char* const resolved = ::realpath(root.c_str(), nullptr);
	if (resolved == nullptr) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };
	realRoot = resolved;
	::free(resolved);

	rootWatch = ::inotify_add_watch(inotifyFd, realRoot.c_str(), treeMask | selfMask);
	if (rootWatch < 0) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };
	treeWatches.emplace(rootWatch, std::string{});

	// A watch follows its inode, so a moved ancestor would go unnoticed without its own watch.
	for (std::string ancestor = realRoot; ancestor != "/";)
	{
		ancestor.resize(std::max<size_t>(ancestor.find_last_of('/'), 1));
		const int ancestorWatch = ::inotify_add_watch(inotifyFd, ancestor.c_str(), selfMask | IN_ONLYDIR);
		if (ancestorWatch >= 0)
			ancestorWatches.insert(ancestorWatch);
		else if (errno == ENOSPC) [[unlikely]]
			return std::unexpected{ capture_last_filesystem_error() };
		else // An execute-only directory cannot be watched
			coverage = watch_coverage::partial;
	}

	if (recursive)
	{
		if (auto placed = watchSubtree({}, nullptr); !placed) [[unlikely]]
			return placed;
	}

	if (isRemoteOrVirtual(realRoot.c_str()))
		coverage = watch_coverage::partial;

	if (const auto reason = rootLossReason())
		stopWatch(*reason, {});
	return {};
}

filesystem_result<void> directory_watcher::impl::watchSubtree(const std::string& relativeDirectory, std::vector<watch_event>* const events)
{
	const std::string absoluteDirectory = joined(realRoot, relativeDirectory);
	if (!relativeDirectory.empty())
	{
		const int descriptor = ::inotify_add_watch(inotifyFd, absoluteDirectory.c_str(), treeMask);
		if (descriptor < 0)
		{
			if (errno == ENOSPC) [[unlikely]]
				return std::unexpected{ capture_last_filesystem_error() };
			// Gone already, or unreadable
			if (errno != ENOENT && errno != ENOTDIR)
				coverage = watch_coverage::partial;
			return {};
		}
		treeWatches.insert_or_assign(descriptor, relativeDirectory);
	}

	// Listed after the watch is placed: an entry made in between is reported twice, never missed.
	const auto entries = list_directory(absoluteDirectory.c_str(), listing_detail::basic);
	if (!entries)
	{
		if (entries.error().native_code != ENOENT && entries.error().native_code != ENOTDIR)
			coverage = watch_coverage::partial;
		return {};
	}

	for (const directory_entry& entry : *entries)
	{
		const std::string path = joined(relativeDirectory, entry.name);
		const bool isDirectory = entry.attributes.kind == entry_kind::directory && !entry.attributes.is_link;
		if (events != nullptr)
			appendUnlessExcluded(watch_event{ .kind = watch_event_kind::created, .entry = entry.attributes.kind, .path = path }, events);

		if (isDirectory && !isExcluded(path))
		{
			if (auto placed = watchSubtree(path, events); !placed) [[unlikely]]
				return placed;
		}
	}

	return {};
}

void directory_watcher::impl::unwatchSubtree(const std::string_view relativeDirectory) noexcept
{
	std::erase_if(treeWatches, [&](const auto& watched) {
		if (watched.first == rootWatch || !isSameOrUnder(watched.second, relativeDirectory))
			return false;
		::inotify_rm_watch(inotifyFd, watched.first);
		return true;
	});
}

void directory_watcher::impl::moveWatchedPaths(const std::string_view from, const std::string_view to)
{
	for (auto& [descriptor, path] : treeWatches)
	{
		if (descriptor != rootWatch && isSameOrUnder(path, from))
			path = std::string{ to } + path.substr(from.size());
	}
}

bool directory_watcher::impl::isExcluded(const std::string_view relativePath) const noexcept
{
	return std::ranges::any_of(excludedDirectories, [relativePath](const native_string& directory) {
		return !directory.empty() && isSameOrUnder(relativePath, directory);
	});
}

// A rename across an excluded directory's boundary is a creation or a removal.
// An excluded directory's own modification is dropped, as on every platform.
void directory_watcher::impl::appendUnlessExcluded(watch_event event, std::vector<watch_event>* const events) const
{
	if (events == nullptr)
		return;

	const auto under = [this](const std::string_view path) {
		return std::ranges::any_of(excludedDirectories, [path](const native_string& directory) { return isUnder(path, directory); });
	};

	if ((event.kind == watch_event_kind::modified || event.kind == watch_event_kind::metadata_changed)
		&& std::ranges::find(excludedDirectories, event.path) != excludedDirectories.end())
		return;

	const bool pathExcluded = under(event.path);
	if (event.kind == watch_event_kind::renamed)
	{
		const bool oldPathExcluded = under(event.old_path);
		if (pathExcluded && oldPathExcluded)
			return;
		if (oldPathExcluded)
		{
			event.kind = watch_event_kind::created;
			event.old_path.clear();
		}
		else if (pathExcluded)
		{
			event.kind = watch_event_kind::removed;
			event.path = std::move(event.old_path);
			event.old_path.clear();
		}
	}
	else if (pathExcluded)
		return;

	events->push_back(std::move(event));
}

std::optional<watch_event> directory_watcher::impl::drain(std::vector<watch_event>* const events, bool& any)
{
	if (stopDelivered)
	{
		discardQueue();
		return {};
	}

	while (!pendingStop)
	{
		const ssize_t bytes = ::read(inotifyFd, buffer.data(), buffer.size());
		if (bytes < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN) [[unlikely]]
				stopWatch(stop_reason::failed, capture_last_filesystem_error());
			break;
		}

		any = true;
		processBuffer(static_cast<size_t>(bytes), events);
	}

	if (!pendingStop)
		return {};

	// Removing the watches queued an IN_IGNORED for each: read out, the descriptor stays quiet from here on.
	discardQueue();
	any = true;
	stopDelivered = true;
	return std::exchange(pendingStop, std::nullopt);
}

void directory_watcher::impl::processBuffer(const size_t bytes, std::vector<watch_event>* const events)
{
	for (size_t offset = 0; offset < bytes && !pendingStop;)
	{
		// Through void*: the kernel aligns every record, and -Wcast-align=strict cannot know that.
		const auto& nativeEvent = *static_cast<const inotify_event*>(static_cast<const void*>(buffer.data() + offset));
		offset += sizeof(inotify_event) + nativeEvent.len;

		if ((nativeEvent.mask & IN_Q_OVERFLOW) != 0)
		{
			flushMoveSource(events);
			appendUnlessExcluded(watch_event{ .kind = watch_event_kind::overflow }, events);
			// Directories made during the overflow have no watch yet
			if (recursive)
			{
				if (auto placed = watchSubtree({}, nullptr); !placed) [[unlikely]]
					stopWatch(stop_reason::failed, placed.error());
			}
			continue;
		}

		if ((nativeEvent.mask & IN_UNMOUNT) != 0)
		{
			stopWatch(stop_reason::volume_released, {});
			return;
		}

		const bool isRoot = nativeEvent.wd == rootWatch;
		if (ancestorWatches.contains(nativeEvent.wd) || (isRoot && (nativeEvent.mask & selfMask) != 0))
		{
			// An ancestor or the root moved back leaves the root path on the watched directory.
			// A deleted root's replacement can reuse its inode number at once, so it compares as the same directory.
			if (const auto reason = rootLossReason())
				stopWatch(*reason, {});
			else if ((nativeEvent.mask & IN_DELETE_SELF) != 0 && isRoot)
				stopWatch(stop_reason::root_replaced, {});
			continue;
		}

		if ((nativeEvent.mask & IN_IGNORED) != 0)
		{
			if (isRoot)
				stopWatch(stop_reason::root_lost, {});
			treeWatches.erase(nativeEvent.wd);
			continue;
		}

		processTreeEvent(nativeEvent, events);
	}

	flushMoveSource(events);
}

void directory_watcher::impl::processTreeEvent(const inotify_event& nativeEvent, std::vector<watch_event>* const events)
{
	const auto watched = treeWatches.find(nativeEvent.wd);
	// A subtree already unwatched, or a directory's own deletion reported to its own watch
	if (watched == treeWatches.end() || nativeEvent.len == 0)
		return;

	const std::string path = joined(watched->second, std::string_view{ nativeEvent.name, ::strnlen(nativeEvent.name, nativeEvent.len) });
	const bool isDirectory = (nativeEvent.mask & IN_ISDIR) != 0;
	const entry_kind kind = isDirectory ? entry_kind::directory : entry_kind::unknown;

	if ((nativeEvent.mask & IN_MOVED_TO) != 0 && moveSource && moveSource->cookie == nativeEvent.cookie)
	{
		const move_source source = *std::exchange(moveSource, std::nullopt);
		if (isDirectory)
		{
			moveWatchedPaths(source.path, path);
			if (isExcluded(path))
				unwatchSubtree(path);
			else if (recursive && isExcluded(source.path))
			{
				if (auto placed = watchSubtree(path, events); !placed) [[unlikely]]
					stopWatch(stop_reason::failed, placed.error());
			}
		}
		appendUnlessExcluded(watch_event{ .kind = watch_event_kind::renamed, .entry = kind, .path = path, .old_path = source.path }, events);
		return;
	}

	flushMoveSource(events);

	if ((nativeEvent.mask & IN_MOVED_FROM) != 0)
	{
		moveSource = move_source{ .cookie = nativeEvent.cookie, .path = path, .isDirectory = isDirectory };
		return;
	}

	if ((nativeEvent.mask & (IN_CREATE | IN_MOVED_TO)) != 0)
	{
		appendUnlessExcluded(watch_event{ .kind = watch_event_kind::created, .entry = kind, .path = path }, events);
		if (isDirectory && recursive && !isExcluded(path))
		{
			if (auto placed = watchSubtree(path, events); !placed) [[unlikely]]
				stopWatch(stop_reason::failed, placed.error());
		}
	}
	else if ((nativeEvent.mask & IN_DELETE) != 0)
		appendUnlessExcluded(watch_event{ .kind = watch_event_kind::removed, .entry = kind, .path = path }, events);
	else if ((nativeEvent.mask & IN_MODIFY) != 0)
		appendUnlessExcluded(watch_event{ .kind = watch_event_kind::modified, .entry = kind, .path = path }, events);
	else if ((nativeEvent.mask & IN_ATTRIB) != 0)
		appendUnlessExcluded(watch_event{ .kind = watch_event_kind::metadata_changed, .entry = kind, .path = path }, events);
}

// An IN_MOVED_FROM without its IN_MOVED_TO moved out of the tree.
void directory_watcher::impl::flushMoveSource(std::vector<watch_event>* const events)
{
	if (!moveSource)
		return;

	const move_source source = *std::exchange(moveSource, std::nullopt);
	if (source.isDirectory)
		unwatchSubtree(source.path);
	appendUnlessExcluded(watch_event{ .kind = watch_event_kind::removed,
		.entry = source.isDirectory ? entry_kind::directory : entry_kind::unknown, .path = source.path }, events);
}

// Empty while the root path still resolves to the watched directory. An unknown identity never matches.
std::optional<stop_reason> directory_watcher::impl::rootLossReason() const
{
	const auto metadata = get_entry_metadata(root.c_str(), link_behavior::follow);
	if (!metadata)
		return stop_reason::root_lost;
	if (!rootId || !metadata->identity || *metadata->identity != *rootId)
		return stop_reason::root_replaced;
	return {};
}

void directory_watcher::impl::stopWatch(const stop_reason reason, const filesystem_error error) noexcept
{
	if (pendingStop || stopDelivered)
		return;

	pendingStop = watch_event{ .kind = watch_event_kind::stopped, .reason = reason, .error = error };
	// Each removal queues an IN_IGNORED, so the descriptor turns readable and a waiter wakes for the stop.
	for (const auto& watched : treeWatches)
		::inotify_rm_watch(inotifyFd, watched.first);
	for (const int descriptor : ancestorWatches)
		::inotify_rm_watch(inotifyFd, descriptor);
	treeWatches.clear();
	ancestorWatches.clear();
	rootWatch = -1;
	moveSource.reset();
}

void directory_watcher::impl::discardQueue() noexcept
{
	while (::read(inotifyFd, buffer.data(), buffer.size()) > 0 || errno == EINTR)
	{
	}
}

filesystem_result<directory_watcher> directory_watcher::watch(const char* const root, watch_options options)
{
	if (root == nullptr) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = EINVAL } };

	auto watcherImpl = std::make_unique<impl>();
	if (auto armed = watcherImpl->arm(root, std::move(options)); !armed) [[unlikely]]
		return std::unexpected{ armed.error() };
	return directory_watcher{ std::move(watcherImpl) };
}

directory_watcher::directory_watcher(std::unique_ptr<impl> watcherImpl) noexcept : _impl{ std::move(watcherImpl) } {}
directory_watcher::directory_watcher(directory_watcher&&) noexcept = default;
directory_watcher& directory_watcher::operator=(directory_watcher&&) noexcept = default;
directory_watcher::~directory_watcher() = default;

native_wait_handle directory_watcher::wait_handle() const noexcept
{
	return _impl ? _impl->inotifyFd : -1;
}

filesystem_result<std::vector<watch_event>> directory_watcher::read_events()
{
	if (!_impl) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = EBADF } };

	std::vector<watch_event> events;
	bool any = false;
	if (auto stop = _impl->drain(&events, any))
		events.push_back(std::move(*stop));
	return events;
}

filesystem_result<discarded_events> directory_watcher::discard_events()
{
	if (!_impl) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = EBADF } };

	discarded_events discarded;
	discarded.stop = _impl->drain(nullptr, discarded.any);
	return discarded;
}

filesystem_result<bool> directory_watcher::wait_for_events(const std::chrono::milliseconds timeout)
{
	if (!_impl) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = EBADF } };

	pollfd descriptor{ .fd = _impl->inotifyFd, .events = POLLIN, .revents = 0 };
	for (;;)
	{
		const int ready = ::poll(&descriptor, 1, pollMilliseconds(timeout));
		if (ready >= 0)
			return ready > 0;
		if (errno != EINTR) [[unlikely]]
			return std::unexpected{ capture_last_filesystem_error() };
	}
}

watch_coverage directory_watcher::coverage() const noexcept
{
	return _impl ? _impl->coverage : watch_coverage::partial;
}

void directory_watcher::set_excluded_directories(std::vector<native_string> directories)
{
	if (!_impl || _impl->pendingStop || _impl->stopDelivered) [[unlikely]]
		return;

	impl& watcher = *_impl;
	const std::vector<native_string> previous = std::exchange(watcher.excludedDirectories, normalizedRelativeDirectories(std::move(directories)));
	if (!watcher.recursive)
		return;

	for (const native_string& directory : watcher.excludedDirectories)
		watcher.unwatchSubtree(directory);
	// A directory leaving the list gets its watches back, and reports nothing for what it already holds.
	for (const native_string& directory : previous)
	{
		if (!directory.empty() && !watcher.isExcluded(directory))
		{
			if (auto placed = watcher.watchSubtree(directory, nullptr); !placed) [[unlikely]]
				watcher.stopWatch(stop_reason::failed, placed.error());
		}
	}
}

void directory_watcher::release() noexcept
{
	if (_impl)
		_impl->stopWatch(stop_reason::volume_released, {});
}

filesystem_result<waker> waker::create()
{
	const int descriptor = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (descriptor < 0) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };
	return waker{ descriptor };
}

waker::waker(const native_wait_handle handle) noexcept : _handle{ handle } {}

waker::waker(waker&& other) noexcept : _handle{ std::exchange(other._handle, -1) } {}

waker& waker::operator=(waker&& other) noexcept
{
	if (this != &other)
	{
		if (_handle >= 0)
			::close(_handle);
		_handle = std::exchange(other._handle, -1);
	}
	return *this;
}

waker::~waker()
{
	if (_handle >= 0)
		::close(_handle);
}

void waker::signal() noexcept
{
	if (_handle >= 0)
	{
		const uint64_t one = 1;
		(void)!::write(_handle, &one, sizeof(one));
	}
}

filesystem_result<wait_result> wait_for_any(const std::span<directory_watcher* const> watchers, waker* const wakeUp,
	const std::chrono::milliseconds timeout)
{
	std::vector<pollfd> descriptors;
	descriptors.reserve(watchers.size() + 1);
	if (wakeUp != nullptr)
		descriptors.push_back(pollfd{ .fd = wakeUp->_handle, .events = POLLIN, .revents = 0 });
	for (directory_watcher* const watcher : watchers)
		descriptors.push_back(pollfd{ .fd = watcher != nullptr ? watcher->wait_handle() : -1, .events = POLLIN, .revents = 0 });

	if (descriptors.empty() || std::ranges::any_of(descriptors, [](const pollfd& descriptor) { return descriptor.fd < 0; })) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = EINVAL } };

	int ready = 0;
	while ((ready = ::poll(descriptors.data(), descriptors.size(), pollMilliseconds(timeout))) < 0)
	{
		if (errno != EINTR) [[unlikely]]
			return std::unexpected{ capture_last_filesystem_error() };
	}

	wait_result result;
	if (ready == 0)
		return result;

	const size_t firstWatcher = wakeUp != nullptr ? 1 : 0;
	if (wakeUp != nullptr && (descriptors.front().revents & POLLIN) != 0)
	{
		uint64_t signals = 0;
		(void)!::read(wakeUp->_handle, &signals, sizeof(signals)); // Consumes every signal: they coalesce
		result.woken = true;
	}
	for (size_t i = 0; i < watchers.size(); ++i)
	{
		if ((descriptors[firstWatcher + i].revents & POLLIN) != 0)
			result.ready.push_back(watchers[i]);
	}
	return result;
}

} // namespace thin_io
