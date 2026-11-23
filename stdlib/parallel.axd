//  parallel.axd — the declaration half of true parallel computation.
//
//  `library "parallel"` means these functions are in parallel.dll / libparallel.so,
//  with the implementation in stdlib/parallel.c. OS threads over **pure data**, so cores.
//
//      function import (return:(parallel));
//
//      int data[1000000];
//      parallelSortInt(#data[0], 1000000, 0);  // 0 = use every core
//      print(parallelSumInt(#data[0], 1000000, 0));
//
//  ── how this splits the work with sync ───────────────────────────────────
//
//  `sync` (pure Axiom) solves **races**: when several `process`es share state, the
//  safe points break compound operations. That is correctness, and one thread needs it too.
//
//  `parallel` (this module) solves **speed**: `process` cannot use several cores,
//  as bytecode has one interpreter thread; these chunk pure work onto OS threads for real.
//
//  The two combine: `process` structures the concurrency (who waits, who produces),
//  and `parallel` pushes heavy computation onto the cores.
//
//  ── why there is no ParallelFor(callback) ────────────────────────────────
//
//  Axiom has no function pointer type and the C side cannot receive an Axiom function,
//  so each parallel function hard-codes what it computes (sum, count, sort). One
//  operation, one function — the inescapable cost of no function pointers, and worth it.
//
//  ── parameter conventions ────────────────────────────────────────────────
//
//    * `threads <= 0`  → every core (`os.cpuCount()` of them)
//    * `threads == 1`  → one thread; results must match the parallel version value for value
//    * `threads` above the element count → drops to the element count
//    * thread creation fails → falls back to one thread, **no error** (same result, slower)
//
//  ── no Axiom fallback ────────────────────────────────────────────────────
//
//  These exist to "use every core"; an Axiom version cannot parallelise, so a fallback
//  is pointless — for a single-threaded version use `algorithm` (`sort` / `minIndex` etc.).
//  A missing dll reports `cannot load library 'parallel'`.

module "parallel" version "0.1.0";

library "parallel";

// ─── reductions ───────────────────────────────────────────────────────────
//
// These touch every element once, so the parallel gain is direct: no chunk depends
// on another, results merge by +/compare, so the two versions **must agree value for value**.

//  Sum, returning a 64-bit total (32-bit elements overflow). Empty array returns 0.
function parallelSumInt(ptr const int data, int count, int threads):int64;

//  The maximum itself. Empty array returns 0 — there is no "no maximum", so check length.
function parallelMaxInt(ptr const int data, int count, int threads):int;

//  The minimum itself. Empty array returns 0.
function parallelMinInt(ptr const int data, int count, int threads):int;

//  Count occurrences of `value`.
function parallelCountInt(ptr const int data, int count, int value, int threads):int64;

// ─── sorting ──────────────────────────────────────────────────────────────

//  In-place ascending sort: chunk-wise quicksort, then an ordered merge.
//
//  **The merge is not parallelised**: sorting is O(n log n) versus merging O(n), so
//  sorting dominates. A parallel merge needs several pairwise rounds, each with a
//  sync, doubling the complexity for limited gain.
//
//  Returns the thread count used (1 = fallback); it tells you if parallelism took effect.
function parallelSortInt(ptr int data, int count, int threads):int;

// ─── filling ──────────────────────────────────────────────────────────────

//  Fill `count` elements with `value`. Returns the number filled.
function parallelFillInt(ptr int data, int count, int value):int;
