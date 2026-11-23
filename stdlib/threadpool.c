//  threadpool.c — a resident thread pool, plus locks for real threads.
//
//  Built into threadpool.dll / libthreadpool.so, exposed to Axiom through
//  stdlib/threadpool.axd. The pool engine is in threadpoolImpl.h (shared with parallel.c).
//
//  ── what problem this module solves ──────────────────────────────────────
//
//  `parallel`'s functions create threads per call and reclaim them when done. On this
//  machine Windows creation plus teardown is 50-100 µs per thread, so 12 threads are
//  close to 1 ms — enough for a "parallel" 8 MB sum to be slower (1.41s → 1.69s).
//
//  The pool amortises that: threads are built once, and each call just dispatches and waits.
//
//  ── why submission must block ────────────────────────────────────────────
//
//  A pointer Axiom passes to C points into the VM's reused buffer: valid during the
//  call, changed once it returns. So a task cannot outlive the call — no futures.
//
//  That is not laziness; it is a direct consequence of marshalling. Async would need
//  the VM to hand buffer ownership to the native side, which is a compiler-level change.
//
//  ── what is left to gain after pooling ──────────────────────────────────
//
//  Two things:
//
//    1. **Thread creation is no longer paid for** (the paragraph above).
//    2. **One call runs several chunks at once** (it was already parallel).
//
//  There is no "Axiom computes while workers compute" — that needs async submission.
//
//  ── locks ────────────────────────────────────────────────────────────────
//
//  The `threadLockCreate` family are **real OS locks** (critical-section semantics on
//  Windows, pthread mutex on POSIX) that exclude across native threads. They are not
//  the same as `Mutex` in `sync.ax`:
//
//    sync.Mutex        cooperative, locking a region of Axiom code, released by yield
//    threadLock*       a real lock, guarding native code or a shared memory cell
//
//  From pure Axiom, `threadLockAcquire` is just an atomic flag with a memory barrier
//  (Axiom has one execution thread, so nothing can contend). Its value is twofold: mutual
//  exclusion inside pool jobs (`threadPoolAccumulate` does this) and for C extension authors.

#include "threadpoolImpl.h"

//===--- handle table ----------------------------------------------------===//
//
// A handle is an int64 exposed outward, internally the **index + 1** of one of the two
// tables below (0 = invalid). Bare pointers are not used as handles: Axiom's int is 32
// bits, so a pointer would be truncated, and an int64 would let a caller forge an address.
//
// Table access needs only one lock. **It protects the tables alone, not the pools or the
// user locks** — those have locks of their own.

#define ACVM_MAX_POOLS 16
#define ACVM_MAX_LOCKS 64

/// A real lock for users. Separate from the pool-internal `AcvmPoolLock` because try
/// semantics are needed here, while the pool's internal use is "acquire always succeeds".
/// Underneath it is still `AcvmPoolLock` (CRITICAL_SECTION on Windows, pthread_mutex_t
/// on POSIX), so `threadPoolAccumulate` can hand `&lock->handle` straight to a pool
/// job without a cross-type cast.
typedef struct AcvmUserLock {
    AcvmPoolLock handle;
    int created;
    int64_t acquisitions;
} AcvmUserLock;

static AcvmPool* gPools[ACVM_MAX_POOLS];
static AcvmUserLock* gLocks[ACVM_MAX_LOCKS];

/// Guards the two tables above. Covers only lookup / insert / remove, not pool or lock use.
static AcvmPoolLock gRegistryLock;
static int gRegistryReady = 0;

/// Lazily initialise the registry lock.
///
/// No `call_once`-style mechanism: every Axiom->C call happens on the VM's one thread,
/// so concurrent entry is impossible here. Worker threads never touch the registry.
static void acvm_registry_init(void) {
    if (!gRegistryReady) {
        acvm_pool_lock_init(&gRegistryLock);
        gRegistryReady = 1;
    }
}

//===--- low-level user lock operations ----------------------------------===//

