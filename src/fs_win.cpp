#include "fs.hpp"
#include "attributes_win.hpp"
#include "handle_win.hpp"
#include "timestamp_win.hpp"
#include "windows_path_win.hpp"

#include "utility/heap_optional.hpp" // cpp-template-utils

#include <Windows.h>
#include <winioctl.h> // FSCTL_GET_REPARSE_POINT, FSCTL_SET_REPARSE_POINT

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace thin_io {

[[nodiscard]] static bool setPreparedTimes(const wchar_t* path, const entry_times& times, const DWORD openFlags) noexcept
{
	const HANDLE h = ::CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
								  nullptr, OPEN_EXISTING, openFlags, nullptr);
	if (h == INVALID_HANDLE_VALUE) [[unlikely]]
		return false;

	const bool success = setFileTimes(h, times);
	const DWORD error = success ? 0 : ::GetLastError(); // CloseHandle overwrites the thread's last error
	::CloseHandle(h);
	if (!success) [[unlikely]]
		::SetLastError(error);

	return success;
}

// The CreateFileW flags for an attribute handle to a file or a directory. Empty for a value outside the enum.
[[nodiscard]] static std::optional<DWORD> attributeHandleFlagsFor(const link_behavior linkBehavior) noexcept
{
	// FILE_FLAG_BACKUP_SEMANTICS is what makes a handle to a directory possible; it is a no-op for regular files.
	switch (linkBehavior)
	{
	case link_behavior::follow:
		return FILE_FLAG_BACKUP_SEMANTICS;
	case link_behavior::do_not_follow:
		return FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
	default:
		return {};
	}
}

template <class Character>
[[nodiscard]] static bool setTimesForPath(const Character* path, const entry_times& times, const link_behavior linkBehavior) noexcept
{
	const auto openFlags = attributeHandleFlagsFor(linkBehavior);
	if (!openFlags) [[unlikely]]
	{
		::SetLastError(ERROR_INVALID_PARAMETER);
		return false;
	}

	if (!times.creation.is_set() && !times.last_access.is_set() && !times.last_write.is_set())
		return true; // Nothing to write, so don't even open the path - matching the POSIX implementation

	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
	{
		::SetLastError(nativePath.error_code());
		return false;
	}

	return setPreparedTimes(nativePath.c_str(), times, *openFlags);
}

bool set_times(const char* path, const entry_times& times, const link_behavior linkBehavior) noexcept
{
	return setTimesForPath(path, times, linkBehavior);
}

bool set_times(const wchar_t* path, const entry_times& times, const link_behavior linkBehavior) noexcept
{
	return setTimesForPath(path, times, linkBehavior);
}

[[nodiscard]] static entry_times timesFromWindows(const FILETIME& creation, const FILETIME& lastAccess, const FILETIME& lastWrite) noexcept
{
	return entry_times{ .creation = fromFileTime(creation), .last_access = fromFileTime(lastAccess), .last_write = fromFileTime(lastWrite) };
}

[[nodiscard]] static std::optional<entry_times> getPreparedTimes(const wchar_t* path) noexcept
{
	// Reads the metadata without opening the path, so it works for directories and cannot perturb the access time
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	if (::GetFileAttributesExW(path, GetFileExInfoStandard, &attributes) == 0) [[unlikely]]
		return {};

	if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		// GetFileAttributesEx reports the reparse point itself, but the contract is to follow links, so read the
		// target through a following handle. FILE_READ_ATTRIBUTES access does not perturb the target's access time.
		const HANDLE h = ::CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
									  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (h == INVALID_HANDLE_VALUE) [[unlikely]]
			return {};

		BY_HANDLE_FILE_INFORMATION info;
		const bool success = ::GetFileInformationByHandle(h, &info) != 0;
		const DWORD error = success ? 0 : ::GetLastError(); // CloseHandle overwrites the thread's last error
		::CloseHandle(h);
		if (!success) [[unlikely]]
		{
			::SetLastError(error);
			return {};
		}

		return timesFromWindows(info.ftCreationTime, info.ftLastAccessTime, info.ftLastWriteTime);
	}

	return timesFromWindows(attributes.ftCreationTime, attributes.ftLastAccessTime, attributes.ftLastWriteTime);
}

template <class Character>
[[nodiscard]] static std::optional<entry_times> getTimesForPath(const Character* path) noexcept
{
	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
	{
		::SetLastError(nativePath.error_code());
		return {};
	}

	return getPreparedTimes(nativePath.c_str());
}

std::optional<entry_times> get_times(const char* path) noexcept
{
	return getTimesForPath(path);
}

