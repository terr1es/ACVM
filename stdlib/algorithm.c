//===--- algo.c - Native algorithm implementations, compiled as C ---------===//
//
// This file exists to show what a native Axiom library looks like: plain C,
// no VM headers, no knowledge of bytecode. It sees a contiguous array and sorts
// it. That isolation is the point -- if these functions needed to know about
// pages or Value tags, marshalling would not be worth doing.
//
// The quicksort lives in sortImpl.h because the thread pool engine needs the same
// int32 sort for its per-chunk pass; it is instantiated per element type below.
//
// Three sort engines live in this file and one function picks between them --
// TimSort for input that is already mostly ordered, counting sort for input with
// few distinct values, LSD radix for everything else. `acvm_sort` (the
// `sortInts` primitive) and `timSortInts` (behind `sort`) share the choice; only
// the structured engine differs. See ACVM_DEFINE_ANALYZE for how it decides.

#include "algorithm.h"
#include "sortImpl.h"

#include <stdlib.h>   /* malloc / free: the merge scratch, released before return */
#include <string.h>   /* memcpy / memmove: the merge itself is element traffic */

ACVM_DEFINE_SORT(i32, int32_t)
ACVM_DEFINE_SORT(i64, int64_t)
ACVM_DEFINE_SORT(f64, double)

#define ACVM_DEFINE_REVERSE(SUFFIX, TYPE)                                       \
    static void acvm_reverse_##SUFFIX(TYPE* a, int64_t n) {                     \
        for (int64_t i = 0, j = n - 1; i < j; ++i, --j) {                        \
            ACVM_SWAP(TYPE, a[i], a[j]);                                         \
        }                                                                        \
    }

ACVM_DEFINE_REVERSE(i32, int32_t)
ACVM_DEFINE_REVERSE(i64, int64_t)
ACVM_DEFINE_REVERSE(f64, double)

//===--- choosing an engine: probe, radix, dispatch ------------------------===//
//
// Two entry points face the same choice, so both take it here rather than inside
// their own engine:
//
//   sort (timSortInts)        the stable sort `algorithm` exports
//   sortInts (acvm_sort)      the compiler primitive, what algorithm.ax falls
//                             back to and what `parallel` sorts chunks with
//
// The rule:
//
//   a long first natural run  -> the data has structure   -> TimSort / quicksort
//   a short first natural run -> the data is unstructured -> LSD radix
//
// The probe costs almost nothing in the case that matters. TimSort's very first
// step is this same scan, so when the data *is* structured the answer is one
// TimSort wanted anyway and the work is not repeated -- TimSort re-scans the
// leading run, which for structured data is a small fraction of the call. For
// unstructured data the scan returns after a handful of elements. Measured cost
// of a full n = 200000 pass: 0.045 ms, i.e. 0.3% of the sort it feeds.
//
// What it buys, at n = 200000 (bench/timsort_vs_qsort.ax has the harness):
//
//   random int32          15.5 ms  ->  0.99 ms   (15.6x)
//   random int64          17.1 ms  ->  3.09 ms   (5.5x)
//   already sorted         0.075 ms (unchanged: the probe says "structured")
//
// Radix is stable -- every pass is a stable bucket pass -- so this does not cost
// sort() its guarantee. It also has no adversarial input: exactly 4 passes for
// int32 and 8 for int64, whatever the data. That retires the quadratic worst
// case the median-of-three quicksort had, including the "median-of-three killer"
// sequences that were built to trigger it: those have short runs, so they take
// the radix path.

/* Below this many elements the fixed radix passes cost more than they save.
   Measured crossover on random int32 (build time only, not shipped):

       n       quicksort     radix    ratio
      32         0.12 us   0.37 us    0.32x
      64         0.31 us   0.47 us    0.66x
     128         0.85 us   0.80 us    1.06x   <- break-even
     256         1.82 us   1.35 us    1.35x
     512         4.30 us   1.91 us    2.26x
    1024        22.81 us   3.51 us    6.50x

   256 rather than 128 keeps a margin: the two are within noise at the crossover
   and radix pays a malloc that a busy allocator can make worse. */
#define ACVM_RADIX_MIN_N 256

/* Two questions, one sampling pass.
 *
 *   structured   is there enough existing order that TimSort's adaptivity beats
 *                a fixed-pass radix sort?
 *   dense        are there few enough distinct values that counting sort beats
 *                radix's unconditional four passes?
 *
 * Neither is answerable from the first run alone. Both of these start with a
 * long ascending run and want opposite engines:
 *
 *     sorted, then a few local swaps      TimSort 4 ms / radix 11 ms
 *     n/8 ascending prefix, random tail   TimSort 81 ms / radix 12 ms
 *
 * So sample: 64 windows of 64 elements spread evenly over the array. In that
 * sample, count the windows that are already monotone (either direction --
 * TimSort reverses a descending run in place) and track the min/max value. A
 * window of 64 random values has a descent almost surely, so random data scores
 * near zero while mostly-ordered data scores near 64. Cost is 4096 compares and
 * ~64 cache lines, independent of n.
 *
 * `dense` is read off the sampled range. This is a *necessary* condition, never
 * a false negative: a sample is a subset, so its range can only be smaller than
 * the real one. A small sampled range therefore never rejects dense data, and a
 * large one is a safe rejection -- the caller re-checks the exact range before
 * committing to counting sort, and the cost of that check is the scan counting
 * sort needs anyway.
 *
 * The structured cut is 3/4, not 1/2: at exactly half ordered the split is
 * ambiguous (an n/2 prefix followed by random tail), and radix is the smaller
 * mistake -- it costs ~11 ms flat, where guessing TimSort there costs 47 ms. */
#define ACVM_SAMPLE_WINDOWS 64
#define ACVM_SAMPLE_WINDOW 64
#define ACVM_STRUCTURED_NUM 3
#define ACVM_STRUCTURED_DEN 4

/* Elements read for the min/max estimate. Taken from the front, separately from
   the windowed monotonicity scan, so that scan can keep its early exit -- that
   exit is what makes the probe nearly free on the structured input it is
   supposed to recognise (two comparisons per window, not sixty-four). */
#define ACVM_RANGE_SAMPLE 256

/* Counting sort costs 4n + range against radix's 8n, so on paper it wins for any
   range below 4n. In practice the counter array is 8 bytes per bucket and has to
   be faulted in: at range = n that is 12n bytes of fresh allocation, which costs
   more than the arithmetic saves. Measured -- at range = n counting sort comes
   out ~20% *slower* than radix, at range = n/8 it is 2.5x faster.
   n/8 keeps the counter array at n bytes, i.e. the same size as the input. */
#define ACVM_DENSE_DIV 8

#define ACVM_DEFINE_ANALYZE(NAME, TYPE)                                                \
    static void NAME(const TYPE* a, int64_t n, int64_t* outLo, int64_t* outHi,         \
                     int* outStructured) {                                             \
        const int64_t windows = ACVM_SAMPLE_WINDOWS;                                   \
        const int64_t width = ACVM_SAMPLE_WINDOW;                                      \
        int64_t ordered = 0;                                                           \
        int64_t total = 0;                                                             \
        /* Range estimate. Deliberately cheap and deliberately allowed to         */ \
        /* under-report: the caller re-scans exactly before using the bounds, so  */ \
        /* a small sample only improves the odds of declining early, never the     */ \
        /* correctness of what follows.                                           */ \
        int64_t lo = a[0];                                                             \
        int64_t hi = a[0];                                                             \
        {                                                                              \
            const int64_t probe = n < ACVM_RANGE_SAMPLE ? n : ACVM_RANGE_SAMPLE;       \
            for (int64_t i = 1; i < probe; ++i) {                                      \
                const int64_t v = (int64_t)a[i];                                       \
                if (v < lo) {                                                          \
                    lo = v;                                                            \
                }                                                                      \
                if (v > hi) {                                                          \
                    hi = v;                                                            \
                }                                                                      \
            }                                                                          \
        }                                                                              \
        for (int64_t w = 0; w < windows; ++w) {                                        \
            /* Evenly spaced, last one flush against the end. For n below the span   */ \
            /* the windows overlap, which is what we want: every element is seen.   */ \
            int64_t base = (n - width) * w / (windows - 1);                            \
            if (base < 0) {                                                            \
                base = 0;                                                              \
            }                                                                          \
            if (base + width > n) {                                                    \
                break;                                                                 \
            }                                                                          \
            int up = 1;                                                                \
            int down = 1;                                                              \
            for (int64_t i = base + 1; i < base + width; ++i) {                        \
                if (a[i] < a[i - 1]) {                                                 \
                    up = 0;                                                            \
                }                                                                      \
                if (a[i] > a[i - 1]) {                                                 \
                    down = 0;                                                          \
                }                                                                      \
                if (!up && !down) {                                                    \
                    break;                                                             \
                }                                                                      \
            }                                                                          \
            if (up || down) {                                                          \
                ++ordered;                                                             \
            }                                                                          \
            ++total;                                                                   \
        }                                                                              \
        *outLo = lo;                                                                   \
        *outHi = hi;                                                                   \
        *outStructured =                                                               \
            total > 0 && ordered * ACVM_STRUCTURED_DEN >= total * ACVM_STRUCTURED_NUM;  \
    }

