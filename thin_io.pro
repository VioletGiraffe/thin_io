TEMPLATE = lib
CONFIG += staticlib

CONFIG -= qt
!win*:CONFIG -= flat

CONFIG += strict_c++ c++latest

mac* | linux* | freebsd {
	CONFIG(release, debug|release):CONFIG *= Release optimize_full
	CONFIG(debug, debug|release):CONFIG *= Debug
}

Release:OUTPUT_DIR=release
Debug:OUTPUT_DIR=debug

DESTDIR  = ../bin/$${OUTPUT_DIR}/
OBJECTS_DIR = ../build/$${OUTPUT_DIR}

*g++*:QMAKE_CXXFLAGS += -fconcepts -std=c++2a
*msvc*{
	Debug:QMAKE_CXXFLAGS += /JMC
}

mac*|linux*{
	QMAKE_CXXFLAGS_WARN_ON = -Wall -Wextra -Wdelete-non-virtual-dtor -Werror=duplicated-cond -Werror=duplicated-branches -Warith-conversion -Warray-bounds -Wattributes -Wcast-align -Wcast-qual -Wconversion -Wdate-time -Wduplicated-branches -Wendif-labels -Werror=overflow -Werror=return-type -Werror=shift-count-overflow -Werror=sign-promo -Werror=undef -Wextra -Winit-self -Wlogical-op -Wmissing-include-dirs -Wnull-dereference -Wpedantic -Wpointer-arith -Wredundant-decls -Wshadow -Wstrict-aliasing -Wstrict-aliasing=3 -Wuninitialized -Wunused-const-variable=2 -Wwrite-strings -Wlogical-op
	QMAKE_CXXFLAGS_WARN_ON += -Wno-missing-include-dirs -Wno-undef
}

HEADERS += \
	src/file.hpp \
	src/file_interface.hpp

win*{
	HEADERS += $$files(src/*_win.hpp, true)
	SOURCES += $$files(src/*_win.cpp, true)
} else {
	HEADERS += $$files(src/*_linux.hpp, true)
	SOURCES += $$files(src/*_linux.cpp, true)
}
