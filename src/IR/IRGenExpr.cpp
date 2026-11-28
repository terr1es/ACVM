//===--- IRGenExpr.cpp - Expression lowering ----------------------------===//
#include "acvm/IR/IRGen.h"

#include "acvm/Common/Diagnostic.h"
#include "acvm/Sema/Builtin.h"

#include <algorithm>
#include <string>

namespace acvm {
namespace {

/// The bit-field a member access names, or null when it names an ordinary field.
///
/// Bit fields are the one place where "the address of this value" is not enough to
/// read or write it: the value starts at a bit inside the byte. Everything that
/// touches a member has to ask this first.
const FieldInfo* bitFieldOf(const Expr& expr) {
    if (expr.kind != ExprKind::Member && expr.kind != ExprKind::Arrow) {
        return nullptr;
    }
    if (expr.kids.empty() || expr.kids[0] == nullptr) {
        return nullptr;
    }
    const Type* base = expr.kids[0]->type;
    if (expr.kind == ExprKind::Arrow) {
        if (base == nullptr || !base->isPtr()) {
            return nullptr;
        }
        base = base->elem;
    }
    if (base == nullptr ||
        (base->kind != TypeKind::Struct && base->kind != TypeKind::Union)) {
        return nullptr;
    }
    if (expr.memberIndex < 0 ||
        static_cast<std::size_t>(expr.memberIndex) >= base->fields.size()) {
        return nullptr;
    }
    const FieldInfo& field = base->fields[static_cast<std::size_t>(expr.memberIndex)];
    return field.bitWidth != 0 ? &field : nullptr;
}

bool isComparisonBinOp(BinOp op) {
    switch (op) {
    case BinOp::Lt:
    case BinOp::Le:
    case BinOp::Gt:
    case BinOp::Ge:
    case BinOp::Eq:
    case BinOp::Ne:
        return true;
    default:
        return false;
    }
}

OpCode arithmeticOpCode(BinOp op) {
    switch (op) {
    case BinOp::Add: return OpCode::Add;
    case BinOp::Sub: return OpCode::Sub;
    case BinOp::Mul: return OpCode::Mul;
    case BinOp::Div: return OpCode::Div;
    case BinOp::Mod: return OpCode::Mod;
    case BinOp::Pow: return OpCode::Pow;
    case BinOp::BitAnd: return OpCode::And;
    case BinOp::BitOr: return OpCode::Or;
    case BinOp::BitXor: return OpCode::Xor;
    case BinOp::Shl: return OpCode::Shl;
    case BinOp::Shr: return OpCode::Shr;
    case BinOp::Lt: return OpCode::Lt;
    case BinOp::Le: return OpCode::Le;
    case BinOp::Gt: return OpCode::Gt;
    case BinOp::Ge: return OpCode::Ge;
    case BinOp::Eq: return OpCode::Eq;
    case BinOp::Ne: return OpCode::Ne;
    default: return OpCode::Add;
    }
}


BinOp binOpOfAssign(AssignOp op) {
    switch (op) {
    case AssignOp::Add: return BinOp::Add;
    case AssignOp::Sub: return BinOp::Sub;
    case AssignOp::Mul: return BinOp::Mul;
    case AssignOp::Div: return BinOp::Div;
    case AssignOp::Mod: return BinOp::Mod;
    case AssignOp::BitAnd: return BinOp::BitAnd;
    case AssignOp::BitOr: return BinOp::BitOr;
    case AssignOp::BitXor: return BinOp::BitXor;
    case AssignOp::Shl: return BinOp::Shl;
    case AssignOp::Shr: return BinOp::Shr;
    default: return BinOp::Add;
    }
}

} // namespace

//===----------------------------------------------------------------------===//
// Addresses
//===----------------------------------------------------------------------===//

void IRGen::emitScaledAdd(Expr& index, std::uint32_t stride) {
    emitExpr(index);
    if (stride != 1) {
        emitConstInt(static_cast<Int128>(stride));
        m_fn->emit(OpCode::Mul, int64MemType());
    }
    m_fn->emit(OpCode::Add, ptrMemType());
}

std::uint32_t IRGen::stageIndex(Expr& index) {
    const std::uint32_t slot = allocRawTemp(8, 8);
    emitExpr(index);
    m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(slot));
    m_fn->emit(OpCode::Swap);
    m_fn->emit(OpCode::Store, int64MemType());
    return slot;
}

void IRGen::emitBoundsCheck(std::uint32_t indexSlot, bool dynamicLength,
                            std::uint32_t lengthSlotOrValue) {
    const int int64Type = int64MemType();

    // index < 0 -> trap
    m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
    m_fn->emit(OpCode::Load, int64Type);
    emitConstInt(0);
    m_fn->emit(OpCode::Lt, int64Type);
    std::size_t skip = m_fn->emitJump(OpCode::JumpIfFalse);
    m_fn->emit(OpCode::Trap, static_cast<std::int32_t>(TrapKind::IndexOutOfBounds), 0);
    m_fn->patchJump(skip, here());

    // index >= length -> trap
    m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
    m_fn->emit(OpCode::Load, int64Type);
    if (dynamicLength) {
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(lengthSlotOrValue));
        m_fn->emit(OpCode::Load, int64Type);
    } else {
        emitConstInt(static_cast<Int128>(lengthSlotOrValue));
    }
    m_fn->emit(OpCode::Ge, int64Type);
    skip = m_fn->emitJump(OpCode::JumpIfFalse);
    m_fn->emit(OpCode::Trap, static_cast<std::int32_t>(TrapKind::IndexOutOfBounds), 0);
    m_fn->patchJump(skip, here());
}

