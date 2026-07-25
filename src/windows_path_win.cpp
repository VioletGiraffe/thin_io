#include "windows_path_win.hpp"

#include <algorithm>
#include <assert.h>
#include <iterator>
#include <string.h>
#include <string_view>
#include <type_traits>

namespace thin_io {
namespace {

static constexpr wchar_t extendedPrefix[] = LR"(\\?\)";
static constexpr wchar_t extendedUncPrefix[] = LR"(\\?\UNC\)";
static constexpr size_t extendedPrefixLength = std::size(extendedPrefix) - 1;
static constexpr size_t extendedUncPrefixLength = std::size(extendedUncPrefix) - 1;

[[nodiscard]] constexpr bool isDriveLetter(const wchar_t c) noexcept
{
	return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

[[nodiscard]] constexpr wchar_t asciiLower(const wchar_t c) noexcept
{
	return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

[[nodiscard]] bool startsWith(const std::wstring_view text, const std::wstring_view prefix) noexcept
{
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

[[nodiscard]] bool startsWithAsciiCaseInsensitive(const std::wstring_view text, const std::wstring_view prefix) noexcept
{
	if (text.size() < prefix.size())
		return false;

	for (size_t i = 0; i < prefix.size(); ++i)
	{
		if (asciiLower(text[i]) != asciiLower(prefix[i]))
			return false;
	}
	return true;
}

[[nodiscard]] bool readNextComponent(const wchar_t* const path, const size_t length, size_t& readOffset,
									 size_t& componentStart, size_t& componentEnd) noexcept
{
	while (readOffset < length && path[readOffset] == L'\\')
		++readOffset;
	if (readOffset == length)
		return false;

	componentStart = readOffset;
	while (readOffset < length && path[readOffset] != L'\\')
		++readOffset;
	componentEnd = readOffset;
	return true;
}

[[nodiscard]] size_t trimTrailingSpacesAndPeriods(const wchar_t* const path, const size_t start, size_t end) noexcept
{
	while (end > start && (path[end - 1] == L' ' || path[end - 1] == L'.'))
		--end;
	return end;
}

[[nodiscard]] bool isComponent(const wchar_t* const path, const size_t start, const size_t end,
							   const std::wstring_view expected) noexcept
{
	return end - start == expected.size() && std::wstring_view{path + start, end - start} == expected;
}

void copyComponent(wchar_t* const path, const size_t start, const size_t end, size_t& writeOffset) noexcept
{
	assert(writeOffset <= start);
	for (size_t i = start; i < end; ++i)
		path[writeOffset++] = path[i];
}

void removePreviousComponent(wchar_t* const path, size_t& writeOffset, const size_t rootEnd) noexcept
{
	while (writeOffset > rootEnd && path[writeOffset - 1] != L'\\')
		--writeOffset;
	if (writeOffset > rootEnd)
		--writeOffset;
}

// Only exactly "." and ".." are path syntax; every other component is taken verbatim. Win32 would additionally strip
// trailing spaces and periods, which leaves entries that carry them unreachable and, where a sibling exists under the
// stripped name, silently redirects the operation onto it.
[[nodiscard]] size_t normalizePathComponents(wchar_t* const path, const size_t inputLength, size_t readOffset,
										 size_t writeOffset, const size_t rootEnd, const bool trailingSeparator) noexcept
{
	assert(rootEnd > 0 && rootEnd <= writeOffset && writeOffset <= readOffset);
	size_t componentStart = 0;
	size_t componentEnd = 0;
	while (readNextComponent(path, inputLength, readOffset, componentStart, componentEnd))
	{
		if (isComponent(path, componentStart, componentEnd, L"."))
			continue;
		if (isComponent(path, componentStart, componentEnd, L".."))
		{
			removePreviousComponent(path, writeOffset, rootEnd);
			continue;
		}

		if (path[writeOffset - 1] != L'\\')
			path[writeOffset++] = L'\\';
		copyComponent(path, componentStart, componentEnd, writeOffset);
	}

	if (trailingSeparator && path[writeOffset - 1] != L'\\')
		path[writeOffset++] = L'\\';
	path[writeOffset] = L'\0';
	return writeOffset;
}

} // namespace

static_assert(std::is_trivially_destructible_v<windows_path_buffer>);
static_assert(sizeof(windows_path_buffer) <= (windows_path_buffer::max_length + 1) * sizeof(wchar_t) + 16);

windows_path_buffer::windows_path_buffer(const char* const utf8Path) noexcept
{
	if (utf8Path == nullptr) [[unlikely]]
	{
		fail(ERROR_INVALID_PARAMETER);
		return;
	}

	const int requiredCharacters = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path, -1, nullptr, 0);
	if (requiredCharacters == 0) [[unlikely]]
	{
		const DWORD conversionError = ::GetLastError();
		fail(conversionError);
		return;
	}
	if (static_cast<size_t>(requiredCharacters - 1) > max_length) [[unlikely]]
	{
		fail(ERROR_FILENAME_EXCED_RANGE);
		return;
	}

	const int convertedCharacters = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path, -1,
																 _path.data(), requiredCharacters);
	if (convertedCharacters == 0) [[unlikely]]
	{
		const DWORD conversionError = ::GetLastError();
		fail(conversionError);
		return;
	}

	_length = static_cast<size_t>(convertedCharacters - 1);
	prepare();
}

windows_path_buffer::windows_path_buffer(const wchar_t* const nativePath) noexcept
{
	if (nativePath == nullptr) [[unlikely]]
	{
		fail(ERROR_INVALID_PARAMETER);
		return;
	}

	size_t length = 0;
	while (length <= max_length && nativePath[length] != L'\0')
		++length;
	if (length > max_length) [[unlikely]]
	{
		fail(ERROR_FILENAME_EXCED_RANGE);
		return;
	}

	std::copy_n(nativePath, length + 1, _path.data());
	_length = length;
	prepare();
}

void windows_path_buffer::prepare() noexcept
{
	if (_length == 0) [[unlikely]]
	{
		fail(ERROR_INVALID_NAME);
		return;
	}

	std::wstring_view path{_path.data(), _length};
	if (startsWith(path, extendedPrefix))
	{
		if (_length == extendedPrefixLength) [[unlikely]]
			fail(ERROR_INVALID_NAME);
		return;
	}

	std::replace(_path.begin(), _path.begin() + static_cast<ptrdiff_t>(_length), L'/', L'\\');
	path = std::wstring_view{_path.data(), _length};
	if (startsWith(path, LR"(\\.\)") || startsWith(path, LR"(\??\)")
		|| startsWithAsciiCaseInsensitive(path, LR"(\Device\)")) [[unlikely]]
	{
		fail(ERROR_NOT_SUPPORTED);
		return;
	}

	const bool trailingSeparator = _path[_length - 1] == L'\\';
	// The extended prefix disables Win32 lexical normalization, so ordinary absolute input must be normalized first.
	if (_length >= 3 && isDriveLetter(_path[0]) && _path[1] == L':' && _path[2] == L'\\')
	{
		_length = normalizePathComponents(_path.data(), _length, 3, 3, 3, trailingSeparator);
		if (_length > max_length - extendedPrefixLength) [[unlikely]]
		{
			fail(ERROR_FILENAME_EXCED_RANGE);
			return;
		}

		::memmove(_path.data() + extendedPrefixLength, _path.data(), (_length + 1) * sizeof(wchar_t));
		std::copy_n(extendedPrefix, extendedPrefixLength, _path.data());
		_length += extendedPrefixLength;
		return;
	}

	if (!startsWith(path, LR"(\\)"))
		return;

	size_t readOffset = 2;
	size_t componentStart = 0;
	size_t componentEnd = 0;
	if (!readNextComponent(_path.data(), _length, readOffset, componentStart, componentEnd)) [[unlikely]]
	{
		fail(ERROR_INVALID_NAME);
		return;
	}

	// Unlike the filesystem components below, a host or share name cannot legitimately carry trailing spaces or
	// periods, so trimming them here loses no reachable name and is what rejects malformed input like "\\..\share".
	componentEnd = trimTrailingSpacesAndPeriods(_path.data(), componentStart, componentEnd);
	if (componentEnd == componentStart) [[unlikely]]
	{
		fail(ERROR_INVALID_NAME);
		return;
	}

	_path[0] = L'\\';
	_path[1] = L'\\';
	size_t writeOffset = 2;
	copyComponent(_path.data(), componentStart, componentEnd, writeOffset);
	_path[writeOffset++] = L'\\';

	if (!readNextComponent(_path.data(), _length, readOffset, componentStart, componentEnd)) [[unlikely]]
	{
		fail(ERROR_INVALID_NAME);
		return;
	}

	componentEnd = trimTrailingSpacesAndPeriods(_path.data(), componentStart, componentEnd);
	if (componentEnd == componentStart) [[unlikely]]
	{
		fail(ERROR_INVALID_NAME);
		return;
	}

	copyComponent(_path.data(), componentStart, componentEnd, writeOffset);
	const size_t rootEnd = writeOffset;
	_length = normalizePathComponents(_path.data(), _length, readOffset, writeOffset, rootEnd, trailingSeparator);
	constexpr size_t uncPrefixGrowth = extendedUncPrefixLength - 2;
	if (_length > max_length - uncPrefixGrowth) [[unlikely]]
	{
		fail(ERROR_FILENAME_EXCED_RANGE);
		return;
	}

	::memmove(_path.data() + extendedUncPrefixLength, _path.data() + 2, (_length - 1) * sizeof(wchar_t));
	std::copy_n(extendedUncPrefix, extendedUncPrefixLength, _path.data());
	_length += uncPrefixGrowth;
}

void windows_path_buffer::fail(const DWORD error) noexcept
{
	_path[0] = L'\0';
	_length = 0;
	_error = error == ERROR_SUCCESS ? ERROR_INVALID_NAME : error;
}

bool windows_path_buffer::append_directory_separator() noexcept
{
	if (!*this) [[unlikely]]
		return false;
	if (_path[_length - 1] == L'\\')
		return true;

	if (_length == max_length) [[unlikely]]
	{
		fail(ERROR_FILENAME_EXCED_RANGE);
		return false;
	}

	_path[_length++] = L'\\';
	_path[_length] = L'\0';
	return true;
}

bool windows_path_buffer::append_directory_search_pattern() noexcept
{
	if (!*this) [[unlikely]]
		return false;

	// A trailing ':' is a bare drive-relative path such as "C:"; a separator would redirect it to the drive root,
	// so the pattern must stay relative: "C:*".
	if (_path[_length - 1] != L':' && !append_directory_separator()) [[unlikely]]
		return false;
	if (_length == max_length) [[unlikely]]
	{
		fail(ERROR_FILENAME_EXCED_RANGE);
		return false;
	}

	_path[_length++] = L'*';
	_path[_length] = L'\0';
	return true;
}

} // namespace thin_io
