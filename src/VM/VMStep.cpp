//===--- VMStep.cpp - The instruction dispatch loop --------------------===//
#include "acvm/VM/VM.h"

#include "acvm/Sema/Builtin.h"
#include "VMInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace acvm {
using namespace vminternal;
namespace {

/// Executes one arithmetic / bitwise / comparison instruction.
/// Address-like operands arrive either as `Ptr` (a real pointer value) or as
/// `Int` (a scaled offset produced by pointer arithmetic).
std::uint64_t addressOf(const Value& value) {
    if (value.tag() == Value::Tag::Ptr || value.tag() == Value::Tag::Addr) {
        return value.asPtr();
    }
    return static_cast<std::uint64_t>(value.asInt());
}

bool computeBinary(OpCode op, const MemType& type, const Value& lhs, const Value& rhs,
                   Value& out, TrapKind& trap) {
    if (type.isFloat()) {
        const double a = lhs.asFloat();
        const double b = rhs.asFloat();
        switch (op) {
        case OpCode::Add: out = Value::fromFloat(a + b); return true;
        case OpCode::Sub: out = Value::fromFloat(a - b); return true;
        case OpCode::Mul: out = Value::fromFloat(a * b); return true;
        case OpCode::Div: out = Value::fromFloat(a / b); return true;
        case OpCode::Mod: out = Value::fromFloat(std::fmod(a, b)); return true;
        case OpCode::Eq: out = Value::fromInt(a == b ? 1 : 0); return true;
        case OpCode::Ne: out = Value::fromInt(a != b ? 1 : 0); return true;
        case OpCode::Lt: out = Value::fromInt(a < b ? 1 : 0); return true;
        case OpCode::Le: out = Value::fromInt(a <= b ? 1 : 0); return true;
        case OpCode::Gt: out = Value::fromInt(a > b ? 1 : 0); return true;
        case OpCode::Ge: out = Value::fromInt(a >= b ? 1 : 0); return true;
        default:
            trap = TrapKind::Unreachable;
            return false;
        }
    }

    if (type.isPtr()) {
        const std::uint64_t a = addressOf(lhs);
        const std::uint64_t b = addressOf(rhs);
        switch (op) {
        case OpCode::Add: out = Value::fromPtr(a + b); return true;
        case OpCode::Sub:
            // Pointer difference is signed: `p - q` where q > p must be a
            // small negative number, not 2^64 minus the distance.
            out = Value::fromInt(static_cast<Int128>(static_cast<std::int64_t>(a - b)));
            return true;
        case OpCode::Eq: out = Value::fromInt(a == b ? 1 : 0); return true;
        case OpCode::Ne: out = Value::fromInt(a != b ? 1 : 0); return true;
        case OpCode::Lt: out = Value::fromInt(a < b ? 1 : 0); return true;
        case OpCode::Le: out = Value::fromInt(a <= b ? 1 : 0); return true;
        case OpCode::Gt: out = Value::fromInt(a > b ? 1 : 0); return true;
        case OpCode::Ge: out = Value::fromInt(a >= b ? 1 : 0); return true;
        default:
            trap = TrapKind::Unreachable;
            return false;
        }
    }

    const bool isUnsigned = type.isUnsigned || type.kind == MemType::Kind::Bool ||
                            type.kind == MemType::Kind::Char;
    const unsigned bits = type.bits == 0 ? 32 : type.bits;

    // Fast path for the widths programs actually use: signed 32 and 64 bit integers.
    //
    // The generic path below is written for 128 bits because that is the widest type the
    // language has, and it stays correct at every width: it masks both operands, computes
    // in 128 bits, masks again, and range-checks the result. For an `int` that is a dozen
    // instructions to produce a 32 bit sum, and integer work is roughly a fifth of all
    // retirement on an ordinary program.
    //
    // At a native width that ceremony is unnecessary, but the equivalence has to be
    // argued rather than assumed, which is what the guard below is for. The generic path
    // accepts operands wider than the declared type -- it computes in 128 bits and only
    // checks the result -- so truncating an operand here would silently change the answer
    // for such a value. Requiring both operands to already fit the declared width makes
    // this path a strict special case: same inputs, same arithmetic, same result.
    //
    // The guard is four predictable comparisons that never fire in a well-typed program,
    // and it costs under 1% -- cheap insurance against the two paths drifting apart.
    if (!isUnsigned && type.kind == MemType::Kind::Int && (bits == 32 || bits == 64)) {
        const Int128 rawA = lhs.asInt();
        const Int128 rawB = rhs.asInt();
        const Int128 narrowMin = bits == 32 ? INT32_MIN : INT64_MIN;
        const Int128 narrowMax = bits == 32 ? INT32_MAX : INT64_MAX;

        if (rawA >= narrowMin && rawA <= narrowMax && rawB >= narrowMin && rawB <= narrowMax) {
            const std::int64_t a = static_cast<std::int64_t>(rawA);
            const std::int64_t b = static_cast<std::int64_t>(rawB);
            // The int64 result of any operation on two values that fit the declared width
            // is exact, so a 32 bit type only needs the final bound test.
            const auto fits = [bits](std::int64_t r) {
                return bits == 64 || (r >= INT32_MIN && r <= INT32_MAX);
            };
            const auto overflow = [&] {
                trap = TrapKind::IntegerOverflow;
                return false;
            };

            std::int64_t result = 0;
            bool defer = false;
            switch (op) {
            case OpCode::Eq: out = Value::fromInt(a == b ? 1 : 0); return true;
            case OpCode::Ne: out = Value::fromInt(a != b ? 1 : 0); return true;
            case OpCode::Lt: out = Value::fromInt(a < b ? 1 : 0); return true;
            case OpCode::Le: out = Value::fromInt(a <= b ? 1 : 0); return true;
            case OpCode::Gt: out = Value::fromInt(a > b ? 1 : 0); return true;
            case OpCode::Ge: out = Value::fromInt(a >= b ? 1 : 0); return true;
            case OpCode::Add:
                if (__builtin_add_overflow(a, b, &result)) {
                    return overflow();
                }
                break;
            case OpCode::Sub:
                if (__builtin_sub_overflow(a, b, &result)) {
                    return overflow();
                }
                break;
            case OpCode::Mul:
                if (__builtin_mul_overflow(a, b, &result)) {
                    return overflow();
                }
                break;
            case OpCode::Div:
                if (b == 0) {
                    trap = TrapKind::DivisionByZero;
                    return false;
                }
                if (a == narrowMin && b == -1) {
                    return overflow();
                }
                result = a / b;
                break;
            case OpCode::Mod:
                if (b == 0) {
                    trap = TrapKind::DivisionByZero;
                    return false;
                }
                // `min % -1` is 0 mathematically; returning it directly keeps the hardware
                // divide from ever seeing the one input that traps.
                result = b == -1 ? 0 : a % b;
                break;
            // Bitwise results of two values with a common sign extension stay inside it,
            // so no bound test is needed -- except that the generic path checks anyway.
            case OpCode::And:
                result = a & b;
                break;
            case OpCode::Or:
                result = a | b;
                break;
            case OpCode::Xor:
                result = a ^ b;
                break;
            case OpCode::Shl:
                // Deliberately not handled here. The generic path shifts the operand as
                // *unsigned* and rejects any bit that leaves the width, so `-1 << 31`
                // traps there -- a negative operand shifts out high bits that the signed
                // form would have kept. Reproducing that would mean doing the same
                // unsigned 128 bit shift the generic path already does, so this op simply
                // falls through to it. Shifts are rare enough that the fast path is not
                // worth reconciling the two readings of a sign bit for.
                defer = true;
                break;

            case OpCode::Shr:
                if (b < 0 || b >= static_cast<std::int64_t>(bits)) {
                    result = a < 0 ? -1 : 0;
                    break;
                }
                result = a >> b;
                break;
            default:
                // Not an operation this path handles: fall through to the generic code,
                // so there is exactly one implementation of it.
                defer = true;
                break;
            }
            if (!defer) {
                if (!fits(result)) {
                    return overflow();
                }
                out = Value::fromInt(static_cast<Int128>(result));
                return true;
            }
        }
    }

    if (isUnsigned) {
        const unsigned __int128 a = static_cast<unsigned __int128>(lhs.asInt()) & maskFor(bits);
        const unsigned __int128 b = static_cast<unsigned __int128>(rhs.asInt()) & maskFor(bits);
        unsigned __int128 result = 0;
        switch (op) {
        case OpCode::Add: result = a + b; break;
        case OpCode::Sub: result = a - b; break;
        case OpCode::Mul: result = a * b; break;
        case OpCode::Div:
            if (b == 0) {
                trap = TrapKind::DivisionByZero;
                return false;
            }
            result = a / b;
            break;
        case OpCode::Mod:
            if (b == 0) {
                trap = TrapKind::DivisionByZero;
                return false;
            }
            result = a % b;
            break;
        case OpCode::Pow: {
            // Unsigned arithmetic wraps in this VM, and the mask at the end of this
            // branch does exactly that, so the products here are allowed to wrap too.
            // There is no negative exponent to reject: an unsigned exponent is never
            // negative, and one written as -1 arrives as 2^bits-1, which takes the
            // same ~log2(exponent) steps as any other value rather than looping.
            unsigned __int128 accumulator = 1;
            unsigned __int128 base = a;
            unsigned __int128 exponent = b;
            while (exponent > 0) {
                if ((exponent & 1) != 0) {
                    accumulator *= base;
                }
                exponent >>= 1;
                if (exponent > 0) {
                    base *= base;
                }
            }
            result = accumulator;
            break;
        }
        case OpCode::And: result = a & b; break;
        case OpCode::Or: result = a | b; break;
        case OpCode::Xor: result = a ^ b; break;
        case OpCode::Shl:
            result = b >= bits ? 0 : (a << static_cast<unsigned>(b));
            break;
        case OpCode::Shr:
            result = b >= bits ? 0 : (a >> static_cast<unsigned>(b));
            break;
        case OpCode::Eq: out = Value::fromInt(a == b ? 1 : 0); return true;
        case OpCode::Ne: out = Value::fromInt(a != b ? 1 : 0); return true;
        case OpCode::Lt: out = Value::fromInt(a < b ? 1 : 0); return true;
        case OpCode::Le: out = Value::fromInt(a <= b ? 1 : 0); return true;
        case OpCode::Gt: out = Value::fromInt(a > b ? 1 : 0); return true;
        case OpCode::Ge: out = Value::fromInt(a >= b ? 1 : 0); return true;
        default:
            trap = TrapKind::Unreachable;
            return false;
        }
        result &= maskFor(bits);
        out = Value::fromInt(static_cast<Int128>(result));
        return true;
    }

    const Int128 a = lhs.asInt();
    const Int128 b = rhs.asInt();
    switch (op) {
    case OpCode::Eq: out = Value::fromInt(a == b ? 1 : 0); return true;
    case OpCode::Ne: out = Value::fromInt(a != b ? 1 : 0); return true;
    case OpCode::Lt: out = Value::fromInt(a < b ? 1 : 0); return true;
    case OpCode::Le: out = Value::fromInt(a <= b ? 1 : 0); return true;
    case OpCode::Gt: out = Value::fromInt(a > b ? 1 : 0); return true;
    case OpCode::Ge: out = Value::fromInt(a >= b ? 1 : 0); return true;
    default:
        break;
    }

    Int128 result = 0;
    switch (op) {
    case OpCode::Add:
        // Overflow-checked rather than a bare `a + b`. For every width below 128
        // the operands are bounded well inside `Int128`, so the sum cannot leave
        // the type and this is the same answer the `inRangeOf` test at the end
        // gives. At 128 the operands can be the full `Int128` range, and then
        // `a + b` is signed overflow -- undefined behaviour in C++, and on a
        // compiler that assumes it cannot happen the check below would be
        // optimized away.
        if (__builtin_add_overflow(a, b, &result)) {
            trap = TrapKind::IntegerOverflow;
            return false;
        }
        break;
    case OpCode::Sub:
        if (__builtin_sub_overflow(a, b, &result)) {
            trap = TrapKind::IntegerOverflow;
            return false;
        }
        break;
    case OpCode::Mul: {
        __int128 product = 0;
        if (__builtin_mul_overflow(a, b, &product)) {
            trap = TrapKind::IntegerOverflow;
            return false;
        }
        result = product;
        break;
    }
    case OpCode::Div:
        if (b == 0) {
            trap = TrapKind::DivisionByZero;
            return false;
        }
        if (a == signedMinOf(bits) && b == -1) {
            trap = TrapKind::IntegerOverflow;
            return false;
        }
        result = a / b;
        break;
    case OpCode::Mod:
        if (b == 0) {
            trap = TrapKind::DivisionByZero;
            return false;
        }
        result = (b == -1) ? 0 : a % b;
        break;
    case OpCode::And:
        result = a & b;
        break;
    case OpCode::Or:
        result = a | b;
        break;
    case OpCode::Xor:
        result = a ^ b;
        break;
    case OpCode::Shl: {
        if (b < 0 || b >= static_cast<Int128>(bits)) {
            if (a == 0) {
                result = 0;
                break;
            }
            trap = TrapKind::IntegerOverflow;
            return false;
        }
        if (bits >= 128) {
            // `~maskFor(128)` is zero, so the carry-out test below can never fire
            // at this width -- there is no 129th bit to catch the loss. Bound the
            // operand against the shifted limit instead: a left shift by `b` stays
            // in range exactly when `a <= max >> b` (for a >= 0) or
            // `a >= min >> b` (for a < 0).
            const unsigned amount = static_cast<unsigned>(b);
            const Int128 limit = a >= 0 ? (signedMaxOf(bits) >> amount)
                                        : (signedMinOf(bits) >> amount);
            if (a >= 0 ? (a > limit) : (a < limit)) {
                trap = TrapKind::IntegerOverflow;
                return false;
            }
            result = static_cast<Int128>(static_cast<unsigned __int128>(a) << amount);
            break;
        }
        const unsigned __int128 shifted =
            static_cast<unsigned __int128>(a) << static_cast<unsigned>(b);
        result = static_cast<Int128>(shifted & maskFor(bits));
        if ((shifted & ~maskFor(bits)) != 0) {
            trap = TrapKind::IntegerOverflow;
            return false;
        }
        break;
    }
    case OpCode::Shr: {
        if (b < 0 || b >= static_cast<Int128>(bits)) {
            result = a < 0 ? -1 : 0;
            break;
        }
        result = a >> static_cast<unsigned>(b);
        break;
    }
    case OpCode::Pow: {
        // Exponentiation by squaring. The final `inRangeOf` below catches a result
        // too wide for the operand type, so only the intermediate products need an
        // explicit check -- they can leave Int128 even when the answer would not.
        if (b < 0) {
            trap = TrapKind::NegativeExponent;
            return false;
        }
        Int128 accumulator = 1;
        Int128 base = a;
        Int128 exponent = b;
        while (exponent > 0) {
            if ((exponent & 1) != 0) {
                if (__builtin_mul_overflow(accumulator, base, &accumulator)) {
                    trap = TrapKind::IntegerOverflow;
                    return false;
                }
            }
            exponent >>= 1;
            if (exponent > 0 && __builtin_mul_overflow(base, base, &base)) {
                trap = TrapKind::IntegerOverflow;
                return false;
            }
        }
        result = accumulator;
        break;
    }
    default:
        trap = TrapKind::Unreachable;
        return false;
    }

    if (!inRangeOf(result, type)) {
        trap = TrapKind::IntegerOverflow;
        return false;
    }
    out = Value::fromInt(result);
    return true;
}

} // namespace

