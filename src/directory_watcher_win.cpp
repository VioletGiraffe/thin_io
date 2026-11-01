#include "directory_watcher.hpp"
#include "handle_win.hpp"
#include "windows_path_win.hpp"

#include <Windows.h>
#include <cfgmgr32.h>
#include <initguid.h> // Before ioevent.h, so that its GUIDs are defined here
#include <ioevent.h>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#pragma comment(lib, "cfgmgr32.lib") // CM_Register_Notification

namespace thin_io {

namespace {

// Larger reads fail on network shares.
constexpr DWORD rootBufferSize = 64 * 1024;
// The parent's records are never read: any completion, an overflow included, is the signal.
constexpr DWORD parentBufferSize = 1024;

constexpr DWORD rootFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE
	| FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_ATTRIBUTES;

[[nodiscard]] bool isUnsupported(const DWORD error) noexcept
{
	return error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED;
}

[[nodiscard]] DWORD waitMilliseconds(const std::chrono::milliseconds timeout) noexcept
{
	if (timeout.count() < 0)
		return INFINITE;
	return static_cast<DWORD>(std::min<std::chrono::milliseconds::rep>(timeout.count(), INFINITE - 1));
}

[[nodiscard]] HANDLE openDirectoryForWatching(const std::wstring& path) noexcept
{
	// FILE_READ_ATTRIBUTES: for GetFileInformationByHandle
	return ::CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
}

// A zero file index identifies nothing: the exFAT root reports one.
[[nodiscard]] std::optional<entry_identity> knownIdentity(const HANDLE handle) noexcept
{
	auto identity = identityForHandle(handle);
	if (identity && identity->entry == decltype(identity->entry){})
		return {};
	return identity;
}

// A prepared path that is not a root
[[nodiscard]] std::wstring parentPathOf(const std::wstring& path)
{
	std::wstring parent = path.substr(0, path.find_last_of(L'\\'));
	if (isRootPath(parent + L'\\'))
		parent += L'\\';
	return parent;
}

[[nodiscard]] filesystem_result<std::wstring> preparedAbsolutePath(const wchar_t* const nativePath)
{
	std::wstring absolute;
	{
		const windows_path_buffer prepared{ nativePath };
		if (!prepared) [[unlikely]]
			return std::unexpected{ filesystem_error{ .native_code = prepared.error_code() } };

		// Relative input stays unprefixed.
		if (std::wstring_view{ prepared.c_str(), prepared.length() }.starts_with(LR"(\\?\)"))
			absolute.assign(prepared.c_str(), prepared.length());
		else
		{
			const DWORD required = ::GetFullPathNameW(prepared.c_str(), 0, nullptr, nullptr);
			if (required == 0) [[unlikely]]
				return std::unexpected{ capture_last_filesystem_error() };

			absolute.resize(required);
			const DWORD written = ::GetFullPathNameW(prepared.c_str(), required, absolute.data(), nullptr);
			if (written == 0 || written >= required) [[unlikely]]
				return std::unexpected{ capture_last_filesystem_error() };
			absolute.resize(written);
		}
	}

	const windows_path_buffer prefixed{ absolute.c_str() };
	if (!prefixed) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = prefixed.error_code() } };

	std::wstring path{ prefixed.c_str(), prefixed.length() };
	if (!isRootPath(path) && path.ends_with(L'\\'))
		path.pop_back();
	return path;
}

[[nodiscard]] watch_coverage coverageOf(const std::wstring& root)
{
	if (root.starts_with(LR"(\\?\UNC\)"))
		return watch_coverage::partial;

	std::wstring volume(root.size() + 2, L'\0'); // A volume path is never longer than a path on it, plus its trailing separator
	if (::GetVolumePathNameW(root.c_str(), volume.data(), static_cast<DWORD>(volume.size())) == 0)
		return watch_coverage::partial;

	return ::GetDriveTypeW(volume.c_str()) == DRIVE_REMOTE ? watch_coverage::partial : watch_coverage::complete;
}