void IRGen::emitAddress(Expr& expr) {
    switch (expr.kind) {
    case ExprKind::Name: {
        Symbol* symbol = expr.sym;
        if (symbol == nullptr) {
            emitConstInt(0);
            return;
        }
        if (symbol->kind == Symbol::Kind::Global) {
            m_fn->emit(OpCode::GlobalAddr, symbol->globalIndex);
        } else {
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(slotFor(symbol)));
        }
        return;
    }

    case ExprKind::Unary:
        if (expr.unop == UnOp::Deref) {
            emitExpr(*expr.kids[0]);
            return;
        }
        break;

    case ExprKind::Member: {
        const Type* baseType = expr.kids[0]->type;
        emitBaseAddress(*expr.kids[0]);
        if (baseType != nullptr && baseType->kind == TypeKind::String) {
            if (expr.memberIndex == kMemberLength) {
                emitConstInt(8);
                m_fn->emit(OpCode::Add, ptrMemType());
            }
            return;
        }
        // Union members are listed here rather than left to fall through. Their
        // offset is always 0, so the code below would work by accident -- but
        // "works because the layout happens to be zero" is the kind of accident
        // that breaks silently the day the layout changes, with no warning at the
        // place that broke.
        if (baseType != nullptr &&
            (baseType->kind == TypeKind::Struct || baseType->kind == TypeKind::Union) &&
            expr.memberIndex >= 0 &&
            static_cast<std::size_t>(expr.memberIndex) < baseType->fields.size()) {
            const std::uint32_t offset = baseType->fields[static_cast<std::size_t>(expr.memberIndex)].offset;
            if (offset != 0) {
                emitConstInt(static_cast<Int128>(offset));
                m_fn->emit(OpCode::Add, ptrMemType());
            }
        }
        return;
    }

    case ExprKind::Arrow: {
        const Type* pointerType = expr.kids[0]->type;
        emitExpr(*expr.kids[0]);
        if (pointerType != nullptr && pointerType->isPtr() &&
            (pointerType->elem->kind == TypeKind::Struct ||
             pointerType->elem->kind == TypeKind::Union) &&
            expr.memberIndex >= 0 &&
            static_cast<std::size_t>(expr.memberIndex) < pointerType->elem->fields.size()) {
            const std::uint32_t offset =
                pointerType->elem->fields[static_cast<std::size_t>(expr.memberIndex)].offset;
            if (offset != 0) {
                emitConstInt(static_cast<Int128>(offset));
                m_fn->emit(OpCode::Add, ptrMemType());
            }
        }
        return;
    }

    case ExprKind::Index: {
        const Type* baseType = expr.kids[0]->type;
        if (baseType == nullptr) {
            emitConstInt(0);
            return;
        }
        if (baseType->kind == TypeKind::Array) {
            const std::uint32_t indexSlot = stageIndex(*expr.kids[1]);
            emitBoundsCheck(indexSlot, false, static_cast<std::uint32_t>(baseType->length));
            emitBaseAddress(*expr.kids[0]);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
            m_fn->emit(OpCode::Load, int64MemType());
            if (baseType->elem->sizeBytes != 1) {
                emitConstInt(static_cast<Int128>(baseType->elem->sizeBytes));
                m_fn->emit(OpCode::Mul, int64MemType());
            }
            m_fn->emit(OpCode::Add, ptrMemType());
            return;
        }
        if (baseType->kind == TypeKind::String) {
            const std::uint32_t indexSlot = stageIndex(*expr.kids[1]);
            const std::uint32_t baseSlot = allocRawTemp(8, 8);
            const std::uint32_t lengthSlot = allocRawTemp(8, 8);

            emitBaseAddress(*expr.kids[0]);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(baseSlot));
            m_fn->emit(OpCode::Swap);
            m_fn->emit(OpCode::Store, ptrMemType());

            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(baseSlot));
            m_fn->emit(OpCode::Load, ptrMemType());
            emitConstInt(8);
            m_fn->emit(OpCode::Add, ptrMemType());
            m_fn->emit(OpCode::Load, int64MemType());
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(lengthSlot));
            m_fn->emit(OpCode::Swap);
            m_fn->emit(OpCode::Store, int64MemType());

            emitBoundsCheck(indexSlot, true, lengthSlot);

            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(baseSlot));
            m_fn->emit(OpCode::Load, ptrMemType());
            m_fn->emit(OpCode::Load, ptrMemType());
            // Stride 1, because a string is UTF-8 and `length` counts bytes. The stride
            // used to be 2 -- UTF-16 code units -- while the bounds check above and the
            // stored length were both in bytes, so index 0 read the first two bytes, the
            // check let `s[4]` through on a five byte string, and the whole thing drifted
            // one byte per index until it ran off the allocation.
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
            m_fn->emit(OpCode::Load, int64MemType());
            m_fn->emit(OpCode::Add, ptrMemType());
            return;
        }
        if (baseType->kind == TypeKind::Ptr) {
            emitExpr(*expr.kids[0]);
            emitScaledAdd(*expr.kids[1], baseType->elem->sizeBytes);
            return;
        }
        if (baseType->kind == TypeKind::Tuple) {
            emitBaseAddress(*expr.kids[0]);
            if (expr.memberIndex >= 0 &&
                static_cast<std::size_t>(expr.memberIndex) < baseType->fields.size()) {
                const std::uint32_t offset =
                    baseType->fields[static_cast<std::size_t>(expr.memberIndex)].offset;
                if (offset != 0) {
                    emitConstInt(static_cast<Int128>(offset));
                    m_fn->emit(OpCode::Add, ptrMemType());
                }
            }
            return;
        }
        break;
    }

    default:
        break;
    }

    m_diagnostics.error(expr.range, "internal error: expression is not addressable");
    emitConstInt(0);
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

std::uint32_t IRGen::emitPackedElementAddress(Expr& expr) {
    const Type* baseType = expr.kids[0]->type;
    const Type* elem = baseType->elem;
    const unsigned perByte = elem->bitElementsPerByte();

    unsigned shift = 0;
    while ((1u << shift) < perByte) {
        ++shift;
    }
    const std::uint32_t perByteMask = perByte - 1;

    const std::uint32_t indexSlot = stageIndex(*expr.kids[1]);
    emitBoundsCheck(indexSlot, false, static_cast<std::uint32_t>(baseType->length));

    const std::uint32_t bitSlot = allocRawTemp(8, 8);

    // bitOffset = (i & (perByte-1)) * bits -- the position inside the byte.
    m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
    m_fn->emit(OpCode::Load, int64MemType());
    emitConstInt(perByteMask);
    m_fn->emit(OpCode::And, int64MemType());
    if (elem->bits != 1) {
        emitConstInt(elem->bits);
        m_fn->emit(OpCode::Mul, int64MemType());
    }
    m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(bitSlot));
    m_fn->emit(OpCode::Swap);
    m_fn->emit(OpCode::Store, int64MemType());

    // Address = base + (i >> shift), the byte that holds the element.
    emitBaseAddress(*expr.kids[0]);
    m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
    m_fn->emit(OpCode::Load, int64MemType());
    if (shift != 0) {
        emitConstInt(shift);
        m_fn->emit(OpCode::Shr, int64MemType());
    }
    m_fn->emit(OpCode::Add, ptrMemType());
    return bitSlot;
}

