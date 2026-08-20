# PRISM

A lossless context-mixing compressor, written from scratch in ~890 lines of C99.

```
make native
./prism c -9 input archive
./prism d archive output
```

---

## First, the honest part

The brief was "invent a compression algorithm better than any existing one." That
exact thing is not achievable, and it is worth being precise about why, because the
reason shapes what *is* worth building.

**No lossless compressor beats another on all inputs.** There are `2^n` distinct
`n`-bit inputs and only `2^n - 1` shorter strings. Any injective encoding that
shortens some inputs must lengthen others, so "universally better" is not a hard
engineering target — it is a false statement about counting. Every real claim is
of the form *better on this class of data, at this cost in time and memory*.

**"Best on realistic data" is also already taken, and not by a wide margin.**
On the standard text benchmark the current frontier is `cmix` and `nncp` at roughly
0.86–1.17 bits per character on enwik8/9 — reached with 10–32 GB of RAM, days of
compute, and in nncp's case a transformer trained during compression. Beating those
is not a weekend's work, and it would not be a new *algorithm* so much as more
models and more compute bolted onto the same context-mixing frame.

So this project targets the question that actually has a good answer: **build a
compressor that beats the general-purpose tools people actually ship — gzip, bzip2,
xz — by a wide margin on ratio, and contribute something to the context-mixing
design space that is not already in the shipping compressors.** Both halves are
measured below, including the parts that did not work.

## What it does

PRISM is a bit-level context-mixing compressor in the lpaq/paq lineage. Each byte
is coded as 8 binary decisions; for every bit, ~27 models each produce a
probability, a network of gated linear mixers combines them in the logistic domain,
two SSE stages recalibrate the result, and a binary arithmetic coder emits it.

```
                 orders 1,2,3,4,5,6,8      \
                 word / word-pair           |
                 sparse + masked contexts   |   27 context models,
                 indirect contexts          |   shared 4-way associative
                 newline column models      |   hash table of bit histories
   input bits -> detected-stride columns    |
                 column linear predictors   /
                 match model  ------------------> 3 direct inputs
                        |
                        v
                 6 layer-1 mixers, gated by { partial byte, previous byte,
                        |                     REGIME, effective order,
                        |                     stride column, byte-class pair }
                        |                    and running at 3 learning rates
                        v
                 layer-2 mixer, gated by REGIME
                        |
                        v
                 SSE stage A (regime x partial byte)  \  averaged with the raw
                 SSE stage B (partial byte x prev)    /  mixer output, 2:1:1
                        |
                        v
                 binary arithmetic coder
```

The architecture — bit histories, StateMaps, logistic mixing, APM/SSE, the nibble-tree
hash table — is the standard one established by Matt Mahoney's PAQ and lpaq work and
by zpaq; see the references. This is an independent implementation of those ideas,
not a fork: no code was copied from any of them. The state-transition table is
generated at start-up from an explicit count ladder rather than hard-coded, so it
can be retuned in one place.

## What is new here

Three things in PRISM are not in the open context-mixing implementations I read
while building it — lpaq1, paq8l, paq8px, zpaq and cmix, whose sources I went
through for exactly this question. That is the claim, and it is deliberately
narrower than "new": I make no claim about compressors I did not read, and the
field's engineering is spread across hundreds of programs and a forum
(encode.su) that this machine's egress policy blocked. Two of the three are also
transfers of ideas that are old elsewhere, credited below.

Each is behind a runtime flag so its contribution is a measured number rather
than an assertion, and the flags are recorded in the stream header so the
decoder mirrors the encoder.

### 1. Harmonic-sum record-length detection (`--no-stride` to disable)

Structured binary data — database rows, star catalogues, sensor tables, 16-bit
images — is dominated by a fixed record length `S`, and knowing `S` unlocks the
single most valuable context there is on such data: the byte in the same column of
the previous record.

Existing detectors have a common weakness. paq8's detector votes on equal spacing
between repeats of the same byte value; zpaq's scans a histogram of gaps between
successive occurrences of each byte and picks the best single bin by a hazard-rate
score. Both are **single-bin statistics**, so a record length of `S` competes with
its own harmonics at `2S`, `3S`, … — and picking `2S` costs you half the column
structure. paq8px carries an explicit special case for the `2×` alias to paper over
exactly this.

