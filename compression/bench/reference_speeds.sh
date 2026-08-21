#!/bin/bash
# Measure the reference tools on the same machine and the same corpus, so the
# cost table compares like with like instead of quoting numbers from elsewhere.
set -u
CORPUS=${1:?usage: reference_speeds.sh <corpus-dir> [prism]}
PRISM=${2:-./prism}
OUT=${OUT:-bench/results}
mkdir -p "$OUT"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cat "$CORPUS"/* > "$TMP/all"
N=$(stat -c%s "$TMP/all")

t() { s=$(date +%s.%N); "$@" > /dev/null 2>&1; e=$(date +%s.%N); awk -v a="$e" -v b="$s" 'BEGIN{print a-b}'; }

gzip -9 -c "$TMP/all" > "$TMP/all.gz"
xz -9e -c "$TMP/all" > "$TMP/all.xz" 2>/dev/null
gc=$(t gzip -9 -c "$TMP/all");   gd=$(t gzip -dc "$TMP/all.gz")
xd=$(t xz -dc "$TMP/all.xz")
# peak RSS of each compressor on one mid-sized file
D=$(dirname "$0")
mem() { "$D/peakrss.sh" "$@" >/dev/null 2>"$TMP/m"; sed -n 's/^peak_rss_kib=//p' "$TMP/m"; }
mg=$(mem gzip -9 -c "$CORPUS/dickens")
mx=$(mem xz -9e -c "$CORPUS/dickens")
mp=$(mem $PRISM c -9 -q "$CORPUS/xml" "$TMP/p.prz")

{ echo "bytes=$N"
  echo "gzip_c_mibs=$(awk -v n=$N -v t=$gc 'BEGIN{printf "%.1f", n/1048576/t}')"
  echo "gzip_d_mibs=$(awk -v n=$N -v t=$gd 'BEGIN{printf "%.0f", n/1048576/t}')"
  echo "xz_d_mibs=$(awk  -v n=$N -v t=$xd 'BEGIN{printf "%.0f", n/1048576/t}')"
  echo "gzip_mem_mb=$((mg/1024))"; echo "xz_mem_mb=$((mx/1024))"; echo "prism_mem_mb=$((mp/1024))"
} > "$OUT/reference_speeds.txt"
cat "$OUT/reference_speeds.txt"