ACVM_DEFINE_ANALYZE(acvm_analyze_i32, int32_t)
ACVM_DEFINE_ANALYZE(acvm_analyze_i64, int64_t)

/* Counting sort -- the engine for data with few distinct values.
 *
 * Cost is one min/max scan, one counting pass, one prefix sum over `range`, one
 * scatter and one copy back, so ~4n + range against radix's 8n. With eight
 * distinct values that is the difference between 4.0 ms and well under 1 ms at
 * n = 200000.
 *
 * `sampledLo` / `sampledHi` are the bounds the sampling pass saw. They are used
 * only to *decline early*: a sample is a subset, so its range can only be
 * smaller than the real one, which means a large sampled range is proof that the
 * real range is large. The reverse does not hold -- the real data can hold
 * values outside the sampled range -- so the bounds are recomputed exactly below
 * before anything is indexed with them. Using the sampled bounds directly was a
 * real out-of-bounds write: `a[i] - lo` goes negative when the array contains a
 * value smaller than anything the sample happened to catch.
 *
 * Stable: the scatter walks the input in order and places equal keys at
 * increasing offsets, which is the prefix-sum variant of counting sort. Equal
 * keys therefore keep their relative order.
 *
 * Returns 1 when the array was sorted, 0 when it declined: the exact range is
 * too wide, or the block could not be allocated. The caller falls through to
 * radix in that case.
 *
 * One allocation holds the counters and the scatter buffer together -- two
 * mallocs would double the failure paths for no gain. */
#define ACVM_DEFINE_COUNTING(NAME, TYPE)                                               \
    static int NAME(TYPE* a, int64_t n, int64_t sampledLo, int64_t sampledHi) {         \
        if (sampledHi - sampledLo + 1 > n / ACVM_DENSE_DIV) {                          \
            return 0;   /* the real range is at least this wide */                      \
        }                                                                              \
        TYPE mn = a[0];                                                                \
        TYPE mx = a[0];                                                                \
        for (int64_t i = 1; i < n; ++i) {                                              \
            if (a[i] < mn) {                                                           \
                mn = a[i];                                                             \
            }                                                                          \
            if (a[i] > mx) {                                                           \
                mx = a[i];                                                             \
            }                                                                          \
        }                                                                              \
        const int64_t lo = (int64_t)mn;                                                 \
        const int64_t hi = (int64_t)mx;                                                 \
        const int64_t range = hi - lo + 1;                                             \
        if (range <= 0 || range > n / ACVM_DENSE_DIV) {                                \
            return 0;                                                                  \
        }                                                                              \
        void* const block = malloc((size_t)range * sizeof(int64_t) +                   \
                                   (size_t)n * sizeof(TYPE));                          \
        if (block == 0) {                                                              \
            return 0;                                                                  \
        }                                                                              \
        int64_t* const count = (int64_t*)block;                                        \
        TYPE* const scatter = (TYPE*)(count + range);                                  \
        for (int64_t v = 0; v < range; ++v) {                                          \
            count[v] = 0;                                                              \
        }                                                                              \
        for (int64_t i = 0; i < n; ++i) {                                              \
            ++count[(int64_t)a[i] - lo];                                                \
        }                                                                              \
        int64_t total = 0;                                                             \
        for (int64_t v = 0; v < range; ++v) {                                          \
            const int64_t c = count[v];                                                \
            count[v] = total;                                                          \
            total += c;                                                                \
        }                                                                              \
        for (int64_t i = 0; i < n; ++i) {                                              \
            scatter[count[(int64_t)a[i] - lo]++] = a[i];                                \
        }                                                                              \
        memcpy(a, scatter, (size_t)n * sizeof(TYPE));                                  \
        free(block);                                                                   \
        return 1;                                                                      \
    }

ACVM_DEFINE_COUNTING(acvm_count_i32, int32_t)
ACVM_DEFINE_COUNTING(acvm_count_i64, int64_t)

//===--- doubles: radix over the bit pattern -------------------------------===//
//
// A double has a total order that *almost* matches its IEEE-754 bit pattern, and
// the exceptions are why this is worth writing carefully:
//
//   * the sign bit is the top bit, so a plain unsigned compare sorts every
//     negative above every positive -- fix by folding: negatives get every bit
//     flipped, positives get only the sign bit flipped. The folded key is then
//     monotone across the whole range, -inf .. +inf;
//   * -0.0 and +0.0 have different bit patterns but compare equal, so the fold
//     puts -0.0 just before +0.0. A stable sort therefore keeps them in input
//     order, and the "sorted" predicate sees `-0.0 <= +0.0` as true either way.
//     No special case needed;
//   * NaN compares false against everything, which breaks comparison sorts
//     outright: a three-way partition puts NaN in the "equal to pivot" bucket,
//     the partition then spans values it did not inspect, and the result is
//     silently unsorted. That was a real bug -- `sortDouble` on a NaN-bearing
//     array returned garbage with no error. Under the folded bit pattern NaN has
//     a definite position (after every number, since its exponent is all ones and
//     positive NaNs sort above +inf), so the radix path is *correct* where the
//     quicksort was not.
//
// Payload: 11 bits per pass, 6 passes for 64 bits. Fewer, wider passes lose to
// cache pressure -- measured 8x8 bit at 1.00 and 16-bit at 0.72, the same effect
// as for int32. 11 is the widest that keeps the counter array in L1 (2048 x 8 B
// = 16 KB is already past it; 6 passes of 11 is the compromise that measured
// best of the widths tried).
//
// The scratch is one allocation of n doubles; on failure the caller keeps the
// quicksort, which is correct for NaN-free input.

#define ACVM_RADIX_BITS 11

/// Folds a double's bits into a key whose unsigned order is the double order.
static uint64_t acvm_double_key(double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return (bits & 0x8000000000000000ull) ? ~bits : (bits | 0x8000000000000000ull);
}

