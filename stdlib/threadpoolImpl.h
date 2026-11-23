//===--- threadpoolImpl.h - the pool engine shared by two modules --------===//
//
// Not a public header. Included by threadpool.c (which exposes the pool to Axiom)
// and by parallel.c (which uses it so its reductions stop paying thread creation
// per call). Every function is `static`, so each translation unit gets its own
// copy -- that is deliberate: the two modules ship as separate shared objects and
// there is no way for one to link against the other's internals.
//
// ── the one architectural fact this design is built around ─────────────────
//
// A pointer that crosses the Axiom-to-C boundary points into a buffer the VM owns
// and **reuses**. The VM copies an array in, calls the function, copies it back;
// after the call returns the buffer holds something else. So a job may not outlive
// the call that submitted it.
//
// That rules out the usual "submit and return a future" thread pool. What is
// possible -- and what this file implements -- is a pool whose **threads persist**
// while every call still blocks until its own work is done:
//
//      submit a batch of chunks  →  workers pick them up  →  block until all done
//
// The win is not overlap with Axiom code (impossible, see above); it is that the
// workers are already alive. Windows thread creation plus teardown measures at
// 50-100 µs per thread on this machine, so a 12-thread call paid close to a
// millisecond before doing any arithmetic -- enough to make "parallel" slower than
// serial on an 8 MB reduction.
//
// ── job kinds ─────────────────────────────────────────────────────────────
//
// Axiom has no function pointers and the C side cannot receive an Axiom function,
// so a job cannot be "run this callback". Each job carries a kind from a fixed
// set and the worker dispatches on it. Adding an operation means adding a kind
// here plus a wrapper in the module that exposes it.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

/// Silences "defined but not used" for functions a translation unit does not call.
/// Both users include this file but use different subsets.
#if defined(__GNUC__) || defined(__clang__)
#define ACVM_POOL_UNUSED __attribute__((unused))
#else
#define ACVM_POOL_UNUSED
#endif

//===--- hardware --------------------------------------------------------===//

/// Hardware concurrency. Conservatively returns 1 when it cannot be read.
static ACVM_POOL_UNUSED int64_t acvm_pool_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (int64_t)info.dwNumberOfProcessors;
#else
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int64_t)count : 1;
#endif
}

//===--- locking primitives ----------------------------------------------===//

#ifdef _WIN32
typedef CRITICAL_SECTION AcvmPoolLock;

static ACVM_POOL_UNUSED void acvm_pool_lock_init(AcvmPoolLock* lock) {
    InitializeCriticalSection(lock);
}

static ACVM_POOL_UNUSED void acvm_pool_lock_destroy(AcvmPoolLock* lock) {
    DeleteCriticalSection(lock);
}

static ACVM_POOL_UNUSED void acvm_pool_lock_acquire(AcvmPoolLock* lock) {
    EnterCriticalSection(lock);
}

static ACVM_POOL_UNUSED void acvm_pool_lock_release(AcvmPoolLock* lock) {
    LeaveCriticalSection(lock);
}

typedef CONDITION_VARIABLE AcvmPoolCond;

static ACVM_POOL_UNUSED void acvm_pool_cond_init(AcvmPoolCond* cond) {
    InitializeConditionVariable(cond);
}

static ACVM_POOL_UNUSED void acvm_pool_cond_destroy(AcvmPoolCond* cond) {
    (void)cond;   // condition variables own no resources on Windows
}

static ACVM_POOL_UNUSED void acvm_pool_cond_wait(AcvmPoolCond* cond, AcvmPoolLock* lock) {
    SleepConditionVariableCS(cond, lock, INFINITE);
}

/// Bounded wait: at most `ms` milliseconds; 0 means it timed out.
static ACVM_POOL_UNUSED int acvm_pool_cond_wait_ms(AcvmPoolCond* cond, AcvmPoolLock* lock,
                                                   unsigned long ms) {
    return SleepConditionVariableCS(cond, lock, ms) != 0;
}

