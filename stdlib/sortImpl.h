//===--- sortImpl.h - the one quicksort in the tree --------------------===//
//
// Included by algorithm.c (which instantiates it for int32 / int64 / double) and
// by threadpoolImpl.h (which needs int32 for its per-chunk sort). The macro body
// used to exist twice: once here as a macro family, once hand-written for int32
// inside the pool engine. Keeping one copy means a fix to the partition scheme
// cannot land in one and miss the other.
//
// A macro instead of a function because a comparator callback -- what `qsort`
// takes -- costs an indirect call per comparison, which is the main reason
// `qsort` loses to a hand written sort by a factor of two or more.
//
// Instantiate with:
//
//     ACVM_DEFINE_SORT(i32, int32_t)      // yields acvm_sort_i32
//
// The generated function is `static`, so each translation unit that instantiates
// it gets its own copy with no linker interaction. That is deliberate: the
// library modules are separate shared objects and cannot link against each
// other's internals.

#pragma once

#include <stdint.h>

#define ACVM_SWAP(TYPE, lhs, rhs)                                              \
    do {                                                                       \
        TYPE acvm_tmp_ = (lhs);                                                 \
        (lhs) = (rhs);                                                          \
        (rhs) = acvm_tmp_;                                                      \
    } while (0)

/// Median of three at both ends and the middle, so already sorted and reversed
/// input does not degrade to quadratic.
#define ACVM_MEDIAN3(TYPE, a, n, mid)                                          \
    do {                                                                       \
        if ((a)[(mid)] < (a)[0]) ACVM_SWAP(TYPE, (a)[(mid)], (a)[0]);           \
        if ((a)[(n)-1] < (a)[0]) ACVM_SWAP(TYPE, (a)[(n)-1], (a)[0]);           \
        if ((a)[(n)-1] < (a)[(mid)]) ACVM_SWAP(TYPE, (a)[(n)-1], (a)[(mid)]);   \
    } while (0)

#define ACVM_DEFINE_SORT(SUFFIX, TYPE)                                          \
    static void acvm_sort_##SUFFIX(TYPE* a, int64_t n) {                        \
        /* Ranges of 12 or fewer are finished by insertion sort: below that the \
           call overhead of another partition costs more than it saves. */      \
        while (n > 12) {                                                        \
            int64_t mid = n / 2;                                                \
            ACVM_MEDIAN3(TYPE, a, n, mid);                                       \
            const TYPE pivot = a[mid];                                           \
            int64_t left = -1;                                                   \
            int64_t right = n;                                                   \
            for (;;) {                                                           \
                /* The bounds tests are not redundant: swapping moves a[0] and   \
                   a[n-1], so the median-of-three invariant alone does not keep  \
                   the scans inside the array. */                                \
                do {                                                             \
                    ++left;                                                      \
                } while (left < n && a[left] < pivot);                           \
                do {                                                             \
                    --right;                                                     \
                } while (right > 0 && a[right] > pivot);                         \
                if (left >= right) {                                             \
                    break;                                                       \
                }                                                                \
                ACVM_SWAP(TYPE, a[left], a[right]);                              \
            }                                                                    \
            int64_t split = right;                                               \
            if (split >= n - 1) {                                                \
                split = n - 2;                                                   \
            }                                                                    \
            if (split < 0) {                                                     \
                split = 0;                                                       \
            }                                                                    \
            const int64_t leftSize = split + 1;                                  \
            const int64_t rightSize = n - leftSize;                              \
            /* Recurse into the smaller half, loop on the larger: recursion      \
               depth stays O(log n) instead of O(n). */                          \
            if (leftSize < rightSize) {                                          \
                acvm_sort_##SUFFIX(a, leftSize);                                 \
                a += leftSize;                                                    \
                n = rightSize;                                                    \
            } else {                                                             \
                acvm_sort_##SUFFIX(a + leftSize, rightSize);                     \
                n = leftSize;                                                     \
            }                                                                    \
        }                                                                        \
        for (int64_t i = 1; i < n; ++i) {                                        \
            const TYPE key = a[i];                                               \
            int64_t j = i - 1;                                                   \
            while (j >= 0 && a[j] > key) {                                       \
                a[j + 1] = a[j];                                                 \
                --j;                                                             \
            }                                                                    \
            a[j + 1] = key;                                                       \
        }                                                                        \
    }
