//===--- hash.c - the hash containers, as C ---------------------------------===//
//
// The four ASTL hash containers -- two integer-keyed sets, an integer-keyed map,
// and the string-keyed pair -- live here. `hash.ax` keeps the Axiom-facing names,
// the documented contract and the struct that holds the handle; everything that
// runs per operation is C.
//
// ── why the whole container and not just the probe ────────────────────────
//
// An earlier change moved only the probe loop and got 2.6x, from 3306 ms to
// 1253 ms. That left 33x behind CPython's `set`, and the remaining instruction
// profile was LOAD_LOCAL 21%, CONST_INT 18%, STORE_LOCAL 11% -- the cost of
// dispatching a container written in the language it is trying to speed up.
//
// Keeping the table in Axiom and calling C for the probe cannot fix that: the
// VM's address space is separately allocated 64 KB pages held in a map, so a
// pointer to a slot array is meaningless in C, and an array parameter is copied
// in and out around every call. Copying the table to probe it would be worse
// than interpreting it.
//
// So the table moves. What crosses the boundary is one handle.
//
// ── the handle is an int64, and that is deliberate ────────────────────────
//
// `ptr` parameters cannot cross into a native library without an element count,
// and the compiler is right to refuse them: the VM would pass an address that C
// cannot dereference. But memory this file allocates with `malloc` *is* in the
// process address space, so the restriction does not apply to it -- only to VM
// memory. Passing the address as `int64` says exactly that: "this is a number,
// and only the C side knows what it points at". It is the same workaround JNI
// uses with `jlong`, and it has the same hazard: a handle from one table type
// used with another is undefined, and nothing checks it.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//===--- shared -----------------------------------------------------------===//

#define ACVM_HASH_EMPTY 0
#define ACVM_HASH_USED 1
#define ACVM_HASH_TOMB 2
#define ACVM_HASH_MIN_SLOTS 8

//  SplitMix64's finalizer. The low bits are what the slot mask consumes, and the
//  finalizer avalanches both halves rather than only spreading the low ones.
static uint64_t acvm_hash_mix(uint64_t key) {
    uint64_t mixed = key;
    mixed ^= mixed >> 33;
    mixed *= UINT64_C(18397679294719823053);
    mixed ^= mixed >> 33;
    mixed *= UINT64_C(14181476777654086739);
    mixed ^= mixed >> 33;
    return mixed;
}

//  FNV-1a over the key's bytes, then the finalizer.
//
//  Content, not the descriptor address: two strings that are equal have to hash
//  equal, and they are separate allocations. FNV is used here rather than the
//  finalizer alone because the input is a byte sequence of unknown length -- the
//  finalizer wants eight bytes.
static uint64_t acvm_hash_bytes(const char* data, int64_t len) {
    uint32_t hash = 2166136261u;
    for (int64_t i = 0; i < len; ++i) {
        hash = (hash ^ (uint32_t)(unsigned char)data[i]) * 16777619u;
    }
    return acvm_hash_mix((uint64_t)hash);
}

//  Rounds up to a power of two, with a floor of eight. Returns 0 for a request
//  that is not positive, which makes the resulting table useless but valid.
static int32_t acvm_hash_slots_for(int32_t requested) {
    if (requested <= 0) {
        return 0;
    }
    int32_t slots = ACVM_HASH_MIN_SLOTS;
    while (slots < requested) {
        if (slots > INT32_MAX / 2) {
            return 0;   // the request cannot be served in this integer range
        }
        slots *= 2;
    }
    return slots;
}

//===--- integer-keyed table ----------------------------------------------===//

typedef struct {
    int64_t* keys;
    //  Null for a set. A set never reads a value, and the array was a third of the
    //  table's memory -- at 200 000 keys that is 2.8 MB of the 8.4 MB working set
    //  spent storing zeros. Cache misses, not instructions, are what remains of the
    //  gap to CPython here.
    int32_t* values;
    //  Three states fit in a byte, and the narrow array is read once per probe step:
    //  0.5 MB instead of 2.1 MB at 200 000 keys.
    uint8_t* states;
    int32_t slots;    // always a power of two, or 0 for an unusable table
    int32_t used;     // live keys
    int32_t filled;   // live keys plus tombstones
    //  Whether `values` was allocated. A set passes 0 to `create`.
    int hasValues;
} HashInt;

