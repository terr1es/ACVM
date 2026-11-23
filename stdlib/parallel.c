//  parallel.c — true parallelism: split pure computation across the cores.
//
//  Built into parallel.dll / libparallel.so, exposed to Axiom via stdlib/parallel.axd.
//
//  ── how this file divides work with sync.ax ──────────────────────────────
//
//  `sync.ax` handles "several processes sharing state" — cooperative scheduling,
//  single-threaded round-robin. That solves **races**, not speed.
//
//  This file deals with another matter: **making the cores work at the same time**;
//  `process` cannot, since the interpreter runs bytecode on one thread. But pure data
//  needs no interpreter — an `int32_t*` plus a length computes the same for anyone.
//
//  ── why there are no callbacks here ──────────────────────────────────────
//
//  Axiom has no function pointers, so `ParallelFor(callback)` cannot be written. Each
//  parallel function instead **knows what it computes**: sum, count and sort are fixed
//  operations whose thread function is a compile-time C function, not a parameter.
//
//  The cost is one function per new operation. The payoff is real parallelism.
//
//  ── where the threads come from: a resident pool, not one per call ───────
//
//  The first version created threads on every call and reclaimed them when done.
//  Measured, that is a net loss: Windows creation plus teardown is 50-100 µs per
//  thread, so 12 threads are close to 1 ms — enough for a 2M sum to be slower (1.41s → 1.69s).
//
//  Now the pools are **cached per thread count** (`threadpoolImpl.h`), and repeated
//  calls at the same count reuse the same workers, paying only dispatch plus wait.
//
//  The VM destroys the pools before unloading this library: `VM::~VM` first looks for
//  the optional export `acvm_stdlib_shutdown` and calls it (see `acvm_parallel_shutdown`).
//
//  This was paid for with a crash. The old design was "never destroy the pool, let the
//  OS reclaim it at exit", noting that "`atexit` in a DLL drags down unload". `atexit`
//  is unusable, but "no cleanup" is no better: workers sleep in `Sleep`, and after
//  `FreeLibrary` unmaps their code pages, the next wake-up returns to a freed PC —
//  measured, one run in a dozen: `access=execute` at MEM_FREE. Only the host hook works.
//
//  ── contract ────────────────────────────────────────────────────────────
//
//    * `threads <= 0` = all cores, `threads == 1` = one thread (for comparison);
//    * a count above the element count drops to it, and empty chunks are skipped;
//    * **a failed pool creation always falls back to single-threaded**: same result,
//      only slower — a library that "sometimes errors" is unusable;
//    * sorting needs scratch, `malloc`ed and freed before return. Temporary memory
//      inside a function does not violate the no-malloc-for-long-lived-data rule.
//
//  ── determinism ─────────────────────────────────────────────────────────
//
//  Summing uses integer addition (order-independent chunks); min / max / count likewise;
//  sorting is "sort chunks + two-way merge", also deterministic. **So the parallel and
//  single-threaded builds must agree value for value**, and the tests compare the two.

#include <stdlib.h>
#include "threadpoolImpl.h"

#define ACVM_PARALLEL_CACHE 8
#define ACVM_PARALLEL_MIN_ELEMENTS_PER_THREAD 65536

typedef struct AcvmParallelPool {
    int64_t threads;    ///< 0 means this slot is still unused
    AcvmPool* pool;
} AcvmParallelPool;

static AcvmParallelPool gPoolCache[ACVM_PARALLEL_CACHE];
static AcvmPoolLock gPoolCacheLock;
static int gPoolCacheReady = 0;

/// Destroy every cached pool before the process exits.
///
/// The cached worker threads live until the process ends. Unloading the DLL without
/// letting them exit first unmaps their code pages while they are still in the loop —
/// the next wake-up jumps into freed address space (measured: the crash reports
/// `access=execute` with a MEM_FREE target at 0x7FF9..., a typical system DLL base).
///
/// The VM calls this through `acvm_stdlib_shutdown` before `FreeLibrary`. It does not
/// use `atexit`: this is a DLL, so `atexit` binds to the DLL's own exit — unreliable
/// timing, and it holds up the unload.
static void acvm_parallel_shutdown(void) {
    if (!gPoolCacheReady) {
        return;
    }
    for (int i = 0; i < ACVM_PARALLEL_CACHE; ++i) {
        AcvmPool* pool = NULL;
        acvm_pool_lock_acquire(&gPoolCacheLock);
        if (gPoolCache[i].pool != NULL) {
            pool = gPoolCache[i].pool;
            gPoolCache[i].pool = NULL;
        }
        acvm_pool_lock_release(&gPoolCacheLock);
        if (pool != NULL) {
            /* destroy outside the lock: it waits for workers to exit, must not block slots. */
            acvm_pool_destroy(pool);
        }
    }
}