void IRGen::emitExpr(Expr& expr) {
    switch (expr.kind) {
    case ExprKind::IntLit:
        if (expr.literalTyped) {
            emitConstInt(expr.constInt);
        } else {
            emitConstInt(expr.lit.intValue);
        }
        return;

    case ExprKind::FloatLit:
        emitConstFloat(expr.lit.floatValue);
        return;

    case ExprKind::CharLit:
        emitConstInt(static_cast<Int128>(expr.lit.charValue));
        return;

    case ExprKind::BoolLit:
        emitConstInt(expr.lit.intValue);
        return;

    case ExprKind::NullLit:
        emitConstInt(0);
        return;

    case ExprKind::StringLit: {
        const int index = m_module->addString(expr.lit.stringValue);
        m_fn->emit(OpCode::ConstStr, index);
        return;
    }

    case ExprKind::Name: {
        // Before the symbol test: a function value resolves to a handle and has no slot
        // of its own, so the ordinary paths below would emit a load through a null symbol.
        if (expr.funcValue >= 0) {
            // The handle is the index plus one, so a zeroed slot means "no function" and
            // CALL_INDIRECT can trap on it instead of calling whatever happens to be first
            // in the module.
            emitConstInt(static_cast<Int128>(expr.funcValue) + 1);
            return;
        }
        Symbol* symbol = expr.sym;
        if (symbol == nullptr) {
            emitConstInt(0);
            return;
        }
        if (symbol->kind == Symbol::Kind::Comptime) {
            if (symbol->constIsFloat) {
                emitConstFloat(symbol->constFloat);
            } else {
                emitConstInt(symbol->constInt);
            }
            return;
        }
        if (symbol->kind == Symbol::Kind::EnumConstant) {
            emitConstInt(static_cast<Int128>(symbol->enumValue));
            return;
        }
        OpCode loadCode = OpCode::LoadLocal;
        std::int32_t slot = 0;
        int memType = 0;
        if (fusedVariable(expr, loadCode, slot, memType)) {
            m_fn->emit(loadCode, slot, memType);
            return;
        }
        emitAddress(expr);
        emitLoadValue(expr.type);
        return;
    }

    case ExprKind::Unary: {
        switch (expr.unop) {
        case UnOp::AddrOf:
            emitAddress(*expr.kids[0]);
            return;
        case UnOp::Deref: {
            const Expr& pointer = *expr.kids[0];
            if (pointer.kind == ExprKind::Name && pointer.sym != nullptr &&
                (pointer.sym->kind == Symbol::Kind::Local ||
                 pointer.sym->kind == Symbol::Kind::Param) &&
                expr.type != nullptr && !expr.type->isAggregate() && !expr.type->isVoid()) {
                const auto it = m_slots.find(pointer.sym);
                if (it != m_slots.end()) {
                    m_fn->emit(OpCode::DerefLocal, static_cast<std::int32_t>(it->second),
                               memTypeOf(expr.type));
                    return;
                }
            }
            emitExpr(*expr.kids[0]);
            emitLoadValue(expr.type);
            return;
        }
        case UnOp::Plus:
            emitExpr(*expr.kids[0]);
            return;
        case UnOp::Neg:
            emitExpr(*expr.kids[0]);
            m_fn->emit(OpCode::Neg, memTypeOf(expr.type));
            return;
        case UnOp::Not:
            emitExpr(*expr.kids[0]);
            m_fn->emit(OpCode::Not);
            return;
        case UnOp::BitNot:
            emitExpr(*expr.kids[0]);
            m_fn->emit(OpCode::BitNot, memTypeOf(expr.type));
            return;
        }
        return;
    }

    case ExprKind::Binary: {
        const Type* lhsType = expr.kids[0]->type;
        const Type* rhsType = expr.kids[1]->type;

        if (expr.binop == BinOp::LogAnd) {
            emitExpr(*expr.kids[0]);
            m_fn->emit(OpCode::Dup);
            const std::size_t skip = m_fn->emitJump(OpCode::JumpIfFalse);
            m_fn->emit(OpCode::Pop);
            emitExpr(*expr.kids[1]);
            m_fn->patchJump(skip, here());
            return;
        }
        if (expr.binop == BinOp::LogOr) {
            emitExpr(*expr.kids[0]);
            m_fn->emit(OpCode::Dup);
            const std::size_t skip = m_fn->emitJump(OpCode::JumpIfTrue);
            m_fn->emit(OpCode::Pop);
            emitExpr(*expr.kids[1]);
            m_fn->patchJump(skip, here());
            return;
        }

        // `string + x` concatenation, where either side may be the string
        // (spec: `isScalarOrString` on both). CONCAT is told how to render each
        // operand, because recovering that at run time would mean inspecting the
        // Value tag, and a Ptr carries no type.
        //
        // The two operands use different encodings for historical reasons:
        //   a (rhs)  -1 means string, >= 0 is a MemType index;
        //   b (lhs)   0 means string, > 0 is a MemType index plus one.
        // `a` cannot be renumbered without invalidating existing modules, so `b`
        // carries the plus one to keep 0 meaning "string" (which is also what a
        // module written before `b` existed holds, so those still work).
        if (expr.binop == BinOp::Add && expr.type != nullptr &&
            expr.type->kind == TypeKind::String) {
            emitExpr(*expr.kids[0]);
            emitExpr(*expr.kids[1]);
            const bool rhsIsString = rhsType != nullptr && rhsType->kind == TypeKind::String;
            const bool lhsIsString = lhsType != nullptr && lhsType->kind == TypeKind::String;
            const int rhsCode = rhsIsString ? -1 : memTypeOf(rhsType);
            const int lhsCode = lhsIsString ? 0 : memTypeOf(lhsType) + 1;
            m_fn->emit(OpCode::Concat, rhsCode, lhsCode);
            return;
        }

        // String **ordering** also compares contents, which the numeric path below
        // cannot do: a string operand would be treated as a descriptor address and
        // the result would be whatever the addresses happen to be -- `"b" < "a"`
        // and `"a" < "b"` both came out true before this case existed. Emit a
        // three-way compare, then reduce it to the requested predicate.
        if ((expr.binop == BinOp::Lt || expr.binop == BinOp::Le ||
             expr.binop == BinOp::Gt || expr.binop == BinOp::Ge) &&
            lhsType != nullptr && lhsType->kind == TypeKind::String) {
            emitExpr(*expr.kids[0]);
            emitExpr(*expr.kids[1]);
            m_fn->emit(OpCode::StrCmp);
            emitConstInt(0);
            OpCode predicate = OpCode::Lt;
            if (expr.binop == BinOp::Le) {
                predicate = OpCode::Le;
            } else if (expr.binop == BinOp::Gt) {
                predicate = OpCode::Gt;
            } else if (expr.binop == BinOp::Ge) {
                predicate = OpCode::Ge;
            }
            m_fn->emit(predicate, int64MemType());
            return;
        }

        // String equality compares contents.
        if ((expr.binop == BinOp::Eq || expr.binop == BinOp::Ne) && lhsType != nullptr &&
            lhsType->kind == TypeKind::String) {
            emitExpr(*expr.kids[0]);
            emitExpr(*expr.kids[1]);
            m_fn->emit(OpCode::StrEq);
            if (expr.binop == BinOp::Ne) {
                m_fn->emit(OpCode::Not);
            }
            return;
        }

        // Aggregate equality compares the storage bytes.
        if ((expr.binop == BinOp::Eq || expr.binop == BinOp::Ne) && lhsType != nullptr &&
            lhsType->isAggregate()) {
            emitBaseAddress(*expr.kids[0]);
            emitBaseAddress(*expr.kids[1]);
            m_fn->emit(OpCode::MemEq, static_cast<std::int32_t>(lhsType->sizeBytes));
            if (expr.binop == BinOp::Ne) {
                m_fn->emit(OpCode::Not);
            }
            return;
        }

        // Pointer arithmetic scales by the pointee size.
        if ((lhsType != nullptr && lhsType->isPtr()) || (rhsType != nullptr && rhsType->isPtr())) {
            if (expr.binop == BinOp::Lt || expr.binop == BinOp::Le || expr.binop == BinOp::Gt ||
                expr.binop == BinOp::Ge || expr.binop == BinOp::Eq || expr.binop == BinOp::Ne) {
                emitExpr(*expr.kids[0]);
                emitExpr(*expr.kids[1]);
                m_fn->emit(arithmeticOpCode(expr.binop), ptrMemType());
                return;
            }
            if (expr.binop == BinOp::Sub && lhsType->isPtr() && rhsType->isPtr()) {
                // `p - q` is the number of elements between the two addresses,
                // not the raw byte distance (spec §4.2.2). The subtraction
                // itself is byte-wise, so scale by the pointee size.
                emitExpr(*expr.kids[0]);
                emitExpr(*expr.kids[1]);
                m_fn->emit(OpCode::Sub, ptrMemType());
                const std::uint32_t elementSize = lhsType->elem->sizeBytes;
                if (elementSize > 1) {
                    emitConstInt(static_cast<Int128>(elementSize));
                    m_fn->emit(OpCode::Div, int64MemType());
                }
                return;
            }
            const bool pointerOnLeft = lhsType->isPtr();
            const Type* pointerType = pointerOnLeft ? lhsType : rhsType;
            const std::uint32_t stride = pointerType->elem->sizeBytes;
            emitExpr(*expr.kids[0]);
            if (!pointerOnLeft) {
                emitConstInt(static_cast<Int128>(stride));
                m_fn->emit(OpCode::Mul, int64MemType());
            }
            emitExpr(*expr.kids[1]);
            if (pointerOnLeft) {
                emitConstInt(static_cast<Int128>(stride));
                m_fn->emit(OpCode::Mul, int64MemType());
            }
            if (expr.binop == BinOp::Sub && pointerOnLeft) {
                // `p - n` is pointer arithmetic, but emitting SUB here would take
                // the VM's **pointer difference** path (`p - q`), which builds the
                // result with `Value::fromInt` -- the address bits are right, the
                // value is no longer tagged as an address. Anything that dispatches
                // on the tag then misreads it: printing `@(data + n - 1)` gave an
                // empty string while `@(data + n)` worked, and assigning to a
                // variable "worked" only because the bits survived the store.
                // Negating the scaled offset and adding keeps the result a pointer.
                m_fn->emit(OpCode::Neg, int64MemType());
                m_fn->emit(OpCode::Add, ptrMemType());
            } else {
                m_fn->emit(expr.binop == BinOp::Sub ? OpCode::Sub : OpCode::Add, ptrMemType());
            }
            return;
        }

        // Plain numeric / comparison.
        const Type* operandType = expr.type;
        if (isComparisonBinOp(expr.binop)) {
            if (lhsType != nullptr && rhsType != nullptr &&
                m_sema.types().same(lhsType, rhsType)) {
                operandType = lhsType;
            } else if (lhsType != nullptr && rhsType != nullptr) {
                const Type* merged = m_sema.types().mergeArithmetic(lhsType, rhsType);
                operandType = merged != nullptr ? merged : lhsType;
            }
        }

        emitExpr(*expr.kids[0]);
        emitConvert(lhsType, operandType);
        emitExpr(*expr.kids[1]);
        emitConvert(rhsType, operandType);
        m_fn->emit(arithmeticOpCode(expr.binop), memTypeOf(operandType));
        return;
    }

    case ExprKind::Assign:
        emitAssign(expr);
        return;

    case ExprKind::Ternary: {
        emitExpr(*expr.kids[0]);
        const std::size_t toElse = m_fn->emitJump(OpCode::JumpIfFalse);
        emitExpr(*expr.kids[1]);
        const std::size_t toEnd = m_fn->emitJump(OpCode::Jump);
        m_fn->patchJump(toElse, here());
        emitExpr(*expr.kids[2]);
        m_fn->patchJump(toEnd, here());
        return;
    }

    case ExprKind::Cast: {
        const Type* from = expr.kids[0]->type;
        const Type* to = expr.type;
        emitExpr(*expr.kids[0]);
        if (from != nullptr && to != nullptr) {
            if (to->isBit()) {
                // `as bitN` truncates: the low N bits are kept and the rest dropped,
                // which is what a bit string means. The VM's cast *traps* on an
                // out-of-range integer, and that rule is right for `as int8` -- so
                // the mask is emitted here instead of weakening it. Widening to
                // int128 first keeps the cast itself from tripping the range check.
                MemType wide;
                wide.kind = MemType::Kind::Int;
                wide.size = 16;
                wide.bits = 128;
                wide.isUnsigned = false;
                const int wideType = m_module->internMemoryType(wide);
                m_fn->emit(OpCode::Cast, memTypeOf(from), wideType);
                if (to->bits < 128) {
                    emitConstInt((static_cast<Int128>(1) << to->bits) - 1);
                    m_fn->emit(OpCode::And, wideType);
                }
                return;
            }
            m_fn->emit(OpCode::Cast, memTypeOf(from), memTypeOf(to));
        }
        return;
    }

    case ExprKind::Call: {
        if (expr.builtin >= 0) {
            const Builtin builtin = static_cast<Builtin>(expr.builtin);
            if (builtin == Builtin::Assert && !m_assertions) {
                // With assertions disabled the call generates no code and the
                // argument is not evaluated (spec §8.9).
                return;
            }
            std::int32_t operandType = 0;
            for (std::size_t i = 1; i < expr.kids.size(); ++i) {
                emitExpr(*expr.kids[i]);
            }
            switch (builtin) {
            case Builtin::Print:
            case Builtin::EPrint:
            case Builtin::Log:
                if (expr.kids.size() > 1) {
                    const Type* type = expr.kids[1]->type;
                    operandType = type != nullptr && type->kind == TypeKind::String
                                      ? -1
                                      : memTypeOf(type);
                }
                break;
            default:
                operandType = 0;
                break;
            }
            m_fn->emit(OpCode::Builtin, static_cast<std::int32_t>(builtin), operandType);
            return;
        }

        if (expr.indirectCall) {
            // Arguments first, then the handle: CALL_INDIRECT pops the callee and then
            // the arguments, which is the same order the interpreter's CALL expects
            // (last pushed, first popped) with the target on top.
            const Type* signature = expr.kids[0]->type;
            if (signature == nullptr || !signature->isFunc()) {
                m_diagnostics.error(expr.range, "internal error: indirect call to a non-Func");
                emitConstInt(0);
                return;
            }
            // An aggregate result is returned into a destination the caller supplies,
            // exactly as the direct call does: the callee's epilogue writes through it,
            // and without one the interpreter's CALL reports a missing destination.
            const Type* returnType = signature->funcReturn();
            if (returnType != nullptr && returnType->isAggregate()) {
                const std::uint32_t destination = allocTemp(returnType);
                emitMemZero(destination, returnType->sizeBytes);
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(destination));
            }
            const std::size_t argc = expr.kids.size() - 1;
            for (std::size_t i = 0; i < argc; ++i) {
                Expr& argument = *expr.kids[i + 1];
                if (signature->funcParam(i) != nullptr &&
                    signature->funcParam(i)->isAggregate()) {
                    emitBaseAddress(argument);
                } else {
                    emitExpr(argument);
                    emitConvert(argument.type, signature->funcParam(i));
                }
            }
            emitExpr(*expr.kids[0]);
            m_fn->emit(OpCode::SafePoint);
            m_fn->emit(OpCode::CallIndirect, 0, static_cast<std::int32_t>(argc));
            return;
        }

        if (expr.funcIndex < 0 ||
            static_cast<std::size_t>(expr.funcIndex) >= m_sema.functions().size()) {
            m_diagnostics.error(expr.range, "internal error: unresolved call");
            emitConstInt(0);
            return;
        }

        const FuncInfo& callee = m_sema.functions()[static_cast<std::size_t>(expr.funcIndex)];
        const bool aggregateResult = callee.ret != nullptr && callee.ret->isAggregate();
        if (aggregateResult) {
            const std::uint32_t destination = allocTemp(callee.ret);
            emitMemZero(destination, callee.ret->sizeBytes);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(destination));
        }

        for (std::size_t i = 0; i < callee.params.size() && i + 1 < expr.kids.size(); ++i) {
            const Type* paramType = callee.params[i];
            Expr& argument = *expr.kids[i + 1];
            if (paramType->isAggregate()) {
                emitBaseAddress(argument);
            } else {
                emitExpr(argument);
                emitConvert(argument.type, paramType);
            }
        }

        m_fn->emit(OpCode::SafePoint);
        m_fn->emit(OpCode::Call, expr.funcIndex,
                   static_cast<std::int32_t>(callee.params.size()));
        return;
    }

    case ExprKind::Index: {
        const Type* baseType = expr.kids[0]->type;
        if (baseType != nullptr && baseType->kind == TypeKind::Array &&
            baseType->elem->bitElementsPerByte() > 0) {
            // Packed element: the byte address alone does not name it, so the shift
            // happens here rather than in `emitLoadValue`.
            const std::uint32_t bitSlot = emitPackedElementAddress(expr);
            MemType byteType;
            byteType.kind = MemType::Kind::Int;
            byteType.size = 1;
            byteType.bits = 8;
            byteType.isUnsigned = true;
            const int byteMemType = m_module->internMemoryType(byteType);

            m_fn->emit(OpCode::Load, byteMemType);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(bitSlot));
            m_fn->emit(OpCode::Load, int64MemType());
            m_fn->emit(OpCode::Shr, int64MemType());
            emitConstInt((static_cast<Int128>(1) << baseType->elem->bits) - 1);
            m_fn->emit(OpCode::And, int64MemType());
            return;
        }
        if (baseType != nullptr && baseType->kind == TypeKind::String) {
            // One byte, zero extended into the `char` the expression has for a type.
            // Reading it with `emitLoadValue` would use the type's own width -- two bytes
            // -- and hand back the byte under `i` welded to the byte under `i+1`. That is
            // where `⬱` came from: "1+1+1"[0] was 0x2B31, '+' above '1'.
            //
            // Unsigned rather than signed so a byte >= 0x80 does not come back negative;
            // a UTF-8 fragment is a byte value, not a small integer.
            MemType byteType;
            byteType.kind = MemType::Kind::Int;
            byteType.size = 1;
            byteType.bits = 8;
            byteType.isUnsigned = true;
            const int byteMemType = m_module->internMemoryType(byteType);

            emitAddress(expr);
            m_fn->emit(OpCode::Load, byteMemType);
            return;
        }
        // `global[i]` for a scalar element becomes one instruction, emitted here rather than
        // fused later.
        //
        // `LOAD_GLOBAL_INDEXED` carries the index slot and the element type and performs both
        // bounds checks itself, which is what allows it to be a single dispatch. Emitting it
        // directly means the two checks are never generated and therefore never need removing:
        // this code knows the base is a global array and knows its element count, so there is
        // nothing for a later pass to prove.
        //
        // The alternative was a peephole that recognised `CONST 0; CMP; TRAP; LOAD_LOCAL;
        // CONST N; CMP; TRAP; GLOBAL_ADDR; LOAD_LOCAL; CONST size; MUL; ADD; LOAD` and deleted
        // the checks from it. Four attempts at matching that window, one of which also deleted
        // the index reload -- the fused load then read a stale slot, and `global_index_check`
        // reported a wrong value rather than a wrong trap. Emitting the instruction the
        // generator actually means has no such failure mode.
        if (baseType != nullptr && baseType->kind == TypeKind::Array && expr.kids[0]->isLValue) {
            Expr& base = *expr.kids[0];
            if (base.kind == ExprKind::Name && base.sym != nullptr &&
                base.sym->kind == Symbol::Kind::Global &&
                baseType->elem->sizeBytes >= 1 && baseType->elem->sizeBytes <= 8) {
                const int elementType = memTypeOf(baseType->elem);
                const std::uint32_t indexSlot = allocRawTemp(8, 8);
                // The instruction encodes the slot in 16 bits. A frame large enough to push
                // the slot past that keeps the general path; the reserved slot then goes
                // unused, which costs eight bytes of frame and nothing else.
                if (loadGlobalIndexed::encodable(static_cast<std::int32_t>(indexSlot),
                                                 elementType)) {
                    // The index is evaluated once, into a 64-bit slot -- the same staging the
                    // general path performs, and the reason the wide flag is set.
                    emitExpr(*expr.kids[1]);
                    m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
                    m_fn->emit(OpCode::Swap);
                    m_fn->emit(OpCode::Store, int64MemType());
                    m_fn->emit(OpCode::LoadGlobalIndexed, base.sym->globalIndex,
                               loadGlobalIndexed::pack(static_cast<std::int32_t>(indexSlot),
                                                       elementType, /*wideIndex=*/true));
                    return;
                }
            }
        }
        emitAddress(expr);
        emitLoadValue(expr.type);
        return;
    }

    case ExprKind::Slice:
        emitBaseAddress(*expr.kids[0]);
        emitExpr(*expr.kids[1]);
        emitExpr(*expr.kids[2]);
        m_fn->emit(OpCode::Builtin, static_cast<std::int32_t>(Builtin::Substring), 0);
        return;

    case ExprKind::Member: {
        if (expr.memberIndex == kMemberLength) {
            const Type* baseType = expr.kids[0]->type;
            if (baseType != nullptr && baseType->kind == TypeKind::Array) {
                emitConstInt(static_cast<Int128>(baseType->length));
                return;
            }
            emitBaseAddress(*expr.kids[0]);
            emitConstInt(8);
            m_fn->emit(OpCode::Add, ptrMemType());
            m_fn->emit(OpCode::Load, int64MemType());
            return;
        }
        if (expr.memberIndex == kMemberPtr) {
            emitBaseAddress(*expr.kids[0]);
            m_fn->emit(OpCode::Load, ptrMemType());
            return;
        }
        if (const FieldInfo* bitField = bitFieldOf(expr)) {
            // Read the byte that holds the field, shift the field down to bit 0 and
            // drop everything above it. `loadScalar` cannot do this for us: it masks
            // by the *type's* width at the byte's own offset, while a bit field sits
            // at an offset inside the byte.
            MemType byteType;
            byteType.kind = MemType::Kind::Int;
            byteType.size = 1;
            byteType.bits = 8;
            byteType.isUnsigned = true;
            const int byteMemType = m_module->internMemoryType(byteType);

            emitAddress(expr);
            m_fn->emit(OpCode::Load, byteMemType);
            if (bitField->bitOffset != 0) {
                emitConstInt(bitField->bitOffset);
                m_fn->emit(OpCode::Shr, int64MemType());
            }
            emitConstInt((static_cast<Int128>(1) << bitField->bitWidth) - 1);
            m_fn->emit(OpCode::And, int64MemType());
            return;
        }
        emitAddress(expr);
        emitLoadValue(expr.type);
        return;
    }

    case ExprKind::Arrow:
        emitAddress(expr);
        emitLoadValue(expr.type);
        return;

    case ExprKind::PreInc:
    case ExprKind::PreDec:
    case ExprKind::PostInc:
    case ExprKind::PostDec:
        emitIncrement(expr, expr.kind == ExprKind::PreInc || expr.kind == ExprKind::PostInc,
                      expr.kind == ExprKind::PreInc || expr.kind == ExprKind::PreDec);
        return;

    case ExprKind::TupleLit: {
        const Type* type = expr.type;
        if (type == nullptr || type->kind != TypeKind::Tuple) {
            emitConstInt(0);
            return;
        }
        const std::uint32_t temp = allocTemp(type);
        emitMemZero(temp, type->sizeBytes);
        for (std::size_t i = 0; i < expr.kids.size() && i < type->elems.size(); ++i) {
            emitInitialize(*expr.kids[i], temp + type->fields[i].offset, type->elems[i]);
        }
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(temp));
        return;
    }

    case ExprKind::SizeOf:
    case ExprKind::AlignOf:
        emitConstInt(expr.constInt);
        return;

    case ExprKind::InitList:
        m_diagnostics.error(expr.range, "internal error: initialiser list outside a declaration");
        emitConstInt(0);
        return;

    case ExprKind::Range:
        m_diagnostics.error(expr.range, "internal error: range expression outside a slice");
        emitConstInt(0);
        return;
    }
}

