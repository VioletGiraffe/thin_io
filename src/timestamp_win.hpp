#pragma once

#include "filesystem_types.hpp"

#include <Windows.h>

#include <limits>
#include <optional>

namespace thin_io {

// FILETIME counts 100 ns ticks from 1601-01-01 UTC; this is the Unix epoch expressed in those units.
inline constexpr int64_t unixEpochAsFileTime = 116'444'736'000'000'000;
inline constexpr int64_t ticksPerSecond = 10'000'000;

// Bounds for the tick arithmetic below. The lower one is exactly the FILETIME epoch, before which a FILETIME cannot
// represent the time at all; the upper one is whatever fits in int64_t once the sub-second ticks are accounted for,
// which lands around the year 30000.
inline constexpr int64_t minRepresentableSeconds = -(unixEpochAsFileTime / ticksPerSecond);
inline constexpr int64_t maxRepresentableSeconds = (std::numeric_limits<int64_t>::max() - unixEpochAsFileTime - (ticksPerSecond - 1)) / ticksPerSecond;

[[nodiscard]] inline bool toFileTime(const timestamp& t, FILETIME& fileTime) noexcept
{
	if (t.seconds < minRepresentableSeconds || t.seconds > maxRepresentableSeconds) [[unlikely]]
		return false;

	const auto ticks = static_cast<uint64_t>(unixEpochAsFileTime + t.seconds * ticksPerSecond + t.nanoseconds / 100);
	fileTime.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFF'FFFFu);
	fileTime.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
	return true;
}

// A zero tick count is how Windows reports a timestamp the filesystem does not keep - FAT has no creation time,
// for one. It means "absent", not the year 1601.
[[nodiscard]] inline std::optional<timestamp> fromFileTimeTicks(const uint64_t rawTicks) noexcept
{
	if (rawTicks == 0)
		return {};

	const int64_t ticks = static_cast<int64_t>(rawTicks) - unixEpochAsFileTime;
	int64_t seconds = ticks / ticksPerSecond;
	int64_t subSecondTicks = ticks % ticksPerSecond;
	if (subSecondTicks < 0) // Division truncates towards zero, but nanoseconds is a positive offset from seconds
	{
		--seconds;
		subSecondTicks += ticksPerSecond;
	}

	return timestamp{ .seconds = seconds, .nanoseconds = static_cast<uint32_t>(subSecondTicks) * 100 };
}

[[nodiscard]] inline std::optional<timestamp> fromFileTime(const FILETIME& fileTime) noexcept
{
	return fromFileTimeTicks((static_cast<uint64_t>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime);
}

// Applies the requested timestamps to an open handle; a nullopt member is left untouched. Fails with
// ERROR_INVALID_PARAMETER when a requested time cannot be represented as FILETIME.
[[nodiscard]] inline bool setFileTimes(const HANDLE fileHandle, const entry_times& times) noexcept
{
	FILETIME creation{}, lastAccess{}, lastWrite{};
	if ((times.creation && !toFileTime(*times.creation, creation))
		|| (times.last_access && !toFileTime(*times.last_access, lastAccess))
		|| (times.last_write && !toFileTime(*times.last_write, lastWrite))) [[unlikely]]
	{
		::SetLastError(ERROR_INVALID_PARAMETER);
		return false;
	}

	return ::SetFileTime(fileHandle,
		times.creation ? &creation : nullptr,
		times.last_access ? &lastAccess : nullptr,
		times.last_write ? &lastWrite : nullptr) != 0;
}

} // namespace thin_io
