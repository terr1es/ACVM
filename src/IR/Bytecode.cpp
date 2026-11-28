//===--- Bytecode.cpp ----------------------------------------------------===//
#include "acvm/IR/Bytecode.h"

#include <utility>

namespace acvm {

std::string_view opCodeName(OpCode op) {
    switch (op) {
    case OpCode::ConstInt: return "CONST_INT";
    case OpCode::ConstFloat: return "CONST_FLOAT";
    case OpCode::ConstStr: return "CONST_STR";
    case OpCode::LocalAddr: return "LOCAL_ADDR";
    case OpCode::GlobalAddr: return "GLOBAL_ADDR";
    case OpCode::Load: return "LOAD";
    case OpCode::Store: return "STORE";
    case OpCode::StoreKeep: return "STORE_KEEP";
    case OpCode::Dup: return "DUP";
    case OpCode::Swap: return "SWAP";
    case OpCode::Pop: return "POP";
    case OpCode::Add: return "ADD";
    case OpCode::Sub: return "SUB";
    case OpCode::Mul: return "MUL";
    case OpCode::Div: return "DIV";
    case OpCode::Mod: return "MOD";
    case OpCode::Neg: return "NEG";
    case OpCode::BitNot: return "BITNOT";
    case OpCode::Not: return "NOT";
    case OpCode::And: return "AND";
    case OpCode::Or: return "OR";
    case OpCode::Xor: return "XOR";
    case OpCode::Shl: return "SHL";
    case OpCode::Shr: return "SHR";
    case OpCode::Eq: return "EQ";
    case OpCode::Ne: return "NE";
    case OpCode::Lt: return "LT";
    case OpCode::Le: return "LE";
    case OpCode::Gt: return "GT";
    case OpCode::Ge: return "GE";
    case OpCode::StrEq: return "STR_EQ";
    case OpCode::StrCmp: return "STR_CMP";
    case OpCode::MemEq: return "MEM_EQ";
    case OpCode::Cast: return "CAST";
    case OpCode::Jump: return "JUMP";
    case OpCode::JumpIfFalse: return "JUMP_IF_FALSE";
    case OpCode::JumpIfTrue: return "JUMP_IF_TRUE";
    case OpCode::Call: return "CALL";
    case OpCode::CallIndirect: return "CALL_INDIRECT";
    case OpCode::Ret: return "RET";
    case OpCode::MemCopy: return "MEM_COPY";
    case OpCode::MemZero: return "MEM_ZERO";
    case OpCode::Builtin: return "BUILTIN";
    case OpCode::Concat: return "CONCAT";
    case OpCode::ArenaMark: return "ARENA_MARK";
    case OpCode::ArenaRelease: return "ARENA_RELEASE";
    case OpCode::SafePoint: return "SAFE_POINT";
    case OpCode::Trap: return "TRAP";
    case OpCode::Halt: return "HALT";
    case OpCode::LoadLocal: return "LOAD_LOCAL";
    case OpCode::StoreLocal: return "STORE_LOCAL";
    case OpCode::LoadGlobal: return "LOAD_GLOBAL";
    case OpCode::StoreGlobal: return "STORE_GLOBAL";
    case OpCode::DerefLocal: return "DEREF_LOCAL";
    case OpCode::StepLocal: return "STEP_LOCAL";
    case OpCode::CopyStep: return "COPY_STEP";
    case OpCode::Pow: return "POW";
    case OpCode::CompareBranch: return "CMP_BRANCH";
    case OpCode::StepJump: return "STEP_JUMP";
    case OpCode::RetLocal: return "RET_LOCAL";
    case OpCode::CmpLocalImm: return "CMP_LOCAL_IMM";
    case OpCode::CopyLocal: return "COPY_LOCAL";
    case OpCode::AddSubLocalImm: return "ADD_SUB_LOCAL_IMM";
    }
    return "??";
}

const char* trapKindName(TrapKind kind) {
    switch (kind) {
    case TrapKind::Explicit: return "explicit trap()";
    case TrapKind::IntegerOverflow: return "integer overflow";
    case TrapKind::DivisionByZero: return "division by zero";
    case TrapKind::CastOutOfRange: return "cast out of range";
    case TrapKind::IndexOutOfBounds: return "index out of bounds";
    case TrapKind::NullPointer: return "null pointer dereference";
    case TrapKind::InvalidPointer: return "invalid pointer";
    case TrapKind::AllocationFailed: return "allocation failed";
    case TrapKind::AssertionFailed: return "assertion failed";
    case TrapKind::Unreachable: return "unreachable";
    case TrapKind::ExternalCallFailed: return "external call failed";
    case TrapKind::NegativeExponent:
        return "negative exponent: '**' produces an integer, and a negative power has no "
               "integer result; use 'pow' for a floating point value";
    }
    return "trap";
}

std::size_t Function::addConstant(Value value) {
    constants.push_back(std::move(value));
    return constants.size() - 1;
}

std::size_t Function::emit(OpCode op, std::int32_t a, std::int32_t b) {
    code.push_back(Instruction{op, a, b});
    lines.push_back(currentLine);
    return code.size() - 1;
}

SourceLine Function::lineAt(std::size_t index) const {
    if (index >= lines.size()) {
        return SourceLine{};
    }
    return lines[index];
}

std::size_t Function::emitJump(OpCode op) { return emit(op, 0, 0); }

void Function::patchJump(std::size_t position, std::size_t target) {
    if (position < code.size()) {
        code[position].a = static_cast<std::int32_t>(target);
    }
}

int Module::addFunction(Function function) {
    functions.push_back(std::move(function));
    return static_cast<int>(functions.size()) - 1;
}

int Module::findFunction(std::string_view name) const {
    for (std::size_t i = 0; i < functions.size(); ++i) {
        if (functions[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int Module::internMemoryType(const MemType& type) {
    for (std::size_t i = 0; i < memTypes.size(); ++i) {
        const MemType& candidate = memTypes[i];
        if (candidate.kind == type.kind && candidate.size == type.size &&
            candidate.bits == type.bits && candidate.isUnsigned == type.isUnsigned) {
            return static_cast<int>(i);
        }
    }
    memTypes.push_back(type);
    return static_cast<int>(memTypes.size()) - 1;
}

int Module::addString(std::string text) {
    strings.push_back(std::move(text));
    return static_cast<int>(strings.size()) - 1;
}

int Module::addExternalLibrary(std::string path) {
    for (std::size_t i = 0; i < externalLibraries.size(); ++i) {
        if (externalLibraries[i] == path) {
            return static_cast<int>(i);
        }
    }
    externalLibraries.push_back(std::move(path));
    return static_cast<int>(externalLibraries.size()) - 1;
}

} // namespace acvm
