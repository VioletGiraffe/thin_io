TEMPLATE = lib
CONFIG += staticlib

CONFIG -= qt
CONFIG -= flat

CONFIG += strict_c++ c++2a

mac* | linux* | freebsd {
	CONFIG(release, debug|release):CONFIG *= Release optimize_full
	CONFIG(debug, debug|release):CONFIG *= Debug
}

*g++*:QMAKE_CXXFLAGS += -fconcepts -std=c++2a
*msvc*{
	Debug:QMAKE_CXXFLAGS += /JMC
}

HEADERS += $$files(src/*.hpp, true)
SOURCES += $$files(src/*.cpp, true)
