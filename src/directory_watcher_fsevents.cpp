#include "directory_watcher.hpp"
#include "directory_watcher_posix.hpp"
#include "fs.hpp"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h> // realpath
#include <strings.h> // strncasecmp
#include <sys/mount.h> // statfs
#include <sys/param.h> // MAXPATHLEN
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <iterator>
#include <memory>
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
	std::string relative; // To the root; empty for the root itself and for unmount, drop and RootChanged events
	FSEventStreamEventFlags flags = 0;
	FSEventStreamEventId id = 0;
	// Nonzero: the followed link of this id lost its target, which moved, was deleted or went away with its volume. relative is the link.
	uint64_t lostLinkId = 0;
};

// The part of absolutePath under one of a directory's spellings: empty for the directory itself, nothing for a path outside it.
// A case-insensitive volume reports the case it stores, which may differ from the spelling given.
[[nodiscard]] std::optional<std::string> pathUnder(std::string_view absolutePath, const std::vector<std::string>& spellings)
{
	// A directory-level event, such as MustScanSubDirs, may end in a separator.
	while (absolutePath.size() > 1 && absolutePath.ends_with('/'))
		absolutePath.remove_suffix(1);

	for (const std::string& spelling : spellings)
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

// WatchRoot: a moved, deleted or replaced root or ancestor is reported as RootChanged. Null on failure.
[[nodiscard]] FSEventStreamRef createStream(const std::string& path, const FSEventStreamCallback callback, void* const info, const double latency)
{
	CFStringRef watchedPath = ::CFStringCreateWithFileSystemRepresentation(nullptr, path.c_str());
	if (watchedPath == nullptr) [[unlikely]]
		return nullptr;
	const CFArrayRef watchedPaths = ::CFArrayCreate(nullptr, reinterpret_cast<const void**>(&watchedPath), 1, &kCFTypeArrayCallBacks);
	::CFRelease(watchedPath);
	if (watchedPaths == nullptr) [[unlikely]]
		return nullptr;

	FSEventStreamContext context{ .version = 0, .info = info, .retain = nullptr, .release = nullptr, .copyDescription = nullptr };
	const FSEventStreamRef stream = ::FSEventStreamCreate(nullptr, callback, &context, watchedPaths, kFSEventStreamEventIdSinceNow, latency,
		kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagNoDefer);
	::CFRelease(watchedPaths);
	return stream;
}

// Stops a stream started on queue; afterwards no callback of it runs
void releaseStream(const FSEventStreamRef stream, const dispatch_queue_t queue) noexcept
{
	::FSEventStreamStop(stream);
	::FSEventStreamInvalidate(stream);
	// The serial queue runs this after any callback already running, so none outlives the stream.
	::dispatch_sync_f(queue, nullptr, [](void*) {});
	::FSEventStreamRelease(stream);
}

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
void onLinkStreamEvents(ConstFSEventStreamRef, void* context, size_t count, void* paths, const FSEventStreamEventFlags flags[],
	const FSEventStreamEventId ids[]) noexcept;

} // namespace

// The stream's callback and the revoke handler run on a private dispatch queue; everything else runs on the caller's thread.
struct directory_watcher::impl {
	impl() = default;
	~impl();

	impl(const impl&) = delete;
	impl& operator=(const impl&) = delete;

	// A followed directory link: the root's stream never reports changes through a link, so its target has its own stream
	struct link_stream {
		impl* watcher = nullptr;
		uint64_t id = 0; // Unique in the watcher: an event queued by a stream since released must not reach a newer link at the same path
		std::optional<entry_identity> targetId;
		std::string path; // Relative to the root
		std::vector<std::string> targetSpellings; // As resolved, and as the volume stores it
		FSEventStreamRef stream = nullptr;
	};

