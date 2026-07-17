# Thin IO

A lightweight cross-platform low-level C++ file library built directly on native system APIs, without `<stdio>`,
`<fstream>`, or Qt. It provides synchronous binary file I/O, mapping, resizing, preallocation, deletion, and file or
directory timestamps. Higher-level concerns such as serialization, recursive traversal, and asynchronous execution
stay outside the library.

The main file interface is in [`src/file_interface.hpp`](src/file_interface.hpp), with timestamp operations declared
in [`src/fs.hpp`](src/fs.hpp).

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
Path-preparation failures are reported through the existing native error mechanism, for example
`thin_io::file::error_code()`.

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

## Building the library

- The supplied build uses `qmake`; another build system only needs the common headers and the implementation files
  for its target platform.
- The subrepository dependency is used only by the tests to provide Catch2.
