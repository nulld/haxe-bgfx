/* PRISM - a context-mixing lossless compressor with online structure detection.
 *
 * Architecture (see ../README.md for the full write-up):
 *
 *   input bits --> [ ~20 context models ] --> [ 6 gated linear mixers ]
 *                  [ match model        ]         |
 *                  [ stride/record model]         v
 *                  [ line/column model  ]    [ layer-2 mixer ]
 *                                                 |
 *                                                 v
 *                                          [ 2 chained SSE/APM ]
 *                                                 |
 *                                                 v
 *                                        binary arithmetic coder
 *
 * The two pieces that are not standard lpaq/paq practice:
 *
 *   1. an O(1)-per-byte record-length ("stride") detector built on a decaying
 *      histogram of repeat offsets, feeding five dedicated column models, and
 *   2. a 6-bit "regime" signal (match strength x data class x stride class)
 *      that selects mixer weight banks and SSE contexts, so the network
 *      re-specialises inside heterogeneous files without block boundaries.
 *
 * Public domain / MIT, see LICENSE in this directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

typedef uint8_t  U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef uint64_t U64;

#define PRISM_MAGIC  0x4D535250u   /* "PRSM" */
#define PRISM_FORMAT 1

/* ------------------------------------------------------------------ */
/* feature mask (stored in the header so the decoder mirrors encoding)  */
/* ------------------------------------------------------------------ */
#define F_STRIDE  1u   /* stride/record models + stride-gated mixer bank */
#define F_REGIME  2u   /* regime-gated mixer banks and SSE               */
#define F_LINE    4u   /* newline/column models                          */
#define F_MATCH   8u   /* match model                                    */
#define F_ALL     (F_STRIDE|F_REGIME|F_LINE|F_MATCH)

static U32 features = F_ALL;

/* ------------------------------------------------------------------ */
/* squash / stretch                                                     */
/* ------------------------------------------------------------------ */

static short stretch_tab[4096];

static int squash(int d) {
  static const int t[33] = {
       1,   2,   3,   6,  10,  16,  27,  45,  73, 120, 194, 310, 488,
     747,1101,1546,2047,2549,2994,3348,3607,3785,3901,3975,4024,4050,
    4068,4079,4085,4089,4092,4093,4094 };
  int w, i;
  if (d > 2047) return 4095;
  if (d < -2047) return 0;
  w = d & 127;
  i = (d >> 7) + 16;
  return (t[i] * (128 - w) + t[i + 1] * w + 64) >> 7;
}

static void init_stretch(void) {
  int pi = 0, x, j;
  for (x = -2047; x <= 2047; ++x) {
    int v = squash(x);
    for (j = pi; j <= v; ++j) stretch_tab[j] = (short)x;
    pi = v + 1;
  }
  for (j = pi; j < 4096; ++j) stretch_tab[j] = 2047;
}

#define stretch(p) ((int)stretch_tab[(p)])

static int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* ------------------------------------------------------------------ */
/* hashing                                                              */
/* ------------------------------------------------------------------ */

static inline U32 hsh(U64 x) {
  x *= 0x9E3779B97F4A7C15ULL;
  x ^= x >> 29;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 32;
  return (U32)x;
}
static inline U32 h2(U64 a, U64 b)          { return hsh(a * 0x2545F4914F6CDD1DULL + b + 0x9E3779B9ULL); }
static inline U32 h3(U64 a, U64 b, U64 c)   { return hsh((a * 0x2545F4914F6CDD1DULL + b) * 0x100000001B3ULL + c); }

/* ------------------------------------------------------------------ */
/* bit-history state machine                                            */
/*                                                                      */
/* States are bounded (n0,n1) pairs quantised onto a ladder.  Observing  */
/* bit y increments n_y and discounts n_(1-y) -- the usual PAQ           */
/* non-stationary rule, which lets a model forget a stale bias fast.     */
/* The table is generated at start-up rather than hard-coded so the      */
/* ladder can be tuned in one place.                                     */
/* ------------------------------------------------------------------ */

#define NLAD 15
static const int LAD[NLAD] = {0,1,2,3,4,5,6,8,10,12,15,20,26,34,44};

static U8  nex_tab[256][2];
static U8  st_total[256];      /* n0+n1, capped -- used as eviction priority */
static U16 st_p0[256];         /* implied P(bit=1) in 12 bits, for StateMap priors */
static int nstates = 0;

static int lad_index_le(int v) {           /* largest ladder index with LAD[i] <= v */
  int i, best = 0;
  for (i = 0; i < NLAD; ++i) if (LAD[i] <= v) best = i;
  return best;
}
static int discount(int v) { return v <= 2 ? v : (v / 2) + 1; }

