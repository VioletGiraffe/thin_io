#include "catch2/catch.hpp"

#include "file.hpp"
#include "fs.hpp"
#include "windows_path_win.hpp"

#include <Windows.h>

#include <stddef.h>
#include <string>
#include <string_view>

using namespace thin_io;

namespace {

template <class Character>
[[nodiscard]] static std::wstring preparePath(const Character* path)
{
	windows_path_buffer prepared{path};
	CHECK(static_cast<bool>(prepared));
	CHECK(prepared.error_code() == ERROR_SUCCESS);
	return prepared.c_str();
}

template <class Character>
static void checkPreparedPath(const Character* input, const std::wstring_view expected)
{
	const std::wstring prepared = preparePath(input);
	CHECK(std::wstring_view{prepared} == expected);
}

} // namespace

TEST_CASE("Windows path preparation converts ordinary UTF-8 paths", "[windows-path]")
{
	static constexpr auto nonAsciiPath = u8"C:/\u0434\u0430\u043D\u0456/\u0444\u0430\u0439\u043B.txt";
	CHECK(preparePath("file.txt") == L"file.txt");
	CHECK(preparePath("folder/file.txt") == L"folder\\file.txt");
	CHECK(preparePath("C:/folder/file.txt") == LR"(\\?\C:\folder\file.txt)");
	CHECK(preparePath(reinterpret_cast<const char*>(nonAsciiPath))
		  == L"\\\\?\\C:\\\u0434\u0430\u043D\u0456\\\u0444\u0430\u0439\u043B.txt");
}

TEST_CASE("Windows path preparation recognizes ordinary absolute roots", "[windows-path]")
{
	CHECK(preparePath(R"(C:\)") == LR"(\\?\C:\)");
	CHECK(preparePath(R"(\\server\share)") == LR"(\\?\UNC\server\share)");
	CHECK(preparePath(R"(\\server\share\)") == LR"(\\?\UNC\server\share\)");
	CHECK(preparePath(R"(\\server\share\folder\file.txt)") == LR"(\\?\UNC\server\share\folder\file.txt)");
	CHECK(preparePath("//server/share/folder/file.txt") == LR"(\\?\UNC\server\share\folder\file.txt)");
}

TEST_CASE("Windows path preparation preserves non-absolute path semantics", "[windows-path]")
{
	CHECK(preparePath("C:file.txt") == L"C:file.txt");
	CHECK(preparePath(R"(\folder\file.txt)") == LR"(\folder\file.txt)");
	CHECK(preparePath(R"(.\folder\..\file.txt)") == LR"(.\folder\..\file.txt)");
	CHECK(preparePath(R"(folder\\child\)") == LR"(folder\\child\)");
}

TEST_CASE("Windows path preparation normalizes ordinary absolute path components before prefixing", "[windows-path]")
{
	CHECK(preparePath(R"(C:\folder\\child\\)") == LR"(\\?\C:\folder\child\)");
	CHECK(preparePath(R"(\\\\server\\share\\folder)") == LR"(\\?\UNC\server\share\folder)");
	CHECK(preparePath(R"(C:\folder\.\child\..\file.txt)") == LR"(\\?\C:\folder\file.txt)");
	CHECK(preparePath(R"(C:\..\file.txt)") == LR"(\\?\C:\file.txt)");
	CHECK(preparePath(R"(\\server\share\folder\..\..\file.txt)") == LR"(\\?\UNC\server\share\file.txt)");
	CHECK(preparePath("C:\\folder. \\file... ") == LR"(\\?\C:\folder\file)");
}

TEST_CASE("Windows path preparation leaves already extended paths opaque", "[windows-path]")
{
	CHECK(preparePath(R"(\\?\C:\folder\.\file... )") == LR"(\\?\C:\folder\.\file... )");
	CHECK(preparePath(R"(\\?\UNC\server\share\folder\\file.txt)") == LR"(\\?\UNC\server\share\folder\\file.txt)");
	CHECK(preparePath(R"(\\?\Volume{01234567-89AB-CDEF-0123-456789ABCDEF}\)")
		  == LR"(\\?\Volume{01234567-89AB-CDEF-0123-456789ABCDEF}\)");
	CHECK(preparePath(R"(\\?\C:/folder/file.txt)") == LR"(\\?\C:/folder/file.txt)");
}

