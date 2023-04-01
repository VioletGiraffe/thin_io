# Thin IO
A lightweight cross-platform low-level C++ file library based directly on the native system API. Not using `<stdio>` or `<fstream>`.

With this library you can create, write, read, and delete files. No extra bloat, no built-in support for serializing high-level types, just binary I/O.

The interface is self-explanatory: https://github.com/VioletGiraffe/thin_io/blob/master/src/file_interface.hpp

## Building the library

* I use `qmake` as the build system for my projects, but you can use any system you want. Compiling the library boils down to compiling the two .cpp files. No setup and no special compiler flags required.
* Contributions of other build system recipies (e. g. CMake) are very welcome.
* You can easily make it header-only, but I decided it against it for my projects in order to not expose the system API headers to every consumer of the library.
* The subrepository dependency is only there for building tests - it provides `Catch2`. You can ignore it. I'll fix it later, use `vcpkg` or something.