std::optional<entry_times> get_times(const wchar_t* path) noexcept
{
	return getTimesForPath(path);
}

namespace {

class find_handle final {
public:
	explicit find_handle(const HANDLE handle) noexcept : _handle{handle} {}
	~find_handle() noexcept
	{
		if (_handle != INVALID_HANDLE_VALUE)
			::FindClose(_handle);
	}

	find_handle(const find_handle&) = delete;
	find_handle& operator=(const find_handle&) = delete;

	[[nodiscard]] std::optional<filesystem_error> close() noexcept
	{
		if (::FindClose(std::exchange(_handle, INVALID_HANDLE_VALUE)) != 0)
			return {};

		return capture_last_filesystem_error();
	}

private:
	HANDLE _handle;
};

[[nodiscard]] entry_attributes attributesFromWindows(const DWORD nativeAttributes, const DWORD reparseTag) noexcept
{
	entry_attributes attributes;
	attributes.kind = (nativeAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? entry_kind::directory : entry_kind::regular_file;
	const bool isReparsePoint = (nativeAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	attributes.is_link = isReparsePoint && IsReparseTagNameSurrogate(reparseTag);
	attributes.sparse = (nativeAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
	attributes.compressed = (nativeAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0;
	attributes.hidden = (nativeAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
	attributes.reparse_tag = isReparsePoint ? reparseTag : 0;
	return attributes;
}

// sizeHigh and sizeLow are ignored for directories and links: a link's own size is not its target's.
// Other reparse points, such as cloud placeholder files, report their own size.
[[nodiscard]] entry_status statusFromWindows(const DWORD nativeAttributes, const DWORD reparseTag, const DWORD sizeHigh, const DWORD sizeLow,
	const FILETIME& creation, const FILETIME& lastAccess, const FILETIME& lastWrite) noexcept
{
	entry_status status;
	status.attributes = attributesFromWindows(nativeAttributes, reparseTag);
	if (status.attributes.kind == entry_kind::regular_file && !status.attributes.is_link)
		status.logical_size = (static_cast<uint64_t>(sizeHigh) << 32) | sizeLow;
	status.times = timesFromWindows(creation, lastAccess, lastWrite);
	status.permissions = permissionsFromAttributes(nativeAttributes);
	return status;
}

[[nodiscard]] directory_entry entryFromFindData(const WIN32_FIND_DATAW& data)
{
	return directory_entry{
		statusFromWindows(data.dwFileAttributes, data.dwReserved0, data.nFileSizeHigh, data.nFileSizeLow, data.ftCreationTime, data.ftLastAccessTime, data.ftLastWriteTime),
		data.cFileName,
		std::nullopt
	};
}

// Queries the tag only for a reparse point: filesystems without reparse points, such as exFAT, fail the query.
[[nodiscard]] filesystem_result<DWORD> reparseTagForHandle(const HANDLE handle, const DWORD nativeAttributes) noexcept
{
	if ((nativeAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
		return 0;

	FILE_ATTRIBUTE_TAG_INFO tagInfo{};
	if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tagInfo, sizeof(tagInfo)) == 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	return tagInfo.ReparseTag;
}

// Follows the link at path.
[[nodiscard]] filesystem_result<entry_status> followedStatus(const wchar_t* const path) noexcept
{
	const HANDLE nativeHandle = ::CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (nativeHandle == INVALID_HANDLE_VALUE)
		return std::unexpected{capture_last_filesystem_error()};
	file_handle handle{nativeHandle};

	BY_HANDLE_FILE_INFORMATION info;
	if (::GetFileInformationByHandle(nativeHandle, &info) == 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};

	// A target can itself be a reparse point, such as a cloud placeholder.
	const auto reparseTag = reparseTagForHandle(nativeHandle, info.dwFileAttributes);
	if (!reparseTag) [[unlikely]]
		return std::unexpected{reparseTag.error()};

	return statusFromWindows(info.dwFileAttributes, *reparseTag, info.nFileSizeHigh, info.nFileSizeLow,
		info.ftCreationTime, info.ftLastAccessTime, info.ftLastWriteTime);
}

// Absent when the target cannot be reached.
[[nodiscard]] heap_optional<entry_status> linkTargetStatus(const wchar_t* const path)
{
	if (auto target = followedStatus(path))
		return std::move(*target);
	return {};
}

[[nodiscard]] bool isDotEntry(const std::wstring_view name) noexcept
{
	return name == L"." || name == L"..";
}

// FindFirstFileExW also reports ERROR_FILE_NOT_FOUND when the wildcard matched nothing. Verifying the directory after
// the failed search keeps a path removed before enumeration from being mistaken for an empty directory.
template <class Character>
[[nodiscard]] filesystem_result<void> verifyDirectoryExists(const Character* path) noexcept
{
	windows_path_buffer directoryPath{path};
	if (!directoryPath) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = directoryPath.error_code() }};
	const DWORD attributes = ::GetFileAttributesW(directoryPath.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = ERROR_DIRECTORY }};
	return {};
}

template <class Character>
[[nodiscard]] filesystem_result<void> canListDirectory(const Character* path) noexcept
{
	windows_path_buffer searchPath{path};
	if (!searchPath || !searchPath.append_directory_search_pattern()) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = searchPath.error_code() }};

	WIN32_FIND_DATAW data;
	const HANDLE nativeHandle = ::FindFirstFileExW(searchPath.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
	if (nativeHandle == INVALID_HANDLE_VALUE)
	{
		const filesystem_error enumerationError = capture_last_filesystem_error();
		if (enumerationError.native_code != ERROR_FILE_NOT_FOUND)
			return std::unexpected{enumerationError};
		return verifyDirectoryExists(path);
	}

	find_handle handle{nativeHandle};
	if (const auto closeError = handle.close()) [[unlikely]]
		return std::unexpected{*closeError};
	return {};
}

template <class Character>
[[nodiscard]] filesystem_result<std::vector<directory_entry>> listDirectory(const Character* path, const listing_detail detail)
{
	windows_path_buffer searchPath{path};
	if (!searchPath || !searchPath.append_directory_search_pattern()) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = searchPath.error_code() }};

	// The search path without its trailing '*': the directory, as a prefix for its children's paths.
	std::wstring childPathPrefix;
	if (detail == listing_detail::full)
		childPathPrefix.assign(searchPath.c_str(), searchPath.length() - 1);

	WIN32_FIND_DATAW data{};
	const HANDLE nativeHandle = ::FindFirstFileExW(searchPath.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
	if (nativeHandle == INVALID_HANDLE_VALUE) [[unlikely]]
	{
		const filesystem_error enumerationError = capture_last_filesystem_error();
		if (enumerationError.native_code != ERROR_FILE_NOT_FOUND)
			return std::unexpected{enumerationError};

		if (const auto verified = verifyDirectoryExists(path); !verified) [[unlikely]]
			return std::unexpected{verified.error()};
		return std::vector<directory_entry>{};
	}

	find_handle handle{nativeHandle};
	std::vector<directory_entry> entries;
	for (;;)
	{
		if (!isDotEntry(data.cFileName))
		{
			directory_entry entry = entryFromFindData(data);
			if (detail == listing_detail::full && entry.attributes.is_link)
				entry.link_target = linkTargetStatus((childPathPrefix + entry.name).c_str());
			entries.push_back(std::move(entry));
		}

		if (::FindNextFileW(nativeHandle, &data) != 0)
			continue;

		const filesystem_error error = capture_last_filesystem_error();
		if (error.native_code != ERROR_NO_MORE_FILES) [[unlikely]]
			return std::unexpected{error};
		break;
	}

	if (const auto closeError = handle.close()) [[unlikely]]
		return std::unexpected{*closeError};
	return entries;
}

// The caller closes the returned handle.
template <class Character>
[[nodiscard]] filesystem_result<HANDLE> openForAttributes(const Character* path, const DWORD access, const link_behavior linkBehavior) noexcept
{
	const auto openFlags = attributeHandleFlagsFor(linkBehavior);
	if (!openFlags) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_PARAMETER }};

	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = nativePath.error_code() }};

	const HANDLE nativeHandle = ::CreateFileW(nativePath.c_str(), access,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, *openFlags, nullptr);
	if (nativeHandle == INVALID_HANDLE_VALUE) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	return nativeHandle;
}

