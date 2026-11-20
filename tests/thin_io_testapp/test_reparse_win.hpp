#pragma once

// Windows reparse points the tests set up.

#include "fs.hpp"
#include "windows_path_win.hpp"

#include <Windows.h>
#include <winioctl.h>

// Empty on failure. A junction stores only an absolute target, which a test knows only at run time.
[[nodiscard]] inline thin_io::native_string absolutePath(const wchar_t* const relativePath)
{
	const DWORD requiredLength = ::GetFullPathNameW(relativePath, 0, nullptr, nullptr); // Including the terminating NUL
	if (requiredLength == 0)
		return {};
	thin_io::native_string path(requiredLength, L'\0');
	const DWORD length = ::GetFullPathNameW(relativePath, requiredLength, path.data(), nullptr);
	if (length + 1 != requiredLength)
		return {};
	path.resize(length);
	return path;
}

// Unlike a symbolic link, a junction needs no privilege, so a test using one always runs on CI.
[[nodiscard]] inline bool createJunction(const wchar_t* const linkPath, const wchar_t* const targetPath)
{
	return thin_io::create_link(linkPath, thin_io::link_definition{ .type = thin_io::link_type::junction, .target = absolutePath(targetPath) }).has_value();
}

// A non-Microsoft tag requires the GUID buffer layout. No filter owns it, so the entry opens only as the reparse point itself.
[[nodiscard]] inline bool setThirdPartyReparsePoint(const wchar_t* const path, const DWORD tag)
{
	thin_io::windows_path_buffer preparedPath{path};
	if (!preparedPath)
		return false;
	const HANDLE handle = ::CreateFileW(preparedPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return false;

	REPARSE_GUID_DATA_BUFFER reparse{};
	reparse.ReparseTag = tag;
	reparse.ReparseDataLength = 1;
	reparse.ReparseGuid = {0x6f1c3a52, 0x1d4e, 0x4b8a, {0x9c, 0x31, 0x2e, 0x7d, 0x5a, 0x40, 0x8b, 0x16}};
	DWORD ignored = 0;
	const bool set = ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &reparse, REPARSE_GUID_DATA_BUFFER_HEADER_SIZE + reparse.ReparseDataLength,
		nullptr, 0, &ignored, nullptr) != 0;
	::CloseHandle(handle);
	return set;
}
