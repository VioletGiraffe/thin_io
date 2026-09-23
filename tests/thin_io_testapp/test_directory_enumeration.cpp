#include "catch_thin_io.hpp"

#include "file.hpp"
#include "fs.hpp"

#include <algorithm>
#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include "windows_path_win.hpp"

#include <Windows.h>
#include <AclAPI.h>
#include <winioctl.h>

#pragma comment(lib, "advapi32.lib") // Security descriptor APIs

#include <array>
#include <cstddef>
#else
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace thin_io;

namespace {

using native_string_view = std::basic_string_view<native_char>;

#ifdef _WIN32
[[nodiscard]] bool createDirectory(const char* const path) { return ::CreateDirectoryA(path, nullptr) != 0; }
bool removeDirectory(const char* const path) { return ::RemoveDirectoryA(path) != 0; }
[[nodiscard]] native_string_view nativeName(const wchar_t* const name) { return name; }
#else
[[nodiscard]] bool createDirectory(const char* const path) { return ::mkdir(path, 0755) == 0; }
bool removeDirectory(const char* const path) { return ::rmdir(path) == 0; }
[[nodiscard]] native_string_view nativeName(const char* const name) { return name; }
#endif

// Test names are ASCII, so widening each character converts them losslessly.
[[nodiscard]] native_string asciiName(const std::string_view name) { return native_string(name.begin(), name.end()); }

[[nodiscard]] const directory_entry* findEntry(const std::vector<directory_entry>& entries, const native_string_view name)
{
	const auto found = std::find_if(entries.begin(), entries.end(), [name](const directory_entry& entry) { return entry.name == name; });
	return found == entries.end() ? nullptr : &*found;
}

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

} // namespace

TEST_CASE("list_directory distinguishes an empty directory from failure", "[fs][directory]")
{
	static constexpr char directoryPath[] = "list-directory-empty";
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));

	const auto empty = list_directory(directoryPath);
	REQUIRE(empty);
	CHECK(empty->empty());

	REQUIRE(removeDirectory(directoryPath));
	const auto missing = list_directory(directoryPath);
	REQUIRE_FALSE(missing);
#ifdef _WIN32
	CHECK((missing.error().native_code == ERROR_FILE_NOT_FOUND || missing.error().native_code == ERROR_PATH_NOT_FOUND));
#else
	CHECK(missing.error().native_code == ENOENT);
#endif
}

TEST_CASE("list_directory returns one directory level with native attributes and sizes", "[fs][directory]")
{
	static constexpr char directoryPath[] = "list-directory-mixed";
	static constexpr char childDirectoryPath[] = "list-directory-mixed/child";
	static constexpr char filePath[] = "list-directory-mixed/file.bin";
	static constexpr char hiddenPath[] = "list-directory-mixed/.hidden";
	static constexpr std::string_view contents = "enumerated contents";
	file::delete_file(hiddenPath);
	file::delete_file(filePath);
	removeDirectory(childDirectoryPath);
	removeDirectory(directoryPath);

	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createDirectory(childDirectoryPath));
	REQUIRE(createFileWithContents(filePath, contents));
	REQUIRE(createFileWithContents(hiddenPath, {}));
#ifdef _WIN32
	REQUIRE(::SetFileAttributesA(hiddenPath, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) != 0);
#endif

	const auto listed = list_directory(directoryPath);
	REQUIRE(listed);
	REQUIRE(listed->size() == 3);
#ifdef _WIN32
	const directory_entry* const child = findEntry(*listed, nativeName(L"child"));
	const directory_entry* const regular = findEntry(*listed, nativeName(L"file.bin"));
	const directory_entry* const hidden = findEntry(*listed, nativeName(L".hidden"));
	CHECK(findEntry(*listed, nativeName(L".")) == nullptr);
	CHECK(findEntry(*listed, nativeName(L"..")) == nullptr);
#else
	const directory_entry* const child = findEntry(*listed, nativeName("child"));
	const directory_entry* const regular = findEntry(*listed, nativeName("file.bin"));
	const directory_entry* const hidden = findEntry(*listed, nativeName(".hidden"));
	CHECK(findEntry(*listed, nativeName(".")) == nullptr);
	CHECK(findEntry(*listed, nativeName("..")) == nullptr);
#endif
	REQUIRE(child != nullptr);
	CHECK(child->attributes.kind == entry_kind::directory);
	CHECK_FALSE(child->attributes.is_link);
	CHECK_FALSE(child->logical_size);
	REQUIRE(regular != nullptr);
	CHECK(regular->attributes.kind == entry_kind::regular_file);
	CHECK_FALSE(regular->attributes.is_link);
#ifdef _WIN32
	REQUIRE(regular->logical_size);
	CHECK(*regular->logical_size == contents.size());
