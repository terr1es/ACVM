//===--- Disassembler.h - Human readable bytecode listing ---------------===//
#pragma once

#include <cstddef>
#include <iosfwd>

namespace acvm {

struct Module;
struct Function;

void dumpModule(const Module& module, std::ostream& out);

/// One instruction, formatted the way it appears in a listing but without the
/// leading address, source position or trailing newline.
///
/// `dumpFunction` and the debugger both call this, which is the only way the two
/// listings stay identical: a second formatter would drift.
void formatInstruction(const Module& module, const Function& function, std::size_t index,
                       std::ostream& out);
void dumpFunction(const Module& module, const Function& function, std::ostream& out);

} // namespace acvm