/// Sleep the current thread for `ms` milliseconds. One version per platform: the
/// worker's idle poll needs it, and `Sleep` is Windows-only.
static ACVM_POOL_UNUSED void acvm_pool_sleep_ms(unsigned long ms) {
    Sleep((DWORD)ms);
}

static ACVM_POOL_UNUSED void acvm_pool_cond_broadcast(AcvmPoolCond* cond) {
    WakeAllConditionVariable(cond);
}

typedef struct AcvmPoolThread {
    HANDLE handle;
    struct AcvmPool* pool;
} AcvmPoolThread;

static ACVM_POOL_UNUSED DWORD WINAPI acvm_pool_thread_entry(LPVOID raw);

static ACVM_POOL_UNUSED int acvm_pool_thread_start(AcvmPoolThread* thread, void* pool) {
    thread->pool = (struct AcvmPool*)pool;
    thread->handle = CreateThread(NULL, 0, acvm_pool_thread_entry, thread, 0, NULL);
    return thread->handle != NULL;
}

/// Reclaim thread resources. 1 = the thread terminated, 0 = it was still running.
///
/// **Do not use its return value to decide whether the pool's memory can be freed** —
/// `liveWorkers` decides that (see `acvm_pool_destroy`). Here we only wait for the thread
/// to really exit (closing the "out of the loop, still in the lock" window), then close it.
static ACVM_POOL_UNUSED int acvm_pool_thread_join(AcvmPoolThread* thread) {
    if (thread->handle == NULL) {
        return 1;
    }
    const DWORD waited = WaitForSingleObject(thread->handle, 50);
    CloseHandle(thread->handle);
    thread->handle = NULL;
    return waited != WAIT_TIMEOUT;
}

#else

typedef pthread_mutex_t AcvmPoolLock;

static ACVM_POOL_UNUSED void acvm_pool_lock_init(AcvmPoolLock* lock) {
    pthread_mutex_init(lock, NULL);
}

static ACVM_POOL_UNUSED void acvm_pool_lock_destroy(AcvmPoolLock* lock) {
    pthread_mutex_destroy(lock);
}

static ACVM_POOL_UNUSED void acvm_pool_lock_acquire(AcvmPoolLock* lock) {
    pthread_mutex_lock(lock);
}

static ACVM_POOL_UNUSED void acvm_pool_lock_release(AcvmPoolLock* lock) {
    pthread_mutex_unlock(lock);
}

typedef pthread_cond_t AcvmPoolCond;

static ACVM_POOL_UNUSED void acvm_pool_cond_init(AcvmPoolCond* cond) {
    pthread_cond_init(cond, NULL);
}

static ACVM_POOL_UNUSED void acvm_pool_cond_destroy(AcvmPoolCond* cond) {
    pthread_cond_destroy(cond);
}

static ACVM_POOL_UNUSED void acvm_pool_cond_wait(AcvmPoolCond* cond, AcvmPoolLock* lock) {
    pthread_cond_wait(cond, lock);
}

/// Bounded wait: at most `ms` milliseconds; 0 means it timed out.
///
/// `pthread_cond_timedwait` wants an **absolute** time, not a duration, so read the
/// current time and add `ms`. `CLOCK_REALTIME` because condition variables default to the
/// realtime clock; `CLOCK_MONOTONIC` needs `pthread_condattr_setclock`, changing every
/// init point. The cost: a forward clock jump wakes the wait early — callers re-check
/// their condition in a loop, so an early wake just spins once more.
static ACVM_POOL_UNUSED int acvm_pool_cond_wait_ms(AcvmPoolCond* cond, AcvmPoolLock* lock,
                                                   unsigned long ms) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return 1;   // no clock: wait without a timeout, old behaviour rather than failing
    }
    deadline.tv_sec += (time_t)(ms / 1000u);
    deadline.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cond, lock, &deadline) != ETIMEDOUT;
}

/// Sleep the current thread for `ms` milliseconds. One version per platform: the
/// worker's idle poll needs it, and `Sleep` is Windows-only.
static ACVM_POOL_UNUSED void acvm_pool_sleep_ms(unsigned long ms) {
    struct timespec span;
    span.tv_sec = (time_t)(ms / 1000u);
    span.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&span, NULL);
}

