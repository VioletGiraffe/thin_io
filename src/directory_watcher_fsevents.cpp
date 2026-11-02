#include "directory_watcher.hpp"
#include "directory_watcher_posix.hpp"
#include "fs.hpp"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h> // realpath
#include <strings.h> // strncasecmp
#include <sys/mount.h> // statfs
#include <sys/param.h> // MAXPATHLEN
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace thin_io {

namespace {

constexpr FSEventStreamEventFlags metadataFlags = kFSEventStreamEventFlagItemInodeMetaMod | kFSEventStreamEventFlagItemChangeOwner
	| kFSEventStreamEventFlagItemXattrMod | kFSEventStreamEventFlagItemFinderInfoMod;

// One event as the stream reported it; interpreted later on the caller's thread
struct stream_event {
	std::string path; // Absolute
	FSEventStreamEventFlags flags = 0;
	FSEventStreamEventId id = 0;
};

[[nodiscard]] entry_kind kindFromFlags(const FSEventStreamEventFlags flags) noexcept
{
	if ((flags & kFSEventStreamEventFlagItemIsDir) != 0)
		return entry_kind::directory;
	if ((flags & kFSEventStreamEventFlagItemIsFile) != 0)
		return entry_kind::regular_file;
	if ((flags & kFSEventStreamEventFlagItemIsSymlink) != 0)
		return entry_kind::other;
	return entry_kind::unknown;
}

[[nodiscard]] bool pathExists(const std::string& path) noexcept
{
	struct stat info{};
	return ::lstat(path.c_str(), &info) == 0;
}

void onStreamEvents(ConstFSEventStreamRef, void* context, size_t count, void* paths, const FSEventStreamEventFlags flags[],
	const FSEventStreamEventId ids[]) noexcept;

} // namespace

// The stream's callback runs on a private dispatch queue; everything else runs on the caller's thread.
struct directory_watcher::impl {
	impl() = default;
	~impl();

	impl(const impl&) = delete;
	impl& operator=(const impl&) = delete;

	[[nodiscard]] filesystem_result<void> arm(const char* rootPath, watch_options options);
	[[nodiscard]] std::optional<watch_event> drain(std::vector<watch_event>* events, bool& any);
	void interpret(const std::vector<stream_event>& received, std::vector<watch_event>* events);
	[[nodiscard]] std::optional<std::string> relativePath(std::string_view absolutePath) const;
	// events: null when discarding
	void emit(watch_event event, std::vector<watch_event>* events) const;
	[[nodiscard]] std::optional<stop_reason> rootLossReason() const;
	void stopWatch(stop_reason reason, filesystem_error error) noexcept;
	// Never called from the queue: it waits for the queue to finish a running callback.
	void tearDownStream() noexcept;
	void signal() noexcept;

	// Shared with the callback
	std::mutex mutex;
	std::vector<stream_event> received;
	bool signalled = false; // The pipe holds a byte: readable while anything is pending

	int signalReadFd = -1;
	int signalWriteFd = -1;
	FSEventStreamRef stream = nullptr;
	dispatch_queue_t queue = nullptr;

	std::string root; // Absolute, as given: the root's identity is checked through it
	// The spellings the stream may report the root under: as given, resolved, and as the volume stores it
	std::vector<std::string> rootSpellings;
	bool recursive = true;
	std::vector<native_string> excludedDirectories;
	std::optional<entry_identity> rootId;
	watch_coverage coverage = watch_coverage::complete;
	std::optional<watch_event> pendingStop;
	bool stopDelivered = false;
};

namespace {

void onStreamEvents(ConstFSEventStreamRef, void* const context, const size_t count, void* const paths, const FSEventStreamEventFlags flags[],
	const FSEventStreamEventId ids[]) noexcept
{
	auto& watcher = *static_cast<directory_watcher::impl*>(context);
	const auto* const absolutePaths = static_cast<const char* const*>(paths);

	std::lock_guard lock{ watcher.mutex };
	for (size_t i = 0; i < count; ++i)
		watcher.received.push_back(stream_event{ .path = absolutePaths[i], .flags = flags[i], .id = ids[i] });
	watcher.signal();
}

} // namespace

directory_watcher::impl::~impl()
{
	tearDownStream();
	if (signalReadFd >= 0)
		::close(signalReadFd);
	if (signalWriteFd >= 0)
		::close(signalWriteFd);
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
		root = joinedPath(currentDirectory, root);
		::free(currentDirectory);
	}
	while (root.size() > 1 && root.ends_with('/'))
		root.pop_back();

	const auto metadata = get_entry_metadata(root.c_str(), link_behavior::follow);
	if (!metadata) [[unlikely]]
		return std::unexpected{ metadata.error() };
	if (metadata->attributes.kind != entry_kind::directory) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = ENOTDIR } };
	rootId = metadata->identity;

	char* const resolved = ::realpath(root.c_str(), nullptr);
	if (resolved == nullptr) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };
	const std::string resolvedRoot = resolved;
	::free(resolved);

	rootSpellings = { root, resolvedRoot };
	if (const int rootFd = ::open(resolvedRoot.c_str(), O_EVTONLY | O_CLOEXEC); rootFd >= 0)
	{
		char stored[MAXPATHLEN]{};
		if (::fcntl(rootFd, F_GETPATH, stored) == 0)
			rootSpellings.emplace_back(stored);
		::close(rootFd);
	}

	int ends[2]{ -1, -1 };
	if (::pipe(ends) != 0) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };
	signalReadFd = ends[0];
	signalWriteFd = ends[1];
	if (!setNonBlockingAndCloseOnExec(signalReadFd) || !setNonBlockingAndCloseOnExec(signalWriteFd)) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };

	struct statfs volume{};
	if (::statfs(resolvedRoot.c_str(), &volume) != 0 || (volume.f_flags & MNT_LOCAL) == 0)
		coverage = watch_coverage::partial;

	CFStringRef watchedPath = ::CFStringCreateWithFileSystemRepresentation(nullptr, resolvedRoot.c_str());
	if (watchedPath == nullptr) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = ENOMEM } };
	const CFArrayRef watchedPaths = ::CFArrayCreate(nullptr, reinterpret_cast<const void**>(&watchedPath), 1, &kCFTypeArrayCallBacks);
	::CFRelease(watchedPath);
	if (watchedPaths == nullptr) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = ENOMEM } };

	FSEventStreamContext context{ .version = 0, .info = this, .retain = nullptr, .release = nullptr, .copyDescription = nullptr };
	// WatchRoot: a moved, deleted or replaced root or ancestor is reported as RootChanged
	stream = ::FSEventStreamCreate(nullptr, onStreamEvents, &context, watchedPaths, kFSEventStreamEventIdSinceNow,
		options.coalescing_latency_seconds,
		kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagNoDefer);
	::CFRelease(watchedPaths);
	if (stream == nullptr) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = EINVAL } };

	queue = ::dispatch_queue_create("thin_io.directory_watcher", DISPATCH_QUEUE_SERIAL);
	::FSEventStreamSetDispatchQueue(stream, queue);
	if (!::FSEventStreamStart(stream)) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = EIO } };

	// A replacement before the stream started went unreported.
	if (const auto reason = rootLossReason())
		stopWatch(*reason, {});
	return {};
}

