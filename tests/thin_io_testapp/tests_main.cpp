#define CATCH_CONFIG_ENABLE_BENCHMARKING
#define CATCH_CONFIG_RUNNER
#include "catch2/catch.hpp"

#if defined(CATCH_CONFIG_WCHAR) && defined(CATCH_PLATFORM_WINDOWS) && defined(_UNICODE)
// Standard C/C++ Win32 Unicode wmain entry point
extern "C" int wmain(int argc, wchar_t* argv[], wchar_t* []) {
#else
// Standard C/C++ main entry point
int main(int argc, char* argv[]) {
#endif

	return Catch::Session().run(argc, argv);
}