#else
	CHECK_FALSE(regular->logical_size);
#endif
	REQUIRE(hidden != nullptr);
	CHECK_FALSE(regular->link_target);
#ifdef _WIN32
	// The find data carries these, so even a basic listing reports them
	CHECK(regular->times.last_write.is_set());
	REQUIRE(regular->permissions);
	CHECK_FALSE(regular->permissions->hidden);
	CHECK_FALSE(regular->attributes.hidden);
	CHECK(hidden->attributes.hidden);
	REQUIRE(hidden->permissions);
	CHECK(hidden->permissions->hidden);
	CHECK(hidden->permissions->system);
	const auto hiddenMetadata = get_entry_metadata(hiddenPath, link_behavior::do_not_follow);
	REQUIRE(hiddenMetadata);
	CHECK(hiddenMetadata->attributes.hidden);
#else
	CHECK_FALSE(regular->times.last_write.is_set());
	CHECK_FALSE(regular->permissions);
	CHECK_FALSE(hidden->attributes.hidden); // A leading dot is a naming convention, not an attribute
#endif

#ifdef _WIN32
	REQUIRE(::SetFileAttributesA(hiddenPath, FILE_ATTRIBUTE_NORMAL) != 0);
#endif
	REQUIRE(file::delete_file(hiddenPath));
	REQUIRE(file::delete_file(filePath));
	REQUIRE(removeDirectory(childDirectoryPath));
	REQUIRE(removeDirectory(directoryPath));
}

TEST_CASE("list_directory captures invalid input and non-directory failures", "[fs][directory]")
{
	const auto nullPath = list_directory(static_cast<const char*>(nullptr));
	REQUIRE_FALSE(nullPath);
#ifdef _WIN32
	CHECK(nullPath.error().native_code == ERROR_INVALID_PARAMETER);
#else
	CHECK(nullPath.error().native_code == EINVAL);
#endif

	static constexpr char filePath[] = "list-directory-not-a-directory.file";
	file::delete_file(filePath);
	REQUIRE(createFileWithContents(filePath, {}));
	const auto fileResult = list_directory(filePath);
	REQUIRE_FALSE(fileResult);
	CHECK(fileResult.error().native_code != 0);
	REQUIRE(file::delete_file(filePath));
}

TEST_CASE("list_directory with full detail reports sizes, times and permissions", "[fs][directory]")
{
	static constexpr char directoryPath[] = "list-directory-full";
	static constexpr char childDirectoryPath[] = "list-directory-full/child";
	static constexpr char filePath[] = "list-directory-full/file.bin";
	static constexpr std::string_view contents = "detailed contents";
	file::delete_file(filePath);
	removeDirectory(childDirectoryPath);
	removeDirectory(directoryPath);

	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createDirectory(childDirectoryPath));
	REQUIRE(createFileWithContents(filePath, contents));
#ifndef _WIN32
	REQUIRE(::chmod(filePath, 0640) == 0);
#endif

	const auto listed = list_directory(directoryPath, listing_detail::full);
	REQUIRE(listed);
	REQUIRE(listed->size() == 2);
	const directory_entry* const regular = findEntry(*listed, asciiName("file.bin"));
	const directory_entry* const child = findEntry(*listed, asciiName("child"));

	REQUIRE(regular != nullptr);
	CHECK(regular->attributes.kind == entry_kind::regular_file);
	REQUIRE(regular->logical_size);
	CHECK(*regular->logical_size == contents.size());
	const auto times = get_times(filePath);
	REQUIRE(times);
	REQUIRE(regular->times.last_write.is_set());
	CHECK(regular->times.last_write == times->last_write);
	CHECK(regular->times.creation == times->creation);
	CHECK(regular->times.last_access.is_set());
	REQUIRE(regular->permissions);
#ifdef _WIN32
	CHECK_FALSE(regular->permissions->read_only);
#else
	CHECK(regular->permissions->mode == 0640);
#endif
	CHECK_FALSE(regular->link_target);

	REQUIRE(child != nullptr);
	CHECK(child->attributes.kind == entry_kind::directory);
	CHECK_FALSE(child->logical_size);
	CHECK(child->times.last_write.is_set());
	CHECK_FALSE(child->link_target);

	REQUIRE(file::delete_file(filePath));
	REQUIRE(removeDirectory(childDirectoryPath));
	REQUIRE(removeDirectory(directoryPath));
}