//  Allocates the three arrays zeroed. States must be zeroed before anything is
//  probed: a garbage state of USED would compare a garbage key, and a garbage
//  state of EMPTY would make a present key look absent.
static int hash_int_alloc(HashInt* t, int32_t slots, int withValues) {
    t->keys = (int64_t*)calloc((size_t)slots, sizeof(int64_t));
    t->values = withValues ? (int32_t*)calloc((size_t)slots, sizeof(int32_t)) : 0;
    t->states = (uint8_t*)calloc((size_t)slots, sizeof(uint8_t));
    if (t->keys == 0 || t->states == 0 || (withValues && t->values == 0)) {
        free(t->keys);
        free(t->values);
        free(t->states);
        t->keys = 0;
        t->values = 0;
        t->states = 0;
        t->slots = 0;
        return 0;
    }
    t->slots = slots;
    t->hasValues = withValues;
    return 1;
}

static void hash_int_release(HashInt* t) {
    free(t->keys);
    free(t->values);
    free(t->states);
    t->keys = 0;
    t->values = 0;
    t->states = 0;
    t->slots = 0;
    t->used = 0;
    t->filled = 0;
    t->hasValues = 0;
}

//  Finds `key`, or the slot it should occupy.
//
//  `*insertAt` receives the insertion point: the first tombstone met, or the
//  empty slot that ended the probe. A tombstone must not end the probe -- a key
//  stored past one is still reachable -- which is why the first one is
//  remembered rather than returned.
static int32_t hash_int_locate(const HashInt* t, int64_t key, int32_t* insertAt) {
    const int32_t slots = t->slots;
    int32_t index = (int32_t)(acvm_hash_mix((uint64_t)key) & (uint64_t)(slots - 1));
    int32_t firstTomb = -1;
    for (int32_t step = 0; step < slots; ++step) {
        const int32_t state = t->states[index];
        if (state == ACVM_HASH_EMPTY) {
            *insertAt = firstTomb >= 0 ? firstTomb : index;
            return -1;
        }
        if (state == ACVM_HASH_TOMB) {
            if (firstTomb < 0) {
                firstTomb = index;
            }
        } else if (t->keys[index] == key) {
            *insertAt = index;
            return index;
        }
        if (++index >= slots) {
            index = 0;
        }
    }
    *insertAt = firstTomb;
    return -1;
}

//  Doubles the table and reinserts the live keys; tombstones disappear.
static int hash_int_grow(HashInt* t, int32_t minSlots) {
    const int32_t target = acvm_hash_slots_for(minSlots);
    if (target <= t->slots) {
        return 1;
    }
    HashInt fresh;
    fresh.keys = 0;
    fresh.values = 0;
    fresh.states = 0;
    fresh.slots = 0;
    fresh.used = 0;
    fresh.filled = 0;
    if (!hash_int_alloc(&fresh, target, t->hasValues)) {
        return 0;
    }
    for (int32_t i = 0; i < t->slots; ++i) {
        if (t->states[i] != ACVM_HASH_USED) {
            continue;
        }
        int32_t at = -1;
        hash_int_locate(&fresh, t->keys[i], &at);
        if (at < 0) {
            hash_int_release(&fresh);
            return 0;
        }
        fresh.keys[at] = t->keys[i];
        if (fresh.hasValues) {
            fresh.values[at] = t->values[i];
        }
        fresh.states[at] = ACVM_HASH_USED;
        ++fresh.used;
        ++fresh.filled;
    }
    hash_int_release(t);
    *t = fresh;
    return 1;
}

//  A set: keys and states only.
int64_t hashIntCreate(int32_t requestedSlots) {
    const int32_t slots = acvm_hash_slots_for(requestedSlots);
    if (slots <= 0) {
        return 0;
    }
    HashInt* t = (HashInt*)calloc(1, sizeof(HashInt));
    if (t == 0) {
        return 0;
    }
    if (!hash_int_alloc(t, slots, 0)) {
        free(t);
        return 0;
    }
    return (int64_t)(uintptr_t)t;
}

//  A map: the same table plus a value array.
//
//  A separate entry point rather than a flag, because a set is the common case and the
//  whole point is not to allocate what it will never read.
int64_t hashIntCreateMap(int32_t requestedSlots) {
    const int32_t slots = acvm_hash_slots_for(requestedSlots);
    if (slots <= 0) {
        return 0;
    }
    HashInt* t = (HashInt*)calloc(1, sizeof(HashInt));
    if (t == 0) {
        return 0;
    }
    if (!hash_int_alloc(t, slots, 1)) {
        free(t);
        return 0;
    }
    return (int64_t)(uintptr_t)t;
}

