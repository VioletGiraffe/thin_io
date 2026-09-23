CONFIG += strict_c++ c++latest

CONFIG -= qt

TEMPLATE = app
CONFIG += console

mac* | linux* | freebsd {
	CONFIG(release, debug|release):CONFIG *= Release optimize_full
	CONFIG(debug, debug|release):CONFIG *= Debug
}

Release:OUTPUT_DIR=release
Debug:OUTPUT_DIR=debug

DESTDIR  = ../bin/$${OUTPUT_DIR}/
OBJECTS_DIR = ../build/$${OUTPUT_DIR}
MOC_DIR     = ../build/$${OUTPUT_DIR}
UI_DIR      = ../build/$${OUTPUT_DIR}
RCC_DIR     = ../build/$${OUTPUT_DIR}

win*{
	QMAKE_CXXFLAGS += /std:c++latest /permissive- /Zc:__cplusplus /Zc:char8_t /utf-8

	QMAKE_CXXFLAGS += /MP /FS
	QMAKE_CXXFLAGS_WARN_ON = /W4 /wd4251
	QMAKE_CXXFLAGS_WARN_ON += /we4715 /we4716 # not all control paths return a value / must return a value
	QMAKE_CXXFLAGS_WARN_ON += /we4172         # returning address of local variable or temporary
	QMAKE_CXXFLAGS_WARN_ON += /we4700         # uninitialized local variable used
	QMAKE_CXXFLAGS_WARN_ON += /we4477         # printf format string does not match the argument
	QMAKE_CXXFLAGS_WARN_ON += /we4551         # function call missing argument list
	QMAKE_CXXFLAGS_WARN_ON += /we4552 /we4553 # operator has no effect; did you intend '='?
	DEFINES += WIN32_LEAN_AND_MEAN NOMINMAX _SCL_SECURE_NO_WARNINGS _CRT_SECURE_NO_WARNINGS

	QMAKE_CXXFLAGS_DEBUG -= -Zi
	QMAKE_CXXFLAGS_DEBUG *= /ZI
	QMAKE_LFLAGS += /DEBUG
	Debug:QMAKE_LFLAGS += /INCREMENTAL

	Release:QMAKE_CXXFLAGS += /Zi

	Release:QMAKE_LFLAGS += /OPT:REF /OPT:ICF /TIME
}

linux*|mac*|freebsd{
	QMAKE_CXXFLAGS_WARN_ON += -pedantic-errors
	QMAKE_CFLAGS_WARN_ON += -pedantic-errors

	QMAKE_CXXFLAGS_WARN_ON *= -Wall -Wextra -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-qual -Wdouble-promotion -Wfloat-conversion -Wundef
	QMAKE_CXXFLAGS_WARN_ON *= -Wformat=2 -Wextra-semi -Wzero-as-null-pointer-constant -Wfloat-equal -Wredundant-decls -Wvla
	QMAKE_CXXFLAGS_WARN_ON *= -Wno-missing-field-initializers

	QMAKE_CXXFLAGS_WARN_ON *= -Werror=return-type -Werror=uninitialized -Werror=delete-non-virtual-dtor -Werror=address
	QMAKE_CXXFLAGS_WARN_ON *= -Werror=sizeof-pointer-div -Werror=sizeof-pointer-memaccess

	contains(QMAKE_COMPILER, clang) {
		QMAKE_CXXFLAGS_WARN_ON *= -Wshadow-all -Wcast-align -Wcomma -Wconditional-uninitialized -Wheader-hygiene -Wloop-analysis -Wextra-semi-stmt -Wunreachable-code-aggressive
		QMAKE_CXXFLAGS_WARN_ON *= -Wno-shadow-uncaptured-local # flags every [x = std::move(x)] init-capture
		QMAKE_CXXFLAGS_WARN_ON *= -Wshorten-64-to-32 -Wmissing-prototypes -Wmissing-variable-declarations
		QMAKE_CXXFLAGS_WARN_ON *= -Wimplicit-fallthrough -Wsuggest-override
		QMAKE_CXXFLAGS_WARN_ON *= -Werror=return-stack-address -Werror=infinite-recursion
	} else {
		QMAKE_CXXFLAGS_WARN_ON *= -Wshadow -Wcast-align=strict -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wnull-dereference
		QMAKE_CXXFLAGS_WARN_ON *= -Wsuggest-override -Wmissing-declarations -Wmismatched-tags -Wunused-const-variable=1
		QMAKE_CXXFLAGS_WARN_ON *= -Werror=return-local-addr -Werror=memset-transposed-args -Werror=nonnull-compare -Werror=mismatched-new-delete -Werror=infinite-recursion
		QMAKE_CXXFLAGS_WARN_ON *= -Wcatch-value=3 -Werror=catch-value # -Werror=catch-value on its own would only enable level 1
		QMAKE_CXXFLAGS_WARN_ON *= -Wno-maybe-uninitialized # False positives on std::optional and std::expected
	}

	Release:DEFINES += NDEBUG=1
	Debug:DEFINES += _DEBUG
}

*g++*{
	QMAKE_CXXFLAGS += -fconcepts -ggdb3

	#QMAKE_CXXFLAGS += -fsanitize=thread
	#QMAKE_LFLAGS += -fsanitize=thread
}


Debug:LIB_PATH += $${PWD}/../../../bin/debug
Release:LIB_PATH += $${PWD}/../../../bin/release

LIBS += -L$${LIB_PATH} -lthin_io

mac*|linux*|freebsd*{
	PRE_TARGETDEPS += $${LIB_PATH}/libthin_io.a
}

INCLUDEPATH += \
	$${PWD}/../../../cpp-template-utils \
	$${PWD}/../../src

SOURCES += \
	test_directory_enumeration.cpp \
	test_entry_metadata.cpp \
	test_file.cpp \
	test_file_links.cpp \
	test_filesystem_error.cpp \
	test_filesystem_space.cpp \
	test_filesystem_types.cpp \
	test_fs.cpp

win*:SOURCES += \
	test_file_win.cpp \
	test_windows_path.cpp

HEADERS += catch_thin_io.hpp
