#!/bin/sh
# CI: merges the per-platform archives in DIST (stage-<platform>.tar and
# debug-<platform>.tar, from tools/ci_linux.sh and the Windows job) into one
# package for every platform, recline-vr-<version>.zip, and one of debug info,
# recline-vr-<version>-debug.zip. Both unpack to a recline-vr/ directory.
#
#   tools/merge_packages.sh <dist dir>
set -eu
dist=$(cd "$1" && pwd)
work=$dist/work
rm -rf "$work"
mkdir -p "$work/stages" "$work/package/recline-vr" "$work/debug/recline-vr"

for t in "$dist"/stage-*.tar; do
  platform=$(basename "$t" .tar)
  platform=${platform#stage-}
  mkdir "$work/stages/$platform"
  tar -xf "$t" -C "$work/stages/$platform"
done

# All builds come from the same commit, so everything outside bin/ must match
# (this also catches line-ending conversion on the Windows checkout).
first=
for d in "$work"/stages/*; do
  if [ -z "$first" ]; then
    first=$d
  elif ! diff -r -x bin "$first" "$d"; then
    echo "$(basename "$d") and $(basename "$first") differ outside bin/" >&2
    exit 1
  fi
  cp -R "$d"/. "$work/package/recline-vr/"
done
for t in "$dist"/debug-*.tar; do tar -xf "$t" -C "$work/debug/recline-vr"; done

name=recline-vr-$(cat "$first/VERSION")
rm -f "$dist/$name.zip" "$dist/$name-debug.zip"
# zip keeps the execute bits, which GitHub's artifact archives drop.
(cd "$work/package" && zip -qr "$dist/$name.zip" recline-vr)
(cd "$work/debug" && zip -qr "$dist/$name-debug.zip" recline-vr)
rm -rf "$work"
echo "$dist/$name.zip"
echo "$dist/$name-debug.zip"
