#include "catch2/catch.hpp"

#include "file.hpp"
#include "windows_path_win.hpp"

#include <Windows.h>

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace thin_io;

namespace {

template <class Character>
static void exerciseFileOperationsThroughPath(const Character* path)
{
	static constexpr char initialContents[] = "file API native-path test";
	static constexpr uint64_t allocatedSize = 4096;

	file::delete_file(path);

	file created;
	REQUIRE(created.open(path, file::access_mode::ReadWrite, file::open_disposition::CreateOrTruncate));
	REQUIRE(created.write(initialContents, sizeof(initialContents)) == sizeof(initialContents));
	REQUIRE(created.resize(allocatedSize));
	REQUIRE(created.preallocate(allocatedSize));

	void* const mapping = created.mmap(file::mmap_access_mode::ReadWrite, 0, allocatedSize);
	REQUIRE(mapping != nullptr);
	static_cast<char*>(mapping)[0] = 'F';
	REQUIRE(created.unmap(mapping));
	REQUIRE(created.close());

	file openedWithDefaults;
	REQUIRE(openedWithDefaults.open(path, file::access_mode::Read));
	char firstByte = 0;
	REQUIRE(openedWithDefaults.read(&firstByte, 1) == 1);
	CHECK(firstByte == 'F');
	REQUIRE(openedWithDefaults.close());

	auto openedWithStaticDefaults = file::open_file(path, file::access_mode::Read);
	REQUIRE(openedWithStaticDefaults);
	REQUIRE(openedWithStaticDefaults.size() == allocatedSize);
	REQUIRE(openedWithStaticDefaults.close());

	auto openedWithStaticDisposition = file::open_file(path, file::access_mode::ReadWrite, file::open_disposition::OpenExisting);
	REQUIRE(openedWithStaticDisposition);
	REQUIRE(openedWithStaticDisposition.close());

	REQUIRE(file::delete_file(path));
}

[[nodiscard]] static bool createDirectory(const wchar_t* path)
{
	windows_path_buffer preparedPath{path};
	if (!preparedPath)
	{
		::SetLastError(preparedPath.error_code());
		return false;
	}

	return ::CreateDirectoryW(preparedPath.c_str(), nullptr) != 0;
}

static void removeDirectory(const wchar_t* path) noexcept
{
	windows_path_buffer preparedPath{path};
	if (preparedPath)
		::RemoveDirectoryW(preparedPath.c_str());
}

static void deleteFile(const wchar_t* path) noexcept
{
	windows_path_buffer preparedPath{path};
	if (preparedPath)
		::DeleteFileW(preparedPath.c_str());
}

[[nodiscard]] static std::optional<std::string> toUtf8(const std::wstring_view nativePath)
{
	if (nativePath.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
		return std::nullopt;

	const int sourceLength = static_cast<int>(nativePath.size());
	const int utf8Length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, nativePath.data(), sourceLength,
											  nullptr, 0, nullptr, nullptr);
	if (utf8Length == 0)
		return std::nullopt;

	std::string utf8Path(static_cast<size_t>(utf8Length), '\0');
	if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, nativePath.data(), sourceLength,
							  utf8Path.data(), utf8Length, nullptr, nullptr) != utf8Length)
		return std::nullopt;

	return utf8Path;
}

class longPathFixture final {
public:
	~longPathFixture() noexcept
	{
		if (!filePath.empty())
			deleteFile(filePath.c_str());

		for (auto directory = directories.rbegin(); directory != directories.rend(); ++directory)
			removeDirectory(directory->c_str());
	}

	std::wstring filePath;
	std::vector<std::wstring> directories;
};

} // namespace

TEST_CASE("Windows file APIs perform the same operations through UTF-8 and native-wide paths", "[file][windows]")
{
	SECTION("UTF-8 relative Unicode path")
	{
		static constexpr auto path = u8"thin_io_\u0444\u0430\u0439\u043B_utf8.tmp";
		exerciseFileOperationsThroughPath(reinterpret_cast<const char*>(path));
	}

	SECTION("native-wide relative Unicode path")
	{
		exerciseFileOperationsThroughPath(L"thin_io_\u0444\u0430\u0439\u043B_wide.tmp");
	}
}

