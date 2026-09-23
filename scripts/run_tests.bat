@echo off
:: Builds the tests with CMake and runs them, the same commands as CI. Exit code: 1 on a build or test failure, 2 when
:: CMake is not found.
:: Visual Studio's bundled CMake is used when none is on PATH.
setlocal
set "root=%~dp0.."
set "configuration=Release"

where cmake >nul 2>nul || call :add_visual_studio_cmake_to_path || exit /b 2

cmake -S "%root%\tests" -B "%root%\build" || exit /b 1
cmake --build "%root%\build" --config %configuration% --parallel || exit /b 1
ctest --test-dir "%root%\build" -C %configuration% --output-on-failure --no-tests=error || exit /b 1
exit /b 0

:add_visual_studio_cmake_to_path
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" (
	echo No cmake on PATH, and vswhere.exe was not found to locate Visual Studio's. 1>&2
	exit /b 1
)
for /f "usebackq delims=" %%d in (`"%vswhere%" -latest -products * -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`) do set "cmakeDirectory=%%d"
if not defined cmakeDirectory (
	echo No cmake on PATH, and no Visual Studio with the C++ CMake tools. 1>&2
	exit /b 1
)
set "PATH=%cmakeDirectory%;%PATH%"
exit /b 0
