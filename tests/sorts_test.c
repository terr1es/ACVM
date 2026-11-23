/* Correctness of the algorithm library's natives: every engine, every dispatch
 * path, plus stability.
 *
 * Four things are checked per case:
 *   1. the result is non-decreasing,
 *   2. it is a permutation of the input (xor is order independent, so it cannot
 *      change under a correct sort),
 *   3. it matches qsort element for element,
 *   4. stable sorts keep equal elements in their original order (checked
 *      separately, with the original index packed into the low bits).
 *
 * The shapes are chosen to reach every branch of the dispatch in algorithm.c:
 *
 *   few-values / low-cardinality  -> counting sort
 *   random / zigzag / organ-pipe  -> radix
 *   ascending / descending / nearly-sorted / blocks -> TimSort (or quicksort
 *                                                     for the primitive)
 *
 * Build and run (the DLL is optional; without it the Axiom bodies run):
 *
 *   clang -O2 -I stdlib -o sorts_test tests/sorts_test.c stdlib/algorithm.c -lm
 *   ./sorts_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "algorithm.h"

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint32_t next_rand(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 32);
}

static int failures = 0;
static int cases = 0;

static void fail(const char* what, const char* shape, int64_t n, int64_t at) {
    printf("  FAIL %-28s shape=%-16s n=%lld at=%lld\n", what, shape, (long long)n,
           (long long)at);
    ++failures;
}

static int cmp_i32(const void* a, const void* b) {
    const int32_t x = *(const int32_t*)a, y = *(const int32_t*)b;
    return (x > y) - (x < y);
}
static int cmp_i64(const void* a, const void* b) {
    const int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

enum {
    SH_RANDOM = 0, SH_ASC, SH_DESC, SH_EQUAL, SH_FEW, SH_ZIGZAG, SH_ORGAN,
    SH_M3KILLER, SH_NEARLY, SH_BLOCKS, SH_HALFSORTED, SH_REVERSED, SH_TWO_VALUES,
    SH_WIDE_DUPES, SH_COUNT
};

static const char* shape_name(int s) {
    static const char* names[] = {"random",       "ascending",   "descending",
                                  "all-equal",    "eight-values", "zigzag",
                                  "organ-pipe",   "m3-killer",   "nearly-sorted",
                                  "blocks",       "half-sorted", "reversed",
                                  "two-values",   "wide-dupes"};
    return (s >= 0 && s < SH_COUNT) ? names[s] : "?";
}

static void m3_killer(int32_t* a, int64_t n) {
    for (int64_t i = 0; i < n; ++i) a[i] = (int32_t)i;
    for (int64_t k = 1; k < n; k = k * 2 + 1) {
        const int64_t mid = (k + n) / 2;
        if (mid < n) {
            const int32_t t = a[k];
            a[k] = a[mid];
            a[mid] = t;
        }
    }
}

static void fill_i32(int32_t* a, int64_t n, int shape) {
    switch (shape) {
    case SH_RANDOM:
        for (int64_t i = 0; i < n; ++i) a[i] = (int32_t)next_rand();
        break;
    case SH_ASC:
        for (int64_t i = 0; i < n; ++i) a[i] = (int32_t)i;
        break;
    case SH_DESC:
        for (int64_t i = 0; i < n; ++i) a[i] = (int32_t)(n - i);
        break;
    case SH_EQUAL:
        for (int64_t i = 0; i < n; ++i) a[i] = 7;
        break;
    case SH_FEW:
        for (int64_t i = 0; i < n; ++i) a[i] = (int32_t)(next_rand() % 8);
        break;
    case SH_ZIGZAG:
        for (int64_t i = 0; i < n; ++i) a[i] = (int32_t)(i ^ (i >> 1));
        break;
    case SH_ORGAN:
        for (int64_t i = 0; i < n; ++i)
            a[i] = (int32_t)(i < n / 2 ? i : n - i);
        break;
    case SH_M3KILLER:
        m3_killer(a, n);
        break;
    case SH_NEARLY:
        for (int64_t i = 0; i < n; ++i) a[i] = (int32_t)i;
        for (int64_t k = 0; k < n / 64 + 1 && n > 1; ++k) {
            const int64_t i = (int64_t)(next_rand() % (uint32_t)n);
            const int64_t j = (int64_t)(next_rand() % (uint32_t)n);
            const int32_t t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
        break;
    case SH_BLOCKS:
        for (int64_t i = 0; i < n; ++i) a[i] = (int32_t)((i / 4) * 1000 + (i % 4));
        break;
    case SH_HALFSORTED:
        for (int64_t i = 0; i < n; ++i)
            a[i] = (int32_t)(i < n / 2 ? i : next_rand() % 1000000);
        break;
    case SH_REVERSED:
        for (int64_t i = 0; i < n; ++i)
            a[i] = (int32_t)(i < n / 2 ? (n / 2 - i) : next_rand() % 100);
        break;
    case SH_TWO_VALUES:
        for (int64_t i = 0; i < n; ++i) a[i] = (next_rand() & 1u) ? 1 : 0;
        break;
    case SH_WIDE_DUPES:
        /* 100000 distinct values, each repeated ~n/100000 times: dense enough
           that the range is small relative to n, wide enough to need real
           counters rather than a handful of buckets */
        for (int64_t i = 0; i < n; ++i) a[i] = (int32_t)(next_rand() % 100000);
        break;
    default:
        for (int64_t i = 0; i < n; ++i) a[i] = 0;
        break;
    }
}

