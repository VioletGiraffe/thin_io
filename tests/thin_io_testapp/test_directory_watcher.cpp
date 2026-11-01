#include "catch_thin_io.hpp"
#include "test_volume.hpp"

#include "directory_watcher.hpp"
#include "file.hpp"
#include "fs.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <optional>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace thin_io;
using namespace std::chrono_literals;

namespace {

#ifdef _WIN32
constexpr native_char separator = L'\\';
#else
constexpr native_char separator = '/';
#endif

// Test names are ASCII: widening each character spells the native name, and '/' becomes the native separator.
[[nodiscard]] native_string native(const std::string_view ascii)
{
	native_string result;
	for (const char c : ascii)
		result += c == '/' ? separator : static_cast<native_char>(c);
	return result;
}

[[nodiscard]] std::string narrow(const native_string& path)
{
	std::string result;
	for (const native_char c : path)
		result += static_cast<char>(c);
	return result;
}

void removeTree(const native_string& path)
{
	if (const auto entries = list_directory(path.c_str()))
	{
		for (const directory_entry& entry : *entries)
		{
			const native_string child = path + separator + entry.name;
			if (entry.attributes.kind == entry_kind::directory && !entry.attributes.is_link)
				removeTree(child);
			else
				(void)remove_entry(child.c_str());
		}
	}
	(void)remove_entry(path.c_str());
}

void writeFile(const native_string& path, const std::string_view contents)
{
	file f;
	REQUIRE(f.open(path.c_str(), file::access_mode::Write));
	REQUIRE(f.write(contents.data(), contents.size()) == contents.size());
	REQUIRE(f.close());
}

// A directory in the working directory, empty at the start of the test and removed at its end
class scratch_tree {
public:
	explicit scratch_tree(const std::string_view name) : _root{ native(name) }
	{
		removeTree(_root);
		REQUIRE(create_directory(_root.c_str()));
	}

	~scratch_tree() { removeTree(_root); }

	scratch_tree(const scratch_tree&) = delete;
	scratch_tree& operator=(const scratch_tree&) = delete;

	[[nodiscard]] const native_string& root() const noexcept { return _root; }
	[[nodiscard]] native_string path(const std::string_view relative) const { return _root + separator + native(relative); }

private:
	const native_string _root;
};

[[nodiscard]] std::string describe(const std::vector<watch_event>& events)
{
	static constexpr const char* kindNames[]{ "created", "removed", "modified", "metadata_changed", "renamed", "overflow", "stopped" };
	std::string text;
	for (const watch_event& event : events)
	{
		text += kindNames[static_cast<size_t>(event.kind)];
		if (!event.old_path.empty())
			text += ' ' + narrow(event.old_path) + " ->";
		text += ' ' + narrow(event.path) + '\n';
	}
	return text;
}

[[nodiscard]] bool hasEvent(const std::vector<watch_event>& events, const watch_event_kind kind, const std::string_view relativePath)
{
	const native_string path = native(relativePath);
	return std::ranges::any_of(events, [&](const watch_event& event) { return event.kind == kind && event.path == path; });
}

[[nodiscard]] bool hasEventUnder(const std::vector<watch_event>& events, const std::string_view relativeDirectory)
{
	const native_string prefix = native(relativeDirectory) + separator;
	return std::ranges::any_of(events, [&](const watch_event& event) {
		return event.path.starts_with(prefix) || event.old_path.starts_with(prefix);
	});
}

[[nodiscard]] const watch_event* stopEvent(const std::vector<watch_event>& events)
{
	const auto stop = std::ranges::find(events, watch_event_kind::stopped, &watch_event::kind);
	return stop != events.end() ? &*stop : nullptr;
}

// Reads events until done() holds for everything read so far, or 10 s pass
[[nodiscard]] std::vector<watch_event> readUntil(directory_watcher& watcher, const std::function<bool(const std::vector<watch_event>&)>& done)
{
	std::vector<watch_event> events;
	const auto deadline = std::chrono::steady_clock::now() + 10s;
	while (!done(events))
	{
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
		if (remaining <= 0ms)
			break;

		const auto pending = watcher.wait_for_events(remaining);
		REQUIRE(pending);
		if (!*pending)
			break;

		auto batch = watcher.read_events();
		REQUIRE(batch);
		events.insert(events.end(), std::make_move_iterator(batch->begin()), std::make_move_iterator(batch->end()));
	}

	INFO(describe(events));
	CHECK(done(events));
	return events;
}

[[nodiscard]] std::vector<watch_event> readUntilEvent(directory_watcher& watcher, const watch_event_kind kind, const std::string_view relativePath)
{
	return readUntil(watcher, [&](const std::vector<watch_event>& events) { return hasEvent(events, kind, relativePath); });
}

// Creates a marker file and reads until it is reported: every event for an earlier change has then been read too.
[[nodiscard]] std::vector<watch_event> readThroughMarker(directory_watcher& watcher, const scratch_tree& tree, const std::string_view marker)
{
	writeFile(tree.path(marker), {});
	return readUntilEvent(watcher, watch_event_kind::created, marker);
}

[[nodiscard]] directory_watcher watchTree(const native_string& root, watch_options options = {})
{
	auto watcher = directory_watcher::watch(root.c_str(), std::move(options));
	REQUIRE(watcher);
	return std::move(*watcher);
}

} // namespace

