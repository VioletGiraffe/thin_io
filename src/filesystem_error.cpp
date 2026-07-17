#include "filesystem_error.hpp"

#include <assert.h>
#include <charconv>
#include <limits>
#include <string>
#include <system_error>
#include <type_traits>

#ifdef _WIN32
#include <Windows.h>
#else
#include <errno.h>
#endif

namespace thin_io {
namespace {

static_assert(std::is_trivially_copyable_v<filesystem_error>);

[[nodiscard]] std::string nativeErrorDescription(const filesystem_error& error)
{
#ifdef _WIN32
	if (error.native_code > static_cast<uint32_t>(std::numeric_limits<int>::max()))
		return {};

	return std::error_code{static_cast<int>(error.native_code), std::system_category()}.message();
#else
	return std::error_code{error.native_code, std::generic_category()}.message();
#endif
}

} // namespace

filesystem_error capture_last_filesystem_error() noexcept
{
#ifdef _WIN32
	return filesystem_error{ .native_code = ::GetLastError() };
#else
	return filesystem_error{ .native_code = errno };
#endif
}

std::string format_filesystem_error(const filesystem_error& error)
{
	std::string text = nativeErrorDescription(error);
	char codeBuffer[std::numeric_limits<filesystem_error_code>::digits10 + 3];
	const auto [codeEnd, conversionError] = std::to_chars(codeBuffer, codeBuffer + sizeof(codeBuffer), error.native_code);
	assert(conversionError == std::errc{});
	const size_t codeLength = static_cast<size_t>(codeEnd - codeBuffer);

	if (!text.empty())
	{
		text.reserve(codeLength + 2 + text.size());
		text.insert(0, ": ");
	}
	text.insert(0, codeBuffer, codeLength);

	return text;
}

} // namespace thin_io