/// The inverse of `acvm_double_key`: turns a sorted key back into the double it
/// came from. Folding is its own inverse on each half, so the same test applies.
static double acvm_double_from_key(uint64_t key) {
    const uint64_t bits =
        (key & 0x8000000000000000ull) ? (key & ~0x8000000000000000ull) : ~key;
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

/// Sorts `n` doubles by radix over the folded bit pattern. Returns 1 on success,
/// 0 when the scratch could not be allocated.
static int acvm_radix_f64(double* a, int64_t n) {
    /* Fold once up front into a key array: a pass that re-reads doubles pays the
       fold on every element, six times over. Two n-sized uint64 buffers, one of
       them the scatter target, from a single allocation so there is one failure
       path instead of two. */
    uint64_t* const block = (uint64_t*)malloc((size_t)n * sizeof(uint64_t) * 2);
    if (block == 0) {
        return 0;
    }
    uint64_t* const keys = block;
    uint64_t* const scratch = block + n;

    for (int64_t i = 0; i < n; ++i) {
        keys[i] = acvm_double_key(a[i]);
    }

    const int64_t buckets = (int64_t)1 << ACVM_RADIX_BITS;
    int64_t count[1 << ACVM_RADIX_BITS];

    uint64_t* from = keys;
    uint64_t* to = scratch;
    for (int shift = 0; shift < 64; shift += ACVM_RADIX_BITS) {
        const int width = (shift + ACVM_RADIX_BITS <= 64) ? ACVM_RADIX_BITS : (64 - shift);
        const uint64_t passMask = (uint64_t)(((int64_t)1 << width) - 1);
        memset(count, 0, (size_t)buckets * sizeof(int64_t));
        for (int64_t i = 0; i < n; ++i) {
            ++count[(from[i] >> shift) & passMask];
        }
        int64_t sum = 0;
        for (int64_t d = 0; d < ((int64_t)1 << width); ++d) {
            const int64_t c = count[d];
            count[d] = sum;
            sum += c;
        }
        for (int64_t i = 0; i < n; ++i) {
            to[count[(from[i] >> shift) & passMask]++] = from[i];
        }
        uint64_t* swap = from;
        from = to;
        to = swap;
    }
    /* Six passes is even, so the keys are back in `keys`; the check covers a
       future width that would leave them in the scratch instead. */
    if (from != keys) {
        memcpy(keys, from, (size_t)n * sizeof(uint64_t));
    }
    for (int64_t i = 0; i < n; ++i) {
        a[i] = acvm_double_from_key(keys[i]);
    }
    free(block);
    return 1;
}

/// Declared in algorithm.axd as `sortDoubleNative`, exported for `sortDouble`.
/// Radix, falling back to the quicksort from sortImpl.h only when the scratch
/// cannot be allocated.
void sortDoubleNative(void* data, int64_t count) {
    double* a = (double*)data;
    const int64_t n = count;
    if (a == 0 || n < 2) {
        return;
    }
    if (!acvm_radix_f64(a, n)) {
        acvm_sort_f64(a, n);
    }
}

/* LSD radix, 8 bits per pass. `sign` flips the top bit so that a plain unsigned
   digit walk visits -2^31 .. 2^31-1 in order (two's complement is monotone once
   the sign bit is inverted).

   Returns 1 when the array was sorted, 0 when the scratch buffer could not be
   allocated -- the caller then keeps whatever engine it had. */
static int acvm_radix_i32(int32_t* a, int64_t n) {
    int32_t* const scratch = (int32_t*)malloc((size_t)n * sizeof(int32_t));
    if (scratch == 0) {
        return 0;
    }
    int32_t* from = a;
    int32_t* to = scratch;
    for (int shift = 0; shift < 32; shift += 8) {
        int64_t count[256];
        memset(count, 0, sizeof(count));
        for (int64_t i = 0; i < n; ++i) {
            ++count[(((uint32_t)a[i] ^ 0x80000000u) >> shift) & 0xFFu];
        }
        int64_t sum = 0;
        for (int d = 0; d < 256; ++d) {
            const int64_t c = count[d];
            count[d] = sum;
            sum += c;
        }
        for (int64_t i = 0; i < n; ++i) {
            to[count[(((uint32_t)from[i] ^ 0x80000000u) >> shift) & 0xFFu]++] = from[i];
        }
        int32_t* swap = from;
        from = to;
        to = swap;
    }
    /* Four passes is even, so the data is back in `a`; the check is for a future
       pass count that is not. */
    if (from != a) {
        memcpy(a, from, (size_t)n * sizeof(int32_t));
    }
    free(scratch);
    return 1;
}

static int acvm_radix_i64(int64_t* a, int64_t n) {
    int64_t* const scratch = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (scratch == 0) {
        return 0;
    }
    int64_t* from = a;
    int64_t* to = scratch;
    for (int shift = 0; shift < 64; shift += 8) {
        int64_t count[256];
        memset(count, 0, sizeof(count));
        for (int64_t i = 0; i < n; ++i) {
            ++count[(((uint64_t)a[i] ^ 0x8000000000000000ull) >> shift) & 0xFFu];
        }
        int64_t sum = 0;
        for (int d = 0; d < 256; ++d) {
            const int64_t c = count[d];
            count[d] = sum;
            sum += c;
        }
        for (int64_t i = 0; i < n; ++i) {
            to[count[(((uint64_t)from[i] ^ 0x8000000000000000ull) >> shift) & 0xFFu]++] =
                from[i];
        }
        int64_t* swap = from;
        from = to;
        to = swap;
    }
    if (from != a) {
        memcpy(a, from, (size_t)n * sizeof(int64_t));
    }
    free(scratch);
    return 1;
}

/* The engine `sortInts` / `sortFloats` use. Unstructured input takes radix,
   everything else the median-of-three quicksort. Deliberately *not* TimSort:
   TimSort's own out-of-memory path calls the quicksort, so routing the builtin
   here keeps the two independent and makes that fallback terminate. */
#define ACVM_DEFINE_SMART(NAME, TYPE, ANALYZE, COUNT, RADIX, FALLBACK)                 \
    static void NAME(TYPE* a, int64_t n) {                                             \
        if (n >= ACVM_RADIX_MIN_N) {                                                    \
            int64_t lo;                                                                 \
            int64_t hi;                                                                 \
            int structured;                                                             \
            ANALYZE(a, n, &lo, &hi, &structured);                                       \
            /* Only when there is no order to exploit: for structured input     */      \
            /* TimSort finishes in O(n), which beats counting sort's 4n. Dense      */      \
            /* data is almost always unstructured, so this loses nothing.          */      \
            if (!structured) {                                                          \
                if (COUNT(a, n, lo, hi) || RADIX(a, n)) {                               \
                    return;                                                             \
                }                                                                       \
            }                                                                           \
        }                                                                              \
        FALLBACK(a, n);                                                                \
    }

ACVM_DEFINE_SMART(acvm_smart_i32, int32_t, acvm_analyze_i32, acvm_count_i32, acvm_radix_i32,
                  acvm_sort_i32)
ACVM_DEFINE_SMART(acvm_smart_i64, int64_t, acvm_analyze_i64, acvm_count_i64, acvm_radix_i64,
                  acvm_sort_i64)

int acvm_sort(void* data, int64_t count, int kind) {
    if (data == 0 || count <= 1) {
        return 0;
    }
    switch (kind) {
    case ACVM_KIND_I32:
        acvm_smart_i32((int32_t*)data, count);
        return 0;
    case ACVM_KIND_I64:
        acvm_smart_i64((int64_t*)data, count);
        return 0;
    case ACVM_KIND_F64:
        sortDoubleNative((double*)data, count);
        return 0;
    default:
        return -1;
    }
}

int acvm_reverse(void* data, int64_t count, int kind) {
    if (data == 0 || count <= 1) {
        return 0;
    }
    switch (kind) {
    case ACVM_KIND_I32:
        acvm_reverse_i32((int32_t*)data, count);
        return 0;
    case ACVM_KIND_I64:
        acvm_reverse_i64((int64_t*)data, count);
        return 0;
    case ACVM_KIND_F64:
        acvm_reverse_f64((double*)data, count);
        return 0;
    default:
        return -1;
    }
}
//===----------------------------------------------------------------------===//
// The algorithm library proper
//===----------------------------------------------------------------------===//
//
// Everything above serves the compiler: `acvm_sort` and `acvm_reverse` back the
// `sortInts` / `reverseInts` primitives and are linked into acvm.exe. Everything
// below is the *library*: the same file is also built as algorithm.dll, and
// Axiom reaches it through stdlib/algorithm.axd.
//
// That split is why the names here have no `acvm_` prefix. The exported symbol is
// exactly the name Axiom code calls -- no mangling, no table, no version
// negotiation.
//
// ── why macros ──────────────────────────────────────────────────────────────
//
// Every algorithm here exists for two element widths: `int` (4 bytes) and
// `int64` (8 bytes). Axiom has no function types and the dynamic boundary cannot
// carry a callback, so a comparator-based generic version is impossible -- each
// width needs its own compiled loop. Writing those loops twice would mean two
// sources of truth for one algorithm and two places to get an off-by-one wrong,
// so each algorithm is defined once as a macro and instantiated per width.
//
// Naming: the 32 bit version keeps the bare name (`lowerBound`), the 64 bit one
// appends `64` (`lowerBound64`). Both return `int64_t`; the Axiom declaration
// decides whether the caller sees `int` or `int64`.
//
// ── conventions ────────────────────────────────────────────────────────────
//
//   * `count` is an element count, never a byte count;
//   * an index result of -1 means "no such element"; a count result is never
//     negative except where the comment says otherwise;
//   * nothing allocates: the caller owns every buffer;
//   * nothing traps or aborts: an error is a return code the caller can check.

// ─── per-width helpers ─────────────────────────────────────────────────────

/// Three-way partition of `a[0..count)`. On return `*less` is the first index not
/// below `pivot` and `*greater` the first index above it, so
/// `[0, less) < pivot == [less, greater] < (greater, count)`.
///
/// Dutch-flag rather than two-way: equal keys are common in real data, and a
/// two-way split degrades to quadratic when everything is equal.
#define ACVM_DEFINE_PARTITION(SUFFIX, TYPE)                                              \
    static void acvm_partition_three_##SUFFIX(TYPE* a, int64_t count, TYPE pivot,         \
                                              int64_t* less, int64_t* greater) {         \
        int64_t low = 0;                                                                 \
        int64_t index = 0;                                                               \
        int64_t high = count - 1;                                                        \
        while (index <= high) {                                                          \
            if (a[index] < pivot) {                                                      \
                ACVM_SWAP(TYPE, a[low], a[index]);                                       \
                ++low;                                                                   \
                ++index;                                                                 \
            } else if (a[index] > pivot) {                                               \
                ACVM_SWAP(TYPE, a[index], a[high]);                                      \
                --high;                                                                  \
            } else {                                                                     \
                ++index;                                                                 \
            }                                                                            \
        }                                                                                \
        *less = low;                                                                     \
        *greater = high + 1;                                                             \
    }

/// Sifts `a[index]` down towards the leaves of a max-heap of `count` elements.
///
/// Hole rather than swap: each step copies the larger child *up* into the hole
/// and moves the hole down, then the saved value lands once at the end. A swap
/// per level is three reads and three writes; this is one read and one write per
/// level plus a single final store. Measured 6% on heapSort at n = 200000 --
/// small, but it applies to heapSort, topK, makeHeap, pushHeap and popHeap all
/// at once, and costs nothing in clarity.
#define ACVM_DEFINE_SIFT_DOWN(SUFFIX, TYPE)                                              \
    static void acvm_sift_down_##SUFFIX(TYPE* a, int64_t index, int64_t count) {         \
        const TYPE value = a[index];                                                     \
        for (;;) {                                                                       \
            const int64_t left = 2 * index + 1;                                          \
            if (left >= count) {                                                         \
                break;                                                                   \
            }                                                                            \
            int64_t largest = left;                                                      \
            if (left + 1 < count && a[left + 1] > a[left]) {                             \
                largest = left + 1;                                                      \
            }                                                                            \
            if (a[largest] <= value) {                                                   \
                break;                                                                   \
            }                                                                            \
            a[index] = a[largest];                                                       \
            index = largest;                                                             \
        }                                                                                \
        a[index] = value;   /* the hole takes the value it started with */                \
    }

