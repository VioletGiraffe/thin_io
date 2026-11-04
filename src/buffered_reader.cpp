#include "buffered_reader.hpp"

#include <algorithm>
#include <assert.h>
#include <string.h>
#include <utility>

namespace thin_io {

// Window starts are multiples of this: page-aligned reads; a start rounded down leaves room for short backward seeks
static constexpr size_t window_alignment = 4096;
static_assert(buffered_reader::default_window_size % window_alignment == 0);
static_assert(buffered_reader::max_window_size % window_alignment == 0);

buffered_reader::buffered_reader(const size_t windowSize) noexcept :
	_windowSize{ std::clamp(windowSize - windowSize % window_alignment, window_alignment, max_window_size) }
{
}

buffered_reader::buffered_reader(buffered_reader&& other) noexcept
{
	*this = std::move(other);
}

buffered_reader& buffered_reader::operator=(buffered_reader&& other) noexcept
{
	_file = std::move(other._file);
	_windowSize = other._windowSize;
	_windowBuffer = std::move(other._windowBuffer);
	_windows = other._windows;
	_recentWindow = other._recentWindow;
	_pos = other._pos;
	_endOfFile = other._endOfFile;
	other.close();
	return *this;
}

bool buffered_reader::open(const char* path)
{
	close();
	allocate_windows_if_missing();
	return _file.open(path, file::access_mode::Read);
}

#ifdef _WIN32
bool buffered_reader::open(const wchar_t* path)
{
	close();
	allocate_windows_if_missing();
	return _file.open(path, file::access_mode::Read);
}
#endif

void buffered_reader::close() noexcept
{
	// Nothing was written, so a failed close loses nothing
	(void)_file.close();
	_pos = 0;
	_windows = {};
	_endOfFile = UINT64_MAX;
}

std::optional<uint64_t> buffered_reader::read(void* dest, const uint64_t size) noexcept
{
	auto* const out = static_cast<std::byte*>(dest);
	const uint64_t startPos = _pos;
	uint64_t total = 0;
	while (total < size)
	{
		const uint64_t remaining = size - total;
		if (const std::span<const std::byte> buffered = buffered_at_pos(); !buffered.empty())
		{
			const uint64_t n = std::min<uint64_t>(remaining, buffered.size());
			::memcpy(out + total, buffered.data(), n);
			total += n;
			_pos += n;
		}
		else if (_pos >= _endOfFile)
			break;
		else if (remaining >= _windowSize)
		{
			const auto n = _file.pread(out + total, std::min(remaining, _endOfFile - _pos), _pos);
			if (!n)
			{
				_pos = startPos;
				return {};
			}
			if (*n == 0)
			{
				_endOfFile = _pos;
				break;
			}
			total += *n;
			_pos += *n;
		}
		else if (!fill_window_at_pos())
		{
			_pos = startPos;
			return {};
		}
	}
	return total;
}

std::optional<std::span<const std::byte>> buffered_reader::peek(const size_t size) noexcept
{
	const auto wanted = static_cast<size_t>(std::min<uint64_t>({ size, _windowSize, _endOfFile - std::min(_pos, _endOfFile) }));
	if (buffered_at_pos().size() < wanted && !fill_window_at_pos())
		return {};

	const std::span<const std::byte> buffered = buffered_at_pos();
	return buffered.first(std::min(wanted, buffered.size()));
}

// Holds window_size() bytes from any position despite the rounded-down start
size_t buffered_reader::window_capacity() const noexcept
{
	return _windowSize + window_alignment;
}

void buffered_reader::allocate_windows_if_missing()
{
	if (!_windowBuffer)
		_windowBuffer = std::make_unique_for_overwrite<std::byte[]>(_windows.size() * window_capacity());
}

std::byte* buffered_reader::window_bytes(const size_t index) const noexcept
{
	return _windowBuffer.get() + index * window_capacity();
}

std::span<const std::byte> buffered_reader::buffered_at_pos() noexcept
{
	// The more recently used first: after a refill it holds more from the position on than the other
	for (const size_t index : { _recentWindow, 1 - _recentWindow })
	{
		const window& w = _windows[index];
		if (_pos >= w.start && _pos < w.start + w.length)
		{
			_recentWindow = index;
			const auto offset = static_cast<size_t>(_pos - w.start);
			return { window_bytes(index) + offset, static_cast<size_t>(w.length) - offset };
		}
	}
	return {};
}

bool buffered_reader::fill_window_at_pos() noexcept
{
	assert(_pos < _endOfFile);
	const size_t capacity = window_capacity();
	uint64_t start = _pos - _pos % window_alignment;
	// Ends at a known end of file, keeping the bytes before the position: a container's tail is often parsed backwards
	if (_endOfFile - start < capacity)
	{
		const uint64_t lowestStart = _endOfFile > capacity ? _endOfFile - capacity : 0;
		start = std::min(start, (lowestStart + window_alignment - 1) / window_alignment * window_alignment);
	}

	const size_t index = 1 - _recentWindow;
	std::byte* const bytes = window_bytes(index);
	const auto wanted = static_cast<size_t>(std::min<uint64_t>(capacity, _endOfFile - start));
	uint64_t length = 0;
	// pread may transfer less than requested before the end of file: only a read of 0 bytes finds it
	while (length < wanted)
	{
		const auto n = _file.pread(bytes + length, wanted - length, start + length);
		if (!n)
		{
			_windows[index].length = 0;
			return false;
		}
		if (*n == 0)
		{
			_endOfFile = start + length;
			break;
		}
		length += *n;
	}

	// Nothing past the end of file replaces the window
	if (length != 0)
	{
		_windows[index] = { start, length };
		_recentWindow = index;
	}
	return true;
}

} // namespace thin_io
