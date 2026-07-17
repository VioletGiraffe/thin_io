#include "catch2/catch.hpp"

#include "fs.hpp"

#ifdef _WIN32
#include <Windows.h>
#else
#include <errno.h>
#endif

using namespace thin_io;

namespace {

void checkSpaceInvariants(const filesystem_space& space)
{
	CHECK(space.capacity > 0);
	CHECK(space.available <= space.free);
	CHECK(space.available <= space.capacity);
#ifdef _WIN32
	if (space.free > space.capacity)
		WARN("A Windows quota makes caller-visible capacity smaller than volume-wide free space");
#else
	CHECK(space.free <= space.capacity);
#endif
}

} // namespace

TEST_CASE("get_filesystem_space reports space and identity for a directory", "[fs][space]")
{
	const auto space = get_filesystem_space(".");
	REQUIRE(space);
	checkSpaceInvariants(*space);

	const auto metadata = get_entry_metadata(".", link_behavior::follow);
	REQUIRE(metadata);
	if (!space->identity || !metadata->identity)
		WARN("The test filesystem does not expose stable filesystem identity");
	else
		CHECK(*space->identity == metadata->identity->filesystem);
}

TEST_CASE("get_filesystem_space captures invalid and missing directory errors", "[fs][space]")
{
	const auto nullPath = get_filesystem_space(static_cast<const char*>(nullptr));
	REQUIRE_FALSE(nullPath);
#ifdef _WIN32
	CHECK(nullPath.error().native_code == ERROR_INVALID_PARAMETER);
#else
	CHECK(nullPath.error().native_code == EINVAL);
#endif

	const auto missing = get_filesystem_space("thin-io-no-such-space-directory");
	REQUIRE_FALSE(missing);
	CHECK(missing.error().native_code != 0);
}

#ifdef _WIN32
TEST_CASE("Windows filesystem-space overloads address the same Unicode directory", "[fs][space][windows]")
{
	static constexpr wchar_t nativePath[] = L"filesystem-space-\u0434\u0430\u043D\u0456";
	static constexpr auto utf8Path = u8"filesystem-space-\u0434\u0430\u043D\u0456";
	const char* const narrowPath = reinterpret_cast<const char*>(utf8Path);
	::RemoveDirectoryW(nativePath);
	REQUIRE(::CreateDirectoryW(nativePath, nullptr) != 0);

	const auto native = get_filesystem_space(nativePath);
	const auto utf8 = get_filesystem_space(narrowPath);
	REQUIRE(native);
	REQUIRE(utf8);
	checkSpaceInvariants(*native);
	checkSpaceInvariants(*utf8);
	CHECK(native->capacity == utf8->capacity);
	CHECK(native->identity == utf8->identity);

	REQUIRE(::RemoveDirectoryW(nativePath) != 0);
}
#endif
