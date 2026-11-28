//===--- SemaExpr.cpp - Expression checking, literals and calls ----------===//
#include "acvm/Sema/Sema.h"

#include "acvm/Common/Diagnostic.h"
#include "acvm/Common/IntRange.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace acvm {

namespace {

/// Defined below, next to `checkAssign`. Forward declared so `checkBinary` can
/// report constant traps while it is still working out the result type.
void warnAboutConstantTrap(DiagnosticEngine& diagnostics, const Expr& expr, const Type& type);

} // namespace
namespace {

/// Bounds of a type for literal range checks.
///
/// These used to be their own copies of the arithmetic in Common/IntRange.h, and
/// they had already drifted: both still clamped at 127 bits, so a `int128`
/// literal was rejected with "constant '-1' is out of range" -- the `-1` being
/// the overflowed max of a type that no longer had one. One definition now, so a
/// boundary can only be wrong in one place.
Int128 literalMin(const Type* type) {
    if (type->isUnsigned) {
        return 0;
    }
    return signedMinOf(type->bits);
}

Int128 literalMax(const Type* type) {
    if (type->isUnsigned) {
        // An unsigned type uses its full width, but a *value* still has to fit the
        // signed `Int128` the VM carries, so 2^127−1 is the ceiling regardless of
        // the declared width. That is exactly why `unsigned int128` is rejected in
        // Sema: its maximum has no representation here at all.
        if (type->bits >= 127) {
            return static_cast<Int128>((static_cast<unsigned __int128>(1) << 127) - 1);
        }
        return static_cast<Int128>((static_cast<unsigned __int128>(1) << type->bits) - 1);
    }
    return signedMaxOf(type->bits);
}

bool isScalarOrString(const Type* type) {
    if (type == nullptr) {
        return false;
    }
    switch (type->kind) {
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Int:
    case TypeKind::Bit:
    case TypeKind::Float:
    case TypeKind::Ptr:
    case TypeKind::Enum:
    case TypeKind::String:
        return true;
    default:
        return false;
    }
}

} // namespace

//===----------------------------------------------------------------------===//
// Literals
//===----------------------------------------------------------------------===//

bool Sema::isLiteralExpr(const Expr& expr) const {
    switch (expr.kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::CharLit:
    case ExprKind::BoolLit:
    case ExprKind::StringLit:
    case ExprKind::NullLit:
        return true;
    case ExprKind::Unary:
        return (expr.unop == UnOp::Neg || expr.unop == UnOp::Plus) &&
               expr.kids[0]->kind == ExprKind::IntLit;
    default:
        return false;
    }
}

namespace {

/// Resolves the literal core of an expression: a plain literal or one wrapped
/// in unary minus / plus.
Expr* literalCore(Expr& expr, bool& negated) {
    negated = false;
    Expr* current = &expr;
    while (current->kind == ExprKind::Unary &&
           (current->unop == UnOp::Neg || current->unop == UnOp::Plus)) {
        if (current->unop == UnOp::Neg) {
            negated = !negated;
        }
        current = current->kids[0].get();
    }
    return current;
}

} // namespace

bool Sema::adaptLiteral(Expr& expr, const Type* target) {
    if (target == nullptr || target->isError()) {
        return false;
    }

    bool negated = false;
    Expr* core = literalCore(expr, negated);
    if (core == nullptr) {
        return false;
    }

    switch (core->kind) {
    case ExprKind::IntLit: {
        if (target->isFloat()) {
            const Int128 magnitude = core->lit.intValue;
            const double value = static_cast<double>(negated ? -magnitude : magnitude);
            expr.type = target;
            expr.literalTyped = true;
            expr.isConstExpr = true;
            expr.constIsFloat = true;
            expr.constFloat = value;
            return true;
        }
        if (!target->isInt() && !target->isEnum() && target->kind != TypeKind::Char &&
            target->kind != TypeKind::Bool && !target->isBit()) {
            return false;
        }
        Int128 value = negated ? -core->lit.intValue : core->lit.intValue;
        if (target->kind == TypeKind::Bool) {
            if (value != 0 && value != 1) {
                return false;
            }
        } else if (target->kind == TypeKind::Char) {
            if (value < 0 || value > 65535) {
                m_diagnostics.error(expr.range, "constant is out of range for 'char'");
                return false;
            }
        } else if (value < literalMin(target) || value > literalMax(target)) {
            // formatInt128, not `static_cast<long long>`: truncating to 64 bits
            // printed a value as '-1' when it was 2^127−1, which sent the reader
            // looking for a negative sign that was never in the source.
            m_diagnostics.error(expr.range, "constant '" + formatInt128(value) +
                                                "' is out of range for '" + target->describe() + "'");
            return false;
        }
        expr.type = target;
        expr.literalTyped = true;
        expr.literalOutOfRange = false;
        expr.isConstExpr = true;
        expr.constIsFloat = false;
        expr.constInt = value;
        return true;
    }
    case ExprKind::FloatLit: {
        if (!target->isFloat()) {
            return false;
        }
        expr.type = target;
        expr.literalTyped = true;
        expr.isConstExpr = true;
        expr.constIsFloat = true;
        expr.constFloat = core->lit.floatValue;
        return true;
    }
    case ExprKind::CharLit: {
        // A character literal stays a character. Letting it decay into an
        // integer is what made `count + '\n'` print 10 instead of a newline;
        // writing a code point is still possible the other way round, with
        // `char c = 65;` (an integer literal adapting to `char`).
        if (target->kind != TypeKind::Char) {
            return false;
        }
        expr.type = target;
        expr.literalTyped = true;
        expr.isConstExpr = true;
        expr.constInt = static_cast<Int128>(core->lit.charValue);
        return true;
    }
    case ExprKind::BoolLit:
        if (target->kind != TypeKind::Bool) {
            return false;
        }
        expr.type = target;
        expr.literalTyped = true;
        expr.isConstExpr = true;
        expr.constInt = core->lit.intValue;
        return true;
    case ExprKind::StringLit:
        if (target->kind != TypeKind::String) {
            return false;
        }
        expr.type = target;
        return true;
    case ExprKind::NullLit:
        if (!target->isPtr()) {
            return false;
        }
        expr.type = target;
        return true;
    default:
        return false;
    }
}

//===----------------------------------------------------------------------===//
// Dispatch
//===----------------------------------------------------------------------===//