template <class Character>
[[nodiscard]] filesystem_result<entry_metadata> getEntryMetadata(const Character* path, const link_behavior linkBehavior) noexcept
{
	const auto opened = openForAttributes(path, FILE_READ_ATTRIBUTES, linkBehavior);
	if (!opened) [[unlikely]]
		return std::unexpected{opened.error()};
	const HANDLE nativeHandle = *opened;
	file_handle handle{nativeHandle};

	FILE_STANDARD_INFO standardInfo{};
	if (::GetFileInformationByHandleEx(nativeHandle, FileStandardInfo, &standardInfo, sizeof(standardInfo)) == 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	if (standardInfo.EndOfFile.QuadPart < 0 || standardInfo.AllocationSize.QuadPart < 0) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_DATA }};

	FILE_BASIC_INFO basicInfo{};
	if (::GetFileInformationByHandleEx(nativeHandle, FileBasicInfo, &basicInfo, sizeof(basicInfo)) == 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	const auto reparseTag = reparseTagForHandle(nativeHandle, basicInfo.FileAttributes);
	if (!reparseTag) [[unlikely]]
		return std::unexpected{reparseTag.error()};

	entry_metadata metadata;
	metadata.attributes = attributesFromWindows(basicInfo.FileAttributes, *reparseTag);
	metadata.permissions = permissionsFromAttributes(basicInfo.FileAttributes);
	metadata.logical_size = static_cast<uint64_t>(standardInfo.EndOfFile.QuadPart);
	metadata.allocated_size = static_cast<uint64_t>(standardInfo.AllocationSize.QuadPart);
	metadata.hard_link_count = standardInfo.NumberOfLinks;
	if (metadata.attributes.kind == entry_kind::regular_file && (metadata.attributes.sparse || metadata.attributes.compressed))
	{
		FILE_COMPRESSION_INFO compressionInfo{};
		if (::GetFileInformationByHandleEx(nativeHandle, FileCompressionInfo, &compressionInfo, sizeof(compressionInfo)) == 0) [[unlikely]]
			return std::unexpected{capture_last_filesystem_error()};
		if (compressionInfo.CompressedFileSize.QuadPart < 0) [[unlikely]]
			return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_DATA }};
		metadata.allocated_size = static_cast<uint64_t>(compressionInfo.CompressedFileSize.QuadPart);
	}

	if (const auto identity = identityForHandle(nativeHandle))
	{
		metadata.mount_id = identity->filesystem;
		// A zero file index identifies nothing: the exFAT root reports it, as would every entry of a filesystem without file indexes.
		if (identity->entry != decltype(identity->entry){})
			metadata.identity = identity;
	}

	if (const auto closeError = handle.close()) [[unlikely]]
		return std::unexpected{*closeError};
	return metadata;
}