static ACVM_POOL_UNUSED void acvm_pool_cond_broadcast(AcvmPoolCond* cond) {
    pthread_cond_broadcast(cond);
}

typedef struct AcvmPoolThread {
    pthread_t handle;
    int started;
    struct AcvmPool* pool;
} AcvmPoolThread;

static ACVM_POOL_UNUSED void* acvm_pool_thread_entry(void* raw);

static ACVM_POOL_UNUSED int acvm_pool_thread_start(AcvmPoolThread* thread, void* pool) {
    thread->pool = (struct AcvmPool*)pool;
    thread->started = pthread_create(&thread->handle, NULL, acvm_pool_thread_entry, thread) == 0;
    return thread->started;
}

/// Reclaim thread resources. 1 = the thread exited, 0 = it was still running at timeout.
///
/// **Do not use its return value to decide whether the pool's memory can be freed** —
/// `liveWorkers` decides that (see `acvm_pool_destroy`). Here we only wait for the thread
/// to really exit (closing the "out of the loop, still in the lock" window), then free.
///
/// POSIX `pthread_join` has no timeout (`pthread_timedjoin_np` needs `_GNU_SOURCE`), so
/// it blocks on POSIX. That is safe only because every call site has already confirmed
/// `liveWorkers == 0`, when termination is all that is left and the call returns at once.
static ACVM_POOL_UNUSED int acvm_pool_thread_join(AcvmPoolThread* thread) {
    if (thread->started) {
        pthread_join(thread->handle, NULL);
        thread->started = 0;
    }
    return 1;
}

#endif

//===--- job kinds -------------------------------------------------------===//

enum AcvmJobKind {
    ACVM_JOB_SUM = 0,       ///< sum the chunk
    ACVM_JOB_MAX = 1,       ///< maximum of the chunk
    ACVM_JOB_MIN = 2,       ///< minimum of the chunk
    ACVM_JOB_COUNT = 3,     ///< how often `param` appears in the chunk
    ACVM_JOB_FILL = 4,      ///< write `param` into the chunk
    ACVM_JOB_SORT = 5,      ///< sort the chunk
    ACVM_JOB_ACCUMULATE = 6,///< chunk sum, added into `shared` under `sharedLock`
    ACVM_JOB_HAMMER = 7     ///< `param` real atomic adds to `shared` (stress test)
};

typedef struct AcvmPoolJob {
    int kind;
    int32_t* data;
    int64_t begin;
    int64_t end;
    int64_t param;
    int64_t result;
    /// Only `ACVM_JOB_ACCUMULATE`: the shared cell and the real lock guarding it.
    int64_t* shared;
    AcvmPoolLock* sharedLock;
} AcvmPoolJob;

//===--- sorting ---------------------------------------------------------===//
//
// The per-chunk sort is `acvm_sort_i32` from sortImpl.h -- the same quicksort
// algorithm.c instantiates for its own element types. This file used to carry a
// hand-written copy; keeping one source means a change to the partition scheme
// cannot land in one and miss the other.
//
// `static`, so parallel.c and threadpool.c each get their own copy with no
// linker interaction between the two shared objects.

#include "sortImpl.h"

ACVM_DEFINE_SORT(i32, int32_t)

