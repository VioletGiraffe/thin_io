#include "file_linux.hpp"

#include <errno.h>
#include <string.h> // strerror

#include <fcntl.h>
 #include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#ifdef __APPLE__
#define O_LARGEFILE 0 // Not needed
#define pread64 pread
#define pwrite64 pwrite
#define ftruncate64 ftruncate
#define lseek64 lseek
#define fstat64 fstat
#define stat64 stat
using off64_t = off_t;
#endif

using namespace thin_io;


bool file_impl::open(const char *path, open_mode openMode, sys_cache_mode cacheMode, sharing_mode /*sharingMode*/) noexcept
{
	int flags = 0;
	switch (openMode) {
	case open_mode::Read:
		flags |= O_RDONLY;
		break;
	case open_mode::Write:
		flags |= (O_WRONLY | O_TRUNC | O_CREAT);
		break;
	default: // ReadWite
		flags |= (O_RDWR | O_CREAT);
		break;
	}

#ifdef __linux__
	if (cacheMode == sys_cache_mode::NoOsCaching) [[unlikely]]
		flags |= O_DIRECT;
#endif

	// The mneaning if sharingMode mode flag is not the same as the Linux access mode. Ignoring sharing flags.
	int access = O_LARGEFILE;
	if ((flags & O_CREAT) != 0) // The access parameter is ignored unless O_CREAT is specified
	{
		access |= (S_IRUSR | S_IRGRP | S_IROTH);
		access |= (S_IWUSR | S_IWGRP | S_IWOTH);
		access |= (S_IXUSR | S_IXGRP | S_IXOTH);
	}

	_fd = ::open(path, flags, access);

#ifdef __APPLE__
	if (cacheMode == sys_cache_mode::NoOsCaching && is_open()) [[unlikely]]
	{
		fcntl(_fd, F_NOCACHE, 1);
		fcntl(_fd, F_RDAHEAD, 0);
	}
#endif

	return is_open();
}

bool file_impl::close() noexcept
{
	for (const auto& mapping: _memoryMappings)
		::munmap(mapping.addr, mapping.length);

	if (is_open() && ::close(_fd) == 0)
	{
		_fd = -1;
		return true;
	}
	return false;
}

std::optional<uint64_t> file_impl::read(void *dest, uint64_t size) noexcept
{
	ssize_t bytesRead = ::read(_fd, dest, size);
	return bytesRead >= 0 ? static_cast<uint64_t>(bytesRead) : std::optional<uint64_t>{};
}

std::optional<uint64_t> file_impl::write(const void *src, uint64_t size) noexcept
{
	const ssize_t bytesWritten = ::write(_fd, src, size);
	return bytesWritten >= 0 ? static_cast<uint64_t>(bytesWritten) : std::optional<uint64_t>{};
}

std::optional<uint64_t> file_impl::pread(void *dest, uint64_t size, uint64_t pos) noexcept
{
	const ssize_t bytesRead = ::pread64(_fd, dest, size, static_cast<off64_t>(pos));
	return bytesRead >= 0 ? static_cast<uint64_t>(bytesRead) : std::optional<uint64_t>{};
}

std::optional<uint64_t> file_impl::pwrite(const void *src, uint64_t size, uint64_t pos) noexcept
{
	const ssize_t bytesWritten = ::pwrite64(_fd, src, size, static_cast<off64_t>(pos));
	return bytesWritten >= 0 ? static_cast<uint64_t>(bytesWritten) : std::optional<uint64_t>{};
}

std::optional<uint64_t> file_impl::size() const noexcept
{
	struct stat64 s;
	if (::fstat64(_fd, &s) == 0)
		return static_cast<uint64_t>(s.st_size);
	return {};
}

std::optional<uint64_t> file_impl::pos() const noexcept
{
	const off64_t pos = ::lseek64(_fd, 0, SEEK_CUR);
	return pos >= 0 ? static_cast<uint64_t>(pos) : std::optional<uint64_t>{};
}

bool file_impl::set_pos(uint64_t newPos) noexcept
{
	const off64_t pos = ::lseek64(_fd, static_cast<off64_t>(newPos), SEEK_SET);
	return pos == static_cast<off64_t>(newPos);
}

// This function also sets file position to the end
bool file_impl::truncate(uint64_t newFileSize) noexcept
{
	return ::ftruncate64(_fd, static_cast<off64_t>(newFileSize)) == 0;
}

bool file_impl::fsync() noexcept
{
#ifndef __APPLE__
	return ::fsync(_fd) == 0;
#else
	return ::fcntl(_fd, F_FULLFSYNC, 0) != -1;
#endif
}

bool file_impl::fdatasync() noexcept
{
#ifndef __APPLE__
	return ::fdatasync(_fd) == 0;
#else
	return fsync();
#endif
}

void* file_impl::mmap(mmap_access_mode mode, const uint64_t offset, const uint64_t length) noexcept
{
	// Offset must be a multiple of page size!
	auto actualOffset = offset;
	if (offset != 0) [[unlikely]]
	{
		// Query only once
		static const uint64_t pageSize = (uint64_t)::sysconf(_SC_PAGE_SIZE);
		const auto nPages = offset / pageSize;
		actualOffset = nPages * pageSize; // Find the closest suitable lower offset
	}

	const auto offsetDiff = offset - actualOffset;
	const int protectFlag = mode == mmap_access_mode::ReadOnly ? PROT_READ : (PROT_READ | PROT_WRITE);
	void* addr = ::mmap(nullptr, length + offsetDiff, protectFlag, MAP_SHARED, _fd, static_cast<__off_t>(actualOffset));
	if (addr == MAP_FAILED) [[unlikely]]
		return nullptr;

	auto* userAddress = reinterpret_cast<std::byte*>(addr) + offsetDiff;
	_memoryMappings.push_back(Mapping{.addr = addr, .userAddr = userAddress, .length = length + offsetDiff});

	return userAddress;
}

bool file_impl::unmap(void* mapAddress) noexcept
{
	auto it = std::find_if(_memoryMappings.begin(), _memoryMappings.end(), [mapAddress](const Mapping& m) {
		return m.userAddr == mapAddress;
	});

	if (it == _memoryMappings.end()) [[unlikely]]
		return false;

	if (::munmap(it->addr, it->length) == 0) [[likely]]
	{
		_memoryMappings.erase(it);
		return true;
	}

	return false;
}

bool file_impl::at_end() const noexcept
{
	return pos() == size();
}

int file_impl::error_code() noexcept
{
	return errno;
}

std::string file_impl::text_for_error(int ec) noexcept
{
	return ::strerror(ec);
}

bool file_impl::delete_file(const char *filePath) noexcept
{
	return ::unlink(filePath) == 0;
}
