#pragma once

#include "filesystem_types.hpp"

#include <Windows.h>

namespace thin_io {

// FileBasicInfo queries require FILE_READ_ATTRIBUTES, which a write-only open lacks. Attribute-only access is exempt
// from sharing restrictions, so a temporary reopen is the universal fallback.
[[nodiscard]] inline bool queryBasicInfo(const HANDLE fileHandle, FILE_BASIC_INFO& info) noexcept
{
	if (::GetFileInformationByHandleEx(fileHandle, FileBasicInfo, &info, sizeof(info)) != 0)
		return true;

	const HANDLE attributeHandle = ::ReOpenFile(fileHandle, FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
	if (attributeHandle == INVALID_HANDLE_VALUE) [[unlikely]]
		return false;

	const bool success = ::GetFileInformationByHandleEx(attributeHandle, FileBasicInfo, &info, sizeof(info)) != 0;
	const DWORD error = success ? 0 : ::GetLastError(); // CloseHandle overwrites the thread's last error
	::CloseHandle(attributeHandle);
	if (!success) [[unlikely]]
		::SetLastError(error);
	return success;
}

// Sets the masked attribute bits to desiredValues, leaving the bits outside the mask untouched.
// Requires a handle with FILE_WRITE_ATTRIBUTES access, which GENERIC_WRITE includes.
[[nodiscard]] inline bool updateAttributeFlags(const HANDLE fileHandle, const DWORD mask, const DWORD desiredValues) noexcept
{
	FILE_BASIC_INFO info;
	if (!queryBasicInfo(fileHandle, info)) [[unlikely]]
		return false;

	const DWORD oldAttributes = info.FileAttributes;
	DWORD newAttributes = (oldAttributes & ~mask) | (desiredValues & mask);
	if (newAttributes == oldAttributes)
		return true;
	if (newAttributes == 0)
		newAttributes = FILE_ATTRIBUTE_NORMAL; // 0 would mean "leave the attributes unchanged"

	FILE_BASIC_INFO update{}; // Zeroed timestamps are left untouched by the API
	update.FileAttributes = newAttributes;
	return ::SetFileInformationByHandle(fileHandle, FileBasicInfo, &update, sizeof(update)) != 0;
}

[[nodiscard]] inline file_permissions permissionsFromAttributes(const DWORD attributes) noexcept
{
	return file_permissions{
		.read_only = (attributes & FILE_ATTRIBUTE_READONLY) != 0,
		.hidden = (attributes & FILE_ATTRIBUTE_HIDDEN) != 0,
		.system = (attributes & FILE_ATTRIBUTE_SYSTEM) != 0
	};
}

[[nodiscard]] inline bool setPermissions(const HANDLE fileHandle, const file_permissions permissions) noexcept
{
	static constexpr DWORD mask = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
	const DWORD desiredValues = (permissions.read_only ? FILE_ATTRIBUTE_READONLY : 0)
		| (permissions.hidden ? FILE_ATTRIBUTE_HIDDEN : 0)
		| (permissions.system ? FILE_ATTRIBUTE_SYSTEM : 0);
	return updateAttributeFlags(fileHandle, mask, desiredValues);
}

} // namespace thin_io