/// Merge `runs` individually sorted ranges into one; `bounds[0..runs]` are the cut points.
/// Returns 1 on success, 0 when allocation failed (ranges stay sorted, just not merged).
///
/// It must be a two-way merge. Picking the minimum linearly each round costs O(n × runs):
/// 1M elements × 12 runs is 12M comparisons, eating the gain — measured, slower than serial.
static ACVM_POOL_UNUSED int acvm_pool_merge_runs(int32_t* values, int64_t count,
                                                const int64_t* bounds, int64_t runs) {
    if (runs <= 1 || values == NULL || count <= 0) {
        return 1;
    }
    int32_t* scratch = (int32_t*)malloc(sizeof(int32_t) * (size_t)count);
    int64_t* starts = (int64_t*)malloc(sizeof(int64_t) * ((size_t)runs + 1));
    int64_t* nextStarts = (int64_t*)malloc(sizeof(int64_t) * ((size_t)runs + 1));
    if (scratch == NULL || starts == NULL || nextStarts == NULL) {
        free(scratch);
        free(starts);
        free(nextStarts);
        return 0;
    }
    for (int64_t r = 0; r <= runs; ++r) {
        starts[r] = bounds[r];
    }

    int32_t* source = values;
    int32_t* target = scratch;
    int64_t active = runs;
    while (active > 1) {
        int64_t produced = 0;
        int64_t writeSlot = 1;
        nextStarts[0] = 0;
        int64_t block = 0;
        while (block < active) {
            const int64_t leftBegin = starts[block];
            const int64_t leftEnd = starts[block + 1];
            if (block + 1 >= active) {
                // An odd run left over: already sorted, just copy it.
                memcpy(target + leftBegin, source + leftBegin,
                       sizeof(int32_t) * (size_t)(leftEnd - leftBegin));
                block += 1;
                produced += 1;
                nextStarts[writeSlot++] = leftEnd;
                continue;
            }
            const int64_t rightBegin = starts[block + 1];
            const int64_t rightEnd = starts[block + 2];
            int64_t i = leftBegin;
            int64_t j = rightBegin;
            int64_t k = leftBegin;
            while (i < leftEnd && j < rightEnd) {
                // `<=` keeps equal elements in left-then-right order, so it is stable.
                target[k++] = (source[i] <= source[j]) ? source[i++] : source[j++];
            }
            while (i < leftEnd) { target[k++] = source[i++]; }
            while (j < rightEnd) { target[k++] = source[j++]; }
            block += 2;
            produced += 1;
            nextStarts[writeSlot++] = rightEnd;
        }

        // Ping-pong swap.
        int32_t* swap = source;
        source = target;
        target = swap;
        for (int64_t r = 0; r <= produced; ++r) {
            starts[r] = nextStarts[r];
        }
        active = produced;
    }

    if (source != values) {
        memcpy(values, source, sizeof(int32_t) * (size_t)count);
    }
    free(starts);
    free(nextStarts);
    free(scratch);
    return 1;
}

//===--- the pool --------------------------------------------------------===//

/// Cap on the thread count. A pool above this is pointless (cores are far fewer), but
/// the cap gives a fixed array size.
#define ACVM_POOL_MAX_THREADS 64
/// Queue depth. A call submits at most `min(threads, 64)` chunks, so 64 suffices; doubled.
#define ACVM_POOL_QUEUE 128

struct AcvmPool;

typedef struct AcvmPool {
    AcvmPoolLock lock;
    AcvmPoolCond work;      ///< new work / shutdown wanted
    AcvmPoolCond done;      ///< outstanding reached zero
    /// The queue stores **pointers** into the submitter's job array.
    ///
    /// Pointers rather than copies because results must be written back into the
    /// submitter's structs; with copies a worker writes its own copy and the caller
    /// never sees it. Safe because batch submission blocks until all chunks finish.
    AcvmPoolJob* queue[ACVM_POOL_QUEUE];
    /// Workers re-read these while **waiting with the lock released**, so they must be
    /// volatile: else the compiler hoists the reads out of the loop and a worker never
    /// sees `shutdown = 1`, so `join` waits forever — or worse, the DLL unloads first.
    volatile int queued;
    volatile int head;
    /// Jobs submitted but not finished (queued + running).
    volatile int outstanding;
    volatile int shutdown;
    /// Threads still inside the worker loop; each decrements it under the lock on exit.
    ///
    /// This is the **authority** on whether the pool's memory can be freed, not the
    /// return value of `WaitForSingleObject(thread)`. The reason is the concrete question:
    /// will any thread still read `pool`'s fields? Whether the OS thread object has
    /// terminated is only a proxy — and we have measured the odd case "all workers left
    /// the loop yet the waiter still timed out", and never established who was wrong.
    /// The question is "who still reads my fields", so let the readers keep the books.
    volatile int liveWorkers;
    int threadCount;
    int64_t executed;       ///< total chunks executed, diagnostic
    AcvmPoolThread threads[ACVM_POOL_MAX_THREADS];
} AcvmPool;

