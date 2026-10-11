#include "catch_thin_io.hpp"
#include "test_volume.hpp"

#include "file.hpp"
#include "fs.hpp"

#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <winioctl.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace thin_io;

namespace {

#ifdef _WIN32
[[nodiscard]] bool createDirectory(const char* const path) { return ::CreateDirectoryA(path, nullptr) != 0; }
bool removeDirectory(const char* const path) { return ::RemoveDirectoryA(path) != 0; }
[[nodiscard]] bool createHardLink(const char* const linkPath, const char* const targetPath)
{
	return ::CreateHardLinkA(linkPath, targetPath, nullptr) != 0;
}
#else
[[nodiscard]] bool createDirectory(const char* const path) { return ::mkdir(path, 0755) == 0; }
bool removeDirectory(const char* const path) { return ::rmdir(path) == 0; }
[[nodiscard]] bool createHardLink(const char* const linkPath, const char* const targetPath)
{
	return ::link(targetPath, linkPath) == 0;
}
#endif

[[nodiscard]] bool createFileWithContents(const char* const path, const std::string_view contents)
{
	file created;
	if (!created.open(path, file::access_mode::Write, file::open_disposition::CreateOrTruncate))
		return false;

	const bool written = contents.empty()
		|| created.write(contents.data(), static_cast<uint64_t>(contents.size())) == static_cast<uint64_t>(contents.size());
	const bool closed = created.close();
	return written && closed;
}

static constexpr uint64_t sparseFileSize = 1024 * 1024 + 1;

[[nodiscard]] bool createSparseFile(const char* const path)
{
#ifdef _WIN32
	const HANDLE handle = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return false;

	DWORD ignored = 0;
	const bool markedSparse = ::DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ignored, nullptr) != 0;
	LARGE_INTEGER offset;
	offset.QuadPart = static_cast<LONGLONG>(sparseFileSize - 1);
	const char finalByte = 1;
	DWORD bytesWritten = 0;
	const bool written = markedSparse && ::SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN) != 0
		&& ::WriteFile(handle, &finalByte, 1, &bytesWritten, nullptr) != 0 && bytesWritten == 1;
	const bool closed = ::CloseHandle(handle) != 0;
	return written && closed;
#elif defined(__APPLE__)
	const int descriptor = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (descriptor == -1)
		return false;

	const bool resized = ::ftruncate(descriptor, static_cast<off_t>(sparseFileSize)) == 0;
	const char finalByte = 1;
	const bool written = resized && ::pwrite(descriptor, &finalByte, 1, static_cast<off_t>(sparseFileSize - 1)) == 1;
	fpunchhole_t hole{};
	hole.fp_offset = 0;
	hole.fp_length = static_cast<off_t>(sparseFileSize - 1);
	const bool punched = written && ::fcntl(descriptor, F_PUNCHHOLE, &hole) != -1;
	const bool closed = ::close(descriptor) == 0;
	return punched && closed;
#else
	file created;
	if (!created.open(path, file::access_mode::Write, file::open_disposition::CreateOrTruncate))
		return false;
	const char finalByte = 1;
	const bool written = created.pwrite(&finalByte, 1, sparseFileSize - 1) == 1;
	const bool closed = created.close();
	return written && closed;
#endif
}

#ifdef _WIN32
[[nodiscard]] bool createCompressedFile(const char* const path)
{
	const HANDLE handle = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return false;

	USHORT compressionFormat = COMPRESSION_FORMAT_DEFAULT;
	DWORD ignored = 0;
	const bool compressed = ::DeviceIoControl(handle, FSCTL_SET_COMPRESSION,
		&compressionFormat, static_cast<DWORD>(sizeof(compressionFormat)), nullptr, 0, &ignored, nullptr) != 0;
	std::vector<uint8_t> contents(static_cast<size_t>(sparseFileSize), 0);
	DWORD bytesWritten = 0;
	const bool written = compressed && ::WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()), &bytesWritten, nullptr) != 0
		&& bytesWritten == contents.size();
	const bool closed = ::CloseHandle(handle) != 0;
	return written && closed;
}
#endif

