#include "buffered_reader.hpp"

#include <algorithm>
#include <string.h>
#include <utility>

namespace thin_io {

// Window starts are rounded down to this: page-aligned reads, and room for short backward seeks
static constexpr uint64_t window_alignment = 4096;
static_assert(buffered_reader::window_size % window_alignment == 0);
// Holds window_size bytes from any position despite the rounded-down start
static constexpr size_t window_capacity = buffered_reader::window_size + window_alignment;

buffered_reader::buffered_reader(buffered_reader&& other) noexcept
{
	*this = std::move(other);
}

buffered_reader& buffered_reader::operator=(buffered_reader&& other) noexcept
{
	_file = std::move(other._file);
	_window = std::move(other._window);
	_windowStart = other._windowStart;
	_windowLength = other._windowLength;
	_pos = other._pos;
	_endOfFile = other._endOfFile;
	other.close();
	return *this;
}

bool buffered_reader::open(const char* path)
{
	close();
	allocate_window_if_missing();
	return _file.open(path, file::access_mode::Read);
}

#ifdef _WIN32
bool buffered_reader::open(const wchar_t* path)
{
	close();
	allocate_window_if_missing();
	return _file.open(path, file::access_mode::Read);
}
#endif

void buffered_reader::close() noexcept
{
	// Nothing was written, so a failed close loses nothing
	(void)_file.close();
	_pos = 0;
	_windowLength = 0;
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
		else if (remaining >= window_size)
		{
			const auto n = _file.pread(out + total, remaining, _pos);
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
	const auto wanted = static_cast<size_t>(std::min<uint64_t>({ size, window_size, _endOfFile - std::min(_pos, _endOfFile) }));
	if (buffered_at_pos().size() < wanted && !fill_window_at_pos())
		return {};

	const std::span<const std::byte> buffered = buffered_at_pos();
	return buffered.first(std::min(wanted, buffered.size()));
}

void buffered_reader::allocate_window_if_missing()
{
	if (!_window)
		_window = std::make_unique_for_overwrite<std::byte[]>(window_capacity);
}

std::span<const std::byte> buffered_reader::buffered_at_pos() const noexcept
{
	if (_pos < _windowStart || _pos >= _windowStart + _windowLength)
		return {};

	const auto offset = static_cast<size_t>(_pos - _windowStart);
	return { _window.get() + offset, static_cast<size_t>(_windowLength) - offset };
}

bool buffered_reader::fill_window_at_pos() noexcept
{
	const uint64_t start = _pos - _pos % window_alignment;
	uint64_t length = 0;
	// pread may transfer less than requested before the end of file
	while (length < window_capacity)
	{
		const auto n = _file.pread(_window.get() + length, window_capacity - length, start + length);
		if (!n)
		{
			_windowLength = 0;
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
		_windowStart = start;
		_windowLength = length;
	}
	return true;
}

} // namespace thin_io
