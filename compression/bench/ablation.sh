#!/bin/bash
# Ablation: recompress the whole corpus with each model component disabled,
# so every claimed contribution is a measured number rather than an assertion.
#
#   ./bench/ablation.sh <corpus-dir> [prism] [level] [jobs]
set -u
CORPUS=${1:?usage: ablation.sh <corpus-dir> [prism] [level] [jobs]}
PRISM=${2:-./prism}
LEVEL=${3:--9}
JOBS=${4:-4}
OUT=${OUT:-bench/results}
mkdir -p "$OUT/abl"
CONFIGS=("no-stride:--no-stride" "no-regime:--no-regime" "no-line:--no-line" "no-match:--no-match")

# Reuse the full-model sizes that run_bench.sh already measured (and verified
# by decompressing) rather than spending another corpus pass recomputing them.
echo "config,file,size" > "$OUT/ablation.csv"
if [ -f "$OUT/raw.csv" ]; then
  tail -n +2 "$OUT/raw.csv" | while IFS=, read -r f orig prism rest; do
    echo "full,$f,$prism" >> "$OUT/ablation.csv"
  done
else
  CONFIGS=("full:" "${CONFIGS[@]}")
fi
for cfg in "${CONFIGS[@]}"; do
  name=${cfg%%:*}; flag=${cfg#*:}
  for f in "$CORPUS"/*; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    ( $PRISM c $LEVEL $flag -q "$f" "$OUT/abl/$name.$b.prz" 2>/dev/null
      echo "$name,$b,$(stat -c%s "$OUT/abl/$name.$b.prz")" >> "$OUT/ablation.csv"
      rm -f "$OUT/abl/$name.$b.prz" ) &
    while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do wait -n; done
  done
  wait
  echo "done: $name" >&2
done
rmdir "$OUT/abl" 2>/dev/null

python3 - "$OUT/ablation.csv" "$OUT/ablation.md" <<'PY'
import csv, sys, collections
tot = collections.defaultdict(int)
per = collections.defaultdict(dict)
for r in csv.DictReader(open(sys.argv[1])):
    tot[r['config']] += int(r['size']); per[r['config']][r['file']] = int(r['size'])
base = tot['full']
names = {'no-stride':'stride detector + column models','no-regime':'regime-gated mixing/SSE',
         'no-line':'newline column models','no-match':'match model'}
out = ["| disabled component | corpus total | cost of removing it | worst-hit file |","|---|---:|---:|---|"]
out.append("| *(nothing -- full model)* | %s | -- | |" % f"{base:,}")
for k in ('no-stride','no-regime','no-line','no-match'):
    if k not in tot: continue
    worst, wd = '', 0.0
    for f, v in per[k].items():
        d = 100.0*(v - per['full'][f])/per['full'][f]
        if d > wd: wd, worst = d, f
    out.append("| %s | %s | **%+.2f%%** | %s %+.1f%% |" % (
        names[k], f"{tot[k]:,}", 100.0*(tot[k]-base)/base, worst, wd))
open(sys.argv[2],'w').write("\n".join(out) + "\n")
print("\n".join(out))
PY