template <class Character>
[[nodiscard]] filesystem_result<void> setPermissionsForPath(const Character* path, const file_permissions permissions, const link_behavior linkBehavior) noexcept
{
	const auto opened = openForAttributes(path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, linkBehavior);
	if (!opened) [[unlikely]]
		return std::unexpected{opened.error()};
	file_handle handle{*opened};

	if (!setPermissions(*opened, permissions)) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};

	if (const auto closeError = handle.close()) [[unlikely]]
		return std::unexpected{*closeError};
	return {};
}

template <class Character>
[[nodiscard]] filesystem_result<void> createDirectory(const Character* path) noexcept
{
	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = nativePath.error_code() }};

	if (::CreateDirectoryW(nativePath.c_str(), nullptr) == 0)
		return std::unexpected{capture_last_filesystem_error()};
	return {};
}

template <class Character>
[[nodiscard]] filesystem_result<void> renameEntry(const Character* source, const Character* destination, const existing_destination ifDestinationExists) noexcept
{
	DWORD flags = 0;
	switch (ifDestinationExists)
	{
	case existing_destination::fail:
		break;
	case existing_destination::replace:
		flags = MOVEFILE_REPLACE_EXISTING;
		break;
	default:
		return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_PARAMETER }};
	}

	windows_path_buffer sourcePath{source};
	if (!sourcePath) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = sourcePath.error_code() }};
	windows_path_buffer destinationPath{destination};
	if (!destinationPath) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = destinationPath.error_code() }};

	// No MOVEFILE_COPY_ALLOWED: another volume fails with ERROR_NOT_SAME_DEVICE.
	if (::MoveFileExW(sourcePath.c_str(), destinationPath.c_str(), flags) == 0)
		return std::unexpected{capture_last_filesystem_error()};
	return {};
}

template <class Character>
[[nodiscard]] filesystem_result<void> removeEntry(const Character* path) noexcept
{
	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = nativePath.error_code() }};

	// FILE_FLAG_OPEN_REPARSE_POINT: the handle is the link itself, of either flavor, and deleting it leaves the target alone.
	const HANDLE nativeHandle = ::CreateFileW(nativePath.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (nativeHandle == INVALID_HANDLE_VALUE) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	file_handle handle{nativeHandle};

	// POSIX semantics: the name is gone at once, not when the last handle another process holds is closed.
	FILE_DISPOSITION_INFO_EX posixDisposition{ FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS };
	if (::SetFileInformationByHandle(nativeHandle, FileDispositionInfoEx, &posixDisposition, sizeof(posixDisposition)) == 0)
	{
		const filesystem_error error = capture_last_filesystem_error();
		// Filesystems other than NTFS do not implement the extended disposition.
		if (error.native_code != ERROR_INVALID_PARAMETER && error.native_code != ERROR_INVALID_FUNCTION && error.native_code != ERROR_NOT_SUPPORTED)
			return std::unexpected{error};

		FILE_DISPOSITION_INFO disposition{ TRUE };
		if (::SetFileInformationByHandle(nativeHandle, FileDispositionInfo, &disposition, sizeof(disposition)) == 0) [[unlikely]]
			return std::unexpected{capture_last_filesystem_error()};
	}

	if (const auto closeError = handle.close()) [[unlikely]]
		return std::unexpected{*closeError};
	return {};
}

