#pragma once

// What the filesystem holding the working directory can store. The tests create their entries there, and a test
// whose expectation depends on the filesystem asks here.

#include "catch_thin_io.hpp"

#include <string_view>

#ifdef _WIN32
#include <Windows.h>
#elif defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/param.h>
#include <sys/mount.h>
#endif

namespace test_volume {

// FAT12, FAT16, FAT32 or exFAT.
[[nodiscard]] inline bool isFatFamily() noexcept
{
	static const bool fatFamily = []() noexcept {
#ifdef _WIN32
		// By handle: a RAM disk the mount manager does not know has no volume path to query by.
		const HANDLE directory = ::CreateFileW(L".", 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (directory == INVALID_HANDLE_VALUE)
			return false;

		wchar_t name[MAX_PATH + 1]{};
		const bool queried = ::GetVolumeInformationByHandleW(directory, nullptr, 0, nullptr, nullptr, nullptr, name, MAX_PATH + 1) != 0;
		::CloseHandle(directory);
		const std::wstring_view type{ name };
		return queried && (type == L"exFAT" || type.starts_with(L"FAT"));
#elif defined(__linux__)
		struct statfs info;
		if (::statfs(".", &info) != 0)
			return false;
		return info.f_type == 0x4d44 /* MSDOS_SUPER_MAGIC */ || info.f_type == 0x2011BAB0 /* EXFAT_SUPER_MAGIC */;
#else
		struct statfs info;
		if (::statfs(".", &info) != 0)
			return false;
		const std::string_view type{ info.f_fstypename };
		return type == "msdos" || type == "msdosfs" || type == "exfat";
#endif
	}();
	return fatFamily;
}

// Hard links, symbolic links and junctions. Warns when it returns true: the caller only has to return.
[[nodiscard]] inline bool linksUnsupported()
{
	if (!isFatFamily())
		return false;

	WARN("This filesystem has no links: test skipped");
	return true;
}

// FAT timestamps start in 1980.
[[nodiscard]] inline bool storesTimesBefore1980() noexcept
{
	return !isFatFamily();
}

[[nodiscard]] inline double secondsApart(const thin_io::timestamp& a, const thin_io::timestamp& b) noexcept
{
	const double difference = static_cast<double>(a.seconds - b.seconds)
		+ (static_cast<double>(a.nanoseconds) - static_cast<double>(b.nanoseconds)) / 1e9;
	return difference < 0 ? -difference : difference;
}

// Checks every timestamp set in requested against actual, as closely as the filesystem stores it:
//   FAT and exFAT - write and creation times to 2 s; FAT32 keeps only the date of the last access;
//   everything else - exactly.
inline void checkStoredTimes(const thin_io::entry_times& actual, const thin_io::entry_times& requested)
{
	const double writeAndCreationTolerance = isFatFamily() ? 2 : 0;
	const double lastAccessTolerance = isFatFamily() ? 24 * 60 * 60 : 0;

	if (requested.creation.is_set())
	{
		CAPTURE(actual.creation, requested.creation);
		CHECK(secondsApart(actual.creation, requested.creation) <= writeAndCreationTolerance);
	}
	if (requested.last_access.is_set())
	{
		CAPTURE(actual.last_access, requested.last_access);
		CHECK(secondsApart(actual.last_access, requested.last_access) <= lastAccessTolerance);
	}
	if (requested.last_write.is_set())
	{
		CAPTURE(actual.last_write, requested.last_write);
		CHECK(secondsApart(actual.last_write, requested.last_write) <= writeAndCreationTolerance);
	}
}

} // namespace test_volume
