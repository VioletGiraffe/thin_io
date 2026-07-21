#pragma once
#include <optional>
#include <stdint.h>
#include <string>

namespace thin_io {

struct file_constants {
	enum class access_mode {Read = 1, Write = 2, ReadWrite = 3};
	// Existence handling is independent of access. CreateNew is an atomic exclusive create; CreateOrTruncate requires
	// write access. Existing contents are preserved by OpenExisting, OpenOrCreate, and a failed CreateNew.
	enum class open_disposition {OpenExisting, OpenOrCreate, CreateNew, CreateOrTruncate};
	enum class sys_cache_mode {CachingEnabled = 0, NoOsCaching = 1};
	enum class sharing_mode {NoSharing = 0, ShareRead = 1, ShareWrite = 2, ShareDelete = 4, ShareExec = 8};
	enum class mmap_access_mode {ReadOnly = 0, ReadWrite = 1};
};

template <class Impl>
class [[nodiscard]] file_interface final : public file_constants {
public:
	// Read opens an existing file, Write creates or truncates, and ReadWrite opens or creates.
	inline bool open(const char* path,
					 access_mode accessMode,
					 sys_cache_mode cacheMode = sys_cache_mode::CachingEnabled,
					 sharing_mode sharingMode = sharing_mode::ShareRead
			) noexcept
	{
		return open(path, accessMode, defaultOpenDisposition(accessMode), cacheMode, sharingMode);
	}

#ifdef _WIN32
	inline bool open(const wchar_t* path,
					 access_mode accessMode,
					 sys_cache_mode cacheMode = sys_cache_mode::CachingEnabled,
					 sharing_mode sharingMode = sharing_mode::ShareRead
			) noexcept
	{
		return open(path, accessMode, defaultOpenDisposition(accessMode), cacheMode, sharingMode);
	}
#endif

	inline bool open(const char* path,
					 access_mode accessMode,
					 open_disposition disposition,
					 sys_cache_mode cacheMode = sys_cache_mode::CachingEnabled,
					 sharing_mode sharingMode = sharing_mode::ShareRead
			) noexcept
	{
		return _impl.open(path, accessMode, disposition, cacheMode, sharingMode);
	}

#ifdef _WIN32
	inline bool open(const wchar_t* path,
					 access_mode accessMode,
					 open_disposition disposition,
					 sys_cache_mode cacheMode = sys_cache_mode::CachingEnabled,
					 sharing_mode sharingMode = sharing_mode::ShareRead
			) noexcept
	{
		return _impl.open(path, accessMode, disposition, cacheMode, sharingMode);
	}
#endif

	inline static file_interface open_file(const char* path,
		access_mode accessMode,
		sys_cache_mode cacheMode = sys_cache_mode::CachingEnabled,
		sharing_mode sharingMode = sharing_mode::ShareRead
	) noexcept
	{
		file_interface<Impl> f;
		f.open(path, accessMode, cacheMode, sharingMode);
		return f;
	}

#ifdef _WIN32
	inline static file_interface open_file(const wchar_t* path,
		access_mode accessMode,
		sys_cache_mode cacheMode = sys_cache_mode::CachingEnabled,
		sharing_mode sharingMode = sharing_mode::ShareRead
	) noexcept
	{
		file_interface<Impl> f;
		f.open(path, accessMode, cacheMode, sharingMode);
		return f;
	}
#endif

	inline static file_interface open_file(const char* path,
		access_mode accessMode,
		open_disposition disposition,
		sys_cache_mode cacheMode = sys_cache_mode::CachingEnabled,
		sharing_mode sharingMode = sharing_mode::ShareRead
	) noexcept
	{
		file_interface<Impl> f;
		f.open(path, accessMode, disposition, cacheMode, sharingMode);
		return f;
	}

#ifdef _WIN32
	inline static file_interface open_file(const wchar_t* path,
		access_mode accessMode,
		open_disposition disposition,
		sys_cache_mode cacheMode = sys_cache_mode::CachingEnabled,
		sharing_mode sharingMode = sharing_mode::ShareRead
	) noexcept
	{
		file_interface<Impl> f;
		f.open(path, accessMode, disposition, cacheMode, sharingMode);
		return f;
	}
#endif

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
	// NOTE: Win32: pread / pwrite alter the file position; Linux / POSIX: the position is NOT altered
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

	// Sets the exact logical file size, preserving existing contents within the new size. Growing does not guarantee
	// physical allocation, and the contents of the extended range are platform-dependent; call preallocate() separately
	// when storage must be reserved. Does not change the file position, which may consequently remain past the new end of
	// file when shrinking. Returns false on failure; error_code() provides the native reason.
	[[nodiscard]] inline bool resize(uint64_t newFileSize) noexcept {
		return _impl.resize(newFileSize);
	}

	// Reserves physical storage for the existing logical range [0, size), so writes to that range will not fail for lack
	// of filesystem space after a successful call. size must not exceed the current logical file size. The implementation
	// may reserve more storage than requested, but does not change the logical size, file contents, or file position.
	// Returns false when preallocation is unsupported or fails; error_code() provides the native reason.
	[[nodiscard]] inline bool preallocate(uint64_t size) noexcept {
		return _impl.preallocate(size);
	}

	[[nodiscard]] inline bool fsync() noexcept {
		return _impl.fsync();
	}

	[[nodiscard]] inline bool fdatasync() noexcept {
		return _impl.fdatasync();
	}

	[[nodiscard]] inline void* mmap(mmap_access_mode mode, uint64_t offset, uint64_t length) noexcept {
		return _impl.mmap(mode, offset, length);
	}

	[[nodiscard]] bool unmap(void* mapAddress) noexcept {
		return _impl.unmap(mapAddress);
	}

	[[nodiscard]] inline std::optional<uint64_t> size() const noexcept {
		return _impl.size();
	}

	// Returns true when the current position is at or past the logical end of file. Returns false if either value cannot
	// be queried.
	[[nodiscard]] inline bool at_end() const noexcept {
		const auto currentPosition = pos();
		const auto currentSize = size();
		return currentPosition && currentSize && *currentPosition >= *currentSize;
	}

	static bool delete_file(const char* filePath) noexcept {
		return Impl::delete_file(filePath);
	}

#ifdef _WIN32
	static bool delete_file(const wchar_t* filePath) noexcept {
		return Impl::delete_file(filePath);
	}
#endif

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
	[[nodiscard]] static constexpr open_disposition defaultOpenDisposition(const access_mode accessMode) noexcept
	{
		switch (accessMode)
		{
		case access_mode::Read:
			return open_disposition::OpenExisting;
		case access_mode::Write:
			return open_disposition::CreateOrTruncate;
		case access_mode::ReadWrite:
			return open_disposition::OpenOrCreate;
		}

		return open_disposition::OpenExisting;
	}

	Impl _impl;
};

} // namespace thin_io