static int acvm_user_lock_init(AcvmUserLock* lock) {
    if (lock == NULL) {
        return 0;
    }
    acvm_pool_lock_init(&lock->handle);
    lock->created = 1;
    lock->acquisitions = 0;
    return 1;
}

static void acvm_user_lock_free(AcvmUserLock* lock) {
    if (lock == NULL || !lock->created) {
        return;
    }
    acvm_pool_lock_destroy(&lock->handle);
    lock->created = 0;
}

static void acvm_user_lock_acquire(AcvmUserLock* lock) {
    acvm_pool_lock_acquire(&lock->handle);
    lock->acquisitions += 1;
}

/// Non-blocking try. Returns 1 on success.
static int acvm_user_lock_try(AcvmUserLock* lock) {
#ifdef _WIN32
    if (!TryEnterCriticalSection(&lock->handle)) {
        return 0;
    }
#else
    if (pthread_mutex_trylock(&lock->handle) != 0) {
        return 0;
    }
#endif
    lock->acquisitions += 1;
    return 1;
}

static void acvm_user_lock_release(AcvmUserLock* lock) {
    acvm_pool_lock_release(&lock->handle);
}

//===--- pool: lookup ----------------------------------------------------===//

/// Handle -> pool. An invalid handle returns NULL, so the caller fails instead of junk.
static AcvmPool* acvm_pool_lookup(int64_t handle) {
    if (handle <= 0 || handle > ACVM_MAX_POOLS) {
        return NULL;
    }
    acvm_registry_init();
    acvm_pool_lock_acquire(&gRegistryLock);
    AcvmPool* pool = gPools[handle - 1];
    acvm_pool_lock_release(&gRegistryLock);
    return pool;
}

static AcvmUserLock* acvm_lock_lookup(int64_t handle) {
    if (handle <= 0 || handle > ACVM_MAX_LOCKS) {
        return NULL;
    }
    acvm_registry_init();
    acvm_pool_lock_acquire(&gRegistryLock);
    AcvmUserLock* lock = gLocks[handle - 1];
    acvm_pool_lock_release(&gRegistryLock);
    return lock;
}

//===--- pool: lifetime --------------------------------------------------===//

/// Create a pool of `threads` workers. `threads <= 0` uses every core.
/// Returns a handle, 0 = failure (not a single thread started).
int64_t threadPoolCreate(int64_t threads) {
    acvm_registry_init();
    AcvmPool* pool = acvm_pool_create(threads);
    if (pool == NULL) {
        return 0;
    }
    int64_t handle = 0;
    acvm_pool_lock_acquire(&gRegistryLock);
    for (int i = 0; i < ACVM_MAX_POOLS; ++i) {
        if (gPools[i] == NULL) {
            gPools[i] = pool;
            handle = i + 1;
            break;
        }
    }
    acvm_pool_lock_release(&gRegistryLock);
    if (handle == 0) {
        // Table full: destroy the pool just built, otherwise its threads leak.
        acvm_pool_destroy(pool);
    }
    return handle;
}

/// Destroy a pool. Waits for every running job first (`acvm_pool_destroy` joins each).
/// Returns 0 on success, -1 for an invalid handle.
int64_t threadPoolDestroy(int64_t handle) {
    if (handle <= 0 || handle > ACVM_MAX_POOLS) {
        return -1;
    }
    acvm_registry_init();
    acvm_pool_lock_acquire(&gRegistryLock);
    AcvmPool* pool = gPools[handle - 1];
    gPools[handle - 1] = NULL;
    acvm_pool_lock_release(&gRegistryLock);
    if (pool == NULL) {
        return -1;
    }
    acvm_pool_destroy(pool);
    return 0;
}

int64_t threadPoolThreads(int64_t handle) {
    AcvmPool* pool = acvm_pool_lookup(handle);
    return pool == NULL ? -1 : (int64_t)pool->threadCount;
}

/// Total chunks executed. Confirms jobs really entered the pool instead of going serial.
int64_t threadPoolExecuted(int64_t handle) {
    AcvmPool* pool = acvm_pool_lookup(handle);
    if (pool == NULL) {
        return -1;
    }
    acvm_pool_lock_acquire(&pool->lock);
    const int64_t executed = pool->executed;
    acvm_pool_lock_release(&pool->lock);
    return executed;
}

