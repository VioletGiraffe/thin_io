#pragma once
#include <stdint.h>

namespace thin_io {

struct file_definitions {
	enum open_mode {Read = 1, Write = 2, ReadWrite = 3};
	enum sys_cache_mode {CachingEnabled = 0, NoOsCaching = 1};
	enum sharing_mode {NoSharing = 0, ShareRead = 1, ShareWrite = 2, ShareDelete = 4};
};

template <class Impl>
class file_interface : public file_definitions {
public:
	inline bool open(const char* path,
					 file_definitions::open_mode openMode,
					 file_definitions::sys_cache_mode cacheMode = file_definitions::CachingEnabled,
					 file_definitions::sharing_mode sharingMode = file_definitions::ShareRead
			) noexcept {
		return _impl.open(path, openMode, cacheMode, sharingMode);
	}

	inline bool is_open() const noexcept {
		return _impl.is_open();
	}

	inline bool close() noexcept {
		return _impl.close();
	}

	inline uint64_t read(void* dest, uint64_t size) noexcept {
		return _impl.read(dest, size);
	}

	inline uint64_t write(const void* dest, uint64_t size) noexcept {
		return _impl.write(dest, size);
	}

	inline int64_t pos() const noexcept {
		return _impl.pos();
	}
	// Sets the absolute file position.
	// The argument is signed, but must be non-negative!
	inline bool setPos(int64_t newPos) noexcept {
		return _impl.setPos(newPos);
	}

	// This function also sets file position to the end
	inline bool truncate(int64_t newFileSize) noexcept {
		return _impl.truncate(newFileSize);
	}

	inline int64_t size() const noexcept {
		return _impl.size();
	}

	inline bool atEnd() const noexcept {
		return _impl.atEnd();
	}

	static bool deleteFile(const char* filePath) noexcept {
		return Impl::deleteFile(filePath);
	}

	// Beware, it's OS-specific!
	static inline auto error_code() noexcept {
		return Impl::error_code();
	}

private:
	Impl _impl;
};

}