[[nodiscard]] std::vector<native_string> normalizedRelativeDirectories(std::vector<native_string> directories)
{
	for (native_string& directory : directories)
	{
		std::replace(directory.begin(), directory.end(), L'/', L'\\');
		while (directory.ends_with(L'\\'))
			directory.pop_back();
	}
	return directories;
}

// NTFS and FAT names compare case-insensitively.
[[nodiscard]] bool hasPrefixIgnoringCase(const std::wstring_view path, const std::wstring_view prefix) noexcept
{
	return path.size() >= prefix.size()
		&& ::CompareStringOrdinal(path.data(), static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool isUnder(const std::wstring_view path, const std::wstring_view directory) noexcept
{
	return !directory.empty() && path.size() > directory.size() && path[directory.size()] == L'\\' && hasPrefixIgnoringCase(path, directory);
}

[[nodiscard]] bool isSameDirectory(const std::wstring_view path, const std::wstring_view directory) noexcept
{
	return !directory.empty() && path.size() == directory.size() && hasPrefixIgnoringCase(path, directory);
}

DWORD CALLBACK onVolumeEvent(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA eventData, DWORD) noexcept;

} // namespace

// The callback registered with CM_Register_Notification runs on an OS thread; everything else runs on the caller's.
struct directory_watcher::impl {
	// One ReadDirectoryChanges(Ex)W on one directory handle
	struct read_slot {
		HANDLE directory = INVALID_HANDLE_VALUE;
		OVERLAPPED overlapped{};
		std::unique_ptr<uint64_t[]> buffer; // 8-byte alignment for FILE_NOTIFY_EXTENDED_INFORMATION
		DWORD bufferSize = 0;
		bool readPending = false;
		bool extendedInformation = false; // How the pending read was issued
		HCMNOTIFICATION volumeNotification = nullptr;

		[[nodiscard]] bool completed() const noexcept { return readPending && HasOverlappedIoCompleted(&overlapped); }
	};

	impl() = default;
	~impl();

	impl(const impl&) = delete;
	impl& operator=(const impl&) = delete;

	[[nodiscard]] filesystem_result<void> arm(watch_options options);
	[[nodiscard]] std::optional<watch_event> drain(std::vector<watch_event>* events, bool& any);
	[[nodiscard]] DWORD issueRead(read_slot& slot, DWORD filter, bool subtree, bool extendedInformation) noexcept;
	[[nodiscard]] DWORD issueRootRead() noexcept;
	void processRootCompletion(std::vector<watch_event>* events, bool& any);
	void processParentCompletion(bool& any) noexcept;
	void appendRecords(std::vector<watch_event>& events) const;
	void appendUnlessExcluded(watch_event event, std::vector<watch_event>& events) const;
	[[nodiscard]] std::optional<stop_reason> rootLossReason() const noexcept;
	void failRoot(DWORD error) noexcept;
	void stopWatch(stop_reason reason, filesystem_error error) noexcept;
	void closeSlot(read_slot& slot) noexcept;
	void registerVolumeNotification(read_slot& slot, HANDLE directory) noexcept;
	// Never called under the mutex, or from the callback: unregistering waits for a running callback.
	void unregisterVolumeNotifications() noexcept;

	std::mutex mutex; // Shared with the volume callback
	std::wstring root; // Prepared and absolute; a trailing separator only on a root
	HANDLE waitEvent = nullptr; // Manual-reset; both slots' OVERLAPPED signal it
	watch_coverage coverage = watch_coverage::complete;
	bool recursive = true;
	std::vector<native_string> excludedDirectories;
	std::optional<entry_identity> rootId; // Of the directory rootSlot watches
	bool extendedInformationSupported = true; // Cleared for good when the filesystem rejects it
	read_slot rootSlot;
	read_slot parentSlot; // Directory renames in the parent: the root's own handle never reports the root moving
	std::optional<watch_event> pendingStop;
	bool stopDelivered = false;
};

namespace {

DWORD CALLBACK onVolumeEvent(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA eventData, DWORD) noexcept
{
	bool releasing = false;
	switch (action)
	{
	case CM_NOTIFY_ACTION_DEVICEQUERYREMOVE:
	case CM_NOTIFY_ACTION_DEVICEREMOVEPENDING:
	case CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE:
		releasing = true;
		break;
	case CM_NOTIFY_ACTION_DEVICECUSTOMEVENT:
		// Safely Remove of removable media locks the volume and sends no query-remove.
		releasing = eventData != nullptr && (::IsEqualGUID(eventData->u.DeviceHandle.EventGuid, GUID_IO_VOLUME_LOCK)
			|| ::IsEqualGUID(eventData->u.DeviceHandle.EventGuid, GUID_IO_VOLUME_DISMOUNT));
		break;
	default:
		break;
	}

	if (releasing)
	{
		auto& watcher = *static_cast<directory_watcher::impl*>(context);
		std::lock_guard lock{ watcher.mutex };
		watcher.stopWatch(stop_reason::volume_released, {});
	}

	return ERROR_SUCCESS; // Never vetoes the removal
}

} // namespace

directory_watcher::impl::~impl()
{
	{
		std::lock_guard lock{ mutex };
		closeSlot(rootSlot);
		closeSlot(parentSlot);
	}

	unregisterVolumeNotifications();
	if (waitEvent != nullptr)
		::CloseHandle(waitEvent);
}

filesystem_result<void> directory_watcher::impl::arm(watch_options options)
{
	recursive = options.recursive;
	excludedDirectories = normalizedRelativeDirectories(std::move(options.excluded_directories));

	waitEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (waitEvent == nullptr) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };

	for (read_slot* slot : { &rootSlot, &parentSlot })
		slot->overlapped.hEvent = waitEvent;
	rootSlot.bufferSize = rootBufferSize;
	parentSlot.bufferSize = parentBufferSize;
	rootSlot.buffer = std::make_unique<uint64_t[]>(rootBufferSize / sizeof(uint64_t));
	parentSlot.buffer = std::make_unique<uint64_t[]>(parentBufferSize / sizeof(uint64_t));

	// The parent is watched first, so a replacement of the root after its handle opens still signals.
	// A root without a parent directory cannot move.
	bool rootTracked = isRootPath(root);
	if (!rootTracked)
	{
		parentSlot.directory = openDirectoryForWatching(parentPathOf(root));
		if (parentSlot.directory != INVALID_HANDLE_VALUE)
		{
			rootTracked = issueRead(parentSlot, FILE_NOTIFY_CHANGE_DIR_NAME, false, false) == ERROR_SUCCESS;
			if (!rootTracked)
				closeSlot(parentSlot);
		}
	}

	rootSlot.directory = openDirectoryForWatching(root);
	if (rootSlot.directory == INVALID_HANDLE_VALUE) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };

	BY_HANDLE_FILE_INFORMATION information;
	if (::GetFileInformationByHandle(rootSlot.directory, &information) == 0) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };
	if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = ERROR_DIRECTORY } };

	// From the handle itself, so the identity is exactly that of the watched directory.
	rootId = knownIdentity(rootSlot.directory);
	if (const DWORD error = issueRootRead(); error != ERROR_SUCCESS) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = error } };
	// Issuing the root's read cleared the event the parent's read may already have set.
	if (parentSlot.completed())
		::SetEvent(waitEvent);

	coverage = rootTracked ? coverageOf(root) : watch_coverage::partial;

	// Last: the callback may run as soon as one registration exists, and close both handles.
	// So both are read first; registering a handle it already closed fails, or reaches an already stopped watch.
	const HANDLE rootDirectory = rootSlot.directory;
	const HANDLE parentDirectory = parentSlot.directory;
	registerVolumeNotification(rootSlot, rootDirectory);
	registerVolumeNotification(parentSlot, parentDirectory);
	return {};
}