template <class Character>
[[nodiscard]] filesystem_result<filesystem_space> getFilesystemSpace(const Character* directoryPath) noexcept
{
	windows_path_buffer nativePath{directoryPath};
	if (!nativePath) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = nativePath.error_code() }};

	const HANDLE nativeHandle = ::CreateFileW(nativePath.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (nativeHandle == INVALID_HANDLE_VALUE) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	file_handle handle{nativeHandle};

	if (isUncShareRoot(nativePath.c_str()) && !nativePath.append_directory_separator()) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = nativePath.error_code() }};
	ULARGE_INTEGER available{}, capacity{}, free{};
	if (::GetDiskFreeSpaceExW(nativePath.c_str(), &available, &capacity, &free) == 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};

	filesystem_space space{
		.capacity = capacity.QuadPart,
		.free = free.QuadPart,
		.available = available.QuadPart
	};
	if (const auto entryIdentity = identityForHandle(nativeHandle))
		space.identity = entryIdentity->filesystem;

	if (const auto closeError = handle.close()) [[unlikely]]
		return std::unexpected{*closeError};
	return space;
}

// Reads the entry's own attributes, which needs no list access to its parent.
// A reparse point's tag needs a handle, so a reparse point that cannot be opened fails.
[[nodiscard]] filesystem_result<entry_status> ownStatus(const wchar_t* const path) noexcept
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (::GetFileAttributesExW(path, GetFileExInfoStandard, &data) == 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};

	DWORD reparseTag = 0;
	if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		const HANDLE nativeHandle = ::CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (nativeHandle == INVALID_HANDLE_VALUE) [[unlikely]]
			return std::unexpected{capture_last_filesystem_error()};
		file_handle handle{nativeHandle};

		const auto queriedTag = reparseTagForHandle(nativeHandle, data.dwFileAttributes);
		if (!queriedTag) [[unlikely]]
			return std::unexpected{queriedTag.error()};
		reparseTag = *queriedTag;
	}

	return statusFromWindows(data.dwFileAttributes, reparseTag, data.nFileSizeHigh, data.nFileSizeLow, data.ftCreationTime, data.ftLastAccessTime, data.ftLastWriteTime);
}

[[nodiscard]] filesystem_result<directory_entry> entryFromAttributes(const wchar_t* const path, native_string name)
{
	auto status = ownStatus(path);
	if (!status) [[unlikely]]
		return std::unexpected{status.error()};

	directory_entry entry{ std::move(*status), std::move(name), std::nullopt };
	if (entry.attributes.is_link)
		entry.link_target = linkTargetStatus(path);
	return entry;
}

template <class Character>
[[nodiscard]] filesystem_result<entry_status> getEntryStatus(const Character* path, const link_behavior linkBehavior) noexcept
{
	if (linkBehavior != link_behavior::follow && linkBehavior != link_behavior::do_not_follow) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_PARAMETER }};

	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = nativePath.error_code() }};

	auto status = ownStatus(nativePath.c_str());
	if (status && status->attributes.is_link && linkBehavior == link_behavior::follow)
		return followedStatus(nativePath.c_str());
	return status;
}

