#include "catch2/catch.hpp"

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

#include <array>
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

#ifndef _WIN32
TEST_CASE("POSIX directory enumeration identifies links and other entries", "[fs][directory][link]")
{
	static constexpr char directoryPath[] = "list-directory-posix-types";
	static constexpr char targetPath[] = "list-directory-posix-types/target";
	static constexpr char linkPath[] = "list-directory-posix-types/link";
	static constexpr char fifoPath[] = "list-directory-posix-types/fifo";
	file::delete_file(linkPath);
	file::delete_file(targetPath);
	::unlink(fifoPath);
	removeDirectory(directoryPath);

	REQUIRE(createDirectory(directoryPath));
	REQUIRE(createFileWithContents(targetPath, {}));
	REQUIRE(::symlink("target", linkPath) == 0);
	REQUIRE(::mkfifo(fifoPath, 0600) == 0);

	const auto listed = list_directory(directoryPath);
	REQUIRE(listed);
	const directory_entry* const link = findEntry(*listed, "link");
	const directory_entry* const fifo = findEntry(*listed, "fifo");
	REQUIRE(link != nullptr);
	CHECK(link->attributes.is_link);
	CHECK(link->attributes.kind == entry_kind::other);
	REQUIRE(fifo != nullptr);
	CHECK_FALSE(fifo->attributes.is_link);
	CHECK(fifo->attributes.kind == entry_kind::other);

	REQUIRE(file::delete_file(linkPath));
	REQUIRE(file::delete_file(targetPath));
	REQUIRE(::unlink(fifoPath) == 0);
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

	const auto listed = list_directory(directoryPath);
	REQUIRE(listed);
	const directory_entry* const link = findEntry(*listed, L"link.file");
	REQUIRE(link != nullptr);
	CHECK(link->attributes.is_link);
	CHECK(link->attributes.reparse_tag == IO_REPARSE_TAG_SYMLINK);
	CHECK_FALSE(link->logical_size);

	REQUIRE(file::delete_file(linkPath));
	REQUIRE(file::delete_file(targetPath));
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