static void init_states(void) {
  static short id[NLAD][NLAD];
  int i, j, s;
  int ni[256], nj[256];

  for (i = 0; i < NLAD; ++i) for (j = 0; j < NLAD; ++j) id[i][j] = -1;

  /* enumerate the reachable set: at least one of the two counts stays small */
  for (i = 0; i < NLAD; ++i)
    for (j = 0; j < NLAD; ++j)
      if (LAD[i] <= 5 || LAD[j] <= 5) {
        if (nstates >= 256) continue;
        id[i][j] = (short)nstates;
        ni[nstates] = i; nj[nstates] = j;
        ++nstates;
      }

  for (s = 0; s < nstates; ++s) {
    int y;
    int v0 = LAD[ni[s]], v1 = LAD[nj[s]];
    int tot = v0 + v1;
    st_total[s] = (U8)(tot > 255 ? 255 : tot);
    /* Krichevsky-Trofimov style prior */
    st_p0[s] = (U16)clampi((int)(((v1 + 0.4) / (v0 + v1 + 0.8)) * 4096.0), 1, 4094);

    for (y = 0; y < 2; ++y) {
      int a0 = v0, a1 = v1, k0, k1, t;
      if (y) { a1 = LAD[ni[s] >= 0 ? (nj[s] + 1 < NLAD ? nj[s] + 1 : NLAD - 1) : 0]; a0 = discount(v0); }
      else   { a0 = LAD[ni[s] + 1 < NLAD ? ni[s] + 1 : NLAD - 1];                    a1 = discount(v1); }
      k0 = lad_index_le(a0);
      k1 = lad_index_le(a1);
      /* keep the pair inside the reachable set */
      if (LAD[k0] > 5 && LAD[k1] > 5) { if (LAD[k0] < LAD[k1]) k0 = lad_index_le(5); else k1 = lad_index_le(5); }
      t = id[k0][k1];
      if (t < 0) t = 0;
      nex_tab[s][y] = (U8)t;
    }
  }
  for (s = nstates; s < 256; ++s) { nex_tab[s][0] = nex_tab[s][1] = 0; st_total[s] = 0; st_p0[s] = 2048; }
}

#define nex(s,y) nex_tab[s][y]

/* ------------------------------------------------------------------ */
/* StateMap: adaptive probability with a 1/(n+2) learning rate          */
/* layout: high 22 bits = P(1), low 10 bits = observation count         */
/* ------------------------------------------------------------------ */

typedef struct { U32 *t; U32 n; U32 cx; int limit; } StateMap;

static void sm_init(StateMap *m, U32 n, int limit, int state_prior) {
  U32 i;
  m->t = (U32*)malloc(n * sizeof(U32));
  if (!m->t) { fprintf(stderr, "prism: out of memory\n"); exit(1); }
  m->n = n; m->cx = 0; m->limit = limit;
  for (i = 0; i < n; ++i) {
    U32 p = state_prior ? ((U32)st_p0[i & 255] << 10) : (1u << 21);
    m->t[i] = p << 10;
  }
}
static inline int sm_p(StateMap *m, U32 cx) { m->cx = cx; return (int)(m->t[cx] >> 20); }
static inline void sm_update(StateMap *m, int y) {
  U32 *p = &m->t[m->cx];
  U32 v = *p;
  int n = (int)(v & 1023);
  int pr = (int)(v >> 10);
  int target = y ? ((1 << 22) - 1) : 0;
  int delta = (target - pr) / (n + 2);
  if (n < m->limit) ++v;
  v += ((U32)delta) << 10;
  *p = v;
}

/* ------------------------------------------------------------------ */
/* APM / SSE: refine a probability given a context, 33 interpolated bins */
/* ------------------------------------------------------------------ */

typedef struct { U16 *t; U32 idx; int rate; } APM;

static void apm_init(APM *a, U32 n, int rate) {
  U32 i, j;
  a->t = (U16*)malloc(n * 33 * sizeof(U16));
  if (!a->t) { fprintf(stderr, "prism: out of memory\n"); exit(1); }
  a->rate = rate; a->idx = 0;
  for (i = 0; i < n; ++i)
    for (j = 0; j < 33; ++j)
      a->t[i * 33 + j] = (U16)(squash(((int)j - 16) * 128) * 16);
}
static inline int apm_pp(APM *a, int pr, U32 cx) {
  int s = stretch(pr) + 2048;
  int w = s & 127;
  U32 i = (U32)(s >> 7) + cx * 33;
  a->idx = i + (U32)(w >> 6);
  return (a->t[i] * (128 - w) + a->t[i + 1] * w) >> 11;
}
static inline void apm_update(APM *a, int y) {
  int g = (y << 16) + (y << a->rate) - y - y;
  a->t[a->idx]     = (U16)(a->t[a->idx]     + ((g - a->t[a->idx])     >> a->rate));
  a->t[a->idx + 1] = (U16)(a->t[a->idx + 1] + ((g - a->t[a->idx + 1]) >> a->rate));
}

/* ------------------------------------------------------------------ */
/* Mixer: gated linear mixing in the logistic domain                     */
/* p = squash( w[ctx] . stretch(p_i) ),  w += lr * (y - p) * x           */
/* ------------------------------------------------------------------ */

typedef struct {
  int *w;        /* nsets * n weights, 16.16 fixed point */
  int *x;        /* shared input vector (owned by caller) */
  int n;         /* number of inputs */
  U32 base;      /* offset of the selected weight bank */
  int pr;        /* last output, 12-bit */
  int lr;        /* learning rate */
  int dot;
} Mixer;