/// Reverses `a[from .. to]` inclusive.
#define ACVM_DEFINE_REVERSE_SPAN(SUFFIX, TYPE)                                           \
    static void acvm_reverse_span_##SUFFIX(TYPE* a, int64_t from, int64_t to) {          \
        while (from < to) {                                                              \
            ACVM_SWAP(TYPE, a[from], a[to]);                                             \
            ++from;                                                                      \
            --to;                                                                        \
        }                                                                                \
    }

ACVM_DEFINE_PARTITION(i32, int32_t)
ACVM_DEFINE_PARTITION(i64, int64_t)
ACVM_DEFINE_SIFT_DOWN(i32, int32_t)
ACVM_DEFINE_SIFT_DOWN(i64, int64_t)
ACVM_DEFINE_REVERSE_SPAN(i32, int32_t)
ACVM_DEFINE_REVERSE_SPAN(i64, int64_t)

// ─── search on sorted input ────────────────────────────────────────────────
//
// None of these checks that the input is sorted: the check would double the cost,
// and the caller is the one who sorted it.

#define ACVM_DEFINE_LOWER_BOUND(NAME, TYPE)                                              \
    int64_t NAME(void* data, int64_t count, int64_t value) {                             \
        const TYPE* a = (const TYPE*)data;                                               \
        const TYPE key = (TYPE)value;                                                    \
        int64_t low = 0;                                                                 \
        int64_t high = count;                                                            \
        while (low < high) {                                                             \
            const int64_t mid = low + (high - low) / 2;                                  \
            if (a[mid] < key) {                                                          \
                low = mid + 1;                                                           \
            } else {                                                                     \
                high = mid;                                                              \
            }                                                                            \
        }                                                                                \
        return low;                                                                      \
    }

#define ACVM_DEFINE_UPPER_BOUND(NAME, TYPE)                                              \
    int64_t NAME(void* data, int64_t count, int64_t value) {                             \
        const TYPE* a = (const TYPE*)data;                                               \
        const TYPE key = (TYPE)value;                                                    \
        int64_t low = 0;                                                                 \
        int64_t high = count;                                                            \
        while (low < high) {                                                             \
            const int64_t mid = low + (high - low) / 2;                                  \
            if (a[mid] <= key) {                                                         \
                low = mid + 1;                                                           \
            } else {                                                                     \
                high = mid;                                                              \
            }                                                                            \
        }                                                                                \
        return low;                                                                      \
    }

#define ACVM_DEFINE_BINARY_SEARCH(NAME, LOWER, TYPE)                                     \
    int64_t NAME(void* data, int64_t count, int64_t value) {                             \
        const int64_t index = LOWER(data, count, value);                                 \
        const TYPE* a = (const TYPE*)data;                                               \
        if (index < count && a[index] == (TYPE)value) {                                  \
            return index;                                                                \
        }                                                                                \
        return -1;                                                                       \
    }

ACVM_DEFINE_LOWER_BOUND(lowerBound, int32_t)
ACVM_DEFINE_LOWER_BOUND(lowerBound64, int64_t)
ACVM_DEFINE_UPPER_BOUND(upperBound, int32_t)
ACVM_DEFINE_UPPER_BOUND(upperBound64, int64_t)
ACVM_DEFINE_BINARY_SEARCH(binarySearch, lowerBound, int32_t)
ACVM_DEFINE_BINARY_SEARCH(binarySearch64, lowerBound64, int64_t)

// ─── timsort ───────────────────────────────────────────────────────────────
//
// The engine behind `sort` / `sortInt64`, declared in algorithm.axd as
// `timSortInts` / `timSortInts64`. TimSort is a stable, adaptive merge sort:
//
//   * natural runs -- ascending stretches, or strictly descending ones that get
//     reversed in place (strictly, so that equal keys keep their order);
//   * a run shorter than `minrun` is extended to it and finished with binary
//     insertion sort, because another merge level costs more than that;
//   * runs sit on a stack whose lengths stay balanced -- no run longer than the
//     sum of the two below it -- so merging can never degenerate to O(n^2);
//   * before a merge the overlap is trimmed from both borders by binary search,
//     so two runs that do not interleave cost two searches and one memcpy;
//   * the smaller side of each merge is copied to a scratch buffer, and the
//     merge walks *streaks*: while one side keeps winning, the inner loop is a
//     compare, a store and one bound test.
//
// The scratch buffer is `count/2 + 1` elements, allocated once per call and
// released before returning (see the malloc rule in algorithm.h: nothing may
// outlive the call). When that allocation fails the array is handed to the
// median-of-three quicksort instead: still O(n log n), but no longer stable --
// the only path where `sort` is not stable.
//
// Not implemented: CPython's in-merge `min_gallop` mode switch (exponential
// galloping *inside* a merge). Trimming the borders already removes the
// pathological cases; the mode switch only shaves constant factors on data
// where the winner of a merge alternates.

/* Runs deeper than this need more than 2^64 elements. The clamp in the driver
   is a guard against a bug writing past the stack arrays, not a real limit. */
#define ACVM_TS_MAX_RUNS 128

/* Scratch elements kept on the C stack, so that sorting a short array costs no
   allocation at all. 512 elements is 2 KB as int32 and 4 KB as int64, and covers
   any count up to 1022. */
#define ACVM_TS_LOCAL 512

/* `minrun`: n, or slightly more, so that n/minrun sits just under a power of
   two and the runs merge in balanced pairs. Does not touch elements, so it is
   written once instead of once per type. */
static int64_t ts_minrun(int64_t n) {
    int64_t r = 0;
    while (n >= 64) {
        r |= (n & 1);
        n >>= 1;
    }
    return n + r;
}

