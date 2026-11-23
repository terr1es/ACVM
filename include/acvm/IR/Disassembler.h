//===--- Disassembler.h - Human readable bytecode listing ---------------===//
#pragma once

#include <iosfwd>

namespace acvm {

struct Module;
struct Function;

void dumpModule(const Module& module, std::ostream& out);
void dumpFunction(const Module& module, const Function& function, std::ostream& out);

} // namespace acvm