static void fill_i64(int64_t* a, int64_t n, int shape) {
    switch (shape) {
    case SH_RANDOM:
        for (int64_t i = 0; i < n; ++i)
            a[i] = (int64_t)next_rand() * 4294967296ll + (int64_t)next_rand();
        break;
    case SH_ASC:
        for (int64_t i = 0; i < n; ++i) a[i] = i;
        break;
    case SH_DESC:
        for (int64_t i = 0; i < n; ++i) a[i] = n - i;
        break;
    case SH_EQUAL:
        for (int64_t i = 0; i < n; ++i) a[i] = -42;
        break;
    case SH_FEW:
        for (int64_t i = 0; i < n; ++i) a[i] = (int64_t)(next_rand() % 8);
        break;
    case SH_ZIGZAG:
        for (int64_t i = 0; i < n; ++i) a[i] = (i ^ (i >> 1)) * 1099511627776ll;
        break;
    case SH_ORGAN:
        for (int64_t i = 0; i < n; ++i) a[i] = (i < n / 2) ? i : n - i;
        break;
    case SH_M3KILLER:
        for (int64_t i = 0; i < n; ++i) a[i] = i;
        break;
    case SH_NEARLY:
        for (int64_t i = 0; i < n; ++i) a[i] = i;
        for (int64_t k = 0; k < n / 64 + 1 && n > 1; ++k) {
            const int64_t i = (int64_t)(next_rand() % (uint32_t)n);
            const int64_t j = (int64_t)(next_rand() % (uint32_t)n);
            const int64_t t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
        break;
    case SH_BLOCKS:
        for (int64_t i = 0; i < n; ++i) a[i] = (i / 4) * 1000 + (i % 4);
        break;
    case SH_HALFSORTED:
        for (int64_t i = 0; i < n; ++i)
            a[i] = (i < n / 2) ? i : (int64_t)(next_rand() % 1000000);
        break;
    case SH_REVERSED:
        for (int64_t i = 0; i < n; ++i)
            a[i] = (i < n / 2) ? (n / 2 - i) : (int64_t)(next_rand() % 100);
        break;
    case SH_TWO_VALUES:
        for (int64_t i = 0; i < n; ++i) a[i] = (next_rand() & 1u) ? 1 : 0;
        break;
    case SH_WIDE_DUPES:
        for (int64_t i = 0; i < n; ++i) a[i] = (int64_t)(next_rand() % 100000);
        break;
    default:
        for (int64_t i = 0; i < n; ++i) a[i] = 0;
        break;
    }
}

/* which: 0 = timSortInts (stable native), 1 = acvm_sort I32 (the primitive). */
static void check32(int64_t n, int shape, int which) {
    ++cases;
    int32_t* a = (int32_t*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int32_t));
    int32_t* b = (int32_t*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int32_t));
    fill_i32(a, n, shape);
    memcpy(b, a, (size_t)n * sizeof(int32_t));

    int64_t xorBefore = 0;
    for (int64_t i = 0; i < n; ++i) xorBefore ^= (int64_t)a[i];

    if (which == 0) {
        timSortInts(a, n);
    } else if (acvm_sort(a, n, ACVM_KIND_I32) != 0) {
        fail("rc != 0", shape_name(shape), n, -1);
    }
    qsort(b, (size_t)n, sizeof(int32_t), cmp_i32);

    int64_t xorAfter = 0;
    for (int64_t i = 0; i < n; ++i) xorAfter ^= (int64_t)a[i];
    if (xorAfter != xorBefore) fail("not a permutation", shape_name(shape), n, -1);
    for (int64_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) { fail("mismatch vs qsort", shape_name(shape), n, i); break; }
    }
    for (int64_t i = 1; i < n; ++i) {
        if (a[i] < a[i - 1]) { fail("not ascending", shape_name(shape), n, i); break; }
    }
    free(a);
    free(b);
}