[[nodiscard]] bool createDenseFile(const char* const path)
{
	std::vector<uint8_t> contents(static_cast<size_t>(sparseFileSize));
	uint32_t state = 0x1234'5678u;
	for (uint8_t& byte : contents)
	{
		state = state * 1'664'525u + 1'013'904'223u;
		byte = static_cast<uint8_t>(state >> 24);
	}

	file created;
	if (!created.open(path, file::access_mode::Write, file::open_disposition::CreateOrTruncate))
		return false;
	const bool written = created.write(contents.data(), static_cast<uint64_t>(contents.size())) == static_cast<uint64_t>(contents.size());
	const bool closed = created.close();
	return written && closed;
}

} // namespace

TEST_CASE("get_entry_metadata reports regular files, empty files, and directories", "[fs][metadata]")
{
	static constexpr char directoryPath[] = "entry-metadata-basic";
	static constexpr char filePath[] = "entry-metadata-basic/file.bin";
	static constexpr char emptyFilePath[] = "entry-metadata-basic/empty.bin";
	static constexpr std::string_view contents = "metadata contents";
	file::delete_file(emptyFilePath);
	file::delete_file(filePath);
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createFileWithContents(filePath, contents));
	REQUIRE(createFileWithContents(emptyFilePath, {}));

	const auto regular = get_entry_metadata(filePath, link_behavior::do_not_follow);
	REQUIRE(regular);
	CHECK(regular->attributes.kind == entry_kind::regular_file);
	CHECK_FALSE(regular->attributes.is_link);
	CHECK(regular->logical_size == contents.size());
	CHECK(regular->hard_link_count >= 1);

	const auto empty = get_entry_metadata(emptyFilePath, link_behavior::follow);
	REQUIRE(empty);
	CHECK(empty->logical_size == 0);

	const auto directory = get_entry_metadata(directoryPath, link_behavior::do_not_follow);
	REQUIRE(directory);
	CHECK(directory->attributes.kind == entry_kind::directory);
	CHECK_FALSE(directory->attributes.is_link);
#ifndef _WIN32
	REQUIRE(regular->mount_id);
	REQUIRE(directory->mount_id);
	CHECK(regular->mount_id == directory->mount_id);
#endif

	REQUIRE(file::delete_file(emptyFilePath));
	REQUIRE(file::delete_file(filePath));
	REQUIRE(removeDirectory(directoryPath));
}

TEST_CASE("get_entry_metadata identifies hard links", "[fs][metadata][link]")
{
	if (test_volume::linksUnsupported())
		return;

	static constexpr char targetPath[] = "entry-metadata-hard-link-target.file";
	static constexpr char linkPath[] = "entry-metadata-hard-link.file";
	file::delete_file(linkPath);
	file::delete_file(targetPath);
	REQUIRE(createFileWithContents(targetPath, "hard link contents"));
	REQUIRE(createHardLink(linkPath, targetPath));

	const auto target = get_entry_metadata(targetPath, link_behavior::do_not_follow);
	const auto link = get_entry_metadata(linkPath, link_behavior::do_not_follow);
	REQUIRE(target);
	REQUIRE(link);
	if (!target->identity || !link->identity)
		WARN("The test filesystem does not expose stable entry identity");
	else
		CHECK(*target->identity == *link->identity);
	CHECK(target->hard_link_count >= 2);
	CHECK(link->hard_link_count == target->hard_link_count);

	REQUIRE(file::delete_file(linkPath));
	REQUIRE(file::delete_file(targetPath));
}