#define ACVM_DEFINE_TIMSORT(NAME, SUFFIX, TYPE, ANALYZE, COUNT, RADIX, FALLBACK)      \
    /* Length of the run at `lo`, reversed in place when it descends. */                 \
    static int64_t ts_run_##SUFFIX(TYPE* a, int64_t lo, int64_t hi) {                    \
        const int64_t remaining = hi - lo;                                               \
        if (remaining < 2) {                                                             \
            return remaining;                                                            \
        }                                                                                \
        int64_t run = 2;                                                                 \
        /* An ascending run may contain equals: they are already in order, so     */      \
        /* leaving them alone is what stability means. A *descending* run must be  */     \
        /* strict -- reversing a stretch with equal neighbours would swap them.    */     \
        if (!(a[lo + 1] < a[lo])) {                                                      \
            while (lo + run < hi && !(a[lo + run - 1] > a[lo + run])) {                  \
                ++run;                                                                    \
            }                                                                             \
            return run;                                                                   \
        }                                                                                \
        while (lo + run < hi && a[lo + run] < a[lo + run - 1]) {                         \
            ++run;                                                                        \
        }                                                                                \
        for (int64_t i = 0, j = run - 1; i < j; ++i, --j) {                               \
            const TYPE t = a[lo + i];                                                     \
            a[lo + i] = a[lo + j];                                                        \
            a[lo + j] = t;                                                                \
        }                                                                                \
        return run;                                                                      \
    }                                                                                    \
                                                                                         \
    /* Binary insertion sort over [lo, hi): log k compares per element and one   */       \
    /* memmove for the shift, instead of a compare-and-swap per position.        */       \
    static void ts_insert_##SUFFIX(TYPE* a, int64_t lo, int64_t hi) {                    \
        for (int64_t i = lo + 1; i < hi; ++i) {                                           \
            const TYPE key = a[i];                                                        \
            int64_t low = lo;                                                             \
            int64_t high = i;                                                             \
            while (low < high) {                                                          \
                const int64_t mid = low + ((high - low) >> 1);                            \
                if (a[mid] <= key) {                                                      \
                    low = mid + 1;                                                        \
                } else {                                                                  \
                    high = mid;                                                           \
                }                                                                         \
            }                                                                             \
            if (low != i) {                                                               \
                memmove(&a[low + 1], &a[low], (size_t)(i - low) * sizeof(TYPE));          \
            }                                                                             \
            a[low] = key;                                                                 \
        }                                                                                 \
    }                                                                                     \
                                                                                         \
    /* First offset in [0, len) whose element is >= key (`lower`) or > key         */      \
    /* (`upper`). Used only to trim a merge, where binary search is already        */      \
    /* logarithmic in the run and the runs are long.                              */      \
    static int64_t ts_lower_##SUFFIX(const TYPE* a, int64_t base, int64_t len, TYPE key) { \
        int64_t low = 0;                                                                  \
        int64_t high = len;                                                               \
        while (low < high) {                                                              \
            const int64_t mid = low + ((high - low) >> 1);                                \
            if (a[base + mid] < key) {                                                    \
                low = mid + 1;                                                            \
            } else {                                                                      \
                high = mid;                                                               \
            }                                                                             \
        }                                                                                 \
        return low;                                                                       \
    }                                                                                     \
    static int64_t ts_upper_##SUFFIX(const TYPE* a, int64_t base, int64_t len, TYPE key) { \
        int64_t low = 0;                                                                  \
        int64_t high = len;                                                               \
        while (low < high) {                                                              \
            const int64_t mid = low + ((high - low) >> 1);                                \
            if (a[base + mid] <= key) {                                                   \
                low = mid + 1;                                                            \
            } else {                                                                      \
                high = mid;                                                               \
            }                                                                             \
        }                                                                                 \
        return low;                                                                       \
    }                                                                                     \
                                                                                         \
    /* Merge two runs where the left one is the smaller side: copy it out and     */      \
    /* merge forward. A tie goes to the left run, which is what keeps the sort    */      \
    /* stable.                                                                     */      \
    static void ts_merge_lo_##SUFFIX(TYPE* restrict a, int64_t b1, int64_t n1,             \
                                     int64_t b2, int64_t n2, TYPE* restrict tmp) {          \
        memcpy(tmp, a + b1, (size_t)n1 * sizeof(TYPE));                                   \
        int64_t i = 0;                                                                    \
        int64_t j = b2;                                                                   \
        int64_t k = b1;                                                                   \
        const int64_t iEnd = n1;                                                          \
        const int64_t jEnd = b2 + n2;                                                     \
        while (i < iEnd && j < jEnd) {                                                    \
            while (i < iEnd && !(a[j] < tmp[i])) {                                        \
                a[k++] = tmp[i++];                                                        \
            }                                                                             \
            if (i >= iEnd) {                                                              \
                break;                                                                    \
            }                                                                             \
            while (j < jEnd && a[j] < tmp[i]) {                                           \
                a[k++] = a[j++];                                                          \
            }                                                                             \
        }                                                                                 \
        if (i < iEnd) {                                                                   \
            memcpy(a + k, tmp + i, (size_t)(iEnd - i) * sizeof(TYPE));                     \
        }                                                                                 \
    }                                                                                     \
                                                                                         \
    /* Mirror of the above for n1 > n2: copy the right run and merge backwards.   */      \
    /* A tie goes to the right run -- also for stability.                         */      \
    static void ts_merge_hi_##SUFFIX(TYPE* restrict a, int64_t b1, int64_t n1,             \
                                     int64_t b2, int64_t n2, TYPE* restrict tmp) {          \
        memcpy(tmp, a + b2, (size_t)n2 * sizeof(TYPE));                                   \
        int64_t i = b1 + n1 - 1;                                                          \
        int64_t j = n2 - 1;                                                               \
        int64_t k = b2 + n2 - 1;                                                          \
        while (i >= b1 && j >= 0) {                                                       \
            while (j >= 0 && !(tmp[j] < a[i])) {                                          \
                a[k--] = tmp[j--];                                                        \
            }                                                                             \
            if (j < 0) {                                                                  \
                break;                                                                    \
            }                                                                             \
            while (i >= b1 && tmp[j] < a[i]) {                                            \
                a[k--] = a[i--];                                                          \
            }                                                                             \
        }                                                                                 \
        if (j >= 0) {                                                                     \
            memcpy(a + k - j, tmp, (size_t)(j + 1) * sizeof(TYPE));                        \
        }                                                                                 \
    }                                                                                     \
                                                                                         \
    /* Merge runs `i` and `i+1`, after trimming the part of each that is already */       \
    /* in place, and shift the stack down over the pair.                         */       \
    static void ts_merge_at_##SUFFIX(TYPE* a, int64_t* base, int64_t* lens, int64_t i,     \
                                     int64_t runs, TYPE* tmp) {                            \
        const int64_t oBase = base[i];                                                     \
        const int64_t oN1 = lens[i];                                                       \
        const int64_t oN2 = lens[i + 1];                                                   \
        int64_t b1 = oBase;                                                                \
        int64_t n1 = oN1;                                                                  \
        int64_t b2 = base[i + 1];                                                          \
        int64_t n2 = oN2;                                                                  \
        /* Left elements <= the right head are already where they belong. */               \
        const int64_t cut = ts_upper_##SUFFIX(a, b1, n1, a[b2]);                           \
        b1 += cut;                                                                         \
        n1 -= cut;                                                                         \
        if (n1 > 0) {                                                                      \
            /* Right elements >= the last left element stay put as well. */                \
            n2 = ts_lower_##SUFFIX(a, b2, n2, a[b1 + n1 - 1]);                             \
        }                                                                                  \
        if (n1 > 0 && n2 > 0) {                                                            \
            if (n1 <= n2) {                                                                \
                ts_merge_lo_##SUFFIX(a, b1, n1, b2, n2, tmp);                              \
            } else {                                                                       \
                ts_merge_hi_##SUFFIX(a, b1, n1, b2, n2, tmp);                              \
            }                                                                              \
        }                                                                                  \
        base[i] = oBase;                                                                   \
        lens[i] = oN1 + oN2;                                                               \
        for (int64_t j = i + 1; j + 1 < runs; ++j) {                                        \
            base[j] = base[j + 1];                                                         \
            lens[j] = lens[j + 1];                                                         \
        }                                                                                  \
    }                                                                                      \
                                                                                           \
    void NAME(void* data, int64_t count) {                                                 \
        if (data == 0 || count < 2) {                                                      \
            return;                                                                        \
        }                                                                                  \
        TYPE* a = (TYPE*)data;                                                             \
        /* Unstructured input goes to radix before any scratch is allocated: radix  */      \
        /* is stable and needs no run stack, so nothing below is set up for nothing. */      \
        if (count >= ACVM_RADIX_MIN_N) {                                                    \
            int64_t lo;                                                                     \
            int64_t hi;                                                                     \
            int structured;                                                                 \
            ANALYZE(a, count, &lo, &hi, &structured);                                       \
            /* Same rule as acvm_smart_*: structured input keeps the log-n engine. */      \
            if (!structured) {                                                              \
                if (COUNT(a, count, lo, hi) || RADIX(a, count)) {                           \
                    return;                                                                 \
                }                                                                           \
            }                                                                               \
        }                                                                                   \
        /* The scratch never needs more than count/2 + 1 elements: a merge copies   */      \
        /* the smaller side, and the two runs of one merge sum to at most count.    */      \
        /* Small sorts -- the common case in a loop over records -- take it from the*/      \
        /* stack and skip malloc entirely.                                          */      \
        TYPE local[ACVM_TS_LOCAL];                                                          \
        const int fromLocal = (count / 2 + 1) <= ACVM_TS_LOCAL;                             \
        TYPE* tmp;                                                                          \
        if (fromLocal) {                                                                    \
            tmp = local;                                                                    \
        } else {                                                                            \
            tmp = (TYPE*)malloc((size_t)(count / 2 + 1) * sizeof(TYPE));                    \
            if (tmp == 0) {                                                                 \
                FALLBACK(data, count);   /* out of memory: degrade, never abort */           \
                return;                                                                     \
            }                                                                               \
        }                                                                                   \
        const int64_t minrun = ts_minrun(count);                                           \
        int64_t base[ACVM_TS_MAX_RUNS];                                                     \
        int64_t lens[ACVM_TS_MAX_RUNS];                                                     \
        int64_t runs = 0;                                                                   \
        int64_t lo = 0;                                                                     \
        while (lo < count) {                                                                \
            int64_t run = ts_run_##SUFFIX(a, lo, count);                                    \
            if (run < minrun) {                                                             \
                int64_t forced = count - lo;                                                \
                if (forced > minrun) {                                                      \
                    forced = minrun;                                                        \
                }                                                                           \
                ts_insert_##SUFFIX(a, lo, lo + forced);                                     \
                run = forced;                                                               \
            }                                                                               \
            base[runs] = lo;                                                                \
            lens[runs] = run;                                                               \
            ++runs;                                                                         \
            lo += run;                                                                      \
            /* Restore `lens[n-3] > lens[n-2] + lens[n-1]` and `lens[n-2] > lens[n-1]`.  */ \
            /* Merging the shorter pair first is what bounds the element traffic.      */   \
            while (runs > 1) {                                                              \
                int64_t n = runs;                                                           \
                if (n > 2 && lens[n - 3] <= lens[n - 2] + lens[n - 1]) {                    \
                    if (lens[n - 3] < lens[n - 1]) {                                        \
                        --n;                                                                \
                    }                                                                       \
                    ts_merge_at_##SUFFIX(a, base, lens, n - 2, runs, tmp);                   \
                    --runs;                                                                 \
                } else if (lens[n - 2] <= lens[n - 1]) {                                    \
                    ts_merge_at_##SUFFIX(a, base, lens, n - 2, runs, tmp);                   \
                    --runs;                                                                 \
                } else {                                                                    \
                    break;                                                                  \
                }                                                                           \
            }                                                                               \
            if (runs == ACVM_TS_MAX_RUNS) {                                                 \
                ts_merge_at_##SUFFIX(a, base, lens, runs - 2, runs, tmp);                    \
                --runs;                                                                     \
            }                                                                               \
        }                                                                                   \
        while (runs > 1) {                                                                  \
            int64_t n = runs;                                                               \
            if (n > 2 && lens[n - 3] < lens[n - 1]) {                                        \
                --n;                                                                        \
            }                                                                               \
            ts_merge_at_##SUFFIX(a, base, lens, n - 2, runs, tmp);                           \
            --runs;                                                                         \
        }                                                                                   \
        if (!fromLocal) {                                                                   \
            free(tmp);                                                                      \
        }                                                                                   \
    }

ACVM_DEFINE_TIMSORT(timSortInts, i32, int32_t, acvm_analyze_i32, acvm_count_i32,
                    acvm_radix_i32, acvm_sort_i32)
