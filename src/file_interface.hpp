#pragma once
#include <optional>
#include <stdint.h>
#include <string>

namespace thin_io {

struct file_constants {
	enum class open_mode {Read = 1, Write = 2, ReadWrite = 3};
	enum class sys_cache_mode {CachingEnabled = 0, NoOsCaching = 1};
	enum class sharing_mode {NoSharing = 0, ShareRead = 1, ShareWrite = 2, ShareDelete = 4, ShareExec = 8};
};

template <class Impl>
class file_interface final : public file_constants {
public:
	inline bool open(const char* path,
					 open_mode openMode,
					 sys_cache_mode cacheMode = sys_cache_mode::CachingEnabled,
					 sharing_mode sharingMode = sharing_mode::ShareRead
			) noexcept
	{
		return _impl.open(path, openMode, cacheMode, sharingMode);
	}

	inline static file_interface create(const char* path,
		open_mode openMode,
		sys_cache_mode cacheMode = sys_cache_mode::CachingEnabled,
		sharing_mode sharingMode = sharing_mode::ShareRead
	) noexcept
	{
		file_interface<Impl> f;
		f.open(path, openMode, cacheMode, sharingMode);
		return f;
	}

	[[nodiscard]] inline bool is_open() const noexcept {
		return _impl.is_open();
	}

	[[nodiscard]] inline operator bool() const noexcept {
		return is_open();
	}

	[[nodiscard]] inline bool close() noexcept {
		return _impl.close();
	}

	inline std::optional<uint64_t> read(void* dest, uint64_t size) noexcept {
		return _impl.read(dest, size);
	}

	inline std::optional<uint64_t> write(const void* dest, uint64_t size) noexcept {
		return _impl.write(dest, size);
	}

	// !!!
	// NOTE: Win32: the position of the file will be altered; Linux / POSIX: the position is NOT altered
	// !!!
	inline std::optional<uint64_t> pread(void* dest, uint64_t size, uint64_t pos) noexcept {
		return _impl.pread(dest, size, pos);
	}

	inline std::optional<uint64_t> pwrite(const void* dest, uint64_t size, uint64_t pos) noexcept {
		return _impl.pwrite(dest, size, pos);
	}

	[[nodiscard]] inline std::optional<uint64_t> pos() const noexcept {
		return _impl.pos();
	}

	// Sets the absolute file position. Do not use this call in new code, use pread / pwrite instead.
	inline bool set_pos(uint64_t newPos) noexcept {
		return _impl.set_pos(newPos);
	}

	// Can resize to a smaller or larger size
	// This function also sets file position to the end
	//
	// !!!
	// Does not change file pointer on Windows!
	// !!!
	inline bool truncate(uint64_t newFileSize) noexcept {
		return _impl.truncate(newFileSize);
	}

	[[nodiscard]] inline bool fsync() noexcept {
		return _impl.fsync();
	}

	[[nodiscard]] inline bool fdatasync() noexcept {
		return _impl.fdatasync();
	}

	// Negative value means an error querying the size
	[[nodiscard]] inline std::optional<uint64_t> size() const noexcept {
		return _impl.size();
	}

	[[nodiscard]] inline bool at_end() const noexcept {
		return _impl.at_end();
	}

	static bool delete_file(const char* filePath) noexcept {
		return Impl::delete_file(filePath);
	}

	// Beware, it's OS-specific!
	[[nodiscard]] static inline auto error_code() noexcept {
		return Impl::error_code();
	}

	// Beware, it's OS-specific!
	[[nodiscard]] static inline std::string text_for_error(decltype(Impl::error_code()) ec) noexcept {
		return Impl::text_for_error(ec);
	}

	// Beware, it's OS-specific!
	[[nodiscard]] static inline std::string text_for_last_error() noexcept {
		return Impl::text_for_error(error_code());
	}

private:
	Impl _impl;
};

} // namespace thin_io
