//  stdlib.c — the shared-library half of the Axiom standard library.
//
//  This file is NOT part of the compiler. It builds into stdlib.dll /
//  libstdlib.so, which Axiom loads at run time through stdlib/stddec.axd:
//
//      library "stdlib";                                   // stddec.axd
//      function nativeSort(ptr int data, int64 count):int64;
//
//  Build it (Windows):
//      clang -shared -O2 -o stdlib.dll stdlib/stdlib.c
//  Build it (Linux/macOS):
//      clang -shared -fPIC -O2 -o libstdlib.so stdlib/stdlib.c
//
//  CMake does this automatically as the `acvm_stdlib` target, so a normal build
//  produces the library next to acvm.exe.
//
//  Contract with the VM (the reasoning is in this header):
//    * C linkage on every exported function -- no name mangling;
//    * arguments and the result travel in one 64 bit general purpose register,
//      so no scalar `double` (that would need an SSE register);
//    * an array arrives as (pointer to a contiguous copy, element count); write
//      through the pointer to modify the caller's array, the VM copies it back;
//    * return 0 for success, non-zero for a failure.
//
//  Axiom's own compiler primitives (sortInts, sortFloats, reverseInts) live in
//  stdlib/algorithm.c and are linked into acvm.exe instead -- those need no
//  library. This file is the other mechanism.

#include <stdint.h>
#include <stdlib.h>

// ─── arrays of int ─────────────────────────────────────────────────────────

static int compareAscending(const void* lhs, const void* rhs) {
    const int32_t a = *(const int32_t*)lhs;
    const int32_t b = *(const int32_t*)rhs;
    return (a > b) - (a < b);
}

static int compareDescending(const void* lhs, const void* rhs) {
    const int32_t a = *(const int32_t*)lhs;
    const int32_t b = *(const int32_t*)rhs;
    return (a < b) - (a > b);
}

//  qsort is deliberate here. These functions exist to demonstrate the dynamic
//  boundary, not to be the fastest sort in the project -- for that, see
//  stdlib/algorithm.c, which is compiled into the interpreter itself.
int64_t nativeSort(void* data, int64_t count) {
    if (data == 0 || count <= 1) {
        return 0;
    }
    qsort(data, (size_t)count, sizeof(int32_t), compareAscending);
    return 0;
}

int64_t nativeSortDesc(void* data, int64_t count) {
    if (data == 0 || count <= 1) {
        return 0;
    }
    qsort(data, (size_t)count, sizeof(int32_t), compareDescending);
    return 0;
}

int64_t nativeReverse(void* data, int64_t count) {
    if (data == 0 || count <= 1) {
        return 0;
    }
    int32_t* values = (int32_t*)data;
    for (int64_t i = 0, j = count - 1; i < j; ++i, --j) {
        const int32_t tmp = values[i];
        values[i] = values[j];
        values[j] = tmp;
    }
    return 0;
}

int64_t nativeSum(void* data, int64_t count) {
    if (data == 0 || count <= 0) {
        return 0;
    }
    const int32_t* values = (const int32_t*)data;
    int64_t total = 0;
    for (int64_t i = 0; i < count; ++i) {
        total += values[i];
    }
    return total;
}

int64_t nativeMax(void* data, int64_t count) {
    if (data == 0 || count <= 0) {
        return 0;
    }
    const int32_t* values = (const int32_t*)data;
    int32_t best = values[0];
    for (int64_t i = 1; i < count; ++i) {
        if (values[i] > best) {
            best = values[i];
        }
    }
    return (int64_t)best;
}

int64_t nativeMin(void* data, int64_t count) {
    if (data == 0 || count <= 0) {
        return 0;
    }
    const int32_t* values = (const int32_t*)data;
    int32_t best = values[0];
    for (int64_t i = 1; i < count; ++i) {
        if (values[i] < best) {
            best = values[i];
        }
    }
    return (int64_t)best;
}

int64_t nativeCount(void* data, int64_t count, int64_t value) {
    if (data == 0 || count <= 0) {
        return 0;
    }
    const int32_t* values = (const int32_t*)data;
    const int32_t wanted = (int32_t)value;
    int64_t found = 0;
    for (int64_t i = 0; i < count; ++i) {
        if (values[i] == wanted) {
            ++found;
        }
    }
    return found;
}

// ─── scalars ───────────────────────────────────────────────────────────────

int64_t nativeClamp(int64_t value, int64_t low, int64_t high) {
    if (low > high) {
        // Report the mistake rather than silently swapping: a caller that mixed
        // up the bounds wants to know, and 0 is a value it can check.
        return 0;
    }
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

int64_t nativeGcd(int64_t a, int64_t b) {
    if (a < 0) {
        a = -a;
    }
    if (b < 0) {
        b = -b;
    }
    while (b != 0) {
        const int64_t remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}
