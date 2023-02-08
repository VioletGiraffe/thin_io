#pragma once

#ifdef _WIN32
#include "file_win.hpp"
#else
#include "file_linux.hpp"
#endif

namespace thin_io {

	using file = file_interface<file_impl>;

}