void hashIntDestroy(int64_t handle) {
    HashInt* t = (HashInt*)(uintptr_t)handle;
    if (t == 0) {
        return;
    }
    hash_int_release(t);
    free(t);
}

//  Inserts or overwrites. Returns 1 on success, 0 when the table could not grow.
int32_t hashIntPut(int64_t handle, int64_t key, int32_t value) {
    HashInt* t = (HashInt*)(uintptr_t)handle;
    if (t == 0 || t->slots <= 0) {
        return 0;
    }
    // Grow at 75% occupancy, counting tombstones: they lengthen searches just as
    // live keys do, so a table that is mostly tombstones is still a slow table.
    if ((t->filled + 1) * 4 >= t->slots * 3) {
        if (!hash_int_grow(t, t->slots * 2)) {
            return 0;
        }
    }
    int32_t at = -1;
    const int32_t found = hash_int_locate(t, key, &at);
    if (found >= 0) {
        if (t->hasValues) {
            t->values[found] = value;
        }
        return 1;
    }
    if (at < 0) {
        return 0;   // every slot is a tombstone and none could be reused
    }
    const int32_t previous = t->states[at];
    t->keys[at] = key;
    if (t->hasValues) {
        t->values[at] = value;
    }
    t->states[at] = ACVM_HASH_USED;
    ++t->used;
    if (previous != ACVM_HASH_TOMB) {
        ++t->filled;
    }
    return 1;
}

//  Returns 1 when found. `out` may be null when only membership matters.
int32_t hashIntGet(int64_t handle, int64_t key, int32_t* out) {
    HashInt* t = (HashInt*)(uintptr_t)handle;
    if (t == 0 || t->slots <= 0) {
        return 0;
    }
    int32_t at = -1;
    const int32_t found = hash_int_locate(t, key, &at);
    if (found < 0) {
        return 0;
    }
    if (out != 0 && t->hasValues) {
        *out = t->values[found];
    }
    return 1;
}

//  Membership only -- no value, no out parameter. This is the shape a set wants,
//  and `HashMapInt`'s `has` too, since a `ptr` out parameter cannot cross this
//  boundary (the compiler refuses one with no element count, and the array form
//  would copy through a scratch buffer for four bytes).
int32_t hashIntContains(int64_t handle, int64_t key) {
    HashInt* t = (HashInt*)(uintptr_t)handle;
    if (t == 0 || t->slots <= 0) {
        return 0;
    }
    int32_t at = -1;
    return hash_int_locate(t, key, &at) >= 0 ? 1 : 0;
}

//  Lookup packed into one `int64`, for the same reason: bit 32 is the found
//  flag and bits 0-31 are the value's bit pattern.
//
//  Two results in one call rather than two calls, because a second probe to
//  fetch the value would double the cost of every successful lookup. The value
//  is returned as its 32-bit pattern rather than sign-extended, since signing it
//  would need bit 32 upwards and collide with the flag; the Axiom side widens it
//  back inside `int64` before narrowing to `int`, which is the only way to make
//  that conversion without trapping on a negative value.
int64_t hashIntTake(int64_t handle, int64_t key) {
    HashInt* t = (HashInt*)(uintptr_t)handle;
    if (t == 0 || t->slots <= 0) {
        return 0;
    }
    int32_t at = -1;
    const int32_t found = hash_int_locate(t, key, &at);
    if (found < 0) {
        return 0;
    }
    if (!t->hasValues) {
        return (int64_t)1 << 32;   // present, no value to report
    }
    return ((int64_t)1 << 32) | (int64_t)(uint32_t)t->values[found];
}

//  Returns 1 when it was present. Leaves a tombstone so the probe chain survives.
int32_t hashIntRemove(int64_t handle, int64_t key) {
    HashInt* t = (HashInt*)(uintptr_t)handle;
    if (t == 0 || t->slots <= 0) {
        return 0;
    }
    int32_t at = -1;
    const int32_t found = hash_int_locate(t, key, &at);
    if (found < 0) {
        return 0;
    }
    t->states[found] = ACVM_HASH_TOMB;
    --t->used;
    return 1;
}

