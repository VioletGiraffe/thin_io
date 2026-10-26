#pragma once
#include "file.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdint.h>
#include <string>

namespace thin_io {

// Read-only file access for many small reads at scattered positions, such as parsing a container's metadata.
// - Bytes are read through a window that keeps what was already read: seeks within it, either way, cost no I/O.
// - Only positional reads are issued: a seek never reaches the OS.
// - Reads of at least window_size bytes go straight to the destination.
class [[nodiscard]] buffered_reader {
public:
	static constexpr size_t window_size = 128 * 1024;

	// Allocates the window: may throw std::bad_alloc
	buffered_reader();

	// Opens an existing file for reading, at position 0
	bool open(const char* path) noexcept;
#ifdef _WIN32
	bool open(const wchar_t* path) noexcept;
#endif

	[[nodiscard]] bool is_open() const noexcept { return _file.is_open(); }
	[[nodiscard]] std::optional<uint64_t> size() const noexcept { return _file.size(); }

	// Any position, end of file and past it included
	[[nodiscard]] uint64_t pos() const noexcept { return _pos; }
	void set_pos(uint64_t newPos) noexcept { _pos = newPos; }

	// Fewer bytes than requested only at end of file. Fails without moving the position.
	std::optional<uint64_t> read(void* dest, uint64_t size) noexcept;
	// min(size, window_size) bytes at the position, which stays put; fewer only at end of file.
	// Valid until the next read, peek or open.
	std::optional<std::span<const std::byte>> peek(size_t size) noexcept;

	// Beware, it's OS-specific!
	[[nodiscard]] static auto error_code() noexcept { return file::error_code(); }
	[[nodiscard]] static std::string text_for_last_error() noexcept { return file::text_for_last_error(); }

private:
	// The window's bytes from the position on; empty when the position is outside it
	[[nodiscard]] std::span<const std::byte> buffered_at_pos() const noexcept;
	bool fill_window_at_pos() noexcept;

private:
	file _file;
	std::unique_ptr<std::byte[]> _window;
	uint64_t _windowStart = 0;
	uint64_t _windowLength = 0;
	uint64_t _pos = 0;
};

} // namespace thin_io