ACVM_DEFINE_TIMSORT(timSortInts64, i64, int64_t, acvm_analyze_i64, acvm_count_i64,
                    acvm_radix_i64, acvm_sort_i64)


// ─── selection ─────────────────────────────────────────────────────────────

#define ACVM_DEFINE_NTH_ELEMENT(NAME, SUFFIX, TYPE)                                      \
    int64_t NAME(void* data, int64_t count, int64_t k) {                                 \
        if (data == 0 || count <= 1 || k < 0 || k >= count) {                            \
            return 0;                                                                    \
        }                                                                                \
        TYPE* a = (TYPE*)data;                                                           \
        int64_t low = 0;                                                                 \
        int64_t high = count;                                                            \
        while (high - low > 1) {                                                         \
            const TYPE pivot = a[low + (high - low) / 2];                                \
            int64_t less = 0;                                                            \
            int64_t greater = 0;                                                         \
            acvm_partition_three_##SUFFIX(a + low, high - low, pivot, &less, &greater);  \
            less += low;                                                                 \
            greater += low;                                                              \
            if (k < less) {                                                              \
                high = less;                                                             \
            } else if (k >= greater) {                                                   \
                low = greater;                                                           \
            } else {                                                                     \
                return 0;   /* a[k] is the pivot: already in place */                    \
            }                                                                            \
        }                                                                                \
        return 0;                                                                        \
    }

// The even-length median averages the two central values. For `int64` that sum can
// overflow when both values are within a factor of two of INT64_MAX; Axiom's
// fallback has the same shape, so both agree -- documented in algorithm.h rather
// than paid for with a branch on every call.
#define ACVM_DEFINE_MEDIAN(NAME, NTH, TYPE)                                              \
    int64_t NAME(void* data, int64_t count) {                                            \
        if (data == 0 || count <= 0) {                                                   \
            return 0;                                                                    \
        }                                                                                \
        TYPE* a = (TYPE*)data;                                                           \
        const int64_t middle = count / 2;                                                \
        NTH(data, count, middle);                                                        \
        if (count % 2 == 1) {                                                            \
            return (int64_t)a[middle];                                                   \
        }                                                                                \
        /* The lower central value is the maximum of the left half, one scan away. */     \
        TYPE lower = a[0];                                                               \
        for (int64_t i = 1; i < middle; ++i) {                                           \
            if (a[i] > lower) {                                                          \
                lower = a[i];                                                            \
            }                                                                            \
        }                                                                                \
        return ((int64_t)lower + (int64_t)a[middle]) / 2;                                \
    }

ACVM_DEFINE_NTH_ELEMENT(nthElement, i32, int32_t)
ACVM_DEFINE_NTH_ELEMENT(nthElement64, i64, int64_t)
ACVM_DEFINE_MEDIAN(medianValue, nthElement, int32_t)
ACVM_DEFINE_MEDIAN(medianValue64, nthElement64, int64_t)

// ─── sequence operations ───────────────────────────────────────────────────

#define ACVM_DEFINE_ROTATE(NAME, SUFFIX, TYPE)                                           \
    int64_t NAME(void* data, int64_t count, int64_t by) {                                \
        if (data == 0 || count <= 1) {                                                   \
            return 0;                                                                    \
        }                                                                                \
        TYPE* a = (TYPE*)data;                                                           \
        int64_t shift = by % count;                                                      \
        if (shift < 0) {                                                                 \
            shift += count;   /* negative rotates the other way */                       \
        }                                                                                \
        if (shift == 0) {                                                                \
            return 0;                                                                    \
        }                                                                                \
        /* Three reversals, no scratch buffer. */                                        \
        acvm_reverse_span_##SUFFIX(a, 0, shift - 1);                                     \
        acvm_reverse_span_##SUFFIX(a, shift, count - 1);                                 \
        acvm_reverse_span_##SUFFIX(a, 0, count - 1);                                     \
        return 0;                                                                        \
    }

#define ACVM_DEFINE_PARTITION_INTS(NAME, SUFFIX, TYPE)                                   \
    int64_t NAME(void* data, int64_t count, int64_t pivot) {                             \
        if (data == 0 || count <= 0) {                                                   \
            return 0;                                                                    \
        }                                                                                \
        int64_t less = 0;                                                                \
        int64_t greater = 0;                                                             \
        acvm_partition_three_##SUFFIX((TYPE*)data, count, (TYPE)pivot, &less, &greater); \
        return less;                                                                     \
    }

#define ACVM_DEFINE_UNIQUE(NAME, TYPE)                                                   \
    int64_t NAME(void* data, int64_t count) {                                            \
        if (data == 0 || count <= 0) {                                                   \
            return 0;                                                                    \
        }                                                                                \
        if (count == 1) {                                                                \
            return 1;                                                                    \
        }                                                                                \
        TYPE* a = (TYPE*)data;                                                           \
        int64_t write = 1;                                                               \
        for (int64_t read = 1; read < count; ++read) {                                   \
            if (a[read] != a[write - 1]) {                                               \
                a[write] = a[read];                                                      \
                ++write;                                                                 \
            }                                                                            \
        }                                                                                \
        return write;                                                                    \
    }

#define ACVM_DEFINE_MERGE(NAME, TYPE)                                                    \
    int64_t NAME(void* destination, int64_t capacity, void* first, int64_t firstCount,   \
                 void* second, int64_t secondCount) {                                    \
        if (firstCount < 0 || secondCount < 0) {                                         \
            return -1;                                                                   \
        }                                                                                \
        const int64_t total = firstCount + secondCount;                                   \
        if (total > capacity) {                                                          \
            return -1;   /* the destination cannot hold the result */                    \
        }                                                                                \
        if (total == 0) {                                                                \
            return 0;                                                                    \
        }                                                                                \
        if (destination == 0 || first == 0 || second == 0) {                             \
            return -1;                                                                   \
        }                                                                                \
        const TYPE* a = (const TYPE*)first;                                              \
        const TYPE* b = (const TYPE*)second;                                             \
        TYPE* out = (TYPE*)destination;                                                  \
        int64_t i = 0;                                                                   \
        int64_t j = 0;                                                                   \
        int64_t k = 0;                                                                   \
        while (i < firstCount && j < secondCount) {                                      \
            /* `<=` keeps the merge stable: equal keys from the first run come first. */  \
            if (a[i] <= b[j]) {                                                          \
                out[k++] = a[i++];                                                       \
            } else {                                                                     \
                out[k++] = b[j++];                                                       \
            }                                                                            \
        }                                                                                \
        while (i < firstCount) {                                                         \
            out[k++] = a[i++];                                                           \
        }                                                                                \
        while (j < secondCount) {                                                        \
            out[k++] = b[j++];                                                           \
        }                                                                                \
        return total;                                                                    \
    }

#define ACVM_DEFINE_FILL(NAME, TYPE)                                                     \
    int64_t NAME(void* data, int64_t count, int64_t value) {                             \
        if (data == 0 || count <= 0) {                                                   \
            return 0;                                                                    \
        }                                                                                \
        TYPE* a = (TYPE*)data;                                                           \
        const TYPE filled = (TYPE)value;                                                 \
        for (int64_t i = 0; i < count; ++i) {                                            \
            a[i] = filled;                                                               \
        }                                                                                \
        return 0;                                                                        \
    }

ACVM_DEFINE_ROTATE(rotateInts, i32, int32_t)
ACVM_DEFINE_ROTATE(rotateInts64, i64, int64_t)
ACVM_DEFINE_PARTITION_INTS(partitionInts, i32, int32_t)
ACVM_DEFINE_PARTITION_INTS(partitionInts64, i64, int64_t)
ACVM_DEFINE_UNIQUE(uniqueSorted, int32_t)
ACVM_DEFINE_UNIQUE(uniqueSorted64, int64_t)
ACVM_DEFINE_MERGE(mergeInts, int32_t)
ACVM_DEFINE_MERGE(mergeInts64, int64_t)
ACVM_DEFINE_FILL(fillInts, int32_t)
ACVM_DEFINE_FILL(fillInts64, int64_t)

// ─── set operations on sorted runs ─────────────────────────────────────────
//
// Each writes into a caller buffer with two ascending inputs, and each is the
// merge inner loop with a different decision, so they share one macro. Six native
// arguments is the boundary's limit -- three (pointer, count) pairs exactly.

