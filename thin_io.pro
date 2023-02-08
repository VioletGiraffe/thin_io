TEMPLATE = lib
CONFIG += staticlib

CONFIG -= qt
CONFIG -= flat

CONFIG += strict_c++ c++2a

*g++*:QMAKE_CXXFLAGS += -fconcepts -std=c++2a
*msvc*:QMAKE_CXXFLAGS += /Zc:char8_t /JMC

HEADERS += $$files(src/*.hpp, true)
SOURCES += $$files(src/*.cpp, true)

INCLUDEPATH += C:/DEV/ChromiumCrawler/cpp-db/3rdparty/x64-windows-static-md/include