TEST_CASE("get_entry_metadata distinguishes sparse allocation from logical size", "[fs][metadata][sparse]")
{
	static constexpr char sparsePath[] = "entry-metadata-sparse.file";
	static constexpr char densePath[] = "entry-metadata-dense.file";
	file::delete_file(sparsePath);
	file::delete_file(densePath);
#ifdef _WIN32
	if (!createSparseFile(sparsePath))
	{
		WARN("The test filesystem does not support creating sparse files");
		file::delete_file(sparsePath);
		return;
	}
#else
	REQUIRE(createSparseFile(sparsePath));
#endif
	REQUIRE(createDenseFile(densePath));

	const auto sparse = get_entry_metadata(sparsePath, link_behavior::do_not_follow);
	const auto dense = get_entry_metadata(densePath, link_behavior::do_not_follow);
	REQUIRE(sparse);
	REQUIRE(dense);
	CHECK(sparse->logical_size == sparseFileSize);
	CHECK(dense->logical_size == sparseFileSize);
	CHECK(sparse->allocated_size < sparse->logical_size);
	CHECK(dense->allocated_size > sparse->allocated_size);
#ifdef _WIN32
	CHECK(sparse->attributes.sparse);
#endif

	REQUIRE(file::delete_file(sparsePath));
	REQUIRE(file::delete_file(densePath));
}

#ifdef _WIN32
TEST_CASE("Windows metadata reports compressed allocation", "[fs][metadata][windows][compression]")
{
	std::string compressedPath = "entry-metadata-compressed.file";
	file::delete_file(compressedPath.c_str());
	if (!createCompressedFile(compressedPath.c_str()))
	{
		file::delete_file(compressedPath.c_str());

		// The working directory may sit on a filesystem without NTFS compression (e. g. ReFS on CI); retry in %TEMP%
		char tempDirectory[MAX_PATH];
		const DWORD tempLength = ::GetTempPathA(MAX_PATH, tempDirectory);
		REQUIRE(tempLength > 0);
		REQUIRE(tempLength < MAX_PATH);
		compressedPath = std::string{tempDirectory, tempLength} + "entry-metadata-compressed.file";
		file::delete_file(compressedPath.c_str());
		if (!createCompressedFile(compressedPath.c_str()))
		{
			WARN("No available filesystem supports creating compressed files");
			file::delete_file(compressedPath.c_str());
			return;
		}
	}

	const auto compressed = get_entry_metadata(compressedPath.c_str(), link_behavior::do_not_follow);
	REQUIRE(compressed);
	CHECK(compressed->attributes.compressed);
	CHECK(compressed->logical_size == sparseFileSize);
	CHECK(compressed->allocated_size < compressed->logical_size);

	REQUIRE(file::delete_file(compressedPath.c_str()));
}
#endif

TEST_CASE("get_entry_metadata fails after an enumerated entry is deleted", "[fs][metadata][directory]")
{
	static constexpr char directoryPath[] = "entry-metadata-deleted";
	static constexpr char filePath[] = "entry-metadata-deleted/file";
	file::delete_file(filePath);
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createFileWithContents(filePath, {}));
	const auto listed = list_directory(directoryPath);
	REQUIRE(listed);
	REQUIRE(listed->size() == 1);
	REQUIRE(file::delete_file(filePath));

	const auto metadata = get_entry_metadata(filePath, link_behavior::do_not_follow);
	REQUIRE_FALSE(metadata);
	CHECK(metadata.error().native_code != 0);
	REQUIRE(removeDirectory(directoryPath));
}

TEST_CASE("get_entry_metadata rejects invalid input", "[fs][metadata]")
{
	const auto nullPath = get_entry_metadata(static_cast<const char*>(nullptr), link_behavior::follow);
	REQUIRE_FALSE(nullPath);
#ifdef _WIN32
	CHECK(nullPath.error().native_code == ERROR_INVALID_PARAMETER);
#else
	CHECK(nullPath.error().native_code == EINVAL);
#endif

	const auto invalidBehavior = get_entry_metadata(".", static_cast<link_behavior>(255));
	REQUIRE_FALSE(invalidBehavior);
#ifdef _WIN32
	CHECK(invalidBehavior.error().native_code == ERROR_INVALID_PARAMETER);
#else
	CHECK(invalidBehavior.error().native_code == EINVAL);
#endif
}