TEST_CASE("directory_watcher reports a file created, modified and removed", "[watcher]")
{
	scratch_tree tree{ "watcher-file-events" };
	directory_watcher watcher = watchTree(tree.root(), { .recursive = false });
	CHECK(watcher.coverage() == watch_coverage::complete);

	writeFile(tree.path("a.txt"), "one");
	(void)readUntilEvent(watcher, watch_event_kind::created, "a.txt");

	writeFile(tree.path("a.txt"), "three");
	(void)readUntilEvent(watcher, watch_event_kind::modified, "a.txt");

	REQUIRE(remove_entry(tree.path("a.txt").c_str()));
	(void)readUntilEvent(watcher, watch_event_kind::removed, "a.txt");
}

TEST_CASE("directory_watcher reports a rename as one event with both names", "[watcher]")
{
	scratch_tree tree{ "watcher-rename" };
	writeFile(tree.path("old.txt"), "x");
	directory_watcher watcher = watchTree(tree.root(), { .recursive = false });

	REQUIRE(rename_entry(tree.path("old.txt").c_str(), tree.path("new.txt").c_str(), existing_destination::fail));
	const auto events = readUntil(watcher, [](const std::vector<watch_event>& read) {
		return std::ranges::any_of(read, [](const watch_event& event) {
			return event.kind == watch_event_kind::renamed && event.old_path == native("old.txt") && event.path == native("new.txt");
		});
	});
	CHECK_FALSE(hasEvent(events, watch_event_kind::removed, "old.txt"));
}

TEST_CASE("directory_watcher reports a same-size rewrite and an attribute-only change", "[watcher]")
{
	scratch_tree tree{ "watcher-in-place" };
	writeFile(tree.path("a.txt"), "aaaa");
	directory_watcher watcher = watchTree(tree.root(), { .recursive = false });

	writeFile(tree.path("a.txt"), "bbbb");
	(void)readUntilEvent(watcher, watch_event_kind::modified, "a.txt");
	(void)readThroughMarker(watcher, tree, "marker");

	const native_string path = tree.path("a.txt");
#ifdef _WIN32
	REQUIRE(set_permissions(path.c_str(), file_permissions{ .read_only = true }, link_behavior::do_not_follow));
	// Windows reports metadata as a modification.
	(void)readUntilEvent(watcher, watch_event_kind::modified, "a.txt");
	REQUIRE(set_permissions(path.c_str(), file_permissions{}, link_behavior::do_not_follow));
#else
	if (test_volume::permissionBitsUnsupported())
		return;
	REQUIRE(set_permissions(path.c_str(), file_permissions{ .mode = 0600 }, link_behavior::do_not_follow));
	(void)readUntilEvent(watcher, watch_event_kind::metadata_changed, "a.txt");
#endif
}

TEST_CASE("A recursive directory_watcher reports a file made at once in new nested directories", "[watcher]")
{
	scratch_tree tree{ "watcher-nested" };
	directory_watcher watcher = watchTree(tree.root(), { .recursive = true });

	REQUIRE(create_directory(tree.path("a").c_str()));
	REQUIRE(create_directory(tree.path("a/b").c_str()));
	REQUIRE(create_directory(tree.path("a/b/c").c_str()));
	writeFile(tree.path("a/b/c/f.txt"), "x");
	(void)readUntilEvent(watcher, watch_event_kind::created, "a/b/c/f.txt");
}