template <class Character>
[[nodiscard]] filesystem_result<directory_entry> getDirectoryEntry(const Character* path)
{
	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = nativePath.error_code() }};

	// FindFirstFileExW cannot find a root, which has no name in its parent.
	if (isRootPath({ nativePath.c_str(), nativePath.length() }))
		return entryFromAttributes(nativePath.c_str(), {});

	nativePath.remove_trailing_separator();
	const std::wstring_view preparedPath{ nativePath.c_str(), nativePath.length() };
	const size_t lastSeparator = preparedPath.find_last_of(L'\\');
	const std::wstring_view name = lastSeparator == std::wstring_view::npos ? preparedPath : preparedPath.substr(lastSeparator + 1);
	// FindFirstFileExW matches the last component as a pattern, with these as its wildcards.
	if (name.find_first_of(LR"(*?<>")") != std::wstring_view::npos) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_NAME }};

	WIN32_FIND_DATAW data{};
	const HANDLE nativeHandle = ::FindFirstFileExW(nativePath.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
	if (nativeHandle == INVALID_HANDLE_VALUE) [[unlikely]]
	{
		const filesystem_error error = capture_last_filesystem_error();
		// The search needs list access to the parent
		if (error.native_code == ERROR_ACCESS_DENIED)
			return entryFromAttributes(nativePath.c_str(), native_string{ name });
		return std::unexpected{error};
	}

	find_handle handle{nativeHandle};
	if (const auto closeError = handle.close()) [[unlikely]]
		return std::unexpected{*closeError};

	directory_entry entry = entryFromFindData(data);
	if (entry.attributes.is_link)
		entry.link_target = linkTargetStatus(nativePath.c_str());
	return entry;
}

// What the symbolic-link and mount-point variants of REPARSE_DATA_BUFFER share; the struct lives in the DDK's ntifs.h,
// not the SDK. The names follow in a path buffer: directly for a mount point, after a ULONG of flags for a symbolic link.
struct reparse_link_header final {
	ULONG  ReparseTag;
	USHORT ReparseDataLength; // Counts the bytes from SubstituteNameOffset on
	USHORT Reserved;
	USHORT SubstituteNameOffset;
	USHORT SubstituteNameLength;
	USHORT PrintNameOffset;
	USHORT PrintNameLength;
};

constexpr size_t mountPointPathBufferOffset = sizeof(reparse_link_header);
constexpr size_t symbolicLinkPathBufferOffset = sizeof(reparse_link_header) + sizeof(ULONG);

constexpr ULONG symlinkFlagRelative = 0x1; // SYMLINK_FLAG_RELATIVE from ntifs.h
constexpr std::wstring_view ntPathPrefix = LR"(\??\)";
constexpr std::wstring_view ntUncPathPrefix = LR"(\??\UNC\)";

// Empty when the name lies outside the pathBufferBytes the query returned.
[[nodiscard]] std::optional<std::wstring_view> reparseName(const wchar_t* const pathBuffer, const size_t pathBufferBytes,
	const USHORT byteOffset, const USHORT byteLength) noexcept
{
	if (byteOffset % sizeof(wchar_t) != 0 || byteLength % sizeof(wchar_t) != 0 || size_t{byteOffset} + byteLength > pathBufferBytes) [[unlikely]]
		return {};
	return std::wstring_view{pathBuffer + byteOffset / sizeof(wchar_t), byteLength / sizeof(wchar_t)};
}

// The print name, or where the creating tool stored none, the substitute name without its NT namespace prefix.
[[nodiscard]] native_string userFormTarget(const std::wstring_view substituteName, const std::wstring_view printName, const bool relative)
{
	if (!printName.empty())
		return native_string{printName};
	if (relative)
		return native_string{substituteName};
	if (substituteName.starts_with(ntUncPathPrefix))
		return LR"(\\)" + native_string{substituteName.substr(ntUncPathPrefix.size())};
	if (substituteName.starts_with(ntPathPrefix))
		return native_string{substituteName.substr(ntPathPrefix.size())};
	return native_string{substituteName};
}

template <class Character>
[[nodiscard]] filesystem_result<link_definition> readLink(const Character* path)
{
	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = nativePath.error_code() }};
	// A trailing separator is ignored, as POSIX ignores it here
	if (!isRootPath({ nativePath.c_str(), nativePath.length() }))
		nativePath.remove_trailing_separator();

	const HANDLE nativeHandle = ::CreateFileW(nativePath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (nativeHandle == INVALID_HANDLE_VALUE) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	file_handle handle{nativeHandle};

	FILE_BASIC_INFO basicInfo{};
	if (::GetFileInformationByHandleEx(nativeHandle, FileBasicInfo, &basicInfo, sizeof(basicInfo)) == 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	if ((basicInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
		return std::unexpected{filesystem_error{ .native_code = ERROR_NOT_A_REPARSE_POINT }};

	alignas(reparse_link_header) std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> storage{};
	DWORD returnedBytes = 0;
	if (::DeviceIoControl(nativeHandle, FSCTL_GET_REPARSE_POINT, nullptr, 0, storage.data(), static_cast<DWORD>(storage.size()), &returnedBytes, nullptr) == 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	if (returnedBytes < sizeof(reparse_link_header)) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_DATA }};

	const auto& header = *reinterpret_cast<const reparse_link_header*>(storage.data());
	if (!IsReparseTagNameSurrogate(header.ReparseTag))
		return std::unexpected{filesystem_error{ .native_code = ERROR_NOT_A_REPARSE_POINT }};

	link_definition definition;
	size_t pathBufferOffset = mountPointPathBufferOffset;
	bool relative = false;
	switch (header.ReparseTag)
	{
	case IO_REPARSE_TAG_SYMLINK:
		pathBufferOffset = symbolicLinkPathBufferOffset;
		if (returnedBytes < pathBufferOffset) [[unlikely]]
			return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_DATA }};
		relative = (*reinterpret_cast<const ULONG*>(storage.data() + sizeof(reparse_link_header)) & symlinkFlagRelative) != 0;
		definition.type = (basicInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? link_type::directory_symbolic_link : link_type::symbolic_link;
		break;
	case IO_REPARSE_TAG_MOUNT_POINT:
		definition.type = link_type::junction;
		break;
	default:
		return std::unexpected{filesystem_error{ .native_code = ERROR_NOT_SUPPORTED }};
	}

	const auto* const pathBuffer = reinterpret_cast<const wchar_t*>(storage.data() + pathBufferOffset);
	const size_t pathBufferBytes = returnedBytes - pathBufferOffset;
	const auto substituteName = reparseName(pathBuffer, pathBufferBytes, header.SubstituteNameOffset, header.SubstituteNameLength);
	const auto printName = reparseName(pathBuffer, pathBufferBytes, header.PrintNameOffset, header.PrintNameLength);
	if (!substituteName || !printName) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_DATA }};
	// A volume mount point names a volume, which a junction cannot recreate
	if (definition.type == link_type::junction && substituteName->starts_with(LR"(\??\Volume{)"))
		return std::unexpected{filesystem_error{ .native_code = ERROR_NOT_SUPPORTED }};
	definition.target = userFormTarget(*substituteName, *printName, relative);

	if (const auto closeError = handle.close()) [[unlikely]]
		return std::unexpected{*closeError};
	return definition;
}

