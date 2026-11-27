//  algorithm.axd — declarations for the algorithm library's native half.
//
//  `library "algorithm"` means these functions are implemented in algorithm.dll
//  (Windows) / libalgorithm.so (Linux), built from stdlib/algorithm.c:
//
//      function import (return:(algorithm));    // resolves to algorithm.axd
//
//      function main():void {
//          int data[1000] = {0};
//          sort(#data[0], 1000);                // TimSort, native when the DLL is present
//          print(binarySearch(#data[0], 1000, 42));
//          eof: pass;
//      }
//
//  Two things make a program work whether or not the shared library is present, and
//  only one of them is an Axiom body:
//
//    * the functions the Axiom side implements (`sort`, `sortIndicesByKey`, the
//      `*Strings` family, ...) have a body in stdlib/algorithm.ax that shares one
//      FuncInfo with this declaration. The body wins at run time when the library
//      cannot be loaded.
//    * the rest -- `binarySearch64`, `heapSort64`, `mergeInts64` and the other 64 bit
//      ones -- have **no** Axiom body. They work anyway because stdlib/algorithm.c is
//      also compiled into the VM itself (see the `acvm_vm` target), so the code is
//      already in the binary and the DLL is an alternative rather than the source.
//      An earlier version of this comment claimed every declaration had an Axiom
//      body, which is not true and hid that dependency.
//
//  Signatures must stay in step with stdlib/algorithm.ax -- a mismatch is reported at
//  the declaration.
//
//  ── two element widths ────────────────────────────────────────────────────
//
//  Each algorithm exists for `int` (4 bytes) and `int64` (8 bytes). The 32 bit
//  version keeps the bare name, the 64 bit one appends `64`. Counts and indices
//  stay `int` in both -- they are element counts, not values.
//
//  NOT declared here: `reverse`, `sortRange`, `sortStrings`, `isSortedStrings`.
//  Those need no shared library -- the first two delegate to the `reverseInts`
//  compiler primitive (linked into acvm.exe), and the two string ones are written
//  in Axiom because a native sort cannot read string contents: an array of
//  `string` marshals to descriptors pointing at paged VM memory, not host memory.
//  See the comment above `sortStrings` in algorithm.ax.
//
//  `sortDouble` *is* declared here, as `sortDoubleNative`: doubles sort by radix
//  over the folded bit pattern, and that is not just faster than the comparison
//  sort it replaced -- it is the only one of the two that can place a NaN.
//
//  `sort` / `sortInt64` *are* declared here, as `timSortInts` / `timSortInts64`:
//  the sort itself is the TimSort in algorithm.c, and algorithm.ax carries an
//  Axiom fallback under the same names.
//
//  ── conventions ───────────────────────────────────────────────────────────
//
//    * an array is `ptr T` immediately followed by its element count; a `ptr`
//      with no count after it is rejected at compile time;
//    * an index result of -1 means "no such element";
//    * `*Sorted` / `lowerBound` / `upperBound` / `binarySearch` / `percentileSorted`
//      assume ascending input and do not check -- sort first;
//    * `nthElement`, `medianValue`, `partialSort`, `heapSort`, `topK` rearrange
//      the array they are given;
//    * the VM cannot verify a count against the real array length, so a count that
//      is too large is memory corruption, exactly as with `memcpy`.

module "algorithm" version "0.1.0";

library "algorithm";

// ─── sorting ───────────────────────────────────────────────────────────────

