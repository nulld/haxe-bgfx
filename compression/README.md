# PRISM

A lossless context-mixing compressor, written from scratch in ~885 lines of C99.

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
engineering target -- it is a false statement about counting. Every real claim is
of the form *better on this class of data, at this cost in time and memory*.

**"Best on realistic data" is also already taken, and not by a wide margin.**
On the standard text benchmark the current frontier is `cmix` and `nncp` at roughly
0.86–1.17 bits per character on enwik8/9 -- reached with 10–32 GB of RAM, days of
compute, and in nncp's case a transformer trained during compression. Beating those
is not a weekend's work, and it would not be a new *algorithm* so much as more
models and more compute bolted onto the same context-mixing frame.

So this project targets the question that actually has a good answer: **build a
compressor that beats the general-purpose tools people actually ship -- gzip, bzip2,
xz -- by a wide margin on ratio, and contribute something to the context-mixing
design space that is not already in the shipping compressors.** Both halves are
measured below, including the parts that did not work.

## What it does

PRISM is a bit-level context-mixing compressor in the lpaq/paq lineage. Each byte
is coded as 8 binary decisions; for every bit, ~27 models each produce a
probability, a network of gated linear mixers combines them in the logistic domain,
one SSE stage recalibrates the result, and a binary arithmetic coder emits it.

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
                 one SSE stage (partial byte x previous byte),
                 blended 3:1 with the raw mixer output
                        |
                        v
                 binary arithmetic coder
