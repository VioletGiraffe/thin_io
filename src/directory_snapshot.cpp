#include "directory_snapshot.hpp"
#include "fs.hpp"

#include <algorithm>
#include <utility>

namespace thin_io {

directory_snapshot::directory_snapshot(std::vector<entry> entries) noexcept : _entries{ std::move(entries) } {}

template <class Character>
filesystem_result<directory_snapshot> directory_snapshot::takeFrom(const Character* const directory, const listing_detail detail)
{
	auto listing = list_directory(directory, detail);
	if (!listing) [[unlikely]]
		return std::unexpected{ listing.error() };

	const auto stateOf = [](const entry_status& status) {
		return entry_state{ .attributes = status.attributes, .logical_size = status.logical_size, .last_write = status.times.last_write,
			.permissions = status.permissions };
	};

	std::vector<entry> entries;
	entries.reserve(listing->size());
	for (directory_entry& listed : *listing)
	{
		entries.push_back(entry{ .name = std::move(listed.name), .state = stateOf(listed),
			.link_target = listed.link_target ? std::optional{ stateOf(*listed.link_target) } : std::nullopt });
	}

	std::ranges::sort(entries, {}, &entry::name);
	return directory_snapshot{ std::move(entries) };
}

filesystem_result<directory_snapshot> directory_snapshot::take(const native_char* const directory, const listing_detail detail)
{
	return takeFrom(directory, detail);
}

#ifdef _WIN32
filesystem_result<directory_snapshot> directory_snapshot::take(const char* const utf8Directory, const listing_detail detail)
{
	return takeFrom(utf8Directory, detail);
}
#endif

} // namespace thin_io
