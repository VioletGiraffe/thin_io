#!/bin/sh

# Builds the tests with CMake and runs them, the same commands as CI. Exit code: non-zero on a build or test failure.
# Arguments:
#   build | nobuild - optional, first: build without running, or run what is already built;
#   directories to run the tests in as well, such as one on a FAT32 or an exFAT volume. The tests create their
#   entries in the current directory.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

mode=all
case "${1:-}" in
	build | nobuild)
		mode=$1
		shift
		;;
esac

# CMAKE_BUILD_TYPE for single-config generators, --config / -C for multi-config ones

if [ "$mode" != nobuild ]; then
	cmake -S "$ROOT/tests" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release "-DOUTPUT_DIR=$ROOT/build/bin"
	cmake --build "$ROOT/build" --config Release --parallel
fi
if [ "$mode" = build ]; then
	exit 0
fi

ctest --test-dir "$ROOT/build" -C Release --output-on-failure --no-tests=error

failed=0
for directory in "$@"; do
	printf '\n===== tests in %s\n' "$directory"
	(cd "$directory" && "$ROOT/build/bin/tests") || failed=1
done
exit $failed