int32_t hashIntCount(int64_t handle) {
    const HashInt* t = (const HashInt*)(uintptr_t)handle;
    return t == 0 ? 0 : t->used;
}

int32_t hashIntSlots(int64_t handle) {
    const HashInt* t = (const HashInt*)(uintptr_t)handle;
    return t == 0 ? 0 : t->slots;
}

//===--- string-keyed table -----------------------------------------------===//

//  A slot holds a copy of the key's bytes. The caller's `string` is a descriptor
//  into VM memory that is valid only for the duration of the call, so the table
//  cannot keep a pointer to it -- it owns a NUL-terminated copy instead.
//
//  Length is carried separately rather than found with `strlen`, because a key
//  may contain a zero byte and the two would then disagree about equality.
typedef struct {
    char* text;
    int64_t len;
} HashStrKey;

typedef struct {
    HashStrKey* keys;
    int32_t* values;
    int32_t* states;
    int32_t slots;
    int32_t used;
    int32_t filled;
} HashStr;

static int hash_str_key_set(HashStrKey* slot, const char* data, int64_t len) {
    char* copy = (char*)malloc((size_t)len + 1);
    if (copy == 0) {
        return 0;
    }
    if (len > 0) {
        memcpy(copy, data, (size_t)len);
    }
    copy[len] = '\0';
    free(slot->text);
    slot->text = copy;
    slot->len = len;
    return 1;
}

static int hash_str_alloc(HashStr* t, int32_t slots) {
    t->keys = (HashStrKey*)calloc((size_t)slots, sizeof(HashStrKey));
    t->values = (int32_t*)calloc((size_t)slots, sizeof(int32_t));
    t->states = (int32_t*)calloc((size_t)slots, sizeof(int32_t));
    if (t->keys == 0 || t->values == 0 || t->states == 0) {
        free(t->keys);
        free(t->values);
        free(t->states);
        t->keys = 0;
        t->values = 0;
        t->states = 0;
        t->slots = 0;
        return 0;
    }
    t->slots = slots;
    return 1;
}

static void hash_str_release(HashStr* t) {
    if (t->keys != 0) {
        for (int32_t i = 0; i < t->slots; ++i) {
            free(t->keys[i].text);
        }
    }
    free(t->keys);
    free(t->values);
    free(t->states);
    t->keys = 0;
    t->values = 0;
    t->states = 0;
    t->slots = 0;
    t->used = 0;
    t->filled = 0;
}

static int32_t hash_str_locate(const HashStr* t, const char* data, int64_t len,
                               int32_t* insertAt) {
    const int32_t slots = t->slots;
    int32_t index = (int32_t)(acvm_hash_bytes(data, len) & (uint64_t)(slots - 1));
    int32_t firstTomb = -1;
    for (int32_t step = 0; step < slots; ++step) {
        const int32_t state = t->states[index];
        if (state == ACVM_HASH_EMPTY) {
            *insertAt = firstTomb >= 0 ? firstTomb : index;
            return -1;
        }
        if (state == ACVM_HASH_TOMB) {
            if (firstTomb < 0) {
                firstTomb = index;
            }
        } else {
            const HashStrKey* stored = &t->keys[index];
            if (stored->len == len && (len == 0 || memcmp(stored->text, data, (size_t)len) == 0)) {
                *insertAt = index;
                return index;
            }
        }
        if (++index >= slots) {
            index = 0;
        }
    }
    *insertAt = firstTomb;
    return -1;
}

static int hash_str_grow(HashStr* t, int32_t minSlots) {
    const int32_t target = acvm_hash_slots_for(minSlots);
    if (target <= t->slots) {
        return 1;
    }
    HashStr fresh;
    fresh.keys = 0;
    fresh.values = 0;
    fresh.states = 0;
    fresh.slots = 0;
    fresh.used = 0;
    fresh.filled = 0;
    if (!hash_str_alloc(&fresh, target)) {
        return 0;
    }
    for (int32_t i = 0; i < t->slots; ++i) {
        if (t->states[i] != ACVM_HASH_USED) {
            continue;
        }
        HashStrKey* old = &t->keys[i];
        int32_t at = -1;
        hash_str_locate(&fresh, old->text, old->len, &at);
        if (at < 0) {
            hash_str_release(&fresh);
            return 0;
        }
        // Move the buffer rather than copy it: the old table is released
        // immediately after, and a copy would double the allocation count for no
        // benefit. Clearing the source also keeps the release from freeing a
        // buffer that now belongs to the new table.
        fresh.keys[at].text = old->text;
        fresh.keys[at].len = old->len;
        old->text = 0;
        old->len = 0;
        fresh.values[at] = t->values[i];
        fresh.states[at] = ACVM_HASH_USED;
        ++fresh.used;
        ++fresh.filled;
    }
    hash_str_release(t);
    *t = fresh;
    return 1;
}