/// Run one chunk. **Called outside the lock** — that is where the parallelism comes
/// from; anything that holds the lock here degenerates to serial.
static ACVM_POOL_UNUSED void acvm_pool_run_job(AcvmPoolJob* job) {
    switch (job->kind) {
    case ACVM_JOB_SUM: {
        int64_t total = 0;
        for (int64_t i = job->begin; i < job->end; ++i) {
            total += (int64_t)job->data[i];
        }
        job->result = total;
        break;
    }
    case ACVM_JOB_MAX: {
        int32_t best = job->data[job->begin];
        for (int64_t i = job->begin + 1; i < job->end; ++i) {
            if (job->data[i] > best) { best = job->data[i]; }
        }
        job->result = (int64_t)best;
        break;
    }
    case ACVM_JOB_MIN: {
        int32_t best = job->data[job->begin];
        for (int64_t i = job->begin + 1; i < job->end; ++i) {
            if (job->data[i] < best) { best = job->data[i]; }
        }
        job->result = (int64_t)best;
        break;
    }
    case ACVM_JOB_COUNT: {
        const int32_t target = (int32_t)job->param;
        int64_t found = 0;
        for (int64_t i = job->begin; i < job->end; ++i) {
            found += job->data[i] == target;
        }
        job->result = found;
        break;
    }
    case ACVM_JOB_FILL: {
        const int32_t value = (int32_t)job->param;
        for (int64_t i = job->begin; i < job->end; ++i) {
            job->data[i] = value;
        }
        job->result = job->end - job->begin;
        break;
    }
    case ACVM_JOB_SORT: {
        acvm_sort_i32(job->data + job->begin, job->end - job->begin);
        job->result = job->end - job->begin;
        break;
    }
    case ACVM_JOB_ACCUMULATE: {
        // Sum over the own chunk first — no lock needed. Then take the lock only to
        // **update the shared cell**, so the hold time is independent of chunk size. The
        // critical section is one read-modify-write, the one nice thing about this.
        int64_t total = 0;
        for (int64_t i = job->begin; i < job->end; ++i) {
            total += (int64_t)job->data[i];
        }
        if (job->sharedLock != NULL) {
            acvm_pool_lock_acquire(job->sharedLock);
            *job->shared = *job->shared + total;
            acvm_pool_lock_release(job->sharedLock);
        } else {
            // No lock: safe only with a single worker. Lock-free accumulation across
            // workers loses updates — that is the caller's choice, not ours to save.
            *job->shared = *job->shared + total;
        }
        job->result = total;
        break;
    }
    case ACVM_JOB_HAMMER: {
        // Real atomic add: several workers fight over one cell. This is the only way to
        // verify atomics — Axiom has one thread, so a loop there proves nothing.
        int32_t* cell = (int32_t*)job->shared;
        for (int64_t i = 0; i < job->param; ++i) {
#ifdef _WIN32
            InterlockedExchangeAdd((volatile LONG*)cell, 1);
#else
            __atomic_fetch_add(cell, 1, __ATOMIC_SEQ_CST);
#endif
        }
        job->result = job->param;
        break;
    }
    default:
        job->result = 0;
        break;
    }
}

