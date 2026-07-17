#pragma once

#include <array>
#include <optional>
#include <stdint.h>
#include <string>

namespace thin_io {

#ifdef _WIN32
using native_char = wchar_t;
using native_string = std::wstring;
#else
using native_char = char;
using native_string = std::string; // Native POSIX names are byte strings and need not contain valid UTF-8.
#endif

enum class entry_kind : uint8_t {
	unknown,
	regular_file,
	directory,
	other
};

struct entry_attributes {
	entry_kind kind = entry_kind::unknown;
	bool is_link = false; // POSIX symbolic link or Windows reparse point.
	bool sparse = false;
	bool compressed = false;
	uint32_t reparse_tag = 0; // Zero when the entry is not a Windows reparse point.

	[[nodiscard]] bool operator==(const entry_attributes&) const noexcept = default;
};

struct directory_entry {
	native_string name; // Relative entry name, never a rebuilt absolute path.
	entry_attributes attributes;
	std::optional<uint64_t> logical_size;

	[[nodiscard]] bool operator==(const directory_entry&) const noexcept = default;
};

using filesystem_identity = uint64_t;

// Windows uses the complete FILE_ID_128 value; POSIX stores the inode in the same zero-padded representation.
struct entry_identity {
	filesystem_identity filesystem = 0; // Windows volume serial number or POSIX device ID.
	std::array<uint8_t, 16> entry{};

	[[nodiscard]] bool operator==(const entry_identity&) const noexcept = default;
};

struct entry_info {
	entry_attributes attributes;
	uint64_t logical_size = 0;
	uint64_t allocated_size = 0;
	uint64_t hard_link_count = 0;
	std::optional<entry_identity> identity;

	[[nodiscard]] bool operator==(const entry_info&) const noexcept = default;
};

enum class link_behavior : uint8_t {
	follow,
	do_not_follow
};

} // namespace thin_io