PRISM scores the whole comb instead. The technique is not new in general — it is
the harmonic sum used for pitch detection in audio, where the same `S` vs `2S`
ambiguity has been solved this way for decades. What is new is applying it to
record-length detection inside a compressor, and letting its output gate the
mixer rather than only supply contexts:

```
score(S) = cnt[S] + Σ_{j≥2} cnt[j·S] / j
```

over a decaying histogram `cnt[d]` of "how often the byte `d` positions back equals
the current byte", refreshed every 4 KB. The harmonics of the true fundamental
*reinforce* it rather than competing with it, while the fundamental's own bin keeps
`S` ahead of `S/2`. The winner feeds five column contexts (`byte at -S`, `-S and
-2S`, `column × byte at -S`, `column × previous byte`, `byte at -S × previous
byte`), two column linear predictors (`clip(2N-NN)` and `clip(N+W-NW)`, the image
predictors applied to the record grid), and a dedicated mixer weight bank indexed by
column.

The histogram costs one vectorised 512-byte compare per input byte; the scoring pass
is 1.5k integer operations per 4 KB. It runs unsupervised and re-decides every
4 KB, so a file that changes structure partway through is tracked without any block
boundary or file-type detector.

On the synthetic 12-byte-record file in the test suite this is the difference
between 3,036 and 23,554 bytes — **7.8×**. On the Silesia corpus, removing the
whole stride machinery costs +1.39%, concentrated on `sao` (a fixed-width
star catalogue) at +10.4%.

### 2. Regime-gated mixing (`--no-regime` to disable)

paq8px switches its whole model set and SSE chain on a *block type* decided by a
file-type detector; zpaq picks a model string from whole-block measurements before
coding starts. Both are discrete, up-front, file-level decisions.

PRISM instead derives a 6-bit **regime** continuously from three signals that are
already being computed: match strength (2 bits), data class from a decaying
printable/digit density counter (2 bits), and stride class (2 bits). That regime
selects a layer-1 mixer weight bank, gates the layer-2 mixer, contexts the
match model's StateMap, and contexts one of the two SSE stages. The network
therefore re-specialises *inside* a heterogeneous file — a tar of source and
binaries, an archive with a header and a payload — with no block boundary and no
detector.

Measured contribution: +1.26% on the full corpus, worst-hit file `osdb` at
+3.1%. Worth recording how that number moved: while tuning on 2 MB samples the
regime gate looked nearly worthless (0.11%), and it would have been reasonable to
delete it. It only pays off at corpus scale, because a weight bank per regime
needs enough data to fill 64 banks before specialising beats diluting. Tuning
decisions taken on small samples are not safe to extrapolate, and this is the
component that showed it.

### 3. Multi-timescale layer 1

The six layer-1 mixers run at three different learning rates (2⁻¹⁶, 2⁻¹⁷, 2⁻¹⁸ per
unit error) rather than one global rate. Fast mixers track regime changes, slow ones
hold a long-run average, and the regime-gated layer-2 mixer arbitrates. cmix
duplicates gating contexts across learning rates for the same reason; the
combination with an explicit regime gate on the arbitrating layer is what is new
here. Measured at about 0.1% on a 16 MB tuning sample — small, but essentially
free, since it changes a constant rather than adding work.

## Results

Silesia corpus (12 files, 211,938,580 bytes), single thread, `prism -9`.
Every PRISM stream in this table was decompressed and compared against the
original; the `verify` column is that check, not an assertion.

