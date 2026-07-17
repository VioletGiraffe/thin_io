#pragma once

#include <expected>
#include <stdint.h>
#include <string>

namespace thin_io {

#ifdef _WIN32
using filesystem_error_code = uint32_t;
#else
using filesystem_error_code = int;
#endif

struct filesystem_error {
	filesystem_error_code native_code;

	[[nodiscard]] bool operator==(const filesystem_error&) const noexcept = default;
};

template <class Value>
using filesystem_result = std::expected<Value, filesystem_error>;

// Captures GetLastError() on Windows or errno on POSIX. Call immediately after the failing native operation, before
// cleanup or any other native call can alter the thread-local error state.
[[nodiscard]] filesystem_error capture_last_filesystem_error() noexcept;

// Includes the numeric native code and its description when available.
[[nodiscard]] std::string format_filesystem_error(const filesystem_error& error);

} // namespace thin_io