// Four independent decisions per operation, because the three set operations
// differ in more than one place: whether a shared key is kept, whether the
// smaller side is kept when the keys differ, and whether a leftover tail is
// drained. One `KEEP_A` flag cannot express both "intersection keeps a on a tie"
// and "intersection drops a when it is only on the left".
#define ACVM_DEFINE_SET_OP(NAME, ON_EQUAL, ON_LESS, ON_GREATER, TAIL_A, TAIL_B, TYPE)    \
    int64_t NAME(void* destination, int64_t capacity, void* first, int64_t firstCount,   \
                 void* second, int64_t secondCount) {                                    \
        if (firstCount < 0 || secondCount < 0) {                                         \
            return -1;                                                                   \
        }                                                                                \
        if (destination == 0) {                                                          \
            return -1;                                                                   \
        }                                                                                \
        const TYPE* a = (const TYPE*)first;                                              \
        const TYPE* b = (const TYPE*)second;                                             \
        TYPE* out = (TYPE*)destination;                                                  \
        int64_t i = 0;                                                                   \
        int64_t j = 0;                                                                   \
        int64_t k = 0;                                                                   \
        while (i < firstCount && j < secondCount) {                                      \
            if (a[i] == b[j]) {                                                          \
                if (ON_EQUAL) {                                                          \
                    if (k >= capacity) { return -1; }                                    \
                    out[k++] = a[i];                                                     \
                }                                                                        \
                ++i;                                                                     \
                ++j;                                                                     \
            } else if (a[i] < b[j]) {                                                    \
                if (ON_LESS) {                                                           \
                    if (k >= capacity) { return -1; }                                    \
                    out[k++] = a[i];                                                     \
                }                                                                        \
                ++i;                                                                     \
            } else {                                                                     \
                if (ON_GREATER) {                                                        \
                    if (k >= capacity) { return -1; }                                    \
                    out[k++] = b[j];                                                     \
                }                                                                        \
                ++j;                                                                     \
            }                                                                            \
        }                                                                                \
        if (TAIL_A) {                                                                    \
            while (i < firstCount) {                                                     \
                if (k >= capacity) { return -1; }                                        \
                out[k++] = a[i++];                                                       \
            }                                                                            \
        }                                                                                \
        if (TAIL_B) {                                                                    \
            while (j < secondCount) {                                                    \
                if (k >= capacity) { return -1; }                                        \
                out[k++] = b[j++];                                                       \
            }                                                                            \
        }                                                                                \
        return k;                                                                        \
    }

// Equal keys: kept once by intersection and union, dropped by difference.
// Differing keys: intersection keeps neither, union keeps the smaller one,
// difference keeps the left one only.
ACVM_DEFINE_SET_OP(intersectSorted, 1, 0, 0, 0, 0, int32_t)
ACVM_DEFINE_SET_OP(intersectSorted64, 1, 0, 0, 0, 0, int64_t)
ACVM_DEFINE_SET_OP(unionSorted, 1, 1, 1, 1, 1, int32_t)
ACVM_DEFINE_SET_OP(unionSorted64, 1, 1, 1, 1, 1, int64_t)
ACVM_DEFINE_SET_OP(differenceSorted, 0, 1, 0, 1, 0, int32_t)
ACVM_DEFINE_SET_OP(differenceSorted64, 0, 1, 0, 1, 0, int64_t)

// ─── queries ───────────────────────────────────────────────────────────────
//
// Where the C side earns its keep most clearly: one native compare per element,
// against a bounds check, an index scale and a page lookup per element in Axiom.

#define ACVM_DEFINE_FIND(NAME, WANT_LAST, TYPE)                                          \
    int64_t NAME(void* data, int64_t count, int64_t value) {                             \
        if (data == 0) {                                                                 \
            return -1;                                                                   \
        }                                                                                \
        const TYPE* a = (const TYPE*)data;                                               \
        const TYPE key = (TYPE)value;                                                    \
        if (WANT_LAST) {                                                                 \
            for (int64_t i = count; i-- > 0;) {                                          \
                if (a[i] == key) {                                                       \
                    return i;                                                            \
                }                                                                        \
            }                                                                            \
            return -1;                                                                   \
        }                                                                                \
        for (int64_t i = 0; i < count; ++i) {                                            \
            if (a[i] == key) {                                                           \
                return i;                                                                \
            }                                                                            \
        }                                                                                \
        return -1;                                                                       \
    }

#define ACVM_DEFINE_COUNT(NAME, TYPE)                                                    \
    int64_t NAME(void* data, int64_t count, int64_t value) {                             \
        if (data == 0 || count <= 0) {                                                   \
            return 0;                                                                    \
        }                                                                                \
        const TYPE* a = (const TYPE*)data;                                               \
        const TYPE key = (TYPE)value;                                                    \
        int64_t found = 0;                                                               \
        for (int64_t i = 0; i < count; ++i) {                                            \
            found += a[i] == key;                                                        \
        }                                                                                \
        return found;                                                                    \
    }

#define ACVM_DEFINE_EXTREME_INDEX(NAME, WANT_MAX, TYPE)                                  \
    int64_t NAME(void* data, int64_t count) {                                            \
        if (data == 0 || count <= 0) {                                                   \
            return -1;                                                                   \
        }                                                                                \
        const TYPE* a = (const TYPE*)data;                                               \
        int64_t best = 0;                                                                \
        for (int64_t i = 1; i < count; ++i) {                                            \
            if (WANT_MAX ? (a[i] > a[best]) : (a[i] < a[best])) {                        \
                best = i;                                                                \
            }                                                                            \
        }                                                                                \
        return best;                                                                     \
    }

#define ACVM_DEFINE_IS_SORTED(NAME, WANT_DESC, TYPE)                                     \
    int64_t NAME(void* data, int64_t count) {                                            \
        if (data == 0 || count <= 1) {                                                   \
            return 1;                                                                    \
        }                                                                                \
        const TYPE* a = (const TYPE*)data;                                               \
        for (int64_t i = 1; i < count; ++i) {                                            \
            if (WANT_DESC ? (a[i - 1] < a[i]) : (a[i - 1] > a[i])) {                     \
                return 0;                                                                \
            }                                                                            \
        }                                                                                \
        return 1;                                                                        \
    }

#define ACVM_DEFINE_EQUAL(NAME, TYPE)                                                    \
    int64_t NAME(void* first, int64_t firstCount, void* second, int64_t secondCount) {   \
        if (firstCount != secondCount) {                                                 \
            return 0;                                                                    \
        }                                                                                \
        if (firstCount <= 0) {                                                           \
            return 1;                                                                    \
        }                                                                                \
        if (first == 0 || second == 0) {                                                 \
            return 0;                                                                    \
        }                                                                                \
        const TYPE* a = (const TYPE*)first;                                              \
        const TYPE* b = (const TYPE*)second;                                             \
        for (int64_t i = 0; i < firstCount; ++i) {                                       \
            if (a[i] != b[i]) {                                                          \
                return 0;                                                                \
            }                                                                            \
        }                                                                                \
        return 1;                                                                        \
    }

ACVM_DEFINE_FIND(findInt, 0, int32_t)
ACVM_DEFINE_FIND(findInt64, 0, int64_t)
ACVM_DEFINE_FIND(findLastInt, 1, int32_t)
ACVM_DEFINE_FIND(findLastInt64, 1, int64_t)
ACVM_DEFINE_COUNT(countInt, int32_t)
ACVM_DEFINE_COUNT(countInt64, int64_t)
ACVM_DEFINE_EXTREME_INDEX(minIndex, 0, int32_t)
ACVM_DEFINE_EXTREME_INDEX(minIndex64, 0, int64_t)
ACVM_DEFINE_EXTREME_INDEX(maxIndex, 1, int32_t)
ACVM_DEFINE_EXTREME_INDEX(maxIndex64, 1, int64_t)
ACVM_DEFINE_IS_SORTED(isSortedAsc, 0, int32_t)
ACVM_DEFINE_IS_SORTED(isSortedAsc64, 0, int64_t)
ACVM_DEFINE_IS_SORTED(isSortedDesc, 1, int32_t)
ACVM_DEFINE_IS_SORTED(isSortedDesc64, 1, int64_t)
ACVM_DEFINE_EQUAL(equalInts, int32_t)
ACVM_DEFINE_EQUAL(equalInts64, int64_t)

/// Writes four values into `out`: minimum, maximum, index of the minimum, index
/// of the maximum. One pass instead of two, and the indices come for free -- a
/// caller that only wants the values still gets them without a second scan.
#define ACVM_DEFINE_MIN_MAX(NAME, TYPE)                                                  \
    int64_t NAME(void* out, int64_t outCapacity, void* data, int64_t count) {            \
        if (out == 0 || outCapacity < 4) {                                               \
            return -1;   /* needs room for four values */                                \
        }                                                                                \
        if (data == 0 || count <= 0) {                                                   \
            return 0;   /* nothing written: the caller checks the return code */          \
        }                                                                                \
        const TYPE* a = (const TYPE*)data;                                               \
        int64_t minAt = 0;                                                               \
        int64_t maxAt = 0;                                                               \
        for (int64_t i = 1; i < count; ++i) {                                            \
            if (a[i] < a[minAt]) {                                                       \
                minAt = i;                                                               \
            }                                                                            \
            if (a[i] > a[maxAt]) {                                                       \
                maxAt = i;                                                               \
            }                                                                            \
        }                                                                                \
        TYPE* slots = (TYPE*)out;                                                        \
        slots[0] = a[minAt];                                                             \
        slots[1] = a[maxAt];                                                             \
        slots[2] = (TYPE)minAt;                                                          \
        slots[3] = (TYPE)maxAt;                                                          \
        return 1;                                                                        \
    }

ACVM_DEFINE_MIN_MAX(minMax, int32_t)
ACVM_DEFINE_MIN_MAX(minMax64, int64_t)