TEST_CASE("Windows path preparation accepts native wide paths without a UTF-8 round trip", "[windows-path]")
{
	checkPreparedPath(L"C:/folder/file.txt", LR"(\\?\C:\folder\file.txt)");
	checkPreparedPath(L"C:/\u0434\u0430\u043D\u0456/\u0444\u0430\u0439\u043B.txt",
				  L"\\\\?\\C:\\\u0434\u0430\u043D\u0456\\\u0444\u0430\u0439\u043B.txt");
}

TEST_CASE("Windows path preparation rejects malformed UTF-8 and retains the conversion error", "[windows-path]")
{
	const std::string invalidUtf8 = "C:\\invalid-\xC3\x28";
	windows_path_buffer prepared{invalidUtf8.c_str()};
	CHECK_FALSE(static_cast<bool>(prepared));
	CHECK(prepared.error_code() == ERROR_NO_UNICODE_TRANSLATION);

	::SetLastError(ERROR_ACCESS_DENIED);
	CHECK(prepared.error_code() == ERROR_NO_UNICODE_TRANSLATION);
}

TEST_CASE("Existing Windows APIs publish path preparation errors", "[windows-path]")
{
	const std::string invalidUtf8 = "C:\\invalid-\xC3\x28";

	file f;
	CHECK_FALSE(f.open(invalidUtf8.c_str(), file::access_mode::Read));
	CHECK(file::error_code() == ERROR_NO_UNICODE_TRANSLATION);

	CHECK_FALSE(file::delete_file(invalidUtf8.c_str()));
	CHECK(file::error_code() == ERROR_NO_UNICODE_TRANSLATION);

	CHECK_FALSE(f.open(static_cast<const wchar_t*>(nullptr), file::access_mode::Read));
	CHECK(file::error_code() == ERROR_INVALID_PARAMETER);

	CHECK_FALSE(file::delete_file(static_cast<const wchar_t*>(nullptr)));
	CHECK(file::error_code() == ERROR_INVALID_PARAMETER);

	CHECK_FALSE(get_times(invalidUtf8.c_str()));
	CHECK(file::error_code() == ERROR_NO_UNICODE_TRANSLATION);
	CHECK_FALSE(get_times(static_cast<const wchar_t*>(nullptr)));
	CHECK(file::error_code() == ERROR_INVALID_PARAMETER);

	entry_times times;
	times.last_write = timestamp{};
	CHECK_FALSE(set_times(invalidUtf8.c_str(), times));
	CHECK(file::error_code() == ERROR_NO_UNICODE_TRANSLATION);
	CHECK_FALSE(set_times(static_cast<const wchar_t*>(nullptr), times));
	CHECK(file::error_code() == ERROR_INVALID_PARAMETER);

	std::string excessivePath = "C:\\";
	excessivePath.append(windows_path_buffer::max_length - excessivePath.size(), 'a');
	CHECK_FALSE(f.open(excessivePath.c_str(), file::access_mode::Read));
	CHECK(file::error_code() == ERROR_FILENAME_EXCED_RANGE);
}

TEST_CASE("Windows path preparation rejects empty and excessively long input", "[windows-path]")
{
	SECTION("empty")
	{
		windows_path_buffer prepared{""};
		CHECK_FALSE(static_cast<bool>(prepared));
		CHECK(prepared.error_code() == ERROR_INVALID_NAME);
	}

	SECTION("prepared path exceeds the Win32 limit")
	{
		std::string excessivePath = "C:\\";
		excessivePath.append(windows_path_buffer::max_length - excessivePath.size(), 'a');
		windows_path_buffer prepared{excessivePath.c_str()};
		CHECK_FALSE(static_cast<bool>(prepared));
		CHECK(prepared.error_code() == ERROR_FILENAME_EXCED_RANGE);
	}

	SECTION("input exceeds the limit before normalization")
	{
		std::wstring excessivePath = L"C:\\";
		while (excessivePath.size() <= windows_path_buffer::max_length)
			excessivePath.append(L".\\");

		windows_path_buffer prepared{excessivePath.c_str()};
		CHECK_FALSE(static_cast<bool>(prepared));
		CHECK(prepared.error_code() == ERROR_FILENAME_EXCED_RANGE);
	}
}