TEST_CASE("get_directory_entry reports one entry as a full listing does", "[fs][directory]")
{
	static constexpr char directoryPath[] = "directory-entry-single";
	static constexpr char filePath[] = "directory-entry-single/file.bin";
	file::delete_file(filePath);
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createFileWithContents(filePath, "single entry"));

	const auto listed = list_directory(directoryPath, listing_detail::full);
	REQUIRE(listed);
	REQUIRE(listed->size() == 1);
	const auto single = get_directory_entry(filePath);
	REQUIRE(single);
	CHECK(*single == listed->front());

	// A trailing separator is ignored
	const auto directory = get_directory_entry("directory-entry-single/");
	REQUIRE(directory);
	CHECK(directory->attributes.kind == entry_kind::directory);
	CHECK(directory->name == asciiName(directoryPath));

	const auto missing = get_directory_entry("directory-entry-single/missing");
	REQUIRE_FALSE(missing);
	const auto nullPath = get_directory_entry(static_cast<const char*>(nullptr));
	REQUIRE_FALSE(nullPath);
#ifdef _WIN32
	CHECK(missing.error().native_code == ERROR_FILE_NOT_FOUND);
	CHECK(nullPath.error().native_code == ERROR_INVALID_PARAMETER);
#else
	CHECK(missing.error().native_code == ENOENT);
	CHECK(nullPath.error().native_code == EINVAL);
#endif

	REQUIRE(file::delete_file(filePath));
	REQUIRE(removeDirectory(directoryPath));
}

#ifndef _WIN32
TEST_CASE("POSIX directory enumeration identifies links and other entries", "[fs][directory][link]")
{
	static constexpr char directoryPath[] = "list-directory-posix-types";
	static constexpr char targetPath[] = "list-directory-posix-types/target";
	static constexpr char subdirectoryPath[] = "list-directory-posix-types/subdir";
	static constexpr char linkPath[] = "list-directory-posix-types/link";
	static constexpr char directoryLinkPath[] = "list-directory-posix-types/dir-link";
	static constexpr char danglingLinkPath[] = "list-directory-posix-types/dangling-link";
	static constexpr char fifoPath[] = "list-directory-posix-types/fifo";
	file::delete_file(danglingLinkPath);
	file::delete_file(directoryLinkPath);
	file::delete_file(linkPath);
	file::delete_file(targetPath);
	::unlink(fifoPath);
	removeDirectory(subdirectoryPath);
	removeDirectory(directoryPath);

	REQUIRE(createDirectory(directoryPath));
	static constexpr std::string_view targetContents = "link target";
	REQUIRE(createFileWithContents(targetPath, targetContents));
	REQUIRE(createDirectory(subdirectoryPath));
	REQUIRE(::symlink("target", linkPath) == 0);
	REQUIRE(::symlink("subdir", directoryLinkPath) == 0);
	REQUIRE(::symlink("no-such-target", danglingLinkPath) == 0);
	REQUIRE(::mkfifo(fifoPath, 0600) == 0);

	const auto listed = list_directory(directoryPath);
	REQUIRE(listed);
	const directory_entry* const link = findEntry(*listed, "link");
	const directory_entry* const directoryLink = findEntry(*listed, "dir-link");
	const directory_entry* const danglingLink = findEntry(*listed, "dangling-link");
	const directory_entry* const fifo = findEntry(*listed, "fifo");
	REQUIRE(link != nullptr);
	CHECK(link->attributes.is_link);
	CHECK(link->attributes.kind == entry_kind::other);
	// A symlink to a directory is still classified as the link entry itself, never as a directory.
	REQUIRE(directoryLink != nullptr);
	CHECK(directoryLink->attributes.is_link);
	CHECK(directoryLink->attributes.kind == entry_kind::other);
	// A dangling link is classified from the entry alone, without the target ever being resolved.
	REQUIRE(danglingLink != nullptr);
	CHECK(danglingLink->attributes.is_link);
	CHECK(danglingLink->attributes.kind == entry_kind::other);
	CHECK_FALSE(danglingLink->logical_size);
	REQUIRE(fifo != nullptr);
	CHECK_FALSE(fifo->attributes.is_link);
	CHECK(fifo->attributes.kind == entry_kind::other);
	CHECK_FALSE(link->link_target);

	const auto detailed = list_directory(directoryPath, listing_detail::full);
	REQUIRE(detailed);
	const directory_entry* const detailedLink = findEntry(*detailed, "link");
	const directory_entry* const detailedDirectoryLink = findEntry(*detailed, "dir-link");
	const directory_entry* const detailedDanglingLink = findEntry(*detailed, "dangling-link");
	const directory_entry* const detailedFifo = findEntry(*detailed, "fifo");
	REQUIRE(detailedLink != nullptr);
	CHECK(detailedLink->attributes.kind == entry_kind::other);
	REQUIRE(detailedLink->link_target);
	CHECK(detailedLink->link_target->attributes.kind == entry_kind::regular_file);
	CHECK(detailedLink->link_target->logical_size == targetContents.size());
	REQUIRE(detailedDirectoryLink != nullptr);
	REQUIRE(detailedDirectoryLink->link_target);
	CHECK(detailedDirectoryLink->link_target->attributes.kind == entry_kind::directory);
	REQUIRE(detailedDanglingLink != nullptr);
	CHECK(detailedDanglingLink->attributes.is_link);
	CHECK_FALSE(detailedDanglingLink->link_target);
	REQUIRE(detailedFifo != nullptr);
	CHECK(detailedFifo->attributes.kind == entry_kind::other);
	CHECK_FALSE(detailedFifo->link_target);

	// A trailing separator would resolve the link; it is ignored instead
	const auto singleDirectoryLink = get_directory_entry("list-directory-posix-types/dir-link/");
	REQUIRE(singleDirectoryLink);
	// Linux updates a symlink's access time when following it, which the full listing has already done
	directory_entry expectedDirectoryLink = *detailedDirectoryLink;
	expectedDirectoryLink.times.last_access = singleDirectoryLink->times.last_access;
	CHECK(*singleDirectoryLink == expectedDirectoryLink);
	const auto singleDanglingLink = get_directory_entry(danglingLinkPath);
	REQUIRE(singleDanglingLink);
	CHECK(singleDanglingLink->attributes.is_link);
	CHECK_FALSE(singleDanglingLink->link_target);

	REQUIRE(file::delete_file(danglingLinkPath));
	REQUIRE(file::delete_file(directoryLinkPath));
	REQUIRE(file::delete_file(linkPath));
	REQUIRE(file::delete_file(targetPath));
	REQUIRE(::unlink(fifoPath) == 0);
	REQUIRE(removeDirectory(subdirectoryPath));
	REQUIRE(removeDirectory(directoryPath));
}