DWORD directory_watcher::impl::issueRead(read_slot& slot, const DWORD filter, const bool subtree, const bool extendedInformation) noexcept
{
	DWORD bytesReturned = 0; // Undefined for an overlapped read, but not documented as optional
	const BOOL issued = extendedInformation
		? ::ReadDirectoryChangesExW(slot.directory, slot.buffer.get(), slot.bufferSize, subtree, filter, &bytesReturned, &slot.overlapped,
			nullptr, ReadDirectoryNotifyExtendedInformation)
		: ::ReadDirectoryChangesW(slot.directory, slot.buffer.get(), slot.bufferSize, subtree, filter, &bytesReturned, &slot.overlapped, nullptr);
	if (issued == 0)
		return ::GetLastError();

	slot.readPending = true;
	slot.extendedInformation = extendedInformation;
	return ERROR_SUCCESS;
}

DWORD directory_watcher::impl::issueRootRead() noexcept
{
	if (extendedInformationSupported)
	{
		const DWORD error = issueRead(rootSlot, rootFilter, recursive, true);
		if (!isUnsupported(error))
			return error;
		extendedInformationSupported = false;
	}

	return issueRead(rootSlot, rootFilter, recursive, false);
}

std::optional<watch_event> directory_watcher::impl::drain(std::vector<watch_event>* const events, bool& any)
{
	if (!stopDelivered && !pendingStop)
	{
		if (rootSlot.completed())
			processRootCompletion(events, any);
		if (!pendingStop && parentSlot.completed())
			processParentCompletion(any);
	}

	// Reset before the re-check: a read completing after it signals again.
	::ResetEvent(waitEvent);
	if (pendingStop)
	{
		any = true;
		stopDelivered = true;
		return std::exchange(pendingStop, std::nullopt);
	}

	if (rootSlot.completed() || parentSlot.completed())
		::SetEvent(waitEvent);
	return {};
}