static void mx_init(Mixer *m, int n, U32 nsets, int *x, int lr, int winit) {
  U32 i, tot = (U32)n * nsets;
  m->w = (int*)malloc(tot * sizeof(int));
  if (!m->w) { fprintf(stderr, "prism: out of memory\n"); exit(1); }
  for (i = 0; i < tot; ++i) m->w[i] = winit;
  m->x = x; m->n = n; m->base = 0; m->pr = 2048; m->lr = lr; m->dot = 0;
}
static inline int mx_mix(Mixer *m, U32 set) {
  const int *w = m->w + (size_t)set * m->n;
  const int *x = m->x;
  int i, n = m->n, s = 0;
  m->base = (U32)((size_t)set * m->n);
  /* Weights live in 16.16 fixed point but the dot product only needs the top
   * bits, so drop 8 of them and the whole reduction fits in 32-bit lanes --
   * which is what lets the compiler vectorise it 8-wide. */
  for (i = 0; i < n; ++i) s += (w[i] >> 8) * x[i];
  m->dot = clampi(s >> 8, -2047, 2047);
  m->pr = squash(m->dot);
  return m->pr;
}
static inline void mx_update(Mixer *m, int y) {
  int *w = m->w + m->base;
  const int *x = m->x;
  int i, n = m->n;
  int err = (y << 12) - m->pr;
  for (i = 0; i < n; ++i) {
    int v = w[i] + ((x[i] * err + (1 << (m->lr - 1))) >> m->lr);
    w[i] = v < -(1 << 19) ? -(1 << 19) : (v > (1 << 19) ? (1 << 19) : v);
  }
}

/* ------------------------------------------------------------------ */
/* carry-less binary arithmetic coder (12-bit probabilities)             */
/* ------------------------------------------------------------------ */

typedef struct { U32 x1, x2, x; FILE *f; int decode; } Coder;

static void rc_init(Coder *c, FILE *f, int decode) {
  int i;
  c->x1 = 0; c->x2 = 0xffffffffu; c->x = 0; c->f = f; c->decode = decode;
  if (decode) for (i = 0; i < 4; ++i) { int ch = getc(f); c->x = (c->x << 8) | (U32)(ch & 255); }
}
static inline int rc_code(Coder *c, int p, int y) {
  U32 xmid = c->x1 + (U32)(((U64)(c->x2 - c->x1) * (U32)p) >> 12);
  if (c->decode) y = (c->x <= xmid);
  if (y) c->x2 = xmid; else c->x1 = xmid + 1;
  while (((c->x1 ^ c->x2) & 0xff000000u) == 0) {
    if (c->decode) { int ch = getc(c->f); c->x = (c->x << 8) | (U32)(ch & 255); }
    else putc((int)(c->x2 >> 24), c->f);
    c->x1 <<= 8;
    c->x2 = (c->x2 << 8) | 255;
  }
  return y;
}
static void rc_flush(Coder *c) {
  if (!c->decode) { int i; for (i = 0; i < 4; ++i) { putc((int)(c->x1 >> 24), c->f); c->x1 <<= 8; } }
}

/* ------------------------------------------------------------------ */
/* the model set                                                        */
/* ------------------------------------------------------------------ */

enum {
  C_O1, C_O2, C_O3, C_O4, C_O6, C_O8,          /* plain byte orders          */
  C_WORD, C_WORD2, C_WPFX,                     /* whitespace-delimited words */
  C_SP13, C_SP14, C_SP24, C_SPMASK,            /* sparse / masked            */
  C_O5,                                        /* order 5                    */
  C_MATCH,                                     /* what the match model expects */
  C_LINE1, C_LINE2,                            /* newline column models      */
  C_ST1, C_ST2, C_ST3, C_ST4, C_ST5,           /* detected-stride columns    */
  C_ST6, C_ST7,                                /* column linear predictors   */
  C_IND1, C_IND2, C_IND3,                      /* indirect: what follows what */
  NCM
};

/* ContextMap: one shared hash table, 16-byte buckets, 4-way associative
 * inside a 64-byte line.  Each bucket holds a checksum byte plus the 15
 * bit-history states of one nibble tree. */
typedef struct {
  U8  *t;
  U32  gmask;            /* (#64-byte groups) - 1 */
  U32  cx[NCM];
  U8  *bp[NCM];
  U8  *cp[NCM];
  int  idx[NCM];
  int  on[NCM];
  StateMap sm[NCM];
} CM;

static CM cm;

/* Indirect model: instead of "context -> statistics", keep "context -> the
 * recent bytes that followed it" and use that history string as the context.
 * It generalises over contexts that behave alike, which no order-N model can. */
static U32 ind1[256];
static U16 ind2[1 << 16];
static inline int match_bucket(void);

static void cm_alloc(CM *c, size_t bytes) {
  size_t groups = bytes / 64, i;
  if (groups < 1024) groups = 1024;
  c->t = (U8*)calloc(groups, 64);
  if (!c->t) { fprintf(stderr, "prism: cannot allocate %.1f MiB for the model\n", groups * 64.0 / 1048576.0); exit(1); }
  c->gmask = (U32)(groups - 1);
  for (i = 0; i < NCM; ++i) {
    sm_init(&c->sm[i], 256, 1023, 1);
    c->cx[i] = 0; c->bp[i] = c->t; c->cp[i] = c->t + 1; c->idx[i] = 1; c->on[i] = 0;
  }
}

static inline U8 *cm_bucket(CM *c, U32 h) {
  U32 chk = ((h >> 24) ^ (h >> 16)) & 255;
  U8 *g, *b;
  int j, best = 0, bpri = 1 << 30;
  if (!chk) chk = 1;
  g = c->t + (size_t)(h & c->gmask) * 64;
  for (j = 0; j < 4; ++j) if (g[j * 16] == (U8)chk) return g + j * 16;
  for (j = 0; j < 4; ++j) {
    int pri = g[j * 16] == 0 ? -1 : (int)st_total[g[j * 16 + 1]];
    if (pri < bpri) { bpri = pri; best = j; }
  }
  b = g + best * 16;
  memset(b, 0, 16);
  b[0] = (U8)chk;
  return b;
}

