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

enum class filesystem_kind {
	other,
	fat, // FAT12, FAT16 or FAT32
	exfat
};

[[nodiscard]] inline filesystem_kind kind() noexcept
{
	static const filesystem_kind detected = []() noexcept {
#ifdef _WIN32
		// By handle: a RAM disk the mount manager does not know has no volume path to query by.
		const HANDLE directory = ::CreateFileW(L".", 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (directory == INVALID_HANDLE_VALUE)
			return filesystem_kind::other;

		wchar_t name[MAX_PATH + 1]{};
		const bool queried = ::GetVolumeInformationByHandleW(directory, nullptr, 0, nullptr, nullptr, nullptr, name, MAX_PATH + 1) != 0;
		::CloseHandle(directory);
		const std::wstring_view type{ name };
		if (queried && type == L"exFAT")
			return filesystem_kind::exfat;
		return queried && type.starts_with(L"FAT") ? filesystem_kind::fat : filesystem_kind::other;
#elif defined(__linux__)
		struct statfs info;
		if (::statfs(".", &info) != 0)
			return filesystem_kind::other;
		if (info.f_type == 0x2011BAB0 /* EXFAT_SUPER_MAGIC */)
			return filesystem_kind::exfat;
		return info.f_type == 0x4d44 /* MSDOS_SUPER_MAGIC */ ? filesystem_kind::fat : filesystem_kind::other;
#else
		struct statfs info;
		if (::statfs(".", &info) != 0)
			return filesystem_kind::other;
		const std::string_view type{ info.f_fstypename };
		if (type == "exfat")
			return filesystem_kind::exfat;
		return type == "msdos" || type == "msdosfs" ? filesystem_kind::fat : filesystem_kind::other;
#endif
	}();
	return detected;
}

[[nodiscard]] inline bool isFatFamily() noexcept
{
	return kind() != filesystem_kind::other;
}

// The ...Unsupported() functions warn when they return true: the caller only has to return.

// Hard links, symbolic links and junctions.
[[nodiscard]] inline bool linksUnsupported()
{
	if (!isFatFamily())
		return false;

	WARN("This filesystem has no links: test skipped");
	return true;
}

// POSIX permission bits. The Windows attributes exist on every filesystem.
[[nodiscard]] inline bool storesPermissionBits() noexcept
{
#ifdef _WIN32
	return true;
#else
	return !isFatFamily();
#endif
}

[[nodiscard]] inline bool permissionBitsUnsupported()
{
	if (storesPermissionBits())
		return false;

	WARN("This filesystem has no permission bits: test skipped");
	return true;
}

[[nodiscard]] inline bool sparseFilesUnsupported()
{
	if (!isFatFamily())
		return false;

	WARN("This filesystem has no sparse files: test skipped");
	return true;
}

// The Linux exFAT driver has no fallocate.
[[nodiscard]] inline bool supportsPreallocation() noexcept
{
#ifdef __linux__
	return kind() != filesystem_kind::exfat;
#else
	return true;
#endif
}

// exFAT names are UTF-16: Linux refuses to create a name that is not valid UTF-8 there.
[[nodiscard]] inline bool nonUtf8NamesUnsupported()
{
	if (kind() != filesystem_kind::exfat)
		return false;

	WARN("This filesystem stores names as Unicode: test skipped");
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
//   FAT and exFAT - to 2 s;
//   FAT - no last access check: it keeps only the date, in local time, and macOS does not set a directory's;
//   everything else - exactly.
inline void checkStoredTimes(const thin_io::entry_times& actual, const thin_io::entry_times& requested)
{
	const double tolerance = isFatFamily() ? 2 : 0;

	if (requested.creation.is_set())
	{
		CAPTURE(actual.creation, requested.creation);
		CHECK(secondsApart(actual.creation, requested.creation) <= tolerance);
	}
	if (requested.last_access.is_set() && kind() != filesystem_kind::fat)
	{
		CAPTURE(actual.last_access, requested.last_access);
		CHECK(secondsApart(actual.last_access, requested.last_access) <= tolerance);
	}
	if (requested.last_write.is_set())
	{
		CAPTURE(actual.last_write, requested.last_write);
		CHECK(secondsApart(actual.last_write, requested.last_write) <= tolerance);
	}
}

} // namespace test_volume