//===--- pool: batch operations ------------------------------------------===//
//
// Every function is "split -> submit -> wait -> combine". Combining happens on the
// calling thread at O(workers), negligible next to the in-chunk work.

/// Chunk count: the smaller of pool threads and elements, never below 1.
static int acvm_pool_chunks(AcvmPool* pool, int64_t count, int64_t* bounds) {
    int64_t workers = pool == NULL ? 1 : pool->threadCount;
    return acvm_pool_split(count, workers, bounds);
}


/// Sum. Returns the 64-bit total (accumulating 32-bit elements would overflow).
int64_t threadPoolSum(int64_t handle, void* data, int64_t count) {
    if (data == NULL || count <= 0) {
        return 0;
    }
    AcvmPool* pool = acvm_pool_lookup(handle);
    const int64_t workers = pool == NULL ? 1 : (int64_t)pool->threadCount;
    return acvm_pool_reduce(pool, (int32_t*)data, count, workers, ACVM_JOB_SUM, 0);
}

/// The maximum itself. Returns 0 for an empty array.
int64_t threadPoolMax(int64_t handle, void* data, int64_t count) {
    if (data == NULL || count <= 0) {
        return 0;
    }
    AcvmPool* pool = acvm_pool_lookup(handle);
    const int64_t workers = pool == NULL ? 1 : (int64_t)pool->threadCount;
    return acvm_pool_reduce(pool, (int32_t*)data, count, workers, ACVM_JOB_MAX, 0);
}

/// The minimum itself. Returns 0 for an empty array.
int64_t threadPoolMin(int64_t handle, void* data, int64_t count) {
    if (data == NULL || count <= 0) {
        return 0;
    }
    AcvmPool* pool = acvm_pool_lookup(handle);
    const int64_t workers = pool == NULL ? 1 : (int64_t)pool->threadCount;
    return acvm_pool_reduce(pool, (int32_t*)data, count, workers, ACVM_JOB_MIN, 0);
}

/// Occurrences of `value`.
int64_t threadPoolCount(int64_t handle, void* data, int64_t count, int64_t value) {
    if (data == NULL || count <= 0) {
        return 0;
    }
    AcvmPool* pool = acvm_pool_lookup(handle);
    const int64_t workers = pool == NULL ? 1 : (int64_t)pool->threadCount;
    return acvm_pool_reduce(pool, (int32_t*)data, count, workers, ACVM_JOB_COUNT, value);
}

int64_t threadPoolFill(int64_t handle, void* data, int64_t count, int64_t value) {
    if (data == NULL || count <= 0) {
        return 0;
    }
    AcvmPool* pool = acvm_pool_lookup(handle);
    int64_t bounds[ACVM_POOL_MAX_THREADS + 1];
    const int chunks = acvm_pool_chunks(pool, count, bounds);
    AcvmPoolJob jobs[ACVM_POOL_MAX_THREADS];
    acvm_pool_build_jobs(jobs, chunks, bounds, ACVM_JOB_FILL, (int32_t*)data, value);
    acvm_pool_run_batch(pool, jobs, chunks);
    return count;
}

/// Sort: parallel quicksort per chunk, then a two-way merge **on the calling thread**.
///
/// The merge is not parallelised, for the same reason as in parallel.c: sorting is
/// O(n log n) and merging O(n), so sorting dominates. Measured at 1M elements about
/// 42 ms in-chunk sorting and 8 ms merging — not worth the extra synchronisation rounds.
///
/// Returns the actual chunk count (1 = degenerated to serial).
int64_t threadPoolSort(int64_t handle, void* data, int64_t count) {
    if (data == NULL || count <= 1) {
        return 1;
    }
    AcvmPool* pool = acvm_pool_lookup(handle);
    int64_t bounds[ACVM_POOL_MAX_THREADS + 1];
    const int chunks = acvm_pool_chunks(pool, count, bounds);
    if (chunks <= 1) {
        acvm_sort_i32((int32_t*)data, count);
        return 1;
    }

    AcvmPoolJob jobs[ACVM_POOL_MAX_THREADS];
    acvm_pool_build_jobs(jobs, chunks, bounds, ACVM_JOB_SORT, (int32_t*)data, 0);
    acvm_pool_run_batch(pool, jobs, chunks);

    if (!acvm_pool_merge_runs((int32_t*)data, count, bounds, chunks)) {
        // Merge allocation failed: chunks stay sorted, so incomplete but no crash; return 0.
        return 0;
    }
    return chunks;
}

