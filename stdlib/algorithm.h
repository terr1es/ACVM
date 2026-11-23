//===--- algorithm.h - C interface of the stdlib 'algorithm' module ------===//
//
// Axiom code cannot call C directly: the VM's memory is a sparse page table,
// so there is no `int*` to hand out. The bridge is marshalling -- the VM copies
// a range into a contiguous buffer, the C function runs on that buffer, and the
// result is copied back. Both directions are memcpy shaped, so an O(n log n)
// algorithm still costs O(n log n) plus two memcpys.
//
// Two kinds of function live here, with different wiring:
//
//   acvm_sort / acvm_reverse   back the compiler primitives. Adding one means
//                              editing src/Sema/Builtin.{h,cpp}, SemaExpr.cpp and
//                              VMBuiltins.cpp, and rebuilding acvm.exe.
//   everything below           exported from algorithm.dll. Adding one means a
//                              declaration here, a definition in algorithm.c, a
//                              line in algorithm.axd, and a rebuild of the DLL --
//                              never a compiler rebuild.
//
// Rules for the implementation:
//   * never allocate through malloc for data that outlives the call -- the VM
//     owns memory, has a 256 MB cap, and cannot free foreign pointers,
//   * an error is reported by returning a non-zero code, not by throwing or
//     aborting,
//   * `count` is an element count, never a byte count: the caller multiplies.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Element kinds the native helpers understand. These mirror Axiom's scalars:
/// `int` is 4 bytes, `int64` and `double` are 8.
typedef enum AcvmElemKind {
    ACVM_KIND_I32 = 0,
    ACVM_KIND_I64 = 1,
    ACVM_KIND_F64 = 2
} AcvmElemKind;

/// Sorts `count` elements in place, ascending. `data` points at a contiguous
/// buffer owned by the caller and holds at least `count * sizeof(element)`
/// bytes. Returns 0 on success, non-zero when `kind` is not supported.
int acvm_sort(void* data, int64_t count, int kind);

/// Reverses `count` elements in place. Same buffer contract as `acvm_sort`.
/// Returns 0 on success, non-zero when `kind` is not supported.
int acvm_reverse(void* data, int64_t count, int kind);

//===--- The algorithm library ------------------------------------------===//
//
// The functions below are not compiler primitives: they are exported from the
// algorithm shared library and reached through stdlib/algorithm.axd. Each name
// is exactly the Axiom name, because the dynamic boundary has no mangling and no
// lookup table -- the symbol *is* the contract.
//
// Every array is `int32_t` and every length is an *element* count, never a byte
// count. A function returning `int64_t` returns -1 (or 0 where noted) for a
// condition the caller should look at; nothing here traps or aborts.

//  First index whose value is >= `value`; `count` when every element is smaller.
//  `data` must already be sorted ascending.
int64_t lowerBound(void* data, int64_t count, int64_t value);

//  Stable, adaptive sort: natural runs, minrun blocks finished with binary
//  insertion sort, a balanced run stack, and merges trimmed by binary search.
//  O(n) on input that is already ordered, O(n log n) worst case.
//
//  `timSortInts` is `int` (4 byte) elements, `timSortInts64` is `int64` (8 byte).
//  Both sort in place and return nothing; when the merge scratch cannot be
//  allocated the array is sorted by the median-of-three quicksort instead, which
//  is the one case where the order of equal elements is not preserved.
void timSortInts(void* data, int64_t count);
void timSortInts64(void* data, int64_t count);

/// Sorts `count` doubles ascending by radix over the folded IEEE-754 bit pattern.
/// NaN and both zeroes get a definite position, which a comparison sort cannot
/// give them; this is the reason the bit pattern is used here, not just speed.
void sortDoubleNative(void* data, int64_t count);

//  First index whose value is > `value`; `count` when none is.
int64_t upperBound(void* data, int64_t count, int64_t value);

//  Index of `value`, or -1. Requires sorted input.
int64_t binarySearch(void* data, int64_t count, int64_t value);

//  Rearranges `data` so that `data[k]` is the k-th smallest. Does nothing when
//  `k` is out of range.
int64_t nthElement(void* data, int64_t count, int64_t k);

//  Median of `count` values. Rearranges `data` (quickselect, no full sort).
int64_t medianValue(void* data, int64_t count);

//  Rotates left by `by`; negative values rotate right.
int64_t rotateInts(void* data, int64_t count, int64_t by);

//  Partitions around `pivot`; returns the first index not below it.
int64_t partitionInts(void* data, int64_t count, int64_t pivot);

//  Compacts equal neighbours of a sorted array; returns the new count.
int64_t uniqueSorted(void* data, int64_t count);

//  Merges two sorted runs into `destination`, which must hold `capacity`
//  elements. Returns the merged length, or -1 when it does not fit.
int64_t mergeInts(void* destination, int64_t capacity, void* first, int64_t firstCount,
                  void* second, int64_t secondCount);

//  Fills `count` elements with `value`.
int64_t fillInts(void* data, int64_t count, int64_t value);

//  Index of the first occurrence of `value`, or -1.
int64_t findInt(void* data, int64_t count, int64_t value);

//  Number of elements equal to `value`.
int64_t countInt(void* data, int64_t count, int64_t value);

//  Index of the smallest / largest element, or -1 when empty. Ties go to the
//  earliest index.
int64_t minIndex(void* data, int64_t count);
int64_t maxIndex(void* data, int64_t count);

//  1 when ascending (equal neighbours are fine), else 0.
int64_t isSortedAsc(void* data, int64_t count);

//  1 when the two ranges hold equal elements, else 0. Counts must agree.
int64_t equalInts(void* first, int64_t firstCount, void* second, int64_t secondCount);

//  Max-heap in array layout: children of i are 2i+1 and 2i+2.
int64_t makeHeap(void* data, int64_t count);
//  Restores the heap after the caller appended an element at data[count-1].
int64_t pushHeap(void* data, int64_t count);
//  Moves the maximum to data[count-1] and restores the heap over count-1 items.
int64_t popHeap(void* data, int64_t count);
//  1 when the array satisfies the heap order, else 0.
int64_t isHeap(void* data, int64_t count);

#ifdef __cplusplus
}
#endif
