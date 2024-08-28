#pragma once
#include "file_interface.hpp"


namespace thin_io {

class [[nodiscard]] file_impl final : public file_constants {
public:
	file_impl() noexcept = default;
	inline file_impl(file_impl&& other) noexcept;
	inline ~file_impl() noexcept;

	inline file_impl& operator=(file_impl&& other) noexcept;

	bool open(const char* path, open_mode openMode,
			  sys_cache_mode cacheMode,
			  sharing_mode sharingMode) noexcept;

	// Does not check if the handle was open, returns false if it wasn't
	bool close() noexcept;

	[[nodiscard]] inline bool is_open() const noexcept;

	std::optional<uint64_t> read(void* dest, uint64_t size) noexcept;
	std::optional<uint64_t> write(const void* src, uint64_t size) noexcept;

	// Note: the position of the file will be altered!
	std::optional<uint64_t> pread(void* dest, uint64_t size, uint64_t pos) noexcept;
	std::optional<uint64_t> pwrite(const void* src, uint64_t size, uint64_t pos) noexcept;

	[[nodiscard]] std::optional<uint64_t> pos() const noexcept;
	// Sets the absolute file position. Do not use this call in new code, use pread / pwrite instead.
	bool set_pos(uint64_t newPos) noexcept;

	// This function also sets file position to the end
	bool truncate(uint64_t newFileSize) noexcept;

	[[nodiscard]] bool fsync() noexcept;
	[[nodiscard]] bool fdatasync() noexcept;


	[[nodiscard]] std::optional<uint64_t> size() const noexcept;
	[[nodiscard]] bool at_end() const noexcept;

	static bool delete_file(const char* filePath) noexcept;

	[[nodiscard]] static int error_code() noexcept;
	[[nodiscard]] static std::string text_for_error(int ec) noexcept;

private:
	int _fd = -1;
};

inline file_impl::file_impl(file_impl &&other) noexcept : _fd{other._fd} {
	other._fd = -1;
}

inline file_impl::~file_impl() noexcept
{
	close();
}

inline file_impl& file_impl::operator=(file_impl&& other) noexcept
{
	close();
	_fd = other._fd;
	other._fd = -1;
	return *this;
}

inline bool file_impl::is_open() const noexcept
{
	return _fd != -1;
}

}