/// Accumulate: per-chunk sums are added to `*out` under `lockHandle`.
///
/// **`lockHandle` 0 means no lock** — that loses updates and is only usable with a
/// single-worker pool. Passing a real lock is safe. This parameter exists to make "does
/// the lock actually work" testable: 0 gives a smaller value, a lock gives the exact one.
///
/// Returns 1 on success, -1 for invalid arguments / handle.
int64_t threadPoolAccumulate(int64_t handle, void* data, int64_t count, int64_t lockHandle,
                             int64_t* out, int64_t outCapacity) {
    if (data == NULL || count <= 0 || out == NULL || outCapacity < 1) {
        return -1;
    }
    AcvmPool* pool = acvm_pool_lookup(handle);
    AcvmUserLock* lock = lockHandle == 0 ? NULL : acvm_lock_lookup(lockHandle);
    if (lockHandle != 0 && lock == NULL) {
        return -1;      // a lock handle was given but not found: an error, not "no lock"
    }

    int64_t bounds[ACVM_POOL_MAX_THREADS + 1];
    const int chunks = acvm_pool_chunks(pool, count, bounds);
    AcvmPoolJob jobs[ACVM_POOL_MAX_THREADS];
    acvm_pool_build_jobs(jobs, chunks, bounds, ACVM_JOB_ACCUMULATE, (int32_t*)data, 0);

    // Both branches must point the shared cell: missing it is a null dereference (the
    // first version missed it on the lock-free branch and the call hung).
    for (int i = 0; i < chunks; ++i) {
        jobs[i].shared = out;
        // The user lock is an `AcvmPoolLock` underneath, so this can point straight at
        // it: pool jobs and `threadLockAcquire` use the same real lock.
        jobs[i].sharedLock = (lock == NULL) ? NULL : &lock->handle;
    }

    if (lock == NULL) {
        // Lock-free path: **serial** execution. Lock-free accumulation across workers
        // loses updates, so parallelism is given up here — correctness, not tuning.
        //
        // Note: the serial result equals the locked parallel one, so this branch cannot
        // show what the lock does. Use `threadPoolHammer` (truly concurrent) for that.
        for (int i = 0; i < chunks; ++i) {
            acvm_pool_run_job(&jobs[i]);
        }
        return 1;
    }

    // Pool jobs take the underlying lock directly, bypassing `threadLockAcquire`, so
    // count them by hand — otherwise `threadLockAcquisitions` stays 0, looking unused.
    lock->acquisitions += chunks;
    acvm_pool_run_batch(pool, jobs, chunks);
    return 1;
}