void IRGen::emitAssign(Expr& expr) {
    const Type* targetType = expr.kids[0]->type;
    const Type* valueType = expr.kids[1]->type;

    // The value of this assignment *is* consumed by the enclosing expression
    // (or by nothing, if we are a statement). Either way the right hand side
    // always produces a value, so the discard flag must not reach into it --
    // otherwise `y = x = 5;` would drop the inner store's value and unbalance
    // the stack.
    const bool discard = m_discardResult;
    m_discardResult = false;

    if (expr.assignop == AssignOp::Assign) {
        if (expr.kids[0]->kind == ExprKind::Index) {
            const Type* baseType = expr.kids[0]->kids[0]->type;
            if (baseType != nullptr && baseType->kind == TypeKind::Array &&
                baseType->elem->bitElementsPerByte() > 0) {
                // Same read-modify-write as a struct bit field, except the field's
                // position is a runtime value here: the mask has to be shifted by it
                // rather than baked in.
                const Type* elem = baseType->elem;
                const Int128 fieldMask = (static_cast<Int128>(1) << elem->bits) - 1;
                MemType byteType;
                byteType.kind = MemType::Kind::Int;
                byteType.size = 1;
                byteType.bits = 8;
                byteType.isUnsigned = true;
                const int byteMemType = m_module->internMemoryType(byteType);

                const std::uint32_t bitSlot = emitPackedElementAddress(*expr.kids[0]);
                const std::uint32_t maskSlot = allocRawTemp(8, 8);

                m_fn->emit(OpCode::Dup);                        // [addr, addr]
                m_fn->emit(OpCode::Load, byteMemType);          // [addr, byte]

                // shiftedMask = fieldMask << bitOffset
                emitConstInt(fieldMask);                        // [addr, byte, fm]
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(bitSlot));
                m_fn->emit(OpCode::Load, int64MemType());       // [addr, byte, fm, off]
                m_fn->emit(OpCode::Shl, int64MemType());        // [addr, byte, shifted]
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(maskSlot));
                m_fn->emit(OpCode::Swap);
                m_fn->emit(OpCode::Store, int64MemType());      // [addr, byte]

                // byte & ~shifted
                emitConstInt(-1);
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(maskSlot));
                m_fn->emit(OpCode::Load, int64MemType());
                m_fn->emit(OpCode::Xor, int64MemType());        // [addr, byte, ~shifted]
                m_fn->emit(OpCode::And, int64MemType());        // [addr, cleared]

                emitExpr(*expr.kids[1]);
                emitConvert(valueType, targetType);
                if (!discard) {
                    m_fn->emit(OpCode::Dup);
                }
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(bitSlot));
                m_fn->emit(OpCode::Load, int64MemType());
                m_fn->emit(OpCode::Shl, int64MemType());        // [addr, cleared, placed]
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(maskSlot));
                m_fn->emit(OpCode::Load, int64MemType());
                m_fn->emit(OpCode::And, int64MemType());        // [addr, cleared, placed]
                m_fn->emit(OpCode::Or, int64MemType());         // [addr, merged]
                m_fn->emit(OpCode::Store, byteMemType);
                return;
            }
        }
        if (const FieldInfo* bitField = bitFieldOf(*expr.kids[0])) {
            // A bit field cannot be stored to: a store would overwrite the seven
            // neighbouring fields that share the byte. So this is a read-modify-write
            // -- load the byte, clear the field's bits, or in the new value, store the
            // byte back. That is also why a bit field never straddles a byte: with one
            // byte involved, the sequence cannot tear across a boundary.
            const Int128 fieldMask = (static_cast<Int128>(1) << bitField->bitWidth) - 1;
            const Int128 shiftedMask = fieldMask << bitField->bitOffset;
            MemType byteType;
            byteType.kind = MemType::Kind::Int;
            byteType.size = 1;
            byteType.bits = 8;
            byteType.isUnsigned = true;
            const int byteMemType = m_module->internMemoryType(byteType);

            emitAddress(*expr.kids[0]);                        // [addr]
            m_fn->emit(OpCode::Dup);                           // [addr, addr]
            m_fn->emit(OpCode::Load, byteMemType);             // [addr, byte]
            emitConstInt(~shiftedMask);                        // [addr, byte, ~mask]
            m_fn->emit(OpCode::And, int64MemType());           // [addr, cleared]
            emitExpr(*expr.kids[1]);
            emitConvert(valueType, targetType);
            if (!discard) {
                // The assignment's value is the value assigned, not the merged byte.
                m_fn->emit(OpCode::Dup);
            }
            if (bitField->bitOffset != 0) {
                emitConstInt(bitField->bitOffset);
                m_fn->emit(OpCode::Shl, int64MemType());
            }
            emitConstInt(shiftedMask);
            m_fn->emit(OpCode::And, int64MemType());           // [addr, cleared, placed]
            m_fn->emit(OpCode::Or, int64MemType());            // [addr, merged]
            m_fn->emit(OpCode::Store, byteMemType);
            return;
        }
        if (targetType != nullptr && targetType->isAggregate()) {
            // MEM_COPY pops the destination first, so the stack has to be
            // [source, destination] with the destination on top. Duplicating
            // *after* pushing the destination (which is what this used to do)
            // left [source, destination, destination], and the copy then read
            // the destination as its own source -- `s = t` for a string or a
            // struct silently did nothing.
            emitBaseAddress(*expr.kids[1]);                       // [source]
            if (!discard) {
                // The value of an aggregate assignment is the assigned
                // aggregate: keep a copy for the enclosing expression.
                m_fn->emit(OpCode::Dup);                          // [source, source]
            }
            emitAddress(*expr.kids[0]);                           // [source, (source,) destination]
            m_fn->emit(OpCode::MemCopy, static_cast<std::int32_t>(targetType->sizeBytes));
            return;
        }
        OpCode storeCode = OpCode::StoreLocal;
        std::int32_t slot = 0;
        int memType = 0;
        if (fusedVariable(*expr.kids[0], storeCode, slot, memType)) {
            emitExpr(*expr.kids[1]);
            emitConvert(valueType, targetType);
            m_fn->emit(storeCode == OpCode::LoadGlobal ? OpCode::StoreGlobal
                                                      : OpCode::StoreLocal,
                       slot, memType | (discard ? 0 : 0x100));
            return;
        }
        emitAddress(*expr.kids[0]);
        emitExpr(*expr.kids[1]);
        emitConvert(valueType, targetType);
        m_fn->emit(discard ? OpCode::Store : OpCode::StoreKeep, memTypeOf(targetType));
        return;
    }

    // `s += rhs` on a string is concatenation, and it has to be built here rather than
    // left to the arithmetic path below: that path emits ADD on the two operands, and a
    // string operand is a descriptor address, so `s += "b"` would have added two pointers
    // together and stored the sum.
    //
    // The two steps are the ones `s = s + rhs` already takes -- CONCAT to build the new
    // string, then the aggregate copy to store the 16 byte descriptor -- with the target's
    // address read once and parked. Reading it twice via `emitExpr` and again via
    // `emitAddress` would index twice in `arr[i] += s`, and the second index could see a
    // different `i` than the first.
    if (targetType != nullptr && targetType->kind == TypeKind::String &&
        expr.assignop == AssignOp::Add) {
        const std::uint32_t addrSlot = allocRawTemp(8, 8);
        emitAddress(*expr.kids[0]);                            // [&target]
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(addrSlot));
        m_fn->emit(OpCode::Swap);                              // [&slot, &target]
        m_fn->emit(OpCode::Store, ptrMemType());               // []

        // The address *is* the value: a string is passed around as the address of its
        // descriptor, which is what CONCAT reads.
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(addrSlot));
        m_fn->emit(OpCode::Load, ptrMemType());                // [target]
        emitExpr(*expr.kids[1]);                               // [target, rhs]
        const bool rhsIsString = valueType != nullptr && valueType->kind == TypeKind::String;
        const int rhsCode = rhsIsString || valueType == nullptr ? -1 : memTypeOf(valueType);
        m_fn->emit(OpCode::Concat, rhsCode, 0);                // [concatenated]
        if (!discard) {
            m_fn->emit(OpCode::Dup);                           // [result, result]
        }
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(addrSlot));
        m_fn->emit(OpCode::Load, ptrMemType());                // [..., &target]
        m_fn->emit(OpCode::MemCopy, static_cast<std::int32_t>(targetType->sizeBytes));
        return;
    }

    const BinOp op = binOpOfAssign(expr.assignop);
    const int operandType = memTypeOf(targetType);
    OpCode storeCode = OpCode::StoreLocal;
    std::int32_t slot = 0;
    int memType = 0;
    const bool fused = fusedVariable(*expr.kids[0], storeCode, slot, memType);
    if (fused) {
        m_fn->emit(storeCode, slot, memType);
    } else {
        emitAddress(*expr.kids[0]);
        m_fn->emit(OpCode::Dup);
        m_fn->emit(OpCode::Load, operandType);
    }
    emitExpr(*expr.kids[1]);
    emitConvert(valueType, targetType);
    m_fn->emit(arithmeticOpCode(op), operandType);
    if (fused) {
        m_fn->emit(storeCode == OpCode::LoadGlobal ? OpCode::StoreGlobal
                                                  : OpCode::StoreLocal,
                   slot, memType | (discard ? 0 : 0x100));
    } else if (discard) {
        m_fn->emit(OpCode::Store, operandType);
    } else {
        m_fn->emit(OpCode::StoreKeep, operandType);
    }
}