void directory_watcher::impl::processRootCompletion(std::vector<watch_event>* const events, bool& any)
{
	DWORD bytes = 0;
	const bool succeeded = ::GetOverlappedResult(rootSlot.directory, &rootSlot.overlapped, &bytes, FALSE) != 0;
	const DWORD error = succeeded ? ERROR_SUCCESS : ::GetLastError();
	rootSlot.readPending = false;

	if (succeeded || error == ERROR_NOTIFY_ENUM_DIR)
	{
		any = true;
		// A completion with no bytes is an overflow too.
		if (events != nullptr && (!succeeded || bytes == 0))
			events->push_back(watch_event{ .kind = watch_event_kind::overflow });
		else if (events != nullptr)
			appendRecords(*events);
	}
	else if (rootSlot.extendedInformation && isUnsupported(error))
		extendedInformationSupported = false;
	// ERROR_OPERATION_ABORTED: the thread that issued the read exited. A release never gets here: closeSlot() waits out its own cancel.
	// The handle keeps the changes between reads, so re-issuing loses nothing.
	else if (error != ERROR_OPERATION_ABORTED)
	{
		failRoot(error);
		return;
	}

	if (const DWORD issueError = issueRootRead(); issueError != ERROR_SUCCESS)
		failRoot(issueError);
}

void directory_watcher::impl::processParentCompletion(bool& any) noexcept
{
	DWORD bytes = 0;
	::GetOverlappedResult(parentSlot.directory, &parentSlot.overlapped, &bytes, FALSE);
	parentSlot.readPending = false;

	// Re-armed before the identity check, so a replacement after the check signals again.
	const DWORD issueError = issueRead(parentSlot, FILE_NOTIFY_CHANGE_DIR_NAME, false, false);
	if (const auto reason = rootLossReason())
	{
		any = true;
		stopWatch(*reason, {});
	}
	else if (issueError != ERROR_SUCCESS)
	{
		any = true;
		stopWatch(stop_reason::failed, filesystem_error{ .native_code = issueError });
	}
}

