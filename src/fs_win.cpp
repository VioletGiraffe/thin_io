#include "fs.hpp"
#include "timestamp_win.hpp"
#include "windows_path_win.hpp"

#include "utility/heap_optional.hpp" // cpp-template-utils

#include <Windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace thin_io {

[[nodiscard]] static bool setPreparedTimes(const wchar_t* path, const entry_times& times) noexcept
{
	// FILE_FLAG_BACKUP_SEMANTICS is what makes a handle to a directory possible; it is a no-op for regular files.
	const HANDLE h = ::CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
								  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) [[unlikely]]
		return false;

	const bool success = setFileTimes(h, times);
	const DWORD error = success ? 0 : ::GetLastError(); // CloseHandle overwrites the thread's last error
	::CloseHandle(h);
	if (!success) [[unlikely]]
		::SetLastError(error);

	return success;
}

template <class Character>
[[nodiscard]] static bool setTimesForPath(const Character* path, const entry_times& times) noexcept
{
	if (!times.creation.is_set() && !times.last_access.is_set() && !times.last_write.is_set())
		return true; // Nothing to write, so don't even open the path - matching the POSIX implementation

	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
	{
		::SetLastError(nativePath.error_code());
		return false;
	}

	return setPreparedTimes(nativePath.c_str(), times);
}

bool set_times(const char* path, const entry_times& times) noexcept
{
	return setTimesForPath(path, times);
}

