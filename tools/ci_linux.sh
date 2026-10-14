#!/bin/sh
# CI build of one Linux platform, inside its Steam Runtime SDK container (see
# .github/workflows/build.yml): build, test, check the binaries' dependencies,
# split off the debug info, and archive the stage and the debug info in dist/
# for tools/merge_packages.sh. Runs the same way locally, from the source root:
#
#   docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD:$PWD" -w "$PWD" \
#     <image> tools/ci_linux.sh <platform> <max glibc>
#
# (A git worktree also needs its main repository mounted, at the same path.)
set -eu
platform=$1
max_glibc=$2
build=${BUILD:-build-ci-$platform}

cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRECLINE_REQUIRE_VULKAN=ON
cmake --build "$build"
ctest --test-dir "$build" --output-on-failure
tools/check_binaries.sh "$build/stage/bin/$platform" "$max_glibc"

# Split a copy: stripping the stage itself would leave nothing to split on the
# next incremental build.
rm -rf "$build/package" "$build/debug"
cp -R "$build/stage" "$build/package"
tools/split_debug.sh "$build/package/bin/$platform" "$build/debug/bin/$platform"
mkdir -p dist
tar -cf "dist/stage-$platform.tar" -C "$build/package" .
tar -cf "dist/debug-$platform.tar" -C "$build/debug" .