#ifndef __APPLE__
TEST_CASE("POSIX directory enumeration preserves native name bytes that are not valid UTF-8", "[fs][directory]")
{
	static constexpr char directoryPath[] = "list-directory-posix-native";
	const std::string nativeNamePath = std::string{directoryPath} + "/\xFF";
	file::delete_file(nativeNamePath.c_str());
	removeDirectory(directoryPath);

	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createFileWithContents(nativeNamePath.c_str(), {}));
	const auto listed = list_directory(directoryPath);
	REQUIRE(listed);

	const native_string invalidUtf8Name{"\xFF", 1};
	const directory_entry* const native = findEntry(*listed, invalidUtf8Name);
	REQUIRE(native != nullptr);
	CHECK(native->name == invalidUtf8Name);

	REQUIRE(file::delete_file(nativeNamePath.c_str()));
	REQUIRE(removeDirectory(directoryPath));
}
#endif

TEST_CASE("POSIX get_directory_entry reports the root with an empty name", "[fs][directory]")
{
	const auto root = get_directory_entry("/");
	REQUIRE(root);
	CHECK(root->attributes.kind == entry_kind::directory);
	CHECK(root->name.empty());
}

#ifdef UF_HIDDEN
TEST_CASE("POSIX full listing reports the UF_HIDDEN flag", "[fs][directory]")
{
	static constexpr char directoryPath[] = "list-directory-uf-hidden";
	static constexpr char filePath[] = "list-directory-uf-hidden/flagged";
	::chflags(filePath, 0);
	file::delete_file(filePath);
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createFileWithContents(filePath, {}));
	REQUIRE(::chflags(filePath, UF_HIDDEN) == 0);

	const auto listed = list_directory(directoryPath, listing_detail::full);
	REQUIRE(listed);
	REQUIRE(listed->size() == 1);
	CHECK(listed->front().attributes.hidden);
	const auto single = get_directory_entry(filePath);
	REQUIRE(single);
	CHECK(single->attributes.hidden);
	const auto metadata = get_entry_metadata(filePath, link_behavior::do_not_follow);
	REQUIRE(metadata);
	CHECK(metadata->attributes.hidden);

	REQUIRE(::chflags(filePath, 0) == 0);
	REQUIRE(file::delete_file(filePath));
	REQUIRE(removeDirectory(directoryPath));
}
#endif

TEST_CASE("POSIX inaccessible-directory result is retained when permissions can be enforced", "[fs][directory]")
{
	static constexpr char directoryPath[] = "list-directory-inaccessible";
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	REQUIRE(::chmod(directoryPath, 0) == 0);

	const auto listed = list_directory(directoryPath);
	REQUIRE(::chmod(directoryPath, 0700) == 0);
	if (listed)
		WARN("The test process can bypass directory permissions; inaccessible-directory assertion skipped");
	else
		CHECK((listed.error().native_code == EACCES || listed.error().native_code == EPERM));

	REQUIRE(removeDirectory(directoryPath));
}
#else

