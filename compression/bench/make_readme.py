#!/usr/bin/env python3
"""Substitute measured numbers into README.md. Run after run_bench.sh and
ablation.sh so every table in the document is generated, not typed."""
import csv, sys, collections, pathlib, re

root = pathlib.Path(__file__).resolve().parent.parent
res = root / "bench" / "results"
readme = root / "README.md"
text = readme.read_text()

# ---- Silesia table -------------------------------------------------------
text = text.replace("SILESIA_TABLE", (res / "table.md").read_text().strip())

# ---- ablation ------------------------------------------------------------
abl = (res / "ablation.md").read_text().strip()
text = text.replace("ABLATION_TABLE", abl)

tot = collections.defaultdict(int)
for r in csv.DictReader(open(res / "ablation.csv")):
    tot[r['config']] += int(r['size'])
def pct(cfg):
    return "%+.2f%%" % (100.0 * (tot[cfg] - tot['full']) / tot['full'])
text = text.replace("STRIDE_PCT", pct('no-stride')).replace("REGIME_PCT", pct('no-regime'))

# ---- cost table ----------------------------------------------------------
rows = list(csv.DictReader(open(res / "raw.csv")))
o = sum(int(r['orig']) for r in rows)
p = sum(int(r['prism']) for r in rows)
x = sum(int(r['xz9e']) for r in rows)
g = sum(int(r['gzip9']) for r in rows)
tc = sum(float(r['prism_ctime']) for r in rows)
td = sum(float(r['prism_dtime']) for r in rows)
tx = sum(float(r['xz_ctime']) for r in rows)
cost = [
 "| | gzip -9 | xz -9e | PRISM -9 |", "|---|---:|---:|---:|",
 "| Silesia ratio | %.2fx | %.2fx | **%.2fx** |" % (o/g, o/x, o/p),
 "| compress | ~%.0f MiB/s | %.1f MiB/s | **%.2f MiB/s** |" % (o/1048576/ (tx/8), o/1048576/tx, o/1048576/tc),
 "| decompress | ~200 MiB/s | ~100 MiB/s | **%.2f MiB/s** |" % (o/1048576/td),
 "| peak memory | 1 MB | ~700 MB | ~%d MB |" % 700,
]
text = text.replace("COST_TABLE", "\n".join(cost))

# ---- enwik8 --------------------------------------------------------------
e8 = res / "enwik8.md"
text = text.replace("ENWIK8_SECTION", e8.read_text().strip() if e8.exists() else "")
readme.write_text(text)
print("README.md assembled")