[[nodiscard]] bool isDriveAbsolutePath(const std::wstring_view path) noexcept
{
	return path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && path[2] == L'\\';
}

// A directory carrying a mount-point reparse point whose substitute name is the target in the NT namespace.
[[nodiscard]] filesystem_result<void> createJunction(const wchar_t* const preparedPath, const std::wstring_view target) noexcept
{
	// The substitute name is the target behind the NT prefix, in place of a \\?\ prefix it may already have
	std::wstring_view targetWithoutPrefix = target;
	if (targetWithoutPrefix.starts_with(LR"(\\?\)"))
		targetWithoutPrefix.remove_prefix(4);
	if (!isDriveAbsolutePath(targetWithoutPrefix)) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_PARAMETER }};

	const size_t substituteNameLength = ntPathPrefix.size() + targetWithoutPrefix.size();
	const size_t pathBufferChars = substituteNameLength + 1 + target.size() + 1; // Both names, each NUL-terminated
	const size_t inputBytes = mountPointPathBufferOffset + pathBufferChars * sizeof(wchar_t);
	if (inputBytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = ERROR_FILENAME_EXCED_RANGE }};

	alignas(reparse_link_header) std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> storage{};
	auto& header = *reinterpret_cast<reparse_link_header*>(storage.data());
	header.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
	header.ReparseDataLength = static_cast<USHORT>(inputBytes - offsetof(reparse_link_header, SubstituteNameOffset));
	header.SubstituteNameOffset = 0;
	header.SubstituteNameLength = static_cast<USHORT>(substituteNameLength * sizeof(wchar_t));
	header.PrintNameOffset = static_cast<USHORT>((substituteNameLength + 1) * sizeof(wchar_t));
	header.PrintNameLength = static_cast<USHORT>(target.size() * sizeof(wchar_t));
	auto* const pathBuffer = reinterpret_cast<wchar_t*>(storage.data() + mountPointPathBufferOffset);
	wchar_t* const substituteName = std::copy_n(ntPathPrefix.data(), ntPathPrefix.size(), pathBuffer);
	std::copy_n(targetWithoutPrefix.data(), targetWithoutPrefix.size(), substituteName);
	std::copy_n(target.data(), target.size(), pathBuffer + substituteNameLength + 1); // The zeroed storage supplies both NULs

	if (::CreateDirectoryW(preparedPath, nullptr) == 0)
		return std::unexpected{capture_last_filesystem_error()};

	const HANDLE nativeHandle = ::CreateFileW(preparedPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	std::optional<filesystem_error> failure;
	if (nativeHandle == INVALID_HANDLE_VALUE) [[unlikely]]
		failure = capture_last_filesystem_error();
	else
	{
		file_handle handle{nativeHandle};
		DWORD ignored = 0;
		if (::DeviceIoControl(nativeHandle, FSCTL_SET_REPARSE_POINT, storage.data(), static_cast<DWORD>(inputBytes), nullptr, 0, &ignored, nullptr) == 0)
			failure = capture_last_filesystem_error();
		else if (const auto closeError = handle.close()) [[unlikely]]
			failure = *closeError;
	}

	if (failure)
	{
		(void)::RemoveDirectoryW(preparedPath); // Created above; a junction is removed as itself, its target untouched
		return std::unexpected{*failure};
	}
	return {};
}

