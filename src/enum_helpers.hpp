#pragma once

#include <type_traits>

template <typename enum_t>
struct enable_enum_arithmetic {
	static constexpr bool value = false;
};

template <typename enum_t>
constexpr bool enable_enum_arithmetic_v = enable_enum_arithmetic<enum_t>::value;

template <typename enum_t>
constexpr auto operator|(const enum_t& lhs, const enum_t& rhs)
	-> typename std::enable_if_t<enable_enum_arithmetic_v<enum_t>,
								 std::underlying_type_t<enum_t>>
{
	return std::to_underlying(lhs) | std::to_underlying(rhs);
}

template <typename enum_t>
constexpr auto operator&(const enum_t& lhs, const enum_t& rhs)
	-> typename std::enable_if_t<enable_enum_arithmetic_v<enum_t>,
								 std::underlying_type_t<enum_t>>
{
	return std::to_underlying(lhs) & std::to_underlying(rhs);
}

#define ENABLE_ENUM_ARITHMETIC(enum_t)                         \
template <>                                                           \
struct enable_enum_arithmetic<enum_t> {                        \
	static_assert(std::is_enum_v<enum_t>, "enum_t must be an enum."); \
	static constexpr bool value = true;                               \
}