//  Sort in place, ascending, stable. `sort` / `sortInt64` in algorithm.ax are
//  the names to call; they forward here.
//
//  Stable regardless of path. The C side picks between three engines:
//
//      countable    few distinct values      counting sort, O(n + range)
//      unstructured no order to exploit      LSD radix, 4 passes (8 for int64)
//      structured   mostly already ordered   TimSort, O(n) on ordered input
//
//  All three are stable, and none has an adversarial input, so no data shape
//  makes this quadratic. How the choice is made -- and why it is a sampled
//  range plus a windowed monotonicity scan rather than the first run length --
//  is documented above ACVM_DEFINE_ANALYZE in algorithm.c.
//
//  Measured on n = 1000000 (`bench/sort_engines.ax`):
//
//      random        93 ms -> 11 ms    sorted          1 ms (unchanged)
//      eight values  4.0 ms -> 1.5 ms  random/1000    3.6 ms -> 2.1 ms
//
//  The Axiom body in algorithm.ax carries two of the three engines: TimSort and
//  counting sort, chosen by the same n/8 range rule. Radix is the one it does
//  not have -- measured at 13.5M instructions against TimSort's 14.4M for wide
//  random data on n = 20000, a 7% gain that would not pay for the extra code
//  plus a second copy for int64. So without the shared library `sort` is correct
//  and handles duplicates well (see the numbers below), but stays n log n on
//  wide random input.
//
//  Measured on n = 20000, Axiom body only, instructions executed:
//
//      eight values  11.2M -> 4.3M   (-62%)
//      random/1000   14.3M -> 4.6M   (-68%)
//      random wide   14.5M -> 14.5M  (unchanged: declines, TimSort runs)
//
//  The last row is the point of the sampling gate: a full min/max scan on data
//  that turns out to be wide is a pass thrown away, and it cost 3.7% before the
//  gate existed.
//
//  One corner differs between the two sides, unreachable in practice: if the
//  scratch allocation fails the C side finishes with a quicksort (not stable)
//  and the Axiom side with insertion sort (stable).
//
//  A note on `sortInts`: the compiler primitive takes the same three-way
//  decision but keeps the quicksort as its structured engine -- it has no
//  TimSort, so on ordered input it stays O(n log n). Use `sort` when either
//  stability or adaptivity matters.
function timSortInts(ptr int data, int count):void;
function timSortInts64(ptr int64 data, int count):void;

//  Doubles, ascending. Radix over the folded IEEE-754 bit pattern.
//
//  This is the one place where the bit pattern is the *reason* the sort is
//  correct rather than a trick to make it fast: `a < b` is false for NaN in both
//  directions, so a comparison sort cannot place it, and the old quicksort path
//  silently returned an unsorted array when a NaN was present. Folding the sign
//  into the exponent gives every double -- including NaN and both zeroes -- a
//  definite position, monotone from -inf to +NaN.
//
//  Consequences worth stating: -0.0 sorts immediately before +0.0 (they compare
//  equal, so a stable sort keeps their input order), and NaN sorts after +inf.
//  Neither matches `a < b` semantics, because for NaN there are none.
//
//  Stable, and O(n) in six passes. Falls back to the quicksort only if the
//  temporary buffer cannot be allocated, which is where NaN input would still
//  break -- see the comment above `acvm_radix_f64` in algorithm.c.
function sortDoubleNative(ptr double data, int count):void;

// ─── search on sorted input ────────────────────────────────────────────────

//  First index whose value is >= `value`; `count` when every element is smaller.
function lowerBound(ptr int data, int count, int value):int;

//  First index whose value is > `value`; `count` when no element exceeds it.
function upperBound(ptr int data, int count, int value):int;

//  Index of `value`, or -1 when it is absent.
function binarySearch(ptr int data, int count, int value):int;

function lowerBound64(ptr int64 data, int count, int64 value):int;
function upperBound64(ptr int64 data, int count, int64 value):int;
function binarySearch64(ptr int64 data, int count, int64 value):int;

// ─── selection ─────────────────────────────────────────────────────────────

//  Rearranges `data` so that `data[k]` holds the k-th smallest element, every
//  smaller value before it and every larger one after. O(n) average instead of
//  O(n log n): the reason to use it over `sort` when only one order statistic is
//  wanted. Does nothing when `k` is out of range.
function nthElement(ptr int data, int count, int k):void;

//  Median of the values. Rearranges `data` (quickselect, no full sort).
//  For an even count this averages the two central values; with `int64` inputs
//  near INT64_MAX that sum can overflow, which the Axiom fallback shares.
function medianValue(ptr int data, int count):int;