int64_t hashStrCreate(int32_t requestedSlots) {
    const int32_t slots = acvm_hash_slots_for(requestedSlots);
    if (slots <= 0) {
        return 0;
    }
    HashStr* t = (HashStr*)calloc(1, sizeof(HashStr));
    if (t == 0) {
        return 0;
    }
    if (!hash_str_alloc(t, slots)) {
        free(t);
        return 0;
    }
    return (int64_t)(uintptr_t)t;
}

void hashStrDestroy(int64_t handle) {
    HashStr* t = (HashStr*)(uintptr_t)handle;
    if (t == 0) {
        return;
    }
    hash_str_release(t);
    free(t);
}

int32_t hashStrPut(int64_t handle, const char* data, int64_t len, int32_t value) {
    HashStr* t = (HashStr*)(uintptr_t)handle;
    if (t == 0 || t->slots <= 0) {
        return 0;
    }
    if ((t->filled + 1) * 4 >= t->slots * 3) {
        if (!hash_str_grow(t, t->slots * 2)) {
            return 0;
        }
    }
    int32_t at = -1;
    const int32_t found = hash_str_locate(t, data, len, &at);
    if (found >= 0) {
        t->values[found] = value;
        return 1;
    }
    if (at < 0) {
        return 0;
    }
    const int32_t previous = t->states[at];
    if (!hash_str_key_set(&t->keys[at], data, len)) {
        return 0;
    }
    t->values[at] = value;
    t->states[at] = ACVM_HASH_USED;
    ++t->used;
    if (previous != ACVM_HASH_TOMB) {
        ++t->filled;
    }
    return 1;
}

int32_t hashStrGet(int64_t handle, const char* data, int64_t len, int32_t* out) {
    HashStr* t = (HashStr*)(uintptr_t)handle;
    if (t == 0 || t->slots <= 0) {
        return 0;
    }
    int32_t at = -1;
    const int32_t found = hash_str_locate(t, data, len, &at);
    if (found < 0) {
        return 0;
    }
    if (out != 0) {
        *out = t->values[found];
    }
    return 1;
}

int32_t hashStrRemove(int64_t handle, const char* data, int64_t len) {
    HashStr* t = (HashStr*)(uintptr_t)handle;
    if (t == 0 || t->slots <= 0) {
        return 0;
    }
    int32_t at = -1;
    const int32_t found = hash_str_locate(t, data, len, &at);
    if (found < 0) {
        return 0;
    }
    // The key stays in the slot: a later probe that passes over this tombstone
    // still has to be able to compare against it, and clearing the text would
    // leave a tombstone that reads as an empty key.
    t->states[found] = ACVM_HASH_TOMB;
    --t->used;
    return 1;
}

int32_t hashStrContains(int64_t handle, const char* data, int64_t len) {
    HashStr* t = (HashStr*)(uintptr_t)handle;
    if (t == 0 || t->slots <= 0) {
        return 0;
    }
    int32_t at = -1;
    return hash_str_locate(t, data, len, &at) >= 0 ? 1 : 0;
}

int64_t hashStrTake(int64_t handle, const char* data, int64_t len) {
    HashStr* t = (HashStr*)(uintptr_t)handle;
    if (t == 0 || t->slots <= 0) {
        return 0;
    }
    int32_t at = -1;
    const int32_t found = hash_str_locate(t, data, len, &at);
    if (found < 0) {
        return 0;
    }
    return ((int64_t)1 << 32) | (int64_t)(uint32_t)t->values[found];
}

int32_t hashStrCount(int64_t handle) {
    const HashStr* t = (const HashStr*)(uintptr_t)handle;
    return t == 0 ? 0 : t->used;
}

int32_t hashStrSlots(int64_t handle) {
    const HashStr* t = (const HashStr*)(uintptr_t)handle;
    return t == 0 ? 0 : t->slots;
}
