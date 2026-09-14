# 3.20 for cxx_std_23
cmake_minimum_required(VERSION 3.20)

project(thin_io-tests LANGUAGES CXX)

set(TARGET_NAME tests)

# Listed in the order of SOURCES in thin_io_testapp/thin_io_testapp.pro, which must hold the same files.
add_executable(${TARGET_NAME}
	thin_io_testapp/test_directory_enumeration.cpp
	thin_io_testapp/test_entry_metadata.cpp
	thin_io_testapp/test_file.cpp
	thin_io_testapp/test_file_links.cpp
	thin_io_testapp/test_filesystem_error.cpp
	thin_io_testapp/test_filesystem_space.cpp
	thin_io_testapp/test_filesystem_types.cpp
	thin_io_testapp/test_fs.cpp
)

if (WIN32)
	target_sources(${TARGET_NAME} PRIVATE
		thin_io_testapp/test_file_win.cpp
		thin_io_testapp/test_windows_path.cpp
	)
endif()

# The library under test; it also brings the C++23 requirement
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. thin_io)
target_link_libraries(${TARGET_NAME} PRIVATE thin_io)

# CONFIG += strict_c++ in the .pro
set_target_properties(${TARGET_NAME} PROPERTIES CXX_EXTENSIONS OFF)

# "catch2/catch.hpp"
target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../3rdparty)

thin_io_add_gcc_clang_options(${TARGET_NAME})

if (MSVC)
	target_compile_options(${TARGET_NAME} PRIVATE /permissive- /Zc:__cplusplus /Zc:char8_t /MP /FS /W4 /wd4251)
	target_compile_definitions(${TARGET_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _SCL_SECURE_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	target_compile_options(${TARGET_NAME} PRIVATE -ggdb3)
endif()

# $<0:> suppresses the per-configuration subdirectory MSBuild otherwise appends
if (DEFINED OUTPUT_DIR)
	set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR}/$<0:>)
endif()

enable_testing()
add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