TEST_CASE("Windows UTF-8 and native-wide file APIs address the same Unicode file", "[file][windows]")
{
	static constexpr auto utf8Path = u8"thin_io_\u0441\u043F\u0456\u043B\u044C\u043D\u0438\u0439.tmp";
	static constexpr wchar_t nativePath[] = L"thin_io_\u0441\u043F\u0456\u043B\u044C\u043D\u0438\u0439.tmp";
	static constexpr char contents[] = "shared path";
	const char* const charPath = reinterpret_cast<const char*>(utf8Path);

	file::delete_file(nativePath);
	auto created = file::open_file(nativePath, file::access_mode::Write, file::open_disposition::CreateNew);
	REQUIRE(created);
	REQUIRE(created.write(contents, sizeof(contents)) == sizeof(contents));
	REQUIRE(created.close());

	file opened;
	REQUIRE(opened.open(charPath, file::access_mode::Read, file::open_disposition::OpenExisting));
	std::array<char, sizeof(contents)> readContents{};
	REQUIRE(opened.read(readContents.data(), readContents.size()) == readContents.size());
	CHECK(readContents == std::to_array(contents));
	REQUIRE(opened.close());
	REQUIRE(file::delete_file(charPath));
}

TEST_CASE("Windows explicit sharing mode is honored exactly", "[file][windows]")
{
	static constexpr const char testFilePath[] = "thin_io_sharing.tmp";
	file::delete_file(testFilePath);

	file writer;
	REQUIRE(writer.open(testFilePath, file::access_mode::Write)); // sharing_mode::Default: concurrent readers allowed

	file reader;
	CHECK_FALSE(reader.open(testFilePath, file::access_mode::Read, file::sys_cache_mode::CachingEnabled, file::sharing_mode::NoSharing));
	CHECK(file::error_code() == ERROR_SHARING_VIOLATION);

	CHECK(reader.open(testFilePath, file::access_mode::Read, file::sys_cache_mode::CachingEnabled,
					  file::sharing_mode::ShareRead | file::sharing_mode::ShareWrite));
	REQUIRE(reader.close());

	CHECK(reader.open(testFilePath, file::access_mode::Read)); // sharing_mode::Default tolerates the concurrent writer
	REQUIRE(reader.close());

	REQUIRE(writer.close());
	REQUIRE(file::delete_file(testFilePath));
}

TEST_CASE("Windows file APIs support long paths independently of process policy", "[file][windows]")
{
	std::array<wchar_t, windows_path_buffer::max_length + 1> currentDirectory{};
	const DWORD currentDirectoryLength = ::GetCurrentDirectoryW(static_cast<DWORD>(currentDirectory.size()), currentDirectory.data());
	REQUIRE(currentDirectoryLength > 0);
	REQUIRE(currentDirectoryLength < currentDirectory.size());

	longPathFixture fixture;
	std::wstring directory{currentDirectory.data(), currentDirectoryLength};
	directory += L"\\thin_io_long_path_" + std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(::GetTickCount64());
	REQUIRE(createDirectory(directory.c_str()));
	fixture.directories.push_back(directory);

	for (size_t segment = 0; directory.size() < static_cast<size_t>(MAX_PATH) + 64; ++segment)
	{
		directory += L"\\segment_" + std::to_wstring(segment) + L"_abcdefghijklmnopqrstuvwxyz";
		REQUIRE(createDirectory(directory.c_str()));
		fixture.directories.push_back(directory);
	}

	fixture.filePath = directory + L"\\\u0444\u0430\u0439\u043B.tmp";
	REQUIRE(fixture.filePath.size() > static_cast<size_t>(MAX_PATH));
	const auto utf8Path = toUtf8(fixture.filePath);
	REQUIRE(utf8Path);

	static constexpr char contents[] = "long path";
	auto created = file::open_file(fixture.filePath.c_str(), file::access_mode::Write, file::open_disposition::CreateNew);
	REQUIRE(created);
	REQUIRE(created.write(contents, sizeof(contents)) == sizeof(contents));
	REQUIRE(created.close());

	file opened;
	REQUIRE(opened.open(utf8Path->c_str(), file::access_mode::Read, file::open_disposition::OpenExisting));
	std::array<char, sizeof(contents)> readContents{};
	REQUIRE(opened.read(readContents.data(), readContents.size()) == readContents.size());
	CHECK(readContents == std::to_array(contents));
	REQUIRE(opened.close());
	REQUIRE(file::delete_file(utf8Path->c_str()));
	fixture.filePath.clear();
}
