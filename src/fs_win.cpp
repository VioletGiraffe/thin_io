#include "fs.hpp"
#include "windows_path_win.hpp"

#include <Windows.h>

#include <limits>

namespace thin_io {

// FILETIME counts 100 ns ticks from 1601-01-01 UTC; this is the Unix epoch expressed in those units.
static constexpr int64_t unixEpochAsFileTime = 116'444'736'000'000'000;
static constexpr int64_t ticksPerSecond = 10'000'000;

// Bounds for the tick arithmetic below. The lower one is exactly the FILETIME epoch, before which a FILETIME cannot
// represent the time at all; the upper one is whatever fits in int64_t once the sub-second ticks are accounted for,
// which lands around the year 30000.
static constexpr int64_t minRepresentableSeconds = -(unixEpochAsFileTime / ticksPerSecond);
static constexpr int64_t maxRepresentableSeconds = (std::numeric_limits<int64_t>::max() - unixEpochAsFileTime - (ticksPerSecond - 1)) / ticksPerSecond;

[[nodiscard]] static bool toFileTime(const timestamp& t, FILETIME& fileTime) noexcept
{
	if (t.seconds < minRepresentableSeconds || t.seconds > maxRepresentableSeconds) [[unlikely]]
		return false;

	const auto ticks = static_cast<uint64_t>(unixEpochAsFileTime + t.seconds * ticksPerSecond + t.nanoseconds / 100);
	fileTime.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFF'FFFFu);
	fileTime.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
	return true;
}

[[nodiscard]] static bool setPreparedTimes(const wchar_t* path, const entry_times& times, const FILETIME& creation,
										   const FILETIME& lastAccess, const FILETIME& lastWrite) noexcept
{
	// FILE_FLAG_BACKUP_SEMANTICS is what makes a handle to a directory possible; it is a no-op for regular files.
	const HANDLE h = ::CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
								 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) [[unlikely]]
		return false;

	const bool success = ::SetFileTime(h,
		times.creation ? &creation : nullptr,
		times.last_access ? &lastAccess : nullptr,
		times.last_write ? &lastWrite : nullptr) != 0;

	const DWORD error = success ? 0 : ::GetLastError(); // CloseHandle overwrites the thread's last error
	::CloseHandle(h);
	if (!success) [[unlikely]]
		::SetLastError(error);

	return success;
}

template <class Character>
[[nodiscard]] static bool setTimesForPath(const Character* path, const entry_times& times) noexcept
{
	if (!times.creation && !times.last_access && !times.last_write)
		return true; // Nothing to write, so don't even open the path - matching the POSIX implementation

	FILETIME creation{}, lastAccess{}, lastWrite{};
	if ((times.creation && !toFileTime(*times.creation, creation))
		|| (times.last_access && !toFileTime(*times.last_access, lastAccess))
		|| (times.last_write && !toFileTime(*times.last_write, lastWrite))) [[unlikely]]
	{
		::SetLastError(ERROR_INVALID_PARAMETER);
		return false;
	}

	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
	{
		::SetLastError(nativePath.error_code());
		return false;
	}

	return setPreparedTimes(nativePath.c_str(), times, creation, lastAccess, lastWrite);
}

bool set_times(const char* path, const entry_times& times) noexcept
{
	return setTimesForPath(path, times);
}

bool set_times(const wchar_t* path, const entry_times& times) noexcept
{
	return setTimesForPath(path, times);
}

// A zero FILETIME is how Windows reports a timestamp the filesystem does not keep - FAT has no creation time, for one.
// It means "absent", not the year 1601.
[[nodiscard]] static std::optional<timestamp> fromFileTime(const FILETIME& fileTime) noexcept
{
	const auto rawTicks = (static_cast<uint64_t>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
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

[[nodiscard]] static std::optional<entry_times> getPreparedTimes(const wchar_t* path) noexcept
{
	// Reads the metadata without opening the path, so it works for directories and cannot perturb the access time
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	if (::GetFileAttributesExW(path, GetFileExInfoStandard, &attributes) == 0) [[unlikely]]
		return {};

	entry_times times;
	times.creation = fromFileTime(attributes.ftCreationTime);
	times.last_access = fromFileTime(attributes.ftLastAccessTime);
	times.last_write = fromFileTime(attributes.ftLastWriteTime);
	return times;
}

template <class Character>
[[nodiscard]] static std::optional<entry_times> getTimesForPath(const Character* path) noexcept
{
	windows_path_buffer nativePath{path};
	if (!nativePath) [[unlikely]]
	{
		::SetLastError(nativePath.error_code());
		return {};
	}

	return getPreparedTimes(nativePath.c_str());
}

std::optional<entry_times> get_times(const char* path) noexcept
{
	return getTimesForPath(path);
}

std::optional<entry_times> get_times(const wchar_t* path) noexcept
{
	return getTimesForPath(path);
}

} // namespace thin_io
