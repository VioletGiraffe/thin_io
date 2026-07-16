#include "fs.hpp"

#include <fcntl.h>    // AT_FDCWD
#include <sys/stat.h> // utimensat, UTIME_OMIT
#include <time.h>

#ifdef __APPLE__
#include <sys/attr.h> // setattrlist, ATTR_CMN_CRTIME
#include <unistd.h>
#endif

namespace thin_io {

[[nodiscard]] static timespec toTimespec(const timestamp& t) noexcept
{
	timespec ts{};
	ts.tv_sec = static_cast<time_t>(t.seconds);
	ts.tv_nsec = static_cast<long>(t.nanoseconds);
	return ts;
}

// UTIME_OMIT in tv_nsec is how utimensat() is told to leave a timestamp alone.
[[nodiscard]] static timespec toTimespecOrOmit(const std::optional<timestamp>& t) noexcept
{
	if (!t)
	{
		timespec ts{};
		ts.tv_nsec = UTIME_OMIT;
		return ts;
	}

	return toTimespec(*t);
}

bool set_times(const char* path, const entry_times& times) noexcept
{
	if (times.last_access || times.last_write)
	{
		const timespec ts[2] { toTimespecOrOmit(times.last_access), toTimespecOrOmit(times.last_write) };
		if (::utimensat(AT_FDCWD, path, ts, 0) != 0)
			return false;
	}

#ifdef __APPLE__
	static_assert(creation_time_settable);

	// Darwin exposes the birth time as a writable attribute; POSIX has no equivalent. Applied after utimensat() so that
	// an explicitly requested birth time cannot be clobbered by a side effect of setting the modification time.
	if (times.creation)
	{
		attrlist attributes{};
		attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
		attributes.commonattr = ATTR_CMN_CRTIME;

		timespec creation = toTimespec(*times.creation);
		if (::setattrlist(path, &attributes, &creation, sizeof(creation), 0) != 0)
			return false;
	}
#else
	static_assert(!creation_time_settable); // Linux has no API to set the birth time, so entry_times::creation is ignored
#endif

	return true;
}

[[nodiscard]] static timestamp fromTimespec(const timespec& ts) noexcept
{
	return timestamp{ .seconds = ts.tv_sec, .nanoseconds = static_cast<uint32_t>(ts.tv_nsec) };
}

std::optional<entry_times> get_times(const char* path) noexcept
{
	struct stat info;
	if (::stat(path, &info) != 0)
		return {};

	entry_times times;
#ifdef __APPLE__
	times.creation = fromTimespec(info.st_birthtimespec);
	times.last_access = fromTimespec(info.st_atimespec);
	times.last_write = fromTimespec(info.st_mtimespec);
#else
	times.last_access = fromTimespec(info.st_atim);
	times.last_write = fromTimespec(info.st_mtim);

#ifdef STATX_BTIME
	// The birth time is absent from stat(): reading it needs statx(), which arrived in kernel 4.11 and reports the
	// field only on the filesystems that keep one. Both shortfalls surface as a birth time this call leaves unset.
	struct statx extendedInfo;
	if (::statx(AT_FDCWD, path, 0, STATX_BTIME, &extendedInfo) == 0 && (extendedInfo.stx_mask & STATX_BTIME) != 0)
		times.creation = timestamp{ .seconds = extendedInfo.stx_btime.tv_sec, .nanoseconds = extendedInfo.stx_btime.tv_nsec };
#endif
#endif

	return times;
}

} // namespace thin_io