void directory_watcher::impl::appendRecords(std::vector<watch_event>& events) const
{
	const auto* const records = reinterpret_cast<const std::byte*>(rootSlot.buffer.get());
	std::optional<watch_event> renameSource; // An old name waiting for its new name

	const auto flushRenameSource = [&] {
		if (renameSource)
			appendUnlessExcluded(watch_event{ .kind = watch_event_kind::removed, .entry = renameSource->entry, .path = std::move(renameSource->old_path) }, events);
		renameSource.reset();
	};

	for (DWORD offset = 0;;)
	{
		DWORD nextOffset = 0;
		DWORD action = 0;
		std::wstring_view name;
		entry_kind kind = entry_kind::unknown;
		if (rootSlot.extendedInformation)
		{
			const auto& record = *reinterpret_cast<const FILE_NOTIFY_EXTENDED_INFORMATION*>(records + offset);
			nextOffset = record.NextEntryOffset;
			action = record.Action;
			name = { record.FileName, record.FileNameLength / sizeof(WCHAR) };
			kind = (record.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? entry_kind::directory : entry_kind::regular_file;
		}
		else
		{
			const auto& record = *reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(records + offset);
			nextOffset = record.NextEntryOffset;
			action = record.Action;
			name = { record.FileName, record.FileNameLength / sizeof(WCHAR) };
		}

		if (action != FILE_ACTION_RENAMED_NEW_NAME)
			flushRenameSource();

		switch (action)
		{
		case FILE_ACTION_ADDED:
			appendUnlessExcluded(watch_event{ .kind = watch_event_kind::created, .entry = kind, .path = native_string{ name } }, events);
			break;
		case FILE_ACTION_REMOVED:
			appendUnlessExcluded(watch_event{ .kind = watch_event_kind::removed, .entry = kind, .path = native_string{ name } }, events);
			break;
		case FILE_ACTION_MODIFIED:
			appendUnlessExcluded(watch_event{ .kind = watch_event_kind::modified, .entry = kind, .path = native_string{ name } }, events);
			break;
		case FILE_ACTION_RENAMED_OLD_NAME:
			renameSource = watch_event{ .kind = watch_event_kind::renamed, .entry = kind, .old_path = native_string{ name } };
			break;
		case FILE_ACTION_RENAMED_NEW_NAME:
			if (renameSource)
			{
				renameSource->entry = kind;
				renameSource->path = native_string{ name };
				appendUnlessExcluded(std::move(*renameSource), events);
				renameSource.reset();
			}
			else
				appendUnlessExcluded(watch_event{ .kind = watch_event_kind::created, .entry = kind, .path = native_string{ name } }, events);
			break;
		default:
			break;
		}

		if (nextOffset == 0)
			break;
		offset += nextOffset;
	}

	flushRenameSource();
}

// A rename across an excluded directory's boundary is a creation or a removal.
// An excluded directory's own modification is dropped: Windows reports one for each entry deleted inside it.
void directory_watcher::impl::appendUnlessExcluded(watch_event event, std::vector<watch_event>& events) const
{
	const auto excluded = [this](const std::wstring_view path) {
		return std::ranges::any_of(excludedDirectories, [path](const native_string& directory) { return isUnder(path, directory); });
	};

	if (event.kind == watch_event_kind::modified && std::ranges::any_of(excludedDirectories,
		[&event](const native_string& directory) { return isSameDirectory(event.path, directory); }))
		return;

	const bool pathExcluded = excluded(event.path);
	if (event.kind == watch_event_kind::renamed)
	{
		const bool oldPathExcluded = excluded(event.old_path);
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

	events.push_back(std::move(event));
}

// Empty while the root path still resolves to the watched directory. An unknown identity never matches.
std::optional<stop_reason> directory_watcher::impl::rootLossReason() const noexcept
{
	const HANDLE nativeHandle = ::CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (nativeHandle == INVALID_HANDLE_VALUE)
		return stop_reason::root_lost;
	file_handle handle{ nativeHandle };

	const auto currentId = knownIdentity(nativeHandle);
	if (!rootId || !currentId || *currentId != *rootId)
		return stop_reason::root_replaced;
	return {};
}

// A deleted root fails its read too, so the failure is reported as the loss when it is one.
void directory_watcher::impl::failRoot(const DWORD error) noexcept
{
	const auto reason = rootLossReason();
	stopWatch(reason.value_or(stop_reason::failed), reason ? filesystem_error{} : filesystem_error{ .native_code = error });
}

// Requires the mutex
void directory_watcher::impl::stopWatch(const stop_reason reason, const filesystem_error error) noexcept
{
	if (pendingStop || stopDelivered)
		return;

	closeSlot(rootSlot);
	closeSlot(parentSlot);
	pendingStop = watch_event{ .kind = watch_event_kind::stopped, .reason = reason, .error = error };
	::SetEvent(waitEvent);
}

// Requires the mutex
void directory_watcher::impl::closeSlot(read_slot& slot) noexcept
{
	if (slot.directory == INVALID_HANDLE_VALUE)
		return;

	if (slot.readPending)
	{
		// The kernel writes into the buffer and the OVERLAPPED until the read completes, even after a cancel.
		// The slots share the event, so it is reset and the completion re-checked before each wait.
		::CancelIoEx(slot.directory, &slot.overlapped);
		for (;;)
		{
			::ResetEvent(waitEvent);
			if (HasOverlappedIoCompleted(&slot.overlapped))
				break;
			::WaitForSingleObject(waitEvent, INFINITE);
		}
		slot.readPending = false;
	}

	::CloseHandle(slot.directory);
	slot.directory = INVALID_HANDLE_VALUE;
}

// Fails for a handle with no removable device behind it, such as one on a network share: nothing to release there.
void directory_watcher::impl::registerVolumeNotification(read_slot& slot, const HANDLE directory) noexcept
{
	if (directory == INVALID_HANDLE_VALUE)
		return;

	CM_NOTIFY_FILTER filter{};
	filter.cbSize = sizeof(filter);
	filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
	filter.u.DeviceHandle.hTarget = directory;
	if (::CM_Register_Notification(&filter, this, onVolumeEvent, &slot.volumeNotification) != CR_SUCCESS)
		slot.volumeNotification = nullptr;
}

void directory_watcher::impl::unregisterVolumeNotifications() noexcept
{
	for (read_slot* slot : { &rootSlot, &parentSlot })
	{
		if (slot->volumeNotification != nullptr)
			::CM_Unregister_Notification(std::exchange(slot->volumeNotification, nullptr));
	}
}

template <class Character>
[[nodiscard]] static filesystem_result<std::unique_ptr<directory_watcher::impl>> armWatcher(const Character* root, watch_options options)
{
	auto watcher = std::make_unique<directory_watcher::impl>();
	if constexpr (std::is_same_v<Character, wchar_t>)
	{
		auto path = preparedAbsolutePath(root);
		if (!path) [[unlikely]]
			return std::unexpected{ path.error() };
		watcher->root = std::move(*path);
	}
	else
	{
		const windows_path_buffer widened{ root };
		if (!widened) [[unlikely]]
			return std::unexpected{ filesystem_error{ .native_code = widened.error_code() } };
		auto path = preparedAbsolutePath(widened.c_str());
		if (!path) [[unlikely]]
			return std::unexpected{ path.error() };
		watcher->root = std::move(*path);
	}

	if (auto armed = watcher->arm(std::move(options)); !armed) [[unlikely]]
		return std::unexpected{ armed.error() };
	return watcher;
}

filesystem_result<directory_watcher> directory_watcher::watch(const wchar_t* root, watch_options options)
{
	auto watcherImpl = armWatcher(root, std::move(options));
	if (!watcherImpl) [[unlikely]]
		return std::unexpected{ watcherImpl.error() };
	return directory_watcher{ std::move(*watcherImpl) };
}

filesystem_result<directory_watcher> directory_watcher::watch(const char* utf8Root, watch_options options)
{
	auto watcherImpl = armWatcher(utf8Root, std::move(options));
	if (!watcherImpl) [[unlikely]]
		return std::unexpected{ watcherImpl.error() };
	return directory_watcher{ std::move(*watcherImpl) };
}

directory_watcher::directory_watcher(std::unique_ptr<impl> watcherImpl) noexcept : _impl{ std::move(watcherImpl) } {}
directory_watcher::directory_watcher(directory_watcher&&) noexcept = default;
directory_watcher& directory_watcher::operator=(directory_watcher&&) noexcept = default;
directory_watcher::~directory_watcher() = default;

native_wait_handle directory_watcher::wait_handle() const noexcept
{
	return _impl ? _impl->waitEvent : nullptr;
}

filesystem_result<std::vector<watch_event>> directory_watcher::read_events()
{
	if (!_impl) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = ERROR_INVALID_HANDLE } };

	std::vector<watch_event> events;
	bool any = false;
	std::lock_guard lock{ _impl->mutex };
	if (auto stop = _impl->drain(&events, any))
		events.push_back(std::move(*stop));
	return events;
}

