#include "file_win.hpp"

#include <Windows.h>

using namespace thin_io;

static_assert(sizeof(file_impl) == sizeof(HANDLE)); // Empty base optimiation test

[[nodiscard]] inline constexpr DWORD accessMask(file_definitions::open_mode mode)
{
	DWORD access = 0;
	if (mode & file_definitions::Read)
		access |= GENERIC_READ;
	if (mode & file_definitions::Write)
		access |= GENERIC_WRITE;

	return access;
}

[[nodiscard]] inline constexpr DWORD creationMode(file_definitions::open_mode mode)
{
	switch (mode)
	{
	case file_definitions::Read:
		return OPEN_EXISTING;
	case file_definitions::Write:
		return CREATE_ALWAYS;
	default:
		return OPEN_ALWAYS;
	}
}

[[nodiscard]] inline constexpr DWORD shareMask(file_definitions::sharing_mode mode)
{
	return mode;
}

[[nodiscard]] inline constexpr DWORD flags(file_definitions::sys_cache_mode cacheMode)
{
	return cacheMode == file_definitions::CachingEnabled ? FILE_ATTRIBUTE_NORMAL : FILE_FLAG_NO_BUFFERING;
}

bool file_impl::open(const char *path, open_mode openMode, sys_cache_mode cacheMode, sharing_mode sharingMode) noexcept
{
	static_assert(INVALID_HANDLE_VALUE == invalid_handle);

	if (is_open() && !close())
		return false;

	_h = ::CreateFileA(path,
					   accessMask(openMode),
					   shareMask(sharingMode),
					   nullptr, // Security attrs
					   creationMode(openMode),
					   flags(cacheMode),
					   nullptr // Template handle
					   );

	return is_open();
}

bool file_impl::close() noexcept
{
	if (is_open() && ::CloseHandle(_h) != 0)
	{
		_h = invalid_handle;
		return true;
	}
	else
		return false;
}

uint64_t file_impl::read(void *dest, uint64_t size) noexcept
{
	DWORD bytesRead = 0;
	::ReadFile(_h, dest, (DWORD)size, &bytesRead, nullptr);
	return bytesRead;
}

uint64_t file_impl::write(const void *src, uint64_t size) noexcept
{
	DWORD bytesWritten = 0;
	::WriteFile(_h, src, (DWORD)size, &bytesWritten, nullptr);
	return bytesWritten;
}

int64_t file_impl::size() const noexcept
{
	LARGE_INTEGER li;
	if (::GetFileSizeEx(_h, &li) != FALSE) [[likely]]
		return li.QuadPart;

	return 0;
}

int64_t file_impl::pos() const noexcept
{
	LARGE_INTEGER offset = {0};
	LARGE_INTEGER pos = {0};
	if (::SetFilePointerEx(_h, offset, &pos, FILE_CURRENT) != 0) [[likely]]
		return pos.QuadPart;

	return 0;
}

bool file_impl::setPos(int64_t newPos) noexcept
{
	LARGE_INTEGER offset;
	offset.QuadPart = newPos;
	LARGE_INTEGER pos;
	if (::SetFilePointerEx(_h, offset, &pos, FILE_BEGIN) != 0) [[likely]]
		return true;

	return false;
}

// This function also sets file position to the end
bool file_impl::truncate(int64_t newFileSize) noexcept
{
	if (!setPos(newFileSize))
		return false;
	return ::SetEndOfFile(_h) != 0;
}

bool file_impl::atEnd() const noexcept
{
	return pos() == size();
}

uint32_t file_impl::error_code() noexcept
{
	return ::GetLastError();
}

bool file_impl::deleteFile(const char *filePath) noexcept
{
	return ::DeleteFileA(filePath) != 0;
}
