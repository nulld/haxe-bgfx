#!/bin/bash
# Everything the README reports, in one pass, so the document can never drift
# from the code: corpus benchmark (with verification), ablation, enwik8 at two
# memory levels, reference-tool speeds, then README assembly.
#
#   ./bench/run_all.sh <silesia-dir> <enwik8-file> [jobs]
set -u
CORPUS=${1:?usage: run_all.sh <silesia-dir> <enwik8-file> [jobs]}
ENWIK=${2:?usage: run_all.sh <silesia-dir> <enwik8-file> [jobs]}
JOBS=${3:-4}
D=$(dirname "$0")
rm -rf "$D/results"; mkdir -p "$D/results"

echo "[1/5] corpus benchmark"      >&2; "$D/run_bench.sh"       "$CORPUS" ./prism -9 "$JOBS" > /dev/null
echo "[2/5] ablation"              >&2; "$D/ablation.sh"        "$CORPUS" ./prism -9 "$JOBS" > /dev/null
echo "[3/5] reference tool speeds" >&2; "$D/reference_speeds.sh" "$CORPUS" ./prism            > /dev/null

echo "[4/5] enwik8" >&2
e8() {  # level -> "size=.. ctime=.. dtime=.. verify=.. peak_rss_kib=.."
  L=$1; T=$(mktemp -d)
  s=$(date +%s.%N); "$D/peakrss.sh" ./prism c "$L" -q "$ENWIK" "$T/a" 2>"$T/m"; e=$(date +%s.%N)
  ct=$(awk -v a=$e -v b=$s 'BEGIN{print a-b}')
  s=$(date +%s.%N); ./prism d -q "$T/a" "$T/b" 2>/dev/null; e=$(date +%s.%N)
  dt=$(awk -v a=$e -v b=$s 'BEGIN{print a-b}')
  cmp -s "$ENWIK" "$T/b" && v=ok || v=MISMATCH
  printf 'size=%s ctime=%.0f dtime=%.0f verify=%s %s\n' "$(stat -c%s "$T/a")" "$ct" "$dt" "$v" "$(cat "$T/m")"
  rm -rf "$T"
}
e8 -9  >  "$D/results/enwik8.txt"
{ echo "gzip=$(gzip -9 -c "$ENWIK" | wc -c) bzip2=$(bzip2 -9 -c "$ENWIK" | wc -c) xz=$(xz -9e -c "$ENWIK" | wc -c)"
  e8 -11 | sed 's/^/l11_/;s/ /\nl11_/g' | tr '\n' ' '; echo
} >> "$D/results/enwik8.txt"

echo "[5/5] assembling README" >&2
python3 "$D/make_readme.py"