TEST_CASE("directory_watcher reports a new directory's kind where the platform supplies it", "[watcher]")
{
	scratch_tree tree{ "watcher-entry-kind" };
	directory_watcher watcher = watchTree(tree.root(), { .recursive = false });

	REQUIRE(create_directory(tree.path("sub").c_str()));
	const auto events = readUntilEvent(watcher, watch_event_kind::created, "sub");
	const auto created = std::ranges::find_if(events, [](const watch_event& event) {
		return event.kind == watch_event_kind::created && event.path == native("sub");
	});
	REQUIRE(created != events.end());
	// Windows reads the kind from the extended information, which FAT32 and exFAT may not supply.
	if (test_volume::isFatFamily())
		WARN("Kind reported for a new directory here: " << Catch::Detail::stringify(created->entry));
	else
		CHECK(created->entry == entry_kind::directory);
}

TEST_CASE("directory_watcher reports a move across the root's boundary as a creation or a removal", "[watcher]")
{
	scratch_tree tree{ "watcher-boundary" };
	const scratch_tree outside{ "watcher-boundary-outside" };
	writeFile(outside.path("in.txt"), "x");
	writeFile(tree.path("out.txt"), "x");
	directory_watcher watcher = watchTree(tree.root(), { .recursive = false });

	REQUIRE(rename_entry(outside.path("in.txt").c_str(), tree.path("in.txt").c_str(), existing_destination::fail));
	(void)readUntilEvent(watcher, watch_event_kind::created, "in.txt");

	REQUIRE(rename_entry(tree.path("out.txt").c_str(), outside.path("out.txt").c_str(), existing_destination::fail));
	(void)readUntilEvent(watcher, watch_event_kind::removed, "out.txt");
}

TEST_CASE("A non-recursive directory_watcher reports nothing below the root's children", "[watcher]")
{
	scratch_tree tree{ "watcher-flat" };
	REQUIRE(create_directory(tree.path("sub").c_str()));
	directory_watcher watcher = watchTree(tree.root(), { .recursive = false });

	writeFile(tree.path("sub/inner.txt"), "x");
	const auto events = readThroughMarker(watcher, tree, "marker");
	INFO(describe(events));
	CHECK_FALSE(hasEventUnder(events, "sub"));
}

TEST_CASE("directory_watcher reports nothing below an excluded directory, until it leaves the list", "[watcher]")
{
	scratch_tree tree{ "watcher-excluded" };
	REQUIRE(create_directory(tree.path("skip").c_str()));
	directory_watcher watcher = watchTree(tree.root(), { .recursive = true, .excluded_directories = { native("skip") } });

	writeFile(tree.path("skip/hidden.txt"), "x");
	// Windows reports the deletion as a modification of skip itself, too.
	REQUIRE(remove_entry(tree.path("skip/hidden.txt").c_str()));
	const auto excludedEvents = readThroughMarker(watcher, tree, "marker");
	INFO(describe(excludedEvents));
	CHECK_FALSE(hasEventUnder(excludedEvents, "skip"));
	CHECK_FALSE(hasEvent(excludedEvents, watch_event_kind::modified, "skip"));

	watcher.set_excluded_directories({});
	writeFile(tree.path("skip/visible.txt"), "x");
	(void)readUntilEvent(watcher, watch_event_kind::created, "skip/visible.txt");
}

TEST_CASE("directory_watcher::discard_events drains without building events", "[watcher]")
{
	scratch_tree tree{ "watcher-discard" };
	directory_watcher watcher = watchTree(tree.root(), { .recursive = false });

	writeFile(tree.path("a.txt"), "x");
	const auto pending = watcher.wait_for_events(10s);
	REQUIRE(pending);
	REQUIRE(*pending);

	auto discarded = watcher.discard_events();
	REQUIRE(discarded);
	CHECK(discarded->any);
	CHECK_FALSE(discarded->stop);

	// The write may complete in more than one notification: drain until quiet.
	for (int round = 0; round < 20; ++round)
	{
		const auto more = watcher.wait_for_events(300ms);
		REQUIRE(more);
		if (!*more)
			break;
		REQUIRE(watcher.discard_events());
	}

	const auto quiet = watcher.wait_for_events(0ms);
	REQUIRE(quiet);
	CHECK_FALSE(*quiet);
}

