#pragma once

#include "filesystem_error.hpp"
#include "filesystem_types.hpp"

#include <optional>
#include <stdint.h>
#include <vector>

namespace thin_io {

// One directory's entries as a listing shows them, for detecting changes by polling where change notifications may
// never arrive: see directory_watcher::coverage(). The caller takes snapshots on its own timer and compares them.
// POSIX: a basic listing holds no sizes or times, so polling for those needs listing_detail::full.
class directory_snapshot {
public:
	[[nodiscard]] static filesystem_result<directory_snapshot> take(const native_char* directory, listing_detail detail);
#ifdef _WIN32
	[[nodiscard]] static filesystem_result<directory_snapshot> take(const char* utf8Directory, listing_detail detail);
#endif

	// Equal when the same names hold entries of the same kind, attributes, permissions, size and modification time.
	// Access and creation times are not compared: reading an entry is not a change.
	// Only snapshots taken with the same listing_detail compare meaningfully.
	[[nodiscard]] bool operator==(const directory_snapshot&) const noexcept = default;

private:
	struct entry_state {
		entry_attributes attributes;
		std::optional<uint64_t> logical_size;
		timestamp last_write;
		std::optional<file_permissions> permissions;

		[[nodiscard]] bool operator==(const entry_state&) const noexcept = default;
	};

	struct entry {
		native_string name;
		entry_state state;
		std::optional<entry_state> link_target; // listing_detail::full, links only

		[[nodiscard]] bool operator==(const entry&) const noexcept = default;
	};

	explicit directory_snapshot(std::vector<entry> entries) noexcept;

	template <class Character>
	[[nodiscard]] static filesystem_result<directory_snapshot> takeFrom(const Character* directory, listing_detail detail);

	std::vector<entry> _entries; // Sorted by name: listing order differs between filesystems
};

} // namespace thin_io