/// Compacts out every element equal to `value`, returning the new length. The
/// array keeps its size; only the first `result` elements are meaningful.
#define ACVM_DEFINE_REMOVE_VALUE(NAME, TYPE)                                             \
    int64_t NAME(void* data, int64_t count, int64_t value) {                             \
        if (data == 0 || count <= 0) {                                                   \
            return 0;                                                                    \
        }                                                                                \
        TYPE* a = (TYPE*)data;                                                           \
        const TYPE target = (TYPE)value;                                                 \
        int64_t write = 0;                                                               \
        for (int64_t read = 0; read < count; ++read) {                                   \
            if (a[read] != target) {                                                     \
                a[write++] = a[read];                                                    \
            }                                                                            \
        }                                                                                \
        return write;                                                                    \
    }

ACVM_DEFINE_REMOVE_VALUE(removeValue, int32_t)
ACVM_DEFINE_REMOVE_VALUE(removeValue64, int64_t)

/// Value at percentile `p` (0..100) of a sorted array, by the nearest-rank rule:
/// index `p * (count - 1) / 100`, truncated.
#define ACVM_DEFINE_PERCENTILE(NAME, TYPE)                                               \
    int64_t NAME(void* data, int64_t count, int64_t p) {                                 \
        if (data == 0 || count <= 0) {                                                   \
            return 0;                                                                    \
        }                                                                                \
        if (p < 0) {                                                                     \
            p = 0;                                                                       \
        }                                                                                \
        if (p > 100) {                                                                   \
            p = 100;                                                                     \
        }                                                                                \
        const int64_t index = p * (count - 1) / 100;                                     \
        return (int64_t)((const TYPE*)data)[index];                                      \
    }

ACVM_DEFINE_PERCENTILE(percentileSorted, int32_t)
ACVM_DEFINE_PERCENTILE(percentileSorted64, int64_t)

// ─── binary heap ───────────────────────────────────────────────────────────
//
// Max-heap in array layout: the children of `i` are `2i+1` and `2i+2`, so `data[0]`
// is the maximum.

#define ACVM_DEFINE_MAKE_HEAP(NAME, SUFFIX, TYPE)                                        \
    int64_t NAME(void* data, int64_t count) {                                            \
        if (data == 0 || count <= 1) {                                                   \
            return 0;                                                                    \
        }                                                                                \
        TYPE* a = (TYPE*)data;                                                           \
        /* Sift down from the last internal node; leaves are already heaps. */            \
        for (int64_t i = count / 2 - 1; i >= 0; --i) {                                   \
            acvm_sift_down_##SUFFIX(a, i, count);                                        \
        }                                                                                \
        return 0;                                                                        \
    }

#define ACVM_DEFINE_PUSH_HEAP(NAME, TYPE)                                                \
    int64_t NAME(void* data, int64_t count) {                                            \
        if (data == 0 || count <= 1) {                                                   \
            return 0;                                                                    \
        }                                                                                \
        TYPE* a = (TYPE*)data;                                                           \
        int64_t index = count - 1;                                                       \
        while (index > 0) {                                                              \
            const int64_t parent = (index - 1) / 2;                                      \
            if (a[parent] >= a[index]) {                                                 \
                break;                                                                   \
            }                                                                            \
            ACVM_SWAP(TYPE, a[parent], a[index]);                                        \
            index = parent;                                                              \
        }                                                                                \
        return 0;                                                                        \
    }

#define ACVM_DEFINE_POP_HEAP(NAME, SUFFIX, TYPE)                                         \
    int64_t NAME(void* data, int64_t count) {                                            \
        if (data == 0 || count <= 1) {                                                   \
            return 0;                                                                    \
        }                                                                                \
        TYPE* a = (TYPE*)data;                                                           \
        ACVM_SWAP(TYPE, a[0], a[count - 1]);                                             \
        acvm_sift_down_##SUFFIX(a, 0, count - 1);                                        \
        return 0;                                                                        \
    }

#define ACVM_DEFINE_IS_HEAP(NAME, TYPE)                                                  \
    int64_t NAME(void* data, int64_t count) {                                            \
        if (data == 0 || count <= 1) {                                                   \
            return 1;                                                                    \
        }                                                                                \
        const TYPE* a = (const TYPE*)data;                                               \
        for (int64_t i = 1; i < count; ++i) {                                            \
            if (a[(i - 1) / 2] < a[i]) {                                                 \
                return 0;                                                                \
            }                                                                            \
        }                                                                                \
        return 1;                                                                        \
    }

ACVM_DEFINE_MAKE_HEAP(makeHeap, i32, int32_t)
ACVM_DEFINE_MAKE_HEAP(makeHeap64, i64, int64_t)
ACVM_DEFINE_PUSH_HEAP(pushHeap, int32_t)
ACVM_DEFINE_PUSH_HEAP(pushHeap64, int64_t)
ACVM_DEFINE_POP_HEAP(popHeap, i32, int32_t)
ACVM_DEFINE_POP_HEAP(popHeap64, i64, int64_t)
ACVM_DEFINE_IS_HEAP(isHeap, int32_t)
ACVM_DEFINE_IS_HEAP(isHeap64, int64_t)

// ─── sort family on top of the heap ────────────────────────────────────────

/// In-place ascending sort with a hard O(n log n) bound. `acvm_sort_i32` (a
/// median-of-three quicksort) is faster on average but quadratic on adversarial
/// input; this one is what to reach for when the data is not trusted.
#define ACVM_DEFINE_HEAP_SORT(NAME, MAKE, POP, TYPE)                                     \
    int64_t NAME(void* data, int64_t count) {                                            \
        if (data == 0 || count <= 1) {                                                   \
            return 0;                                                                    \
        }                                                                                \
        MAKE(data, count);                                                               \
        for (int64_t remaining = count; remaining > 1; --remaining) {                     \
            POP(data, remaining);                                                        \
        }                                                                                \
        return 0;                                                                        \
    }

ACVM_DEFINE_HEAP_SORT(heapSort, makeHeap, popHeap, int32_t)
ACVM_DEFINE_HEAP_SORT(heapSort64, makeHeap64, popHeap64, int64_t)

/// Moves the `k` largest elements to `data[count-k .. count-1]`, ascending.
/// Everything before that range is whatever the heap left there. Cheaper than a
/// full sort when `k` is small: O(n + k log n).
#define ACVM_DEFINE_TOP_K(NAME, MAKE, POP)                                               \
    int64_t NAME(void* data, int64_t count, int64_t k) {                                 \
        if (data == 0 || count <= 1 || k <= 0) {                                         \
            return 0;                                                                    \
        }                                                                                \
        if (k > count) {                                                                 \
            k = count;                                                                   \
        }                                                                                \
        MAKE(data, count);                                                               \
        for (int64_t done = 0; done < k; ++done) {                                       \
            POP(data, count - done);                                                     \
        }                                                                                \
        return k;                                                                        \
    }

ACVM_DEFINE_TOP_K(topK, makeHeap, popHeap)
ACVM_DEFINE_TOP_K(topK64, makeHeap64, popHeap64)

// ─── partial sort ──────────────────────────────────────────────────────────

/// Sorts the first `k` elements ascending, leaving the rest unspecified but all of
/// them >= `data[k-1]`. Selection first, then a sort of just those `k`.
#define ACVM_DEFINE_PARTIAL_SORT(NAME, NTH, SORT)                                        \
    int64_t NAME(void* data, int64_t count, int64_t k) {                                 \
        if (data == 0 || count <= 1 || k <= 0) {                                         \
            return 0;                                                                    \
        }                                                                                \
        if (k >= count) {                                                                \
            SORT(data, count);                                                           \
            return count;                                                                \
        }                                                                                \
        NTH(data, count, k);                                                             \
        SORT(data, k);                                                                   \
        return k;                                                                        \
    }

// The static quicksorts from the primitive half, not the exported wrappers: taking
// the address of a function is not something Axiom can express, so the macro takes
// the C symbol directly.
ACVM_DEFINE_PARTIAL_SORT(partialSort, nthElement, acvm_sort_i32)
ACVM_DEFINE_PARTIAL_SORT(partialSort64, nthElement64, acvm_sort_i64)

// ─── statistics ────────────────────────────────────────────────────────────

/// Writes `out[0] = mean` and `out[1] = population variance` as doubles, over the
/// values of an `int` array. Needs room for two doubles.
///
/// A double result cannot cross the boundary as a return value -- it would come
/// back in an SSE register the 64 bit call site cannot read -- so it comes out
/// through a `ptr double` buffer instead. Two passes rather than Welford: the
/// values are integers, so the sums are exact in double until 2^53.
int64_t variance(void* out, int64_t outCapacity, void* data, int64_t count) {
    if (out == 0 || outCapacity < 2) {
        return -1;
    }
    if (data == 0 || count <= 0) {
        return 0;
    }
    const int32_t* a = (const int32_t*)data;
    double sum = 0.0;
    for (int64_t i = 0; i < count; ++i) {
        sum += (double)a[i];
    }
    const double mean = sum / (double)count;
    double squares = 0.0;
    for (int64_t i = 0; i < count; ++i) {
        const double delta = (double)a[i] - mean;
        squares += delta * delta;
    }
    double* slots = (double*)out;
    slots[0] = mean;
    slots[1] = squares / (double)count;
    return 1;
}