/// Exported to the VM: destroy every cached pool before unloading, so workers exit.
///
/// This is the only cleanup entry here that needs external linkage — `VM::~VM` finds it
/// with `GetProcAddress` / `dlsym` and skips it when absent, so it is an **optional**
/// contract, and `acvm_parallel_shutdown` above therefore stays `static`.
void acvm_stdlib_shutdown(void) {
    acvm_parallel_shutdown();
}

/// Lazily initialise the cache lock. Every Axiom->C call runs on the VM's one thread,
/// so concurrent entry is impossible here and no `call_once` is needed.
static void acvm_parallel_cache_init(void) {
    if (!gPoolCacheReady) {
        acvm_pool_lock_init(&gPoolCacheLock);
        gPoolCacheReady = 1;
    }
}

/// Get (or create) the pool of `threads` workers. NULL means fall back to serial.
static AcvmPool* acvm_parallel_pool(int64_t threads) {
    if (threads < 1) {
        threads = 1;
    }
    if (threads > ACVM_POOL_MAX_THREADS) {
        threads = ACVM_POOL_MAX_THREADS;
    }
    acvm_parallel_cache_init();
    acvm_pool_lock_acquire(&gPoolCacheLock);

    for (int i = 0; i < ACVM_PARALLEL_CACHE; ++i) {
        if (gPoolCache[i].pool != NULL && gPoolCache[i].threads == threads) {
            AcvmPool* pool = gPoolCache[i].pool;
            acvm_pool_lock_release(&gPoolCacheLock);
            return pool;
        }
    }

    int slot = -1;
    for (int i = 0; i < ACVM_PARALLEL_CACHE; ++i) {
        if (gPoolCache[i].pool == NULL) {
            slot = i;
            break;
        }
    }
    acvm_pool_lock_release(&gPoolCacheLock);
    if (slot < 0) {
        // Cache full: fall back to serial instead of an uncached pool (the old behaviour).
        return NULL;
    }

    // Create outside the lock: it starts dozens of threads and must not block other tiers.
    AcvmPool* pool = acvm_pool_create(threads);
    if (pool == NULL) {
        return NULL;
    }

    acvm_pool_lock_acquire(&gPoolCacheLock);
    if (gPoolCache[slot].pool == NULL) {
        gPoolCache[slot].pool = pool;
        gPoolCache[slot].threads = threads;
        acvm_pool_lock_release(&gPoolCacheLock);
        return pool;
    }
    acvm_pool_lock_release(&gPoolCacheLock);
    // Slot taken by another tier: the pool went unused; destroy it (no threads leak), retry.
    acvm_pool_destroy(pool);
    return acvm_parallel_pool(threads);
}

/// Minimum elements per thread. Below this scale parallelism is always slower, so go serial.
static int64_t acvm_parallel_worker_count(int64_t count, int64_t requested) {
    if (count <= 0) {
        return 0;
    }
    int64_t workers = requested;
    if (workers <= 0) {
        workers = acvm_pool_cpu_count();
        const int64_t affordable = count / ACVM_PARALLEL_MIN_ELEMENTS_PER_THREAD;
        if (affordable < workers) {
            workers = affordable;
        }
    }
    if (workers > count) {
        workers = count;
    }
    if (workers < 1) {
        workers = 1;
    }
    return workers;
}


/// Reference single-threaded sum, shared by every fallback path.
static int64_t acvm_parallel_sum_serial(const int32_t* values, int64_t count) {
    int64_t total = 0;
    for (int64_t i = 0; i < count; ++i) {
        total += (int64_t)values[i];
    }
    return total;
}

//===--- reductions -------------------------------------------------------===//

/// Sum over `threads` threads (`<= 0` = every core). Returns the 64-bit total.
int64_t parallelSumInt(void* data, int64_t count, int64_t threads) {
    if (data == NULL || count <= 0) {
        return 0;
    }
    const int64_t workers = acvm_parallel_worker_count(count, threads);
    AcvmPool* pool = workers <= 1 ? NULL : acvm_parallel_pool(workers);
    if (pool == NULL) {
        return acvm_parallel_sum_serial((const int32_t*)data, count);
    }
    return acvm_pool_reduce(pool, (int32_t*)data, count, workers, ACVM_JOB_SUM, 0);
}

/// The maximum itself. Returns 0 for an empty array.
int64_t parallelMaxInt(void* data, int64_t count, int64_t threads) {
    if (data == NULL || count <= 0) {
        return 0;
    }
    const int64_t workers = acvm_parallel_worker_count(count, threads);
    AcvmPool* pool = workers <= 1 ? NULL : acvm_parallel_pool(workers);
    if (pool == NULL) {
        const int32_t* values = (const int32_t*)data;
        int32_t best = values[0];
        for (int64_t i = 1; i < count; ++i) {
            if (values[i] > best) { best = values[i]; }
        }
        return (int64_t)best;
    }
    return acvm_pool_reduce(pool, (int32_t*)data, count, workers, ACVM_JOB_MAX, 0);
}