	[[nodiscard]] filesystem_result<void> arm(const char* rootPath, watch_options options);
	// Empty for an event that can produce no watch event. Runs on the queue: reads only what arm() set before the stream started.
	[[nodiscard]] std::optional<stream_event> relevantEvent(const char* absolutePath, FSEventStreamEventFlags flags, FSEventStreamEventId id) const;
	// The same for a link's stream: its events are reported under the link's path
	[[nodiscard]] static std::optional<stream_event> relevantLinkEvent(const link_stream& link, const char* absolutePath, FSEventStreamEventFlags flags,
		FSEventStreamEventId id);
	[[nodiscard]] std::optional<watch_event> drain(std::vector<watch_event>* events, bool& any);
	void interpret(const std::vector<stream_event>& received, std::vector<watch_event>* events);
	[[nodiscard]] std::optional<std::string> relativePath(std::string_view absolutePath) const;
	// Follows the links under relativeDirectory, which is watched already
	void followLinksUnder(const std::string& relativeDirectory);
	void followLink(const std::string& relativePath);
	// relativePath and what is under it, or only what is under it. An empty path is the root.
	void unfollowLinks(std::string_view relativePath, bool includingItself) noexcept;
	// Follows the links that the events show appearing, and drops those leaving
	void updateFollowedLinks(const std::vector<watch_event>& events);
	// The root, a directory, or a followed link: not a link that is not or no longer followed
	[[nodiscard]] bool isWalkable(const std::string& relativePath) const;
	// Drops the links under the excluded directories, and follows those under the directories that left the list
	void updateFollowedLinksForExclusions(const std::vector<native_string>& previouslyExcluded);
	// events: null when discarding
	void emit(watch_event event, std::vector<watch_event>* events) const;
	[[nodiscard]] std::optional<stop_reason> rootLossReason() const;
	void stopWatch(stop_reason reason, filesystem_error error) noexcept;
	// Never called from the queue: it waits for the queue to finish a running callback.
	void tearDownNotifications() noexcept;
	void signal() noexcept;

	// Shared with the callback
	std::mutex mutex;
	std::vector<stream_event> received;
	bool signalled = false; // The pipe holds a byte: readable while anything is pending

	int signalReadFd = -1;
	int signalWriteFd = -1;
	FSEventStreamRef stream = nullptr;
	dispatch_queue_t queue = nullptr;
	// The root's own volume going away reaches no stream: the kernel revokes the root's vnode instead.
	// O_EVTONLY: the descriptor never keeps the volume busy.
	int rootFd = -1;
	dispatch_source_t revokeSource = nullptr;
	dispatch_semaphore_t revokeCancelled = nullptr; // The cancel handler signals it: rootFd stays open until then

	std::string root; // Absolute, as given: the root's identity is checked through it
	// The spellings the stream may report the root under: as given, resolved, and as the volume stores it
	std::vector<std::string> rootSpellings;
	bool recursive = true;
	bool followDirectoryLinks = false;
	double coalescingLatency = 0;
	std::vector<std::unique_ptr<link_stream>> linkStreams; // Stable addresses: their streams' callbacks refer to them
	uint64_t lastLinkId = 0;
	std::vector<native_string> excludedDirectories;
	std::optional<entry_identity> rootId;
	std::string mountPoint; // Of the root's volume; empty when unknown
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

	std::vector<stream_event> relevant;
	for (size_t i = 0; i < count; ++i)
	{
		if (auto reported = watcher.relevantEvent(absolutePaths[i], flags[i], ids[i]))
			relevant.push_back(std::move(*reported));
	}
	if (relevant.empty())
		return;

	std::lock_guard lock{ watcher.mutex };
	watcher.received.insert(watcher.received.end(), std::make_move_iterator(relevant.begin()), std::make_move_iterator(relevant.end()));
	watcher.signal();
}

void onLinkStreamEvents(ConstFSEventStreamRef, void* const context, const size_t count, void* const paths, const FSEventStreamEventFlags flags[],
	const FSEventStreamEventId ids[]) noexcept
{
	const auto& link = *static_cast<const directory_watcher::impl::link_stream*>(context);
	const auto* const absolutePaths = static_cast<const char* const*>(paths);

	std::vector<stream_event> relevant;
	for (size_t i = 0; i < count; ++i)
	{
		if (auto reported = directory_watcher::impl::relevantLinkEvent(link, absolutePaths[i], flags[i], ids[i]))
			relevant.push_back(std::move(*reported));
	}
	if (relevant.empty())
		return;

	directory_watcher::impl& watcher = *link.watcher;
	std::lock_guard lock{ watcher.mutex };
	watcher.received.insert(watcher.received.end(), std::make_move_iterator(relevant.begin()), std::make_move_iterator(relevant.end()));
	watcher.signal();
}

