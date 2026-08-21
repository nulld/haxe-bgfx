#!/bin/bash
# Full-corpus benchmark: ratio and speed for PRISM against the standard
# general-purpose tools, with every PRISM stream verified by decompressing it
# and comparing against the original.
#
#   ./bench/run_bench.sh <corpus-dir> [prism-binary] [level] [jobs]
set -u
CORPUS=${1:?usage: run_bench.sh <corpus-dir> [prism] [level] [jobs]}
PRISM=${2:-./prism}
LEVEL=${3:--9}
JOBS=${4:-4}
OUT=${OUT:-bench/results}
mkdir -p "$OUT" "$OUT/tmp"

run_one() {
  f=$1; base=$(basename "$f"); orig=$(stat -c%s "$f")
  t0=$(date +%s.%N)
  $PRISM c $LEVEL -q "$f" "$OUT/tmp/$base.prz" 2>/dev/null
  t1=$(date +%s.%N)
  $PRISM d -q "$OUT/tmp/$base.prz" "$OUT/tmp/$base.out" 2>/dev/null
  t2=$(date +%s.%N)
  if cmp -s "$f" "$OUT/tmp/$base.out"; then verify=ok; else verify=MISMATCH; fi
  psz=$(stat -c%s "$OUT/tmp/$base.prz")
  rm -f "$OUT/tmp/$base.out"
  gz=$(gzip -9 -c "$f" | wc -c)
  bz=$(bzip2 -9 -c "$f" | wc -c)
  t3=$(date +%s.%N); xz=$(xz -9e -c "$f" | wc -c); t4=$(date +%s.%N)
  printf '%s,%s,%s,%s,%s,%s,%.2f,%.2f,%.2f,%s\n' \
    "$base" "$orig" "$psz" "$gz" "$bz" "$xz" \
    "$(echo "$t1 $t0" | awk '{print $1-$2}')" \
    "$(echo "$t2 $t1" | awk '{print $1-$2}')" \
    "$(echo "$t4 $t3" | awk '{print $1-$2}')" "$verify" >> "$OUT/raw.csv"
}

echo "file,orig,prism,gzip9,bzip2_9,xz9e,prism_ctime,prism_dtime,xz_ctime,verify" > "$OUT/raw.csv"
for f in "$CORPUS"/*; do
  [ -f "$f" ] || continue
  run_one "$f" &
  while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do wait -n; done
done
wait
rmdir "$OUT/tmp" 2>/dev/null

python3 - "$OUT/raw.csv" "$OUT/table.md" "$LEVEL" <<'PY'
import csv, sys
rows = sorted(csv.DictReader(open(sys.argv[1])), key=lambda r: r['file'])
tot = {k: 0 for k in ('orig','prism','gzip9','bzip2_9','xz9e')}
tc = td = txc = 0.0
out = ["| file | size | gzip -9 | bzip2 -9 | xz -9e | **PRISM %s** | bpb | vs xz | verify |" % sys.argv[3],
       "|---|---:|---:|---:|---:|---:|---:|---:|:--:|"]
for r in rows:
    o = int(r['orig']); p = int(r['prism'])
    for k in tot: tot[k] += int(r[k])
    tc += float(r['prism_ctime']); td += float(r['prism_dtime']); txc += float(r['xz_ctime'])
    out.append("| %s | %s | %s | %s | %s | **%s** | %.3f | %+.1f%% | %s |" % (
        r['file'], f"{o:,}", f"{int(r['gzip9']):,}", f"{int(r['bzip2_9']):,}",
        f"{int(r['xz9e']):,}", f"{p:,}", p*8/o, 100.0*(p-int(r['xz9e']))/int(r['xz9e']), r['verify']))
o, p = tot['orig'], tot['prism']
out.append("| **total** | **%s** | **%s** | **%s** | **%s** | **%s** | **%.3f** | **%+.1f%%** | |" % (
    f"{o:,}", f"{tot['gzip9']:,}", f"{tot['bzip2_9']:,}", f"{tot['xz9e']:,}", f"{p:,}",
    p*8/o, 100.0*(p-tot['xz9e'])/tot['xz9e']))
out.append("")
out.append("Ratios: gzip -9 %.3fx, bzip2 -9 %.3fx, xz -9e %.3fx, PRISM %.3fx." % (
    o/tot['gzip9'], o/tot['bzip2_9'], o/tot['xz9e'], o/p))
out.append("PRISM: %.0f s compress, %.0f s decompress (%.2f / %.2f MiB/s single-threaded). "
           "xz -9e: %.0f s compress (%.2f MiB/s)." % (
    tc, td, o/1048576.0/tc, o/1048576.0/td, txc, o/1048576.0/txc))
open(sys.argv[2],'w').write("\n".join(out) + "\n")
print("\n".join(out))
PY