| file | size | gzip -9 | bzip2 -9 | xz -9e | **PRISM -9** | bpb | vs xz | verify |
|---|---:|---:|---:|---:|---:|---:|---:|:--:|
| dickens | 10,192,446 | 3,851,823 | 2,799,520 | 2,831,212 | **2,226,279** | 1.747 | -21.4% | ok |
| mozilla | 51,220,480 | 18,994,142 | 17,914,392 | 13,376,240 | **12,547,375** | 1.960 | -6.2% | ok |
| mr | 9,970,564 | 3,673,940 | 2,441,280 | 2,751,892 | **2,211,898** | 1.775 | -19.6% | ok |
| nci | 33,553,445 | 2,987,533 | 1,812,734 | 1,449,272 | **1,340,893** | 0.320 | -7.5% | ok |
| ooffice | 6,152,192 | 3,090,442 | 2,862,526 | 2,427,224 | **2,047,211** | 2.662 | -15.7% | ok |
| osdb | 10,085,684 | 3,716,342 | 2,802,792 | 2,844,556 | **2,401,645** | 1.905 | -15.6% | ok |
| reymont | 6,627,202 | 1,820,834 | 1,246,230 | 1,315,592 | **994,015** | 1.200 | -24.4% | ok |
| samba | 21,606,400 | 5,408,272 | 4,549,759 | 3,739,524 | **3,280,091** | 1.214 | -12.3% | ok |
| sao | 7,251,944 | 5,327,041 | 4,940,524 | 4,425,664 | **3,831,691** | 4.227 | -13.4% | ok |
| webster | 41,458,703 | 12,061,624 | 8,644,714 | 8,368,672 | **6,328,441** | 1.221 | -24.4% | ok |
| x-ray | 8,474,240 | 6,037,713 | 4,051,112 | 4,491,264 | **3,771,158** | 3.560 | -16.0% | ok |
| xml | 5,345,280 | 662,284 | 441,186 | 434,892 | **390,995** | 0.585 | -10.1% | ok |
| **total** | **211,938,580** | **67,631,990** | **54,506,769** | **48,456,004** | **41,371,692** | **1.562** | **-14.6%** | |

Ratios: gzip -9 3.134x, bzip2 -9 3.888x, xz -9e 4.374x, PRISM 5.123x.
PRISM: 1399 s compress, 1429 s decompress (0.14 / 0.14 MiB/s single-threaded). xz -9e: 211 s compress (0.96 MiB/s).


### enwik8

The other standard reference point, so the result can be placed against the published literature. 100,000,000 bytes of Wikipedia XML; the gzip and bzip2 rows below reproduce the long-published values for this file to within a version's difference, which is the check that this setup is measuring the same thing everyone else is.

| | size | bpc |
|---|---:|---:|
| gzip -9 | 36,445,248 | 2.916 |
| bzip2 -9 | 29,008,758 | 2.321 |
| xz -9e | 24,831,648 | 1.987 |
| **PRISM -9** | **20,685,087** | **1.655** |

16.7% below xz -9e. Verified by decompressing: ok. 599 s to compress, 585 s to decompress, 758 MB peak.

**And this is where PRISM loses.** The published Large Text Compression Benchmark figures (not measured here) put lpaq1 -9 at 19,755,948 bytes, 1.581 bpc -- **4.7% smaller than PRISM**, from a 600-line compressor released in 2007. zpaq -m5 reaches 17,855,729 and cmix v21 14,623,723, the latter on roughly 26 GB of RAM.

The gap is not mysterious: lpaq1 spends its whole model budget on English text -- a word model carrying several previous words, and orders tuned for it -- while PRISM spends a third of its contexts on record structure that enwik8 does not have. That trade is visible in the two benchmarks: PRISM is ahead on Silesia, which is 60% binary and structured, and behind on 100 MB of prose. A compressor is a bet about what its input looks like, and this one bets differently.


## Ablation

Each row recompresses the whole corpus with one component disabled.

| disabled component | corpus total | cost of removing it | worst-hit file |
|---|---:|---:|---|
| *(nothing -- full model)* | 41,371,692 | -- | |
| stride detector + column models | 41,945,503 | **+1.39%** | sao +10.4% |
| regime-gated mixing/SSE | 41,891,443 | **+1.26%** | osdb +3.1% |
| newline column models | 41,588,797 | **+0.52%** | nci +8.6% |
| match model | 42,816,811 | **+3.49%** | xml +14.4% |

