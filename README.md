# Thin IO

A lightweight cross-platform low-level C++ file library built directly on native system APIs, without `<stdio>`,
`<fstream>`, or Qt. It provides synchronous binary file I/O, mapping, resizing, preallocation, deletion, and file or
directory timestamps. Higher-level concerns such as serialization, recursive traversal, and asynchronous execution
stay outside the library.

The main file interface is in [`src/file_interface.hpp`](src/file_interface.hpp). Timestamp operations and synchronous
single-directory discovery and accounting operations are declared in [`src/fs.hpp`](src/fs.hpp).

## Native path contract

Path-taking operations preserve the platform's native representation and expose only the conversion conveniences
documented below.

### Windows

Each public path-taking API has a native `const wchar_t*` overload and a `const char*` convenience overload. The wide
overload accepts UTF-16 directly. The narrow overload requires UTF-8, converts it once, and then uses the same native
wide operation. Native-wide callers therefore do not make a UTF-8 round trip.

Except for opaque extended input described below, forward slashes are converted to backslashes.

Ordinary absolute drive and UNC paths are prepared for Win32 extended-length operation, independently of the
process-wide long-path policy:

- `C:\data\file.bin` becomes `\\?\C:\data\file.bin`.
- `\\server\share\file.bin` becomes `\\?\UNC\server\share\file.bin`.
- Repeated separators are collapsed, `.` and `..` components are resolved without moving above the drive or share
  root, trailing spaces and periods are removed from ordinary components, and one trailing separator is retained.

Relative paths, drive-relative paths such as `C:file.bin`, and rooted paths without a drive remain unprefixed so
Win32 resolves them in their normal context. These non-absolute paths do not otherwise receive lexical normalization.

Input already beginning with `\\?\` is an opaque extended Win32 path: it is neither normalized nor prefixed again.
Device and raw NT namespaces (`\\.\`, `\??\`, and `\Device\`) are not accepted as filesystem paths and fail with
`ERROR_NOT_SUPPORTED`.

Both the incoming UTF-16 path, including UTF-8 input after decoding, and the prepared path may contain at most 32,767
UTF-16 code units excluding the terminating NUL. Input beyond that limit is rejected before lexical normalization.
Path-preparation failures retain their Win32 error code. File operations expose it through the existing native error
mechanism, for example `thin_io::file::error_code()`; discovery and accounting operations return it in
`filesystem_error`.

```cpp
thin_io::file nativeFile;
nativeFile.open(L"C:\\data\\\u0444\u0430\u0439\u043B.bin", thin_io::file::access_mode::Read);

thin_io::file utf8File;
utf8File.open("relative/report.bin", thin_io::file::access_mode::ReadWrite);
```

### POSIX

`const char*` is the native byte path. Paths are passed to the operating system without transcoding, and a native
path or returned filename is not assumed to be valid UTF-8. POSIX builds do not expose artificial wide-path
overloads.

## Filesystem discovery and accounting

Each call performs one synchronous filesystem operation. These APIs do not recurse, schedule work, apply traversal
policy, or depend on Qt.

### Results and errors

`list_directory()`, `get_entry_metadata()`, and `get_filesystem_space()` return
`filesystem_result<T>`, an alias for `std::expected<T, filesystem_error>`. `filesystem_error::native_code` is the
captured Win32 error code or POSIX `errno` value. Failures are captured before cleanup or another native call can
overwrite the thread-local error state.

`format_filesystem_error()` formats the numeric code followed by the platform description when one is available. The
numeric code remains the value callers should branch on. The older timestamp and file APIs retain their existing
bool/optional results and native last-error access for compatibility.

### Directory enumeration

`list_directory()` lists exactly the immediate children of one directory. It excludes `.` and `..`, returns names
relative to the listed directory in `native_string`, and never returns a partial vector as a successful result. On
Windows the names are UTF-16; on POSIX they preserve the bytes supplied by the filesystem.

Each `directory_entry` contains its basic kind and the cheaply available sparse, compressed, and link/reparse
attributes. `is_link` is true for POSIX symbolic links and Windows reparse points; `reparse_tag` retains the exact
Windows tag and is zero otherwise. `logical_size` is optional because directory enumeration does not provide it
reliably on every platform.

### Detailed entry metadata

`get_entry_metadata()` requires an explicit `link_behavior`: `follow` describes the target, while `do_not_follow`
describes the symbolic link or reparse entry itself. The result includes logical size, allocated size, hard-link
count, attributes, an optional stable entry identity, and an optional scan-local mount identity. Linux obtains the
mount identity from `statx`, which distinguishes bind mounts even though they retain the target's device ID. Other
platforms use the available filesystem or volume identity. Mount identity is for traversal boundaries within the
current mount namespace; it is not a persistent cross-scan identifier.

Allocated size follows the native accounting definition:

- Windows uses the allocation size reported by `FILE_STANDARD_INFO`; sparse and compressed regular files use
  `FILE_COMPRESSION_INFO::CompressedFileSize` so unallocated ranges and compression are reflected.
- POSIX uses `st_blocks * 512`, as specified for `stat` rather than assuming the filesystem block size.

`entry_identity` combines a filesystem identity with a 16-byte entry identity. Windows stores the full `FILE_ID_128`
and volume serial number where the filesystem exposes them. POSIX stores the device ID and a zero-padded inode value.
`thin_io` reports hard links and their identity but deliberately does not decide how a caller should count them.

### Filesystem space

`get_filesystem_space()` accepts a directory path, follows symbolic links and reparse points, and reports the space
and identity of the containing filesystem:

- `capacity` is the total capacity visible to the caller; a Windows quota may reduce it.
- `free` is total filesystem/volume free space, including space unavailable to the caller.
- `available` is the space available to the caller after quotas and filesystem reservations.

`available` cannot exceed `free` or caller-visible `capacity`. On Windows, volume-wide `free` can exceed
quota-reduced `capacity`; callers must not assume `free <= capacity` there. Filesystem identity is optional when a
Windows filesystem does not expose `FILE_ID_128` information.

## Building the library

- The supplied build uses `qmake`; another build system only needs the common headers and the implementation files
  for its target platform.
- The subrepository dependency is used only by the tests to provide Catch2.
