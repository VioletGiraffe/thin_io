#pragma once

// Every test includes Catch through this header: a StringMaker must be visible in every file that prints its type.

#define CATCH_CONFIG_ENABLE_OPTIONAL_STRINGMAKER
#include "catch2/catch.hpp"

#include "filesystem_error.hpp"
#include "filesystem_types.hpp"

#include <iomanip>
#include <sstream>
#include <string>

namespace Catch {

template<>
struct StringMaker<thin_io::entry_kind> {
	static std::string convert(const thin_io::entry_kind kind)
	{
		switch (kind)
		{
		case thin_io::entry_kind::unknown:
			return "unknown";
		case thin_io::entry_kind::regular_file:
			return "regular_file";
		case thin_io::entry_kind::directory:
			return "directory";
		case thin_io::entry_kind::other:
			return "other";
		}

		return "entry_kind(" + std::to_string(static_cast<int>(kind)) + ')';
	}
};

template<>
struct StringMaker<thin_io::timestamp> {
	// Not a decimal fraction: nanoseconds is a positive offset even when seconds is negative
	static std::string convert(const thin_io::timestamp& t)
	{
		return std::to_string(t.seconds) + " s + " + std::to_string(t.nanoseconds) + " ns";
	}
};

template<>
struct StringMaker<thin_io::entry_times> {
	static std::string convert(const thin_io::entry_times& times)
	{
		return "{ creation: " + Detail::stringify(times.creation)
			+ ", last_access: " + Detail::stringify(times.last_access)
			+ ", last_write: " + Detail::stringify(times.last_write) + " }";
	}
};

template<>
struct StringMaker<thin_io::file_permissions> {
	static std::string convert(const thin_io::file_permissions& permissions)
	{
#ifdef _WIN32
		std::string flags;
		if (permissions.read_only)
			flags += " read_only";
		if (permissions.hidden)
			flags += " hidden";
		if (permissions.system)
			flags += " system";
		return '{' + flags + " }";
#else
		std::ostringstream stream;
		stream << '0' << std::oct << permissions.mode;
		return stream.str();
#endif
	}
};

template<>
struct StringMaker<thin_io::entry_attributes> {
	static std::string convert(const thin_io::entry_attributes& attributes)
	{
		std::ostringstream stream;
		stream << "{ " << Detail::stringify(attributes.kind);
		if (attributes.is_link)
			stream << " link";
		if (attributes.sparse)
			stream << " sparse";
		if (attributes.compressed)
			stream << " compressed";
		if (attributes.hidden)
			stream << " hidden";
		if (attributes.reparse_tag != 0)
			stream << " reparse_tag: 0x" << std::hex << attributes.reparse_tag;
		stream << " }";
		return stream.str();
	}
};

template<>
struct StringMaker<thin_io::entry_status> {
	static std::string convert(const thin_io::entry_status& status)
	{
		return "{ attributes: " + Detail::stringify(status.attributes)
			+ ", logical_size: " + Detail::stringify(status.logical_size)
			+ ", times: " + Detail::stringify(status.times)
			+ ", permissions: " + Detail::stringify(status.permissions) + " }";
	}
};

template<>
struct StringMaker<thin_io::directory_entry> {
	static std::string convert(const thin_io::directory_entry& entry)
	{
		return "{ name: " + Detail::stringify(entry.name)
			+ ", status: " + StringMaker<thin_io::entry_status>::convert(entry)
			+ ", link_target: " + Detail::stringify(entry.link_target) + " }";
	}
};

template<>
struct StringMaker<thin_io::entry_identity> {
	static std::string convert(const thin_io::entry_identity& identity)
	{
		std::ostringstream stream;
		stream << "{ filesystem: " << identity.filesystem << ", entry: " << std::hex << std::setfill('0');
		for (const uint8_t byte : identity.entry)
			stream << std::setw(2) << static_cast<unsigned int>(byte);
		stream << " }";
		return stream.str();
	}
};

template<>
struct StringMaker<thin_io::entry_metadata> {
	static std::string convert(const thin_io::entry_metadata& metadata)
	{
		return "{ attributes: " + Detail::stringify(metadata.attributes)
			+ ", logical_size: " + std::to_string(metadata.logical_size)
			+ ", allocated_size: " + std::to_string(metadata.allocated_size)
			+ ", hard_link_count: " + std::to_string(metadata.hard_link_count)
			+ ", identity: " + Detail::stringify(metadata.identity)
			+ ", mount_id: " + Detail::stringify(metadata.mount_id) + " }";
	}
};

template<>
struct StringMaker<thin_io::filesystem_space> {
	static std::string convert(const thin_io::filesystem_space& space)
	{
		return "{ capacity: " + std::to_string(space.capacity)
			+ ", free: " + std::to_string(space.free)
			+ ", available: " + std::to_string(space.available)
			+ ", identity: " + Detail::stringify(space.identity) + " }";
	}
};

template<>
struct StringMaker<thin_io::filesystem_error> {
	static std::string convert(const thin_io::filesystem_error& error)
	{
		return thin_io::format_filesystem_error(error);
	}
};

} // namespace Catch
