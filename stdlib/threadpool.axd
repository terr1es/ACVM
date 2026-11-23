//  threadpool.axd — a resident thread pool, plus locks for real threads.
//
//  `library "threadpool"` means these functions are in threadpool.dll / libthreadpool.so,
//  implemented in stdlib/threadpool.c.
//
//      function import (return:(threadpool));
//
//      int64 pool = threadPoolCreate(0);        // 0 = every core
//      parallel-ish: threadPoolSum(pool, #data[0], n)
//      threadPoolDestroy(pool);
//
//  ── how this divides work with parallel / sync ───────────────────────────
//
//  | module | what it does | threads |
//  |---|---|---|
//  | `sync` | cooperative concurrency (processes sharing state) | none, pure Axiom |
//  | `parallel` | one-off parallel compute, reuses a pool internally | hidden from caller |
//  | `threadpool` | **explicit** pool and real-lock management | caller creates / destroys |
//
//  When to use this instead of `parallel`:
//
//    * to **reuse** one pool across operations (`parallel` takes a pool per function too,
//      so this is not really a difference);
//    * to see **pool statistics** (chunks executed) and confirm parallelism really ran;
//    * to have native code change shared memory under a **real lock**
//      (`threadPoolAccumulate` + `threadLockCreate`);
//    * to **verify that real atomics work** (`threadPoolHammer`).
//
//  ── one architectural limit worth stating: no async submission ───────────
//
//  An array pointer Axiom passes to C points into the VM's **reused buffer**: valid
//  during the call, changed once it returns (the VM copies it back and reuses it).
//
//  So a task cannot outlive the call that submitted it — there is no "return a future,
//  collect later". Async would need the VM to hand buffer ownership to the native side,
//  which is a compiler-level change.
//
//  That is why every submit function **blocks until done**, not out of laziness.
//
//  ── handles ──────────────────────────────────────────────────────────────
//
//  Pools and locks both use `int64` handles. 0 = invalid — so a handle can be compared
//  with 0 safely, no nullable type. A handle is a table index + 1, not a pointer.

module "threadpool" version "0.1.0";

library "threadpool";

// ─── pool ─────────────────────────────────────────────────────────────────

//  Create a pool of `threads` workers. `threads <= 0` uses every core.
//  Returns a handle, 0 = failure (not a single thread started).
function threadPoolCreate(int threads):int64;

//  Destroy a pool. Waits for every running job first. Returns 0, or -1 for a bad handle.
function threadPoolDestroy(int64 pool):int;

//  Worker count. Returns -1 for an invalid handle.
function threadPoolThreads(int64 pool):int;

//  Total chunks executed. Use it to confirm jobs really entered the pool instead of
//  falling back to serial — a number that never grows means it is off. -1 = bad handle.
function threadPoolExecuted(int64 pool):int64;

// ─── batch operations on a pool ───────────────────────────────────────────
//
// All are "split -> dispatch -> wait -> combine", returning on the calling thread.
// Chunks share no state, which is where the parallelism comes from.

//  Sum, 64-bit total (accumulating 32-bit elements overflows). Empty array returns 0.
function threadPoolSum(int64 pool, ptr const int data, int count):int64;

//  The maximum itself. Empty array returns 0.
function threadPoolMax(int64 pool, ptr const int data, int count):int;

//  The minimum itself. Empty array returns 0.
function threadPoolMin(int64 pool, ptr const int data, int count):int;

//  Occurrences of `value`.
function threadPoolCount(int64 pool, ptr const int data, int count, int value):int64;

//  Fill with `value`. Returns the number filled.
function threadPoolFill(int64 pool, ptr int data, int count, int value):int;

//  In-place ascending sort: parallel quicksort per chunk + two-way merge.
//  Returns the chunk count (1 = serial), 0 = the merge failed to allocate (chunks stay sorted).
function threadPoolSort(int64 pool, ptr int data, int count):int;

// ─── accumulating under a real lock ───────────────────────────────────────
//
// Each chunk sums locally (no lock), then takes the lock only to **update the shared
// cell** — the hold time is independent of chunk size.
//
// `lock` 0 means **no locking**: that loses updates and is only usable with a single
// worker. The parameter exists to make "does the lock work" testable: 0 gives a smaller
// value, a lock gives the exact one.
//
// `out` is the `(ptr int64, int outCapacity)` pair form: a `ptr` must be followed by
// an element count, or the VM hands C a bare address (the compiler rejects that
// declaration). `outCapacity` must be at least 1; the function writes only `out[0]`.
//
// Returns 1 on success, -1 for invalid arguments or handle.
function threadPoolAccumulate(int64 pool, ptr int data, int count, int64 lock,
                              ptr int64 out, int outCapacity):int;

// ── real atomic stress test ──
//
// Add to `*counter` `times` times, split across workers using platform atomics. The
// return value is the count after — it must equal `times` exactly.
//
// Why a separate function: Axiom has one execution thread, so a `for` loop there is
// serial and proves nothing. Verifying atomics needs native threads fighting over one cell.
//
// Returns -1 for invalid arguments.
//
// **The parameter order must not become `(pool, ptr int counter, int times)`.** The
// marshalling rule is "a `ptr T` followed by an integer is an array pair", so `times`
// would be taken as the element count and `times * 4` bytes copied — a one-element
// counter becomes a 200,000-byte over-read. Putting `times` before `ptr` removes that.
function threadPoolHammer(int64 pool, int times, ptr int counter, int counterCapacity):int;

// ─── real OS locks ────────────────────────────────────────────────────────
//
// Not the same thing as `Mutex` in `sync.ax`:
//
//    sync.Mutex      cooperative. Locks Axiom code, released by yield, one thread only
//    threadLock*     real. A critical section on Windows, a pthread mutex on POSIX,
//                    excluding across native threads
//
// From pure Axiom, `threadLockAcquire` is just an atomic flag with a memory barrier
// (Axiom has one execution thread, so nothing contends). Its value is twofold: mutual
// exclusion inside pool jobs (`threadPoolAccumulate` does this) and for C extension authors.

//  Create a lock, returns a handle, 0 = failure.
function threadLockCreate():int64;

//  Acquire, blocking until held. Returns 0 on success, -1 for an invalid handle.
function threadLockAcquire(int64 lock):int;

//  Non-blocking try. Returns 1 acquired, 0 busy, -1 invalid handle.
function threadLockTryAcquire(int64 lock):int;

//  Release. Returns 0 on success, -1 for an invalid handle.
function threadLockRelease(int64 lock):int;

//  Total acquisitions. Diagnostic: shows whether the lock was ever really contended.
function threadLockAcquisitions(int64 lock):int64;

//  Destroy. Returns 0 on success, -1 for an invalid handle.
function threadLockDestroy(int64 lock):int;
