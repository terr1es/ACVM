//  random.c — pseudo-random numbers, shuffling and hashing.
//
//  Built into random.dll / librandom.so and reached from Axiom through
//  stdlib/random.axd. Nothing here is a compiler primitive: a program that never
//  imports `random` needs no library.
//
//  ── why the state is a caller buffer ──────────────────────────────────────
//
//  A generator is only useful if it remembers where it is, but this library holds
//  no globals: the VM runs several `process` functions and a shared seed would
//  make every run order-dependent. Instead the caller owns the state, exactly like
//  a C `rand_r`:
//
//      unsigned int seed[1] = {12345u32};
//      unsigned int r = randNext(#seed[0], seed.length);
//
//  The buffer must hold at least one element. A zero seed is replaced with the
//  golden-ratio constant, because xorshift from an all-zero state is the one fixed
//  point that never moves.
//
//  ── the generator ─────────────────────────────────────────────────────────
//
//  xorshift32 (Marsaglia, 2003): three shifts and three xors, period 2^32-1, and
//  no multiply -- which matters because it is the only generator Axiom's fallback
//  can reproduce exactly. A 64 bit generator needs wrapping 64 bit multiplication,
//  and Axiom traps on signed overflow, so the two implementations could not agree.
//
//  Quality is adequate for shuffling, sampling and test data. It is NOT suitable
//  for anything cryptographic, and the 32 bit period means a program consuming
//  more than four billion values repeats itself.
//
//  ── the one function with no Axiom fallback ───────────────────────────────
//
//  `randomSecure` reads the operating system's entropy source. It is the only
//  function here that cannot be written in Axiom at all -- a language cannot
//  evaluate a syscall, and without one there is no way to obtain unpredictability.
//  So it has no fallback: on a build without random.dll the call reports failure
//  (`-1`) instead of quietly substituting the PRNG. A silent downgrade from secure
//  to predictable is the one failure mode that must never happen here.

#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/// One step of xorshift32. `*s` must be non-zero; a zero is replaced rather than
/// looping forever on the fixed point.
static uint32_t acvm_xorshift32(uint32_t* s) {
    uint32_t x = *s;
    if (x == 0) {
        x = 0x9E3779B9u;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/// Unbiased value in [0, bound) by rejection: `2^32 mod bound` values would
/// otherwise skew the result towards the low end. Returns 0 when `bound` is not
/// positive, which the caller is expected to check.
static uint32_t acvm_rand_below(uint32_t* s, uint64_t bound) {
    if (bound == 0) {
        return 0;
    }
    const uint32_t threshold = (uint32_t)(0x100000000ull % bound);
    for (;;) {
        const uint32_t r = acvm_xorshift32(s);
        if (r >= threshold) {
            return (uint32_t)(r % bound);
        }
    }
}

int64_t randNext(void* state, int64_t stateCapacity) {
    if (state == 0 || stateCapacity < 1) {
        return 0;
    }
    return (int64_t)acvm_xorshift32((uint32_t*)state);
}

int64_t randBelow(void* state, int64_t stateCapacity, int64_t bound) {
    if (state == 0 || stateCapacity < 1 || bound <= 0) {
        return 0;
    }
    return (int64_t)acvm_rand_below((uint32_t*)state, (uint64_t)bound);
}

int64_t randRange(void* state, int64_t stateCapacity, int64_t low, int64_t high) {
    if (state == 0 || stateCapacity < 1) {
        return low;
    }
    if (high <= low) {
        return low;   // an empty range has one sensible answer
    }
    const uint64_t span = (uint64_t)(high - low);
    return low + (int64_t)acvm_rand_below((uint32_t*)state, span);
}

int64_t shuffle(void* data, int64_t count, void* state, int64_t stateCapacity) {
    if (data == 0 || count <= 1 || state == 0 || stateCapacity < 1) {
        return 0;
    }
    int32_t* a = (int32_t*)data;
    uint32_t* s = (uint32_t*)state;
    // Fisher-Yates, walking down: every permutation is equally likely, and the
    // result is decided by exactly count-1 draws.
    for (int64_t i = count - 1; i > 0; --i) {
        const int64_t j = (int64_t)acvm_rand_below(s, (uint64_t)i + 1);
        const int32_t tmp = a[i];
        a[i] = a[j];
        a[j] = tmp;
    }
    return 0;
}

/// FNV-1a over the *values* rather than the bytes: Axiom's `char` is a UTF-16 code
/// unit, so a byte view of an `int` array is not something the fallback can build.
/// Mixing one element per round is enough to build hash tables and fingerprints.
int64_t hashInts(void* data, int64_t count) {
    uint32_t h = 2166136261u;   // FNV offset basis
    if (data != 0 && count > 0) {
        const int32_t* a = (const int32_t*)data;
        for (int64_t i = 0; i < count; ++i) {
            h ^= (uint32_t)a[i];
            h *= 16777619u;     // FNV prime
        }
    }
    // Zero-extended on purpose: the Axiom declaration says `unsigned int`, and a
    // sign-extending return would disagree with it for every hash above 2^31.
    return (int64_t)h;
}

//===--- OS entropy -------------------------------------------------------===//

/// Fills `buffer` with `count` bytes from the operating system.
///
/// Returns 0 on success, -1 when the platform refuses. Callers must treat -1 as a
/// hard failure: there is no acceptable substitute for real entropy.
///
/// Windows uses `BCryptGenRandom` with the system-preferred RNG, which is the
/// documented interface since Vista and draws from the same pool as
/// `RtlGenRandom`. POSIX uses `getrandom` when the C library has it (glibc 2.25+,
/// no file descriptor to leak, and it blocks until the pool is seeded) and falls
/// back to `/dev/urandom` otherwise. The fallback is not a quality downgrade on
/// any current system; it exists because `getrandom` is not universal.
int64_t randomSecure(void* buffer, int64_t count) {
    if (count < 0) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (buffer == 0) {
        return -1;
    }

#ifdef _WIN32
    // BCryptGenRandom takes a ULONG length, so a request above 4 GB is split.
    uint8_t* at = (uint8_t*)buffer;
    int64_t left = count;
    while (left > 0) {
        const ULONG chunk = left > 0x40000000 ? 0x40000000UL : (ULONG)left;
        if (BCryptGenRandom(NULL, at, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return -1;
        }
        at += chunk;
        left -= (int64_t)chunk;
    }
    return 0;
#else
    uint8_t* at = (uint8_t*)buffer;
    int64_t left = count;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
    while (left > 0) {
        const ssize_t got = getrandom(at, (size_t)left, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        at += got;
        left -= (int64_t)got;
    }
    return 0;
#else
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    while (left > 0) {
        const ssize_t got = read(fd, at, (size_t)left);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (got == 0) {
            close(fd);
            return -1;
        }
        at += got;
        left -= (int64_t)got;
    }
    close(fd);
    return 0;
#endif
#endif
}
