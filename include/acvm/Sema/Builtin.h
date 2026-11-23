//===--- Builtin.h - Intrinsics and library functions known to the VM ----===//
#pragma once

#include <cstddef>
#include <cstdint>

namespace acvm {

/// Every name the compiler knows how to call without user code.
/// `Sema` maps resolved names onto these ids, `IRGen` emits `OpCode::Builtin`
/// and the VM implements them.
enum class Builtin : std::int32_t {
    None = -1,

    // §8.1 memory intrinsics
    Alloc = 0,
    Alloc0,
    Free,
    Memcpy,
    Memmove,
    Dup,
    MakeString,
    Yield,
    Trap,

    // §8.2 output
    Print,
    EPrint,
    Log,

    // §8.3 input
    ReadLine,
    ReadToken,
    /// Command-line arguments the host passed to the program, after `--`.
    ArgCount,
    ArgAt,
    ReadInt,
    ReadFloat,

    // §8.4 / §8.5 conversion
    Itoa,
    Ftoa,
    Atoi,
    Atof,

    // §8.6 string operations
    IndexOf,
    LastIndexOf,
    Substring,
    StartsWith,
    EndsWith,
    Trim,
    ToUpper,
    ToLower,

    // §8.8 libm
    Sqrt,
    Fabs,
    Floor,
    Ceil,
    Round,
    Trunc,
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    Atan2,
    Exp,
    LogE,
    Log2,
    Log10,
    Pow,
    Fmod,
    Min,
    Max,
    Abs,

    // §8.9
    Assert,

    // Native helpers, implemented in C (native/algo.c). Not part of the
    // language spec: ACVM ships them so programs can reach native speed for
    // whole-array work, which an interpreter cannot.
    SortInts,
    SortInt64s,
    SortFloats,
    ReverseInts,
};

const char* builtinName(Builtin builtin);

struct BuiltinEntry {
    const char* name;
    Builtin id;
};

/// The whole table, in declaration order; used by Sema to declare the names.
const BuiltinEntry* builtinTable(std::size_t& count);

/// True for the intrinsics that need no import and cannot be shadowed.
bool isIntrinsicName(const char* name);

} // namespace acvm
