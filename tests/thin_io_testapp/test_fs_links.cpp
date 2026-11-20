#include "catch_thin_io.hpp"
#include "test_volume.hpp"

#include "file.hpp"
#include "fs.hpp"

#ifdef _WIN32
#include "test_reparse_win.hpp"

#include <Windows.h>
#else
#include <errno.h>
#endif

#include <string>

using namespace thin_io;

#ifdef _WIN32
#define NATIVE(text) L##text
#else
#define NATIVE(text) text
#endif

namespace {

[[nodiscard]] bool createEmptyFile(const char* const path)
{
	file created;
	return created.open(path, file::access_mode::Write, file::open_disposition::CreateNew) && created.close();
}

// True, with a warning, where this process may not create symbolic links: Windows without Developer Mode or elevation.
[[nodiscard]] bool symbolicLinksUnavailable(const filesystem_result<void>& creation)
{
#ifdef _WIN32
	if (!creation && creation.error().native_code == ERROR_PRIVILEGE_NOT_HELD)
	{
		WARN("Symbolic-link creation is unavailable: test skipped");
		return true;
	}
#else
	(void)creation;
#endif
	return false;
}

} // namespace

TEST_CASE("create_link and read_link round-trip a symbolic link's target verbatim", "[fs][link]")
{
	if (test_volume::linksUnsupported())
		return;

	static constexpr char directoryPath[] = "link-round-trip";
	static constexpr char targetPath[] = "link-round-trip/target.file";
	static constexpr char linkPath[] = "link-round-trip/link.file";
	(void)remove_entry(linkPath);
	(void)remove_entry(targetPath);
	(void)remove_entry(directoryPath);
	REQUIRE(create_directory(directoryPath));
	REQUIRE(createEmptyFile(targetPath));

	const link_definition relative{ .type = link_type::symbolic_link, .target = NATIVE("target.file") };
	const auto created = create_link(linkPath, relative);
	if (symbolicLinksUnavailable(created))
	{
		REQUIRE(remove_entry(targetPath));
		REQUIRE(remove_entry(directoryPath));
		return;
	}
	REQUIRE(created);

	SECTION("a relative target stays relative, and the link resolves next to itself")
	{
		const auto status = get_entry_status(linkPath, link_behavior::do_not_follow);
		REQUIRE(status);
		CHECK(status->attributes.is_link);
		const auto followed = get_entry_status(linkPath, link_behavior::follow);
		REQUIRE(followed);
		CHECK(followed->attributes.kind == entry_kind::regular_file);
		CHECK(read_link(linkPath) == relative);
	}

	SECTION("a dangling absolute target is stored as given")
	{
		REQUIRE(remove_entry(linkPath));
#ifdef _WIN32
		const link_definition absolute{ .type = link_type::symbolic_link, .target = LR"(Q:\no\such\target.file)" };
#else
		const link_definition absolute{ .type = link_type::symbolic_link, .target = "/no/such/target.file" };
#endif
		REQUIRE(create_link(linkPath, absolute));
		CHECK_FALSE(get_entry_status(linkPath, link_behavior::follow));
		CHECK(read_link(linkPath) == absolute);
	}

	SECTION("a target longer than the first read buffer is read whole")
	{
		REQUIRE(remove_entry(linkPath));
		const link_definition longTarget{ .type = link_type::symbolic_link, .target = native_string(300, NATIVE('x')) };
		REQUIRE(create_link(linkPath, longTarget));
		CHECK(read_link(linkPath) == longTarget);
	}

	SECTION("a trailing separator still names the link itself")
	{
		CHECK(read_link("link-round-trip/link.file/") == relative);
	}

	SECTION("an existing entry at the path fails the creation and stays as it was")
	{
		const link_definition other{ .type = link_type::symbolic_link, .target = NATIVE("other.file") };
		const auto overLink = create_link(linkPath, other);
		REQUIRE_FALSE(overLink);
#ifdef _WIN32
		CHECK(overLink.error().native_code == ERROR_ALREADY_EXISTS);
#else
		CHECK(overLink.error().native_code == EEXIST);
#endif
		CHECK(read_link(linkPath) == relative);

		CHECK_FALSE(create_link(targetPath, other));
		const auto target = get_entry_status(targetPath, link_behavior::do_not_follow);
		REQUIRE(target);
		CHECK_FALSE(target->attributes.is_link);
	}

	REQUIRE(remove_entry(linkPath));
	REQUIRE(remove_entry(targetPath));
	REQUIRE(remove_entry(directoryPath));
}

TEST_CASE("read_link fails on an entry that is not a link", "[fs][link]")
{
	static constexpr char filePath[] = "read-link-not-a-link.file";
	(void)remove_entry(filePath);
	REQUIRE(createEmptyFile(filePath));

	const auto regularFile = read_link(filePath);
	REQUIRE_FALSE(regularFile);
#ifdef _WIN32
	CHECK(regularFile.error().native_code == ERROR_NOT_A_REPARSE_POINT);
#else
	CHECK(regularFile.error().native_code == EINVAL);
#endif

	REQUIRE(remove_entry(filePath));

	const auto missing = read_link(filePath);
	REQUIRE_FALSE(missing);
#ifdef _WIN32
	CHECK(missing.error().native_code == ERROR_FILE_NOT_FOUND);
#else
	CHECK(missing.error().native_code == ENOENT);
#endif
}

