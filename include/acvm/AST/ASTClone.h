//===--- ASTClone.h - Deep copies of syntax tree nodes ------------------===//
//
// A generic function is compiled by copying its declaration once per distinct
// set of type arguments (see Sema's monomorphisation). The copy has to be a real
// deep copy rather than a shared reference for one concrete reason: Sema writes
// its results back into the tree -- `Expr::type`, `Expr::sym`, `Stmt::funcIndex`,
// `TypeNode::resolved` and friends. Two instantiations sharing one node would
// mean the second overwrites the first, and IRGen (which runs after all of Sema)
// would then lower the first instance using the second's resolved symbols.
//
// Every clone therefore resets the Sema-owned fields to their default
// "not analysed yet" state. Missing a field here is a silent miscompile, so the
// cloners below are written field-by-field on purpose instead of using a copy
// constructor plus fixups.
#pragma once

#include "acvm/AST/AST.h"

namespace acvm {

/// Deep copy of a statement (including nested blocks and bodies).
[[nodiscard]] StmtPtr cloneStmt(const Stmt& stmt);

/// Deep copy of an expression (including all subexpressions).
[[nodiscard]] ExprPtr cloneExpr(const Expr& expr);

/// Deep copy of a type as written, without its resolved type.
[[nodiscard]] TypeNodePtr cloneTypeNode(const TypeNode& node);

} // namespace acvm
