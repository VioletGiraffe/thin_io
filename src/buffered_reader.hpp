#pragma once
#include "file.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdint.h>
#include <string>

namespace thin_io {

// Read-only file access for many small reads at scattered positions, such as parsing a container's metadata.
// - Bytes are read through two windows that keep what was already read: seeks within them, either way, cost no I/O.
//   A read outside both refills the less recently used one, so reads alternating between two regions, such as a header and
//   the metadata it points to, cost no I/O after the first of each.
// - The end of file is kept once a read finds it: no read reaches past it, reads and peeks at or past it cost no I/O,
//   and a window filled near it ends at it.
// - What was read is a snapshot: later changes to the file, growth included, may go unseen until open.
// - Only positional reads are issued: a seek never reaches the OS.
// - Reads of at least window_size() bytes go straight to the destination.
class [[nodiscard]] buffered_reader {
public:
	static constexpr size_t default_window_size = 128 * 1024;
	static constexpr size_t max_window_size = size_t{ 1 } << 30;

	buffered_reader() noexcept = default;
	// windowSize is each window's, rounded down to a multiple of 4 KB, from 4 KB to max_window_size
	explicit buffered_reader(size_t windowSize) noexcept;
	// Leaves other closed, as if newly constructed with the same window size
	buffered_reader(buffered_reader&& other) noexcept;
	buffered_reader& operator=(buffered_reader&& other) noexcept;

	// Opens an existing file for reading, at position 0. The current file is closed first, even when this open fails.
	// Allocates the windows if there are none yet: may throw std::bad_alloc
	bool open(const char* path);
#ifdef _WIN32
	bool open(const wchar_t* path);
#endif
	void close() noexcept;

	[[nodiscard]] bool is_open() const noexcept { return _file.is_open(); }
	[[nodiscard]] std::optional<uint64_t> size() const noexcept { return _file.size(); }
	[[nodiscard]] size_t window_size() const noexcept { return _windowSize; }

	// Any position, end of file and past it included
	[[nodiscard]] uint64_t pos() const noexcept { return _pos; }
	void set_pos(uint64_t newPos) noexcept { _pos = newPos; }

	// Fewer bytes than requested only at end of file. Fails without moving the position.
	std::optional<uint64_t> read(void* dest, uint64_t size) noexcept;
	// min(size, window_size()) bytes at the position, which stays put; fewer only at end of file.
	// Valid until the next read, peek or open.
	std::optional<std::span<const std::byte>> peek(size_t size) noexcept;

	// Beware, it's OS-specific!
	[[nodiscard]] static auto error_code() noexcept { return file::error_code(); }
	[[nodiscard]] static std::string text_for_last_error() noexcept { return file::text_for_last_error(); }

private:
	struct window {
		uint64_t start = 0;
		uint64_t length = 0;
	};

	[[nodiscard]] size_t window_capacity() const noexcept;
	void allocate_windows_if_missing();
	[[nodiscard]] std::byte* window_bytes(size_t index) const noexcept;
	// The bytes from the position on, in the window holding it, which becomes the more recently used; empty when neither holds it
	[[nodiscard]] std::span<const std::byte> buffered_at_pos() noexcept;
	// Refills the less recently used window. Requires the position before _endOfFile.
	// On success, the position is in that window or at or past _endOfFile.
	bool fill_window_at_pos() noexcept;

private:
	file _file;
	size_t _windowSize = default_window_size;
	std::unique_ptr<std::byte[]> _windowBuffer; // both windows' bytes, window_capacity() each
	std::array<window, 2> _windows;
	size_t _recentWindow = 0; // the index of the more recently used
	uint64_t _pos = 0;
	// Reads at or past it return nothing; UINT64_MAX until a read finds the end of file
	uint64_t _endOfFile = UINT64_MAX;
};

} // namespace thin_io