TEST_CASE("set_permissions by path round-trips through get_entry_metadata", "[fs][metadata][permissions]")
{
	static constexpr char directoryPath[] = "entry-permissions";
	static constexpr char filePath[] = "entry-permissions/file.bin";
#ifdef _WIN32
	static constexpr file_permissions changed{ .read_only = true, .hidden = true, .system = true };
	static constexpr file_permissions original{};
	static constexpr file_permissions changedDirectory{ .hidden = true };
#else
	static constexpr file_permissions changed{ .mode = 0440 };
	static constexpr file_permissions original{ .mode = 0644 };
	static constexpr file_permissions changedDirectory{ .mode = 0750 };
#endif
	(void)set_permissions(filePath, original, link_behavior::do_not_follow); // A failed earlier run may have left it read-only
	file::delete_file(filePath);
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createFileWithContents(filePath, "contents"));

	REQUIRE(set_permissions(filePath, changed, link_behavior::do_not_follow));
	const auto afterChange = get_entry_metadata(filePath, link_behavior::do_not_follow);
	REQUIRE(afterChange);
	CHECK(afterChange->permissions == changed);
#ifdef _WIN32
	CHECK((::GetFileAttributesA(filePath) & FILE_ATTRIBUTE_ARCHIVE) != 0); // Attributes outside file_permissions survive
#endif

	REQUIRE(set_permissions(filePath, original, link_behavior::follow));
	const auto afterRestore = get_entry_metadata(filePath, link_behavior::do_not_follow);
	REQUIRE(afterRestore);
	CHECK(afterRestore->permissions == original);

	REQUIRE(set_permissions(directoryPath, changedDirectory, link_behavior::do_not_follow));
	const auto directory = get_entry_metadata(directoryPath, link_behavior::do_not_follow);
	REQUIRE(directory);
	CHECK(directory->permissions == changedDirectory);

	REQUIRE(file::delete_file(filePath));
	REQUIRE(removeDirectory(directoryPath));
}

TEST_CASE("get_entry_status reports a file, a directory, and a missing entry", "[fs][status]")
{
	static constexpr char directoryPath[] = "entry-status";
	static constexpr char filePath[] = "entry-status/file.bin";
	static constexpr std::string_view contents = "status contents";
	file::delete_file(filePath);
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createFileWithContents(filePath, contents));

	const auto regular = get_entry_status(filePath, link_behavior::do_not_follow);
	REQUIRE(regular);
	CHECK(regular->attributes.kind == entry_kind::regular_file);
	CHECK_FALSE(regular->attributes.is_link);
	CHECK(regular->logical_size == contents.size());
	CHECK(regular->times.last_write.is_set());
	REQUIRE(regular->permissions);
	const auto metadata = get_entry_metadata(filePath, link_behavior::do_not_follow);
	REQUIRE(metadata);
	CHECK(*regular->permissions == metadata->permissions);
	CHECK(get_entry_status(filePath, link_behavior::follow) == regular);

	const auto directory = get_entry_status(directoryPath, link_behavior::do_not_follow);
	REQUIRE(directory);
	CHECK(directory->attributes.kind == entry_kind::directory);
	CHECK_FALSE(directory->logical_size);

	REQUIRE(file::delete_file(filePath));
	const auto missing = get_entry_status(filePath, link_behavior::do_not_follow);
	REQUIRE_FALSE(missing);
#ifdef _WIN32
	CHECK(missing.error().native_code == ERROR_FILE_NOT_FOUND);
#else
	CHECK(missing.error().native_code == ENOENT);
#endif
	REQUIRE(removeDirectory(directoryPath));
}

