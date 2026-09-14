TEMPLATE = lib
CONFIG += staticlib

CONFIG -= qt
!win*:CONFIG -= flat

CONFIG += strict_c++

exists(../global.pri){
	include(../global.pri)
} else {
	CONFIG += c++2b
}

mac* | linux* | freebsd {
	CONFIG(release, debug|release):CONFIG *= Release optimize_full
	CONFIG(debug, debug|release):CONFIG *= Debug
}

Release:OUTPUT_DIR=release
Debug:OUTPUT_DIR=debug

DESTDIR  = ../bin/$${OUTPUT_DIR}/
OBJECTS_DIR = ../build/$${OUTPUT_DIR}

!mac*:*g++*:QMAKE_CXXFLAGS += -fconcepts
*msvc*{
	Debug:QMAKE_CXXFLAGS += /JMC
}

linux*|mac*|freebsd{
	QMAKE_CXXFLAGS += -pedantic-errors
	QMAKE_CFLAGS += -pedantic-errors

	QMAKE_CXXFLAGS_WARN_ON *= -Wall -Wextra -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-qual -Wdouble-promotion -Wfloat-conversion -Wundef
	QMAKE_CXXFLAGS_WARN_ON *= -Wformat=2 -Wextra-semi -Wzero-as-null-pointer-constant -Wfloat-equal -Wredundant-decls -Wvla

	QMAKE_CXXFLAGS *= -Werror=return-type -Werror=uninitialized -Werror=delete-non-virtual-dtor -Werror=address
	QMAKE_CXXFLAGS *= -Werror=sizeof-pointer-div -Werror=sizeof-pointer-memaccess

	contains(QMAKE_COMPILER, clang) {
		QMAKE_CXXFLAGS_WARN_ON *= -Wshadow-all -Wcast-align -Wcomma -Wconditional-uninitialized -Wheader-hygiene -Wloop-analysis -Wextra-semi-stmt -Wunreachable-code-aggressive
		QMAKE_CXXFLAGS_WARN_ON *= -Wshorten-64-to-32 -Wmissing-prototypes -Wmissing-variable-declarations -Wno-weak-vtables
		QMAKE_CXXFLAGS_WARN_ON *= -Wimplicit-fallthrough -Wsuggest-override
		QMAKE_CXXFLAGS *= -Werror=return-stack-address -Werror=infinite-recursion
	} else {
		QMAKE_CXXFLAGS_WARN_ON *= -Wshadow -Wcast-align=strict -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wnull-dereference
		QMAKE_CXXFLAGS_WARN_ON *= -Wsuggest-override -Wnoexcept -Wmissing-declarations -Wmismatched-tags -Wunused-const-variable=1
		QMAKE_CXXFLAGS *= -Werror=return-local-addr -Werror=memset-transposed-args -Werror=nonnull-compare -Werror=mismatched-new-delete -Werror=infinite-recursion
		QMAKE_CXXFLAGS *= -Wcatch-value=3 -Werror=catch-value # -Werror=catch-value on its own would only enable level 1
	}

	Release:DEFINES += NDEBUG=1
	Debug:DEFINES += _DEBUG
}


HEADERS += \
	src/enum_helpers.hpp \
	src/file.hpp \
	src/file_interface.hpp \
	src/filesystem_error.hpp \
	src/filesystem_types.hpp \
	src/fs.hpp

SOURCES += src/filesystem_error.cpp

win*{
	HEADERS += $$files(src/*_win.hpp, true)
	SOURCES += $$files(src/*_win.cpp, true)
} else {
	HEADERS += $$files(src/*_linux.hpp, true)
	SOURCES += $$files(src/*_linux.cpp, true)
}

win*{
	DEFINES += WIN32_LEAN_AND_MEAN NOMINMAX
}
