//===--- Value.cpp ------------------------------------------------------===//
#include "acvm/Common/Value.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace acvm {
namespace {

/// Magnitude of a negative 128 bit value, computed without overflowing.
Int128 magnitude(Int128 value) {
    if (value >= 0) {
        return value;
    }
    return static_cast<Int128>(-(static_cast<unsigned __int128>(value)));
}

} // namespace

std::string formatInt128(Int128 value, int base) {
    if (base < 2 || base > 36) {
        base = 10;
    }
    if (value == 0) {
        return "0";
    }

    const bool negative = value < 0;
    unsigned __int128 magnitudeValue = negative ? static_cast<unsigned __int128>(magnitude(value))
                                                : static_cast<unsigned __int128>(value);

    char buffer[140];
    int index = static_cast<int>(sizeof(buffer));
    const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";

    while (magnitudeValue != 0) {
        buffer[--index] = digits[static_cast<int>(magnitudeValue % static_cast<unsigned>(base))];
        magnitudeValue /= static_cast<unsigned>(base);
    }
    if (negative) {
        buffer[--index] = '-';
    }
    return std::string(buffer + index, sizeof(buffer) - static_cast<std::size_t>(index));
}

std::string formatFloat(double value) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value < 0 ? "-inf" : "inf";
    }

    // Shortest representation that round-trips.
    char buffer[64];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value) {
            break;
        }
    }
    return std::string(buffer);
}

bool parseInt128(const std::string& text, Int128& out) {
    std::string digits;
    digits.reserve(text.size());
    int base = 10;
    std::size_t i = 0;

    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        i = 2;
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        i = 2;
    }

    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '_') {
            continue;
        }
        int digit = -1;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        }
        if (digit < 0 || digit >= base) {
            return false;
        }
        digits.push_back(static_cast<char>(digit));
    }

    if (digits.empty()) {
        return false;
    }

    unsigned __int128 value = 0;
    const unsigned __int128 limit = static_cast<unsigned __int128>(1) << 127;
    for (char digit : digits) {
        const unsigned int d = static_cast<unsigned int>(digit);
        if (value > (limit - d) / static_cast<unsigned>(base)) {
            return false; // overflows int128
        }
        value = value * static_cast<unsigned>(base) + d;
    }

    if (value >= limit) {
        return false;
    }
    out = static_cast<Int128>(value);
    return true;
}

} // namespace acvm
