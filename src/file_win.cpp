#include "file_win.hpp"

#include <string.h> // memcpy
#include <Windows.h>

using namespace thin_io;

static_assert(sizeof(file_impl) == sizeof(HANDLE)); // Empty base optimiation test

template <size_t N>
static inline void to_wide_unc_path(const char* str, WCHAR(&wCharArray)[N])
{
	const size_t path_length = ::strlen(str);
	size_t prefix_length = 0;
	if (path_length >= 2 && str[1] == ':') // Absolute path?
	{
		static constexpr WCHAR prefix[] = LR"(\\?\)";
		prefix_length = std::size(prefix) - 1 /* null */;
		::memcpy(wCharArray, prefix, prefix_length * sizeof(WCHAR));
	}

	const auto nChars = ::MultiByteToWideChar(CP_UTF8, 0, str, (int)path_length, wCharArray + prefix_length, (int)(N - prefix_length - 1));
	wCharArray[prefix_length + nChars] = 0;

	// Fix non-Windows slashes.
	// TODO: is memchr faster?
	for (size_t i = prefix_length; i < prefix_length + nChars; ++i)
	{
		if (wCharArray[i] == L'/')
			wCharArray[i] = L'\\';
	}
}

#if !(defined(THIN_IO_WANT_FDATASYNC) && THIN_IO_WANT_FDATASYNC == 0)
struct IO_STATUS_BLOCK {
	union {
		NTSTATUS Status;
		PVOID    Pointer;
	};
	ULONG_PTR Information;
};

using NtFlushBuffersFileEx_t = NTSTATUS(__stdcall*) (HANDLE, ULONG, PVOID, ULONG, IO_STATUS_BLOCK*);
static NtFlushBuffersFileEx_t NtFlushBuffersFileEx = []() -> NtFlushBuffersFileEx_t {
	auto* lib = ::LoadLibraryA("ntdll.dll");
	if (!lib)
		return nullptr;
	auto* func = ::GetProcAddress(lib, "NtFlushBuffersFileEx");
	return (NtFlushBuffersFileEx_t)func;
}();
#endif

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

[[nodiscard]] inline constexpr DWORD shareMask(file_definitions::open_mode openMode, file_definitions::sharing_mode sharing)
{
	if (openMode == file_definitions::Read)
		return sharing | file_definitions::ShareWrite; // Add permission to read files open for writing with SHARE_READ only
	else
		return sharing; // Otherwise no change to permissions
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

	WCHAR wPath[32768];
	to_wide_unc_path(path, wPath);

	const auto access = accessMask(openMode);
	const auto sharing = shareMask(openMode, sharingMode);
	const auto creationDisposition = creationMode(openMode);
	const auto flagsAndAttrs = flags(cacheMode);

	_h = ::CreateFileW(wPath,
					   access,
					   sharing,
					   nullptr, // Security attrs
					   creationDisposition,
					   flagsAndAttrs,
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

std::optional<uint64_t> file_impl::read(void *dest, uint64_t size) noexcept
{
	DWORD bytesRead = 0;
	return ::ReadFile(_h, dest, (DWORD)size, &bytesRead, nullptr) ?
			bytesRead : std::optional<uint64_t>{};
}

std::optional<uint64_t> file_impl::write(const void *src, uint64_t size) noexcept
{
	DWORD bytesWritten = 0;
	return ::WriteFile(_h, src, (DWORD)size, &bytesWritten, nullptr) ?
				bytesWritten : std::optional<uint64_t>{};
}

std::optional<uint64_t> file_impl::pread(void *dest, uint64_t size, uint64_t pos) noexcept
{
	DWORD bytesRead = 0;
	OVERLAPPED o;
	o.Internal = 0;
	o.InternalHigh = 0;
	o.hEvent = 0;

	o.OffsetHigh = static_cast<DWORD>(pos >> 32);
	o.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFu);
	return ::ReadFile(_h, dest, (DWORD)size, &bytesRead, &o) ?
			bytesRead : std::optional<uint64_t>{};
}

std::optional<uint64_t> file_impl::pwrite(const void *src, uint64_t size, uint64_t pos) noexcept
{
	DWORD bytesWritten = 0;
	OVERLAPPED o;
	o.Internal = 0;
	o.InternalHigh = 0;
	o.hEvent = 0;

	o.OffsetHigh = static_cast<DWORD>(pos >> 32);
	o.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFu);
	return ::WriteFile(_h, src, (DWORD)size, &bytesWritten, &o) ?
			bytesWritten : std::optional<uint64_t>{};
}

std::optional<uint64_t> file_impl::size() const noexcept
{
	LARGE_INTEGER li;
	return ::GetFileSizeEx(_h, &li) != FALSE ?
			static_cast<uint64_t>(li.QuadPart): std::optional<uint64_t>{};
}

std::optional<uint64_t> file_impl::pos() const noexcept
{
	LARGE_INTEGER offset = {0};
	LARGE_INTEGER pos;
	return ::SetFilePointerEx(_h, offset, &pos, FILE_CURRENT) != 0 ?
			static_cast<uint64_t>(pos.QuadPart) : std::optional<uint64_t>{};
}

bool file_impl::set_pos(uint64_t newPos) noexcept
{
	LARGE_INTEGER offset;
	offset.QuadPart = static_cast<LONGLONG>(newPos);
	return ::SetFilePointerEx(_h, offset, nullptr, FILE_BEGIN) != 0;
}

// This function also sets file position to the end
bool file_impl::truncate(uint64_t newFileSize) noexcept
{
	// TODO: a better way is SetFileInformationByHandle with FileEndOfFileInfo
	FILE_END_OF_FILE_INFO eof;
	eof.EndOfFile.QuadPart = static_cast<LONGLONG>(newFileSize);
	return ::SetFileInformationByHandle(_h, FileEndOfFileInfo, &eof, sizeof(eof)) != 0;
}

bool file_impl::fsync() noexcept
{
	return ::FlushFileBuffers(_h) != 0;
}

bool file_impl::fdatasync() noexcept
{
#if !(defined(THIN_IO_WANT_FDATASYNC) && THIN_IO_WANT_FDATASYNC == 0)
	static constexpr NTSTATUS STATUS_SUCCESS = 0;

	IO_STATUS_BLOCK iosb;
	::memset(&iosb, 0, sizeof(iosb));
	if (NtFlushBuffersFileEx && NtFlushBuffersFileEx(_h, FLUSH_FLAGS_FILE_DATA_SYNC_ONLY, nullptr, 0, &iosb) == STATUS_SUCCESS)
		return true;
	else
		return fsync(); // Could be unsupported target filesystem
#else
	return fsync();
#endif
}

bool file_impl::at_end() const noexcept
{
	return pos() == size();
}

uint32_t file_impl::error_code() noexcept
{
	return ::GetLastError();
}

std::string file_impl::text_for_error(uint32_t ec) noexcept
{
	std::string text(255, '\0');
	const auto nCharsWritten = ::FormatMessageA(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr,
		ec,
		MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
		text.data(),
		static_cast<DWORD>(text.size() - 1),
		nullptr);

	if (nCharsWritten > 0)
	{
		text.resize(nCharsWritten);
		return text;
	}
	else
		return std::string{ "Failed to format error code with FormatMessageA!" };
}

bool file_impl::delete_file(const char *filePath) noexcept
{
	WCHAR wPath[32768];
	to_wide_unc_path(filePath, wPath);

	return ::DeleteFileW(wPath) != 0;
}
