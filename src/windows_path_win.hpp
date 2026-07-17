#pragma once

#include <Windows.h>

#include <array>
#include <stddef.h>

namespace thin_io {

class [[nodiscard]] windows_path_buffer final {
public:
	static constexpr size_t max_length = 32'767;

	explicit windows_path_buffer(const char* utf8Path) noexcept;
	explicit windows_path_buffer(const wchar_t* nativePath) noexcept;
	windows_path_buffer(const windows_path_buffer&) = delete;
	windows_path_buffer& operator=(const windows_path_buffer&) = delete;

	[[nodiscard]] explicit operator bool() const noexcept { return _error == ERROR_SUCCESS; }
	[[nodiscard]] const wchar_t* c_str() const noexcept { return _path.data(); }
	[[nodiscard]] DWORD error_code() const noexcept { return _error; }

	[[nodiscard]] bool append_directory_separator() noexcept;
	[[nodiscard]] bool append_directory_search_pattern() noexcept;

private:
	void prepare() noexcept;
	void fail(DWORD error) noexcept;

	std::array<wchar_t, max_length + 1> _path;
	size_t _length = 0;
	DWORD _error = ERROR_SUCCESS;
};

} // namespace thin_io