static ACVM_POOL_UNUSED void acvm_pool_worker_loop(void* raw) {
    AcvmPool* pool = (AcvmPool*)raw;
    for (;;) {
        acvm_pool_lock_acquire(&pool->lock);

        /* Idle wait: condition variable plus a 1 ms timeout.
           Both are needed —
             * INFINITE alone loses wake-ups: one lost wake-up strands a worker after
               destroy, `join` never sees it, and unloading the DLL jumps it into freed
               code pages (measured: `access=execute` into MEM_FREE).
             * `sleep(1)` polling alone is too slow: after the destroyer broadcasts it
               waits for a natural wake, 1-2 ms extra per destroy; `threadpool.c` builds
               and destroys per call, piling up to seconds (measured 2.4 s → 4.2 s).
           The 1 ms timeout wakes the normal path at once, the odd one within 1 ms. */
        while (pool->queued == 0 && !pool->shutdown) {
            acvm_pool_cond_wait_ms(&pool->work, &pool->lock, 1);
        }
        if (pool->queued == 0 && pool->shutdown) {
            /* Under the lock: the destroyer reads it under the same lock to decide the free. */
            pool->liveWorkers -= 1;
            acvm_pool_lock_release(&pool->lock);
            return;
        }

        AcvmPoolJob* job = pool->queue[pool->head];
        pool->head = (pool->head + 1) % ACVM_POOL_QUEUE;
        pool->queued -= 1;
        acvm_pool_lock_release(&pool->lock);

        // Run outside the lock: chunks share no state (except the cell ACCUMULATE
        // guards explicitly), which is where the parallelism comes from.
        if (job != NULL) {
            acvm_pool_run_job(job);
        }

        acvm_pool_lock_acquire(&pool->lock);
        pool->executed += 1;
        pool->outstanding -= 1;
        // Broadcast every time, not just on zero: it is issued under the lock and waiters
        // re-check under the same lock, so it cannot be lost. Broadcast-on-zero works
        // too, but this rules out "the caller was not yet waiting at the zero instant".
        acvm_pool_cond_broadcast(&pool->done);
        acvm_pool_lock_release(&pool->lock);
    }
}

#ifdef _WIN32
static ACVM_POOL_UNUSED DWORD WINAPI acvm_pool_thread_entry(LPVOID raw) {
    AcvmPoolThread* thread = (AcvmPoolThread*)raw;
    acvm_pool_worker_loop(thread->pool);
    return 0;
}
#else
static ACVM_POOL_UNUSED void* acvm_pool_thread_entry(void* raw) {
    AcvmPoolThread* thread = (AcvmPoolThread*)raw;
    acvm_pool_worker_loop(thread->pool);
    return NULL;
}
#endif

/// Create a pool. `threads <= 0` uses every core. NULL means not one thread started, so
/// the caller falls back to single-threaded instead of reporting an error.
static ACVM_POOL_UNUSED AcvmPool* acvm_pool_create(int64_t threads) {
    if (threads <= 0) {
        threads = acvm_pool_cpu_count();
    }
    if (threads > ACVM_POOL_MAX_THREADS) {
        threads = ACVM_POOL_MAX_THREADS;
    }
    if (threads < 1) {
        threads = 1;
    }

    AcvmPool* pool = (AcvmPool*)calloc(1, sizeof(AcvmPool));
    if (pool == NULL) {
        return NULL;
    }
    acvm_pool_lock_init(&pool->lock);
    acvm_pool_cond_init(&pool->work);
    acvm_pool_cond_init(&pool->done);

    int started = 0;
    for (int i = 0; i < (int)threads; ++i) {
        if (!acvm_pool_thread_start(&pool->threads[i], pool)) {
            break;
        }
        ++started;
    }
    if (started == 0) {
        acvm_pool_lock_destroy(&pool->lock);
        acvm_pool_cond_destroy(&pool->work);
        acvm_pool_cond_destroy(&pool->done);
        free(pool);
        return NULL;
    }
    pool->threadCount = started;
    pool->liveWorkers = started;
    return pool;
}