/* ------------------------------------------------------------------ */
/* match model                                                          */
/* ------------------------------------------------------------------ */

#define MM_MINLEN 8
static U32 *mm_ht = NULL;
static U32  mm_mask = 0;
static U32  mm_ptr = 0;
static int  mm_len = 0;
static int  mm_bit = 0;
static int  mm_valid = 0;
static StateMap sm_match, sm_match2;

/* ------------------------------------------------------------------ */
/* stride / record-length detector                                      */
/*                                                                      */
/* Every byte we look up where the previous occurrence of the current    */
/* 4-gram was and vote for that offset in a decaying histogram.  Fixed-  */
/* size records make one offset (and its multiples) dominate; we then    */
/* fold multiples down to the fundamental period.  O(1) per byte, plus   */
/* one O(SD_MAX) sweep every SD_WINDOW bytes.                            */
/* ------------------------------------------------------------------ */

static U8 *buf;
static U64 pos;

#define SD_MAX     512
#define SD_WINDOW  4096
static U16  sd_cnt[SD_MAX];
static int  sd_stride = 0;
static U64  sd_next = SD_WINDOW;
static int  sd_verbose = 0;

/* Fold the winning lag down to its fundamental period: if the record length
 * is 8, lags 16 and 24 also score highly, and the shortest one carries the
 * most usable context. */
static void sd_resolve(U64 at) {
  int d, best = 0, bv = 0, prev = sd_stride;
  long long sum = 0;
  /* Score every candidate period by a harmonic sum over its own lag and its
   * multiples.  A record length S also lights up 2S, 3S, ...; scoring the
   * comb rather than a single bin means the harmonics reinforce the true
   * fundamental instead of competing with it, which is what makes plain
   * argmax pick 2S about as often as S. */
  {
    long long bscore = 0;
    for (d = 2; d < SD_MAX; ++d) sum += sd_cnt[d];
    for (d = 2; d <= SD_MAX / 2; ++d) {
      long long sc = sd_cnt[d];
      int j;
      for (j = 2; j <= 6 && j * d < SD_MAX; ++j) sc += sd_cnt[j * d] / j;
      if (sc > bscore) { bscore = sc; best = d; }
    }
    bv = best ? sd_cnt[best] : 0;
  }
  if (bv >= 640 && (long long)bv * SD_MAX > sum * 3) sd_stride = best;
  else sd_stride = 0;
  if (sd_verbose && sd_stride != prev)
    fprintf(stderr, "[stride] @%llu: %d -> %d (score %d)\n",
            (unsigned long long)at, prev, sd_stride, bv);
  for (d = 0; d < SD_MAX; ++d) sd_cnt[d] = (U16)(sd_cnt[d] - (sd_cnt[d] >> 1));
}

/* O(SD_MAX) per byte but fully vectorisable and entirely inside L1: count,
 * for every lag d, how often the byte at distance d equals the current one. */
static inline void sd_observe(U8 c) {
  int d, lim = (pos < SD_MAX) ? (int)pos : SD_MAX;
  const U8 *b = buf + pos;
  for (d = 1; d < lim; ++d) sd_cnt[d] = (U16)(sd_cnt[d] + (b[-d] == c));
}

/* ------------------------------------------------------------------ */
/* global model state                                                   */
/* ------------------------------------------------------------------ */

#define NINPUT 64   /* exactly 27*2 context inputs + 6 direct + 3 match + bias */
#define NMIX1  6

static int  c0 = 1;             /* partial byte, leading 1 */
static int  bitpos = 0;

static U32  wordh = 0, prevword = 0;
static U64  ln_start = 0, ln_prev = 0;
static int  tscore = 0, dscore = 0;
static int  regime = 0;
static int  eff_order = 0;      /* how deep a context currently has evidence */
static int  colidx = 0;

static int  xin[NINPUT];
static Mixer mx1[NMIX1];
static int  x2in[NMIX1 + 1];
static Mixer mx2;
static APM  apm2;
static StateMap sm_o0, sm_o1, sm_o2;
static U32  o2ctx = 0;

static inline int bget(U64 back) { return (pos >= back) ? buf[pos - back] : 0; }

static void models_init(int level) {
  size_t cmbytes = (size_t)1 << (20 + level);
  if (level < 0 || level > 11) { fprintf(stderr, "prism: bad level in header\n"); exit(1); }
  U32 mmbits = (U32)(16 + level);
  int i;

  init_stretch();
  init_states();
  cm_alloc(&cm, cmbytes);

  mm_mask = (1u << mmbits) - 1;
  mm_ht = (U32*)calloc((size_t)mm_mask + 1, sizeof(U32));
  if (!mm_ht) { fprintf(stderr, "prism: out of memory\n"); exit(1); }

  sm_init(&sm_match,  64 * 8, 1023, 0);
  sm_init(&sm_match2, 1024,   1023, 0);
  sm_init(&sm_o0, 256, 1023, 0);
  sm_init(&sm_o1, 1 << 16, 1023, 0);
  sm_init(&sm_o2, 1 << 22, 1023, 0);

  for (i = 0; i < NMIX1; ++i) {
    static const U32 sets[NMIX1] = { 256, 256, 64, 128, 64, 256 };
    /* Layer 1 deliberately runs at three timescales: the fast mixers track
     * regime changes, the slow ones hold a long-run average, and the gated
     * layer-2 mixer decides which to believe right now. */
    static const int lrs[NMIX1]  = { 16, 17, 18, 17, 18, 16 };
    mx_init(&mx1[i], NINPUT, sets[i], xin, lrs[i], 1 << 14);
  }
  mx_init(&mx2, NMIX1 + 1, 64, x2in, 16, 1 << 15);
  apm_init(&apm2, 1 << 16, 6);

  memset(sd_cnt, 0, sizeof(sd_cnt));
}

