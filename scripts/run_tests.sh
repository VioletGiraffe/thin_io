#!/bin/sh

# Builds the tests with CMake and runs them, the same commands as CI. Exit code: non-zero on a build or test failure.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# CMAKE_BUILD_TYPE for single-config generators, --config / -C for multi-config ones

cmake -S "$ROOT/tests" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" --config Release --parallel
ctest --test-dir "$ROOT/build" -C Release --output-on-failure --no-tests=error