/// Destroy a pool: tell the workers to exit, wait them out of the loop, free resources.
/// NULL is fine. Contract: **no batch may be in flight** — both call paths satisfy it:
/// `threadpool.c` destroys only after `acvm_pool_run_batch` returns, and
/// `acvm_stdlib_shutdown` runs during VM teardown.
static ACVM_POOL_UNUSED void acvm_pool_destroy(AcvmPool* pool) {
    if (pool == NULL) {
        return;
    }
    acvm_pool_lock_acquire(&pool->lock);
    pool->shutdown = 1;
    /* Clear the queue: its job pointers point into the caller's stack array
       (`acvm_pool_reduce`'s locals), and those addresses are dead after destroy. A
       worker still picking them up would read and write a dead stack frame.

       `outstanding` must be zeroed too. Clearing only `queued` leaves a contradictory
       state: queue empty, yet N jobs shown in flight, so a waiter spins forever in
       `while (outstanding != 0)` (1 ms per turn, endlessly). By the contract above it
       is already 0; clearing it makes a "broken contract" call return early instead of
       hanging — a doubtful result beats a stuck process. */
    pool->queued = 0;
    pool->head = 0;
    pool->outstanding = 0;
    acvm_pool_cond_broadcast(&pool->work);
    acvm_pool_lock_release(&pool->lock);

    /* Wait for the workers to leave the loop themselves. A worker sits in
       `acvm_pool_sleep_ms(1)`, so it sees `shutdown` within 1 ms; polling at 1 ms
       means a total wait of 1-2 ms. Coarser would hurt: `threadpool.c` builds and
       destroys per call, so thousands of cycles would accumulate 5 ms each into
       seconds (measured 2.4 s → 4.7 s). The 1 s cap is for a machine so busy that a
       thread cannot get a slot. Only `liveWorkers` is read here — see that field. */
    int allGone = 0;
    for (unsigned long waited = 0; waited <= 1000; waited += 1) {
        acvm_pool_lock_acquire(&pool->lock);
        const int live = pool->liveWorkers;
        acvm_pool_lock_release(&pool->lock);
        if (live == 0) {
            allGone = 1;
            break;
        }
        acvm_pool_sleep_ms(1);
    }

    /* Close the handle / pthread_join. The workers are already out of the loop, so
       this is immediate; it closes the "left the loop, still in the lock" window. */
    int allJoined = 1;
    for (int i = 0; i < pool->threadCount; ++i) {
        if (!acvm_pool_thread_join(&pool->threads[i])) {
            allJoined = 0;
        }
    }
    if (!allGone || !allJoined) {
        /* A worker is still in the loop. Destroying the lock and `free(pool)` would
           make its next field read jump into freed memory — exactly how `access=execute`
           into MEM_FREE happens. Leaking one pool beats pulling memory from under it. */
        fprintf(stderr,
                "[acvm] warning: a pool worker did not stop; leaking the pool "
                "(live=%d joined=%d)\n", (int)pool->liveWorkers, allJoined);
        fflush(stderr);
        return;
    }
    acvm_pool_lock_destroy(&pool->lock);
    acvm_pool_cond_destroy(&pool->work);
    acvm_pool_cond_destroy(&pool->done);
    free(pool);
}

/// Hand `jobs[0..count)` to the pool and **block until all finish**; results land in
/// each job's `result` (the caller's array, not a copy). Returns jobs submitted; NULL pool = 0.
///
/// Why block: pointers are valid only during the call, so an async write lands in someone
/// else's data (see the header); no "returns a future" version exists, or is needed.
static ACVM_POOL_UNUSED int acvm_pool_run_batch(AcvmPool* pool, AcvmPoolJob* jobs, int count) {
    if (jobs == NULL || count <= 0) {
        return 0;
    }
    if (count > ACVM_POOL_QUEUE) {
        count = ACVM_POOL_QUEUE;
    }
    if (pool == NULL) {
        // No pool: run serially in place. Same result, just not parallel.
        for (int i = 0; i < count; ++i) {
            acvm_pool_run_job(&jobs[i]);
        }
        return count;
    }

    acvm_pool_lock_acquire(&pool->lock);
    pool->outstanding = count;
    for (int i = 0; i < count; ++i) {
        pool->queue[(pool->head + pool->queued) % ACVM_POOL_QUEUE] = &jobs[i];
        pool->queued += 1;
    }
    acvm_pool_cond_broadcast(&pool->work);

    // The wait condition must be re-checked **under the lock**, and a worker must
    // broadcast after every decrement of outstanding. `while (outstanding > 0) wait()`
    // alone loses wake-ups: if it reaches 0 before the caller enters wait, nobody takes
    // the broadcast and the caller waits forever.
    /* `!= 0` rather than `> 0`: a count pushed below zero would let `> 0` pass at once
       and the main thread return with unclaimed jobs queued. A 1 ms timed condition
       variable beats INFINITE: normal path wakes at once, a lost wake-up costs 1 ms. */
    while (pool->outstanding != 0) {
        acvm_pool_cond_wait_ms(&pool->done, &pool->lock, 1);
    }
    acvm_pool_lock_release(&pool->lock);
    return count;
}