/* set the byte-level contexts; called once per byte boundary */
static void set_contexts(void) {
  int b1 = bget(1), b2 = bget(2), b3 = bget(3), b4 = bget(4);
  U64 last8 = 0;
  int i, S = (features & F_STRIDE) ? sd_stride : 0;
  int col = 0, bS = 0, b2S = 0;
  U64 lcol;

  for (i = 1; i <= 8; ++i) last8 = (last8 << 8) | (U64)bget(i);

  cm.cx[C_O1]  = h2(0x01, (U64)b1);
  cm.cx[C_O2]  = h2(0x02, last8 & 0xffffULL);
  cm.cx[C_O3]  = h2(0x03, last8 & 0xffffffULL);
  cm.cx[C_O4]  = h2(0x04, last8 & 0xffffffffULL);
  cm.cx[C_O6]  = h2(0x06, last8 & 0xffffffffffffULL);
  cm.cx[C_O8]  = h2(0x08, last8);
  cm.cx[C_O5]  = h2(0x05, last8 & 0xffffffffffULL);
  for (i = 0; i < 6; ++i) cm.on[i] = 1;
  cm.on[C_O5] = 1;

  /* What the match model expects, as an ordinary context: lets the mixer
   * learn how far to trust the match in this part of the file. */
  if ((features & F_MATCH) && mm_len > 0 && (U64)mm_ptr < pos) {
    cm.cx[C_MATCH] = h3(0x09, (U64)buf[mm_ptr], (U64)match_bucket());
    cm.on[C_MATCH] = 1;
  } else cm.on[C_MATCH] = 0;

  cm.cx[C_WORD]  = h2(0x10, (U64)wordh);
  cm.cx[C_WORD2] = h3(0x11, (U64)wordh, (U64)prevword);
  cm.cx[C_WPFX]  = h3(0x12, (U64)wordh, (U64)b1);
  cm.on[C_WORD] = cm.on[C_WORD2] = cm.on[C_WPFX] = 1;

  cm.cx[C_SP13]   = h3(0x20, (U64)b1, (U64)b3);
  cm.cx[C_SP14]   = h3(0x21, (U64)b1, (U64)b4);
  cm.cx[C_SP24]   = h3(0x22, (U64)b2, (U64)b4);
  cm.cx[C_SPMASK] = h3(0x23, (U64)((b1 & 0xf0) | ((b2 & 0xf0) << 8)), (U64)(b3 & 0xf0));
  cm.on[C_SP13] = cm.on[C_SP14] = cm.on[C_SP24] = cm.on[C_SPMASK] = 1;

  if (features & F_LINE) {
    int above = 0, above2 = 0;
    lcol = pos - ln_start;
    if (ln_prev + lcol < ln_start)     above  = buf[ln_prev + lcol];
    if (ln_prev + lcol + 1 < ln_start) above2 = buf[ln_prev + lcol + 1];
    cm.cx[C_LINE1] = h3(0x30, (U64)above, (U64)((above2 << 8) | (lcol < 63 ? (int)lcol : 63)));
    cm.cx[C_LINE2] = h3(0x31, (U64)above, (U64)b1);
    cm.on[C_LINE1] = cm.on[C_LINE2] = 1;
  } else {
    cm.on[C_LINE1] = cm.on[C_LINE2] = 0;
  }

  if (S >= 2) {
    col = (int)(pos % (U64)S);
    bS  = bget((U64)S);
    b2S = bget((U64)S * 2);
    cm.cx[C_ST1] = h2(0x40, (U64)bS);
    cm.cx[C_ST2] = h3(0x41, (U64)bS, (U64)b2S);
    cm.cx[C_ST3] = h3(0x42, (U64)col, (U64)bS);
    cm.cx[C_ST4] = h3(0x43, (U64)col, (U64)b1);
    cm.cx[C_ST5] = h3(0x44, (U64)bS, (U64)b1);
    /* Numeric columns are better predicted by extrapolating down the column
     * than by matching it: these are the image predictors applied to the
     * detected record grid. */
    cm.cx[C_ST6] = h2(0x45, (U64)clampi(2 * bS - b2S, 0, 255));
    cm.cx[C_ST7] = h3(0x46, (U64)clampi(bS + b1 - bget((U64)S + 1), 0, 255), (U64)col);
    for (i = C_ST1; i <= C_ST7; ++i) cm.on[i] = 1;
    colidx = col < 63 ? col : 63;
  } else {
    for (i = C_ST1; i <= C_ST7; ++i) cm.on[i] = 0;
    colidx = 0;
  }

  {
    U64 t = (U64)b1 | ((U64)ind1[b1] << 8);
    U32 dd = (U32)(b1 | (b2 << 8));
    cm.cx[C_IND1] = h2(0x50, t & 0xffffffULL);
    cm.cx[C_IND2] = h2(0x51, t);
    cm.cx[C_IND3] = h2(0x52, (U64)dd | ((U64)ind2[dd] << 16));
    cm.on[C_IND1] = cm.on[C_IND2] = cm.on[C_IND3] = 1;
  }

  o2ctx = (h3(0x60, (U64)b1, (U64)b2) & 0x3fff) << 8;

  /* Issue every hash-table probe before touching any of them, so the ~20
   * independent DRAM misses overlap instead of serialising.  This is worth
   * more than any arithmetic optimisation in this loop. */
  for (i = 0; i < NCM; ++i)
    if (cm.on[i]) __builtin_prefetch(cm.t + (size_t)(cm.cx[i] & cm.gmask) * 64, 1, 3);
  __builtin_prefetch((const char*)(sm_o2.t + o2ctx), 1, 3);
  __builtin_prefetch((const char*)(sm_o2.t + o2ctx + 128), 1, 3);
  __builtin_prefetch((const char*)(sm_o2.t + o2ctx + 192), 1, 3);
  for (i = 0; i < NCM; ++i) {
    if (cm.on[i]) {
      cm.bp[i] = cm_bucket(&cm, cm.cx[i]);
      cm.idx[i] = 1;
      cm.cp[i] = cm.bp[i] + 1;
    }
  }
}