const Type* Sema::checkExpr(Expr& expr, Scope& scope, Flow& flow) {
    const Type* type = m_types.errorType();

    switch (expr.kind) {
    case ExprKind::IntLit: {
        const Type* literalType = m_types.intType();
        switch (expr.lit.suffix) {
        case LitValue::Suffix::I: {
            const unsigned bits = expr.lit.suffixBits == 0 ? 32 : expr.lit.suffixBits;
            literalType = m_types.getInt(bits, false);
            break;
        }
        case LitValue::Suffix::U: {
            const unsigned bits = expr.lit.suffixBits == 0 ? 32 : expr.lit.suffixBits;
            literalType = m_types.getInt(bits, true);
            break;
        }
        case LitValue::Suffix::F: {
            const unsigned bits = expr.lit.suffixBits == 0 ? 32 : expr.lit.suffixBits;
            literalType = m_types.getFloat(bits, 0, 0);
            break;
        }
        case LitValue::Suffix::D:
            literalType = m_types.doubleType();
            break;
        case LitValue::Suffix::None:
            break;
        }

        if (literalType->isError()) {
            m_diagnostics.error(expr.range, "invalid literal suffix in '" + expr.lit.text + "'");
            type = m_types.errorType();
            break;
        }
        if (literalType->isInt() &&
            (expr.lit.intValue < literalMin(literalType) ||
             expr.lit.intValue > literalMax(literalType))) {
            if (expr.lit.suffix != LitValue::Suffix::None) {
                m_diagnostics.error(expr.range, "constant '" + expr.lit.text +
                                                    "' is out of range for '" +
                                                    literalType->describe() + "'");
                type = m_types.errorType();
                break;
            }
            // The literal may still be retyped by its context; the deferred
            // check runs once the whole unit has been checked.
            expr.literalOutOfRange = true;
        }
        expr.isConstExpr = true;
        expr.constInt = expr.lit.intValue;
        type = literalType;
        break;
    }
    case ExprKind::FloatLit: {
        const Type* literalType = m_types.doubleType();
        switch (expr.lit.suffix) {
        case LitValue::Suffix::F: {
            const unsigned bits = expr.lit.suffixBits == 0 ? 32 : expr.lit.suffixBits;
            literalType = m_types.getFloat(bits, 0, 0);
            break;
        }
        case LitValue::Suffix::D:
            literalType = m_types.doubleType();
            break;
        default:
            break;
        }
        if (literalType->isError()) {
            m_diagnostics.error(expr.range, "invalid literal suffix in '" + expr.lit.text + "'");
            type = m_types.errorType();
            break;
        }
        expr.isConstExpr = true;
        expr.constIsFloat = true;
        expr.constFloat = expr.lit.floatValue;
        type = literalType;
        break;
    }
    case ExprKind::CharLit:
        expr.isConstExpr = true;
        expr.constInt = static_cast<Int128>(expr.lit.charValue);
        type = m_types.charType();
        break;
    case ExprKind::StringLit:
        type = m_types.stringType();
        break;
    case ExprKind::BoolLit:
        expr.isConstExpr = true;
        expr.constInt = expr.lit.intValue;
        type = m_types.boolType();
        break;
    case ExprKind::NullLit:
        type = m_null;
        break;
    case ExprKind::Name:
        return checkName(expr, scope, flow);
    case ExprKind::Unary:
        return checkUnary(expr, scope, flow);
    case ExprKind::Binary:
        return checkBinary(expr, scope, flow);
    case ExprKind::Assign:
        return checkAssign(expr, scope, flow);
    case ExprKind::Ternary:
        return checkTernary(expr, scope, flow);
    case ExprKind::Cast:
        return checkCast(expr, scope, flow);
    case ExprKind::Call:
        return checkCall(expr, scope, flow);
    case ExprKind::Index:
        return checkIndex(expr, scope, flow);
    case ExprKind::Slice:
        return checkSlice(expr, scope, flow);
    case ExprKind::Member:
    case ExprKind::Arrow:
        return checkMember(expr, scope, flow);
    case ExprKind::PreInc:
    case ExprKind::PreDec:
    case ExprKind::PostInc:
    case ExprKind::PostDec: {
        Expr& operand = *expr.kids[0];
        const Type* operandType = checkExpr(operand, scope, flow);
        if (operandType->isError()) {
            type = operandType;
            break;
        }
        if (!operand.isLValue) {
            m_diagnostics.error(operand.range, "the operand of '++' / '--' must be a variable");
        }
        if (operandType->isPtr()) {
            // `p++` advances by one element, exactly like `p + 1` (spec §4.2.2).
            if (operandType->elem->isVoid()) {
                m_diagnostics.error(expr.range,
                                    "'ptr void' cannot be stepped because 'void' has no size");
                type = m_types.errorType();
                break;
            }
        } else if (!operandType->isNumeric() && operandType->kind != TypeKind::Char) {
            m_diagnostics.error(expr.range, "'++' / '--' requires a numeric operand, a 'char' "
                                            "or a pointer, found '" + operandType->describe() +
                                                "'");
            type = m_types.errorType();
            break;
        }
        if (Symbol* root = lvalueRoot(operand); root != nullptr && !root->isMutable) {
            m_diagnostics.error(operand.range,
                                "cannot modify const " +
                                    std::string(root->kind == Symbol::Kind::Param ? "parameter '"
                                                                                  : "variable '") +
                                    root->name + "'");
        }
        if (Symbol* root = lvalueRoot(operand)) {
            flow.assigned.insert(root);
        }
        type = operandType;
        break;
    }
    case ExprKind::TupleLit: {
        std::vector<const Type*> elements;
        for (ExprPtr& kid : expr.kids) {
            elements.push_back(checkExpr(*kid, scope, flow));
        }
        type = m_types.getTuple(std::move(elements));
        break;
    }
    case ExprKind::InitList:
        m_diagnostics.error(expr.range,
                            "a '{...}' initialiser is only allowed in a variable declaration");
        break;
    case ExprKind::SizeOf:
    case ExprKind::AlignOf: {
        const Type* operand = nullptr;
        if (expr.typeNode) {
            operand = resolveTypeNode(*expr.typeNode, scope);
        } else if (!expr.kids.empty()) {
            operand = checkExpr(*expr.kids[0], scope, flow);
        }
        if (operand == nullptr || operand->isError()) {
            type = m_types.errorType();
            break;
        }
        if (operand->isVoid()) {
            m_diagnostics.error(expr.range, "'sizeof' / 'alignof' of 'void' is not defined");
            type = m_types.errorType();
            break;
        }
        expr.isConstExpr = true;
        expr.constInt = expr.kind == ExprKind::SizeOf ? static_cast<Int128>(operand->sizeBytes)
                                                      : static_cast<Int128>(operand->alignBytes);
        type = m_types.intType();
        break;
    }
    case ExprKind::Range:
        m_diagnostics.error(expr.range,
                            "a range '..' is only allowed inside a slice or a 'for ... in' loop");
        type = m_types.errorType();
        break;
    }

    expr.type = type;
    return type;
}

//===----------------------------------------------------------------------===//
// Names
//===----------------------------------------------------------------------===//

const Type* Sema::checkName(Expr& expr, Scope& scope, Flow& flow) {
    Symbol* symbol = scope.lookup(expr.text);
    if (symbol == nullptr) {
        m_diagnostics.error(expr.range, "undefined variable '" + expr.text + "'");
        if (std::string suggestion = suggestName(expr.text, scope); !suggestion.empty()) {
            m_diagnostics.help(expr.range.begin, "did you mean '" + suggestion + "'?");
        }
        expr.type = m_types.errorType();
        return expr.type;
    }

    expr.sym = symbol;

    switch (symbol->kind) {
    case Symbol::Kind::Local:
    case Symbol::Kind::Param:
    case Symbol::Kind::Global:
        if (!m_lvalueContext) {
            checkValueReadable(*symbol, flow, expr.range);
        }
        expr.isLValue = true;
        break;
    case Symbol::Kind::Constant:
        expr.isLValue = true;
        break;
    case Symbol::Kind::Comptime:
        expr.isConstExpr = true;
        expr.constIsFloat = symbol->constIsFloat;
        expr.constInt = symbol->constInt;
        expr.constFloat = symbol->constFloat;
        break;
    case Symbol::Kind::EnumConstant:
        expr.isConstExpr = true;
        expr.constInt = static_cast<Int128>(symbol->enumValue);
        break;
    case Symbol::Kind::Function:
        m_diagnostics.error(expr.range, "function '" + expr.text +
                                            "' can only be called, not used as a value");
        expr.type = m_types.errorType();
        return expr.type;
    case Symbol::Kind::Builtin:
        m_diagnostics.error(expr.range, "'" + expr.text + "' is a builtin and can only be called");
        expr.type = m_types.errorType();
        return expr.type;
    case Symbol::Kind::TypeName:
        m_diagnostics.error(expr.range,
                            "'" + expr.text + "' is a type, not a value");
        expr.type = m_types.errorType();
        return expr.type;
    case Symbol::Kind::Error:
        break;
    }

    expr.type = symbol->type;
    return expr.type;
}

//===----------------------------------------------------------------------===//
// Operators
//===----------------------------------------------------------------------===//

