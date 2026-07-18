#include "catch2/catch.hpp"

#include "filesystem_types.hpp"

#include <type_traits>

using namespace thin_io;

#ifdef _WIN32
static_assert(std::is_same_v<native_char, wchar_t>);
static_assert(std::is_same_v<native_string, std::wstring>);
#else
static_assert(std::is_same_v<native_char, char>);
static_assert(std::is_same_v<native_string, std::string>);
#endif

static_assert(sizeof(entry_attributes) == 8);

TEST_CASE("directory entries distinguish unavailable size from zero", "[filesystem-types]")
{
	directory_entry entry;
	CHECK_FALSE(entry.logical_size);

	entry.logical_size = 0;
	REQUIRE(entry.logical_size);
	CHECK(*entry.logical_size == 0);
}

TEST_CASE("entry attributes retain uniform link state and native reparse detail", "[filesystem-types]")
{
	entry_attributes attributes;
	attributes.kind = entry_kind::directory;
	attributes.is_link = true;
	attributes.reparse_tag = 0xA000'000Cu;

	CHECK(attributes.kind == entry_kind::directory);
	CHECK(attributes.is_link);
	CHECK(attributes.reparse_tag == 0xA000'000Cu);
}

TEST_CASE("entry identity compares the filesystem and all 128 entry bits", "[filesystem-types]")
{
	entry_identity first;
	first.filesystem = 7;
	first.entry[15] = 0x80;

	entry_identity same = first;
	CHECK(same == first);

	same.entry[15] = 0;
	CHECK_FALSE(same == first);

	same = first;
	++same.filesystem;
	CHECK_FALSE(same == first);
}

TEST_CASE("entry metadata represents unavailable identity separately", "[filesystem-types]")
{
	entry_metadata metadata;
	CHECK_FALSE(metadata.identity);
	CHECK_FALSE(metadata.mount_id);
	CHECK(metadata.logical_size == 0);
	CHECK(metadata.allocated_size == 0);
	CHECK(metadata.hard_link_count == 0);
}

#ifndef _WIN32
TEST_CASE("native POSIX names preserve bytes that are not valid UTF-8", "[filesystem-types]")
{
	const native_string name{"\xFF", 1};
	REQUIRE(name.size() == 1);
	CHECK(static_cast<unsigned char>(name[0]) == 0xFF);
}
#endif
