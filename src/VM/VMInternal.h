//===--- VMInternal.h - Helpers shared by the VM translation units -----===//
#pragma once

#include "acvm/IR/IntRangeMemType.h"
#include "acvm/Common/Value.h"
#include "acvm/IR/Bytecode.h"

#include <string>

namespace acvm {
namespace vminternal {

// maskFor / signedMinOf / signedMaxOf / inRangeOf moved to Common/IntRange.h:
// the optimizer needs the same overflow rule to fold constants, and a second
// copy would drift silently.

/// Renders a UTF-16 code unit as UTF-8 for output.
inline std::string formatCharacter(std::uint32_t code) {
    std::string result;
    if (code < 0x80) {
        result.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        result.push_back(static_cast<char>(0xC0 | (code >> 6)));
        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        result.push_back(static_cast<char>(0xE0 | (code >> 12)));
        result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    return result;
}

} // namespace vminternal
} // namespace acvm
