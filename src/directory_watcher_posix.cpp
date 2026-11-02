#include "directory_watcher_posix.hpp"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <utility>

namespace thin_io {

namespace {

[[nodiscard]] int pollMilliseconds(const std::chrono::milliseconds timeout) noexcept
{
	if (timeout.count() < 0)
		return -1;
	return static_cast<int>(std::min<std::chrono::milliseconds::rep>(timeout.count(), INT_MAX));
}

[[nodiscard]] bool isUnder(const std::string_view path, const std::string_view directory) noexcept
{
	return !directory.empty() && path.size() > directory.size() && isSameOrUnder(path, directory);
}

} // namespace

bool setNonBlockingAndCloseOnExec(const int descriptor) noexcept
{
	const int statusFlags = ::fcntl(descriptor, F_GETFL);
	return statusFlags >= 0 && ::fcntl(descriptor, F_SETFL, statusFlags | O_NONBLOCK) == 0 && ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0;
}

std::string joinedPath(const std::string_view directory, const std::string_view name)
{
	if (directory.empty())
		return std::string{ name };
	std::string path{ directory };
	if (!path.ends_with('/'))
		path += '/';
	path += name;
	return path;
}

bool isSameOrUnder(const std::string_view path, const std::string_view directory) noexcept
{
	return path.starts_with(directory) && (path.size() == directory.size() || path[directory.size()] == '/');
}

bool isExcluded(const std::string_view relativePath, const std::vector<native_string>& excludedDirectories) noexcept
{
	return std::ranges::any_of(excludedDirectories, [relativePath](const native_string& directory) {
		return !directory.empty() && isSameOrUnder(relativePath, directory);
	});
}

std::vector<native_string> normalizedRelativeDirectories(std::vector<native_string> directories)
{
	for (native_string& directory : directories)
	{
		while (directory.ends_with('/'))
			directory.pop_back();
	}
	return directories;
}

void appendUnlessExcluded(watch_event event, const std::vector<native_string>& excludedDirectories, std::vector<watch_event>& events)
{
	const auto under = [&excludedDirectories](const std::string_view path) {
		return std::ranges::any_of(excludedDirectories, [path](const native_string& directory) { return isUnder(path, directory); });
	};

	if ((event.kind == watch_event_kind::modified || event.kind == watch_event_kind::metadata_changed)
		&& std::ranges::find(excludedDirectories, event.path) != excludedDirectories.end())
		return;

	const bool pathExcluded = under(event.path);
	if (event.kind == watch_event_kind::renamed)
	{
		const bool oldPathExcluded = under(event.old_path);
		if (pathExcluded && oldPathExcluded)
			return;
		if (oldPathExcluded)
		{
			event.kind = watch_event_kind::created;
			event.old_path.clear();
		}
		else if (pathExcluded)
		{
			event.kind = watch_event_kind::removed;
			event.path = std::move(event.old_path);
			event.old_path.clear();
		}
	}
	else if (pathExcluded)
		return;

	events.push_back(std::move(event));
}

filesystem_result<bool> waitUntilReadable(const int descriptor, const std::chrono::milliseconds timeout)
{
	pollfd polled{ .fd = descriptor, .events = POLLIN, .revents = 0 };
	for (;;)
	{
		const int ready = ::poll(&polled, 1, pollMilliseconds(timeout));
		if (ready >= 0)
			return ready > 0;
		if (errno != EINTR) [[unlikely]]
			return std::unexpected{ capture_last_filesystem_error() };
	}
}

void drainDescriptor(const int descriptor) noexcept
{
	std::array<char, 4096> discarded;
	for (;;)
	{
		const ssize_t bytes = ::read(descriptor, discarded.data(), discarded.size());
		if (bytes <= 0 && (bytes == 0 || errno != EINTR))
			return;
	}
}

filesystem_result<waker> waker::create()
{
	int ends[2]{ -1, -1 };
	if (::pipe(ends) != 0) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };

	waker created{ ends[0], ends[1] };
	if (!setNonBlockingAndCloseOnExec(ends[0]) || !setNonBlockingAndCloseOnExec(ends[1])) [[unlikely]]
		return std::unexpected{ capture_last_filesystem_error() };
	return created;
}

waker::waker(const native_wait_handle readEnd, const int writeEnd) noexcept : _handle{ readEnd }, _writeEnd{ writeEnd } {}

waker::waker(waker&& other) noexcept : _handle{ std::exchange(other._handle, -1) }, _writeEnd{ std::exchange(other._writeEnd, -1) } {}

waker& waker::operator=(waker&& other) noexcept
{
	if (this != &other)
	{
		if (_handle >= 0)
			::close(_handle);
		if (_writeEnd >= 0)
			::close(_writeEnd);
		_handle = std::exchange(other._handle, -1);
		_writeEnd = std::exchange(other._writeEnd, -1);
	}
	return *this;
}

waker::~waker()
{
	if (_handle >= 0)
		::close(_handle);
	if (_writeEnd >= 0)
		::close(_writeEnd);
}

// A full pipe already holds a signal: the failed write loses nothing.
void waker::signal() noexcept
{
	if (_writeEnd >= 0)
		(void)!::write(_writeEnd, "", 1);
}

filesystem_result<wait_result> wait_for_any(const std::span<directory_watcher* const> watchers, waker* const wakeUp,
	const std::chrono::milliseconds timeout)
{
	std::vector<pollfd> polled;
	polled.reserve(watchers.size() + 1);
	if (wakeUp != nullptr)
		polled.push_back(pollfd{ .fd = wakeUp->_handle, .events = POLLIN, .revents = 0 });
	for (directory_watcher* const watcher : watchers)
		polled.push_back(pollfd{ .fd = watcher != nullptr ? watcher->wait_handle() : -1, .events = POLLIN, .revents = 0 });

	if (polled.empty() || std::ranges::any_of(polled, [](const pollfd& descriptor) { return descriptor.fd < 0; })) [[unlikely]]
		return std::unexpected{ filesystem_error{ .native_code = EINVAL } };

	int ready = 0;
	while ((ready = ::poll(polled.data(), static_cast<nfds_t>(polled.size()), pollMilliseconds(timeout))) < 0)
	{
		if (errno != EINTR) [[unlikely]]
			return std::unexpected{ capture_last_filesystem_error() };
	}

	wait_result result;
	if (ready == 0)
		return result;

	const size_t firstWatcher = wakeUp != nullptr ? 1 : 0;
	if (wakeUp != nullptr && (polled.front().revents & POLLIN) != 0)
	{
		drainDescriptor(wakeUp->_handle); // Consumes every signal: they coalesce
		result.woken = true;
	}
	for (size_t i = 0; i < watchers.size(); ++i)
	{
		if ((polled[firstWatcher + i].revents & POLLIN) != 0)
			result.ready.push_back(watchers[i]);
	}
	return result;
}

} // namespace thin_io