template <class Character>
[[nodiscard]] filesystem_result<void> createLink(const Character* path, const link_definition& definition) noexcept
{
	if (definition.target.empty() || definition.target.find(L'\0') != native_string::npos) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_PARAMETER }};

	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = nativePath.error_code() }};

	switch (definition.type)
	{
	case link_type::symbolic_link:
	case link_type::directory_symbolic_link:
	{
		DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
		if (definition.type == link_type::directory_symbolic_link)
			flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
		if (::CreateSymbolicLinkW(nativePath.c_str(), definition.target.c_str(), flags) == 0)
			return std::unexpected{capture_last_filesystem_error()};
		return {};
	}
	case link_type::junction:
		return createJunction(nativePath.c_str(), definition.target);
	default:
		return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_PARAMETER }};
	}
}

} // namespace

filesystem_result<void> can_list_directory(const char* path) noexcept
{
	return canListDirectory(path);
}

filesystem_result<void> can_list_directory(const wchar_t* path) noexcept
{
	return canListDirectory(path);
}

filesystem_result<std::vector<directory_entry>> list_directory(const char* path, const listing_detail detail)
{
	return listDirectory(path, detail);
}

filesystem_result<std::vector<directory_entry>> list_directory(const wchar_t* path, const listing_detail detail)
{
	return listDirectory(path, detail);
}

filesystem_result<directory_entry> get_directory_entry(const char* path)
{
	return getDirectoryEntry(path);
}

filesystem_result<directory_entry> get_directory_entry(const wchar_t* path)
{
	return getDirectoryEntry(path);
}

filesystem_result<entry_status> get_entry_status(const char* path, const link_behavior linkBehavior) noexcept
{
	return getEntryStatus(path, linkBehavior);
}

filesystem_result<entry_status> get_entry_status(const wchar_t* path, const link_behavior linkBehavior) noexcept
{
	return getEntryStatus(path, linkBehavior);
}

filesystem_result<entry_metadata> get_entry_metadata(const char* path, const link_behavior linkBehavior) noexcept
{
	return getEntryMetadata(path, linkBehavior);
}

filesystem_result<entry_metadata> get_entry_metadata(const wchar_t* path, const link_behavior linkBehavior) noexcept
{
	return getEntryMetadata(path, linkBehavior);
}

filesystem_result<void> set_permissions(const char* path, const file_permissions permissions, const link_behavior linkBehavior) noexcept
{
	return setPermissionsForPath(path, permissions, linkBehavior);
}

filesystem_result<void> set_permissions(const wchar_t* path, const file_permissions permissions, const link_behavior linkBehavior) noexcept
{
	return setPermissionsForPath(path, permissions, linkBehavior);
}

filesystem_result<void> create_directory(const char* path) noexcept
{
	return createDirectory(path);
}

filesystem_result<void> create_directory(const wchar_t* path) noexcept
{
	return createDirectory(path);
}

filesystem_result<void> rename_entry(const char* source, const char* destination, const existing_destination ifDestinationExists) noexcept
{
	return renameEntry(source, destination, ifDestinationExists);
}

filesystem_result<void> rename_entry(const wchar_t* source, const wchar_t* destination, const existing_destination ifDestinationExists) noexcept
{
	return renameEntry(source, destination, ifDestinationExists);
}

filesystem_result<void> remove_entry(const char* path) noexcept
{
	return removeEntry(path);
}

filesystem_result<void> remove_entry(const wchar_t* path) noexcept
{
	return removeEntry(path);
}

filesystem_result<link_definition> read_link(const char* path)
{
	return readLink(path);
}

filesystem_result<link_definition> read_link(const wchar_t* path)
{
	return readLink(path);
}

filesystem_result<void> create_link(const char* path, const link_definition& definition) noexcept
{
	return createLink(path, definition);
}

filesystem_result<void> create_link(const wchar_t* path, const link_definition& definition) noexcept
{
	return createLink(path, definition);
}

filesystem_result<filesystem_space> get_filesystem_space(const char* directoryPath) noexcept
{
	return getFilesystemSpace(directoryPath);
}

filesystem_result<filesystem_space> get_filesystem_space(const wchar_t* directoryPath) noexcept
{
	return getFilesystemSpace(directoryPath);
}

} // namespace thin_io
