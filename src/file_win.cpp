#include "file_win.hpp"
#include "enum_helpers.hpp"
#include "windows_path_win.hpp"

ENABLE_ENUM_ARITHMETIC(thin_io::file_constants::access_mode);
ENABLE_ENUM_ARITHMETIC(thin_io::file_constants::sharing_mode);

#include <assert.h>
#include <string.h> // memset
#include <Windows.h>
#include <winternl.h>

#include <limits>

using namespace thin_io;

static_assert(sizeof(file_impl) == sizeof(HANDLE) + sizeof(std::vector<int>)); // Empty base optimiation test

[[nodiscard]] inline constexpr DWORD accessMask(file_constants::access_mode mode)
{
	DWORD access = 0;
	if (mode & file_constants::access_mode::Read)
		access |= GENERIC_READ;
	if (mode & file_constants::access_mode::Write)
		access |= GENERIC_WRITE;

	return access;
}

[[nodiscard]] inline constexpr DWORD creationDisposition(file_constants::open_disposition disposition)
{
	switch (disposition)
	{
	case file_constants::open_disposition::OpenExisting:
		return OPEN_EXISTING;
	case file_constants::open_disposition::OpenOrCreate:
		return OPEN_ALWAYS;
	case file_constants::open_disposition::CreateNew:
		return CREATE_NEW;
	case file_constants::open_disposition::CreateOrTruncate:
		return CREATE_ALWAYS;
	}

	return OPEN_EXISTING;
}

[[nodiscard]] inline constexpr DWORD shareMask(file_constants::access_mode accessMode, file_constants::sharing_mode sharing)
{
	if (accessMode == file_constants::access_mode::Read)
		return sharing | file_constants::sharing_mode::ShareWrite; // Add permission to read files open for writing with SHARE_READ only
	else
		return static_cast<DWORD>(sharing); // Otherwise no change to permissions
}

[[nodiscard]] inline constexpr DWORD flags(file_constants::sys_cache_mode cacheMode)
{
	return cacheMode == file_constants::sys_cache_mode::CachingEnabled ? FILE_ATTRIBUTE_NORMAL : FILE_FLAG_NO_BUFFERING;
}

// ReadFile / WriteFile take a 32-bit byte count; an oversized request is clamped and completes as a partial transfer,
// which callers must handle anyway. The clamp is 64 KiB-aligned so aligned no-buffering I/O stays aligned.
[[nodiscard]] inline constexpr DWORD requestableIoSize(const uint64_t size) noexcept
{
	constexpr DWORD maxRequestSize = 0xFFFF'0000u;
	return size < maxRequestSize ? static_cast<DWORD>(size) : maxRequestSize;
}

bool file_impl::open(const char* path, const access_mode accessMode, const open_disposition disposition,
					 sys_cache_mode cacheMode, sharing_mode sharingMode) noexcept
{
	return open_path(path, accessMode, disposition, cacheMode, sharingMode);
}

bool file_impl::open(const wchar_t* path, const access_mode accessMode, const open_disposition disposition,
					 sys_cache_mode cacheMode, sharing_mode sharingMode) noexcept
{
	return open_path(path, accessMode, disposition, cacheMode, sharingMode);
}

template <class Character>
bool file_impl::open_path(const Character* path, const access_mode accessMode, const open_disposition disposition,
						  const sys_cache_mode cacheMode, const sharing_mode sharingMode) noexcept
{
	if (is_open() && !close())
		return false;

	if (accessMode == access_mode::Read && disposition == open_disposition::CreateOrTruncate) [[unlikely]]
	{
		::SetLastError(ERROR_INVALID_PARAMETER);
		return false;
	}

	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
	{
		::SetLastError(nativePath.error_code());
		return false;
	}

	return open_prepared_path(nativePath.c_str(), accessMode, disposition, cacheMode, sharingMode);
}

bool file_impl::open_prepared_path(const wchar_t* path, const access_mode accessMode, const open_disposition disposition,
								   const sys_cache_mode cacheMode, const sharing_mode sharingMode) noexcept
{
	static_assert(INVALID_HANDLE_VALUE == invalid_handle);

	const auto access = accessMask(accessMode);
	const auto sharing = shareMask(accessMode, sharingMode);
	const auto dispositionValue = creationDisposition(disposition);
	const auto flagsAndAttrs = flags(cacheMode);

	_h = ::CreateFileW(path,
					   access,
					   sharing,
					   nullptr, // Security attrs
					   dispositionValue,
					   flagsAndAttrs,
					   nullptr // Template handle
					);

	return is_open();
}

bool file_impl::close() noexcept
{
	// Unmap memory before closing the file
	for (const auto& mapping : _memoryMappings)
		do_unmap(mapping);
	_memoryMappings.clear();

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
	return ::ReadFile(_h, dest, requestableIoSize(size), &bytesRead, nullptr) ?
			bytesRead : std::optional<uint64_t>{};
}

std::optional<uint64_t> file_impl::write(const void *src, uint64_t size) noexcept
{
	DWORD bytesWritten = 0;
	return ::WriteFile(_h, src, requestableIoSize(size), &bytesWritten, nullptr) ?
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
	return ::ReadFile(_h, dest, requestableIoSize(size), &bytesRead, &o) ?
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
	return ::WriteFile(_h, src, requestableIoSize(size), &bytesWritten, &o) ?
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

bool file_impl::resize(const uint64_t newFileSize) noexcept
{
	if (newFileSize > static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max())) [[unlikely]]
	{
		::SetLastError(ERROR_FILE_TOO_LARGE);
		return false;
	}

	FILE_END_OF_FILE_INFO eof;
	eof.EndOfFile.QuadPart = static_cast<LONGLONG>(newFileSize);
	return ::SetFileInformationByHandle(_h, FileEndOfFileInfo, &eof, sizeof(eof)) != 0;
}

