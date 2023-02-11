#include "file_linux.hpp"

#include <errno.h>
#include <string.h> // strerror

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace thin_io;


bool file_impl::open(const char *path, open_mode openMode, sys_cache_mode cacheMode, sharing_mode /*sharingMode*/) noexcept
{
	int flags = O_LARGEFILE;
	switch (openMode) {
	case Read:
		flags |= O_RDONLY;
		break;
	case Write:
		flags |= (O_WRONLY | O_TRUNC | O_CREAT);
		break;
	default: // ReadWite
		flags |= (O_RDWR | O_CREAT);
		break;
	}

	if (cacheMode == NoOsCaching) [[unlikely]]
		flags |= O_DIRECT;


	// The mneaning if sharingMode mode flag is not the same as the Linux access mode. Ignoring sharing flags.
	int access = 0;
	if ((flags & O_CREAT) != 0) // The access parameter is ignored unless O_CREAT is specified
	{
		access |= (S_IRUSR | S_IRGRP | S_IROTH);
		access |= (S_IWUSR | S_IWGRP | S_IWOTH);
		access |= (S_IXUSR | S_IXGRP | S_IXOTH);
	}

	_fd = ::open(path, flags, access);
	return is_open();
}

bool file_impl::close() noexcept
{
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

bool file_impl::setPos(uint64_t newPos) noexcept
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
	return ::fsync(_fd) == 0;
}

bool file_impl::fdatasync() noexcept
{
	return ::fdatasync(_fd) == 0;
}

bool file_impl::atEnd() const noexcept
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

bool file_impl::deleteFile(const char *filePath) noexcept
{
	return ::unlink(filePath) == 0;
}
