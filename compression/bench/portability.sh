#!/bin/sh
# The model is integer-exact, so a stream must not depend on the compiler,
# the optimisation level or the instruction set it was produced with.
# Anything else would mean archives are only readable by the binary that
# wrote them.
set -e
SRC=$(dirname "$0")/../src/prism.c
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
IN=${1:-$SRC}

built=""
for spec in "cc -O0" "cc -O2" "cc -O3 -funroll-loops" "cc -O3 -march=native" "clang -O2"; do
  bin=$TMP/$(echo "$spec" | tr -c 'a-zA-Z0-9' '_')
  if $spec -w -o "$bin" "$SRC" 2>/dev/null; then
    "$bin" c -4 -q "$IN" "$bin.prz"
    printf '  %-28s %s\n' "$spec" "$(md5sum < "$bin.prz" | cut -d' ' -f1)"
    built="$built $bin"
  else
    printf '  %-28s (unavailable, skipped)\n' "$spec"
  fi
done

first=""; distinct=0
for b in $built; do
  h=$(md5sum < "$b.prz" | cut -d' ' -f1)
  [ -z "$first" ] && first=$h
  [ "$h" = "$first" ] || distinct=1
done
[ "$distinct" -eq 0 ] || { echo "FAIL: builds disagree on the compressed stream"; exit 1; }

# and every build must decode every other build's stream
for b in $built; do for c in $built; do
  "$b" d -q "$c.prz" "$TMP/out"; cmp -s "$IN" "$TMP/out" || { echo "FAIL: cross-build decode"; exit 1; }
done; done
echo "portability: all builds agree bit-for-bit and decode each other's streams"