TEST_CASE("get_entry_status and set_permissions reject invalid input", "[fs][status][permissions]")
{
#ifdef _WIN32
	static constexpr filesystem_error_code invalidArgument = ERROR_INVALID_PARAMETER;
#else
	static constexpr filesystem_error_code invalidArgument = EINVAL;
#endif
	const auto nullPath = set_permissions(static_cast<const char*>(nullptr), file_permissions{}, link_behavior::follow);
	REQUIRE_FALSE(nullPath);
	CHECK(nullPath.error().native_code == invalidArgument);

	const auto invalidBehavior = set_permissions(".", file_permissions{}, static_cast<link_behavior>(255));
	REQUIRE_FALSE(invalidBehavior);
	CHECK(invalidBehavior.error().native_code == invalidArgument);

	const auto nullStatusPath = get_entry_status(static_cast<const char*>(nullptr), link_behavior::follow);
	REQUIRE_FALSE(nullStatusPath);
	CHECK(nullStatusPath.error().native_code == invalidArgument);

	const auto invalidStatusBehavior = get_entry_status(".", static_cast<link_behavior>(255));
	REQUIRE_FALSE(invalidStatusBehavior);
	CHECK(invalidStatusBehavior.error().native_code == invalidArgument);
}

#ifndef _WIN32
TEST_CASE("POSIX set_permissions reaches a symbolic link's target only when following", "[fs][metadata][permissions][link]")
{
	if (test_volume::linksUnsupported())
		return;

	static constexpr char targetPath[] = "entry-permissions-symlink-target.file";
	static constexpr char linkPath[] = "entry-permissions-symlink.file";
	::unlink(linkPath);
	file::delete_file(targetPath);
	REQUIRE(createFileWithContents(targetPath, "target"));
	REQUIRE(::symlink(targetPath, linkPath) == 0);

	REQUIRE(set_permissions(linkPath, file_permissions{ .mode = 0600 }, link_behavior::follow));
	const auto followed = get_entry_metadata(targetPath, link_behavior::do_not_follow);
	REQUIRE(followed);
	CHECK(followed->permissions.mode == 0600);

	const auto notFollowed = set_permissions(linkPath, file_permissions{ .mode = 0640 }, link_behavior::do_not_follow);
#ifdef __linux__
	REQUIRE_FALSE(notFollowed);
	CHECK(notFollowed.error().native_code == EOPNOTSUPP);
#else
	CHECK(notFollowed);
#endif
	const auto target = get_entry_metadata(targetPath, link_behavior::do_not_follow);
	REQUIRE(target);
	CHECK(target->permissions.mode == 0600);

	REQUIRE(::unlink(linkPath) == 0);
	REQUIRE(file::delete_file(targetPath));
}

TEST_CASE("POSIX metadata explicitly follows or inspects symbolic links", "[fs][metadata][link]")
{
	if (test_volume::linksUnsupported())
		return;

	static constexpr char targetPath[] = "entry-metadata-symlink-target.file";
	static constexpr char linkPath[] = "entry-metadata-symlink.file";
	static constexpr char targetName[] = "entry-metadata-symlink-target.file";
	file::delete_file(linkPath);
	file::delete_file(targetPath);
	REQUIRE(createFileWithContents(targetPath, "target"));
	REQUIRE(::symlink(targetName, linkPath) == 0);

	const auto target = get_entry_metadata(targetPath, link_behavior::do_not_follow);
	const auto link = get_entry_metadata(linkPath, link_behavior::do_not_follow);
	const auto followed = get_entry_metadata(linkPath, link_behavior::follow);
	REQUIRE(target);
	REQUIRE(link);
	REQUIRE(followed);
	CHECK(link->attributes.is_link);
	CHECK(link->attributes.kind == entry_kind::other);
	CHECK(link->logical_size == std::string_view{targetName}.size());
	CHECK_FALSE(followed->attributes.is_link);
	CHECK(followed->attributes.kind == entry_kind::regular_file);

	const auto linkStatus = get_entry_status(linkPath, link_behavior::do_not_follow);
	const auto followedStatus = get_entry_status(linkPath, link_behavior::follow);
	REQUIRE(linkStatus);
	REQUIRE(followedStatus);
	CHECK(linkStatus->attributes == link->attributes);
	CHECK(followedStatus->attributes == followed->attributes);
	CHECK(followedStatus->logical_size == followed->logical_size);

	if (!target->identity || !link->identity || !followed->identity)
		WARN("The test filesystem does not expose stable entry identity");
	else
	{
		CHECK(followed->identity == target->identity);
		CHECK(link->identity != target->identity);
	}

	REQUIRE(file::delete_file(linkPath));
	REQUIRE(file::delete_file(targetPath));
}