TEST_CASE("create_link rejects a definition it cannot store", "[fs][link]")
{
	static constexpr char linkPath[] = "create-link-rejected.link";
	(void)remove_entry(linkPath);

#ifdef _WIN32
	static constexpr DWORD invalidParameter = ERROR_INVALID_PARAMETER;
#else
	static constexpr int invalidParameter = EINVAL;
#endif

	const native_string embeddedNul{ NATIVE("before\0after"), 12 };
	const auto withNul = create_link(linkPath, link_definition{ .type = link_type::symbolic_link, .target = embeddedNul });
	REQUIRE_FALSE(withNul);
	CHECK(withNul.error().native_code == invalidParameter);

	const auto empty = create_link(linkPath, link_definition{ .type = link_type::symbolic_link, .target = {} });
	REQUIRE_FALSE(empty);
	CHECK(empty.error().native_code == invalidParameter);

#ifdef _WIN32
	// A junction cannot hold a relative target
	const auto relativeJunction = create_link(linkPath, link_definition{ .type = link_type::junction, .target = L"relative.dir" });
	REQUIRE_FALSE(relativeJunction);
	CHECK(relativeJunction.error().native_code == invalidParameter);
#else
	const auto junction = create_link(linkPath, link_definition{ .type = link_type::junction, .target = "/tmp" });
	REQUIRE_FALSE(junction);
	CHECK(junction.error().native_code == invalidParameter);
#endif

	CHECK_FALSE(get_entry_status(linkPath, link_behavior::do_not_follow));
}

#ifdef _WIN32

TEST_CASE("Windows create_link and read_link round-trip a directory symbolic link", "[fs][link][windows]")
{
	if (test_volume::linksUnsupported())
		return;

	static constexpr char targetPath[] = "directory-symlink-target.dir";
	static constexpr char linkPath[] = "directory-symlink.dir";
	(void)remove_entry(linkPath);
	(void)remove_entry(targetPath);
	REQUIRE(create_directory(targetPath));

	const link_definition definition{ .type = link_type::directory_symbolic_link, .target = L"directory-symlink-target.dir" };
	const auto created = create_link(linkPath, definition);
	if (symbolicLinksUnavailable(created))
	{
		REQUIRE(remove_entry(targetPath));
		return;
	}
	REQUIRE(created);

	const auto status = get_entry_status(linkPath, link_behavior::do_not_follow);
	REQUIRE(status);
	CHECK(status->attributes.is_link);
	CHECK(status->attributes.kind == entry_kind::directory);
	CHECK(status->attributes.reparse_tag == IO_REPARSE_TAG_SYMLINK);
	CHECK(read_link(linkPath) == definition);

	REQUIRE(remove_entry(linkPath));
	REQUIRE(remove_entry(targetPath));
}

TEST_CASE("Windows create_link and read_link round-trip a junction", "[fs][link][windows]")
{
	if (test_volume::linksUnsupported())
		return;

	static constexpr char targetPath[] = "junction-target.dir";
	static constexpr char targetFilePath[] = "junction-target.dir/inside.file";
	static constexpr char linkPath[] = "junction.dir";
	static constexpr char linkedFilePath[] = "junction.dir/inside.file";
	(void)remove_entry(linkPath);
	(void)remove_entry(targetFilePath);
	(void)remove_entry(targetPath);
	REQUIRE(create_directory(targetPath));
	REQUIRE(createEmptyFile(targetFilePath));

	// Junctions need no privilege
	const link_definition definition{ .type = link_type::junction, .target = absolutePath(L"junction-target.dir") };
	REQUIRE(create_link(linkPath, definition));

	const auto status = get_entry_status(linkPath, link_behavior::do_not_follow);
	REQUIRE(status);
	CHECK(status->attributes.is_link);
	CHECK(status->attributes.reparse_tag == IO_REPARSE_TAG_MOUNT_POINT);
	CHECK(get_entry_status(linkedFilePath, link_behavior::do_not_follow)); // Reached through the junction
	CHECK(read_link(linkPath) == definition);

	SECTION("an extended-length target is accepted and stored as given")
	{
		REQUIRE(remove_entry(linkPath));
		const link_definition extended{ .type = link_type::junction, .target = LR"(\\?\)" + definition.target };
		REQUIRE(create_link(linkPath, extended));
		CHECK(get_entry_status(linkedFilePath, link_behavior::do_not_follow));
		CHECK(read_link(linkPath) == extended);
	}

	REQUIRE(remove_entry(linkPath));
	CHECK(get_entry_status(targetFilePath, link_behavior::do_not_follow)); // Removing the junction leaves its target
	REQUIRE(remove_entry(targetFilePath));
	REQUIRE(remove_entry(targetPath));
}

