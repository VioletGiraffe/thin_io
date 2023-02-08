#pragma once
#include "file_interface.hpp"

#include <stddef.h>
#include <stdint.h>

using HANDLE = void*;

namespace thin_io {

using error_code = uint32_t;

class file_impl : public file_definitions {
public:
	file_impl() noexcept = default;
	inline file_impl(file_impl&& other) noexcept;
	inline ~file_impl() noexcept;

	bool open(const char* path, open_mode openMode,
			  sys_cache_mode cacheMode,
			  file_definitions::sharing_mode sharingMode) noexcept;

	// Does not check if the handle was open, returns false if it wasn't
	bool close() noexcept;

	[[nodiscard]] inline bool is_open() const noexcept;

	uint64_t read(void* dest, uint64_t size) noexcept;
	uint64_t write(const void* src, uint64_t size) noexcept;

	int64_t pos() const noexcept;
	// Sets the absolute file position.
	// The argument is signed, but must be non-negative!
	bool setPos(int64_t newPos) noexcept;

	// This function also sets file position to the end
	bool truncate(int64_t newFileSize) noexcept;

	int64_t size() const noexcept;
	bool atEnd() const noexcept;

	static uint32_t error_code() noexcept;

	static bool deleteFile(const char* filePath) noexcept;

private:
	static constexpr auto invalid_handle = (HANDLE)(~size_t{0});
	HANDLE _h = invalid_handle;
};

inline file_impl::file_impl(file_impl &&other) noexcept : _h{other._h} {
	other._h = nullptr;
}

inline file_impl::~file_impl() noexcept
{
	close();
}

inline bool file_impl::is_open() const noexcept
{
	return _h != invalid_handle;
}

}