function nthElement64(ptr int64 data, int count, int k):void;
function medianValue64(ptr int64 data, int count):int64;

// ─── sequence operations ───────────────────────────────────────────────────

//  Rotates left by `by` places, in place; negative values rotate right.
//  `rotateInts(#d[0], 5, 2)` on `1 2 3 4 5` gives `3 4 5 1 2`.
function rotateInts(ptr int data, int count, int by):void;

//  Partitions around `pivot` in place. Returns the first index whose value is not
//  below `pivot`: `data[0 .. r-1] < pivot <= data[r .. count-1]`. Within each side
//  the order is unspecified.
function partitionInts(ptr int data, int count, int pivot):int;

//  Removes duplicate neighbours in a *sorted* array, returning the new length. The
//  array keeps its size; only the first `result` elements are meaningful.
function uniqueSorted(ptr int data, int count):int;

//  Merges two ascending runs into `destination`, which must hold `capacity`
//  elements. Stable. Returns the merged length, or -1 when it does not fit.
//
//  `destination` must be a third array: the VM gives each array parameter its own
//  buffer, so writing back over an input would be ambiguous.
function mergeInts(ptr int destination, int capacity, ptr int first, int firstCount,
                   ptr int second, int secondCount):int;

//  Sets `count` elements to `value`.
function fillInts(ptr int data, int count, int value):void;

function rotateInts64(ptr int64 data, int count, int by):void;
function partitionInts64(ptr int64 data, int count, int64 pivot):int;
function uniqueSorted64(ptr int64 data, int count):int;
function mergeInts64(ptr int64 destination, int capacity, ptr int64 first, int firstCount,
                     ptr int64 second, int secondCount):int;
function fillInts64(ptr int64 data, int count, int64 value):void;

// ─── set operations on sorted runs ─────────────────────────────────────────
//
// All three take two ascending inputs and one destination buffer, and return the
// number written, or -1 when the destination is too small. Six native arguments
// is the boundary's limit: three (pointer, count) pairs, exactly.

//  Values present in both. Equal keys appear once.
function intersectSorted(ptr int destination, int capacity, ptr int first, int firstCount,
                         ptr int second, int secondCount):int;

//  Values present in either. Equal keys appear once.
function unionSorted(ptr int destination, int capacity, ptr int first, int firstCount,
                     ptr int second, int secondCount):int;

//  Values in `first` that are absent from `second`.
function differenceSorted(ptr int destination, int capacity, ptr int first, int firstCount,
                          ptr int second, int secondCount):int;

function intersectSorted64(ptr int64 destination, int capacity, ptr int64 first,
                           int firstCount, ptr int64 second, int secondCount):int;
function unionSorted64(ptr int64 destination, int capacity, ptr int64 first, int firstCount,
                       ptr int64 second, int secondCount):int;
function differenceSorted64(ptr int64 destination, int capacity, ptr int64 first,
                            int firstCount, ptr int64 second, int secondCount):int;

// ─── queries ───────────────────────────────────────────────────────────────
//
// One native compare per element, against a bounds check and a page lookup per
// element in Axiom -- this is where the shared library pays off most.

//  Index of the first / last occurrence of `value`, or -1.
function findInt(ptr int data, int count, int value):int;
function findLastInt(ptr int data, int count, int value):int;

//  Number of elements equal to `value`.
function countInt(ptr int data, int count, int value):int;

//  Index of the smallest / largest element, or -1 when empty. Ties go to the
//  earliest index.
function minIndex(ptr int data, int count):int;
function maxIndex(ptr int data, int count):int;

//  True when the array is ascending / descending (equal neighbours are fine).
function isSortedAsc(ptr int data, int count):bool;
function isSortedDesc(ptr int data, int count):bool;

//  True when the two ranges hold equal elements, element by element. Both lengths
//  are passed: a `ptr` with no count is not an array pair.
function equalInts(ptr int first, int firstCount, ptr int second, int secondCount):bool;