void directory_watcher::impl::tearDownStream() noexcept
{
	if (stream != nullptr)
	{
		::FSEventStreamStop(stream);
		::FSEventStreamInvalidate(stream);
		// The serial queue runs this after any callback already running, so none outlives the stream.
		::dispatch_sync_f(queue, nullptr, [](void*) {});
		::FSEventStreamRelease(stream);
		stream = nullptr;
	}

	if (queue != nullptr)
	{
		::dispatch_release(queue);
		queue = nullptr;
	}
}

// Requires the mutex
void directory_watcher::impl::signal() noexcept
{
	if (!std::exchange(signalled, true))
		(void)!::write(signalWriteFd, "", 1);
}

std::optional<watch_event> directory_watcher::impl::drain(std::vector<watch_event>* const events, bool& any)
{
	std::vector<stream_event> batch;
	{
		// The buffer and the pipe empty together, so the pipe stays readable exactly while events are pending.
		std::lock_guard lock{ mutex };
		batch.swap(received);
		drainDescriptor(signalReadFd);
		signalled = false;
	}

	if (stopDelivered)
		return {};

	if (!batch.empty())
		any = true;
	if (!pendingStop)
		interpret(batch, events);
	if (!pendingStop)
		return {};

	// The stream is gone: nothing signals again, and the byte the stop wrote is read out here.
	{
		std::lock_guard lock{ mutex };
		received.clear();
		drainDescriptor(signalReadFd);
		signalled = false;
	}

	any = true;
	stopDelivered = true;
	return std::exchange(pendingStop, std::nullopt);
}