void onRootRevoked(void* const context) noexcept
{
	auto& watcher = *static_cast<directory_watcher::impl*>(context);
	std::lock_guard lock{ watcher.mutex };
	watcher.received.push_back(stream_event{ .flags = kFSEventStreamEventFlagUnmount });
	watcher.signal();
}

} // namespace

directory_watcher::impl::~impl()
{
	tearDownNotifications();
	if (signalReadFd >= 0)
		::close(signalReadFd);
	if (signalWriteFd >= 0)
		::close(signalWriteFd);
}

filesystem_result<void> directory_watcher::impl::arm(const char* const rootPath, watch_options options)
{
	recursive = options.recursive;
	followDirectoryLinks = options.recursive && options.follow_directory_links;
	coalescingLatency = options.coalescing_latency_seconds;
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
	rootFd = ::open(resolvedRoot.c_str(), O_EVTONLY | O_CLOEXEC);
	if (rootFd >= 0)
	{
		char stored[MAXPATHLEN]{};
		if (::fcntl(rootFd, F_GETPATH, stored) == 0)
			rootSpellings.emplace_back(stored);
	}

	int ends[2]{ -1, -1 };
	if (::pipe(ends) != 0) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };
	signalReadFd = ends[0];
	signalWriteFd = ends[1];
	if (!setNonBlockingAndCloseOnExec(signalReadFd) || !setNonBlockingAndCloseOnExec(signalWriteFd)) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };

	struct statfs volume{};
	if (::statfs(resolvedRoot.c_str(), &volume) != 0)
		coverage = watch_coverage::partial;
	else
	{
		mountPoint = volume.f_mntonname;
		if ((volume.f_flags & MNT_LOCAL) == 0)
			coverage = watch_coverage::partial;
	}

	stream = createStream(resolvedRoot, onStreamEvents, this, coalescingLatency);
	if (stream == nullptr) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = EINVAL } };

	queue = ::dispatch_queue_create("thin_io.directory_watcher", DISPATCH_QUEUE_SERIAL);
	if (rootFd >= 0)
	{
		revokeSource = ::dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, static_cast<uintptr_t>(rootFd), DISPATCH_VNODE_REVOKE, queue);
		if (revokeSource == nullptr) [[unlikely]]
			return std::unexpected{ filesystem_error{ .native_code = ENOMEM } };

		revokeCancelled = ::dispatch_semaphore_create(0);
		::dispatch_set_context(revokeSource, this);
		::dispatch_source_set_event_handler_f(revokeSource, onRootRevoked);
		::dispatch_source_set_cancel_handler_f(revokeSource, [](void* const context) {
			::dispatch_semaphore_signal(static_cast<impl*>(context)->revokeCancelled);
		});
		::dispatch_resume(revokeSource);
	}
	::FSEventStreamSetDispatchQueue(stream, queue);
	if (!::FSEventStreamStart(stream)) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = EIO } };

	// After the root's stream starts, so a link made during the walk is reported, or found by it
	if (followDirectoryLinks)
		followLinksUnder({});

	// A replacement before the stream started went unreported.
	if (const auto reason = rootLossReason())
		stopWatch(*reason, {});
	return {};
}

void directory_watcher::impl::tearDownNotifications() noexcept
{
	if (revokeSource != nullptr)
	{
		::dispatch_source_cancel(revokeSource);
		::dispatch_semaphore_wait(revokeCancelled, DISPATCH_TIME_FOREVER);
		::dispatch_release(revokeSource);
		revokeSource = nullptr;
	}
	if (revokeCancelled != nullptr)
	{
		::dispatch_release(revokeCancelled);
		revokeCancelled = nullptr;
	}
	if (rootFd >= 0)
	{
		::close(rootFd);
		rootFd = -1;
	}

	unfollowLinks({}, true);
	if (stream != nullptr)
	{
		releaseStream(stream, queue);
		stream = nullptr;
	}

	if (queue != nullptr)
	{
		::dispatch_release(queue);
		queue = nullptr;
	}
}