TEST_CASE("directory_watcher stops with root_lost when the root is deleted", "[watcher][root]")
{
	scratch_tree tree{ "watcher-root-deleted" };
	directory_watcher watcher = watchTree(tree.root());

	REQUIRE(remove_entry(tree.root().c_str()));
	const auto events = readUntil(watcher, [](const std::vector<watch_event>& read) { return stopEvent(read) != nullptr; });
	const watch_event* const stop = stopEvent(events);
	REQUIRE(stop);
	CHECK(stop->reason == stop_reason::root_lost);
}

TEST_CASE("directory_watcher stops with root_lost when the root is moved", "[watcher][root]")
{
	scratch_tree tree{ "watcher-root-moved" };
	const native_string movedRoot = native("watcher-root-moved-away");
	removeTree(movedRoot);
	directory_watcher watcher = watchTree(tree.root());

	REQUIRE(rename_entry(tree.root().c_str(), movedRoot.c_str(), existing_destination::fail));
	const auto events = readUntil(watcher, [](const std::vector<watch_event>& read) { return stopEvent(read) != nullptr; });
	const watch_event* const stop = stopEvent(events);
	REQUIRE(stop);
	CHECK(stop->reason == stop_reason::root_lost);

	removeTree(movedRoot);
}

TEST_CASE("directory_watcher stops with root_replaced when another directory takes the root's name", "[watcher][root]")
{
	if (test_volume::isFatFamily())
	{
		WARN("FAT and exFAT identify an entry by its position, which a replacement may reuse: test skipped");
		return;
	}

	scratch_tree tree{ "watcher-root-replaced" };
	directory_watcher watcher = watchTree(tree.root());

	// Replaced before the watcher reads: it only ever sees the new directory under the name.
	REQUIRE(remove_entry(tree.root().c_str()));
	REQUIRE(create_directory(tree.root().c_str()));
	const auto events = readUntil(watcher, [](const std::vector<watch_event>& read) { return stopEvent(read) != nullptr; });
	const watch_event* const stop = stopEvent(events);
	REQUIRE(stop);
	CHECK(stop->reason == stop_reason::root_replaced);
}

#ifdef _WIN32
TEST_CASE("Windows: the watched root's parent and grandparent cannot be renamed", "[watcher][root]")
{
	scratch_tree tree{ "watcher-ancestor-lock" };
	REQUIRE(create_directory(tree.path("parent").c_str()));
	REQUIRE(create_directory(tree.path("parent/root").c_str()));
	directory_watcher watcher = watchTree(tree.path("parent/root"));

	const auto parentRenamed = rename_entry(tree.path("parent").c_str(), tree.path("parent-renamed").c_str(), existing_destination::fail);
	REQUIRE_FALSE(parentRenamed);
	CHECK(parentRenamed.error().native_code == ERROR_ACCESS_DENIED);

	const native_string renamedTree = native("watcher-ancestor-lock-renamed");
	const auto grandparentRenamed = rename_entry(tree.root().c_str(), renamedTree.c_str(), existing_destination::fail);
	if (grandparentRenamed)
		REQUIRE(rename_entry(renamedTree.c_str(), tree.root().c_str(), existing_destination::fail));
	REQUIRE_FALSE(grandparentRenamed);
	CHECK(grandparentRenamed.error().native_code == ERROR_ACCESS_DENIED);
}

// Manual: needs removable hardware. Run it alone with THIN_IO_REMOVABLE_DIRECTORY naming a directory on the drive,
// from a working directory elsewhere, then eject or pull out the drive within two minutes.
TEST_CASE("Windows: removing the drive stops the watch with volume_released", "[.][watcher][removable]")
{
	const char* const directory = std::getenv("THIN_IO_REMOVABLE_DIRECTORY");
	REQUIRE(directory != nullptr);
	auto watcher = directory_watcher::watch(directory, { .recursive = false });
	REQUIRE(watcher);

	std::cerr << "Watching " << directory << ": remove the drive now" << std::endl;
	const auto started = std::chrono::steady_clock::now();
	std::vector<watch_event> events;
	while (stopEvent(events) == nullptr && std::chrono::steady_clock::now() - started < 120s)
	{
		const auto pending = watcher->wait_for_events(1s);
		REQUIRE(pending);
		if (!*pending)
			continue;

		auto batch = watcher->read_events();
		REQUIRE(batch);
		events.insert(events.end(), std::make_move_iterator(batch->begin()), std::make_move_iterator(batch->end()));
	}

	const watch_event* const stop = stopEvent(events);
	REQUIRE(stop);
	std::cerr << "Stopped after " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
		<< " ms, reason " << static_cast<int>(stop->reason) << ", error " << stop->error.native_code << std::endl;
	CHECK(stop->reason == stop_reason::volume_released);
}
#endif