bool file_impl::preallocate(const uint64_t requestedSize) noexcept
{
	const auto currentSize = size();
	if (!currentSize) [[unlikely]]
		return false;

	if (requestedSize > *currentSize) [[unlikely]]
	{
		::SetLastError(ERROR_INVALID_PARAMETER);
		return false;
	}

	if (requestedSize == 0)
		return true;

	// FileAllocationInfo describes total allocation rather than a range. Allocating the current logical size may reserve
	// more than requested, but avoids shrinking EOF or pre-existing allocation below EOF.
	FILE_ALLOCATION_INFO allocation;
	allocation.AllocationSize.QuadPart = static_cast<LONGLONG>(*currentSize);
	return ::SetFileInformationByHandle(_h, FileAllocationInfo, &allocation, sizeof(allocation)) != 0;
}

bool file_impl::fsync() noexcept
{
	return ::FlushFileBuffers(_h) != 0;
}

bool file_impl::fdatasync() noexcept
{
#if !(defined(THIN_IO_WANT_FDATASYNC) && THIN_IO_WANT_FDATASYNC == 0)
	static constexpr NTSTATUS STATUS_SUCCESS = 0;

	using NtFlushBuffersFileEx_t = NTSTATUS(NTAPI*) (HANDLE, ULONG, PVOID, ULONG, PIO_STATUS_BLOCK);
	static NtFlushBuffersFileEx_t NtFlushBuffersFileEx = []() -> NtFlushBuffersFileEx_t {
		auto* lib = ::LoadLibraryA("ntdll.dll");
		if (!lib)
			return nullptr;
		auto* func = ::GetProcAddress(lib, "NtFlushBuffersFileEx");
		return (NtFlushBuffersFileEx_t)func;
	}();

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

void* file_impl::mmap(mmap_access_mode mode, uint64_t offset, uint64_t length) noexcept
{
	// Guard required: CreateFileMapping interprets INVALID_HANDLE_VALUE as a request for a pagefile-backed
	// anonymous mapping, so without it mapping a closed file would silently succeed.
	assert(is_open());
	if (!is_open()) [[unlikely]]
	{
		::SetLastError(ERROR_INVALID_HANDLE);
		return nullptr;
	}

	uint64_t actualOffset = offset;
	if (offset != 0)
	{
		// Query only once
		static const uint64_t pageSize = [] {
			SYSTEM_INFO info;
			::GetSystemInfo(&info);

			return info.dwPageSize;
		}();

		const auto nPages = offset / pageSize;
		actualOffset = nPages * pageSize; // Find the closest suitable lower offset
	}

	const uint64_t offsetDifference = offset - actualOffset;
	const uint64_t actualLength = length + offsetDifference;

	const DWORD protectFlag = mode == mmap_access_mode::ReadOnly ? PAGE_READONLY : PAGE_READWRITE;
	// The mapping object size is measured from the start of the file, so it must span through the end of the view.
	const uint64_t mappingSize = actualOffset + actualLength;
	HANDLE fileMappingHandle = ::CreateFileMappingA(
		_h,
		nullptr,
		protectFlag,
		static_cast<DWORD>(mappingSize >> 32),
		static_cast<DWORD>(mappingSize & 0xFFFFFFFFu),
		nullptr
	);

	if (fileMappingHandle == nullptr) [[unlikely]]
		return nullptr;

	const DWORD fileAccessFlag = mode == mmap_access_mode::ReadOnly ? FILE_MAP_READ : FILE_MAP_WRITE;

	void* addr = ::MapViewOfFile(
		fileMappingHandle,
		fileAccessFlag,
		static_cast<DWORD>(actualOffset >> 32),
		static_cast<DWORD>(actualOffset & 0xFF'FF'FF'FFull),
		actualLength
	);

	if (addr == nullptr) [[unlikely]]
	{
		::CloseHandle(fileMappingHandle);
		return nullptr;
	}

	auto* userAddress = reinterpret_cast<std::byte*>(addr) + offsetDifference;
	_memoryMappings.push_back(Mapping{.userAddr = userAddress, .addr = addr, .handle = fileMappingHandle});
	return userAddress;
}

bool file_impl::unmap(void* mapAddress) noexcept
{
	auto it = std::find_if(_memoryMappings.begin(), _memoryMappings.end(), [mapAddress](const Mapping& m) {
		return m.userAddr == mapAddress;
	});

	if (it == _memoryMappings.end()) [[unlikely]]
		return false;

	if (do_unmap(*it)) [[likely]]
	{
		_memoryMappings.erase(it);
		return true;
	}

	return false;
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

bool file_impl::delete_file(const char* filePath) noexcept
{
	return delete_path(filePath);
}

bool file_impl::delete_file(const wchar_t* filePath) noexcept
{
	return delete_path(filePath);
}

template <class Character>
bool file_impl::delete_path(const Character* filePath) noexcept
{
	windows_path_buffer nativePath{filePath};
	if (!nativePath) [[unlikely]]
	{
		::SetLastError(nativePath.error_code());
		return false;
	}

	return delete_prepared_path(nativePath.c_str());
}

bool file_impl::delete_prepared_path(const wchar_t* filePath) noexcept
{
	return ::DeleteFileW(filePath) != 0;
}

bool file_impl::do_unmap(const Mapping& mapping) noexcept
{
	const BOOL success = ::UnmapViewOfFile(mapping.addr) && ::CloseHandle(mapping.handle);
	return success != 0;
}