/* ------------------------------------------------------------------ */
/* prediction                                                           */
/* ------------------------------------------------------------------ */

static inline int match_bucket(void) {
  return mm_len == 0 ? 0 : (mm_len < 8 ? 1 : (mm_len < 32 ? 2 : 3));
}

static int predict(void) {
  int k = 0, i, pr, p;
  U32 sets[NMIX1];

  /* Five inputs per context, not one.  Beyond the stretched probability the
   * mixer also sees how *deterministic* the bit history is -- a context that
   * has never seen a zero is qualitatively different from one that is merely
   * 90% ones, and a linear mixer cannot recover that from p alone. */
  for (i = 0; i < NCM; ++i) {
    if (cm.on[i]) {
      p = sm_p(&cm.sm[i], (U32)(*cm.cp[i]));
      xin[k++] = stretch(p);
      xin[k++] = (p >> 4) - 128;
    } else { xin[k] = xin[k+1] = 0; k += 2; }
  }

  p = sm_p(&sm_o0, (U32)c0);                          xin[k++] = stretch(p); xin[k++] = (p - 2048) >> 4;
  p = sm_p(&sm_o1, (U32)((bget(1) << 8) | c0));       xin[k++] = stretch(p); xin[k++] = (p - 2048) >> 4;
  p = sm_p(&sm_o2, o2ctx | (U32)c0);                  xin[k++] = stretch(p); xin[k++] = (p - 2048) >> 4;

  mm_valid = 0;
  if ((features & F_MATCH) && mm_len > 0) {
    int expected = buf[mm_ptr];
    if (((expected | 256) >> (8 - bitpos)) == c0) {
      int lb = mm_len < 31 ? mm_len : 31;
      int lb2 = mm_len < 7 ? mm_len : 7;
      mm_valid = 1;
      mm_bit = (expected >> (7 - bitpos)) & 1;
      p = sm_p(&sm_match, (U32)(((lb << 1) | mm_bit) * 8 + bitpos));
      xin[k++] = stretch(p);
      p = sm_p(&sm_match2, (U32)((((features & F_REGIME) ? regime : 0) << 4) | (lb2 << 1) | mm_bit));
      xin[k++] = stretch(p);
      xin[k++] = (mm_bit ? 1 : -1) * clampi(mm_len * 24, 0, 1600);
    } else {
      mm_len = 0;
      xin[k++] = 0; xin[k++] = 0; xin[k++] = 0;
    }
  } else { xin[k++] = 0; xin[k++] = 0; xin[k++] = 0; }

  xin[k++] = 256;   /* bias */
  while (k < NINPUT) xin[k++] = 0;

  /* "Effective order": how many of the deep contexts have any evidence at
   * all, or -- when a match is running -- how long it is.  A free and very
   * strong signal for how much the mixer should trust the high orders. */
  if (mm_len == 0)
    eff_order = (*cm.cp[C_O2] != 0) + (*cm.cp[C_O3] != 0) + (*cm.cp[C_O4] != 0) + (*cm.cp[C_O6] != 0);
  else
    eff_order = 5 + (mm_len >= 8) + (mm_len >= 12) + (mm_len >= 16) + (mm_len >= 32);

  sets[0] = (U32)c0;
  sets[1] = (U32)bget(1);
  sets[2] = (features & F_REGIME) ? (U32)regime : 0;
  sets[3] = (U32)(eff_order * 8 + bitpos);
  sets[4] = (features & F_STRIDE) ? (U32)colidx : 0;
  sets[5] = (U32)(((bget(1) >> 4) << 4) | (bget(2) >> 4));

  for (i = 0; i < NMIX1; ++i) x2in[i] = stretch(mx_mix(&mx1[i], sets[i]));
  x2in[NMIX1] = 256;

  pr = mx_mix(&mx2, (features & F_REGIME) ? (U32)regime : 0);
  /* One SSE stage, and the raw mixer output keeps three quarters of the weight.
   * Every part of that went against the textbook and every part of it is
   * measured: a second chained stage was worth exactly nothing here, a second
   * parallel stage contexted on the regime was worse than not having it, and
   * letting SSE take more of the weight caps how confident the final
   * prediction can get -- which costs real bits on predictable data. */
  pr = (3 * pr + apm_pp(&apm2, pr, (U32)(((c0 << 8) | bget(1)) & 0xffff)) + 2) >> 2;
  return clampi(pr, 1, 4094);
}

