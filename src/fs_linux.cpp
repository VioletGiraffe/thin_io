#include "fs.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>    // AT_FDCWD
#include <string.h>
#include <sys/stat.h> // utimensat, UTIME_OMIT
#include <time.h>

#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

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

namespace {

class directory_handle final {
public:
	explicit directory_handle(DIR* const directory) noexcept : _directory{directory} {}
	~directory_handle() noexcept
	{
		if (_directory != nullptr)
			::closedir(_directory);
	}

	directory_handle(const directory_handle&) = delete;
	directory_handle& operator=(const directory_handle&) = delete;

	[[nodiscard]] std::optional<filesystem_error> close() noexcept
	{
		if (::closedir(std::exchange(_directory, nullptr)) == 0)
			return {};

		return capture_last_filesystem_error();
	}

private:
	DIR* _directory;
};

[[nodiscard]] entry_attributes attributesFromMode(const mode_t mode) noexcept
{
	entry_attributes attributes;
	if (S_ISREG(mode))
		attributes.kind = entry_kind::regular_file;
	else if (S_ISDIR(mode))
		attributes.kind = entry_kind::directory;
	else
		attributes.kind = entry_kind::other;
	attributes.is_link = S_ISLNK(mode);
	return attributes;
}

[[nodiscard]] entry_identity identityFromStat(const struct stat& info) noexcept
{
	static_assert(sizeof(info.st_dev) <= sizeof(filesystem_identity));
	static_assert(sizeof(info.st_ino) <= entry_identity{}.entry.size());

	entry_identity identity;
	identity.filesystem = static_cast<filesystem_identity>(info.st_dev);
	using unsigned_inode = std::make_unsigned_t<decltype(info.st_ino)>;
	const auto inode = static_cast<unsigned_inode>(info.st_ino);
	for (size_t i = 0; i < sizeof(inode); ++i)
		identity.entry[i] = static_cast<uint8_t>(inode >> (i * 8));
	return identity;
}

[[nodiscard]] filesystem_result<entry_attributes> attributesFromDirectoryEntry(DIR* const directory, const dirent& entry)
{
	switch (entry.d_type)
	{
	case DT_REG:
		return entry_attributes{ .kind = entry_kind::regular_file };
	case DT_DIR:
		return entry_attributes{ .kind = entry_kind::directory };
	case DT_LNK:
		return entry_attributes{ .kind = entry_kind::other, .is_link = true };
	case DT_UNKNOWN:
	{
		struct stat info;
		if (::fstatat(::dirfd(directory), entry.d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) [[unlikely]]
			return std::unexpected{capture_last_filesystem_error()};
		return attributesFromMode(info.st_mode);
	}
	default:
		return entry_attributes{ .kind = entry_kind::other };
	}
}

[[nodiscard]] bool isDotEntry(const char* const name) noexcept
{
	return ::strcmp(name, ".") == 0 || ::strcmp(name, "..") == 0;
}

} // namespace

filesystem_result<std::vector<directory_entry>> list_directory(const char* const path)
{
	if (path == nullptr) [[unlikely]]
	{
		errno = EINVAL;
		return std::unexpected{capture_last_filesystem_error()};
	}

	DIR* const nativeDirectory = ::opendir(path);
	if (nativeDirectory == nullptr) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};

	directory_handle directory{nativeDirectory};
	std::vector<directory_entry> entries;
	for (;;)
	{
		errno = 0;
		const dirent* const nativeEntry = ::readdir(nativeDirectory);
		if (nativeEntry == nullptr)
		{
			if (errno != 0) [[unlikely]]
				return std::unexpected{capture_last_filesystem_error()};
			break;
		}
		if (isDotEntry(nativeEntry->d_name))
			continue;

		auto attributes = attributesFromDirectoryEntry(nativeDirectory, *nativeEntry);
		if (!attributes) [[unlikely]]
			return std::unexpected{attributes.error()};
		entries.push_back(directory_entry{ .name = nativeEntry->d_name, .attributes = *attributes });
	}

	if (const auto closeError = directory.close()) [[unlikely]]
		return std::unexpected{*closeError};
	return entries;
}

filesystem_result<entry_metadata> get_entry_metadata(const char* const path, const link_behavior linkBehavior) noexcept
{
	if (path == nullptr) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = EINVAL }};

	struct stat info;
	int queryResult = 0;
	switch (linkBehavior)
	{
	case link_behavior::follow:
		queryResult = ::stat(path, &info);
		break;
	case link_behavior::do_not_follow:
		queryResult = ::lstat(path, &info);
		break;
	default:
		return std::unexpected{filesystem_error{ .native_code = EINVAL }};
	}
	if (queryResult != 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	if (info.st_size < 0 || info.st_blocks < 0) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = EOVERFLOW }};

	const uint64_t allocatedBlocks = static_cast<uint64_t>(info.st_blocks);
	if (allocatedBlocks > std::numeric_limits<uint64_t>::max() / 512) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = EOVERFLOW }};

	entry_metadata metadata;
	metadata.attributes = attributesFromMode(info.st_mode);
	metadata.logical_size = static_cast<uint64_t>(info.st_size);
	metadata.allocated_size = allocatedBlocks * 512;
	metadata.hard_link_count = static_cast<uint64_t>(info.st_nlink);
	metadata.identity = identityFromStat(info);
	return metadata;
}

} // namespace thin_io
