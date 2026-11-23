//===--- IntRange.h - does a value fit this memory type? ----------------===//
//
// The VM needs this to decide whether an arithmetic result overflows, and the
// optimizer needs the exact same answer to decide whether it may fold a constant
// expression at compile time. Two copies of the rule would drift, and a drift
// here is silent: the fold would compute a value the VM would have rejected.
//
// It lives in Common rather than beside the VM for that reason -- both the VM
// and IR (the optimizer) depend on Common, and neither may depend on the other.

#pragma once

#include "acvm/Common/Value.h"

namespace acvm {

/// All-ones mask of `bits` bits, as an unsigned 128 bit value.
inline unsigned __int128 maskFor(unsigned bits) {
    if (bits >= 128) {
        return ~static_cast<unsigned __int128>(0);
    }
    return (static_cast<unsigned __int128>(1) << bits) - 1;
}

/// Lowest representable value of a `bits`-wide signed integer.
inline Int128 signedMinOf(unsigned bits) {
    if (bits >= 128) {
        // −2^127, the minimum of `Int128` itself. Written as −(2^127−1)−1 so the
        // intermediate stays representable: casting 2^127 straight to a signed
        // 128 bit value is an out-of-range conversion, and the result would depend
        // on how the compiler chooses to define it.
        return -static_cast<Int128>((static_cast<unsigned __int128>(1) << 127) - 1) - 1;
    }
    if (bits >= 127) {
        return -static_cast<Int128>(static_cast<unsigned __int128>(1) << 126);
    }
    return -static_cast<Int128>(static_cast<unsigned __int128>(1) << (bits - 1));
}

/// Highest representable value of a `bits`-wide signed integer.
inline Int128 signedMaxOf(unsigned bits) {
    if (bits >= 128) {
        // 2^127−1: every bit but the sign, which is exactly `Int128`'s maximum.
        return static_cast<Int128>((static_cast<unsigned __int128>(1) << 127) - 1);
    }
    if (bits >= 127) {
        return static_cast<Int128>((static_cast<unsigned __int128>(1) << 126) - 1);
    }
    return static_cast<Int128>((static_cast<unsigned __int128>(1) << (bits - 1)) - 1);
}

/// True when `value` fits a `bits`-wide integer of the given signedness. Anything
/// else is what the VM reports as an integer overflow trap.
///
/// Deliberately takes `bits` and `isUnsigned` rather than a `MemType`: the
/// optimizer, the VM and Sema all need this rule, and only two of the three have a
/// `MemType` at hand (Sema works with its own `Type`). The `MemType` convenience
/// overload lives in `acvm/IR/IntRangeMemType.h`.
inline bool inRangeOf(Int128 value, unsigned bits, bool isUnsigned) {
    if (isUnsigned) {
        return value >= 0 && static_cast<unsigned __int128>(value) <= maskFor(bits);
    }
    return value >= signedMinOf(bits) && value <= signedMaxOf(bits);
}

} // namespace acvm