TEST_CASE("Windows read_link refuses a reparse point it cannot report as a link definition", "[fs][link][windows]")
{
	if (test_volume::linksUnsupported())
		return;

	static constexpr char placeholderPath[] = "read-link-placeholder.file";
	static constexpr char surrogatePath[] = "read-link-surrogate.file";
	(void)remove_entry(placeholderPath);
	(void)remove_entry(surrogatePath);
	REQUIRE(createEmptyFile(placeholderPath));
	REQUIRE(createEmptyFile(surrogatePath));

	// A reparse point that is not a link, as a cloud placeholder is
	REQUIRE(setThirdPartyReparsePoint(L"read-link-placeholder.file", 0x00007A11));
	const auto placeholder = read_link(placeholderPath);
	REQUIRE_FALSE(placeholder);
	CHECK(placeholder.error().native_code == ERROR_NOT_A_REPARSE_POINT);

	// A link of a kind create_link() cannot recreate: the name-surrogate bit on an unknown tag
	REQUIRE(setThirdPartyReparsePoint(L"read-link-surrogate.file", 0x20007A12));
	const auto status = get_entry_status(surrogatePath, link_behavior::do_not_follow);
	REQUIRE(status);
	REQUIRE(status->attributes.is_link);
	const auto surrogate = read_link(surrogatePath);
	REQUIRE_FALSE(surrogate);
	CHECK(surrogate.error().native_code == ERROR_NOT_SUPPORTED);

	REQUIRE(remove_entry(placeholderPath));
	REQUIRE(remove_entry(surrogatePath));
}

#endif

TEST_CASE("set_times rejects a link behavior outside the enum", "[fs][link]")
{
	static constexpr char filePath[] = "set-times-invalid-behavior.file";
	(void)remove_entry(filePath);
	REQUIRE(createEmptyFile(filePath));

	CHECK_FALSE(set_times(filePath, entry_times{ .last_write = timestamp{ .seconds = 1'234'567'890 } }, static_cast<link_behavior>(7)));

	REQUIRE(remove_entry(filePath));
}

TEST_CASE("create_link fails on a filesystem without links and leaves nothing behind", "[fs][link]")
{
	if (!test_volume::isFatFamily())
		return;

	static constexpr char targetPath[] = "no-links-target.dir";
	static constexpr char linkPath[] = "no-links.link";
	(void)remove_entry(linkPath);
	(void)remove_entry(targetPath);
	REQUIRE(create_directory(targetPath));

	CHECK_FALSE(create_link(linkPath, link_definition{ .type = link_type::symbolic_link, .target = NATIVE("no-links-target.dir") }));
	CHECK_FALSE(get_entry_status(linkPath, link_behavior::do_not_follow));
#ifdef _WIN32
	// A junction starts as a directory, which this filesystem can hold
	CHECK_FALSE(create_link(linkPath, link_definition{ .type = link_type::junction, .target = absolutePath(L"no-links-target.dir") }));
	CHECK_FALSE(get_entry_status(linkPath, link_behavior::do_not_follow));
#endif

	REQUIRE(remove_entry(targetPath));
}

TEST_CASE("set_times without following changes the link's own times and leaves its target", "[fs][link]")
{
	if (test_volume::linksUnsupported())
		return;

	static constexpr char targetPath[] = "set-times-link-target.dir";
	static constexpr char linkPath[] = "set-times-link.dir";
	(void)remove_entry(linkPath);
	(void)remove_entry(targetPath);
	REQUIRE(create_directory(targetPath));

	const entry_times targetTimes{ .last_write = timestamp{ .seconds = 1'234'567'890 } };
	REQUIRE(set_times(targetPath, targetTimes));

	// A junction on Windows: it needs no privilege, and it carries times of its own as a symbolic link does
#ifdef _WIN32
	REQUIRE(create_link(linkPath, link_definition{ .type = link_type::junction, .target = absolutePath(L"set-times-link-target.dir") }));
#else
	REQUIRE(create_link(linkPath, link_definition{ .type = link_type::symbolic_link, .target = "set-times-link-target.dir" }));
#endif

	entry_times linkTimes{ .last_write = timestamp{ .seconds = 1'500'000'000, .nanoseconds = 500 } };
	if constexpr (creation_time_settable)
		linkTimes.creation = timestamp{ .seconds = 1'400'000'000 };
	REQUIRE(set_times(linkPath, linkTimes, link_behavior::do_not_follow));

	const auto link = get_entry_status(linkPath, link_behavior::do_not_follow);
	REQUIRE(link);
	test_volume::checkStoredTimes(link->times, linkTimes);

	const auto target = get_entry_status(targetPath, link_behavior::do_not_follow);
	REQUIRE(target);
	test_volume::checkStoredTimes(target->times, targetTimes);

	REQUIRE(remove_entry(linkPath));
	REQUIRE(remove_entry(targetPath));
}