TEST_CASE("POSIX metadata for a dangling symbolic link", "[fs][metadata][link]")
{
	if (test_volume::linksUnsupported())
		return;

	static constexpr char linkPath[] = "entry-metadata-dangling.link";
	::unlink(linkPath);
	REQUIRE(::symlink("no-such-target-entry", linkPath) == 0);

	const auto inspected = get_entry_metadata(linkPath, link_behavior::do_not_follow);
	REQUIRE(inspected);
	CHECK(inspected->attributes.is_link);

	const auto followed = get_entry_metadata(linkPath, link_behavior::follow);
	REQUIRE_FALSE(followed);
	CHECK(followed.error().native_code == ENOENT);

	REQUIRE(::unlink(linkPath) == 0);
}

TEST_CASE("POSIX metadata retains an inaccessible-path error when permissions can be enforced", "[fs][metadata]")
{
	static constexpr char directoryPath[] = "entry-metadata-inaccessible";
	static constexpr char filePath[] = "entry-metadata-inaccessible/file";
	file::delete_file(filePath);
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createFileWithContents(filePath, {}));
	REQUIRE(::chmod(directoryPath, 0) == 0);

	const auto metadata = get_entry_metadata(filePath, link_behavior::do_not_follow);
	REQUIRE(::chmod(directoryPath, 0700) == 0);
	if (metadata)
		WARN("The test process can bypass directory permissions; inaccessible-metadata assertion skipped");
	else
		CHECK((metadata.error().native_code == EACCES || metadata.error().native_code == EPERM));

	REQUIRE(file::delete_file(filePath));
	REQUIRE(removeDirectory(directoryPath));
}
#else
TEST_CASE("Windows metadata explicitly follows or inspects reparse points", "[fs][metadata][windows][link]")
{
	static constexpr char targetPath[] = "entry-metadata-reparse-target.file";
	static constexpr char linkPath[] = "entry-metadata-reparse.file";
	file::delete_file(linkPath);
	file::delete_file(targetPath);
	REQUIRE(createFileWithContents(targetPath, "target"));
	static constexpr DWORD allowUnprivilegedCreate = 0x2;
	if (::CreateSymbolicLinkA(linkPath, targetPath, allowUnprivilegedCreate) == 0)
	{
		WARN("Symbolic-link creation is unavailable; Windows reparse metadata assertion skipped");
		REQUIRE(file::delete_file(targetPath));
		return;
	}

	const auto target = get_entry_metadata(targetPath, link_behavior::do_not_follow);
	const auto link = get_entry_metadata(linkPath, link_behavior::do_not_follow);
	const auto followed = get_entry_metadata(linkPath, link_behavior::follow);
	REQUIRE(target);
	REQUIRE(link);
	REQUIRE(followed);
	CHECK(link->attributes.is_link);
	CHECK(link->attributes.reparse_tag == IO_REPARSE_TAG_SYMLINK);
	CHECK_FALSE(followed->attributes.is_link);

	const auto linkStatus = get_entry_status(linkPath, link_behavior::do_not_follow);
	const auto followedStatus = get_entry_status(linkPath, link_behavior::follow);
	REQUIRE(linkStatus);
	REQUIRE(followedStatus);
	CHECK(linkStatus->attributes == link->attributes);
	CHECK_FALSE(linkStatus->logical_size);
	CHECK(followedStatus->attributes == followed->attributes);
	CHECK(followedStatus->logical_size == followed->logical_size);

	if (!target->identity || !link->identity || !followed->identity)
		WARN("The test filesystem does not expose stable entry identity");
	else
	{
		CHECK(followed->identity == target->identity);
		CHECK(link->identity != target->identity);
	}

	REQUIRE(file::delete_file(linkPath));
	REQUIRE(file::delete_file(targetPath));
}

