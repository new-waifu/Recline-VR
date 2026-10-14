#!/bin/sh
# CI check that the Linux binaries in DIR run on any system with glibc MAX or
# newer and need nothing else: libstdc++ and libgcc are linked statically, and
# Vulkan and the OpenVR runtime are loaded at run time.
#
#   tools/check_binaries.sh <dir> <max glibc version>
set -eu
dir=$1
max=$2
fail=0
for f in "$dir"/*; do
  [ -f "$f" ] || continue
  for lib in $(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
    case $lib in
      libc.so.6 | libm.so.6 | libdl.so.2 | librt.so.1 | libpthread.so.0 | ld-linux-*) ;;
      *) echo "$f: depends on $lib" >&2; fail=1 ;;
    esac
  done
  if objdump -T "$f" | grep -q 'GLIBCXX_\|CXXABI_\|GCC_[0-9]'; then
    echo "$f: imports from the shared libstdc++ or libgcc" >&2
    fail=1
  fi
  newest=$(objdump -T "$f" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n 1)
  if [ -n "$newest" ] && [ "$(printf '%s\n%s\n' "$newest" "$max" | sort -V | tail -n 1)" != "$max" ]; then
    echo "$f: needs glibc $newest, newer than $max" >&2
    fail=1
  fi
  echo "$f: glibc ${newest:-(none)}"
done
exit $fail