/// The minimum itself. Returns 0 for an empty array.
int64_t parallelMinInt(void* data, int64_t count, int64_t threads) {
    if (data == NULL || count <= 0) {
        return 0;
    }
    const int64_t workers = acvm_parallel_worker_count(count, threads);
    AcvmPool* pool = workers <= 1 ? NULL : acvm_parallel_pool(workers);
    if (pool == NULL) {
        const int32_t* values = (const int32_t*)data;
        int32_t best = values[0];
        for (int64_t i = 1; i < count; ++i) {
            if (values[i] < best) { best = values[i]; }
        }
        return (int64_t)best;
    }
    return acvm_pool_reduce(pool, (int32_t*)data, count, workers, ACVM_JOB_MIN, 0);
}

/// Count how often `value` appears.
int64_t parallelCountInt(void* data, int64_t count, int64_t value, int64_t threads) {
    if (data == NULL || count <= 0) {
        return 0;
    }
    const int64_t workers = acvm_parallel_worker_count(count, threads);
    AcvmPool* pool = workers <= 1 ? NULL : acvm_parallel_pool(workers);
    if (pool == NULL) {
        const int32_t* values = (const int32_t*)data;
        const int32_t target = (int32_t)value;
        int64_t found = 0;
        for (int64_t i = 0; i < count; ++i) {
            found += values[i] == target;
        }
        return found;
    }
    return acvm_pool_reduce(pool, (int32_t*)data, count, workers, ACVM_JOB_COUNT, value);
}

//===--- sorting ---------------------------------------------------------===//

/// In-place ascending sort: parallel quicksort per chunk, then a two-way merge **on
/// the calling thread**. The merge is not parallel: sorting is O(n log n) and merging
/// O(n), so sorting dominates. Measured at 1M elements about 42 ms in-chunk sorting and
/// 8 ms merging — parallelising the merge would not pay for the extra synchronisation.
///
/// Returns the thread count used (1 = fallback or no parallelism needed, 0 = merge failed).
int64_t parallelSortInt(void* data, int64_t count, int64_t threads) {
    if (data == NULL || count <= 1) {
        return 1;
    }
    int32_t* values = (int32_t*)data;
    const int64_t workers = acvm_parallel_worker_count(count, threads);
    if (workers <= 1) {
        acvm_sort_i32(values, count);
        return 1;
    }
    AcvmPool* pool = acvm_parallel_pool(workers);
    if (pool == NULL) {
        acvm_sort_i32(values, count);
        return 1;
    }

    int64_t bounds[ACVM_POOL_MAX_THREADS + 1];
    AcvmPoolJob jobs[ACVM_POOL_MAX_THREADS];
    const int chunks = acvm_pool_split(count, workers, bounds);
    if (chunks <= 1) {
        acvm_sort_i32(values, count);
        return 1;
    }
    acvm_pool_build_jobs(jobs, chunks, bounds, ACVM_JOB_SORT, values, 0);
    acvm_pool_run_batch(pool, jobs, chunks);

    if (!acvm_pool_merge_runs(values, count, bounds, chunks)) {
        // The merge scratch buffer failed to allocate: chunks are each sorted, so the
        // whole is incomplete but does not crash. Return 0 so the caller knows.
        return 0;
    }
    return chunks;
}

//===--- filling ---------------------------------------------------------===//

/// Fill `count` elements with `value`. Returns the number filled.
///
/// Filling is memory-bandwidth bound, so threads barely help — but going through the
/// pool only costs dispatch plus wait, so large arrays still use it, small ones loop.
int64_t parallelFillInt(void* data, int64_t count, int64_t value) {
    if (data == NULL || count <= 0) {
        return 0;
    }
    int32_t* values = (int32_t*)data;
    const int32_t filled = (int32_t)value;
    const int64_t workers = acvm_parallel_worker_count(count, 0);
    if (workers <= 1) {
        for (int64_t i = 0; i < count; ++i) {
            values[i] = filled;
        }
        return count;
    }
    AcvmPool* pool = acvm_parallel_pool(workers);
    if (pool == NULL) {
        for (int64_t i = 0; i < count; ++i) {
            values[i] = filled;
        }
        return count;
    }

    int64_t bounds[ACVM_POOL_MAX_THREADS + 1];
    AcvmPoolJob jobs[ACVM_POOL_MAX_THREADS];
    const int chunks = acvm_pool_split(count, workers, bounds);
    acvm_pool_build_jobs(jobs, chunks, bounds, ACVM_JOB_FILL, values, value);
    acvm_pool_run_batch(pool, jobs, chunks);
    return count;
}