/* ------------------------------------------------------------------ */
/* per-byte model update                                                */
/* ------------------------------------------------------------------ */

static void byte_update(int c) {
  buf[pos] = (U8)c;
  if (features & F_STRIDE) sd_observe((U8)c);

  if (mm_len > 0) {
    if (buf[mm_ptr] == (U8)c) { ++mm_ptr; if (mm_len < 60000) ++mm_len; }
    else mm_len = 0;
  }
  ++pos;

  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 128)
    wordh = hsh(((U64)wordh << 8) | (U64)(c | 32));
  else { if (wordh) prevword = wordh; wordh = 0; }

  if (c == '\n') { ln_prev = ln_start; ln_start = pos; }

  if (pos >= 3) {
    U32 dd = (U32)(buf[pos - 2] | (buf[pos - 3] << 8));
    ind1[buf[pos - 2]] = (ind1[buf[pos - 2]] << 8) | (U32)c;
    ind2[dd] = (U16)((ind2[dd] << 8) | (U32)c);
  }

  tscore += (((c >= 32 && c < 127) || c == '\n' || c == '\t') ? 32 : 0) - (tscore >> 5);
  dscore += ((c >= '0' && c <= '9') ? 32 : 0) - (dscore >> 5);

  if ((features & F_MATCH) && pos >= MM_MINLEN) {
    U64 v; U32 h;
    memcpy(&v, buf + pos - 8, 8);
    h = hsh(v) & mm_mask;
    if (mm_len == 0) {
      U32 cand = mm_ht[h];
      if (cand > 0 && (U64)cand < pos) {
        int l = 0;
        while (l < 32 && (U64)l < (U64)cand && (U64)l < pos && buf[cand - 1 - l] == buf[pos - 1 - l]) ++l;
        if (l >= MM_MINLEN) { mm_ptr = cand; mm_len = l; }
      }
    }
    mm_ht[h] = (U32)pos;
  }

  if ((features & F_STRIDE) && pos >= sd_next) { sd_next = pos + SD_WINDOW; sd_resolve(pos); }

  {
    int mb = match_bucket(), tc, sc;
    if (tscore < 384) tc = 0; else if (tscore < 832) tc = 1; else tc = (dscore > 320 ? 3 : 2);
    sc = sd_stride == 0 ? 0 : (sd_stride <= 4 ? 1 : (sd_stride <= 16 ? 2 : 3));
    regime = mb | (tc << 2) | (sc << 4);
  }
}

static void update(int y) {
  int i;
  apm_update(&apm2, y);
  mx_update(&mx2, y);
  for (i = 0; i < NMIX1; ++i) mx_update(&mx1[i], y);
  if (mm_valid) { sm_update(&sm_match, y); sm_update(&sm_match2, y); }
  sm_update(&sm_o2, y);
  sm_update(&sm_o1, y);
  sm_update(&sm_o0, y);
  for (i = 0; i < NCM; ++i) {
    if (!cm.on[i]) continue;
    sm_update(&cm.sm[i], y);
    *cm.cp[i] = nex(*cm.cp[i], y);
    cm.idx[i] = cm.idx[i] * 2 + y;
    if (cm.idx[i] < 16) cm.cp[i] = cm.bp[i] + cm.idx[i];
  }

  c0 = c0 * 2 + y;
  ++bitpos;

  if (bitpos == 4) {
    U32 nh[NCM];
    for (i = 0; i < NCM; ++i) {
      if (!cm.on[i]) continue;
      nh[i] = hsh(((U64)cm.cx[i] << 8) | (U64)c0);
      __builtin_prefetch(cm.t + (size_t)(nh[i] & cm.gmask) * 64, 1, 3);
    }
    for (i = 0; i < NCM; ++i) {
      if (!cm.on[i]) continue;
      cm.bp[i] = cm_bucket(&cm, nh[i]);
      cm.idx[i] = 1;
      cm.cp[i] = cm.bp[i] + 1;
    }
  } else if (bitpos == 8) {
    byte_update(c0 & 255);
    c0 = 1; bitpos = 0;
    set_contexts();
  }
}

/* ------------------------------------------------------------------ */
/* driver                                                               */
/* ------------------------------------------------------------------ */

static void model_start(U64 size, int level) {
  models_init(level);
  buf = (U8*)malloc(size ? (size_t)size : 1);
  if (!buf) { fprintf(stderr, "prism: cannot allocate %.1f MiB history buffer\n", size / 1048576.0); exit(1); }
  pos = 0; c0 = 1; bitpos = 0;
  wordh = prevword = 0; ln_start = ln_prev = 0;
  tscore = dscore = 0; regime = 0; colidx = 0;
  mm_ptr = 0; mm_len = 0; mm_valid = 0;
  sd_stride = 0; sd_next = SD_WINDOW;
  set_contexts();
}

