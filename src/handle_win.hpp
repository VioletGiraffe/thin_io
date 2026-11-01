#pragma once

#include "filesystem_error.hpp"
#include "filesystem_types.hpp"

#include <Windows.h>

#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <utility>

namespace thin_io {

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

// FILE_ID_128, or the 64-bit file index on a filesystem without it, such as FAT and exFAT.
// The entry part can be all zeros: see getEntryMetadata() in fs_win.cpp.
[[nodiscard]] inline std::optional<entry_identity> identityForHandle(const HANDLE handle) noexcept
{
	entry_identity identity;

	FILE_ID_INFO fileIdInfo{};
	if (::GetFileInformationByHandleEx(handle, FileIdInfo, &fileIdInfo, sizeof(fileIdInfo)) != 0)
	{
		identity.filesystem = fileIdInfo.VolumeSerialNumber;
		for (size_t i = 0; i < identity.entry.size(); ++i)
			identity.entry[i] = fileIdInfo.FileId.Identifier[i];
		return identity;
	}

	BY_HANDLE_FILE_INFORMATION info;
	if (::GetFileInformationByHandle(handle, &info) == 0)
		return {};

	identity.filesystem = info.dwVolumeSerialNumber;
	const uint64_t fileIndex = (uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow;
	for (size_t i = 0; i < sizeof(fileIndex); ++i)
		identity.entry[i] = static_cast<uint8_t>(fileIndex >> (8 * i));
	return identity;
}

} // namespace thin_io