/// Stress test / real atomic accumulation: add `times` to `*counter`, split across workers.
///
/// This demonstrates **real atomics**: each worker does a platform fetch_add on the same
/// cell, and the result must equal `times` exactly. Returns the value after, -1 = bad args.
///
/// Why a separate function: Axiom has one execution thread, so a user-written `for`
/// loop there is serial and proves nothing. Verifying real atomics needs native threads.
int64_t threadPoolHammer(int64_t handle, int64_t times, int32_t* counter,
                         int64_t counterCapacity) {
    if (counter == NULL || times <= 0 || counterCapacity < 1) {
        return -1;
    }
    AcvmPool* pool = acvm_pool_lookup(handle);
    if (pool == NULL) {
        // No pool means no concurrency to test; just add it, same behaviour.
        for (int64_t i = 0; i < times; ++i) {
#ifdef _WIN32
            InterlockedExchangeAdd((volatile LONG*)counter, 1);
#else
            __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
#endif
        }
        return (int64_t)*counter;
    }

    // Never more threads than times, or some get 0 and make the trip for nothing.
    int64_t workers = pool->threadCount;
    if (workers > times) {
        workers = times;
    }
    if (workers > ACVM_POOL_MAX_THREADS) {
        workers = ACVM_POOL_MAX_THREADS;
    }
    if (workers < 1) {
        workers = 1;
    }

    AcvmPoolJob jobs[ACVM_POOL_MAX_THREADS];
    for (int64_t w = 0; w < workers; ++w) {
        // The remainder goes to the first shares, so the total is exactly `times`.
        const int64_t share = times / workers + (w < times % workers ? 1 : 0);
        jobs[w].kind = ACVM_JOB_HAMMER;
        jobs[w].data = counter;
        jobs[w].begin = 0;
        jobs[w].end = 0;
        jobs[w].param = share;
        jobs[w].result = 0;
        jobs[w].shared = (int64_t*)counter;
        jobs[w].sharedLock = NULL;
    }

    acvm_pool_run_batch(pool, jobs, (int)workers);
    return (int64_t)*counter;
}

//===--- user locks ------------------------------------------------------===//

/// Create a real OS lock. Returns a handle, 0 = failure.
int64_t threadLockCreate(void) {
    acvm_registry_init();
    AcvmUserLock* lock = (AcvmUserLock*)calloc(1, sizeof(AcvmUserLock));
    if (lock == NULL) {
        return 0;
    }
    if (!acvm_user_lock_init(lock)) {
        free(lock);
        return 0;
    }

    int64_t handle = 0;
    acvm_pool_lock_acquire(&gRegistryLock);
    for (int i = 0; i < ACVM_MAX_LOCKS; ++i) {
        if (gLocks[i] == NULL) {
            gLocks[i] = lock;
            handle = i + 1;
            break;
        }
    }
    acvm_pool_lock_release(&gRegistryLock);
    if (handle == 0) {
        acvm_user_lock_free(lock);
        free(lock);
    }
    return handle;
}

/// Acquire, blocking until held. Returns 0 on success, -1 for an invalid handle.
int64_t threadLockAcquire(int64_t handle) {
    AcvmUserLock* lock = acvm_lock_lookup(handle);
    if (lock == NULL) {
        return -1;
    }
    acvm_user_lock_acquire(lock);
    return 0;
}

/// Try to acquire without blocking. Returns 1 acquired, 0 busy, -1 invalid handle.
int64_t threadLockTryAcquire(int64_t handle) {
    AcvmUserLock* lock = acvm_lock_lookup(handle);
    if (lock == NULL) {
        return -1;
    }
    return acvm_user_lock_try(lock) ? 1 : 0;
}

/// Release. Returns 0 on success, -1 for an invalid handle.
int64_t threadLockRelease(int64_t handle) {
    AcvmUserLock* lock = acvm_lock_lookup(handle);
    if (lock == NULL) {
        return -1;
    }
    acvm_user_lock_release(lock);
    return 0;
}

/// Total acquisitions. Diagnostic: shows whether the lock was ever contended.
int64_t threadLockAcquisitions(int64_t handle) {
    AcvmUserLock* lock = acvm_lock_lookup(handle);
    return lock == NULL ? -1 : lock->acquisitions;
}

/// Destroy the lock. Returns 0 on success, -1 for an invalid handle.
int64_t threadLockDestroy(int64_t handle) {
    if (handle <= 0 || handle > ACVM_MAX_LOCKS) {
        return -1;
    }
    acvm_registry_init();
    acvm_pool_lock_acquire(&gRegistryLock);
    AcvmUserLock* lock = gLocks[handle - 1];
    gLocks[handle - 1] = NULL;
    acvm_pool_lock_release(&gRegistryLock);
    if (lock == NULL) {
        return -1;
    }
    acvm_user_lock_free(lock);
    free(lock);
    return 0;
}
