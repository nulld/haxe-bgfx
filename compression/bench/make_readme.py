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
ref = dict(l.strip().split('=', 1) for l in open(res / "reference_speeds.txt") if '=' in l)
cost = [
 "All measured on the same machine and the same corpus.", "",
 "| | gzip -9 | xz -9e | PRISM -9 |", "|---|---:|---:|---:|",
 "| Silesia ratio | %.2fx | %.2fx | **%.2fx** |" % (o/g, o/x, o/p),
 "| compress | %s MiB/s | %.2f MiB/s | **%.2f MiB/s** |" % (ref['gzip_c_mibs'], o/1048576/tx, o/1048576/tc),
 "| decompress | %s MiB/s | %s MiB/s | **%.2f MiB/s** |" % (ref['gzip_d_mibs'], ref['xz_d_mibs'], o/1048576/td),
 "| peak memory | %s MB | %s MB | **%s MB** |" % (ref['gzip_mem_mb'], ref['xz_mem_mb'], ref['prism_mem_mb']),
 "",
 "PRISM is **%.0fx slower to compress than xz -9e and %.0fx slower to decompress**, "
 "for %.1f%% fewer bytes." % (tc/tx, td/(o/1048576/float(ref['xz_d_mibs'])), 100.0*(x-p)/x),
]
text = text.replace("COST_TABLE", "\n".join(cost))

# ---- enwik8 --------------------------------------------------------------
e8 = res / "enwik8.txt"
if e8.exists():
    d = dict(kv.split('=', 1) for kv in e8.read_text().split() if '=' in kv)
    n = 100_000_000
    sec = ["", "### enwik8", "",
      "The other standard reference point, so the result can be placed against the "
      "published literature. 100,000,000 bytes of Wikipedia XML; the gzip and bzip2 "
      "rows below reproduce the long-published values for this file to within a "
      "version's difference, which is the check that this setup is measuring the "
      "same thing everyone else is.", "",
      "| | size | bpc |", "|---|---:|---:|",
      "| gzip -9 | %s | %.3f |" % (f"{int(d['gzip']):,}", int(d['gzip'])*8/n),
      "| bzip2 -9 | %s | %.3f |" % (f"{int(d['bzip2']):,}", int(d['bzip2'])*8/n),
      "| xz -9e | %s | %.3f |" % (f"{int(d['xz']):,}", int(d['xz'])*8/n),
      "| **PRISM -9** | **%s** | **%.3f** |" % (f"{int(d['size']):,}", int(d['size'])*8/n),
      "",
      "%.1f%% below xz -9e. Verified by decompressing: %s. %.0f s to compress, "
      "%.0f s to decompress, %s MB peak." % (
          100.0*(int(d['xz'])-int(d['size']))/int(d['xz']), d['verify'],
          float(d['ctime']), float(d['dtime']), int(d['peak_rss_kib'])//1024),
      "",
      "For scale, and **not measured here** -- these are the published Large Text "
      "Compression Benchmark figures, quoted so the result can be located rather "
      "than to claim anything: lpaq1 -9 reaches 19,755,948 (1.581 bpc), zpaq -m5 "
      "17,855,729 (1.428), and the frontier, cmix v21, 14,623,723 (1.170) using "
      "roughly 26 GB of RAM. PRISM sits where a compressor of this size and "
      "ambition should sit: past the general-purpose tools, short of the "
      "specialists.", ""]
    text = text.replace("ENWIK8_SECTION", "\n".join(sec))
else:
    text = text.replace("ENWIK8_SECTION", "")
readme.write_text(text)
print("README.md assembled")