filesystem_result<discarded_events> directory_watcher::discard_events()
{
	if (!_impl) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = ERROR_INVALID_HANDLE } };

	discarded_events discarded;
	std::lock_guard lock{ _impl->mutex };
	discarded.stop = _impl->drain(nullptr, discarded.any);
	return discarded;
}

filesystem_result<bool> directory_watcher::wait_for_events(const std::chrono::milliseconds timeout)
{
	if (!_impl) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = ERROR_INVALID_HANDLE } };

	switch (::WaitForSingleObject(_impl->waitEvent, waitMilliseconds(timeout)))
	{
	case WAIT_OBJECT_0:
		return true;
	case WAIT_TIMEOUT:
		return false;
	default:
		return std::unexpected{ capture_last_filesystem_error() };
	}
}

watch_coverage directory_watcher::coverage() const noexcept
{
	return _impl ? _impl->coverage : watch_coverage::partial;
}

void directory_watcher::set_excluded_directories(std::vector<native_string> directories)
{
	if (!_impl) [[unlikely]]
		return;

	auto normalized = normalizedRelativeDirectories(std::move(directories));
	std::lock_guard lock{ _impl->mutex };
	_impl->excludedDirectories = std::move(normalized);
}

void directory_watcher::release() noexcept
{
	if (!_impl)
		return;

	{
		std::lock_guard lock{ _impl->mutex };
		_impl->stopWatch(stop_reason::volume_released, {});
	}
	_impl->unregisterVolumeNotifications();
}

