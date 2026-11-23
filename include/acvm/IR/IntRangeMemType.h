//===--- IntRangeMemType.h - the MemType overload of inRangeOf ----------===//
//
// Split from Common/IntRange.h so that a layer which has no bytecode types (Sema)
// can still use the overflow rule without pulling in the instruction set.
#pragma once

#include "acvm/Common/IntRange.h"
#include "acvm/IR/Bytecode.h"

namespace acvm {

inline bool inRangeOf(Int128 value, const MemType& type) {
    return inRangeOf(value, type.bits, type.isUnsigned);
}

} // namespace acvm