std::optional<stream_event> directory_watcher::impl::relevantEvent(const char* const absolutePath, const FSEventStreamEventFlags flags,
	const FSEventStreamEventId id) const
{
	stream_event reported{ .path = absolutePath, .flags = flags, .id = id };
	if ((flags & (kFSEventStreamEventFlagUnmount | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped)) != 0)
		return reported;
	if ((flags & kFSEventStreamEventFlagRootChanged) != 0)
	{
		if (!rootLossReason())
			return {};
		return reported;
	}

	auto relative = relativePath(reported.path);
	if (!relative || (!recursive && relative->find('/') != std::string::npos))
		return {};
	// The root's own changes are no entry events: its loss stops the watch through RootChanged.
	if (relative->empty() && (flags & kFSEventStreamEventFlagMustScanSubDirs) == 0)
		return {};

	reported.relative = std::move(*relative);
	return reported;
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
	{
		// Following links needs the events even when the caller discards them
		std::vector<watch_event> interpreted;
		interpret(batch, events != nullptr || followDirectoryLinks ? &interpreted : nullptr);
		if (followDirectoryLinks && !pendingStop)
			updateFollowedLinks(interpreted);
		if (events != nullptr)
			std::ranges::move(interpreted, std::back_inserter(*events));
	}
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
// The batch holds only events relevantEvent() kept.
void directory_watcher::impl::interpret(const std::vector<stream_event>& batch, std::vector<watch_event>* const events)
{
	for (size_t i = 0; i < batch.size() && !pendingStop; ++i)
	{
		const stream_event& reported = batch[i];
		const FSEventStreamEventFlags flags = reported.flags;

		if (reported.lostLinkId != 0)
		{
			// A stream released since may have queued this
			if (std::ranges::none_of(linkStreams, [&reported](const auto& link) { return link->id == reported.lostLinkId; }))
				continue;

			// A target lost with its volume reports nothing: a listing through the link would touch the volume during an eject
			if ((flags & kFSEventStreamEventFlagUnmount) == 0)
				emit(watch_event{ .kind = watch_event_kind::overflow, .path = reported.relative }, events);
			unfollowLinks(reported.relative, true);
			coverage = watch_coverage::partial; // Nothing follows the link again if its target comes back
			continue;
		}
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

		const std::string& relative = reported.relative;
		if ((flags & kFSEventStreamEventFlagMustScanSubDirs) != 0)
		{
			emit(watch_event{ .kind = watch_event_kind::overflow, .path = relative }, events);
			continue;
		}

		const entry_kind kind = kindFromFlags(flags);
		const bool exists = pathExists(reported.path);
		if ((flags & kFSEventStreamEventFlagItemRemoved) != 0 && !exists)
		{
			emit(watch_event{ .kind = watch_event_kind::removed, .entry = kind, .path = relative }, events);
			continue;
		}

		if ((flags & kFSEventStreamEventFlagItemRenamed) != 0)
		{
			if (!exists && i + 1 < batch.size())
			{
				const stream_event& next = batch[i + 1];
				if ((next.flags & kFSEventStreamEventFlagItemRenamed) != 0 && next.id == reported.id + 1 && !next.relative.empty() && pathExists(next.path))
				{
					emit(watch_event{ .kind = watch_event_kind::renamed, .entry = kindFromFlags(next.flags), .path = next.relative, .old_path = relative }, events);
					++i;
					continue;
				}
			}

			// Moved across the root's boundary, or reported unpaired
			emit(watch_event{ .kind = exists ? watch_event_kind::created : watch_event_kind::removed, .entry = kind, .path = relative }, events);
			if (!exists)
				continue;
		}
		else if ((flags & kFSEventStreamEventFlagItemCreated) != 0 && exists)
			emit(watch_event{ .kind = watch_event_kind::created, .entry = kind, .path = relative }, events);

		if (!exists)
			continue;
		if ((flags & kFSEventStreamEventFlagItemModified) != 0)
			emit(watch_event{ .kind = watch_event_kind::modified, .entry = kind, .path = relative }, events);
		if ((flags & metadataFlags) != 0)
			emit(watch_event{ .kind = watch_event_kind::metadata_changed, .entry = kind, .path = relative }, events);
	}
}

// Empty for a path outside the root
std::optional<std::string> directory_watcher::impl::relativePath(const std::string_view absolutePath) const
{
	return pathUnder(absolutePath, rootSpellings);
}

std::optional<stream_event> directory_watcher::impl::relevantLinkEvent(const link_stream& link, const char* const absolutePath,
	const FSEventStreamEventFlags flags, const FSEventStreamEventId id)
{
	stream_event reported{ .path = absolutePath, .relative = link.path, .flags = flags, .id = id };
	if ((flags & (kFSEventStreamEventFlagUnmount | kFSEventStreamEventFlagRootChanged)) != 0)
	{
		reported.lostLinkId = link.id;
		return reported;
	}
	if ((flags & (kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped)) != 0)
	{
		reported.flags = kFSEventStreamEventFlagMustScanSubDirs; // An overflow under the link's path, not the root's
		return reported;
	}

	const auto underTarget = pathUnder(reported.path, link.targetSpellings);
	// The target's own changes are no entry events: its loss arrives as RootChanged
	if (!underTarget || (underTarget->empty() && (flags & kFSEventStreamEventFlagMustScanSubDirs) == 0))
		return {};

	reported.relative = joinedPath(link.path, *underTarget);
	return reported;
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
	{
		// An unmount removes the root path too: a mount point no longer holding the root's volume tells it from a deletion.
		struct stat mounted{};
		const bool unmounted = rootId && !mountPoint.empty()
			&& (::stat(mountPoint.c_str(), &mounted) != 0 || static_cast<filesystem_identity>(mounted.st_dev) != rootId->filesystem);
		return unmounted ? stop_reason::volume_released : stop_reason::root_lost;
	}
	if (!rootId || !metadata->identity || *metadata->identity != *rootId)
		return stop_reason::root_replaced;
	return {};
}

void directory_watcher::impl::stopWatch(const stop_reason reason, const filesystem_error error) noexcept
{
	if (pendingStop || stopDelivered)
		return;

	tearDownNotifications();
	pendingStop = watch_event{ .kind = watch_event_kind::stopped, .reason = reason, .error = error };
	std::lock_guard lock{ mutex };
	signal(); // A waiter wakes for the stop
}

void directory_watcher::impl::followLinksUnder(const std::string& relativeDirectory)
{
	const auto entries = list_directory(joinedPath(root, relativeDirectory).c_str(), listing_detail::basic);
	if (!entries)
		return;

	for (const directory_entry& entry : *entries)
	{
		const std::string path = joinedPath(relativeDirectory, entry.name);
		if (isExcluded(path, excludedDirectories))
			continue;

		if (entry.attributes.is_link)
		{
			if (isFollowableDirectoryLink(root, path))
				followLink(path);
		}
		else if (entry.attributes.kind == entry_kind::directory)
			followLinksUnder(path);
	}
}

void directory_watcher::impl::followLink(const std::string& relativePath)
{
	if (isExcluded(relativePath, excludedDirectories))
		return;

	// Events and walks can both reach one link: one already followed to the same target keeps its stream.
	// A link replaced in place has another target.
	const auto targetMetadata = get_entry_metadata(joinedPath(root, relativePath).c_str(), link_behavior::follow);
	const std::optional<entry_identity> targetId = targetMetadata ? targetMetadata->identity : std::nullopt;
	if (targetId && std::ranges::any_of(linkStreams, [&](const auto& link) { return link->path == relativePath && link->targetId == targetId; }))
		return;
	unfollowLinks(relativePath, true);

	char* const resolved = ::realpath(joinedPath(root, relativePath).c_str(), nullptr);
	if (resolved == nullptr)
		return;

	auto link = std::make_unique<link_stream>();
	link->watcher = this;
	link->id = ++lastLinkId;
	link->targetId = targetId;
	link->path = relativePath;
	link->targetSpellings.emplace_back(resolved);
	::free(resolved);

	const std::string target = link->targetSpellings.front(); // A copy: the spellings grow below
	if (const int targetFd = ::open(target.c_str(), O_EVTONLY | O_CLOEXEC); targetFd >= 0)
	{
		char stored[MAXPATHLEN]{};
		if (::fcntl(targetFd, F_GETPATH, stored) == 0)
			link->targetSpellings.emplace_back(stored);
		::close(targetFd);
	}

	struct statfs volume{};
	if (::statfs(target.c_str(), &volume) != 0 || (volume.f_flags & MNT_LOCAL) == 0)
		coverage = watch_coverage::partial;

	link->stream = createStream(target, onLinkStreamEvents, link.get(), coalescingLatency);
	if (link->stream == nullptr)
	{
		coverage = watch_coverage::partial;
		return;
	}

	::FSEventStreamSetDispatchQueue(link->stream, queue);
	if (!::FSEventStreamStart(link->stream))
	{
		// Never started, so no callback of it runs
		::FSEventStreamInvalidate(link->stream);
		::FSEventStreamRelease(link->stream);
		coverage = watch_coverage::partial;
		return;
	}

	linkStreams.push_back(std::move(link));
	followLinksUnder(relativePath);
}

void directory_watcher::impl::unfollowLinks(const std::string_view relativePath, const bool includingItself) noexcept
{
	const auto unfollowed = std::partition(linkStreams.begin(), linkStreams.end(), [&](const auto& link) {
		return !relativePath.empty() && !(isSameOrUnder(link->path, relativePath) && (includingItself || link->path != relativePath));
	});

	for (auto link = unfollowed; link != linkStreams.end(); ++link)
		releaseStream((*link)->stream, queue);
	linkStreams.erase(unfollowed, linkStreams.end());
}

void directory_watcher::impl::updateFollowedLinks(const std::vector<watch_event>& events)
{
	// A directory that appears may be a link, or hold some
	const auto appeared = [this](const std::string& path) {
		if (isFollowableDirectoryLink(root, path))
		{
			followLink(path);
			return;
		}

		unfollowLinks(path, true); // What appeared may have replaced a followed link
		if (const auto status = get_entry_status(joinedPath(root, path).c_str(), link_behavior::do_not_follow);
			status && status->attributes.kind == entry_kind::directory && !status->attributes.is_link)
			followLinksUnder(path);
	};

	for (const watch_event& event : events)
	{
		switch (event.kind)
		{
		case watch_event_kind::created:
			if (event.entry != entry_kind::regular_file)
				appeared(event.path);
			break;
		case watch_event_kind::removed:
			unfollowLinks(event.path, true);
			break;
		case watch_event_kind::renamed:
			unfollowLinks(event.old_path, true);
			if (event.entry != entry_kind::regular_file)
				appeared(event.path);
			break;
		case watch_event_kind::overflow: // Links may have come and gone unreported under the path
			unfollowLinks(event.path, false);
			if (isWalkable(event.path))
				followLinksUnder(event.path);
			break;
		default:
			break;
		}
	}
}

bool directory_watcher::impl::isWalkable(const std::string& relativePath) const
{
	if (relativePath.empty() || std::ranges::any_of(linkStreams, [&relativePath](const auto& link) { return link->path == relativePath; }))
		return true;

	const auto status = get_entry_status(joinedPath(root, relativePath).c_str(), link_behavior::do_not_follow);
	return status && !status->attributes.is_link;
}

void directory_watcher::impl::updateFollowedLinksForExclusions(const std::vector<native_string>& previouslyExcluded)
{
	for (const native_string& directory : excludedDirectories)
		unfollowLinks(directory, true);

	for (const native_string& directory : previouslyExcluded)
	{
		if (directory.empty() || isExcluded(directory, excludedDirectories))
			continue;
		if (isFollowableDirectoryLink(root, directory))
			followLink(directory);
		else
			followLinksUnder(directory);
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
	if (!_impl)
		return;

	const std::vector<native_string> previous = std::exchange(_impl->excludedDirectories, normalizedRelativeDirectories(std::move(directories)));
	if (_impl->followDirectoryLinks && !_impl->pendingStop && !_impl->stopDelivered)
		_impl->updateFollowedLinksForExclusions(previous);
}

void directory_watcher::release() noexcept
{
	if (_impl)
		_impl->stopWatch(stop_reason::volume_released, {});
}

} // namespace thin_io