filesystem_result<waker> waker::create()
{
	const HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (event == nullptr) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };
	return waker{ event };
}

waker::waker(const native_wait_handle handle) noexcept : _handle{ handle } {}

waker::waker(waker&& other) noexcept : _handle{ std::exchange(other._handle, nullptr) } {}

waker& waker::operator=(waker&& other) noexcept
{
	if (this != &other)
	{
		if (_handle != nullptr)
			::CloseHandle(_handle);
		_handle = std::exchange(other._handle, nullptr);
	}
	return *this;
}

waker::~waker()
{
	if (_handle != nullptr)
		::CloseHandle(_handle);
}

void waker::signal() noexcept
{
	if (_handle != nullptr)
		::SetEvent(_handle);
}

filesystem_result<wait_result> wait_for_any(const std::span<directory_watcher* const> watchers, waker* const wakeUp,
	const std::chrono::milliseconds timeout)
{
	std::vector<HANDLE> handles;
	handles.reserve(watchers.size() + 1);
	// First: the wait then consumes the waker's signal whenever it is set.
	if (wakeUp != nullptr)
		handles.push_back(wakeUp->_handle);
	for (directory_watcher* const watcher : watchers)
		handles.push_back(watcher != nullptr ? watcher->wait_handle() : nullptr);

	if (handles.empty() || handles.size() > MAXIMUM_WAIT_OBJECTS || std::ranges::find(handles, nullptr) != handles.end()) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = ERROR_INVALID_PARAMETER } };

	const DWORD signalled = ::WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, waitMilliseconds(timeout));
	if (signalled == WAIT_TIMEOUT)
		return wait_result{};
	if (signalled >= WAIT_OBJECT_0 + handles.size()) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };

	wait_result result;
	result.woken = wakeUp != nullptr && (signalled == WAIT_OBJECT_0 || ::WaitForSingleObject(wakeUp->_handle, 0) == WAIT_OBJECT_0);
	for (directory_watcher* const watcher : watchers)
	{
		if (::WaitForSingleObject(watcher->wait_handle(), 0) == WAIT_OBJECT_0)
			result.ready.push_back(watcher);
	}
	return result;
}

} // namespace thin_io