namespace {

[[nodiscard]] bool createDirectory(const wchar_t* const path)
{
	windows_path_buffer prepared{path};
	return prepared && ::CreateDirectoryW(prepared.c_str(), nullptr) != 0;
}

bool removeDirectory(const wchar_t* const path) noexcept
{
	windows_path_buffer prepared{path};
	return prepared && ::RemoveDirectoryW(prepared.c_str()) != 0;
}

class long_path_fixture final {
public:
	~long_path_fixture() noexcept
	{
		if (!file_path.empty())
			file::delete_file(file_path.c_str());
		for (auto directory = directories.rbegin(); directory != directories.rend(); ++directory)
			removeDirectory(directory->c_str());
	}

	std::wstring file_path;
	std::vector<std::wstring> directories;
};

// Denies the current user list access to one directory until destroyed.
class list_access_denial final {
public:
	explicit list_access_denial(std::wstring directoryPath) : _path{std::move(directoryPath)}
	{
		if (::GetNamedSecurityInfoW(_path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &_originalAcl, nullptr,
			&_originalDescriptor) != ERROR_SUCCESS)
			return;

		HANDLE token = nullptr;
		if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0)
			return;
		std::array<std::byte, 256> userBuffer{};
		DWORD userSize = 0;
		const bool userRead = ::GetTokenInformation(token, TokenUser, userBuffer.data(), static_cast<DWORD>(userBuffer.size()), &userSize) != 0;
		::CloseHandle(token);
		if (!userRead)
			return;

		EXPLICIT_ACCESSW denial{};
		denial.grfAccessPermissions = FILE_LIST_DIRECTORY;
		denial.grfAccessMode = DENY_ACCESS;
		denial.grfInheritance = NO_INHERITANCE;
		denial.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		denial.Trustee.TrusteeType = TRUSTEE_IS_USER;
		denial.Trustee.ptstrName = static_cast<LPWSTR>(reinterpret_cast<const TOKEN_USER*>(userBuffer.data())->User.Sid);

		PACL deniedAcl = nullptr;
		if (::SetEntriesInAclW(1, &denial, _originalAcl, &deniedAcl) != ERROR_SUCCESS)
			return;
		_applied = ::SetNamedSecurityInfoW(_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, deniedAcl, nullptr) == ERROR_SUCCESS;
		::LocalFree(deniedAcl);
	}

	~list_access_denial() noexcept
	{
		// The owner keeps the right to change the DACL, so the denial cannot lock this out.
		if (_applied)
			::SetNamedSecurityInfoW(_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, _originalAcl, nullptr);
		::LocalFree(_originalDescriptor);
	}

	list_access_denial(const list_access_denial&) = delete;
	list_access_denial& operator=(const list_access_denial&) = delete;

	[[nodiscard]] explicit operator bool() const noexcept { return _applied; }

private:
	std::wstring _path;
	PSECURITY_DESCRIPTOR _originalDescriptor = nullptr;
	PACL _originalAcl = nullptr; // Points into _originalDescriptor
	bool _applied = false;
};

// The mount-point variant of REPARSE_DATA_BUFFER lives in the DDK's ntifs.h, not the SDK; mirror just that layout.
struct mount_point_reparse_buffer final {
	ULONG   ReparseTag;
	USHORT  ReparseDataLength;
	USHORT  Reserved;
	USHORT  SubstituteNameOffset;
	USHORT  SubstituteNameLength;
	USHORT  PrintNameOffset;
	USHORT  PrintNameLength;
	wchar_t PathBuffer[1];
};

// Unlike a symbolic link, a junction needs no privilege, so this always runs on CI. There is no Win32 wrapper for it:
// the reparse point is set by hand via FSCTL_SET_REPARSE_POINT with an absolute NT-namespace target.
[[nodiscard]] bool createJunction(const wchar_t* const linkPath, const wchar_t* const targetPath)
{
	if (!createDirectory(linkPath))
		return false;

	std::array<wchar_t, windows_path_buffer::max_length + 1> absoluteTarget{};
	const DWORD targetLength = ::GetFullPathNameW(targetPath, static_cast<DWORD>(absoluteTarget.size()), absoluteTarget.data(), nullptr);
	if (targetLength == 0 || targetLength >= absoluteTarget.size())
		return false;

	const std::wstring substituteName = std::wstring{L"\\??\\"} + std::wstring{absoluteTarget.data(), targetLength};
	const std::wstring_view printName{absoluteTarget.data(), targetLength};
	const size_t pathBufferChars = substituteName.size() + 1 + printName.size() + 1; // both names, each NUL-terminated

	alignas(mount_point_reparse_buffer) std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> storage{};
	auto* const reparse = reinterpret_cast<mount_point_reparse_buffer*>(storage.data());
	reparse->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
	reparse->ReparseDataLength = static_cast<USHORT>(4 * sizeof(USHORT) + pathBufferChars * sizeof(wchar_t));
	reparse->SubstituteNameOffset = 0;
	reparse->SubstituteNameLength = static_cast<USHORT>(substituteName.size() * sizeof(wchar_t));
	reparse->PrintNameOffset = static_cast<USHORT>((substituteName.size() + 1) * sizeof(wchar_t));
	reparse->PrintNameLength = static_cast<USHORT>(printName.size() * sizeof(wchar_t));
	std::copy_n(substituteName.c_str(), substituteName.size() + 1, reparse->PathBuffer); // includes the terminating NUL
	std::copy_n(printName.data(), printName.size(), reparse->PathBuffer + substituteName.size() + 1);
	reparse->PathBuffer[substituteName.size() + 1 + printName.size()] = L'\0';

	windows_path_buffer preparedLink{linkPath};
	if (!preparedLink)
		return false;
	const HANDLE handle = ::CreateFileW(preparedLink.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return false;

	const DWORD inputSize = static_cast<DWORD>(FIELD_OFFSET(mount_point_reparse_buffer, PathBuffer) + pathBufferChars * sizeof(wchar_t));
	DWORD ignored = 0;
	const bool set = ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, reparse, inputSize, nullptr, 0, &ignored, nullptr) != 0;
	::CloseHandle(handle);
	return set;
}

} // namespace

