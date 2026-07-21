#pragma once
#include "file_interface.hpp"

#include <stddef.h>
#include <utility>
#include <vector>

using HANDLE = void*;

namespace thin_io {

using error_code = uint32_t;

class [[nodiscard]] file_impl final : public file_constants {
public:
	file_impl() noexcept = default;
	inline file_impl(file_impl&& other) noexcept;
	inline ~file_impl() noexcept;

	inline file_impl& operator=(file_impl&& other) noexcept;

	bool open(const char* path, access_mode accessMode,
			  open_disposition disposition,
			  sys_cache_mode cacheMode,
			  sharing_mode sharingMode) noexcept;
	bool open(const wchar_t* path, access_mode accessMode,
			  open_disposition disposition,
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

	[[nodiscard]] bool resize(uint64_t newFileSize) noexcept;
	[[nodiscard]] bool preallocate(uint64_t size) noexcept;

	[[nodiscard]] bool fsync() noexcept;
	[[nodiscard]] bool fdatasync() noexcept;

	[[nodiscard]] void* mmap(mmap_access_mode mode, uint64_t offset, uint64_t length) noexcept;
	[[nodiscard]] bool unmap(void* mapAddress) noexcept;


	[[nodiscard]] std::optional<uint64_t> size() const noexcept;

	static bool delete_file(const char* filePath) noexcept;
	static bool delete_file(const wchar_t* filePath) noexcept;

	[[nodiscard]] static uint32_t error_code() noexcept;
	[[nodiscard]] static std::string text_for_error(uint32_t ec) noexcept;

private:
	struct Mapping {
		void* userAddr = nullptr;
		void* addr = nullptr;
		HANDLE handle = nullptr;
	};

	bool do_unmap(const Mapping& mapping) noexcept;
	template <class Character>
	bool open_path(const Character* path, access_mode accessMode, open_disposition disposition,
				   sys_cache_mode cacheMode, sharing_mode sharingMode) noexcept;
	bool open_prepared_path(const wchar_t* path, access_mode accessMode, open_disposition disposition,
							sys_cache_mode cacheMode, sharing_mode sharingMode) noexcept;
	template <class Character>
	static bool delete_path(const Character* filePath) noexcept;
	static bool delete_prepared_path(const wchar_t* filePath) noexcept;

private:
	static constexpr auto invalid_handle = (HANDLE)(~size_t{0});
	std::vector<Mapping> _memoryMappings;

	HANDLE _h = invalid_handle;
};

inline file_impl::file_impl(file_impl &&other) noexcept
	: _memoryMappings{std::move(other._memoryMappings)}, _h{other._h}
{
	other._h = invalid_handle;
}

inline file_impl::~file_impl() noexcept
{
	close();
}

inline file_impl& file_impl::operator=(file_impl&& other) noexcept
{
	close();
	_memoryMappings = std::move(other._memoryMappings);
	_h = other._h;
	other._h = invalid_handle;
	return *this;
}

inline bool file_impl::is_open() const noexcept
{
	return _h != invalid_handle;
}

}
