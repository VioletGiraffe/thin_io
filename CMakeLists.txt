# 3.25 for MSVC_DEBUG_INFORMATION_FORMAT
cmake_minimum_required(VERSION 3.25)

project(thin_io LANGUAGES CXX)

set(TARGET_NAME thin_io)

# The linux|mac|freebsd block of thin_io.pro and tests/thin_io_testapp/thin_io_testapp.pro, plus their -fconcepts
function(thin_io_add_gcc_clang_options TARGET)
	if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
		return()
	endif()

	target_compile_options(${TARGET} PRIVATE
		-pedantic-errors
		-Wall -Wextra -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-qual -Wdouble-promotion -Wfloat-conversion -Wundef
		-Wformat=2 -Wextra-semi -Wzero-as-null-pointer-constant -Wfloat-equal -Wredundant-decls -Wvla
		-Wno-missing-field-initializers
		-Werror=return-type -Werror=uninitialized -Werror=delete-non-virtual-dtor -Werror=address
		-Werror=sizeof-pointer-div -Werror=sizeof-pointer-memaccess
	)

	if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		target_compile_options(${TARGET} PRIVATE
			-Wshadow-all -Wcast-align -Wcomma -Wconditional-uninitialized -Wheader-hygiene -Wloop-analysis -Wextra-semi-stmt -Wunreachable-code-aggressive
			-Wno-shadow-uncaptured-local # flags every [x = std::move(x)] init-capture
			-Wshorten-64-to-32 -Wmissing-prototypes -Wmissing-variable-declarations
			-Wimplicit-fallthrough -Wsuggest-override
			-Werror=return-stack-address -Werror=infinite-recursion
		)
	else()
		target_compile_options(${TARGET} PRIVATE
			-fconcepts
			-Wshadow -Wcast-align=strict -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wnull-dereference
			-Wsuggest-override -Wmissing-declarations -Wmismatched-tags -Wunused-const-variable=1
			-Werror=return-local-addr -Werror=memset-transposed-args -Werror=nonnull-compare -Werror=mismatched-new-delete -Werror=infinite-recursion
			-Wcatch-value=3 -Werror=catch-value # -Werror=catch-value on its own would only enable level 1
			-Wno-maybe-uninitialized # False positives on std::optional and std::expected
		)
	endif()

	target_compile_definitions(${TARGET} PRIVATE
		"$<$<CONFIG:Release>:NDEBUG=1>"
		"$<$<CONFIG:Debug>:_DEBUG>"
	)
endfunction()

# The MSVC options common to both targets, including the /we list both .pro files hold
function(thin_io_add_msvc_options TARGET)
	if (NOT MSVC)
		return()
	endif()

	target_compile_options(${TARGET} PRIVATE
		/permissive- /Zc:__cplusplus /utf-8 /MP /W4
		/we4715 /we4716 # not all control paths return a value / must return a value
		/we4172         # returning address of local variable or temporary
		/we4700         # uninitialized local variable used
		/we4477         # printf format string does not match the argument
		/we4551         # function call missing argument list
		/we4552 /we4553 # operator has no effect; did you intend '='?
	)
	target_compile_definitions(${TARGET} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endfunction()

# Must hold the same files as thin_io.pro
if (WIN32)
	file(GLOB PLATFORM_FILES CONFIGURE_DEPENDS src/*_win.cpp src/*_win.hpp)
else()
	file(GLOB PLATFORM_FILES CONFIGURE_DEPENDS src/*_linux.cpp src/*_linux.hpp)
endif()

add_library(${TARGET_NAME} STATIC
	src/enum_helpers.hpp
	src/file.hpp
	src/file_interface.hpp
	src/filesystem_error.hpp
	src/filesystem_types.hpp
	src/fs.hpp
	src/filesystem_error.cpp
	${PLATFORM_FILES}
)

target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)

# PUBLIC: the headers require C++23 (std::expected, std::to_underlying)
target_compile_features(${TARGET_NAME} PUBLIC cxx_std_23)
# CONFIG += strict_c++ in the .pro
set_target_properties(${TARGET_NAME} PROPERTIES CXX_EXTENSIONS OFF)

thin_io_add_gcc_clang_options(${TARGET_NAME})
thin_io_add_msvc_options(${TARGET_NAME})

if (MSVC)
	target_compile_options(${TARGET_NAME} PRIVATE $<$<CONFIG:Debug>:/JMC>)
	set_target_properties(${TARGET_NAME} PROPERTIES MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase) # /Zi
endif()

# $<0:> suppresses the per-configuration subdirectory MSBuild otherwise appends
if (DEFINED OUTPUT_DIR)
	set_target_properties(${TARGET_NAME} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${OUTPUT_DIR}/$<0:>)
endif()