TEST_CASE("Windows directory enumeration returns native Unicode names through both path overloads", "[fs][directory][windows]")
{
	static constexpr wchar_t directoryPath[] = L"list-directory-\u0434\u0430\u043D\u0456";
	static constexpr wchar_t filePath[] = L"list-directory-\u0434\u0430\u043D\u0456\\\u0444\u0430\u0439\u043B.bin";
	static constexpr auto utf8DirectoryPath = u8"list-directory-\u0434\u0430\u043D\u0456";
	file::delete_file(filePath);
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	file created;
	REQUIRE(created.open(filePath, file::access_mode::Write, file::open_disposition::CreateNew));
	REQUIRE(created.close());

	const auto wide = list_directory(directoryPath);
	const auto utf8 = list_directory(reinterpret_cast<const char*>(utf8DirectoryPath));
	REQUIRE(wide);
	REQUIRE(utf8);
	REQUIRE(wide->size() == 1);
	CHECK(*wide == *utf8);
	CHECK(wide->front().name == L"\u0444\u0430\u0439\u043B.bin");

	REQUIRE(file::delete_file(filePath));
	REQUIRE(removeDirectory(directoryPath));
}

TEST_CASE("Windows directory enumeration reports reparse points and their tag", "[fs][directory][windows][link]")
{
	static constexpr wchar_t directoryPath[] = L"list-directory-reparse";
	static constexpr wchar_t targetPath[] = L"list-directory-reparse\\target.file";
	static constexpr wchar_t linkPath[] = L"list-directory-reparse\\link.file";
	static constexpr wchar_t danglingLinkPath[] = L"list-directory-reparse\\dangling.file";
	file::delete_file(danglingLinkPath);
	file::delete_file(linkPath);
	file::delete_file(targetPath);
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	file target;
	REQUIRE(target.open(targetPath, file::access_mode::Write, file::open_disposition::CreateNew));
	REQUIRE(target.close());

	static constexpr DWORD allowUnprivilegedCreate = 0x2;
	if (::CreateSymbolicLinkW(linkPath, L"target.file", allowUnprivilegedCreate) == 0)
	{
		WARN("Symbolic-link creation is unavailable; Windows reparse enumeration assertion skipped");
		REQUIRE(file::delete_file(targetPath));
		REQUIRE(removeDirectory(directoryPath));
		return;
	}
	REQUIRE(::CreateSymbolicLinkW(danglingLinkPath, L"no-such-target.file", allowUnprivilegedCreate) != 0);

	const auto listed = list_directory(directoryPath);
	REQUIRE(listed);
	const directory_entry* const link = findEntry(*listed, L"link.file");
	const directory_entry* const danglingLink = findEntry(*listed, L"dangling.file");
	REQUIRE(link != nullptr);
	CHECK(link->attributes.is_link);
	CHECK(link->attributes.reparse_tag == IO_REPARSE_TAG_SYMLINK);
	CHECK_FALSE(link->logical_size);
	// The tag comes from the enumeration record itself, so a link whose target does not exist still carries it.
	REQUIRE(danglingLink != nullptr);
	CHECK(danglingLink->attributes.is_link);
	CHECK(danglingLink->attributes.reparse_tag == IO_REPARSE_TAG_SYMLINK);
	CHECK_FALSE(danglingLink->logical_size);
	CHECK_FALSE(link->link_target);

	const auto detailed = list_directory(directoryPath, listing_detail::full);
	REQUIRE(detailed);
	const directory_entry* const detailedLink = findEntry(*detailed, L"link.file");
	const directory_entry* const detailedDanglingLink = findEntry(*detailed, L"dangling.file");
	REQUIRE(detailedLink != nullptr);
	REQUIRE(detailedLink->link_target);
	CHECK(detailedLink->link_target->attributes.kind == entry_kind::regular_file);
	CHECK_FALSE(detailedLink->link_target->attributes.is_link);
	CHECK(detailedLink->link_target->logical_size == 0);
	REQUIRE(detailedDanglingLink != nullptr);
	CHECK_FALSE(detailedDanglingLink->link_target);

	const auto singleLink = get_directory_entry(linkPath);
	REQUIRE(singleLink);
	CHECK(*singleLink == *detailedLink);

	REQUIRE(file::delete_file(danglingLinkPath));
	REQUIRE(file::delete_file(linkPath));
	REQUIRE(file::delete_file(targetPath));
	REQUIRE(removeDirectory(directoryPath));
}