The honest reading: the **match model is the largest single contribution** at
3.49%, and it is the least novel part of the whole compressor — it is the LZ77
idea, fed to the mixer as a probability instead of used as a decision. The two
components this project actually contributes come next, at 1.39% and 1.26%, and
each is concentrated exactly where its motivation says it should be (`sao` for
the record grid, `osdb` for regime switching inside a file).

Worth more than any of them was a tuning constant found by measurement, not an
idea: keeping half the final weight on the raw mixer output instead of letting
the SSE stages dominate. The textbook topology (parallel SSE banks averaged with
equal weight) cost 12% on `xml` alone. That is the honest shape of work in this
field — the architecture is published, and most of the remaining distance is
measurement.

## What it costs

This is the part that decides whether you would actually use it.

All measured on the same machine and the same corpus.

| | gzip -9 | xz -9e | PRISM -9 |
|---|---:|---:|---:|
| Silesia ratio | 3.13x | 4.37x | **5.12x** |
| compress | 9.1 MiB/s | 0.96 MiB/s | **0.14 MiB/s** |
| decompress | 145 MiB/s | 55 MiB/s | **0.14 MiB/s** |
| peak memory | 1 MB | 140 MB | **668 MB** |

PRISM is **7x slower to compress than xz -9e and 389x slower to decompress**, for 14.6% fewer bytes.

- **Decompression costs the same as compression.** The model must be rebuilt
  identically, so there is no fast decode path. xz decompresses this corpus at
  55 MiB/s, PRISM at 0.14. For anything write-once-read-many, that is
  disqualifying.
- **No random access, no streaming, no recovery.** The whole file is held in memory
  and the model state is a single unbroken chain; a corrupt byte destroys the
  remainder.
- **Incompressible data expands by about 0.7%.** There is no stored-block escape.
- **Memory is a fixed budget, not a function of input size.** `-9` allocates ~700 MB
  regardless of whether the input is 4 KB or 4 GB.

Where it is genuinely the right tool: cold archival of structured or mixed data
where bytes cost more than cycles and both ends control the software. For pure
English prose, lpaq1 is smaller and roughly ten times faster (see enwik8 above)
-- use that instead.

## Usage

```
prism c|d [-0..-9] [ablation flags] infile outfile

  c            compress
  d            decompress (settings are read from the header)
  -0 .. -9     model memory, 1 MiB .. 512 MiB of hash table (default -7)
  -v           report record-length detections to stderr
  -q           no progress output

  --no-stride  disable stride detection and the column models
  --no-regime  disable regime-gated mixing and SSE
  --no-line    disable the newline column models
  --no-match   disable the match model
  --baseline   --no-stride --no-regime
```

Ablation flags are recorded in the header, so an archive written with them decodes
correctly without repeating them.

```
make            # portable build
make native     # allow -march=native
make check      # round-trip suite: 42 cases incl. empty, 1-byte, random, records
./bench/run_bench.sh /path/to/silesia ./prism -9
./bench/ablation.sh /path/to/silesia ./prism -9
```

## Layout

```
src/prism.c            the whole compressor
bench/roundtrip.sh     correctness suite
bench/run_bench.sh     corpus benchmark against gzip/bzip2/xz, with verification
bench/ablation.sh      per-component contribution measurement
bench/results/         generated tables
```

## References

The architecture follows the published context-mixing literature. Nothing here is
copied, but the debt is total:

- M. Mahoney, *Data Compression Explained*, and the PAQ/lpaq/zpaq sources —
  bit-history states, StateMap, logistic mixing, APM/SSE, the nibble-tree hash table.
  <https://www.mattmahoney.net/dc/dce.html>, <https://github.com/zpaq/zpaq>
- B. Knoll, *cmix* — multi-layer mixing, per-mixer learning rates.
  <https://github.com/byronknoll/cmix>
- *paq8px* — record model, sparse and indirect contexts, SSE topology.
  <https://github.com/hxim/paq8px>
- J. Veness et al., *Gated Linear Networks*, arXiv:1910.01526 — the formal frame in
  which "gate the mixer on any measurable signal" is the free design parameter.
- Silesia corpus: <https://github.com/MiloszKrajewski/SilesiaCorpus>;
  enwik8: <http://prize.hutter1.net/>

## Licence

MIT — see `LICENSE`.