void IRGen::emitIncrement(Expr& expr, bool isIncrement, bool isPrefix, bool keepResult) {
    const Type* type = expr.kids[0]->type;
    const int operandType = memTypeOf(type);
    const OpCode op = isIncrement ? OpCode::Add : OpCode::Sub;
    // On a pointer the step is one element, so the added constant is the
    // pointee size rather than 1.
    const Int128 step = type != nullptr && type->isPtr() ? static_cast<Int128>(type->elem->sizeBytes)
                                                        : static_cast<Int128>(1);

    OpCode loadCode = OpCode::LoadLocal;
    std::int32_t slot = 0;
    int fusedType = 0;
    if (fusedVariable(*expr.kids[0], loadCode, slot, fusedType)) {
        // `x++` as a statement (result discarded) collapses into one step
        // instruction. The step has to fit the packed 24 bit immediate.
        if (!keepResult && loadCode == OpCode::LoadLocal && step <= 0x7F'FFFF) {
            const std::int32_t delta = static_cast<std::int32_t>(step);
            m_fn->emit(OpCode::StepLocal, slot,
                       fusedType | ((isIncrement ? delta : -delta) << 8));
            return;
        }
        const OpCode storeCode = loadCode == OpCode::LoadGlobal ? OpCode::StoreGlobal
                                                               : OpCode::StoreLocal;
        m_fn->emit(loadCode, slot, fusedType);
        emitConstInt(step);
        m_fn->emit(op, fusedType);
        m_fn->emit(storeCode, slot, fusedType | (keepResult ? 0x100 : 0));
        return;
    }

    emitAddress(*expr.kids[0]);

    if (!keepResult) {
        // The result is discarded (`x++;` as a statement), so neither the old
        // value nor the new one needs to be left on the stack.
        m_fn->emit(OpCode::Dup);
        m_fn->emit(OpCode::Load, operandType);
        emitConstInt(step);
        m_fn->emit(op, operandType);
        m_fn->emit(OpCode::Store, operandType);
        return;
    }

    if (isPrefix) {
        m_fn->emit(OpCode::Dup);
        m_fn->emit(OpCode::Load, operandType);
        emitConstInt(step);
        m_fn->emit(op, operandType);
        m_fn->emit(OpCode::StoreKeep, operandType);
        return;
    }

    // Postfix: keep the old value.
    m_fn->emit(OpCode::Dup);        // [addr, addr]
    m_fn->emit(OpCode::Load, operandType);  // [addr, old]
    m_fn->emit(OpCode::Swap);       // [old, addr]
    m_fn->emit(OpCode::Dup);        // [old, addr, addr]
    m_fn->emit(OpCode::Load, operandType);  // [old, addr, old]
    emitConstInt(step);
    m_fn->emit(op, operandType);    // [old, addr, new]
    m_fn->emit(OpCode::Store, operandType); // [old]
}

} // namespace acvm
