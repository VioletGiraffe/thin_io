#include "fs.hpp"
#include "timestamp_win.hpp"
#include "windows_path_win.hpp"

#include <Windows.h>

#include <optional>
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
	if (!times.creation && !times.last_access && !times.last_write)
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

		entry_times times;
		times.creation = fromFileTime(info.ftCreationTime);
		times.last_access = fromFileTime(info.ftLastAccessTime);
		times.last_write = fromFileTime(info.ftLastWriteTime);
		return times;
	}

	entry_times times;
	times.creation = fromFileTime(attributes.ftCreationTime);
	times.last_access = fromFileTime(attributes.ftLastAccessTime);
	times.last_write = fromFileTime(attributes.ftLastWriteTime);
	return times;
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
	attributes.reparse_tag = attributes.is_link ? reparseTag : 0;
	return attributes;
}

[[nodiscard]] bool isDotEntry(const std::wstring_view name) noexcept
{
	return name == L"." || name == L"..";
}

[[nodiscard]] bool isUncShareRoot(const std::wstring_view path) noexcept
{
	static constexpr std::wstring_view extendedUncPrefix = LR"(\\?\UNC\)";
	if (!path.starts_with(extendedUncPrefix))
		return false;

	const size_t serverEnd = path.find(L'\\', extendedUncPrefix.size());
	return serverEnd != std::wstring_view::npos && serverEnd + 1 < path.size() && path.find(L'\\', serverEnd + 1) == std::wstring_view::npos;
}

void appendDirectoryEntry(std::vector<directory_entry>& entries, const WIN32_FIND_DATAW& data)
{
	if (isDotEntry(data.cFileName))
		return;

	directory_entry entry;
	entry.name = data.cFileName;
	entry.attributes = attributesFromWindows(data.dwFileAttributes, data.dwReserved0);
	if (entry.attributes.kind == entry_kind::regular_file && !entry.attributes.is_link)
		entry.logical_size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
	entries.push_back(std::move(entry));
}

template <class Character>
[[nodiscard]] filesystem_result<std::vector<directory_entry>> listDirectory(const Character* path)
{
	windows_path_buffer searchPath{path};
	if (!searchPath || !searchPath.append_directory_search_pattern()) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = searchPath.error_code() }};

	WIN32_FIND_DATAW data{};
	const HANDLE nativeHandle = ::FindFirstFileExW(searchPath.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
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
		appendDirectoryEntry(entries, data);
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

} // namespace

filesystem_result<std::vector<directory_entry>> list_directory(const char* path)
{
	return listDirectory(path);
}

filesystem_result<std::vector<directory_entry>> list_directory(const wchar_t* path)
{
	return listDirectory(path);
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