TEST_CASE("Windows set_permissions reaches a reparse point's target only when following", "[fs][metadata][permissions][windows][link]")
{
	static constexpr char targetPath[] = "entry-permissions-reparse-target.file";
	static constexpr char linkPath[] = "entry-permissions-reparse.file";
	file::delete_file(linkPath);
	file::delete_file(targetPath);
	REQUIRE(createFileWithContents(targetPath, "target"));
	static constexpr DWORD allowUnprivilegedCreate = 0x2;
	if (::CreateSymbolicLinkA(linkPath, targetPath, allowUnprivilegedCreate) == 0)
	{
		WARN("Symbolic-link creation is unavailable; Windows reparse permissions assertion skipped");
		REQUIRE(file::delete_file(targetPath));
		return;
	}

	REQUIRE(set_permissions(linkPath, file_permissions{ .hidden = true }, link_behavior::do_not_follow));
	auto link = get_entry_metadata(linkPath, link_behavior::do_not_follow);
	auto target = get_entry_metadata(targetPath, link_behavior::do_not_follow);
	REQUIRE(link);
	REQUIRE(target);
	CHECK(link->attributes.is_link);
	CHECK(link->permissions == file_permissions{ .hidden = true });
	CHECK(target->permissions == file_permissions{});

	REQUIRE(set_permissions(linkPath, file_permissions{ .system = true }, link_behavior::follow));
	link = get_entry_metadata(linkPath, link_behavior::do_not_follow);
	target = get_entry_metadata(targetPath, link_behavior::do_not_follow);
	REQUIRE(link);
	REQUIRE(target);
	CHECK(link->permissions == file_permissions{ .hidden = true });
	CHECK(target->permissions == file_permissions{ .system = true });

	REQUIRE(file::delete_file(linkPath));
	REQUIRE(file::delete_file(targetPath));
}

TEST_CASE("Windows metadata tolerates a handle that denies data sharing", "[fs][metadata][windows]")
{
	static constexpr char filePath[] = "entry-metadata-sharing.file";
	file::delete_file(filePath);
	REQUIRE(createFileWithContents(filePath, {}));
	const HANDLE exclusive = ::CreateFileA(filePath, GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	REQUIRE(exclusive != INVALID_HANDLE_VALUE);

	const auto metadata = get_entry_metadata(filePath, link_behavior::do_not_follow);
	REQUIRE(metadata);
	CHECK(metadata->attributes.kind == entry_kind::regular_file);
	REQUIRE(::CloseHandle(exclusive) != 0);
	REQUIRE(file::delete_file(filePath));
}

TEST_CASE("Windows metadata wide and UTF-8 overloads address the same Unicode entry", "[fs][metadata][windows]")
{
	static constexpr wchar_t nativePath[] = L"entry-metadata-\u0444\u0430\u0439\u043B.file";
	static constexpr auto utf8Path = u8"entry-metadata-\u0444\u0430\u0439\u043B.file";
	const char* const narrowPath = reinterpret_cast<const char*>(utf8Path);
	file::delete_file(nativePath);
	file created;
	REQUIRE(created.open(nativePath, file::access_mode::Write, file::open_disposition::CreateNew));
	REQUIRE(created.close());

	const auto native = get_entry_metadata(nativePath, link_behavior::do_not_follow);
	const auto utf8 = get_entry_metadata(narrowPath, link_behavior::do_not_follow);
	REQUIRE(native);
	REQUIRE(utf8);
	CHECK(*native == *utf8);

	REQUIRE(file::delete_file(nativePath));
}
#endif
