//===--- ASTPrinter.h - Textual dump of the syntax tree ------------------===//
#pragma once

#include <iosfwd>

namespace acvm {

struct Unit;

/// Renders the tree as an indented listing. Used by `acvm ast`.
void dumpAst(const Unit& unit, std::ostream& out);

} // namespace acvm
