//  hash.axd — declarations for the hash module's native half.
//
//  `library "hash"` means these are implemented in hash.dll (Windows) /
//  libhash.so (Linux), built from stdlib/hash.c. That file is also compiled into
//  the VM itself, so the functions work whether or not the shared library was
//  built.
//
//  ── the handle ─────────────────────────────────────────────────────────────
//
//  Every function takes or returns an `int64` handle rather than a pointer. The
//  reason is that `ptr` parameters cannot cross this boundary: the compiler
//  refuses one with no element count, because the VM's memory is separately
//  allocated pages that native code cannot dereference.
//
//  Memory allocated by hash.c is a different matter -- it is `malloc`ed, so it
//  *is* in the process address space. The handle says exactly that: an opaque
//  number that only the C side interprets. A `ptr` would claim the VM could
//  address it, which is false in both directions.
//
//  A handle of 0 means the table could not be created. Every function tolerates
//  it and reports failure rather than trapping, which is what makes a set whose
//  allocation failed merely useless instead of fatal.
//
//  ── the string functions ───────────────────────────────────────────────────
//
//  A `string` parameter crosses as (pointer, length) into a scratch buffer that
//  is valid only for the duration of the call, so the table copies the bytes and
//  owns its copy. Length is passed explicitly rather than taken from a terminator,
//  because a key may contain a zero byte and the two would then disagree about
//  equality.

library "hash";

//  Creates a table sized for `requestedSlots`, rounded up to a power of two with
//  a floor of eight. Returns 0 when the request is not positive or allocation
//  failed.
function hashIntCreate(int requestedSlots):int64;

//  The same table plus a value array. A set does not allocate one: `values` was a
//  third of the working set and a set never reads it, which at 200 000 keys is the
//  difference between 8.4 MB and 5.6 MB of cache pressure.
function hashIntCreateMap(int requestedSlots):int64;
function hashIntDestroy(int64 handle):void;

//  Inserts or overwrites. Returns 1 on success, 0 when the table could not grow.
function hashIntPut(int64 handle, int64 key, int value):int;

//  Returns 1 when `key` is present. Membership without the value.
function hashIntContains(int64 handle, int64 key):int;

//  Lookup packed into one value: bit 32 is the found flag, bits 0-31 are the
//  value's bit pattern. Zero means absent, since a present key always sets bit 32.
//
//  Two results in one call: fetching the value with a second probe would double
//  the cost of every hit. `ptr int out` is not an option -- a `ptr` parameter
//  cannot cross this boundary without an element count, and the array form would
//  copy through a scratch buffer to move four bytes.
function hashIntTake(int64 handle, int64 key):int64;

//  Returns 1 when it was present. Leaves a tombstone.
function hashIntRemove(int64 handle, int64 key):int;
function hashIntCount(int64 handle):int;

//  Slot capacity. Diagnostic: a table whose count is flat while its slots keep
//  growing has accumulated tombstones.
function hashIntSlots(int64 handle):int;

function hashStrCreate(int requestedSlots):int64;
function hashStrDestroy(int64 handle):void;
function hashStrPut(int64 handle, string key, int value):int;
function hashStrContains(int64 handle, string key):int;

//  Lookup packed as `hashIntTake` packs it.
function hashStrTake(int64 handle, string key):int64;
function hashStrRemove(int64 handle, string key):int;
function hashStrCount(int64 handle):int;
function hashStrSlots(int64 handle):int;