static void check64(int64_t n, int shape, int which) {
    ++cases;
    int64_t* a = (int64_t*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    int64_t* b = (int64_t*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    fill_i64(a, n, shape);
    memcpy(b, a, (size_t)n * sizeof(int64_t));

    int64_t xorBefore = 0;
    for (int64_t i = 0; i < n; ++i) xorBefore ^= a[i];

    if (which == 2) {
        timSortInts64(a, n);
    } else if (acvm_sort(a, n, ACVM_KIND_I64) != 0) {
        fail("i64 rc != 0", shape_name(shape), n, -1);
    }
    qsort(b, (size_t)n, sizeof(int64_t), cmp_i64);

    int64_t xorAfter = 0;
    for (int64_t i = 0; i < n; ++i) xorAfter ^= a[i];
    if (xorAfter != xorBefore) fail("i64 not a permutation", shape_name(shape), n, -1);
    for (int64_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) { fail("i64 mismatch vs qsort", shape_name(shape), n, i); break; }
    }
    free(a);
    free(b);
}

/* Stability: element = (key << 12) | index, so indices must ascend within each
 * key group after a stable sort. n is capped at 4096 to keep the index in 12
 * bits. */
static void check_stability_i32(int64_t n, int shape) {
    ++cases;
    if (n < 2 || n > 4096) return;
    int32_t* a = (int32_t*)malloc((size_t)n * sizeof(int32_t));
    for (int64_t i = 0; i < n; ++i) {
        int32_t key = 0;
        switch (shape) {
        case SH_RANDOM: key = (int32_t)(next_rand() % 512); break;
        case SH_FEW: key = (int32_t)(next_rand() % 8); break;
        case SH_TWO_VALUES: key = (int32_t)(next_rand() & 1u); break;
        case SH_EQUAL: key = 3; break;
        case SH_ASC: key = (int32_t)(i / 8); break;
        case SH_DESC: key = (int32_t)((n - i) / 8); break;
        case SH_WIDE_DUPES: key = (int32_t)(i % 64); break;
        default: key = (int32_t)(i % 16); break;
        }
        a[i] = (key << 12) | (int32_t)i;
    }
    timSortInts(a, n);
    for (int64_t i = 1; i < n; ++i) {
        const int32_t kPrev = a[i - 1] >> 12, kCur = a[i] >> 12;
        const int32_t iPrev = a[i - 1] & 0xFFF, iCur = a[i] & 0xFFF;
        if (kPrev > kCur) {
            fail("stability: keys out of order", shape_name(shape), n, i);
            break;
        }
        if (kPrev == kCur && iPrev > iCur) {
            fail("stability: equal keys reordered", shape_name(shape), n, i);
            break;
        }
    }
    free(a);
}

/* ── doubles ───────────────────────────────────────────────────────────────
 *
 * The folded bit pattern is the sort key, so "ascending" means ascending under
 * `acvm_double_key`, which is the same relation `sortDoubleNative` guarantees.
 * Checking with `<` would be wrong: NaN is unordered under `<`, which is the
 * whole reason this engine exists.
 */
static uint64_t double_key(double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return (bits & 0x8000000000000000ull) ? ~bits : (bits | 0x8000000000000000ull);
}

static void check_double(const double* input, int64_t n, const char* label) {
    ++cases;
    double* a = (double*)malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    memcpy(a, input, (size_t)n * sizeof(double));

    uint64_t xorBefore = 0;
    for (int64_t i = 0; i < n; ++i) xorBefore ^= double_key(a[i]);

    sortDoubleNative(a, n);

    uint64_t xorAfter = 0;
    for (int64_t i = 0; i < n; ++i) xorAfter ^= double_key(a[i]);
    if (xorAfter != xorBefore) fail("double: not a permutation", label, n, -1);
    for (int64_t i = 1; i < n; ++i) {
        if (double_key(a[i]) < double_key(a[i - 1])) {
            fail("double: key not ascending", label, n, i);
            break;
        }
    }
    free(a);
}

static void check_doubles(void) {
    /* Every awkward value at once: both zeroes, both infinities, NaN, denormals.
       The old quicksort path returned this array unsorted and reported nothing. */
    const double specials[] = {0.0, -0.0, INFINITY, -INFINITY, NAN, -1.0, 1.0,
                               5e-324, -5e-324, 3.14159, -2.71828, 42.0};
    const int nSpecials = (int)(sizeof(specials) / sizeof(specials[0]));
    check_double(specials, nSpecials, "double-specials");

    double allNaN[64];
    for (int i = 0; i < 64; ++i) allNaN[i] = NAN;
    check_double(allNaN, 64, "double-all-nan");

    /* NaN scattered through ordinary values: the exact shape that used to break. */
    double mixed[256];
    for (int i = 0; i < 256; ++i) mixed[i] = (i % 7 == 0) ? NAN : (double)(255 - i);
    check_double(mixed, 256, "double-mixed-nan");

    /* Random bit patterns, which include NaN, denormals and both zeroes. */
    const int64_t sizes[] = {1, 2, 3, 7, 8, 15, 16, 31, 32, 255, 256, 1000, 20000};
    for (int k = 0; k < (int)(sizeof(sizes) / sizeof(sizes[0])); ++k) {
        const int64_t n = sizes[k];
        double* a = (double*)malloc((size_t)n * sizeof(double));
        for (int64_t i = 0; i < n; ++i) {
            const uint64_t bits = ((uint64_t)next_rand() << 32) | next_rand();
            memcpy(&a[i], &bits, sizeof(double));
        }
        check_double(a, n, "double-random-bits");
        free(a);
    }
}

int main(void) {
    /* Sizes straddle every constant in algorithm.c: ACVM_RADIX_MIN_N (256),
       ACVM_TS_LOCAL (512), the minrun boundaries (63/64/65) and the 1024 point
       where the sampling window span is reached. */
    const int64_t sizes[] = {0, 1, 2, 3, 4, 5, 7, 8, 11, 12, 13, 16, 31, 32, 33, 63,
                             64, 65, 100, 127, 128, 129, 255, 256, 257, 511, 512, 513,
                             1000, 1023, 1024, 1025, 4096, 20000, 100000};
    const int nSizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

    for (int s = 0; s < SH_COUNT; ++s) {
        for (int i = 0; i < nSizes; ++i) {
            check32(sizes[i], s, 0);
            check32(sizes[i], s, 1);
            check64(sizes[i], s, 2);
            check64(sizes[i], s, 3);
        }
    }

    for (int s = 0; s < SH_COUNT; ++s) {
        for (int64_t n = 2; n <= 4096; n = n * 2 + 1) {
            check_stability_i32(n, s);
        }
        check_stability_i32(3000, s);
    }

    check_doubles();

    printf("sorts_test: %d cases, %d failures\n", cases, failures);
    return failures != 0;
}