const Type* Sema::checkUnary(Expr& expr, Scope& scope, Flow& flow) {
    Expr& operand = *expr.kids[0];

    // `#x` computes an address; it never reads x, so an uninitialised buffer
    // may legitimately be handed to a function that fills it.
    const bool addressesOperand = expr.unop == UnOp::AddrOf;
    if (addressesOperand) {
        m_lvalueContext = true;
    }
    const Type* operandType = checkExpr(operand, scope, flow);
    if (addressesOperand) {
        m_lvalueContext = false;
    }

    if (operandType->isError()) {
        expr.type = operandType;
        return expr.type;
    }

    switch (expr.unop) {
    case UnOp::Plus:
        if (!operandType->isNumeric()) {
            m_diagnostics.error(expr.range, "unary '+' requires a numeric operand, found '" +
                                                operandType->describe() + "'");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.type = operandType;
        break;

    case UnOp::Neg:
        if (!operandType->isNumeric()) {
            m_diagnostics.error(expr.range, "unary '-' requires a numeric operand, found '" +
                                                operandType->describe() + "'");
            expr.type = m_types.errorType();
            return expr.type;
        }
        if (operandType->isUnsigned && operandType->isInt()) {
            m_diagnostics.warning(expr.range, "negating an unsigned value");
        }
        expr.type = operandType;
        if (operand.isConstExpr && !operand.constIsFloat) {
            expr.isConstExpr = true;
            expr.constInt = -operand.constInt;
        }
        break;

    case UnOp::Not:
        if (!isScalarOrString(operandType) || operandType->kind == TypeKind::String) {
            m_diagnostics.error(expr.range, "'!' requires a scalar operand, found '" +
                                                operandType->describe() + "'");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.type = m_types.boolType();
        break;

    case UnOp::BitNot:
        if (!operandType->isInt() && !operandType->isEnum()) {
            m_diagnostics.error(expr.range, "'~' requires an integer operand, found '" +
                                                operandType->describe() + "'");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.type = operandType;
        break;

    case UnOp::Deref: {
        if (operandType->kind == TypeKind::Null) {
            m_diagnostics.error(expr.range, "cannot dereference 'null'");
            expr.type = m_types.errorType();
            return expr.type;
        }
        if (!operandType->isPtr()) {
            m_diagnostics.error(expr.range, "'@' requires a pointer operand, found '" +
                                                operandType->describe() + "'");
            expr.type = m_types.errorType();
            return expr.type;
        }
        if (operandType->elem->isVoid()) {
            m_diagnostics.error(expr.range,
                                "'ptr void' cannot be dereferenced; cast it to a concrete pointer "
                                "type first");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.isLValue = true;
        expr.type = operandType->elem;
        break;
    }

    case UnOp::AddrOf: {
        if (!operand.isLValue) {
            m_diagnostics.error(expr.range, "'#' requires a variable, a member or an element");
            expr.type = m_types.errorType();
            return expr.type;
        }
        // A bit field may start mid-byte, so it has no address the VM could name.
        // C forbids `&` on bit fields for the same reason; the difference is that
        // here it is an error at the point of use rather than a vague one later.
        if (operandType != nullptr && operandType->isBit()) {
            m_diagnostics.error(expr.range,
                                "cannot take the address of '" + operandType->describe() +
                                    "': a bit string has no address; read the value instead of "
                                    "pointing at it");
            expr.type = m_types.errorType();
            return expr.type;
        }
        Symbol* root = lvalueRoot(operand);
        if (root != nullptr && !root->isMutable) {
            m_diagnostics.error(expr.range,
                                "cannot take the address of const " +
                                    std::string(root->kind == Symbol::Kind::Param ? "parameter '"
                                                                                  : "variable '") +
                                    root->name + "'");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.type = m_types.getPtr(operandType);
        break;
    }
    }

    return expr.type;
}

const Type* Sema::checkBinary(Expr& expr, Scope& scope, Flow& flow) {
    const Type* lhsType = checkExpr(*expr.kids[0], scope, flow);
    const Type* rhsType = checkExpr(*expr.kids[1], scope, flow);

    if (lhsType->isError() || rhsType->isError()) {
        expr.type = m_types.errorType();
        return expr.type;
    }

    // A literal operand adopts the type of the other side, so that `u + 1`
    // works for any numeric width (spec §2.4.1.1). A bit string takes the same
    // treatment for comparison only: `flags[i] == 1` should not force the reader to
    // write `1 as bit1`, even though the type refuses every arithmetic operator.
    const auto takesLiteral = [](const Type* type) {
        return type != nullptr && (type->isNumeric() || type->isBit());
    };
    if (isLiteralExpr(*expr.kids[0]) && !isLiteralExpr(*expr.kids[1]) && takesLiteral(rhsType)) {
        if (adaptLiteral(*expr.kids[0], rhsType) && expr.kids[0]->type != nullptr) {
            lhsType = expr.kids[0]->type;
        }
    } else if (isLiteralExpr(*expr.kids[1]) && !isLiteralExpr(*expr.kids[0]) &&
               takesLiteral(lhsType)) {
        if (adaptLiteral(*expr.kids[1], lhsType) && expr.kids[1]->type != nullptr) {
            rhsType = expr.kids[1]->type;
        }
    }

    // Logical operators short-circuit and produce bool.
    if (expr.binop == BinOp::LogAnd || expr.binop == BinOp::LogOr) {
        const auto usable = [](const Type* type) {
            return type->isNumeric() || type->kind == TypeKind::Bool ||
                   type->kind == TypeKind::Ptr || type->kind == TypeKind::Char;
        };
        if (!usable(lhsType) || !usable(rhsType)) {
            m_diagnostics.error(expr.range,
                                "operator '" + std::string(binOpName(expr.binop)) +
                                    "' requires scalar operands, found '" + lhsType->describe() +
                                    "' and '" + rhsType->describe() + "'");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.type = m_types.boolType();
        return expr.type;
    }

    // String concatenation: `+` with a string on either side.
    if (expr.binop == BinOp::Add &&
        (lhsType->kind == TypeKind::String || rhsType->kind == TypeKind::String)) {
        if (!isScalarOrString(lhsType) || !isScalarOrString(rhsType)) {
            m_diagnostics.error(expr.range,
                                "'+' cannot concatenate '" + lhsType->describe() + "' and '" +
                                    rhsType->describe() + "'");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.type = m_types.stringType();
        return expr.type;
    }

    // Pointer arithmetic.
    if (lhsType->isPtr() || rhsType->isPtr()) {
        const auto isInteger = [](const Type* type) {
            return type->isInt() || type->kind == TypeKind::Char ||
                   type->kind == TypeKind::Enum || type->kind == TypeKind::Bool;
        };
        const bool comparison = expr.binop == BinOp::Eq || expr.binop == BinOp::Ne ||
                                expr.binop == BinOp::Lt || expr.binop == BinOp::Le ||
                                expr.binop == BinOp::Gt || expr.binop == BinOp::Ge;

        if (comparison) {
            const bool sameType = m_types.same(lhsType, rhsType);
            const bool nullInvolved = lhsType->kind == TypeKind::Null ||
                                      rhsType->kind == TypeKind::Null;
            if (!sameType && !nullInvolved) {
                m_diagnostics.error(expr.range, "cannot compare '" + lhsType->describe() +
                                                    "' with '" + rhsType->describe() +
                                                    "'; pointers must have the same type");
            }
            expr.type = m_types.boolType();
            return expr.type;
        }

        if (expr.binop == BinOp::Add || expr.binop == BinOp::Sub) {
            if (lhsType->isPtr() && rhsType->isPtr()) {
                if (expr.binop != BinOp::Sub) {
                    m_diagnostics.error(expr.range, "two pointers cannot be added");
                    expr.type = m_types.errorType();
                    return expr.type;
                }
                expr.type = m_types.getInt(64, false);
                return expr.type;
            }
            const Type* pointer = lhsType->isPtr() ? lhsType : rhsType;
            const Type* other = lhsType->isPtr() ? rhsType : lhsType;
            if (!isInteger(other)) {
                m_diagnostics.error(expr.range, "pointer arithmetic requires an integer offset, "
                                                "found '" + other->describe() + "'");
                expr.type = m_types.errorType();
                return expr.type;
            }
            if (pointer->elem->isVoid()) {
                m_diagnostics.error(expr.range,
                                    "'ptr void' cannot be used in pointer arithmetic because "
                                    "'void' has no size");
                expr.type = m_types.errorType();
                return expr.type;
            }
            expr.type = pointer;
            return expr.type;
        }

        m_diagnostics.error(expr.range, "operator '" + std::string(binOpName(expr.binop)) +
                                            "' is not defined for pointers");
        expr.type = m_types.errorType();
        return expr.type;
    }

    // `char` and `bool` are not numbers: mixing them into arithmetic silently
    // turns a newline into the number 10 (spec §4.2.2 already makes `bool` +
    // `intN` a hard error, and `char` follows the same rule).
    const auto rejectsArithmetic = [](const Type* type) {
        return type->kind == TypeKind::Char || type->kind == TypeKind::Bool;
    };
    const auto isArithmeticLike = [](BinOp op) {
        switch (op) {
        case BinOp::Add:
        case BinOp::Sub:
        case BinOp::Mul:
        case BinOp::Div:
        case BinOp::Mod:
        case BinOp::Pow:
        case BinOp::BitAnd:
        case BinOp::BitOr:
        case BinOp::BitXor:
        case BinOp::Shl:
        case BinOp::Shr:
            return true;
        default:
            return false;
        }
    };
    if (isArithmeticLike(expr.binop) &&
        (rejectsArithmetic(lhsType) || rejectsArithmetic(rhsType))) {
        const Type* offender = rejectsArithmetic(lhsType) ? lhsType : rhsType;
        const std::string advice =
            offender->kind == TypeKind::Char
                ? "convert it first, e.g. '\\n' as int; to build a string use "
                  "'\"\" + value + '\\n''"
                : "convert it first, e.g. 'b as int'";
        m_diagnostics.error(expr.range, "operator '" + std::string(binOpName(expr.binop)) +
                                            "' cannot be applied to '" + offender->describe() +
                                            "'; " + advice);
        expr.type = m_types.errorType();
        return expr.type;
    }

    // Shifts keep the left hand type.
    if (expr.binop == BinOp::Shl || expr.binop == BinOp::Shr) {
        if (!lhsType->isIntegerLike() || !rhsType->isIntegerLike()) {
            m_diagnostics.error(expr.range, "shift operators require integer operands, found '" +
                                                lhsType->describe() + "' and '" +
                                                rhsType->describe() + "'");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.type = lhsType->kind == TypeKind::Char || lhsType->kind == TypeKind::Bool
                        ? m_types.getInt(32, false)
                        : lhsType;
        // Shifts return early, so they need their own call: the one at the end of
        // the arithmetic path is never reached for them.
        warnAboutConstantTrap(m_diagnostics, expr, *expr.type);
        return expr.type;
    }

    // Bitwise operators require integers.
    if (expr.binop == BinOp::BitAnd || expr.binop == BinOp::BitOr || expr.binop == BinOp::BitXor) {
        if (!lhsType->isIntegerLike() || !rhsType->isIntegerLike()) {
            m_diagnostics.error(expr.range,
                                "bitwise operators require integer operands, found '" +
                                    lhsType->describe() + "' and '" + rhsType->describe() + "'");
            expr.type = m_types.errorType();
            return expr.type;
        }
    }

    // Comparison.
    if (expr.binop == BinOp::Eq || expr.binop == BinOp::Ne || expr.binop == BinOp::Lt ||
        expr.binop == BinOp::Le || expr.binop == BinOp::Gt || expr.binop == BinOp::Ge) {
        const bool comparable = m_types.same(lhsType, rhsType) ||
                                (lhsType->isNumeric() && rhsType->isNumeric());
        if (!comparable) {
            m_diagnostics.error(expr.range, "cannot compare '" + lhsType->describe() +
                                                "' with '" + rhsType->describe() + "'");
            expr.type = m_types.errorType();
            return expr.type;
        }

        // Ordering is not defined for aggregates. `==` / `!=` are (they compare
        // storage bytes, D8), but `<` has no meaning: field order is arbitrary, and
        // byte order is not what anyone means by "smaller".
        //
        // The implementation used to fall through to pointer comparison here, so
        // `{1,2} < {1,2}` was *true* -- the result followed the two variables'
        // stack offsets, not their contents. Silently wrong, and reachable through
        // precisely the code that would trust it: `sortIndicesByKey<T>` and
        // `isSortedByKey<T>` compare keys with `>`, so a struct key was ordered by
        // address and `isSortedByKey` then certified the result as sorted.
        //
        // `string` is the exception: its ordering is defined (three-way compare on
        // the bytes, StrCmp) and long-standing, so it stays.
        const bool isOrdering = expr.binop == BinOp::Lt || expr.binop == BinOp::Le ||
                                expr.binop == BinOp::Gt || expr.binop == BinOp::Ge;
        if (isOrdering && lhsType->isAggregate() && lhsType->kind != TypeKind::String) {
            m_diagnostics.error(expr.range,
                                "operator '" + std::string(binOpName(expr.binop)) +
                                    "' is not defined for '" + lhsType->describe() + "'");
            m_diagnostics.help(expr.range.begin,
                               "compare the fields you mean ('a.x < b.x'), or sort by a key with "
                               "sortIndicesByKey");
            expr.type = m_types.errorType();
            return expr.type;
        }

        expr.type = m_types.boolType();
        return expr.type;
    }

    // Arithmetic.
    //
    // A bit string is storage, not a number: `bit3 + 1` has no meaning the language
    // is willing to guess at (does the result wrap at 3 bits, or widen?). Saying so
    // here keeps the generic "mixed signed and unsigned" message for the case it
    // was written for.
    if ((lhsType != nullptr && lhsType->isBit()) || (rhsType != nullptr && rhsType->isBit())) {
        const Type* bitSide = (lhsType != nullptr && lhsType->isBit()) ? lhsType : rhsType;
        m_diagnostics.error(expr.range, "operator '" + std::string(binOpName(expr.binop)) +
                                            "' is not defined for '" + bitSide->describe() +
                                            "': a bit string is storage, not a number; convert it "
                                            "with 'as int' first");
        expr.type = m_types.errorType();
        return expr.type;
    }
    // `**` is integer exponentiation, and it says so: a floating point base or
    // exponent is refused with a pointer to `pow` rather than silently routed
    // through it. Two spellings of one operation that disagree in rounding and in
    // result type is how a program ends up with a double where it expected an int.
    if (expr.binop == BinOp::Pow) {
        const auto integral = [](const Type* type) {
            return type != nullptr && type->isNumeric() && !type->isFloat();
        };
        if (!integral(lhsType) || !integral(rhsType)) {
            const Type* offender = integral(lhsType) ? rhsType : lhsType;
            m_diagnostics.error(expr.range,
                                "'**' is integer exponentiation; '" + offender->describe() +
                                    "' is not an integer type");
            m_diagnostics.help(expr.range.begin,
                               "use 'pow(base, exponent)' for floating point, or cast with "
                               "'as int' if an integer result is what you want");
            expr.type = m_types.errorType();
            return expr.type;
        }
        // A negative exponent has no integer result: 2 ** -1 is 0.5. Catching a
        // constant here turns a runtime trap into a compile time error at the line
        // that wrote it.
        if (expr.kids[1]->isConstExpr && !expr.kids[1]->constIsFloat &&
            expr.kids[1]->constInt < 0) {
            m_diagnostics.error(expr.kids[1]->range,
                                "a negative exponent has no integer result; the value would be "
                                "a fraction, and '**' produces an integer");
            m_diagnostics.help(expr.kids[1]->range.begin,
                               "use 'pow' for a floating point result, or negate the whole "
                               "expression if you meant the reciprocal");
            expr.type = m_types.errorType();
            return expr.type;
        }
    }

    const Type* merged = m_types.mergeArithmetic(lhsType, rhsType);
    if (merged == nullptr) {
        // `mergeArithmetic` answers null for two unrelated reasons: a signed /
        // unsigned clash, and an operand that is not arithmetic at all. Telling
        // two identical struct types that they are "mixed signed and unsigned"
        // sends the reader looking for a sign problem that cannot exist. This
        // matters more now that a generic body can be checked with `T` bound to
        // a struct: the message is the only thing explaining which operation the
        // template cannot do for that type.
        const bool bothSameNonArithmetic =
            m_types.same(lhsType, rhsType) && !lhsType->isNumeric() &&
            lhsType->kind != TypeKind::Char && lhsType->kind != TypeKind::Ptr;
        if (bothSameNonArithmetic) {
            m_diagnostics.error(expr.range, "operator '" + std::string(binOpName(expr.binop)) +
                                                "' requires a numeric operand, but '" +
                                                lhsType->describe() +
                                                "' is not one; Axiom has no operator overloading, "
                                                "so a generic over this type cannot use arithmetic");
        } else {
            m_diagnostics.error(expr.range,
                                "mixed signed and unsigned operands ('" + lhsType->describe() +
                                    "' and '" + rhsType->describe() +
                                    "'); cast one explicitly with 'as'");
        }
        expr.type = m_types.errorType();
        return expr.type;
    }
    if (!merged->isNumeric() && merged->kind != TypeKind::Char) {
        m_diagnostics.error(expr.range, "operator '" + std::string(binOpName(expr.binop)) +
                                            "' requires numeric operands, found '" +
                                            lhsType->describe() + "' and '" +
                                            rhsType->describe() + "'");
        expr.type = m_types.errorType();
        return expr.type;
    }
    expr.type = merged;
    warnAboutConstantTrap(m_diagnostics, expr, *merged);
    return expr.type;
}

namespace {

/// Warns when a binary operation on two compile-time constants is certain to trap.
///
/// This is the compile-time counterpart of the VM's overflow and division checks:
/// `int x = 1 / 0;` is reported here, with the caret on the expression, instead of
/// on the first execution. The rules come from `Common/IntRange.h` -- the same ones
/// the VM applies -- so a warning always means a trap at run time, never a false
/// alarm.
///
/// It lives in Sema rather than IRGen so that `acvm check` reports it too: `check`
/// stops after type checking and never builds bytecode, and a diagnostic that only
/// appears when you run the program is a diagnostic you find too late.
///
/// A warning rather than an error on purpose. A constant expression still sits
/// inside some branch, and
///
///     if (mode == Fast) { return 1 / 0; }
///
/// is a legitimate way to write "not supported in this mode". Rejecting the
/// program would be wrong.
void warnAboutConstantTrap(DiagnosticEngine& diagnostics, const Expr& expr, const Type& type) {
    if (expr.kids.size() < 2 || !type.isInt()) {
        return;
    }
    const Expr& lhs = *expr.kids[0];
    const Expr& rhs = *expr.kids[1];
    if (!lhs.isConstExpr || !rhs.isConstExpr || lhs.constIsFloat || rhs.constIsFloat) {
        return;
    }

    const unsigned bits = type.bits == 0 ? 32 : type.bits;
    const Int128 a = lhs.constInt;
    const Int128 b = rhs.constInt;
    const bool divisor = expr.binop == BinOp::Div || expr.binop == BinOp::Mod;

    if (type.isUnsigned) {
        // Unsigned arithmetic wraps instead of trapping; only division can trap.
        if (divisor && (b & static_cast<Int128>(maskFor(bits))) == 0) {
            diagnostics.warning(expr.range, "constant zero divisor; this traps when reached");
        }
        return;
    }

    if (divisor) {
        if (b == 0) {
            diagnostics.warning(expr.range, "constant zero divisor; this traps when reached");
            return;
        }
        if (expr.binop == BinOp::Div && a == signedMinOf(bits) && b == -1) {
            diagnostics.warning(expr.range,
                                "constant division overflows; this traps when reached");
        }
        return;
    }

    if (expr.binop == BinOp::Add || expr.binop == BinOp::Sub || expr.binop == BinOp::Mul) {
        Int128 result = 0;
        bool overflowed = false;
        if (expr.binop == BinOp::Add) {
            overflowed = __builtin_add_overflow(a, b, &result);
        } else if (expr.binop == BinOp::Sub) {
            overflowed = __builtin_sub_overflow(a, b, &result);
        } else {
            overflowed = __builtin_mul_overflow(a, b, &result);
        }
        if (overflowed || result < signedMinOf(bits) || result > signedMaxOf(bits)) {
            diagnostics.warning(expr.range,
                                "constant arithmetic overflows the result type; "
                                "this traps when reached");
        }
        return;
    }

    if (expr.binop == BinOp::Shl) {
        // The VM traps on an out-of-range amount unless the value is zero, and on
        // a shift that discards bits.
        if (b < 0 || b >= static_cast<Int128>(bits)) {
            if (a != 0) {
                diagnostics.warning(expr.range, "constant shift amount is out of range; "
                                                "this traps when reached");
            }
        } else if ((static_cast<unsigned __int128>(a) << static_cast<unsigned>(b)) & ~maskFor(bits)) {
            diagnostics.warning(expr.range, "constant shift discards bits; "
                                            "this traps when reached");
        }
        return;
    }

    if (expr.binop == BinOp::Pow) {
        // `2 ** 40` in an `int` is the case this exists for: it reads like a literal
        // and fails at run time. Saying so at the line that wrote it is the difference
        // between a one second fix and a debug session.
        if (b < 0) {
            diagnostics.warning(expr.range, "constant exponent is negative; "
                                            "this traps when reached");
            return;
        }
        Int128 accumulator = 1;
        Int128 base = a;
        Int128 exponent = b;
        bool overflowed = false;
        while (exponent > 0 && !overflowed) {
            if ((exponent & 1) != 0) {
                overflowed = __builtin_mul_overflow(accumulator, base, &accumulator);
            }
            exponent >>= 1;
            if (exponent > 0) {
                overflowed = __builtin_mul_overflow(base, base, &base);
            }
        }
        if (overflowed || accumulator < signedMinOf(bits) || accumulator > signedMaxOf(bits)) {
            diagnostics.warning(expr.range,
                                "constant exponentiation overflows the result type; "
                                "this traps when reached");
            // Two different problems wear the same symptom, and the advice differs.
            // `2 ** 40` in an `int` only needs a wider type; `2 ** 128` is past the
            // largest integer the language has, and widening cannot reach it.
            if (overflowed) {
                diagnostics.help(expr.range.begin,
                                 "the value does not fit any integer type: 'int128' holds at most "
                                 "2^127-1, so 2^128 is out of range; restate the constant or "
                                 "compute it at run time");
            } else {
                diagnostics.help(expr.range.begin,
                                 "widen the operand first, e.g. '(2 as int64) ** 40', or compute it "
                                 "at run time where the exponent is not a constant");
            }
        }
    }
}

} // namespace

const Type* Sema::checkAssign(Expr& expr, Scope& scope, Flow& flow) {
    Expr& target = *expr.kids[0];
    Expr& value = *expr.kids[1];

    const Type* targetType = nullptr;
    if (target.kind == ExprKind::Name) {
        // Assigning defines the variable, so the definite-assignment read
        // check must not run on the target.
        Symbol* symbol = scope.lookup(target.text);
        if (symbol == nullptr) {
            m_diagnostics.error(target.range, "undefined variable '" + target.text + "'");
            targetType = m_types.errorType();
        } else {
            target.sym = symbol;
            target.type = symbol->type;
            target.isLValue = true;
            targetType = symbol->type;
        }
    } else {
        // The target of an assignment is written, not read: `a[3] = 7;` must
        // not require `a` to have been initialised.
        m_lvalueContext = true;
        targetType = checkExpr(target, scope, flow);
        m_lvalueContext = false;
    }

    if (!target.isLValue) {
        // A string element reads as a byte and cannot be stored through: the byte is one
        // wide and `char` is two, so the store would either clobber the neighbouring
        // character or need a read-modify-write the type system cannot describe. Naming
        // the alternative is the difference between "you cannot do that" and "here is
        // what you wanted".
        if (target.kind == ExprKind::Index && target.kids[0] != nullptr &&
            target.kids[0]->type != nullptr &&
            target.kids[0]->type->kind == TypeKind::String) {
            m_diagnostics.error(target.range,
                                "'s[i]' reads a byte of a string and cannot be assigned to; "
                                "build the new text with str.replace, or fill a byte buffer "
                                "and makeString it");
        } else {
            m_diagnostics.error(target.range,
                                "the left side of an assignment must be a variable, a member or "
                                "an element");
        }
    } else if (Symbol* root = lvalueRoot(target); root != nullptr && !root->isMutable) {
        m_diagnostics.error(target.range,
                            "cannot assign to const " +
                                std::string(root->kind == Symbol::Kind::Param ? "parameter '"
                                                                              : "variable '") +
                                root->name + "'");
    }

    // `op = mul;` -- the same resolution as an initialiser, with the target type taken
    // from the left side.
    const bool assignedFunctionValue =
        expr.assignop == AssignOp::Assign && targetType != nullptr && targetType->isFunc() &&
        value.kind == ExprKind::Name && resolveFunctionValue(value, targetType, scope);
    const Type* valueType = assignedFunctionValue ? value.type : checkExpr(value, scope, flow);

    if (expr.assignop == AssignOp::Assign) {
        const unsigned errorsBefore = m_diagnostics.errorCount();
        adaptLiteral(value, targetType);
        const Type* effective = value.type != nullptr ? value.type : valueType;
        if (m_diagnostics.errorCount() == errorsBefore && !targetType->isError() &&
            !effective->isError() && !m_types.isAssignable(effective, targetType)) {
            m_diagnostics.error(value.range, "cannot assign a value of type '" +
                                                 effective->describe() + "' to '" +
                                                 targetType->describe() + "'" +
                                                 conversionHint(effective, targetType));
        }
    } else {
        // A compound assignment reads the target before writing it, so the target has to
        // hold a value already. This check was missing: `m_lvalueContext` above told
        // `checkExpr` that the target was a pure write, so `int x; x += 1;` compiled and
        // added to whatever the stack slot happened to contain -- measured, it printed 1
        // because the slot was zero, which is the worst way for this to be wrong.
        if (Symbol* root = lvalueRoot(target); root != nullptr) {
            checkValueReadable(*root, flow, target.range);
        }

        // `+=` on a string is concatenation, and it is the only compound operator that
        // means anything for text. Saying "requires a numeric target" for `s -= "x"`
        // would send the reader looking for a numeric conversion that is not there, so
        // the string case is separated from the arithmetic one.
        if (targetType->kind == TypeKind::String && expr.assignop == AssignOp::Add) {
            // Whatever `+` can render into a string, `+=` can append: another string, an
            // int, a char, a float, a bool, an enum, a pointer.
            if (!isScalarOrString(valueType) && !valueType->isError()) {
                m_diagnostics.error(value.range,
                                    "'+=' on a string can append a string or a scalar, found '" +
                                        valueType->describe() + "'");
            }
        } else if (targetType->kind == TypeKind::String) {
            m_diagnostics.error(expr.range,
                                "'" + std::string(assignOpName(expr.assignop)) +
                                    "' is not defined for a string; only '+=' concatenates");
        } else if (!targetType->isNumeric() && targetType->kind != TypeKind::Char) {
            m_diagnostics.error(expr.range,
                                "compound assignment requires a numeric target, found '" +
                                    targetType->describe() + "'");
        } else if (!valueType->isNumeric() && valueType->kind != TypeKind::Char &&
                   valueType->kind != TypeKind::Ptr) {
            m_diagnostics.error(value.range,
                                "compound assignment requires a numeric value, found '" +
                                    valueType->describe() + "'");
        }
    }

    if (Symbol* root = lvalueRoot(target)) {
        flow.assigned.insert(root);
    }

    expr.type = targetType;
    return expr.type;
}

const Type* Sema::checkTernary(Expr& expr, Scope& scope, Flow& flow) {
    const Type* condType = checkExpr(*expr.kids[0], scope, flow);
    const Type* thenType = checkExpr(*expr.kids[1], scope, flow);
    const Type* elseType = checkExpr(*expr.kids[2], scope, flow);

    if (condType->isAggregate() || condType->isVoid()) {
        m_diagnostics.error(expr.kids[0]->range,
                            "a conditional expression needs a scalar condition, found '" +
                                condType->describe() + "'");
    }

    if (thenType->isError() || elseType->isError()) {
        expr.type = m_types.errorType();
        return expr.type;
    }

    if (m_types.same(thenType, elseType)) {
        expr.type = thenType;
        return expr.type;
    }
    if (thenType->isPtr() && elseType->kind == TypeKind::Null) {
        expr.type = thenType;
        return expr.type;
    }
    if (elseType->isPtr() && thenType->kind == TypeKind::Null) {
        expr.type = elseType;
        return expr.type;
    }
    if (isLiteralExpr(*expr.kids[1]) && adaptLiteral(*expr.kids[1], elseType)) {
        expr.type = elseType;
        return expr.type;
    }
    if (isLiteralExpr(*expr.kids[2]) && adaptLiteral(*expr.kids[2], thenType)) {
        expr.type = thenType;
        return expr.type;
    }
    const Type* merged = m_types.mergeArithmetic(thenType, elseType);
    if (merged != nullptr) {
        expr.type = merged;
        return expr.type;
    }

    m_diagnostics.error(expr.range, "the branches of a conditional expression have different "
                                    "types ('" + thenType->describe() + "' and '" +
                                        elseType->describe() + "')");
    expr.type = m_types.errorType();
    return expr.type;
}

const Type* Sema::checkCast(Expr& expr, Scope& scope, Flow& flow) {
    const Type* source = checkExpr(*expr.kids[0], scope, flow);
    if (expr.typeNode == nullptr) {
        expr.type = m_types.errorType();
        return expr.type;
    }
    const Type* target = resolveTypeNode(*expr.typeNode, scope);

    if (source->isError() || target->isError()) {
        expr.type = target;
        return expr.type;
    }

    const auto numeric = [](const Type* type) {
        return type->isNumeric() || type->kind == TypeKind::Char ||
               type->kind == TypeKind::Bool;
    };

    bool allowed = false;
    if (numeric(source) && numeric(target)) {
        allowed = true;
    } else if (source->isPtr() && target->isPtr()) {
        allowed = true;
    } else if (source->isPtr() && target->isInt()) {
        allowed = true;
    } else if (source->isInt() && target->isPtr()) {
        allowed = true;
    } else if (source->isBit() && target->isInt()) {
        // Reading a bit string out as a number, and writing one back with the value
        // truncated to the declared width. This is the only way in and out: the type
        // refuses arithmetic, so `as int` is where a caller states what it means.
        // A width past the VM's 128 bit cell has no numeric representation at all,
        // so it cannot be read out -- it is storage, and storage is copied, not
        // converted.
        allowed = source->sizeBytes <= 16;
    } else if (source->isInt() && target->isBit()) {
        allowed = target->sizeBytes <= 16;
    } else if (m_types.same(source, target)) {
        allowed = true;
    }

    if (!allowed) {
        m_diagnostics.error(expr.range, "cannot cast '" + source->describe() + "' to '" +
                                            target->describe() + "'");
        expr.type = m_types.errorType();
        return expr.type;
    }

    // A cast of a constant that overflows is a compile time error (spec
    // §2.4.2.1).
    if (expr.kids[0]->isConstExpr && target->isInt()) {
        Int128 value = expr.kids[0]->constIsFloat
                           ? static_cast<Int128>(expr.kids[0]->constFloat)
                           : expr.kids[0]->constInt;
        if (value < literalMin(target) || value > literalMax(target)) {
            m_diagnostics.error(expr.range, "constant '" + formatInt128(value) +
                                                "' does not fit in '" + target->describe() + "'");
        } else {
            expr.isConstExpr = true;
            expr.constInt = value;
        }
    }

    expr.type = target;
    return expr.type;
}

//===----------------------------------------------------------------------===//
// Calls
//===----------------------------------------------------------------------===//

const Type* Sema::checkCall(Expr& expr, Scope& scope, Flow& flow) {
    Expr& callee = *expr.kids[0];
    std::vector<ExprPtr>& args = expr.kids;

    if (callee.kind == ExprKind::Name) {
        Symbol* symbol = scope.lookup(callee.text);
        if (symbol == nullptr) {
            m_diagnostics.error(callee.range, "undefined function '" + callee.text + "'");
            if (std::string suggestion = suggestName(callee.text, scope); !suggestion.empty()) {
                m_diagnostics.help(callee.range.begin, "did you mean '" + suggestion + "'?");
            }
            for (std::size_t i = 1; i < args.size(); ++i) {
                checkExpr(*args[i], scope, flow);
            }
            expr.type = m_types.errorType();
            return expr.type;
        }

        callee.sym = symbol;

        if (symbol->kind == Symbol::Kind::Builtin) {
            expr.builtin = static_cast<int>(symbol->builtin);
            return checkBuiltinCall(expr, *symbol, scope, flow);
        }

        if (symbol->kind == Symbol::Kind::Function) {
            const int index = resolveOverload(*symbol, args, scope, flow, expr.range);
            expr.funcIndex = index;
            if (index < 0) {
                expr.type = m_types.errorType();
                return expr.type;
            }
            expr.type = m_functions[static_cast<std::size_t>(index)].ret;
            return expr.type;
        }

        // A `Func<...>` variable: the callee is a run-time value, so the signature is
        // checked here and the call becomes CALL_INDIRECT.
        if (symbol->type != nullptr && symbol->type->isFunc()) {
            // IRGen reads the signature off the callee expression to convert the arguments,
            // and the callee is not visited as a value elsewhere.
            callee.type = symbol->type;
            return checkIndirectCallOf(expr, callee, symbol->type, args, scope, flow);
        }

        m_diagnostics.error(callee.range, "'" + callee.text + "' is not callable; it has type '" +
                                              symbol->type->describe() + "'");
        for (std::size_t i = 1; i < args.size(); ++i) {
            checkExpr(*args[i], scope, flow);
        }
        expr.type = m_types.errorType();
        return expr.type;
    }

    if (callee.kind == ExprKind::Member || callee.kind == ExprKind::Arrow) {
        const Expr& object = *callee.kids[0];

        // `Point.origin()`: the object is a type name, so this is a static method.
        // Checked before the value path because a type name is a symbol too, and
        // the module-namespace message below would be actively misleading.
        if (callee.kind == ExprKind::Member && object.kind == ExprKind::Name) {
            // `Pair<int>.zero()`: a generic type has no symbol of its own, so the
            // parser hung the type on the name node instead. The instance owns the
            // methods, so resolving it here is all the static path needs.
            if (object.typeNode != nullptr) {
                const Type* instanceType = resolveTypeNode(*object.typeNode, scope);
                if (instanceType != nullptr && !instanceType->isError()) {
                    const Type* resolved =
                        checkMethodCall(expr, callee, scope, flow, /*objectIsType=*/true,
                                        instanceType);
                    if (resolved != nullptr) {
                        return resolved;
                    }
                    m_diagnostics.error(callee.range,
                                        "'" + instanceType->describe() +
                                            "' has no static method '" + callee.text + "'");
                    expr.type = m_types.errorType();
                    return expr.type;
                }
                expr.type = m_types.errorType();
                return expr.type;
            }
            if (Symbol* objectSymbol = scope.lookup(object.text);
                objectSymbol != nullptr && objectSymbol->kind == Symbol::Kind::TypeName) {
                const Type* resolved =
                    checkMethodCall(expr, callee, scope, flow, /*objectIsType=*/true,
                                    objectSymbol->type);
                if (resolved != nullptr) {
                    return resolved;
                }
                m_diagnostics.error(callee.range,
                                    "'" + objectSymbol->type->describe() +
                                        "' has no static method '" + callee.text + "'");
                expr.type = m_types.errorType();
                return expr.type;
            }
        }

        // `quicksort.main()`: the object is a name that does not exist. The
        // useful thing to say is that modules are not loaded, not that a
        // variable is undefined.
        if (callee.kind == ExprKind::Member && object.kind == ExprKind::Name &&
            scope.lookup(object.text) == nullptr) {
            m_diagnostics.error(callee.range,
                                "undefined name '" + object.text +
                                    "'; a module cannot be used as a namespace here (this build "
                                    "compiles a single file, and 'import' is parsed but not "
                                    "loaded)");
            expr.type = m_types.errorType();
            return expr.type;
        }

        const Type* objectType = checkExpr(*callee.kids[0], scope, flow);

        // A `Func<...>` stored in a field, called as a method: `table.run(x)`. This has to
        // come before method resolution, because `run` is a field and not a method -- the
        // method path would report "has no method 'run'" for something that works.
        //
        // The callee stays the member expression, so IRGen's indirect path emits the field
        // load and then CALL_INDIRECT, exactly as it does for a local.
        {
            const Type* holder = objectType;
            if (callee.kind == ExprKind::Arrow && objectType != nullptr && objectType->isPtr()) {
                holder = objectType->elem;
            }
            if (holder != nullptr &&
                (holder->kind == TypeKind::Struct || holder->kind == TypeKind::Union)) {
                int fieldIndex = -1;
                for (std::size_t i = 0; i < holder->fields.size(); ++i) {
                    if (holder->fields[i].name == callee.text) {
                        fieldIndex = static_cast<int>(i);
                        break;
                    }
                }
                if (fieldIndex >= 0 && holder->fields[static_cast<std::size_t>(fieldIndex)]
                                           .type->isFunc()) {
                    // Check the member expression itself so it carries the field's type and
                    // the index IRGen needs.
                    callee.type = holder->fields[static_cast<std::size_t>(fieldIndex)].type;
                    callee.memberIndex = fieldIndex;
                    return checkIndirectCallOf(expr, callee, callee.type, args, scope, flow);
                }
            }
        }

        // `p->m()` takes the receiver from the pointee; `v.m()` from the value.
        const Type* receiverType = objectType;
        if (callee.kind == ExprKind::Arrow) {
            if (!objectType->isError() && !objectType->isPtr()) {
                m_diagnostics.error(callee.range, "'->' requires a pointer, found '" +
                                                      objectType->describe() + "'");
                for (std::size_t i = 1; i < args.size(); ++i) {
                    checkExpr(*args[i], scope, flow);
                }
                expr.type = m_types.errorType();
                return expr.type;
            }
            if (!objectType->isError()) {
                receiverType = objectType->elem;
            }
        }

        if (!receiverType->isError() &&
            (receiverType->kind == TypeKind::Struct || receiverType->kind == TypeKind::Union)) {
            const Type* resolved =
                checkMethodCall(expr, callee, scope, flow, /*objectIsType=*/false, receiverType);
            if (resolved != nullptr) {
                return resolved;
            }
        }

        for (std::size_t i = 1; i < args.size(); ++i) {
            checkExpr(*args[i], scope, flow);
        }
        // `.` on a pointer does not dereference -- Axiom spells that `->`
        // everywhere. When the method exists on the pointee, say which operator
        // to use instead of reporting that the pointer has no such method.
        const bool ptrMethodViaDot =
            callee.kind == ExprKind::Member && !objectType->isError() && objectType->isPtr() &&
            objectType->elem != nullptr &&
            (objectType->elem->kind == TypeKind::Struct ||
             objectType->elem->kind == TypeKind::Union) &&
            m_methods.count(objectType->elem) != 0 &&
            m_methods[objectType->elem].count(callee.text) != 0;

        if (objectType->kind == TypeKind::Vector) {
            m_diagnostics.error(callee.range,
                                "'vector' is not implemented in this build; the 'vec' module "
                                "provides an equivalent growable array");
        } else if (ptrMethodViaDot) {
            m_diagnostics.error(callee.range, "the receiver is a pointer; write '" + callee.text +
                                                  "' with '->' to call a method through it "
                                                  "('object->" + callee.text + "(...)')");
        } else if (!receiverType->isError()) {
            // A builtin in method position: `s.startsWith("x")`. The builtins are
            // functions -- `startsWith(s, "x")` -- so "has no method" sends the reader
            // looking for a typo in a method that was never going to exist. Fourteen of
            // them are string functions, which makes this the shape people actually write.
            std::size_t entryCount = 0;
            const BuiltinEntry* entries = builtinTable(entryCount);
            bool isBuiltinName = false;
            for (std::size_t i = 0; i < entryCount; ++i) {
                if (callee.text == entries[i].name) {
                    isBuiltinName = true;
                    break;
                }
            }
            if (isBuiltinName) {
                // Naming the receiver makes the rewrite complete for the common case
                // (`s.startsWith(x)` -> `startsWith(s, x)`); for anything longer the
                // placeholder still says what shape to write.
                const std::string receiver =
                    object.kind == ExprKind::Name ? object.text : std::string("<value>");
                m_diagnostics.error(callee.range,
                                    "'" + callee.text + "' is a builtin function, not a "
                                    "method of '" + receiverType->describe() + "'; write '" +
                                        callee.text + "(" + receiver + ", ...)'");
            } else {
                m_diagnostics.error(callee.range,
                                    "'" + receiverType->describe() + "' has no method '" +
                                        callee.text + "'");
            }
        }
        expr.type = m_types.errorType();
        return expr.type;
    }

    // A `Func<...>` reached any other way than a bare name, a field or an element:
    // nothing has typed the callee yet -- `table[0](4)` is an Index -- and the value's
    // type is where the signature lives. Checking it is what makes the signature available
    // at all; `checkIndirectCallOf` then reads the arguments against it.
    if (callee.type == nullptr) {
        checkExpr(callee, scope, flow);
    }
    if (callee.type != nullptr && callee.type->isFunc()) {
        return checkIndirectCallOf(expr, callee, callee.type, args, scope, flow);
    }

    m_diagnostics.error(callee.range, "this expression is not callable");
    expr.type = m_types.errorType();
    return expr.type;
}

const Type* Sema::checkBuiltinCall(Expr& expr, Symbol& symbol, Scope& scope, Flow& flow) {
    const Builtin builtin = symbol.builtin;
    const std::size_t argc = expr.kids.size() - 1;

    std::vector<const Type*> argumentTypes;
    argumentTypes.reserve(argc);
    bool sawError = false;
    for (std::size_t i = 1; i < expr.kids.size(); ++i) {
        const Type* argumentType = checkExpr(*expr.kids[i], scope, flow);
        sawError = sawError || argumentType->isError();
        argumentTypes.push_back(argumentType);
    }
    if (sawError) {
        // An argument already failed; its diagnostic is the useful one, and a
        // second complaint about "'print' cannot print '<error>'" only buries
        // it. Spec §7.2 asks for no cascading diagnostics.
        expr.type = m_types.errorType();
        return expr.type;
    }

    const auto arityError = [&](std::size_t expected) {
        m_diagnostics.error(expr.range, "'" + std::string(builtinName(builtin)) + "' expects " +
                                            std::to_string(expected) + " argument(s) but " +
                                            std::to_string(argc) + " were provided");
    };
    const auto requireArity = [&](std::size_t expected) {
        if (argc != expected) {
            arityError(expected);
            return false;
        }
        return true;
    };
    const auto arg = [&](std::size_t index) { return argumentTypes[index]; };

    switch (builtin) {
    case Builtin::Print:
    case Builtin::EPrint:
    case Builtin::Log: {
        if (!requireArity(1)) {
            break;
        }
        if (!isScalarOrString(arg(0))) {
            m_diagnostics.error(expr.kids[1]->range,
                                "'" + std::string(builtinName(builtin)) +
                                    "' cannot print a value of type '" + arg(0)->describe() + "'");
        }
        expr.type = m_types.voidType();
        return expr.type;
    }

    case Builtin::Alloc:
    case Builtin::Alloc0:
        if (!requireArity(1)) {
            break;
        }
        if (!arg(0)->isIntegerLike()) {
            m_diagnostics.error(expr.kids[1]->range, "the size must be an integer");
        }
        expr.type = m_types.getPtr(m_types.voidType());
        return expr.type;

    case Builtin::SortInts:
    case Builtin::SortInt64s:
    case Builtin::SortFloats:
    case Builtin::ReverseInts: {
        // The native helpers take (pointer, element count) and write through
        // the pointer. Checking the element type here is what keeps the C side
        // honest: it reinterprets the buffer as int32_t* or double*, so a
        // mismatch would silently corrupt data rather than fail.
        if (!requireArity(2)) {
            break;
        }
        const bool floats = builtin == Builtin::SortFloats;
        const bool wide = builtin == Builtin::SortInt64s;
        const Type* wanted = floats ? m_types.doubleType()
                                    : (wide ? m_types.getInt(64, false) : m_types.intType());
        if (!arg(0)->isPtr()) {
            m_diagnostics.error(expr.kids[1]->range,
                                "'" + std::string(builtinName(builtin)) + "' expects 'ptr " +
                                    wanted->describe() + "', found '" + arg(0)->describe() + "'");
        } else if (!m_types.same(arg(0)->elem, wanted)) {
            m_diagnostics.error(expr.kids[1]->range,
                                "'" + std::string(builtinName(builtin)) + "' expects 'ptr " +
                                    wanted->describe() + "', found '" + arg(0)->describe() + "'");
        }
        if (!arg(1)->isIntegerLike()) {
            m_diagnostics.error(expr.kids[2]->range,
                                "the element count must be an integer, found '" +
                                    arg(1)->describe() + "'");
        }
        expr.type = m_types.voidType();
        return expr.type;
    }

    case Builtin::Free:
        if (!requireArity(1)) {
            break;
        }
        if (!arg(0)->isPtr() && arg(0)->kind != TypeKind::Null) {
            m_diagnostics.error(expr.kids[1]->range, "'free' expects a pointer");
        }
        expr.type = m_types.voidType();
        return expr.type;

    case Builtin::Memcpy:
    case Builtin::Memmove: {
        if (!requireArity(3)) {
            break;
        }
        if (arg(0)->kind == TypeKind::Null) {
            m_diagnostics.error(expr.kids[1]->range, "the destination pointer is null");
        }
        if (!arg(2)->isIntegerLike()) {
            m_diagnostics.error(expr.kids[3]->range, "the byte count must be an integer");
        }
        // `memcpy(#constVar, ...)` is rejected (spec §2.4.2, Q98).
        const Expr& destination = *expr.kids[1];
        if (destination.kind == ExprKind::Unary && destination.unop == UnOp::AddrOf) {
            if (Symbol* root = lvalueRoot(*destination.kids[0]); root != nullptr && !root->isMutable) {
                m_diagnostics.error(destination.range,
                                    "the destination of a memcpy cannot come from a const variable");
            }
        }
        expr.type = m_types.getPtr(m_types.voidType());
        return expr.type;
    }

    case Builtin::Dup:
        if (!requireArity(1)) {
            break;
        }
        if (arg(0)->kind != TypeKind::String) {
            m_diagnostics.error(expr.kids[1]->range, "'dup' expects a 'string'");
        }
        expr.type = m_types.stringType();
        return expr.type;

    case Builtin::MakeString:
        if (!requireArity(2)) {
            break;
        }
        if (arg(0)->isPtr() && !m_types.same(arg(0)->elem, m_types.charType())) {
            m_diagnostics.error(expr.kids[1]->range, "'makeString' expects a 'ptr char' buffer");
        }
        if (!arg(1)->isIntegerLike()) {
            m_diagnostics.error(expr.kids[2]->range, "the length must be an integer");
        }
        expr.type = m_types.stringType();
        return expr.type;

    case Builtin::Yield:
    case Builtin::Trap:
        if (!requireArity(0)) {
            break;
        }
        expr.type = m_types.voidType();
        return expr.type;

    case Builtin::ReadLine:
    case Builtin::ReadToken:
        if (!requireArity(0)) {
            break;
        }
        expr.type = m_types.stringType();
        return expr.type;

    case Builtin::ArgCount:
        if (!requireArity(0)) {
            break;
        }
        expr.type = m_types.intType();
        return expr.type;

    case Builtin::ArgAt:
        if (!requireArity(1)) {
            break;
        }
        // The index must be an integer, but not necessarily `int`: `int64` counts
        // too, and the conversion happens in IRGen.
        if (!arg(0)->isIntegerLike()) {
            m_diagnostics.error(expr.kids[1]->range, "the argument index must be an integer");
        }
        expr.type = m_types.stringType();
        return expr.type;

    case Builtin::ReadInt:
        if (!requireArity(0)) {
            break;
        }
        expr.type = m_types.intType();
        return expr.type;

    case Builtin::ReadFloat:
        if (!requireArity(0)) {
            break;
        }
        expr.type = m_types.doubleType();
        return expr.type;

    case Builtin::Itoa:
        if (!requireArity(2)) {
            break;
        }
        if (!arg(0)->isIntegerLike()) {
            m_diagnostics.error(expr.kids[1]->range, "'itoa' expects an integer value");
        }
        if (!arg(1)->isIntegerLike()) {
            m_diagnostics.error(expr.kids[2]->range, "'itoa' expects an integer base");
        }
        expr.type = m_types.stringType();
        return expr.type;

    case Builtin::Ftoa:
        if (!requireArity(1)) {
            break;
        }
        expr.type = m_types.stringType();
        return expr.type;

    case Builtin::Atoi:
        if (!requireArity(1)) {
            break;
        }
        if (arg(0)->kind != TypeKind::String) {
            m_diagnostics.error(expr.kids[1]->range, "'atoi' expects a 'string'");
        }
        expr.type = m_types.intType();
        return expr.type;

    case Builtin::Atof:
        if (!requireArity(1)) {
            break;
        }
        if (arg(0)->kind != TypeKind::String) {
            m_diagnostics.error(expr.kids[1]->range, "'atof' expects a 'string'");
        }
        expr.type = m_types.doubleType();
        return expr.type;

    case Builtin::IndexOf:
    case Builtin::LastIndexOf:
        if (!requireArity(2)) {
            break;
        }
        if (arg(0)->kind != TypeKind::String || arg(1)->kind != TypeKind::String) {
            m_diagnostics.error(expr.range, "this function expects two 'string' arguments");
        }
        expr.type = m_types.intType();
        return expr.type;

    case Builtin::Substring:
        if (!requireArity(3)) {
            break;
        }
        if (arg(0)->kind != TypeKind::String) {
            m_diagnostics.error(expr.kids[1]->range, "the first argument must be a 'string'");
        }
        expr.type = m_types.stringType();
        return expr.type;

    case Builtin::StartsWith:
    case Builtin::EndsWith:
        if (!requireArity(2)) {
            break;
        }
        if (arg(0)->kind != TypeKind::String || arg(1)->kind != TypeKind::String) {
            m_diagnostics.error(expr.range, "this function expects two 'string' arguments");
        }
        expr.type = m_types.boolType();
        return expr.type;

    case Builtin::Trim:
    case Builtin::ToUpper:
    case Builtin::ToLower:
        if (!requireArity(1)) {
            break;
        }
        if (arg(0)->kind != TypeKind::String) {
            m_diagnostics.error(expr.kids[1]->range, "this function expects a 'string'");
        }
        expr.type = m_types.stringType();
        return expr.type;

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
        if (!requireArity(1)) {
            break;
        }
        if (!arg(0)->isFloat()) {
            m_diagnostics.error(expr.kids[1]->range,
                                "'" + std::string(builtinName(builtin)) +
                                    "' expects a floating point argument");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.type = arg(0);
        return expr.type;
    }

    case Builtin::Atan2:
    case Builtin::Pow:
    case Builtin::Fmod: {
        if (!requireArity(2)) {
            break;
        }
        if (!arg(0)->isFloat() || !arg(1)->isFloat()) {
            m_diagnostics.error(expr.range,
                                "'" + std::string(builtinName(builtin)) +
                                    "' expects two floating point arguments");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.type = arg(0)->bits >= arg(1)->bits ? arg(0) : arg(1);
        return expr.type;
    }

    case Builtin::Min:
    case Builtin::Max: {
        if (!requireArity(2)) {
            break;
        }
        const Type* merged = m_types.mergeArithmetic(arg(0), arg(1));
        if (merged == nullptr) {
            m_diagnostics.error(expr.range,
                                "'" + std::string(builtinName(builtin)) +
                                    "' requires two operands of the same numeric type");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.type = merged;
        return expr.type;
    }

    case Builtin::Abs:
        if (!requireArity(1)) {
            break;
        }
        if (!arg(0)->isIntegerLike()) {
            m_diagnostics.error(expr.kids[1]->range,
                                "'abs' expects an integer; use 'fabs' for floating point values");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.type = arg(0)->kind == TypeKind::Char ? m_types.getInt(32, false) : arg(0);
        return expr.type;

    case Builtin::Assert:
        if (!requireArity(1)) {
            break;
        }
        if (arg(0)->isAggregate() || arg(0)->isVoid()) {
            m_diagnostics.error(expr.kids[1]->range, "'assert' expects a scalar condition");
        }
        expr.type = m_types.voidType();
        return expr.type;

    case Builtin::None:
        break;
    }

    expr.type = m_types.voidType();
    return expr.type;
}

} // namespace acvm