bool set_times(const wchar_t* path, const entry_times& times) noexcept
{
	return setTimesForPath(path, times);
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

class file_handle final {
public:
	explicit file_handle(const HANDLE handle) noexcept : _handle{handle} {}
	~file_handle() noexcept
	{
		if (_handle != INVALID_HANDLE_VALUE)
			::CloseHandle(_handle);
	}

	file_handle(const file_handle&) = delete;
	file_handle& operator=(const file_handle&) = delete;

	[[nodiscard]] std::optional<filesystem_error> close() noexcept
	{
		if (::CloseHandle(std::exchange(_handle, INVALID_HANDLE_VALUE)) != 0)
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
	attributes.is_link = (nativeAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	attributes.sparse = (nativeAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
	attributes.compressed = (nativeAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0;
	attributes.hidden = (nativeAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
	attributes.reparse_tag = attributes.is_link ? reparseTag : 0;
	return attributes;
}

[[nodiscard]] bool isNameSurrogateLink(const entry_attributes& attributes) noexcept
{
	return attributes.is_link && IsReparseTagNameSurrogate(attributes.reparse_tag);
}

// sizeHigh and sizeLow are ignored for directories and links: a link's own size is not its target's.
// Other reparse points, such as WOF-compressed or cloud placeholder files, report their own size.
[[nodiscard]] entry_status statusFromWindows(const DWORD nativeAttributes, const DWORD reparseTag, const DWORD sizeHigh, const DWORD sizeLow,
	const FILETIME& creation, const FILETIME& lastAccess, const FILETIME& lastWrite) noexcept
{
	entry_status status;
	status.attributes = attributesFromWindows(nativeAttributes, reparseTag);
	if (status.attributes.kind == entry_kind::regular_file && !isNameSurrogateLink(status.attributes))
		status.logical_size = (static_cast<uint64_t>(sizeHigh) << 32) | sizeLow;
	status.times = timesFromWindows(creation, lastAccess, lastWrite);
	status.permissions = file_permissions{
		.read_only = (nativeAttributes & FILE_ATTRIBUTE_READONLY) != 0,
		.hidden = status.attributes.hidden,
		.system = (nativeAttributes & FILE_ATTRIBUTE_SYSTEM) != 0
	};
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

// Follows the link at path. Absent when the target cannot be reached.
[[nodiscard]] heap_optional<entry_status> linkTargetStatus(const wchar_t* const path)
{
	const HANDLE nativeHandle = ::CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (nativeHandle == INVALID_HANDLE_VALUE)
		return {};
	file_handle handle{nativeHandle};

	BY_HANDLE_FILE_INFORMATION info;
	if (::GetFileInformationByHandle(nativeHandle, &info) == 0) [[unlikely]]
		return {};

	// A target can itself be a reparse point, such as a cloud placeholder: its tag needs a separate query.
	FILE_ATTRIBUTE_TAG_INFO tagInfo{};
	if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
		&& ::GetFileInformationByHandleEx(nativeHandle, FileAttributeTagInfo, &tagInfo, sizeof(tagInfo)) == 0) [[unlikely]]
		return {};

	return statusFromWindows(info.dwFileAttributes, tagInfo.ReparseTag, info.nFileSizeHigh, info.nFileSizeLow,
		info.ftCreationTime, info.ftLastAccessTime, info.ftLastWriteTime);
}

[[nodiscard]] bool isDotEntry(const std::wstring_view name) noexcept
{
	return name == L"." || name == L"..";
}

// With or without one trailing separator.
[[nodiscard]] bool isUncShareRoot(std::wstring_view path) noexcept
{
	static constexpr std::wstring_view extendedUncPrefix = LR"(\\?\UNC\)";
	if (!path.starts_with(extendedUncPrefix))
		return false;

	if (path.ends_with(L'\\'))
		path.remove_suffix(1);
	const size_t serverEnd = path.find(L'\\', extendedUncPrefix.size());
	return serverEnd != std::wstring_view::npos && serverEnd + 1 < path.size() && path.find(L'\\', serverEnd + 1) == std::wstring_view::npos;
}

// A prepared path naming a drive root, the current drive's root, or a UNC share root.
[[nodiscard]] bool isRootPath(const std::wstring_view path) noexcept
{
	static constexpr std::wstring_view extendedPrefix = LR"(\\?\)";
	const std::wstring_view drivePath = path.starts_with(extendedPrefix) ? path.substr(extendedPrefix.size()) : path;
	if (drivePath == L"\\" || (drivePath.size() == 3 && drivePath[1] == L':' && drivePath[2] == L'\\'))
		return true;

	return isUncShareRoot(path);
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

		// FindFirstFileExW also uses ERROR_FILE_NOT_FOUND when the wildcard matched nothing. Verify the directory after
		// the failed search so a path removed before enumeration cannot be mistaken for an empty directory.
		windows_path_buffer directoryPath{path};
		if (!directoryPath) [[unlikely]]
			return std::unexpected{filesystem_error{ .native_code = directoryPath.error_code() }};
		const DWORD attributes = ::GetFileAttributesW(directoryPath.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) [[unlikely]]
			return std::unexpected{capture_last_filesystem_error()};
		if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) [[unlikely]]
			return std::unexpected{filesystem_error{ .native_code = ERROR_DIRECTORY }};
		return std::vector<directory_entry>{};
	}

	find_handle handle{nativeHandle};
	std::vector<directory_entry> entries;
	for (;;)
	{
		if (!isDotEntry(data.cFileName))
		{
			directory_entry entry = entryFromFindData(data);
			if (detail == listing_detail::full && isNameSurrogateLink(entry.attributes))
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

[[nodiscard]] std::optional<entry_identity> identityForHandle(const HANDLE handle) noexcept
{
	FILE_ID_INFO fileIdInfo{};
	if (::GetFileInformationByHandleEx(handle, FileIdInfo, &fileIdInfo, sizeof(fileIdInfo)) == 0)
		return {};

	entry_identity identity;
	identity.filesystem = fileIdInfo.VolumeSerialNumber;
	for (size_t i = 0; i < identity.entry.size(); ++i)
		identity.entry[i] = fileIdInfo.FileId.Identifier[i];
	return identity;
}

template <class Character>
[[nodiscard]] filesystem_result<entry_metadata> getEntryMetadata(const Character* path, const link_behavior linkBehavior) noexcept
{
	DWORD openFlags = FILE_FLAG_BACKUP_SEMANTICS;
	switch (linkBehavior)
	{
	case link_behavior::follow:
		break;
	case link_behavior::do_not_follow:
		openFlags |= FILE_FLAG_OPEN_REPARSE_POINT;
		break;
	default:
		return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_PARAMETER }};
	}

	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = nativePath.error_code() }};

	const HANDLE nativeHandle = ::CreateFileW(nativePath.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, openFlags, nullptr);
	if (nativeHandle == INVALID_HANDLE_VALUE) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	file_handle handle{nativeHandle};

	FILE_STANDARD_INFO standardInfo{};
	if (::GetFileInformationByHandleEx(nativeHandle, FileStandardInfo, &standardInfo, sizeof(standardInfo)) == 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	if (standardInfo.EndOfFile.QuadPart < 0 || standardInfo.AllocationSize.QuadPart < 0) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = ERROR_INVALID_DATA }};

	FILE_ATTRIBUTE_TAG_INFO attributeInfo{};
	if (::GetFileInformationByHandleEx(nativeHandle, FileAttributeTagInfo, &attributeInfo, sizeof(attributeInfo)) == 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};

	entry_metadata metadata;
	metadata.attributes = attributesFromWindows(attributeInfo.FileAttributes, attributeInfo.ReparseTag);
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

	metadata.identity = identityForHandle(nativeHandle);
	if (metadata.identity)
		metadata.mount_id = metadata.identity->filesystem;

	if (const auto closeError = handle.close()) [[unlikely]]
		return std::unexpected{*closeError};
	return metadata;
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
[[nodiscard]] filesystem_result<directory_entry> entryFromAttributes(const wchar_t* const path, native_string name)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (::GetFileAttributesExW(path, GetFileExInfoStandard, &data) == 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};

	FILE_ATTRIBUTE_TAG_INFO tagInfo{};
	if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		const HANDLE nativeHandle = ::CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (nativeHandle == INVALID_HANDLE_VALUE) [[unlikely]]
			return std::unexpected{capture_last_filesystem_error()};
		file_handle handle{nativeHandle};

		if (::GetFileInformationByHandleEx(nativeHandle, FileAttributeTagInfo, &tagInfo, sizeof(tagInfo)) == 0) [[unlikely]]
			return std::unexpected{capture_last_filesystem_error()};
	}

	directory_entry entry{
		statusFromWindows(data.dwFileAttributes, tagInfo.ReparseTag, data.nFileSizeHigh, data.nFileSizeLow, data.ftCreationTime, data.ftLastAccessTime, data.ftLastWriteTime),
		std::move(name),
		std::nullopt
	};
	if (isNameSurrogateLink(entry.attributes))
		entry.link_target = linkTargetStatus(path);
	return entry;
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
	if (isNameSurrogateLink(entry.attributes))
		entry.link_target = linkTargetStatus(nativePath.c_str());
	return entry;
}

} // namespace

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

filesystem_result<entry_metadata> get_entry_metadata(const char* path, const link_behavior linkBehavior) noexcept
{
	return getEntryMetadata(path, linkBehavior);
}

filesystem_result<entry_metadata> get_entry_metadata(const wchar_t* path, const link_behavior linkBehavior) noexcept
{
	return getEntryMetadata(path, linkBehavior);
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
