#include "catch2/catch.hpp"

#include "file.hpp"

#include <memory.h>

#ifndef _WIN32
#include <unistd.h>

using namespace thin_io;

TEST_CASE("exclusive creation does not follow a link", "[file][link]")
{
	static constexpr char targetPath[] = "exclusive-create-target.file";
	static constexpr char linkPath[] = "exclusive-create-link.file";
	static constexpr char targetContents[] = "target contents";
	file::delete_file(linkPath);
	file::delete_file(targetPath);

	file target;
	REQUIRE(target.open(targetPath, file::access_mode::Write, file::open_disposition::CreateNew));
	REQUIRE(target.write(targetContents, sizeof(targetContents)) == sizeof(targetContents));
	REQUIRE(target.close());
	REQUIRE(::symlink(targetPath, linkPath) == 0);

	file link;
	REQUIRE(!link.open(linkPath, file::access_mode::Write, file::open_disposition::CreateNew));
	REQUIRE(!link.is_open());

	REQUIRE(target.open(targetPath, file::access_mode::Read, file::open_disposition::OpenExisting));
	char contentsAfterFailedCreate[sizeof(targetContents)]{};
	REQUIRE(target.read(contentsAfterFailedCreate, sizeof(contentsAfterFailedCreate)) == sizeof(contentsAfterFailedCreate));
	REQUIRE(::memcmp(contentsAfterFailedCreate, targetContents, sizeof(targetContents)) == 0);
	REQUIRE(target.close());

	REQUIRE(file::delete_file(linkPath));
	REQUIRE(file::delete_file(targetPath));
}
#endif