//===----------------------------------------------------------------------===//
// External (shared library) calls
//===----------------------------------------------------------------------===//

namespace {

/// The boundary is deliberately narrow: every argument and the result travel in
/// one 64 bit general purpose register. `double` is excluded because it would be
/// passed in an SSE register, and a call site cannot know which without libffi.
/// Arrays of doubles are fine -- those cross as a pointer and are copied.
using ExternalFn = std::int64_t (*)(std::int64_t, std::int64_t, std::int64_t,
                                    std::int64_t, std::int64_t, std::int64_t);

/// Six, not four: an array pair costs two slots (buffer, count), so a function
/// taking two arrays plus scalars -- `merge(dst, a, na, b, nb)` -- needs five.
/// Four was the number of registers Win64 passes in registers; the ABI puts the
/// rest on the stack, which the compiler handles because the pointer type above
/// declares all six.
constexpr std::size_t kMaxExternalArgs = 6;

} // namespace

template <typename FailFn>
VM::ExternalOutcome VM::callExternal(Process& process, const Function& function,
                                    std::size_t functionIndex, std::size_t argc,
                                    std::string& error, FailFn&& fail) {
    const auto reject = [&](TrapKind kind, const std::string& detail) {
        fail(kind, detail);
        return ExternalOutcome::Failed;
    };

    void* symbol = resolveExternal(function, functionIndex, error);
    if (symbol == nullptr) {
        // The library or symbol is missing. When there is an Axiom body to run
        // instead, say nothing and leave the stack alone: the CALL handler will
        // fall back to it. Otherwise this is a deployment error worth trapping.
        if (!function.code.empty()) {
            // ACVM_DEBUG_EXTERNAL=1 traces which calls fall back and why, the
            // same way --stats reports instruction counts. Off unless asked
            // for, so the normal path pays nothing.
            if (std::getenv("ACVM_DEBUG_EXTERNAL") != nullptr) {
                std::fprintf(stderr, "[external] %s: falling back to Axiom (%s)\n",
                             function.name.c_str(), error.c_str());
            }
            return ExternalOutcome::Unavailable;
        }
        // `reject` writes through `error` too, so the message must be copied
        // first: passing the same string as both output and detail overwrites
        // it with the trap kind name before it is read.
        const std::string reason = error;
        return reject(TrapKind::ExternalCallFailed, reason);
    }

    // Pop the Axiom arguments first, newest first, then walk the marshalling
    // plan in source order: an array pair is (pointer, count), so the two pop
    // in reverse.
    std::vector<Value> raw(argc);
    for (std::size_t i = argc; i-- > 0;) {
        if (!process.stack.empty()) {
            raw[i] = process.stack.back();
            process.stack.pop_back();
        } else {
            return reject(TrapKind::Unreachable, "stack underflow in external call");
        }
    }

    struct Buffered {
        std::uint64_t address = 0;
        std::size_t bytes = 0;
        std::size_t offset = 0;
        /// Trailing bytes owned by this entry but not part of the value: one for a
        /// string, so its NUL cannot be overwritten by the next buffer.
        std::size_t pad = 0;
        /// Strings are immutable: their bytes are copied out but never back.
        bool readOnly = false;
    };

    // Pass one: work out the layout and validate, but touch nothing yet.
    //
    // The staging buffer must not be pointed at until it has stopped growing.
    // `resize()` may reallocate, and a pointer captured before that dangles by
    // the time the native call reads it -- which is exactly what happened with
    // two array parameters: the second array's growth freed the first array's
    // buffer, so the C side wrote its answer into reclaimed memory and the
    // caller saw the array unchanged. One `resize` up front, then record offsets
    // (not pointers) and fill them in afterwards.
    std::vector<Buffered> buffers;
    std::size_t totalBytes = 0;
    std::size_t cursor = 0;
    for (const ExternalParam& param : function.externalParams) {
        if (!param.isArray && !param.isString) {
            if (cursor >= raw.size()) {
                return reject(TrapKind::Unreachable, "internal error: missing argument");
            }
            ++cursor;
            continue;
        }
        if (param.isString) {
            // A string argument is one Axiom value: the descriptor address. Its
            // data pointer and length are read here so the size is known before
            // the single resize below.
            if (cursor >= raw.size()) {
                return reject(TrapKind::Unreachable, "internal error: missing argument");
            }
            const std::uint64_t descriptor = raw[cursor++].asPtr();
            Buffered buffer;
            buffer.readOnly = true;
            if (descriptor != 0) {
                std::uint64_t data = 0;
                std::uint64_t length = 0;
                if (!readBytes(descriptor, &data, sizeof(data)) ||
                    !readBytes(descriptor + 8, &length, sizeof(length))) {
                    return reject(TrapKind::InvalidPointer,
                                  "'" + function.name + "': string is not readable");
                }
                buffer.address = data;
                buffer.bytes = static_cast<std::size_t>(length);
            }
            // One byte per string, reserved here rather than at the end of the
            // whole buffer: the next array is written immediately after this
            // entry's bytes, and a shared terminator at the far end would be
            // overwritten by it -- turning ".../out.txt" into ".../out.txtA".
            buffer.pad = 1;
            buffer.offset = totalBytes;
            totalBytes += buffer.bytes + buffer.pad;
            buffers.push_back(buffer);
            continue;
        }
        if (cursor + 1 >= raw.size()) {
            return reject(TrapKind::Unreachable, "internal error: array parameter is unpaired");
        }
        const std::uint64_t address = raw[cursor].asPtr();
        const Int128 count = raw[cursor + 1].asInt();
        cursor += 2;

        if (count < 0) {
            return reject(TrapKind::Explicit,
                          "'" + function.name + "': array length must not be negative");
        }
        if (count > 0 && address == 0) {
            return reject(TrapKind::NullPointer, "'" + function.name + "': null array pointer");
        }
        Buffered buffer;
        buffer.address = address;
        buffer.readOnly = param.arrayReadOnly;
        buffer.bytes = static_cast<std::size_t>(count) * param.elemSize;
        buffer.offset = totalBytes;
        totalBytes += buffer.bytes;
        buffers.push_back(buffer);
    }
    if (cursor != raw.size()) {
        return reject(TrapKind::Unreachable,
                      "internal error: argument count mismatch in '" + function.name + "'");
    }

    // Only the string terminators need to be zero; every array byte is
    // overwritten by the copy below. This used to zero the whole buffer, which
    // for an 8 MB array meant an extra 8 MB memset on every call -- a third of
    // the traffic in a reduction that is already memory bound, and paid again on
    // each iteration of a loop.
    //
    // `resize` still zeroes bytes it *adds* (when the buffer has to grow), so a
    // one-off call costs the same; the saving is on the repeat calls, which is
    // exactly the case that matters.
    m_nativeBuffer.resize(totalBytes);
    for (const Buffered& buffer : buffers) {
        if (buffer.pad > 0) {
            std::memset(m_nativeBuffer.data() + buffer.offset + buffer.bytes, 0, buffer.pad);
        }
    }

    // Pass two: copy each array in and build the C argument list.
    std::vector<std::int64_t> cArgs;
    cArgs.reserve(kMaxExternalArgs);
    std::size_t bufferIndex = 0;
    cursor = 0;
    for (const ExternalParam& param : function.externalParams) {
        if (cArgs.size() >= kMaxExternalArgs) {
            return reject(TrapKind::Unreachable,
                          "'" + function.name + "' needs more than " +
                              std::to_string(kMaxExternalArgs) +
                              " native arguments; the dynamic boundary supports at most " +
                              std::to_string(kMaxExternalArgs));
        }
        if (param.isArray || param.isString) {
            const Buffered& buffer = buffers[bufferIndex++];
            if (buffer.bytes > 0 &&
                !readBytes(buffer.address, m_nativeBuffer.data() + buffer.offset, buffer.bytes)) {
                return reject(TrapKind::InvalidPointer,
                              "'" + function.name + "': array is not readable");
            }
            cArgs.push_back(static_cast<std::int64_t>(
                reinterpret_cast<std::uintptr_t>(m_nativeBuffer.data() + buffer.offset)));
            if (param.isString) {
                // One Axiom value, and `cursor` must advance by one exactly as it
                // did in the sizing pass. Forgetting that here put every later
                // argument one slot off: a string descriptor was handed to C as if
                // it were the next array pointer.
                cArgs.push_back(static_cast<std::int64_t>(buffer.bytes));
                ++cursor;
            } else {
                cArgs.push_back(static_cast<std::int64_t>(raw[cursor + 1].asInt()));
                cursor += 2;
            }
        } else {
            const Value& value = raw[cursor++];
            if (value.tag() == Value::Tag::Float) {
                return reject(TrapKind::Unreachable,
                              "'" + function.name +
                                  "': a scalar 'double' cannot cross the dynamic boundary "
                                  "(pass 'ptr double' instead)");
            }
            cArgs.push_back(value.tag() == Value::Tag::Ptr
                                ? static_cast<std::int64_t>(value.asPtr())
                                : static_cast<std::int64_t>(value.asInt()));
        }
    }
    while (cArgs.size() < kMaxExternalArgs) {
        cArgs.push_back(0);
    }

    const auto target = reinterpret_cast<ExternalFn>(symbol);
    const std::int64_t result =
        target(cArgs[0], cArgs[1], cArgs[2], cArgs[3], cArgs[4], cArgs[5]);

    // Copy every array back: a native function may have written to it in place,
    // which is the contract for in-place algorithms like sort.
    for (const Buffered& buffer : buffers) {
        if (!buffer.readOnly && buffer.bytes > 0 &&
            !writeBytes(buffer.address, m_nativeBuffer.data() + buffer.offset, buffer.bytes)) {
            return reject(TrapKind::InvalidPointer,
                          "'" + function.name + "': array is not writable");
        }
    }

    if (function.externalReturnKind == 1) {
        process.stack.push_back(Value::fromInt(static_cast<Int128>(result)));
    } else if (function.externalReturnKind == 2) {
        double value = 0.0;
        std::memcpy(&value, &result, sizeof(value));
        process.stack.push_back(Value::fromFloat(value));
    }
    return ExternalOutcome::Called;
}

