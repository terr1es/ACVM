//===--- VMBuiltins.cpp - Intrinsics and the standard library ----------===//
#include "acvm/VM/VM.h"

#include "algorithm.h"   // stdlib/algorithm.h

#include "acvm/Sema/Builtin.h"
#include "VMInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

namespace acvm {
using namespace vminternal;
namespace {

bool parseIntStrict(const std::string& text, long long& out) {
    std::size_t index = 0;
    bool negative = false;
    if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
        negative = text[index] == '-';
        ++index;
    }
    if (index >= text.size()) {
        return false;
    }
    long long value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') {
            return false;
        }
        if (__builtin_mul_overflow(value, 10LL, &value) ||
            __builtin_add_overflow(value, static_cast<long long>(c - '0'), &value)) {
            return false;
        }
    }
    out = negative ? -value : value;
    return true;
}

bool parseFloatStrict(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

std::string currentTimestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return std::string(buffer);
}

std::string toUpperAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

std::string toLowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

} // namespace

bool VM::executeBuiltin(Process& process, const Instruction& inst, std::string& error, Step& step) {
    const Builtin builtin = static_cast<Builtin>(inst.a);

    const auto popValue = [&](Value& out) {
        if (process.stack.empty()) {
            return false;
        }
        out = process.stack.back();
        process.stack.pop_back();
        return true;
    };
    const auto push = [&](Value value) { process.stack.push_back(std::move(value)); };

    const auto formatForOutput = [&](const Value& value, int typeIndex) -> std::string {
        if (typeIndex < 0) {
            return readString(value);
        }
        const MemType& type = memType(typeIndex);
        switch (type.kind) {
        case MemType::Kind::Float:
            return formatFloat(value.asFloat());
        case MemType::Kind::Bool:
            return value.asInt() != 0 ? "true" : "false";
        case MemType::Kind::Char:
            return formatCharacter(static_cast<std::uint32_t>(value.asInt()));
        case MemType::Kind::Ptr: {
            const std::uint64_t pointer = value.asPtr();
            if (pointer == 0) {
                return "null";
            }
            return "0x" + formatInt128(static_cast<Int128>(pointer), 16);
        }
        case MemType::Kind::Int:
            break;
        }
        return formatInt128(value.asInt());
    };

    const auto stringResult = [&](const std::string& text) {
        const std::uint64_t descriptor = makeStringInArena(process, text);
        if (descriptor == 0) {
            trap(error, TrapKind::AllocationFailed, "arena exhausted while creating a string");
            return false;
        }
        push(Value::fromAddr(descriptor));
        return true;
    };

    switch (builtin) {
    case Builtin::Print:
    case Builtin::EPrint:
    case Builtin::Log: {
        Value value;
        if (!popValue(value)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in print");
        }
        const std::string text = formatForOutput(value, inst.b);
        if (builtin == Builtin::Print) {
            m_out << text;
            m_out.flush();
        } else if (builtin == Builtin::EPrint) {
            m_err << text;
            m_err.flush();
        } else {
            m_err << '[' << currentTimestamp() << "] " << text << '\n';
            m_err.flush();
        }
        return true;
    }

    case Builtin::Alloc:
    case Builtin::Alloc0: {
        Value size;
        if (!popValue(size)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in alloc");
        }
        const Int128 requested = size.asInt();
        if (requested < 0) {
            return trap(error, TrapKind::AllocationFailed, "negative allocation size");
        }
        const std::uint64_t address =
            heapAlloc(static_cast<std::uint64_t>(requested), builtin == Builtin::Alloc0);
        push(Value::fromPtr(address));
        return true;
    }

    case Builtin::Free: {
        Value pointer;
        if (!popValue(pointer)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in free");
        }
        heapFree(pointer.asPtr());
        return true;
    }

    case Builtin::Memcpy:
    case Builtin::Memmove: {
        Value count;
        Value source;
        Value destination;
        if (!popValue(count) || !popValue(source) || !popValue(destination)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in memcpy");
        }
        const Int128 bytes = count.asInt();
        if (bytes < 0) {
            return trap(error, TrapKind::AllocationFailed, "negative memcpy size");
        }
        const auto size = static_cast<std::size_t>(bytes);
        if (size != 0) {
            if (destination.asPtr() == 0 || source.asPtr() == 0) {
                return trap(error, TrapKind::NullPointer, "memcpy through a null pointer");
            }
            std::vector<std::uint8_t> buffer(size);
            readBytes(source.asPtr(), buffer.data(), size);
            writeBytes(destination.asPtr(), buffer.data(), size);
        }
        push(Value::fromPtr(destination.asPtr()));
        return true;
    }

    case Builtin::Dup: {
        Value value;
        if (!popValue(value)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in dup");
        }
        push(Value::fromAddr(makeStringOnHeap(readString(value))));
        return true;
    }

    case Builtin::MakeString: {
        Value length;
        Value pointer;
        if (!popValue(length) || !popValue(pointer)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in makeString");
        }
        const Int128 count = length.asInt();
        if (count < 0) {
            return trap(error, TrapKind::AllocationFailed, "negative string length");
        }
        std::string text(static_cast<std::size_t>(count), '\0');
        if (count > 0) {
            readBytes(pointer.asPtr(), text.data(), static_cast<std::size_t>(count));
        }
        push(Value::fromAddr(makeStringOnHeap(text)));
        return true;
    }

    case Builtin::Yield:
        step = Step::SafePoint;
        return true;

    case Builtin::Trap:
        return trap(error, TrapKind::Explicit, "trap()");

    case Builtin::ReadLine: {
        std::string line;
        if (!std::getline(std::cin, line)) {
            push(Value::fromAddr(makeStringInArena(process, "")));
            return true;
        }
        return stringResult(line);
    }

    case Builtin::ReadToken: {
        std::string token;
        if (!(std::cin >> token)) {
            return stringResult("");
        }
        return stringResult(token);
    }

    case Builtin::ArgCount:
        push(Value::fromInt(static_cast<Int128>(m_programArgs.size())));
        return true;

    case Builtin::ArgAt: {
        Value which;
        if (!popValue(which)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in argAt");
        }
        const Int128 index = which.asInt();
        // Out of range yields the empty string rather than a trap: the idiomatic
        // `for (i = 0; i < argCount(); i++)` can never go out of range, so a
        // bad index means the caller got the loop wrong. Letting the program
        // finish and print its way to the problem beats stopping at the first
        // bad offset.
        if (index < 0 || index >= static_cast<Int128>(m_programArgs.size())) {
            return stringResult("");
        }
        return stringResult(m_programArgs[static_cast<std::size_t>(index)]);
    }

    case Builtin::ReadInt: {
        std::string token;
        if (!(std::cin >> token)) {
            return trap(error, TrapKind::Explicit, "readInt: end of input");
        }
        long long value = 0;
        if (!parseIntStrict(token, value)) {
            return trap(error, TrapKind::Explicit, "readInt: '" + token + "' is not an integer");
        }
        push(Value::fromInt(static_cast<Int128>(value)));
        return true;
    }

    case Builtin::ReadFloat: {
        std::string token;
        if (!(std::cin >> token)) {
            return trap(error, TrapKind::Explicit, "readFloat: end of input");
        }
        double value = 0;
        if (!parseFloatStrict(token, value)) {
            return trap(error, TrapKind::Explicit, "readFloat: '" + token + "' is not a number");
        }
        push(Value::fromFloat(value));
        return true;
    }

    case Builtin::Itoa: {
        Value base;
        Value value;
        if (!popValue(base) || !popValue(value)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in itoa");
        }
        const Int128 radix = base.asInt();
        if (radix < 2 || radix > 36) {
            return trap(error, TrapKind::Explicit, "itoa: base must be in [2, 36]");
        }
        return stringResult(formatInt128(value.asInt(), static_cast<int>(radix)));
    }

    case Builtin::Ftoa: {
        Value value;
        if (!popValue(value)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in ftoa");
        }
        return stringResult(formatFloat(value.asFloat()));
    }

    case Builtin::Atoi: {
        Value value;
        if (!popValue(value)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in atoi");
        }
        long long parsed = 0;
        if (!parseIntStrict(readString(value), parsed)) {
            return trap(error, TrapKind::Explicit, "atoi: the string is not a valid integer");
        }
        push(Value::fromInt(static_cast<Int128>(parsed)));
        return true;
    }

    case Builtin::Atof: {
        Value value;
        if (!popValue(value)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in atof");
        }
        double parsed = 0;
        if (!parseFloatStrict(readString(value), parsed)) {
            return trap(error, TrapKind::Explicit, "atof: the string is not a valid number");
        }
        push(Value::fromFloat(parsed));
        return true;
    }

    case Builtin::IndexOf:
    case Builtin::LastIndexOf: {
        Value needle;
        Value haystack;
        if (!popValue(needle) || !popValue(haystack)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in indexOf");
        }
        const std::string text = readString(haystack);
        const std::string part = readString(needle);
        std::size_t position = std::string::npos;
        if (builtin == Builtin::IndexOf) {
            position = text.find(part);
        } else {
            position = text.rfind(part);
        }
        push(Value::fromInt(position == std::string::npos ? -1
                                                         : static_cast<Int128>(position)));
        return true;
    }

    case Builtin::Substring: {
        Value high;
        Value low;
        Value source;
        if (!popValue(high) || !popValue(low) || !popValue(source)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in substring");
        }
        const std::string text = readString(source);
        Int128 begin = low.asInt();
        Int128 end = high.asInt();
        // Slices clamp instead of trapping (spec §2.4.3.1 R-3).
        begin = std::max<Int128>(0, std::min<Int128>(begin, static_cast<Int128>(text.size())));
        end = std::max<Int128>(0, std::min<Int128>(end, static_cast<Int128>(text.size())));
        if (end < begin) {
            end = begin;
        }
        return stringResult(text.substr(static_cast<std::size_t>(begin),
                                        static_cast<std::size_t>(end - begin)));
    }

    case Builtin::StartsWith:
    case Builtin::EndsWith: {
        Value affix;
        Value text;
        if (!popValue(affix) || !popValue(text)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in startsWith");
        }
        const std::string haystack = readString(text);
        const std::string part = readString(affix);
        bool result = false;
        if (builtin == Builtin::StartsWith) {
            result = haystack.size() >= part.size() &&
                     haystack.compare(0, part.size(), part) == 0;
        } else {
            result = haystack.size() >= part.size() &&
                     haystack.compare(haystack.size() - part.size(), part.size(), part) == 0;
        }
        push(Value::fromInt(result ? 1 : 0));
        return true;
    }

    case Builtin::Trim: {
        Value source;
        if (!popValue(source)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in trim");
        }
        const std::string text = readString(source);
        std::size_t begin = 0;
        std::size_t end = text.size();
        while (begin < end && isSpace(text[begin])) {
            ++begin;
        }
        while (end > begin && isSpace(text[end - 1])) {
            --end;
        }
        return stringResult(text.substr(begin, end - begin));
    }

    case Builtin::ToUpper:
    case Builtin::ToLower: {
        Value source;
        if (!popValue(source)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in toUpper");
        }
        const std::string text = readString(source);
        return stringResult(builtin == Builtin::ToUpper ? toUpperAscii(text) : toLowerAscii(text));
    }

    case Builtin::Sqrt:
    case Builtin::Fabs:
    case Builtin::Floor:
    case Builtin::Ceil:
    case Builtin::Round:
    case Builtin::Trunc:
    case Builtin::Sin:
    case Builtin::Cos:
    case Builtin::Tan:
    case Builtin::Asin:
    case Builtin::Acos:
    case Builtin::Atan:
    case Builtin::Exp:
    case Builtin::LogE:
    case Builtin::Log2:
    case Builtin::Log10: {
        Value value;
        if (!popValue(value)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in a math function");
        }
        const double x = value.asFloat();
        double result = 0;
        switch (builtin) {
        case Builtin::Sqrt: result = std::sqrt(x); break;
        case Builtin::Fabs: result = std::fabs(x); break;
        case Builtin::Floor: result = std::floor(x); break;
        case Builtin::Ceil: result = std::ceil(x); break;
        case Builtin::Round: result = std::round(x); break;
        case Builtin::Trunc: result = std::trunc(x); break;
        case Builtin::Sin: result = std::sin(x); break;
        case Builtin::Cos: result = std::cos(x); break;
        case Builtin::Tan: result = std::tan(x); break;
        case Builtin::Asin: result = std::asin(x); break;
        case Builtin::Acos: result = std::acos(x); break;
        case Builtin::Atan: result = std::atan(x); break;
        case Builtin::Exp: result = std::exp(x); break;
        case Builtin::LogE: result = std::log(x); break;
        case Builtin::Log2: result = std::log2(x); break;
        case Builtin::Log10: result = std::log10(x); break;
        default: break;
        }
        push(Value::fromFloat(result));
        return true;
    }

    case Builtin::Atan2:
    case Builtin::Pow:
    case Builtin::Fmod: {
        Value second;
        Value first;
        if (!popValue(second) || !popValue(first)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in a math function");
        }
        const double a = first.asFloat();
        const double b = second.asFloat();
        double result = 0;
        if (builtin == Builtin::Atan2) {
            result = std::atan2(a, b);
        } else if (builtin == Builtin::Pow) {
            result = std::pow(a, b);
        } else {
            result = std::fmod(a, b);
        }
        push(Value::fromFloat(result));
        return true;
    }

    case Builtin::Min:
    case Builtin::Max: {
        Value second;
        Value first;
        if (!popValue(second) || !popValue(first)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in min/max");
        }
        const bool useFloat = first.isFloat() || second.isFloat();
        bool firstWins = false;
        if (useFloat) {
            const double a = first.asFloat();
            const double b = second.asFloat();
            firstWins = builtin == Builtin::Min ? a <= b : a >= b;
        } else {
            const Int128 a = first.asInt();
            const Int128 b = second.asInt();
            firstWins = builtin == Builtin::Min ? a <= b : a >= b;
        }
        push(firstWins ? first : second);
        return true;
    }

    case Builtin::Abs: {
        Value value;
        if (!popValue(value)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in abs");
        }
        const Int128 x = value.asInt();
        if (x == -static_cast<Int128>(static_cast<unsigned __int128>(1) << 126)) {
            return trap(error, TrapKind::IntegerOverflow, "abs() overflow");
        }
        push(Value::fromInt(x < 0 ? -x : x));
        return true;
    }

    case Builtin::SortInts:
    case Builtin::SortInt64s:
    case Builtin::SortFloats:
    case Builtin::ReverseInts: {
        Value count;
        Value base;
        if (!popValue(count) || !popValue(base)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in a native helper");
        }
        const Int128 elements = count.asInt();
        const char* name = builtinName(builtin);
        if (elements < 0) {
            return trap(error, TrapKind::Explicit,
                        std::string(name) + ": element count must not be negative");
        }
        if (elements <= 1) {
            return true;   // nothing to do, and no need to touch memory
        }
        const std::uint64_t address = base.asPtr();
        if (address == 0) {
            return trap(error, TrapKind::NullPointer, std::string(name) + ": null pointer");
        }

        const bool floats = builtin == Builtin::SortFloats;
        const bool wide = builtin == Builtin::SortInt64s;
        const std::size_t elementSize =
            floats || wide ? sizeof(std::int64_t) : sizeof(std::int32_t);
        const std::size_t bytes = static_cast<std::size_t>(elements) * elementSize;

        // Marshal once in, once out. The algorithm then runs with no
        // interpreter overhead at all -- no per-element LOAD, no bounds test,
        // no page lookup. That is the whole point of the native path: the two
        // copies are O(n), the work being replaced is O(n log n) * overhead.
        m_nativeBuffer.resize(bytes);
        readBytes(address, m_nativeBuffer.data(), bytes);

        const int kind = floats ? ACVM_KIND_F64 : (wide ? ACVM_KIND_I64 : ACVM_KIND_I32);
        const int rc = builtin == Builtin::ReverseInts
                           ? acvm_reverse(m_nativeBuffer.data(), static_cast<std::int64_t>(elements), kind)
                           : acvm_sort(m_nativeBuffer.data(), static_cast<std::int64_t>(elements), kind);
        if (rc != 0) {
            return trap(error, TrapKind::Explicit,
                        std::string(name) + ": unsupported element kind");
        }

        writeBytes(address, m_nativeBuffer.data(), bytes);
        return true;
    }

    case Builtin::Assert: {
        Value condition;
        if (!popValue(condition)) {
            return trap(error, TrapKind::Unreachable, "stack underflow in assert");
        }
        const bool truthy = condition.tag() == Value::Tag::Float ? condition.asFloat() != 0.0
                                                                : condition.asInt() != 0;
        if (!truthy && m_assertions) {
            return trap(error, TrapKind::AssertionFailed, "assertion failed");
        }
        return true;
    }

    case Builtin::None:
        break;
    }

    return trap(error, TrapKind::Unreachable, "unknown builtin");
}

} // namespace acvm