TEST_CASE("Windows path preparation rejects null input", "[windows-path]")
{
	windows_path_buffer utf8Path{static_cast<const char*>(nullptr)};
	CHECK_FALSE(static_cast<bool>(utf8Path));
	CHECK(utf8Path.error_code() == ERROR_INVALID_PARAMETER);

	windows_path_buffer nativePath{static_cast<const wchar_t*>(nullptr)};
	CHECK_FALSE(static_cast<bool>(nativePath));
	CHECK(nativePath.error_code() == ERROR_INVALID_PARAMETER);
}

TEST_CASE("Windows path preparation rejects structurally malformed paths", "[windows-path]")
{
	const char* const malformedPaths[] { R"(\\server)", R"(\\server\)", R"(\\?\)" };
	for (const char* path : malformedPaths)
	{
		INFO("path: " << path);
		windows_path_buffer prepared{path};
		CHECK_FALSE(static_cast<bool>(prepared));
		CHECK(prepared.error_code() == ERROR_INVALID_NAME);
	}
}

TEST_CASE("Windows path preparation rejects unsupported namespaces", "[windows-path]")
{
	const char* const unsupportedPaths[] {
		R"(\\.\C:)",
		R"(\\.\PhysicalDrive0)",
		R"(\??\C:\file.txt)",
		R"(\Device\HarddiskVolume1\file.txt)"
	};

	for (const char* path : unsupportedPaths)
	{
		INFO("path: " << path);
		windows_path_buffer prepared{path};
		CHECK_FALSE(static_cast<bool>(prepared));
		CHECK(prepared.error_code() == ERROR_NOT_SUPPORTED);
	}
}

TEST_CASE("Windows path preparation appends a directory search pattern", "[windows-path]")
{
	SECTION("ordinary directory")
	{
		windows_path_buffer prepared{R"(C:\folder)"};
		REQUIRE(prepared.append_directory_search_pattern());
		CHECK(std::wstring_view{prepared.c_str()} == LR"(\\?\C:\folder\*)");
	}

	SECTION("drive root")
	{
		windows_path_buffer prepared{R"(C:\)"};
		REQUIRE(prepared.append_directory_search_pattern());
		CHECK(std::wstring_view{prepared.c_str()} == LR"(\\?\C:\*)");
	}

	SECTION("existing trailing separator")
	{
		windows_path_buffer prepared{R"(folder\)"};
		REQUIRE(prepared.append_directory_search_pattern());
		CHECK(std::wstring_view{prepared.c_str()} == LR"(folder\*)");
	}

	SECTION("path at the Win32 limit")
	{
		std::wstring maximumPath = LR"(\\?\)";
		maximumPath.append(windows_path_buffer::max_length - maximumPath.size(), L'a');
		windows_path_buffer prepared{maximumPath.c_str()};
		REQUIRE(static_cast<bool>(prepared));
		CHECK_FALSE(prepared.append_directory_search_pattern());
		CHECK(prepared.error_code() == ERROR_FILENAME_EXCED_RANGE);
	}
}

TEST_CASE("Windows path preparation appends a directory separator", "[windows-path]")
{
	windows_path_buffer directory{R"(C:\folder)"};
	REQUIRE(directory.append_directory_separator());
	CHECK(std::wstring_view{directory.c_str()} == LR"(\\?\C:\folder\)");

	windows_path_buffer uncShare{R"(\\server\share)"};
	REQUIRE(uncShare.append_directory_separator());
	CHECK(std::wstring_view{uncShare.c_str()} == LR"(\\?\UNC\server\share\)");
}