TEST_CASE("Windows directory enumeration reports a junction as a linked directory with the mount-point tag", "[fs][directory][windows][link]")
{
	static constexpr wchar_t directoryPath[] = L"list-directory-junction";
	static constexpr wchar_t targetPath[] = L"list-directory-junction\\target.dir";
	static constexpr wchar_t linkPath[] = L"list-directory-junction\\link.dir";
	removeDirectory(linkPath);
	removeDirectory(targetPath);
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createDirectory(targetPath));
	// Junctions require no privilege, so this case runs unconditionally - no skip guard as for symbolic links.
	REQUIRE(createJunction(linkPath, targetPath));

	const auto listed = list_directory(directoryPath);
	REQUIRE(listed);
	const directory_entry* const link = findEntry(*listed, L"link.dir");
	REQUIRE(link != nullptr);
	CHECK(link->attributes.kind == entry_kind::directory);
	CHECK(link->attributes.is_link);
	CHECK(link->attributes.reparse_tag == IO_REPARSE_TAG_MOUNT_POINT);
	CHECK_FALSE(link->logical_size);

	const auto detailed = list_directory(directoryPath, listing_detail::full);
	REQUIRE(detailed);
	const directory_entry* const detailedLink = findEntry(*detailed, L"link.dir");
	REQUIRE(detailedLink != nullptr);
	REQUIRE(detailedLink->link_target);
	CHECK(detailedLink->link_target->attributes.kind == entry_kind::directory);
	const auto singleLink = get_directory_entry(L"list-directory-junction\\link.dir\\");
	REQUIRE(singleLink);
	CHECK(*singleLink == *detailedLink);

	REQUIRE(removeDirectory(linkPath));
	REQUIRE(removeDirectory(targetPath));
	REQUIRE(removeDirectory(directoryPath));
}

TEST_CASE("Windows directory enumeration reports a directory symbolic link with the symlink tag", "[fs][directory][windows][link]")
{
	static constexpr wchar_t directoryPath[] = L"list-directory-dir-symlink";
	static constexpr wchar_t targetPath[] = L"list-directory-dir-symlink\\target.dir";
	static constexpr wchar_t linkPath[] = L"list-directory-dir-symlink\\link.dir";
	removeDirectory(linkPath);
	removeDirectory(targetPath);
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createDirectory(targetPath));

	static constexpr DWORD allowUnprivilegedCreate = 0x2;
	if (::CreateSymbolicLinkW(linkPath, L"target.dir", SYMBOLIC_LINK_FLAG_DIRECTORY | allowUnprivilegedCreate) == 0)
	{
		WARN("Symbolic-link creation is unavailable; Windows directory-symlink enumeration assertion skipped");
		REQUIRE(removeDirectory(targetPath));
		REQUIRE(removeDirectory(directoryPath));
		return;
	}

	const auto listed = list_directory(directoryPath);
	REQUIRE(listed);
	const directory_entry* const link = findEntry(*listed, L"link.dir");
	REQUIRE(link != nullptr);
	// A directory symbolic link reports its own directory bit, unlike a POSIX symlink which is always `other`.
	CHECK(link->attributes.kind == entry_kind::directory);
	CHECK(link->attributes.is_link);
	CHECK(link->attributes.reparse_tag == IO_REPARSE_TAG_SYMLINK);
	CHECK_FALSE(link->logical_size);

	const auto detailed = list_directory(directoryPath, listing_detail::full);
	REQUIRE(detailed);
	const directory_entry* const detailedLink = findEntry(*detailed, L"link.dir");
	REQUIRE(detailedLink != nullptr);
	REQUIRE(detailedLink->link_target);
	CHECK(detailedLink->link_target->attributes.kind == entry_kind::directory);

	REQUIRE(removeDirectory(linkPath));
	REQUIRE(removeDirectory(targetPath));
	REQUIRE(removeDirectory(directoryPath));
}