// FSEvents coalesces an entry's changes into one event and reports a rename as two events, the old name first, with
// consecutive IDs and no other pairing: whether a path still exists tells the outcome.
void directory_watcher::impl::interpret(const std::vector<stream_event>& batch, std::vector<watch_event>* const events)
{
	for (size_t i = 0; i < batch.size() && !pendingStop; ++i)
	{
		const stream_event& reported = batch[i];
		const FSEventStreamEventFlags flags = reported.flags;

		if ((flags & kFSEventStreamEventFlagUnmount) != 0)
		{
			stopWatch(stop_reason::volume_released, {});
			return;
		}
		if ((flags & kFSEventStreamEventFlagRootChanged) != 0)
		{
			// The root or an ancestor moved back leaves the root path on the watched directory.
			if (const auto reason = rootLossReason())
				stopWatch(*reason, {});
			continue;
		}
		if ((flags & (kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped)) != 0)
		{
			emit(watch_event{ .kind = watch_event_kind::overflow }, events);
			continue;
		}

		const auto relative = relativePath(reported.path);
		if (!relative || (!recursive && relative->find('/') != std::string::npos))
			continue;

		if ((flags & kFSEventStreamEventFlagMustScanSubDirs) != 0)
		{
			emit(watch_event{ .kind = watch_event_kind::overflow, .path = *relative }, events);
			continue;
		}

		const entry_kind kind = kindFromFlags(flags);
		const bool exists = pathExists(reported.path);
		if ((flags & kFSEventStreamEventFlagItemRemoved) != 0 && !exists)
		{
			emit(watch_event{ .kind = watch_event_kind::removed, .entry = kind, .path = *relative }, events);
			continue;
		}

		if ((flags & kFSEventStreamEventFlagItemRenamed) != 0)
		{
			if (!exists && i + 1 < batch.size())
			{
				const stream_event& next = batch[i + 1];
				const auto nextRelative = relativePath(next.path);
				if ((next.flags & kFSEventStreamEventFlagItemRenamed) != 0 && next.id == reported.id + 1 && nextRelative
					&& (recursive || nextRelative->find('/') == std::string::npos) && pathExists(next.path))
				{
					emit(watch_event{ .kind = watch_event_kind::renamed, .entry = kindFromFlags(next.flags), .path = *nextRelative, .old_path = *relative }, events);
					++i;
					continue;
				}
			}

			// Moved across the root's boundary, or reported unpaired
			emit(watch_event{ .kind = exists ? watch_event_kind::created : watch_event_kind::removed, .entry = kind, .path = *relative }, events);
			if (!exists)
				continue;
		}
		else if ((flags & kFSEventStreamEventFlagItemCreated) != 0 && exists)
			emit(watch_event{ .kind = watch_event_kind::created, .entry = kind, .path = *relative }, events);

		if (!exists)
			continue;
		if ((flags & kFSEventStreamEventFlagItemModified) != 0)
			emit(watch_event{ .kind = watch_event_kind::modified, .entry = kind, .path = *relative }, events);
		if ((flags & metadataFlags) != 0)
			emit(watch_event{ .kind = watch_event_kind::metadata_changed, .entry = kind, .path = *relative }, events);
	}
}

// Empty for a path outside the root. A case-insensitive volume reports the case it stores, which may differ from the root as given.
std::optional<std::string> directory_watcher::impl::relativePath(std::string_view absolutePath) const
{
	// A directory-level event, such as MustScanSubDirs, may end in a separator.
	while (absolutePath.size() > 1 && absolutePath.ends_with('/'))
		absolutePath.remove_suffix(1);

	for (const std::string& spelling : rootSpellings)
	{
		if (spelling == "/")
			return std::string{ absolutePath.substr(1) };
		if (absolutePath.size() < spelling.size() || ::strncasecmp(absolutePath.data(), spelling.data(), spelling.size()) != 0)
			continue;
		if (absolutePath.size() == spelling.size())
			return std::string{};
		if (absolutePath[spelling.size()] == '/')
			return std::string{ absolutePath.substr(spelling.size() + 1) };
	}
	return {};
}

void directory_watcher::impl::emit(watch_event event, std::vector<watch_event>* const events) const
{
	if (events != nullptr)
		appendUnlessExcluded(std::move(event), excludedDirectories, *events);
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

	tearDownStream();
	pendingStop = watch_event{ .kind = watch_event_kind::stopped, .reason = reason, .error = error };
	std::lock_guard lock{ mutex };
	signal(); // A waiter wakes for the stop
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
	return _impl ? _impl->signalReadFd : -1;
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

	return waitUntilReadable(_impl->signalReadFd, timeout);
}

watch_coverage directory_watcher::coverage() const noexcept
{
	return _impl ? _impl->coverage : watch_coverage::partial;
}

void directory_watcher::set_excluded_directories(std::vector<native_string> directories)
{
	if (_impl)
		_impl->excludedDirectories = normalizedRelativeDirectories(std::move(directories));
}

void directory_watcher::release() noexcept
{
	if (_impl)
		_impl->stopWatch(stop_reason::volume_released, {});
}

} // namespace thin_io
