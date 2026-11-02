#include "catch_thin_io.hpp"
#include "test_volume.hpp"

#include "directory_snapshot.hpp"
#include "file.hpp"
#include "fs.hpp"

#include <stdint.h>
#include <string_view>

using namespace thin_io;

namespace {

constexpr char directoryPath[] = "directory-snapshot";
constexpr char firstFilePath[] = "directory-snapshot/a.txt";
constexpr char secondFilePath[] = "directory-snapshot/b.txt";

void writeFile(const char* const path, const std::string_view contents)
{
	file f;
	REQUIRE(f.open(path, file::access_mode::Write));
	REQUIRE(f.write(contents.data(), contents.size()) == contents.size());
	REQUIRE(f.close());
}

// Leaves the test directory holding only a.txt
void resetTestDirectory()
{
	(void)remove_entry(secondFilePath);
	(void)remove_entry(firstFilePath);
	(void)remove_entry(directoryPath);
	REQUIRE(create_directory(directoryPath));
	writeFile(firstFilePath, "aaaa");
}

void removeTestDirectory()
{
	(void)remove_entry(secondFilePath);
	REQUIRE(remove_entry(firstFilePath));
	REQUIRE(remove_entry(directoryPath));
}

[[nodiscard]] directory_snapshot takeSnapshot()
{
	auto snapshot = directory_snapshot::take(directoryPath, listing_detail::full);
	REQUIRE(snapshot);
	return std::move(*snapshot);
}

void setLastWrite(const char* const path, const int64_t seconds)
{
	REQUIRE(set_times(path, entry_times{ .last_write = timestamp{ .seconds = seconds } }));
}

} // namespace

TEST_CASE("directory_snapshot compares equal while nothing changes, and differs once an entry is added, renamed or removed", "[snapshot]")
{
	resetTestDirectory();
	const directory_snapshot initial = takeSnapshot();
	CHECK(takeSnapshot() == initial);

	writeFile(secondFilePath, "b");
	const directory_snapshot added = takeSnapshot();
	CHECK(added != initial);

	REQUIRE(remove_entry(secondFilePath));
	CHECK(takeSnapshot() == initial);

	// replace: the destination is gone already, and macOS exFAT has no exclusive rename
	REQUIRE(rename_entry(firstFilePath, secondFilePath, existing_destination::replace));
	CHECK(takeSnapshot() != initial);
	REQUIRE(rename_entry(secondFilePath, firstFilePath, existing_destination::replace));

	removeTestDirectory();
}

TEST_CASE("directory_snapshot differs after a same-size rewrite", "[snapshot]")
{
	resetTestDirectory();
	// Explicit times, 10 s apart: FAT stores modification times to 2 s.
	setLastWrite(firstFilePath, 1'000'000'000);
	const directory_snapshot before = takeSnapshot();

	writeFile(firstFilePath, "bbbb");
	setLastWrite(firstFilePath, 1'000'000'010);
	CHECK(takeSnapshot() != before);

	removeTestDirectory();
}

TEST_CASE("directory_snapshot differs after an attribute-only change", "[snapshot]")
{
	if (test_volume::permissionBitsUnsupported())
		return;

	resetTestDirectory();
	const directory_snapshot before = takeSnapshot();

#ifdef _WIN32
	REQUIRE(set_permissions(firstFilePath, file_permissions{ .read_only = true }, link_behavior::do_not_follow));
	CHECK(takeSnapshot() != before);
	REQUIRE(set_permissions(firstFilePath, file_permissions{}, link_behavior::do_not_follow));
#else
	const auto original = get_entry_status(firstFilePath, link_behavior::do_not_follow);
	REQUIRE(original);
	REQUIRE(original->permissions);
	REQUIRE(set_permissions(firstFilePath, file_permissions{ .mode = original->permissions->mode ^ 0100u }, link_behavior::do_not_follow));
	CHECK(takeSnapshot() != before);
#endif

	removeTestDirectory();
}

TEST_CASE("directory_snapshot ignores an access-time change", "[snapshot]")
{
	resetTestDirectory();
	const directory_snapshot before = takeSnapshot();

	REQUIRE(set_times(firstFilePath, entry_times{ .last_access = timestamp{ .seconds = 1'000'000'000 } }));
	CHECK(takeSnapshot() == before);

	removeTestDirectory();
}

TEST_CASE("directory_snapshot::take fails for a missing directory", "[snapshot]")
{
	(void)remove_entry(directoryPath);
	CHECK_FALSE(directory_snapshot::take(directoryPath, listing_detail::full));
}