TEST_CASE("Windows get_directory_entry reports a drive root and rejects wildcards", "[fs][directory][windows]")
{
	std::array<wchar_t, windows_path_buffer::max_length + 1> currentDirectory{};
	const DWORD length = ::GetCurrentDirectoryW(static_cast<DWORD>(currentDirectory.size()), currentDirectory.data());
	REQUIRE(length > 1);
	REQUIRE(length < currentDirectory.size());
	if (currentDirectory[1] != L':')
	{
		WARN("The current directory is not on a drive; drive-root assertion skipped");
		return;
	}

	const wchar_t driveRoot[4] { currentDirectory[0], L':', L'\\', L'\0' };
	const auto root = get_directory_entry(driveRoot);
	REQUIRE(root);
	CHECK(root->attributes.kind == entry_kind::directory);
	CHECK(root->name.empty());
	CHECK_FALSE(root->link_target);

	const auto wildcard = get_directory_entry("directory-entry-*");
	REQUIRE_FALSE(wildcard);
	CHECK(wildcard.error().native_code == ERROR_INVALID_NAME);
}

TEST_CASE("Windows get_directory_entry reports an entry whose parent cannot be listed", "[fs][directory][windows]")
{
	static constexpr char directoryPath[] = "directory-entry-unlisted";
	static constexpr char filePath[] = "directory-entry-unlisted/File.bin";
	file::delete_file(filePath);
	removeDirectory(directoryPath);
	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createFileWithContents(filePath, "unlisted"));

	const auto listable = get_directory_entry(filePath);
	REQUIRE(listable);
	{
		const list_access_denial denial{ L"directory-entry-unlisted" };
		REQUIRE(denial);
		if (list_directory(directoryPath))
		{
			WARN("The test process can bypass the denial; unlisted-parent assertions skipped");
		}
		else
		{
			const auto unlisted = get_directory_entry("directory-entry-unlisted/file.bin");
			REQUIRE(unlisted);
			CHECK(unlisted->name == L"file.bin"); // The path's spelling: the filesystem's needs the listing
			CHECK(static_cast<const entry_status&>(*unlisted) == static_cast<const entry_status&>(*listable));
		}
	}

	REQUIRE(file::delete_file(filePath));
	REQUIRE(removeDirectory(directoryPath));
}

TEST_CASE("Windows drive-relative enumeration lists the drive's current directory", "[fs][directory][windows]")
{
	std::array<wchar_t, windows_path_buffer::max_length + 1> currentDirectory{};
	const DWORD length = ::GetCurrentDirectoryW(static_cast<DWORD>(currentDirectory.size()), currentDirectory.data());
	REQUIRE(length > 1);
	REQUIRE(length < currentDirectory.size());
	if (currentDirectory[1] != L':')
	{
		WARN("The current directory is not on a drive; drive-relative enumeration assertion skipped");
		return;
	}

	// "X:" must resolve to the drive's current directory, not to its root
	const wchar_t driveRelative[3] { currentDirectory[0], L':', L'\0' };
	const auto relative = list_directory(driveRelative);
	const auto current = list_directory(".");
	REQUIRE(relative);
	REQUIRE(current);
	CHECK(*relative == *current);
}

TEST_CASE("Windows directory enumeration supports long native paths", "[fs][directory][windows]")
{
	std::array<wchar_t, windows_path_buffer::max_length + 1> currentDirectory{};
	const DWORD currentDirectoryLength = ::GetCurrentDirectoryW(static_cast<DWORD>(currentDirectory.size()), currentDirectory.data());
	REQUIRE(currentDirectoryLength > 0);
	REQUIRE(currentDirectoryLength < currentDirectory.size());

	long_path_fixture fixture;
	std::wstring directory{currentDirectory.data(), currentDirectoryLength};
	directory += L"\\thin_io_list_long_" + std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(::GetTickCount64());
	REQUIRE(createDirectory(directory.c_str()));
	fixture.directories.push_back(directory);
	for (size_t segment = 0; directory.size() < static_cast<size_t>(MAX_PATH) + 32; ++segment)
	{
		directory += L"\\segment_" + std::to_wstring(segment) + L"_abcdefghijklmnopqrstuvwxyz";
		REQUIRE(createDirectory(directory.c_str()));
		fixture.directories.push_back(directory);
	}

	fixture.file_path = directory + L"\\listed.file";
	file created;
	REQUIRE(created.open(fixture.file_path.c_str(), file::access_mode::Write, file::open_disposition::CreateNew));
	REQUIRE(created.close());
	const auto listed = list_directory(directory.c_str());
	REQUIRE(listed);
	CHECK(findEntry(*listed, L"listed.file") != nullptr);
	REQUIRE(file::delete_file(fixture.file_path.c_str()));
	fixture.file_path.clear();
}
#endif