static void put64(FILE *f, U64 v) { int i; for (i = 0; i < 8; ++i) putc((int)((v >> (8 * i)) & 255), f); }
static U64  get64(FILE *f)        { int i; U64 v = 0; for (i = 0; i < 8; ++i) v |= (U64)(getc(f) & 255) << (8 * i); return v; }

static void progress(const char *what, U64 done, U64 total, clock_t t0) {
  double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
  fprintf(stderr, "\r%s %6.2f%%  %.2f MiB/s   ", what,
          total ? 100.0 * (double)done / (double)total : 100.0,
          sec > 0 ? (double)done / 1048576.0 / sec : 0.0);
  fflush(stderr);
}

int main(int argc, char **argv) {
  int level = 7, mode = 0, i, quiet = 0;
  const char *inname = NULL, *outname = NULL;
  FILE *in, *out;
  U64 size, n;
  Coder rc;
  clock_t t0;

  for (i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (!strcmp(a, "c") || !strcmp(a, "-c")) mode = 'c';
    else if (!strcmp(a, "d") || !strcmp(a, "-d")) mode = 'd';
    else if (a[0] == '-' && a[1] >= '0' && a[1] <= '9') {
      char *end; long v = strtol(a + 1, &end, 10);
      if (*end || v < 0 || v > 11) { fprintf(stderr, "prism: level must be 0..11\n"); return 1; }
      level = (int)v;
    }
    else if (!strcmp(a, "--no-stride")) features &= ~F_STRIDE;
    else if (!strcmp(a, "--no-regime")) features &= ~F_REGIME;
    else if (!strcmp(a, "--no-line"))   features &= ~F_LINE;
    else if (!strcmp(a, "--no-match"))  features &= ~F_MATCH;
    else if (!strcmp(a, "--baseline"))  features &= ~(F_STRIDE | F_REGIME);
    else if (!strcmp(a, "-q")) quiet = 1;
    else if (!strcmp(a, "-v")) sd_verbose = 1;
    else if (!inname) inname = a;
    else if (!outname) outname = a;
  }
  if (!mode || !inname || !outname) {
    fprintf(stderr,
      "PRISM " "1.0" " - context-mixing compressor with online structure detection\n"
      "usage: prism c|d [-0..-11] [--no-stride|--no-regime|--no-line|--no-match] in out\n"
      "  c        compress      d        decompress\n"
      "  -0..-11  hash table 1 MiB .. 2 GiB (default -7 = 128 MiB; -11 needs ~2.6 GiB)\n"
      "  --*      ablation switches, recorded in the header so d mirrors c\n");
    return 1;
  }

  in = fopen(inname, "rb");
  if (!in) { perror(inname); return 1; }
  out = fopen(outname, "wb");
  if (!out) { perror(outname); return 1; }

  t0 = clock();
  if (mode == 'c') {
    fseek(in, 0, SEEK_END);
    size = (U64)ftell(in);
    fseek(in, 0, SEEK_SET);
    /* The match model and the stride detector index history with 32-bit
     * positions, so 4 GiB is a hard limit rather than a tuning choice. */
    if (size >= 0xffffffffULL) {
      fprintf(stderr, "prism: input is %.1f GiB; this build handles inputs below 4 GiB\n",
              size / 1073741824.0);
      return 1;
    }
    put64(out, ((U64)PRISM_MAGIC) | ((U64)PRISM_FORMAT << 32) | ((U64)level << 40) | ((U64)features << 48));
    put64(out, size);
    model_start(size, level);
    rc_init(&rc, out, 0);
    for (n = 0; n < size; ++n) {
      int ch = getc(in), b;
      for (b = 7; b >= 0; --b) { int y = (ch >> b) & 1; rc_code(&rc, predict(), y); update(y); }
      if (!quiet && (n & 0xfffff) == 0) progress("compress", n, size, t0);
    }
    rc_flush(&rc);
    if (!quiet) { progress("compress", size, size, t0); fprintf(stderr, "\n%llu -> %llu bytes (%.4f bpb)\n",
      (unsigned long long)size, (unsigned long long)ftell(out), size ? ftell(out) * 8.0 / (double)size : 0.0); }
  } else {
    U64 hdr = get64(in);
    if ((U32)hdr != PRISM_MAGIC) { fprintf(stderr, "prism: not a PRISM stream\n"); return 1; }
    if (((hdr >> 32) & 255) != PRISM_FORMAT) { fprintf(stderr, "prism: unsupported format version\n"); return 1; }
    level = (int)((hdr >> 40) & 255);
    features = (U32)((hdr >> 48) & 255);
    size = get64(in);
    if (size >= 0xffffffffULL) { fprintf(stderr, "prism: corrupt header (implausible size)\n"); return 1; }
    model_start(size, level);
    rc_init(&rc, in, 1);
    for (n = 0; n < size; ++n) {
      int ch = 0, b;
      for (b = 0; b < 8; ++b) { int y = rc_code(&rc, predict(), 0); ch = ch * 2 + y; update(y); }
      putc(ch, out);
      if (!quiet && (n & 0xfffff) == 0) progress("decompress", n, size, t0);
    }
    if (!quiet) { progress("decompress", size, size, t0); fprintf(stderr, "\n"); }
  }
  fclose(in);
  fclose(out);
  return 0;
}
