#pragma once

#include "filesystem_types.hpp"

#include <fcntl.h>    // AT_* constants
#include <sys/stat.h> // UTIME_OMIT, statx
#include <time.h>

namespace thin_io {

[[nodiscard]] inline timespec toTimespec(const timestamp& t) noexcept
{
	timespec ts{};
	ts.tv_sec = static_cast<time_t>(t.seconds);
	ts.tv_nsec = static_cast<long>(t.nanoseconds);
	return ts;
}

// UTIME_OMIT in tv_nsec is how utimensat() and futimens() are told to leave a timestamp alone.
[[nodiscard]] inline timespec toTimespecOrOmit(const std::optional<timestamp>& t) noexcept
{
	if (!t)
	{
		timespec ts{};
		ts.tv_nsec = UTIME_OMIT;
		return ts;
	}

	return toTimespec(*t);
}

[[nodiscard]] inline timestamp fromTimespec(const timespec& ts) noexcept
{
	return timestamp{ .seconds = ts.tv_sec, .nanoseconds = static_cast<uint32_t>(ts.tv_nsec) };
}

#ifdef STATX_BTIME
// The birth time is absent from stat(): reading it needs statx(), which arrived in kernel 4.11 and reports the
// field only on the filesystems that keep one. Both shortfalls surface as a nullopt birth time.
[[nodiscard]] inline std::optional<timestamp> statxBirthTime(const int dirFd, const char* const path, const int flags) noexcept
{
	struct statx extendedInfo;
	if (::statx(dirFd, path, flags, STATX_BTIME, &extendedInfo) == 0 && (extendedInfo.stx_mask & STATX_BTIME) != 0)
		return timestamp{ .seconds = extendedInfo.stx_btime.tv_sec, .nanoseconds = extendedInfo.stx_btime.tv_nsec };

	return {};
}
#endif

} // namespace thin_io
