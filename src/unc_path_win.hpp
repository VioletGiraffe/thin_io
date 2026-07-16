#pragma once

#include <Windows.h>

#include <iterator> // std::size
#include <stddef.h>
#include <string.h> // strlen, memcpy

namespace thin_io {

// Converts a UTF-8 path to UTF-16, prefixing absolute paths with \\?\ to lift the MAX_PATH limit. That prefix also
// turns off path normalization, which is why the slashes have to be fixed up here instead of by Win32.
template <size_t N>
inline void to_wide_unc_path(const char* str, WCHAR(&wCharArray)[N])
{
	const size_t path_length = ::strlen(str);
	size_t prefix_length = 0;
	if (path_length >= 2 && str[1] == ':') // Absolute path?
	{
		static constexpr WCHAR prefix[] = LR"(\\?\)";
		prefix_length = std::size(prefix) - 1 /* null */;
		::memcpy(wCharArray, prefix, prefix_length * sizeof(WCHAR));
	}

	const auto nChars = ::MultiByteToWideChar(CP_UTF8, 0, str, (int)path_length, wCharArray + prefix_length, (int)(N - prefix_length - 1));
	wCharArray[prefix_length + nChars] = 0;

	// Fix non-Windows slashes.
	// TODO: is memchr faster?
	for (size_t i = prefix_length; i < prefix_length + nChars; ++i)
	{
		if (wCharArray[i] == L'/')
			wCharArray[i] = L'\\';
	}
}

} // namespace thin_io