//  Writes four values into `out`: minimum, maximum, index of the minimum, index
//  of the maximum. `outCapacity` must be at least 4. One pass instead of the two
//  that `minIndex` + `maxIndex` would cost. Returns 1 when written, 0 when the
//  input was empty, -1 when `out` is too small.
function minMax(ptr int out, int outCapacity, ptr int data, int count):int;

//  Compacts out every element equal to `value`, returning the new length. The tail
//  keeps its old contents, so only the first `result` elements are meaningful.
function removeValue(ptr int data, int count, int value):int;

//  Value at percentile `p` (0..100, clamped) of a sorted array, by the
//  nearest-rank rule: index `p * (count - 1) / 100`, truncated.
function percentileSorted(ptr int data, int count, int p):int;

function findInt64(ptr int64 data, int count, int64 value):int;
function findLastInt64(ptr int64 data, int count, int64 value):int;
function countInt64(ptr int64 data, int count, int64 value):int;
function minIndex64(ptr int64 data, int count):int;
function maxIndex64(ptr int64 data, int count):int;
function isSortedAsc64(ptr int64 data, int count):bool;
function isSortedDesc64(ptr int64 data, int count):bool;
function equalInts64(ptr int64 first, int firstCount, ptr int64 second, int secondCount):bool;
function minMax64(ptr int64 out, int outCapacity, ptr int64 data, int count):int;
function removeValue64(ptr int64 data, int count, int64 value):int;
function percentileSorted64(ptr int64 data, int count, int p):int64;

// ─── binary heap ───────────────────────────────────────────────────────────
//
// Max-heap in array layout: the children of `i` are `2i+1` and `2i+2`, so
// `data[0]` is the maximum. Together these drive a priority queue without ever
// reallocating:
//
//      makeHeap(#q[0], n);
//      @(#q[0] + n) = item;  pushHeap(#q[0], n + 1);      // push
//      popHeap(#q[0], n);    top = @(#q[0] + n - 1);      // pop

function makeHeap(ptr int data, int count):void;

//  Restores the heap after the caller appended the new element at `data[count-1]`.
function pushHeap(ptr int data, int count):void;

//  Moves the maximum to `data[count-1]` and restores the heap over the first
//  `count-1` elements.
function popHeap(ptr int data, int count):void;

//  True when the array satisfies the heap order.
function isHeap(ptr int data, int count):bool;

function makeHeap64(ptr int64 data, int count):void;
function pushHeap64(ptr int64 data, int count):void;
function popHeap64(ptr int64 data, int count):void;
function isHeap64(ptr int64 data, int count):bool;

// ─── sorting that does not use the quicksort primitive ─────────────────────
//
// `sort` (from algorithm.ax, via the `sortInts` builtin) is a median-of-three
// quicksort: faster on average, quadratic on adversarial input. The two below are
// heap based, so their worst case is O(n log n) with no scratch buffer.

//  In-place ascending sort with a hard O(n log n) bound.
function heapSort(ptr int data, int count):void;

//  Moves the `k` largest elements to `data[count-k .. count-1]`, ascending;
//  everything before that range is unspecified. O(n + k log n), so cheaper than a
//  full sort when `k` is small. Returns the number moved (clamped to `count`).
function topK(ptr int data, int count, int k):int;

//  Sorts the first `k` elements ascending, leaving the rest unspecified but all of
//  them >= `data[k-1]`. Selection then a sort of just those `k`. Returns `k`
//  (clamped to `count`).
function partialSort(ptr int data, int count, int k):int;

function heapSort64(ptr int64 data, int count):void;
function topK64(ptr int64 data, int count, int k):int;
function partialSort64(ptr int64 data, int count, int k):int;

// ─── statistics ────────────────────────────────────────────────────────────

//  Writes `out[0] = mean` and `out[1] = population variance` as doubles over the
//  values of an `int` array. `outCapacity` must be at least 2, and `out` must be
//  a `ptr double` because a scalar double cannot be returned across the boundary
//  (it would come back in an SSE register the call site cannot read).
//  Returns 1 when written, 0 for an empty input, -1 when `out` is too small.
function variance(ptr double out, int outCapacity, ptr int data, int count):int;