TEST_CASE("A directory changing beside the root does not stop the watch", "[watcher][root]")
{
	scratch_tree tree{ "watcher-sibling" };
	directory_watcher watcher = watchTree(tree.root());

	const scratch_tree sibling{ "watcher-sibling-neighbour" };
	const auto events = readThroughMarker(watcher, tree, "marker");
	CHECK(stopEvent(events) == nullptr);
}

TEST_CASE("directory_watcher::release stops the watch with volume_released, after which nothing is pending", "[watcher]")
{
	scratch_tree tree{ "watcher-release" };
	directory_watcher watcher = watchTree(tree.root());

	watcher.release();
	const auto events = watcher.read_events();
	REQUIRE(events);
	REQUIRE(events->size() == 1);
	CHECK(events->front().kind == watch_event_kind::stopped);
	CHECK(events->front().reason == stop_reason::volume_released);

	writeFile(tree.path("after.txt"), "x");
	const auto pending = watcher.wait_for_events(200ms);
	REQUIRE(pending);
	CHECK_FALSE(*pending);
	const auto after = watcher.read_events();
	REQUIRE(after);
	CHECK(after->empty());
}

TEST_CASE("wait_for_any reports every ready watcher, the waker, and a timeout", "[watcher][wait]")
{
	scratch_tree first{ "watcher-wait-first" };
	scratch_tree second{ "watcher-wait-second" };
	directory_watcher firstWatcher = watchTree(first.root(), { .recursive = false });
	directory_watcher secondWatcher = watchTree(second.root(), { .recursive = false });
	directory_watcher* const watchers[]{ &firstWatcher, &secondWatcher };

	auto wakeUp = waker::create();
	REQUIRE(wakeUp);

	const auto idle = wait_for_any(watchers, &*wakeUp, 50ms);
	REQUIRE(idle);
	CHECK_FALSE(idle->woken);
	CHECK(idle->ready.empty());

	// A signal sent before the wait wakes it, once.
	wakeUp->signal();
	wakeUp->signal();
	const auto woken = wait_for_any(watchers, &*wakeUp, 10s);
	REQUIRE(woken);
	CHECK(woken->woken);
	CHECK(woken->ready.empty());
	const auto afterWake = wait_for_any(watchers, &*wakeUp, 50ms);
	REQUIRE(afterWake);
	CHECK_FALSE(afterWake->woken);

	writeFile(first.path("a.txt"), "x");
	writeFile(second.path("b.txt"), "x");
	for (directory_watcher* const watcher : watchers)
	{
		const auto pending = watcher->wait_for_events(10s);
		REQUIRE(pending);
		REQUIRE(*pending);
	}

	const auto both = wait_for_any(watchers, nullptr, 10s);
	REQUIRE(both);
	CHECK_FALSE(both->woken);
	CHECK(both->ready.size() == 2);
}

TEST_CASE("A directory_watcher made on another thread keeps reporting", "[watcher]")
{
	scratch_tree tree{ "watcher-other-thread" };
	std::optional<directory_watcher> watcher;
	// The thread that issued the first read exits before any event arrives.
	std::thread{ [&] { watcher = watchTree(tree.root(), { .recursive = false }); } }.join();
	REQUIRE(watcher);

	writeFile(tree.path("a.txt"), "x");
	(void)readUntilEvent(*watcher, watch_event_kind::created, "a.txt");
}

TEST_CASE("directory_watcher::watch fails for a missing path and for a file", "[watcher]")
{
	scratch_tree tree{ "watcher-invalid-root" };
	writeFile(tree.path("a.txt"), "x");

	const auto missing = directory_watcher::watch(tree.path("missing").c_str(), {});
	REQUIRE_FALSE(missing);
	const auto regularFile = directory_watcher::watch(tree.path("a.txt").c_str(), {});
	REQUIRE_FALSE(regularFile);
#ifdef _WIN32
	CHECK(missing.error().native_code == ERROR_FILE_NOT_FOUND);
	CHECK(regularFile.error().native_code == ERROR_DIRECTORY);
#endif
}
