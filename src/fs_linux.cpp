#include "fs.hpp"
#include "timestamp_linux.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>    // AT_FDCWD
#include <string.h>
#include <sys/stat.h> // utimensat, UTIME_OMIT
#include <sys/statvfs.h>
#include <time.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sys/sysmacros.h>
#endif

#ifdef __APPLE__
#include <sys/attr.h> // setattrlist, ATTR_CMN_CRTIME
#include <unistd.h>
#endif

namespace thin_io {

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
	times.creation = statxBirthTime(AT_FDCWD, path, 0);
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

template<class Inode>
[[nodiscard]] entry_identity entryIdentity(const filesystem_identity filesystem, const Inode nativeInode) noexcept
{
	static_assert(sizeof(Inode) <= entry_identity{}.entry.size());

	entry_identity identity;
	identity.filesystem = filesystem;
	using unsigned_inode = std::make_unsigned_t<Inode>;
	const auto inode = static_cast<unsigned_inode>(nativeInode);
	for (size_t i = 0; i < sizeof(inode); ++i)
		identity.entry[i] = static_cast<uint8_t>(inode >> (i * 8));
	return identity;
}

[[nodiscard]] entry_identity identityFromStat(const struct stat& info) noexcept
{
	static_assert(sizeof(info.st_dev) <= sizeof(filesystem_identity));
	return entryIdentity(static_cast<filesystem_identity>(info.st_dev), info.st_ino);
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
		entries.push_back(directory_entry{ .name = nativeEntry->d_name, .attributes = *attributes, .logical_size = std::nullopt });
	}

	if (const auto closeError = directory.close()) [[unlikely]]
		return std::unexpected{*closeError};
	return entries;
}

filesystem_result<entry_metadata> get_entry_metadata(const char* const path, const link_behavior linkBehavior) noexcept
{
	if (path == nullptr) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = EINVAL }};

#if defined(__linux__) && defined(STATX_MNT_ID)
	int statxFlags = 0;
	switch (linkBehavior)
	{
	case link_behavior::follow:
		break;
	case link_behavior::do_not_follow:
		statxFlags = AT_SYMLINK_NOFOLLOW;
		break;
	default:
		return std::unexpected{filesystem_error{ .native_code = EINVAL }};
	}

	struct statx extendedInfo{};
	constexpr unsigned int requestedFields = STATX_BASIC_STATS | STATX_MNT_ID;
	if (::statx(AT_FDCWD, path, statxFlags, requestedFields, &extendedInfo) == 0)
	{
		if ((extendedInfo.stx_mask & STATX_BASIC_STATS) != STATX_BASIC_STATS) [[unlikely]]
			return std::unexpected{filesystem_error{ .native_code = EIO }};
		if (extendedInfo.stx_blocks > std::numeric_limits<uint64_t>::max() / 512) [[unlikely]]
			return std::unexpected{filesystem_error{ .native_code = EOVERFLOW }};

		static_assert(sizeof(dev_t) <= sizeof(filesystem_identity));
		const dev_t device = ::makedev(extendedInfo.stx_dev_major, extendedInfo.stx_dev_minor);
		entry_metadata metadata;
		metadata.attributes = attributesFromMode(static_cast<mode_t>(extendedInfo.stx_mode));
		metadata.logical_size = extendedInfo.stx_size;
		metadata.allocated_size = extendedInfo.stx_blocks * 512;
		metadata.hard_link_count = extendedInfo.stx_nlink;
		metadata.identity = entryIdentity(static_cast<filesystem_identity>(device), extendedInfo.stx_ino);
		metadata.mount_id = metadata.identity->filesystem;
		if ((extendedInfo.stx_mask & STATX_MNT_ID) != 0)
			metadata.mount_id = static_cast<mount_identity>(extendedInfo.stx_mnt_id);
		return metadata;
	}

	const filesystem_error statxError = capture_last_filesystem_error();
	if (statxError.native_code != ENOSYS && statxError.native_code != EINVAL)
		return std::unexpected{statxError};
#endif

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
	metadata.mount_id = metadata.identity->filesystem;
	return metadata;
}

filesystem_result<filesystem_space> get_filesystem_space(const char* const directoryPath) noexcept
{
	if (directoryPath == nullptr) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = EINVAL }};

	DIR* const nativeDirectory = ::opendir(directoryPath);
	if (nativeDirectory == nullptr) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	directory_handle directory{nativeDirectory};
	const int descriptor = ::dirfd(nativeDirectory);
	if (descriptor == -1) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};

	struct statvfs nativeSpace;
	if (::fstatvfs(descriptor, &nativeSpace) != 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};
	struct stat info;
	if (::fstat(descriptor, &info) != 0) [[unlikely]]
		return std::unexpected{capture_last_filesystem_error()};

	static_assert(sizeof(nativeSpace.f_frsize) <= sizeof(uint64_t));
	static_assert(sizeof(nativeSpace.f_blocks) <= sizeof(uint64_t));
	static_assert(sizeof(nativeSpace.f_bfree) <= sizeof(uint64_t));
	static_assert(sizeof(nativeSpace.f_bavail) <= sizeof(uint64_t));
	const uint64_t fragmentSize = static_cast<uint64_t>(nativeSpace.f_frsize);
	const uint64_t capacityBlocks = static_cast<uint64_t>(nativeSpace.f_blocks);
	const uint64_t freeBlocks = static_cast<uint64_t>(nativeSpace.f_bfree);
	const uint64_t availableBlocks = static_cast<uint64_t>(nativeSpace.f_bavail);
	const uint64_t largestBlockCount = std::max({capacityBlocks, freeBlocks, availableBlocks});
	if (fragmentSize == 0) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = EIO }};
	if (largestBlockCount > std::numeric_limits<uint64_t>::max() / fragmentSize) [[unlikely]]
		return std::unexpected{filesystem_error{ .native_code = EOVERFLOW }};

	filesystem_space space{
		.capacity = capacityBlocks * fragmentSize,
		.free = freeBlocks * fragmentSize,
		.available = availableBlocks * fragmentSize,
		.identity = identityFromStat(info).filesystem
	};
	if (const auto closeError = directory.close()) [[unlikely]]
		return std::unexpected{*closeError};
	return space;
}

} // namespace thin_io
