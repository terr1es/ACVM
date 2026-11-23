//===--- ConstEval.cpp - Constant folding for comptime / case / lengths --===//
#include "acvm/Sema/Sema.h"

#include "acvm/Common/Diagnostic.h"

#include <cmath>

namespace acvm {
namespace {

bool binaryOp(BinOp op, const ConstValue& lhs, const ConstValue& rhs, ConstValue& out,
              bool& divisionByZero) {
    divisionByZero = false;
    const bool isFloat = lhs.isFloat || rhs.isFloat;

    if (isFloat) {
        const double a = lhs.isFloat ? lhs.f : static_cast<double>(lhs.i);
        const double b = rhs.isFloat ? rhs.f : static_cast<double>(rhs.i);
        out.isFloat = true;
        out.f = 0;
        switch (op) {
        case BinOp::Add: out.f = a + b; return true;
        case BinOp::Sub: out.f = a - b; return true;
        case BinOp::Mul: out.f = a * b; return true;
        case BinOp::Div:
            if (b == 0.0) {
                divisionByZero = true;
                return false;
            }
            out.f = a / b;
            return true;
        case BinOp::Mod:
            if (b == 0.0) {
                divisionByZero = true;
                return false;
            }
            out.f = std::fmod(a, b);
            return true;
        case BinOp::Lt: out.isFloat = false; out.i = a < b ? 1 : 0; return true;
        case BinOp::Le: out.isFloat = false; out.i = a <= b ? 1 : 0; return true;
        case BinOp::Gt: out.isFloat = false; out.i = a > b ? 1 : 0; return true;
        case BinOp::Ge: out.isFloat = false; out.i = a >= b ? 1 : 0; return true;
        case BinOp::Eq: out.isFloat = false; out.i = a == b ? 1 : 0; return true;
        case BinOp::Ne: out.isFloat = false; out.i = a != b ? 1 : 0; return true;
        default: return false;
        }
    }

    const Int128 a = lhs.i;
    const Int128 b = rhs.i;
    out.isFloat = false;
    switch (op) {
    case BinOp::Add: out.i = a + b; return true;
    case BinOp::Sub: out.i = a - b; return true;
    case BinOp::Mul: out.i = a * b; return true;
    case BinOp::Div:
        if (b == 0) {
            divisionByZero = true;
            return false;
        }
        out.i = a / b;
        return true;
    case BinOp::Mod:
        if (b == 0) {
            divisionByZero = true;
            return false;
        }
        out.i = a % b;
        return true;
    case BinOp::Shl:
        out.i = a << static_cast<int>(b & 127);
        return true;
    case BinOp::Shr:
        out.i = a >> static_cast<int>(b & 127);
        return true;
    case BinOp::BitAnd: out.i = a & b; return true;
    case BinOp::BitOr: out.i = a | b; return true;
    case BinOp::BitXor: out.i = a ^ b; return true;
    case BinOp::Lt: out.i = a < b ? 1 : 0; return true;
    case BinOp::Le: out.i = a <= b ? 1 : 0; return true;
    case BinOp::Gt: out.i = a > b ? 1 : 0; return true;
    case BinOp::Ge: out.i = a >= b ? 1 : 0; return true;
    case BinOp::Eq: out.i = a == b ? 1 : 0; return true;
    case BinOp::Ne: out.i = a != b ? 1 : 0; return true;
    case BinOp::LogAnd: out.i = (a != 0 && b != 0) ? 1 : 0; return true;
    case BinOp::LogOr: out.i = (a != 0 || b != 0) ? 1 : 0; return true;
    }
    return false;
}

} // namespace

bool Sema::evalConst(Expr& expr, Scope& scope, ConstValue& out) {
    switch (expr.kind) {
    case ExprKind::IntLit:
        out.isFloat = false;
        out.i = expr.lit.intValue;
        return expr.lit.fits128;
    case ExprKind::FloatLit:
        out.isFloat = true;
        out.f = expr.lit.floatValue;
        return true;
    case ExprKind::CharLit:
        out.isFloat = false;
        out.i = static_cast<Int128>(expr.lit.charValue);
        return true;
    case ExprKind::BoolLit:
        out.isFloat = false;
        out.i = expr.lit.intValue;
        return true;
    case ExprKind::Name: {
        Symbol* symbol = scope.lookup(expr.text);
        if (symbol == nullptr) {
            return false;
        }
        if (symbol->kind == Symbol::Kind::Comptime) {
            expr.sym = symbol;
            out.isFloat = symbol->constIsFloat;
            out.i = symbol->constInt;
            out.f = symbol->constFloat;
            return true;
        }
        if (symbol->kind == Symbol::Kind::EnumConstant) {
            expr.sym = symbol;
            out.isFloat = false;
            out.i = static_cast<Int128>(symbol->enumValue);
            return true;
        }
        return false;
    }
    case ExprKind::Unary: {
        ConstValue operand;
        if (!evalConst(*expr.kids[0], scope, operand)) {
            return false;
        }
        switch (expr.unop) {
        case UnOp::Plus:
            out = operand;
            return true;
        case UnOp::Neg:
            out = operand;
            if (operand.isFloat) {
                out.f = -operand.f;
            } else {
                out.i = -operand.i;
            }
            return true;
        case UnOp::Not:
            out.isFloat = false;
            out.i = operand.isFloat ? (operand.f == 0.0 ? 1 : 0) : (operand.i == 0 ? 1 : 0);
            return true;
        case UnOp::BitNot:
            if (operand.isFloat) {
                return false;
            }
            out.isFloat = false;
            out.i = ~operand.i;
            return true;
        default:
            return false;
        }
    }
    case ExprKind::Binary: {
        ConstValue lhs;
        ConstValue rhs;
        if (!evalConst(*expr.kids[0], scope, lhs) || !evalConst(*expr.kids[1], scope, rhs)) {
            return false;
        }
        bool divisionByZero = false;
        const bool ok = binaryOp(expr.binop, lhs, rhs, out, divisionByZero);
        if (divisionByZero) {
            m_diagnostics.error(expr.range, "division by zero in a constant expression");
        }
        return ok;
    }
    case ExprKind::Ternary: {
        ConstValue condition;
        if (!evalConst(*expr.kids[0], scope, condition)) {
            return false;
        }
        const bool truthy = condition.isFloat ? condition.f != 0.0 : condition.i != 0;
        return evalConst(*expr.kids[truthy ? 1 : 2], scope, out);
    }
    case ExprKind::Cast: {
        ConstValue operand;
        if (!evalConst(*expr.kids[0], scope, operand)) {
            return false;
        }
        if (expr.typeNode == nullptr) {
            return false;
        }
        const Type* target = resolveTypeNodeQuiet(*expr.typeNode);
        if (target == nullptr || target->isError()) {
            return false;
        }
        if (target->isFloat()) {
            out.isFloat = true;
            out.f = operand.isFloat ? operand.f : static_cast<double>(operand.i);
            return true;
        }
        if (target->isInt() || target->isEnum()) {
            out.isFloat = false;
            if (operand.isFloat) {
                out.i = static_cast<Int128>(operand.f);
            } else {
                out.i = operand.i;
            }
            return true;
        }
        return false;
    }
    case ExprKind::SizeOf:
    case ExprKind::AlignOf: {
        const Type* type = nullptr;
        if (expr.typeNode != nullptr) {
            type = resolveTypeNodeQuiet(*expr.typeNode);
        }
        if (type == nullptr) {
            return false;
        }
        out.isFloat = false;
        out.i = expr.kind == ExprKind::SizeOf ? static_cast<Int128>(type->sizeBytes)
                                              : static_cast<Int128>(type->alignBytes);
        return true;
    }
    default:
        return false;
    }
}

} // namespace acvm