VM::Step VM::step(Process& process, std::string& error) {
    // Runs instructions until control has to leave the interpreter: a safe point (the
    // scheduler's turn), a trap, or the end of the call stack.
    //
    // The loop exists because per-instruction cost was dominated by the machinery around
    // the instruction rather than by the instruction itself. Measured on quicksort at
    // 50 000 elements, 12.8M instructions took 159 ms -- 12.4 ns each, about 37 cycles for
    // work like "push a constant" or "compare and branch". The overhead was structural:
    // every instruction re-read frames.back(), copied a Frame, re-derived code.size() and
    // baseAddress, and returned through an enum that the caller switched on. Holding ip,
    // code and baseAddress in locals turns those into registers, and it also removes one
    // store per taken branch, because a jump no longer writes frames.back().ip twice.
    //
    // Anything that grows or shrinks `frames` invalidates this snapshot, so Call and Ret
    // write ip back first and restart the snapshot instead of patching it in place.
    for (;;) {
        if (process.frames.empty()) {
            return Step::Finished;
        }
        if (process.frames.size() < m_frameFloor) {
            // Set by the JIT while it runs an interpreted callee: stop at the moment that
            // frame returns rather than continuing into the compiled caller's bytecode.
            return Step::Finished;
        }

        Frame& frame = process.frames.back();
        const Function& function = *frame.function;
        const Instruction* const code = function.code.data();
        const std::size_t codeSize = function.code.size();
        const std::uint64_t baseAddress = frame.base;
        std::size_t ip = frame.ip;

        // Set by the two instructions that change the frame stack; ends the inner loop so
        // the snapshot above is taken again.
        bool reload = false;

        while (!reload) {
            if (ip >= codeSize) {
                frame.ip = ip;
                return Step::Finished;
            }
            if (m_budget == 0) {
                frame.ip = ip;
                trap(error, TrapKind::Unreachable,
                     "instruction budget exhausted (possible infinite loop)");
                return Step::Trap;
            }
            if (memoryExhausted()) {
                frame.ip = ip;
                trap(error, TrapKind::AllocationFailed,
                     "the program touched more than " +
                         std::to_string(kMaxMemoryBytes / (1024 * 1024)) +
                         " MB of memory (runaway pointer or unbounded allocation)");
                return Step::Trap;
            }
            --m_budget;

            // The debugger sees the instruction *before* it executes, which is the only
            // moment at which a breakpoint can prevent it. One pointer test when nothing
            // is attached; the callback itself decides whether to stop.
            if (m_debugAttached) {
                DebugStepInfo info;
                // The frame holds a pointer into the module's function table; the ABI
                // wants an index, so the difference is computed rather than stored.
                const std::ptrdiff_t ordinal =
                    m_module == nullptr ? -1 : frame.function - m_module->functions.data();
                info.functionIndex = ordinal >= 0 ? static_cast<std::int32_t>(ordinal) : -1;
                info.ip = static_cast<std::int32_t>(ip);
                info.opcode = static_cast<std::int32_t>(code[ip].op);
                info.operandA = code[ip].a;
                info.operandB = code[ip].b;
                info.frameDepth = static_cast<std::int32_t>(process.frames.size());
                info.instructionsRetired = static_cast<std::int64_t>(m_instructions);
                if (m_debugHooks.beforeInstruction != nullptr &&
                    m_debugHooks.beforeInstruction(m_debugHooks.user, &info) ==
                        DebugAction::Abort) {
                    m_debugAttached = false;
                    m_debugAborted = true;
                    frame.ip = ip;
                    trap(error, TrapKind::Unreachable, "stopped by the debugger");
                    return Step::Trap;
                }
            }

            const Instruction& inst = code[ip];
            ++ip;
            ++m_instructions;
            ++m_opcodeCounts[static_cast<unsigned>(inst.op)];

            // Every `return Step::Continue` below became `continue`, which targets this
            // loop. Note that `continue` inside the nested switch over ExternalOutcome
            // also targets this loop, not that switch -- which is what that path wants:
            // the native call completed and the next instruction follows.
            const auto fail = [&](TrapKind kind, const std::string& detail) {
                frame.ip = ip;
                trap(error, kind, detail);
                return Step::Trap;
            };
            const auto popValue = [&](Value& out) {
                if (process.stack.empty()) {
                    return false;
                }
                out = process.stack.back();
                process.stack.pop_back();
                return true;
            };
            const auto pushValue = [&](Value value) { process.stack.push_back(std::move(value)); };

            switch (inst.op) {
            case OpCode::ConstInt:
            case OpCode::ConstFloat:
                if (inst.a < 0 ||
                    static_cast<std::size_t>(inst.a) >= function.constants.size()) {
                    return fail(TrapKind::Unreachable, "invalid constant index");
                }
                pushValue(function.constants[static_cast<std::size_t>(inst.a)]);
                continue;

            case OpCode::ConstStr:
                if (inst.a < 0 || static_cast<std::size_t>(inst.a) >= m_stringAddresses.size()) {
                    return fail(TrapKind::Unreachable, "invalid string index");
                }
                pushValue(Value::fromAddr(m_stringAddresses[static_cast<std::size_t>(inst.a)]));
                continue;

            case OpCode::LocalAddr:
                pushValue(Value::fromAddr(baseAddress + static_cast<std::uint64_t>(inst.a)));
                continue;

            case OpCode::GlobalAddr:
                if (inst.a < 0 || static_cast<std::size_t>(inst.a) >= m_module->globals.size()) {
                    return fail(TrapKind::Unreachable, "invalid global index");
                }
                pushValue(Value::fromAddr(kGlobalBase +
                                          m_module->globals[static_cast<std::size_t>(inst.a)].offset));
                continue;

            case OpCode::Load: {
                Value address;
                if (!popValue(address)) {
                    return fail(TrapKind::Unreachable, "stack underflow in LOAD");
                }
                if (address.asPtr() == 0) {
                    return fail(TrapKind::NullPointer, "load through a null pointer");
                }
                Value value;
                if (!loadScalar(address.asPtr(), inst.a, value)) {
                    return fail(TrapKind::Unreachable, "invalid memory type in LOAD");
                }
                pushValue(value);
                continue;
            }

            case OpCode::Store:
            case OpCode::StoreKeep: {
                Value value;
                Value address;
                if (!popValue(value) || !popValue(address)) {
                    return fail(TrapKind::Unreachable, "stack underflow in STORE");
                }
                if (address.asPtr() == 0) {
                    return fail(TrapKind::NullPointer, "store through a null pointer");
                }
                if (!storeScalar(address.asPtr(), inst.a, value)) {
                    return fail(TrapKind::Unreachable, "invalid memory type in STORE");
                }
                if (inst.op == OpCode::StoreKeep) {
                    pushValue(value);
                }
                continue;
            }

            case OpCode::Dup: {
                if (process.stack.empty()) {
                    return fail(TrapKind::Unreachable, "stack underflow in DUP");
                }
                pushValue(process.stack.back());
                continue;
            }

            case OpCode::Swap: {
                if (process.stack.size() < 2) {
                    return fail(TrapKind::Unreachable, "stack underflow in SWAP");
                }
                const std::size_t size = process.stack.size();
                std::swap(process.stack[size - 1], process.stack[size - 2]);
                continue;
            }

            case OpCode::Pop:
                if (process.stack.empty()) {
                    return fail(TrapKind::Unreachable, "stack underflow in POP");
                }
                process.stack.pop_back();
                continue;

            case OpCode::Add:
            case OpCode::Sub:
            case OpCode::Mul:
            case OpCode::Div:
            case OpCode::Mod:
            case OpCode::Pow:
            case OpCode::And:
            case OpCode::Or:
            case OpCode::Xor:
            case OpCode::Shl:
            case OpCode::Shr:
            case OpCode::Eq:
            case OpCode::Ne:
            case OpCode::Lt:
            case OpCode::Le:
            case OpCode::Gt:
            case OpCode::Ge: {
                Value rhs;
                Value lhs;
                if (!popValue(rhs) || !popValue(lhs)) {
                    return fail(TrapKind::Unreachable, "stack underflow in an arithmetic instruction");
                }
                Value result;
                TrapKind trapKind = TrapKind::Unreachable;
                if (!computeBinary(inst.op, memType(inst.a), lhs, rhs, result, trapKind)) {
                    if (trapKind == TrapKind::IntegerOverflow) {
                        return fail(trapKind, "signed overflow in '" + function.name + "'");
                    }
                    if (trapKind == TrapKind::DivisionByZero) {
                        return fail(trapKind, "in '" + function.name + "'");
                    }
                    return fail(trapKind, "in '" + function.name + "'");
                }
                pushValue(result);
                continue;
            }

            case OpCode::Neg: {
                Value operand;
                if (!popValue(operand)) {
                    return fail(TrapKind::Unreachable, "stack underflow in NEG");
                }
                const MemType& type = memType(inst.a);
                if (type.isFloat()) {
                    pushValue(Value::fromFloat(-operand.asFloat()));
                    continue;
                }
                const Int128 value = operand.asInt();
                if (!type.isUnsigned && value == signedMinOf(type.bits)) {
                    return fail(TrapKind::IntegerOverflow, "negation overflow in '" + function.name + "'");
                }
                const unsigned bits = type.bits == 0 ? 32 : type.bits;
                const Int128 negated = -value;
                if (type.isUnsigned) {
                    const unsigned __int128 raw =
                        static_cast<unsigned __int128>(negated) & maskFor(bits);
                    pushValue(Value::fromInt(static_cast<Int128>(raw)));
                    continue;
                }
                if (!inRangeOf(negated, type)) {
                    return fail(TrapKind::IntegerOverflow, "negation overflow in '" + function.name + "'");
                }
                pushValue(Value::fromInt(negated));
                continue;
            }

            case OpCode::BitNot: {
                Value operand;
                if (!popValue(operand)) {
                    return fail(TrapKind::Unreachable, "stack underflow in BITNOT");
                }
                const MemType& type = memType(inst.a);
                const unsigned bits = type.bits == 0 ? 32 : type.bits;
                const unsigned __int128 raw =
                    static_cast<unsigned __int128>(~operand.asInt()) & maskFor(bits);
                Int128 result = static_cast<Int128>(raw);
                if (!type.isUnsigned && bits < 128 &&
                    (raw & (static_cast<unsigned __int128>(1) << (bits - 1))) != 0) {
                    result = static_cast<Int128>(raw | ~maskFor(bits));
                }
                pushValue(Value::fromInt(result));
                continue;
            }

            case OpCode::Not: {
                Value operand;
                if (!popValue(operand)) {
                    return fail(TrapKind::Unreachable, "stack underflow in NOT");
                }
                const bool truthy = operand.tag() == Value::Tag::Float ? operand.asFloat() != 0.0
                                                                      : operand.asInt() != 0;
                pushValue(Value::fromInt(truthy ? 0 : 1));
                continue;
            }

            case OpCode::StrEq: {
                Value rhs;
                Value lhs;
                if (!popValue(rhs) || !popValue(lhs)) {
                    return fail(TrapKind::Unreachable, "stack underflow in STR_EQ");
                }
                const bool equal = equalStrings(lhs, rhs);
                pushValue(Value::fromInt(equal ? 1 : 0));
                continue;
            }

            case OpCode::StrCmp: {
                Value rhs;
                Value lhs;
                if (!popValue(rhs) || !popValue(lhs)) {
                    return fail(TrapKind::Unreachable, "stack underflow in STR_CMP");
                }
                pushValue(Value::fromInt(compareStrings(lhs, rhs)));
                continue;
            }

            case OpCode::MemEq: {
                Value rhs;
                Value lhs;
                if (!popValue(rhs) || !popValue(lhs)) {
                    return fail(TrapKind::Unreachable, "stack underflow in MEM_EQ");
                }
                const std::size_t size = static_cast<std::size_t>(std::max<std::int32_t>(inst.a, 0));
                std::vector<std::uint8_t> left(size);
                std::vector<std::uint8_t> right(size);
                readBytes(lhs.asPtr(), left.data(), size);
                readBytes(rhs.asPtr(), right.data(), size);
                const bool equal = std::memcmp(left.data(), right.data(), size) == 0;
                pushValue(Value::fromInt(equal ? 1 : 0));
                continue;
            }

            case OpCode::Cast: {
                Value input;
                if (!popValue(input)) {
                    return fail(TrapKind::Unreachable, "stack underflow in CAST");
                }
                const MemType& from = memType(inst.a);
                const MemType& to = memType(inst.b);

                if (to.isFloat()) {
                    if (from.isFloat()) {
                        pushValue(Value::fromFloat(input.asFloat()));
                    } else if (from.isPtr()) {
                        pushValue(Value::fromFloat(static_cast<double>(input.asPtr())));
                    } else {
                        pushValue(Value::fromFloat(static_cast<double>(input.asInt())));
                    }
                    continue;
                }

                if (to.isPtr()) {
                    if (from.isPtr()) {
                        pushValue(Value::fromPtr(input.asPtr()));
                    } else {
                        pushValue(Value::fromPtr(static_cast<std::uint64_t>(input.asInt())));
                    }
                    continue;
                }

                Int128 value = 0;
                if (from.isFloat()) {
                    const double number = input.asFloat();
                    if (std::isnan(number) || std::isinf(number)) {
                        return fail(TrapKind::CastOutOfRange,
                                    "cannot convert NaN or infinity to an integer");
                    }
                    value = static_cast<Int128>(number); // truncation toward zero
                } else if (from.isPtr()) {
                    value = static_cast<Int128>(input.asPtr());
                } else {
                    value = input.asInt();
                }

                if (!to.isInteger()) {
                    pushValue(Value::fromInt(value));
                    continue;
                }
                if (!inRangeOf(value, to)) {
                    return fail(TrapKind::CastOutOfRange,
                                "value " + formatInt128(value) + " does not fit in the target type");
                }
                pushValue(Value::fromInt(value));
                continue;
            }

            case OpCode::Jump:
                ip = static_cast<std::size_t>(inst.a);
                continue;

            case OpCode::JumpIfFalse:
            case OpCode::JumpIfTrue: {
                Value condition;
                if (!popValue(condition)) {
                    return fail(TrapKind::Unreachable, "stack underflow in a conditional jump");
                }
                const bool truthy = condition.tag() == Value::Tag::Float ? condition.asFloat() != 0.0
                                                                        : condition.asInt() != 0;
                if ((inst.op == OpCode::JumpIfTrue) == truthy) {
                    ip = static_cast<std::size_t>(inst.a);
                }
                continue;
            }

            case OpCode::Call:
            case OpCode::CallIndirect: {
                // One implementation for both: the only difference is where the target
                // comes from. For CALL_INDIRECT it is a handle on the stack -- the index
                // plus one, so a zeroed slot (which is what an uninitialised `Func` holds)
                // traps instead of calling the first function in the module.
                std::int32_t target = inst.a;
                if (inst.op == OpCode::CallIndirect) {
                    Value handle;
                    if (!popValue(handle)) {
                        return fail(TrapKind::Unreachable, "stack underflow in CALL_INDIRECT");
                    }
                    if (handle.asInt() <= 0) {
                        return fail(TrapKind::NullPointer,
                                    "calling a function value that was never set");
                    }
                    target = static_cast<std::int32_t>(handle.asInt() - 1);
                }
                if (target < 0 ||
                    static_cast<std::size_t>(target) >= m_module->functions.size()) {
                    return fail(TrapKind::Unreachable, "invalid call target");
                }
                const Function& callee = m_module->functions[static_cast<std::size_t>(target)];
                const std::size_t argc = inst.b < 0 ? 0 : static_cast<std::size_t>(inst.b);

                // A function from a `library "x";` file has no bytecode: resolve the
                // symbol on first call, marshal the arguments, and make one native call.
                // Control returns to the next instruction either way.
                if (callee.isExternal()) {
                    switch (callExternal(process, callee, static_cast<std::size_t>(target), argc,
                                         error, fail)) {
                    case ExternalOutcome::Called:
                        continue;
                    case ExternalOutcome::Failed:
                        return Step::Trap;
                    case ExternalOutcome::Unavailable:
                        // The shared library is missing but this function also has an
                        // Axiom body (the .ax fallback next to the declaration file).
                        // Fall through to the normal call path: it sets up a frame from
                        // the arguments still sitting on the stack and runs that body,
                        // so the program behaves the same, only slower.
                        break;
                    }
                }

                std::vector<Value> arguments(argc);
                for (std::size_t i = argc; i-- > 0;) {
                    if (!popValue(arguments[i])) {
                        return fail(TrapKind::Unreachable, "stack underflow in CALL");
                    }
                }

                std::uint64_t destination = 0;
                if (callee.returnsAggregate) {
                    Value value;
                    if (!popValue(value)) {
                        return fail(TrapKind::Unreachable, "missing return destination in CALL");
                    }
                    destination = value.asPtr();
                }

                const std::uint64_t frameBase = process.stackTop;
                const std::uint64_t frameSize = std::max<std::uint64_t>(callee.frameSize, 8);

                // Recursion without a base case compiles fine and would otherwise grow
                // the frame stack forever. Stop with a diagnosable trap rather than
                // letting the machine run out of memory.
                if (process.frames.size() >= kMaxCallDepth) {
                    return fail(TrapKind::AllocationFailed,
                                "call depth exceeded " + std::to_string(kMaxCallDepth) +
                                    " frames in '" + callee.name +
                                    "' (unbounded recursion?)");
                }

                zeroBytes(frameBase, frameSize);
                process.stackTop += frameSize;

                for (std::size_t i = 0; i < argc && i < callee.params.size(); ++i) {
                    const ParamInfo& param = callee.params[i];
                    if (param.aggregate) {
                        std::vector<std::uint8_t> buffer(param.size);
                        readBytes(arguments[i].asPtr(), buffer.data(), param.size);
                        writeBytes(frameBase + param.offset, buffer.data(), param.size);
                    } else {
                        storeScalar(frameBase + param.offset, param.memType, arguments[i]);
                    }
                }
                if (callee.returnsAggregate) {
                    writeBytes(frameBase, &destination, sizeof(destination));
                }

                // `frame` is a reference into `frames`, so it must not be touched after the
                // push: the vector may reallocate. Write ip back first, then leave the inner
                // loop so the snapshot at the top is taken again for the callee.
                frame.ip = ip;
                process.frames.push_back(Frame{&callee, 0, frameBase});

                // Compiled code takes over here, after the frame exists and its parameters
                // are in place. It also performs the callee's `Ret`, so on success the frame
                // is already gone and there is nothing left to interpret. Anything the
                // lowering refused falls through to the interpreter below, which is why the
                // frame has to be set up first rather than inside the compiled body.
                if (m_jit.mode == JitMode::On) {
                    const std::size_t calleeIndex = static_cast<std::size_t>(target);
                    if (JitFunction native =
                            m_jit.entryFor(*m_module, calleeIndex, m_jitRefusal)) {
                        const std::int64_t outcome =
                            native(this, &process, frameBase, &error);
                        if (outcome == 0) {
                            return Step::Trap;
                        }
                        if (outcome == 2) {
                            // The compiled body reached a safe point with more than one
                            // process runnable, and wrote its own resume ip back. Handing
                            // back Step::SafePoint is exactly what the interpreter's own
                            // SAFE_POINT handler does.
                            return Step::SafePoint;
                        }
                        return Step::Continue;
                    }
                }

                reload = true;
                break;
            }

            case OpCode::Ret: {
                const bool hasResult = function.returnsValue;
                Value result;
                if (hasResult && !popValue(result)) {
                    return fail(TrapKind::Unreachable, "missing return value in '" + function.name + "'");
                }
                const std::uint64_t callerBase = frame.base;
                process.frames.pop_back();
                process.stackTop = callerBase;
                if (hasResult) {
                    pushValue(result);
                }
                if (process.frames.empty()) {
                    // Keep the entry point's result on the stack so that callers which
                    // run a function to completion (event conditions) can read it.
                    return Step::Finished;
                }
                // The caller's frame is a different function: re-snapshot before dispatching.
                reload = true;
                break;
            }

            case OpCode::MemCopy: {
                Value destination;
                Value source;
                if (!popValue(destination) || !popValue(source)) {
                    return fail(TrapKind::Unreachable, "stack underflow in MEM_COPY");
                }
                const std::size_t size = static_cast<std::size_t>(std::max<std::int32_t>(inst.a, 0));
                if (size == 0) {
                    continue;
                }
                if (destination.asPtr() == 0 || source.asPtr() == 0) {
                    return fail(TrapKind::NullPointer, "memcpy through a null pointer");
                }
                std::vector<std::uint8_t> buffer(size);
                readBytes(source.asPtr(), buffer.data(), size);
                writeBytes(destination.asPtr(), buffer.data(), size);
                continue;
            }

            case OpCode::MemZero: {
                Value destination;
                if (!popValue(destination)) {
                    return fail(TrapKind::Unreachable, "stack underflow in MEM_ZERO");
                }
                const std::size_t size = static_cast<std::size_t>(std::max<std::int32_t>(inst.a, 0));
                if (size != 0 && destination.asPtr() == 0) {
                    return fail(TrapKind::NullPointer, "memset through a null pointer");
                }
                zeroBytes(destination.asPtr(), size);
                continue;
            }

            case OpCode::Concat: {
                Value rhs;
                Value lhs;
                if (!popValue(rhs) || !popValue(lhs)) {
                    return fail(TrapKind::Unreachable, "stack underflow in CONCAT");
                }

                // Renders one operand as text. `code` is -1 for a string and otherwise a
                // MemType index; `shifted` marks the left operand, whose codes are stored
                // as index+1 so that 0 can mean "string" without colliding with index 0.
                // A `Value` cannot answer this on its own: `Ptr` is used both for strings
                // and for real pointers, and an `Int` carries no width.
                const auto render = [&](const Value& value, int code, bool shifted) -> std::string {
                    if (shifted) {
                        if (code == 0) {
                            return readString(value);
                        }
                        code -= 1;
                    } else if (code < 0) {
                        return readString(value);
                    }
                    const MemType& type = memType(code);
                    if (type.isFloat()) {
                        return formatFloat(value.asFloat());
                    }
                    if (type.isPtr()) {
                        const std::uint64_t pointer = value.asPtr();
                        return pointer == 0 ? std::string("null")
                                            : ("0x" + formatInt128(static_cast<Int128>(pointer), 16));
                    }
                    if (type.kind == MemType::Kind::Bool) {
                        return value.asInt() != 0 ? "true" : "false";
                    }
                    if (type.kind == MemType::Kind::Char) {
                        return formatCharacter(static_cast<std::uint32_t>(value.asInt()));
                    }
                    return formatInt128(value.asInt());
                };

                std::string text = render(lhs, inst.b, /*shifted=*/true);
                text += render(rhs, inst.a, /*shifted=*/false);
                const std::uint64_t descriptor = makeStringInArena(process, text);
                if (descriptor == 0) {
                    return fail(TrapKind::AllocationFailed, "arena exhausted while concatenating a string");
                }
                pushValue(Value::fromAddr(descriptor));
                continue;
            }

            case OpCode::Builtin: {
                Step result = Step::Continue;
                if (!executeBuiltin(process, inst, error, result)) {
                    frame.ip = ip;
                    return Step::Trap;
                }
                // io / alloc builtins can ask to leave the interpreter (a safe point, or a
                // read that finishes the process), so the result is propagated. ip has to be
                // written back first: returning Continue without it makes the next entry into
                // step() re-run this same instruction forever.
                frame.ip = ip;
                return result;
            }

            case OpCode::ArenaMark:
                process.arenaMarks.push_back(process.arenaOffset);
                continue;

            case OpCode::ArenaRelease:
                if (!process.arenaMarks.empty()) {
                    process.arenaOffset = process.arenaMarks.back();
                    process.arenaMarks.pop_back();
                }
                continue;

            case OpCode::SafePoint:
                frame.ip = ip;
                return Step::SafePoint;

            case OpCode::LoadLocal: {
                Value value;
                if (!loadScalar(baseAddress + static_cast<std::uint64_t>(inst.a), inst.b & 0xFF, value)) {
                    return fail(TrapKind::Unreachable, "invalid memory type in LOAD_LOCAL");
                }
                pushValue(value);
                continue;
            }

            case OpCode::StoreLocal: {
                Value value;
                if (!popValue(value)) {
                    return fail(TrapKind::Unreachable, "stack underflow in STORE_LOCAL");
                }
                if (!storeScalar(baseAddress + static_cast<std::uint64_t>(inst.a), inst.b & 0xFF, value)) {
                    return fail(TrapKind::Unreachable, "invalid memory type in STORE_LOCAL");
                }
                if ((inst.b & 0x100) != 0) {
                    pushValue(value);
                }
                continue;
            }

            case OpCode::LoadGlobal: {
                if (inst.a < 0 || static_cast<std::size_t>(inst.a) >= m_module->globals.size()) {
                    return fail(TrapKind::Unreachable, "invalid global index");
                }
                Value value;
                if (!loadScalar(kGlobalBase + m_module->globals[static_cast<std::size_t>(inst.a)].offset,
                                inst.b & 0xFF, value)) {
                    return fail(TrapKind::Unreachable, "invalid memory type in LOAD_GLOBAL");
                }
                pushValue(value);
                continue;
            }

            case OpCode::StoreGlobal: {
                if (inst.a < 0 || static_cast<std::size_t>(inst.a) >= m_module->globals.size()) {
                    return fail(TrapKind::Unreachable, "invalid global index");
                }
                Value value;
                if (!popValue(value)) {
                    return fail(TrapKind::Unreachable, "stack underflow in STORE_GLOBAL");
                }
                if (!storeScalar(kGlobalBase + m_module->globals[static_cast<std::size_t>(inst.a)].offset,
                                 inst.b & 0xFF, value)) {
                    return fail(TrapKind::Unreachable, "invalid memory type in STORE_GLOBAL");
                }
                if ((inst.b & 0x100) != 0) {
                    pushValue(value);
                }
                continue;
            }

            case OpCode::DerefLocal: {
                Value pointer;
                const std::uint64_t slot = baseAddress + static_cast<std::uint64_t>(inst.a);
                // A frame slot holding a pointer is always 8 bytes wide.
                pointer = Value::fromPtr(0);
                std::uint64_t raw = 0;
                if (!readBytes(slot, &raw, sizeof(raw))) {
                    return fail(TrapKind::Unreachable, "cannot read the pointer slot");
                }
                pointer = Value::fromPtr(raw);
                const std::uint64_t address = addressOf(pointer);
                if (address == 0) {
                    return fail(TrapKind::NullPointer, "load through a null pointer");
                }
                Value value;
                if (!loadScalar(address, inst.b, value)) {
                    return fail(TrapKind::Unreachable, "invalid memory type in DEREF_LOCAL");
                }
                pushValue(value);
                continue;
            }

            case OpCode::StepLocal: {
                const std::uint64_t slot = baseAddress + static_cast<std::uint64_t>(inst.a);
                const int typeIndex = inst.b & 0xFF;
                const std::int32_t delta = inst.b >> 8;   // arithmetic shift: sign kept
                Value current;
                if (!loadScalar(slot, typeIndex, current)) {
                    return fail(TrapKind::Unreachable, "invalid memory type in STEP_LOCAL");
                }
                const MemType& type = memType(typeIndex);
                Value updated;
                TrapKind trapKind = TrapKind::Unreachable;
                if (!computeBinary(delta < 0 ? OpCode::Sub : OpCode::Add, type, current,
                                   Value::fromInt(static_cast<Int128>(delta < 0 ? -delta : delta)),
                                   updated, trapKind)) {
                    return fail(trapKind, "in '" + function.name + "'");
                }
                storeScalar(slot, typeIndex, updated);
                continue;
            }

            case OpCode::CopyStep: {
                const std::uint64_t sourceSlot = baseAddress + static_cast<std::uint64_t>(inst.a);
                const std::uint64_t destinationSlot =
                    baseAddress + static_cast<std::uint64_t>(inst.b & 0x0FFF'FFFF);
                const std::size_t elementSize =
                    static_cast<std::size_t>((inst.b >> 28) & 0x7) + 1;

                std::uint64_t source = 0;
                std::uint64_t destination = 0;
                readBytes(sourceSlot, &source, sizeof(source));
                readBytes(destinationSlot, &destination, sizeof(destination));
                if (source == 0) {
                    return fail(TrapKind::NullPointer, "load through a null pointer");
                }
                if (destination == 0) {
                    return fail(TrapKind::NullPointer, "store through a null pointer");
                }

                std::uint8_t buffer[8] = {};
                readBytes(source, buffer, elementSize);
                writeBytes(destination, buffer, elementSize);

                source += elementSize;
                destination += elementSize;
                writeBytes(sourceSlot, &source, sizeof(source));
                writeBytes(destinationSlot, &destination, sizeof(destination));
                continue;
            }

            case OpCode::Trap: {
                const TrapKind kind = static_cast<TrapKind>(inst.a);
                return fail(kind, "in '" + function.name + "'");
            }

            case OpCode::Halt:
                frame.ip = ip;
                return Step::Finished;

            default:
                return fail(TrapKind::Unreachable, "unknown opcode");
            }
        }
    }
}

} // namespace acvm