/// Split an array into `workers` chunks, filling `bounds[0..workers]` with the cut
/// points. The remainder goes to the first chunks, so nothing is missed or doubled.
static ACVM_POOL_UNUSED int acvm_pool_split(int64_t count, int64_t workers, int64_t* bounds) {
    if (count <= 0 || workers <= 0) {
        return 0;
    }
    if (workers > count) {
        workers = count;
    }
    if (workers > ACVM_POOL_MAX_THREADS) {
        workers = ACVM_POOL_MAX_THREADS;
    }
    int64_t begin = 0;
    bounds[0] = 0;
    for (int64_t w = 0; w < workers; ++w) {
        const int64_t length = count / workers + (w < count % workers ? 1 : 0);
        begin += length;
        bounds[w + 1] = begin;
    }
    return (int)workers;
}

/// Fill a batch of same-kind jobs for the chunks cut by `bounds`. Returns the chunk count.
///
/// The table filling used to exist three times: once in parallel.c, once in
/// threadpool.c, and once inlined in `acvm_pool_reduce`. If any one missed a field
/// (say `param` for COUNT) it answered wrongly in silence, looking identical.
static ACVM_POOL_UNUSED int acvm_pool_build_jobs(AcvmPoolJob* jobs, int chunks,
                                                 const int64_t* bounds, int kind,
                                                 int32_t* data, int64_t param) {
    for (int i = 0; i < chunks; ++i) {
        jobs[i].kind = kind;
        jobs[i].data = data;
        jobs[i].begin = bounds[i];
        jobs[i].end = bounds[i + 1];
        jobs[i].param = param;
        jobs[i].result = 0;
        jobs[i].shared = NULL;
        jobs[i].sharedLock = NULL;
    }
    return chunks;
}

/// Keep one complete reduction in one place: split -> build jobs -> dispatch -> combine.
///
/// The skeleton of the four reductions (sum / max / min / count) was written four times
/// each in parallel.c and threadpool.c — eight copies of "split, fill jobs, run, merge
/// by kind". If any one missed a field (say `param` for COUNT) it answered wrongly in
/// silence, and they all looked the same, so nothing showed.
///
/// `kind` picks the combining: SUM and COUNT add, MAX / MIN take the extreme. Combining
/// happens on the calling thread at O(chunks), negligible next to the in-chunk work.
///
/// `param` matters only for `ACVM_JOB_COUNT` (the value to find); pass 0 otherwise.
///
/// Returning 0 for an empty array is deliberate: there is no "no maximum" value, so
/// callers that need to tell them apart check `count` first.
static ACVM_POOL_UNUSED int64_t acvm_pool_reduce(AcvmPool* pool, int32_t* data,
                                                 int64_t count, int64_t workers,
                                                 int kind, int64_t param) {
    if (data == NULL || count <= 0) {
        return 0;
    }
    int64_t bounds[ACVM_POOL_MAX_THREADS + 1];
    const int chunks = acvm_pool_split(count, workers, bounds);
    if (chunks <= 0) {
        return 0;
    }

    AcvmPoolJob jobs[ACVM_POOL_MAX_THREADS];
    acvm_pool_build_jobs(jobs, chunks, bounds, kind, data, param);
    acvm_pool_run_batch(pool, jobs, chunks);

    if (kind == ACVM_JOB_SUM || kind == ACVM_JOB_COUNT) {
        int64_t total = 0;
        for (int i = 0; i < chunks; ++i) {
            total += jobs[i].result;
        }
        return total;
    }

    // MAX / MIN: start from the first chunk's result.
    int64_t best = jobs[0].result;
    for (int i = 1; i < chunks; ++i) {
        if (kind == ACVM_JOB_MAX ? jobs[i].result > best : jobs[i].result < best) {
            best = jobs[i].result;
        }
    }
    return best;
}
