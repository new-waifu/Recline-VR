#!/bin/sh
# CI: moves the debug info of the Linux binaries in DIR to DEBUG/<name>.debug,
# leaving a debuglink to it, so the shipped binaries stay small.
#
#   tools/split_debug.sh <dir> <debug dir>
set -eu
dir=$1
debug=$2
mkdir -p "$debug"
for f in "$dir"/*; do
  [ -f "$f" ] || continue
  name=$(basename "$f")
  objcopy --only-keep-debug "$f" "$debug/$name.debug"
  objcopy --strip-debug --add-gnu-debuglink="$debug/$name.debug" "$f"
done