```

The architecture -- bit histories, StateMaps, logistic mixing, APM/SSE, the nibble-tree
hash table -- is the standard one established by Matt Mahoney's PAQ and lpaq work and
by zpaq; see the references. This is an independent implementation of those ideas,
not a fork: no code was copied from any of them. The state-transition table is
generated at start-up from an explicit count ladder rather than hard-coded, so it
can be retuned in one place.

## What is new here

Three things in PRISM are not in the open context-mixing implementations I read
while building it -- lpaq1, paq8l, paq8px, zpaq and cmix, whose sources I went
through for exactly this question. That is the claim, and it is deliberately
narrower than "new": I make no claim about compressors I did not read, and the
field's engineering is spread across hundreds of programs and a forum
(encode.su) that this machine's egress policy blocked. Two of the three are also
transfers of ideas that are old elsewhere, credited below.

Each is behind a runtime flag so its contribution is a measured number rather
than an assertion, and the flags are recorded in the stream header so the
decoder mirrors the encoder.

### 1. Harmonic-sum record-length detection (`--no-stride` to disable)

Structured binary data -- database rows, star catalogues, sensor tables, 16-bit
images -- is dominated by a fixed record length `S`, and knowing `S` unlocks the
single most valuable context there is on such data: the byte in the same column of
the previous record.

Existing detectors have a common weakness. paq8's detector votes on equal spacing
between repeats of the same byte value; zpaq's scans a histogram of gaps between
successive occurrences of each byte and picks the best single bin by a hazard-rate
score. Both are **single-bin statistics**, so a record length of `S` competes with
its own harmonics at `2S`, `3S`, … -- and picking `2S` costs you half the column
structure. paq8px carries an explicit special case for the `2×` alias to paper over
exactly this.

PRISM scores the whole comb instead. The technique is not new in general -- it is
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
between 3,036 and 23,554 bytes -- **7.8×**. On the Silesia corpus, removing the
whole stride machinery costs +1.46%, concentrated where its motivation says
it should be: `sao` at +11.8%.

### 2. Regime-gated mixing (`--no-regime` to disable)

paq8px switches its whole model set and SSE chain on a *block type* decided by a
file-type detector; zpaq picks a model string from whole-block measurements before
coding starts. Both are discrete, up-front, file-level decisions.

PRISM instead derives a 6-bit **regime** continuously from three signals that are
already being computed: match strength (2 bits), data class from a decaying
printable/digit density counter (2 bits), and stride class (2 bits). That regime
selects a layer-1 mixer weight bank, gates the layer-2 mixer, and contexts the
match model's StateMap. The network therefore re-specialises *inside* a
heterogeneous file -- a tar of source and binaries, an archive with a header and
a payload -- with no block boundary and no detector.

Measured contribution: +0.32% on the full corpus (worst hit: `webster` at +1.0%).

That number has a history worth recording, because it moved twice. While tuning
on 2 MB samples the regime gate measured 0.11% and looked deletable. On the full
corpus it measured 1.26% -- a weight bank per regime needs enough data to fill 64
banks before specialising beats diluting, so small samples systematically
understate it. Then the SSE sweep found that the whole SSE stage the regime was
contexting should be removed; that made the compressor 2.7% smaller overall and
took most of the regime's measured contribution with it, leaving +0.32%.

None of those three numbers is wrong -- they measure the same component in three
different compressors. The honest summary is that the regime gate is worth
having and is not worth much.

### 3. Multi-timescale layer 1

The six layer-1 mixers run at three different learning rates (2⁻¹⁶, 2⁻¹⁷, 2⁻¹⁸ per
unit error) rather than one global rate. Fast mixers track regime changes, slow ones
hold a long-run average, and the regime-gated layer-2 mixer arbitrates. cmix
duplicates gating contexts across learning rates for the same reason; the
combination with an explicit regime gate on the arbitrating layer is what is new
here. Measured at about 0.1% on a 16 MB tuning sample -- small, but essentially
free, since it changes a constant rather than adding work.

## Results

Silesia corpus (12 files, 211,938,580 bytes), single thread, `prism -9`.
Every PRISM stream in this table was decompressed and compared against the
original; the `verify` column is that check, not an assertion.

| file | size | gzip -9 | bzip2 -9 | xz -9e | **PRISM -9** | bpb | vs xz | verify |
|---|---:|---:|---:|---:|---:|---:|---:|:--:|
| dickens | 10,192,446 | 3,851,823 | 2,799,520 | 2,831,212 | **2,197,265** | 1.725 | -22.4% | ok |
| mozilla | 51,220,480 | 18,994,142 | 17,914,392 | 13,376,240 | **12,204,454** | 1.906 | -8.8% | ok |
| mr | 9,970,564 | 3,673,940 | 2,441,280 | 2,751,892 | **2,217,641** | 1.779 | -19.4% | ok |
| nci | 33,553,445 | 2,987,533 | 1,812,734 | 1,449,272 | **1,204,257** | 0.287 | -16.9% | ok |
| ooffice | 6,152,192 | 3,090,442 | 2,862,526 | 2,427,224 | **2,041,127** | 2.654 | -15.9% | ok |
| osdb | 10,085,684 | 3,716,342 | 2,802,792 | 2,844,556 | **2,367,137** | 1.878 | -16.8% | ok |
| reymont | 6,627,202 | 1,820,834 | 1,246,230 | 1,315,592 | **952,820** | 1.150 | -27.6% | ok |
| samba | 21,606,400 | 5,408,272 | 4,549,759 | 3,739,524 | **3,119,091** | 1.155 | -16.6% | ok |
| sao | 7,251,944 | 5,327,041 | 4,940,524 | 4,425,664 | **3,798,031** | 4.190 | -14.2% | ok |
| webster | 41,458,703 | 12,061,624 | 8,644,714 | 8,368,672 | **6,078,942** | 1.173 | -27.4% | ok |
| x-ray | 8,474,240 | 6,037,713 | 4,051,112 | 4,491,264 | **3,742,256** | 3.533 | -16.7% | ok |
| xml | 5,345,280 | 662,284 | 441,186 | 434,892 | **347,888** | 0.521 | -20.0% | ok |
| **total** | **211,938,580** | **67,631,990** | **54,506,769** | **48,456,004** | **40,270,909** | **1.520** | **-16.9%** | |

Ratios: gzip -9 3.134x, bzip2 -9 3.888x, xz -9e 4.374x, PRISM 5.263x.
PRISM: 1203 s compress, 1206 s decompress (0.17 / 0.17 MiB/s single-threaded). xz -9e: 179 s compress (1.13 MiB/s).


### enwik8

The other standard reference point, so the result can be placed against the published literature. 100,000,000 bytes of Wikipedia XML; the gzip and bzip2 rows below reproduce the long-published values for this file to within a version's difference, which is the check that this setup is measuring the same thing everyone else is.

| | size | bpc |
|---|---:|---:|
| gzip -9 | 36,445,248 | 2.916 |
| bzip2 -9 | 29,008,758 | 2.321 |
| xz -9e | 24,831,648 | 1.987 |
| **PRISM -9** | **20,216,047** | **1.617** |

18.6% below xz -9e. Verified by decompressing: ok. 557 s to compress, 570 s to decompress, 757 MB peak. Raising the model memory to -11 (2677 MB) gets to 19,985,924 bytes, 1.599 bpc -- **-1.1% for four times the RAM**, which is the shape of the memory/ratio curve up here.

**And this is where PRISM loses.** The published Large Text Compression Benchmark figures (not measured here) put lpaq1 -9 at 19,755,948 bytes, 1.581 bpc -- smaller than PRISM at either memory level, from a 600-line compressor released in 2007. zpaq -m5 reaches 17,855,729 and cmix v21 14,623,723, the latter on roughly 26 GB of RAM.

The gap is not mysterious: lpaq1 spends its whole model budget on English text -- a word model carrying several previous words, and orders tuned for it -- while PRISM spends a third of its contexts on record structure that enwik8 does not have. That trade is visible in the two benchmarks: PRISM is ahead on Silesia, which is 60% binary and structured, and behind on 100 MB of prose. A compressor is a bet about what its input looks like, and this one bets differently.

## Ablation

Each row recompresses the whole corpus with one component disabled.

| disabled component | corpus total | cost of removing it | worst-hit file |
|---|---:|---:|---|
| *(nothing -- full model)* | 40,270,909 | -- | |
| stride detector + column models | 40,860,380 | **+1.46%** | sao +11.8% |
| regime-gated mixing/SSE | 40,400,690 | **+0.32%** | webster +1.0% |
| newline column models | 40,553,022 | **+0.70%** | nci +11.4% |
| match model | 41,250,583 | **+2.43%** | nci +11.1% |

The honest reading: the **match model is the largest single contribution** at
+2.43%, and it is the least novel part of the whole compressor -- it is the
LZ77 idea, fed to the mixer as a probability instead of used as a decision. The
stride machinery comes next at +1.46%, concentrated exactly where its
motivation says it should be (`sao` at +11.8%). The newline column models, which
are three lines of code, are worth +0.70%. Regime gating is last and smallest
at +0.32%.

Worth more than all of them put together was the SSE blend, arrived at by
sweeping rather than by reasoning. Three quarters of the final weight stays on
the raw mixer output, and the second SSE stage that every reference
implementation has was deleted. Moving from the textbook topology to that was
**2.7% on the full corpus** -- more than the stride detector, the newline models
and the regime gate combined, from removing code rather than adding it.

That is the honest shape of work in this field. The architecture is published
and has been for twenty years; most of the remaining distance is measurement,
and a good deal of it is measurement that tells you to take something out.

## What did not work

Most of what was tried made things worse, and the list is more informative than
the list of what stayed. All figures are on the 16 MB mixed tuning sample.

| attempt | result |
|---|---|
| paq8's five mixer inputs per context (adding bit-history determinism signals) | **+0.65% worse** |
| a second SSE stage chained after the first (the lpaq1 topology) | worth **exactly nothing**; the optimiser drove its weight to zero |
| a second SSE stage in parallel, contexted on the regime | **+0.59% worse** than not having it -- it was removed |
| an ISSE correction ladder over orders 1,2,3,4,6 and the word context (the zpaq idea) | **+0.01%**, i.e. nothing, at three learning rates |
| a run model (last byte and repeat count per context) over three context hashes | **-0.04%**, not worth three tables |
| word-history contexts: skip-a-word pairs, and a letter-stream hash that ignores punctuation | **+0.27% worse on text**, which is where it was supposed to help |
| x86 (prefix, opcode, mod/rm) tuple contexts | -0.03%, dropped |
| magnitude-masked sparse contexts | -0.04%, dropped |
| gating the regime mixer more finely (regime x bit position) | **+0.56% worse** |
| 4x the model memory on enwik8 (-11, 2.7 GB) | -1.1%, and still behind lpaq1 |

There is one theme running through almost all of it: **in a mixer this size,
adding inputs dilutes**. Every model added has to earn its place against the
slower convergence it imposes on every other model's weights, and past about
thirty models most candidates do not. The gains that stuck came from tuning what
was already there -- the SSE blend, the learning rates -- not from adding
machinery. That is not the impression the literature gives, where compressors
are described by their model lists.

## What it costs

This is the part that decides whether you would actually use it.

All measured on the same machine and the same corpus.

| | gzip -9 | xz -9e | PRISM -9 |
|---|---:|---:|---:|
| Silesia ratio | 3.13x | 4.37x | **5.26x** |
| compress | 9.1 MiB/s | 1.13 MiB/s | **0.17 MiB/s** |
| decompress | 149 MiB/s | 56 MiB/s | **0.17 MiB/s** |
| peak memory | 1 MB | 140 MB | **667 MB** |

PRISM is **7x slower to compress than xz -9e and 334x slower to decompress**, for 16.9% fewer bytes.

- **Decompression costs the same as compression.** The model must be rebuilt
  identically, so there is no fast decode path. See the table above: xz
  decompresses this corpus roughly 300 times faster. For anything
  write-once-read-many, that is disqualifying.
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
prism c|d [-0..-11] [ablation flags] infile outfile

  c            compress
  d            decompress (settings are read from the header)
  -0 .. -11    hash table size, 1 MiB .. 2 GiB (default -7 = 128 MiB)
  -v           report record-length detections to stderr
  -q           no progress output

  --no-stride  disable stride detection and the column models
  --no-regime  disable regime-gated mixing
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

- M. Mahoney, *Data Compression Explained*, and the PAQ/lpaq/zpaq sources --
  bit-history states, StateMap, logistic mixing, APM/SSE, the nibble-tree hash table.
  <https://www.mattmahoney.net/dc/dce.html>, <https://github.com/zpaq/zpaq>
- B. Knoll, *cmix* -- multi-layer mixing, per-mixer learning rates.
  <https://github.com/byronknoll/cmix>
- *paq8px* -- record model, sparse and indirect contexts, SSE topology.
  <https://github.com/hxim/paq8px>
- J. Veness et al., *Gated Linear Networks*, arXiv:1910.01526 -- the formal frame in
  which "gate the mixer on any measurable signal" is the free design parameter.
- Silesia corpus: <https://github.com/MiloszKrajewski/SilesiaCorpus>;
  enwik8: <http://prize.hutter1.net/>

## Licence

MIT -- see `LICENSE`.
