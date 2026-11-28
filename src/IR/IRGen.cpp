//===--- IRGen.cpp - Module layout, function frames and statements ------===//
#include "acvm/IR/IRGen.h"

#include "acvm/Common/Diagnostic.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace acvm {
namespace {

std::uint32_t alignUp(std::uint32_t value, std::uint32_t align) {
    if (align <= 1) {
        return value;
    }
    return (value + align - 1) / align * align;
}

} // namespace

IRGen::IRGen(DiagnosticEngine& diagnostics, Sema& sema)
    : m_diagnostics(diagnostics), m_sema(sema) {}

//===----------------------------------------------------------------------===//
// Module
//===----------------------------------------------------------------------===//

namespace {

/// Fills `function.externalParams` from an Axiom signature. The rule is narrow
/// on purpose: `ptr T` followed by an integer means "an array of T plus its
/// element count", which is the only shape that needs marshalling. Everything
/// else crosses as a single 64 bit value.
void describeExternalParams(const FuncInfo& info, Function& function) {
    function.externalParams.clear();
    for (std::size_t i = 0; i < info.params.size(); ++i) {
        const Type* type = info.params[i];
        ExternalParam param;
        if (type != nullptr && type->isFloat()) {
            param.isFloat = true;
        } else if (type != nullptr && type->kind == TypeKind::String) {
            // A string crosses as (char pointer, length). It arrives as a
            // descriptor address otherwise, which is a pointer to `{data, len}`
            // in VM memory -- useful to nothing on the C side.
            param.isString = true;
        } else if (type != nullptr && type->isPtr() && i + 1 < info.params.size() &&
                   info.params[i + 1] != nullptr && info.params[i + 1]->isIntegerLike()) {
            param.isArray = true;
            param.elemSize = type->elem != nullptr ? type->elem->sizeBytes : 0;
            param.arrayReadOnly =
                i < info.paramPtrReadOnly.size() && info.paramPtrReadOnly[i];
            ++i;   // the count parameter is consumed by the pair
        }
        function.externalParams.push_back(param);
    }
}

} // namespace

bool IRGen::generate(Unit& unit, Module& module) {
    m_module = &module;
    module.functions.clear();
    module.globals.clear();
    module.memTypes.clear();
    module.strings.clear();
    module.processes.clear();
    module.events.clear();
    module.eventConditions.clear();
    module.externalLibraries.clear();
    // Reset the entry points too: a Module may be regenerated in place, and a
    // stale index would point into a vector that has just been cleared.
    module.entryPoint = -1;
    module.initFunction = -1;

    for (const FuncInfo& info : m_sema.functions()) {
        Function function;
        function.name = info.name;
        module.addFunction(std::move(function));
    }

    for (const GlobalInfo& info : m_sema.globals()) {
        GlobalSlot slot;
        slot.name = info.name;
        slot.size = info.type->sizeBytes;
        slot.align = info.type->alignBytes;
        module.globals.push_back(slot);
    }
    std::uint32_t offset = 0;
    for (GlobalSlot& slot : module.globals) {
        offset = alignUp(offset, std::max<std::uint32_t>(slot.align, 1));
        slot.offset = offset;
        offset += slot.size;
    }
    module.globalDataSize = alignUp(offset, 8);

    emitGlobalInit(unit);

    for (std::size_t i = 0; i < m_sema.functions().size(); ++i) {
        const FuncInfo& info = m_sema.functions()[i];
        if (info.decl == nullptr || !info.hasBody) {
            continue;
        }
        if (info.decl->funcKind == FuncKind::Event) {
            continue;
        }
        emitFunctionBody(*info.decl, static_cast<int>(i));
    }

    for (std::size_t i = 0; i < m_sema.functions().size(); ++i) {
        const FuncInfo& info = m_sema.functions()[i];
        if (info.decl == nullptr || !info.hasBody || info.decl->funcKind != FuncKind::Event) {
            continue;
        }
        emitFunctionBody(*info.decl, static_cast<int>(i));
        module.events.push_back(static_cast<int>(i));

        Function condition;
        condition.name = "__event_cond_" + info.name;
        condition.isEventCondition = true;
        condition.isSynthetic = true;
        const int conditionIndex = module.addFunction(std::move(condition));
        emitEventCondition(*info.decl, conditionIndex);
        module.eventConditions.push_back(conditionIndex);
    }

    for (std::size_t i = 0; i < m_sema.functions().size(); ++i) {
        if (m_sema.functions()[i].kind == FuncKind::Process && m_sema.functions()[i].hasBody) {
            module.processes.push_back(static_cast<int>(i));
        }
    }

    // Functions that were declared but never defined become trap stubs -- unless
    // the file is a library declaration file, where a missing body means "the
    // implementation is in a shared library" (see docs/NATIVE.md).
    for (std::size_t i = 0; i < m_sema.functions().size(); ++i) {
        const FuncInfo& info = m_sema.functions()[i];
        Function& function = module.functions[i];
        function.returnsValue = info.ret != nullptr && !info.ret->isVoid();
        function.returnsAggregate = info.ret != nullptr && info.ret->isAggregate();
        if (info.hasBody) {
            // An Axiom body and a library symbol can coexist: the .ax fallback
            // is compiled in, and the .dll is preferred when it is present. The
            // VM falls back to this code when resolution fails.
            if (!info.externalLibrary.empty()) {
                function.externalLibrary = module.addExternalLibrary(info.externalLibrary);
                function.externalSymbol = info.name;
                describeExternalParams(info, function);
                function.externalReturnKind =
                    !function.returnsValue ? 0 : (info.ret != nullptr && info.ret->isFloat()) ? 2 : 1;
            }
            continue;
        }
        const int libraryIndex =
            info.externalLibrary.empty() ? -1 : module.addExternalLibrary(info.externalLibrary);
        if (libraryIndex >= 0) {
            // External: record how to call it and leave the code empty.
            function.externalLibrary = libraryIndex;
            function.externalSymbol = info.name;
            function.frameSize = 0;
            function.code.clear();
            describeExternalParams(info, function);
            if (function.returnsAggregate) {
                m_diagnostics.error(info.loc,
                                    "'" + info.name + "' is declared in a library file but returns "
                                    "an aggregate; only scalars and pointers can cross the dynamic "
                                    "boundary");
            }
            function.externalReturnKind =
                !function.returnsValue ? 0 : (info.ret != nullptr && info.ret->isFloat()) ? 2 : 1;
            continue;
        }
        function.frameSize = 8;
        function.code.clear();
        function.code.push_back(Instruction{OpCode::Trap, static_cast<std::int32_t>(TrapKind::Unreachable), 0});
        function.code.push_back(Instruction{OpCode::Ret, 0, 0});
    }

    // A library declaration file has no entry point by design.
    if (unit.isExternalLibrary()) {
        module.entryPoint = -1;
        m_module = nullptr;
        return !m_diagnostics.hasErrors();
    }

    module.entryPoint = module.findFunction("main");
    if (module.entryPoint < 0) {
        m_diagnostics.error(SourceLocation{}, "no 'main' function in the module");
        return false;
    }

    m_module = nullptr;
    return !m_diagnostics.hasErrors();
}

void IRGen::emitGlobalInit(Unit& unit) {
    (void)unit;

    // No global needs an initialiser: emit nothing at all instead of an empty
    // function that would show up in the disassembly and inflate the count.
    const bool needsInit = [&]() {
        for (const GlobalInfo& info : m_sema.globals()) {
            if (info.decl != nullptr && info.decl->init != nullptr) {
                return true;
            }
        }
        return false;
    }();
    if (!needsInit) {
        m_module->initFunction = -1;
        return;
    }

    Function init;
    init.name = "__acvm_init_globals";
    init.isSynthetic = true;
    const int index = m_module->addFunction(std::move(init));
    m_module->initFunction = index;

    m_fn = &m_module->functions[static_cast<std::size_t>(index)];
    m_functionIndex = index;
    m_slots.clear();
    m_frameSize = 0;
    m_loops.clear();
    m_eof.clear();
    m_arenaDepth = 0;
    m_returnType = m_sema.types().voidType();
    m_fn->returnsValue = false;

    for (std::size_t g = 0; g < m_sema.globals().size(); ++g) {
        const GlobalInfo& info = m_sema.globals()[g];
        Stmt* decl = info.decl;
        if (decl == nullptr || decl->init == nullptr) {
            continue;
        }
        const Type* type = info.type;

        if (type->isAggregate()) {
            const std::uint32_t temp = allocTemp(type);
            emitMemZero(temp, type->sizeBytes);
            emitInitialize(*decl->init, temp, type);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(temp));
            m_fn->emit(OpCode::GlobalAddr, static_cast<std::int32_t>(g));
            m_fn->emit(OpCode::MemCopy, static_cast<std::int32_t>(type->sizeBytes));
        } else {
            m_fn->emit(OpCode::GlobalAddr, static_cast<std::int32_t>(g));
            emitExpr(*decl->init);
            emitConvert(decl->init->type, type);
            m_fn->emit(OpCode::Store, memTypeOf(type));
        }
    }

    m_fn->emit(OpCode::Ret);
    m_fn->frameSize = alignUp(m_frameSize, 8);
    m_fn = nullptr;
}

//===----------------------------------------------------------------------===//
// Frame helpers
//===----------------------------------------------------------------------===//

int IRGen::ptrMemType() {
    MemType type;
    type.kind = MemType::Kind::Ptr;
    type.size = 8;
    type.bits = 64;
    type.isUnsigned = true;
    return m_module->internMemoryType(type);
}

int IRGen::int64MemType() {
    MemType type;
    type.kind = MemType::Kind::Int;
    type.size = 8;
    type.bits = 64;
    type.isUnsigned = false;
    return m_module->internMemoryType(type);
}

bool IRGen::fusedVariable(const Expr& expr, OpCode& code, std::int32_t& slot, int& memType) const {
    if (expr.kind != ExprKind::Name || expr.sym == nullptr) {
        return false;
    }
    const Symbol* symbol = expr.sym;
    if (symbol->kind != Symbol::Kind::Local && symbol->kind != Symbol::Kind::Param &&
        symbol->kind != Symbol::Kind::Global) {
        return false;
    }
    const Type* type = expr.type != nullptr ? expr.type : symbol->type;
    if (type == nullptr || type->isAggregate() || type->isVoid()) {
        return false;
    }
    memType = const_cast<IRGen*>(this)->memTypeOf(type);
    if (symbol->kind == Symbol::Kind::Global) {
        code = OpCode::LoadGlobal;
        slot = symbol->globalIndex;
    } else {
        code = OpCode::LoadLocal;
        const auto it = m_slots.find(symbol);
        slot = it == m_slots.end() ? 0 : static_cast<std::int32_t>(it->second);
    }
    return true;
}

int IRGen::memTypeOf(const Type* type) {
    MemType mem;
    if (type == nullptr) {
        return m_module->internMemoryType(mem);
    }
    switch (type->kind) {
    case TypeKind::Float:
        mem.kind = MemType::Kind::Float;
        mem.size = static_cast<std::uint8_t>(std::max(1u, type->bits / 8));
        mem.bits = static_cast<std::uint8_t>(type->bits);
        mem.isUnsigned = false;
        break;
    case TypeKind::Ptr:
    case TypeKind::Null:
        mem.kind = MemType::Kind::Ptr;
        mem.size = 8;
        mem.bits = 64;
        mem.isUnsigned = true;
        break;
    case TypeKind::Char:
        mem.kind = MemType::Kind::Char;
        mem.size = 2;
        mem.bits = 16;
        mem.isUnsigned = true;
        break;
    case TypeKind::Bool:
        mem.kind = MemType::Kind::Bool;
        mem.size = 1;
        mem.bits = 8;
        mem.isUnsigned = true;
        break;
    case TypeKind::Enum:
        mem.kind = MemType::Kind::Int;
        mem.size = 4;
        mem.bits = 32;
        mem.isUnsigned = false;
        break;
    case TypeKind::Int:
        mem.kind = MemType::Kind::Int;
        mem.size = static_cast<std::uint8_t>((type->bits + 7) / 8);
        mem.bits = static_cast<std::uint8_t>(type->bits);
        mem.isUnsigned = type->isUnsigned;
        break;
    case TypeKind::Bit:
        // Same memory path as an unsigned integer of the same width: `loadScalar`
        // already masks by `bits` and zero-extends, which is exactly what a bit
        // string needs. The difference between the two types lives in Sema, not in
        // how the bytes are read.
        mem.kind = MemType::Kind::Int;
        mem.size = static_cast<std::uint8_t>((type->bits + 7) / 8);
        mem.bits = static_cast<std::uint8_t>(type->bits);
        mem.isUnsigned = true;
        break;
    default:
        mem.kind = MemType::Kind::Int;
        mem.size = 8;
        mem.bits = 64;
        break;
    }
    return m_module->internMemoryType(mem);
}

std::uint32_t IRGen::allocSlot(std::uint32_t size, std::uint32_t align) {
    m_frameSize = alignUp(m_frameSize, std::max<std::uint32_t>(align, 1));
    const std::uint32_t offset = m_frameSize;
    m_frameSize += std::max<std::uint32_t>(size, 1);
    return offset;
}

std::uint32_t IRGen::allocRawTemp(std::uint32_t size, std::uint32_t align) {
    return allocSlot(size, align);
}

std::uint32_t IRGen::allocTemp(const Type* type) {
    if (type == nullptr || type->isVoid()) {
        return allocSlot(8, 8);
    }
    return allocSlot(std::max<std::uint32_t>(type->sizeBytes, 1), type->alignBytes);
}

std::uint32_t IRGen::slotFor(Symbol* symbol) {
    const auto it = m_slots.find(symbol);
    return it == m_slots.end() ? 0 : it->second;
}

void IRGen::emitMemZero(std::uint32_t offset, std::uint32_t size) {
    if (size == 0) {
        return;
    }
    m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(offset));
    m_fn->emit(OpCode::MemZero, static_cast<std::int32_t>(size));
}

void IRGen::emitConstInt(Int128 value) {
    const std::size_t index = m_fn->addConstant(Value::fromInt(value));
    m_fn->emit(OpCode::ConstInt, static_cast<std::int32_t>(index));
}

void IRGen::emitConstFloat(double value) {
    const std::size_t index = m_fn->addConstant(Value::fromFloat(value));
    m_fn->emit(OpCode::ConstFloat, static_cast<std::int32_t>(index));
}

void IRGen::emitReleaseArenas(int depth) {
    for (int i = 0; i < depth; ++i) {
        m_fn->emit(OpCode::ArenaRelease);
    }
}

void IRGen::emitConvert(const Type* from, const Type* to) {
    if (from == nullptr || to == nullptr || from->isError() || to->isError()) {
        return;
    }
    if (to->isFloat() && !from->isFloat()) {
        m_fn->emit(OpCode::Cast, memTypeOf(from), memTypeOf(to));
    }
}

void IRGen::emitLoadValue(const Type* type) {
    if (type == nullptr || type->isAggregate() || type->isVoid()) {
        return;
    }
    m_fn->emit(OpCode::Load, memTypeOf(type));
}

void IRGen::emitBaseAddress(Expr& base) {
    if (base.isLValue) {
        emitAddress(base);
    } else {
        emitExpr(base);
    }
}

//===----------------------------------------------------------------------===//
// Function bodies
//===----------------------------------------------------------------------===//

void IRGen::emitFunctionBody(Stmt& decl, int funcIndex) {
    if (funcIndex < 0 || static_cast<std::size_t>(funcIndex) >= m_sema.functions().size()) {
        return;
    }
    const FuncInfo& info = m_sema.functions()[static_cast<std::size_t>(funcIndex)];
    Function& function = m_module->functions[static_cast<std::size_t>(funcIndex)];

    m_fn = &function;
    m_functionIndex = funcIndex;
    m_slots.clear();
    m_loops.clear();
    m_eof.clear();
    m_epilogueJumps.clear();
    m_frameSize = 0;
    m_arenaDepth = 0;
    m_processLoop = 0;
    m_returnType = info.ret;

    function.code.clear();
    function.constants.clear();
    function.params.clear();
    function.isProcess = decl.funcKind == FuncKind::Process;
    function.returnsAggregate = info.ret != nullptr && info.ret->isAggregate();
    function.returnSize = function.returnsAggregate ? info.ret->sizeBytes : 0;
    function.returnMemType = memTypeOf(info.ret);
    function.returnsValue = info.ret != nullptr && !info.ret->isVoid();

    if (function.returnsAggregate) {
        m_destinationOffset = allocSlot(8, 8);
    }
    m_returnOffset = 0;
    if (info.ret != nullptr && !info.ret->isVoid()) {
        m_returnOffset =
            allocSlot(info.ret->sizeBytes, std::max<std::uint32_t>(info.ret->alignBytes, 1));
    }

    for (Param& param : decl.params) {
        const Type* type = param.resolvedType;
        const std::uint32_t offset =
            allocSlot(std::max<std::uint32_t>(type->sizeBytes, 1), type->alignBytes);
        if (param.sym != nullptr) {
            m_slots[param.sym] = offset;
        }
        ParamInfo paramInfo;
        paramInfo.offset = offset;
        paramInfo.size = type->sizeBytes;
        paramInfo.memType = memTypeOf(type);
        paramInfo.aggregate = type->isAggregate();
        function.params.push_back(paramInfo);
    }

    m_eof.emplace_back();

    if (function.isProcess) {
        m_processLoop = here();
        function.emit(OpCode::SafePoint);
    }

    emitStmtList(decl.body);

    if (decl.defaultRet && m_returnType != nullptr && !m_returnType->isVoid()) {
        if (m_returnType->isAggregate()) {
            emitBaseAddress(*decl.defaultRet);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(m_returnOffset));
            m_fn->emit(OpCode::MemCopy, static_cast<std::int32_t>(m_returnType->sizeBytes));
        } else {
            emitExpr(*decl.defaultRet);
            emitConvert(decl.defaultRet->type, m_returnType);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(m_returnOffset));
            m_fn->emit(OpCode::Swap);
            m_fn->emit(OpCode::Store, memTypeOf(m_returnType));
        }
    }

    if (function.isProcess) {
        m_fn->emit(OpCode::Jump, static_cast<std::int32_t>(m_processLoop));
    }

    const std::size_t eofLabel = here();
    emitStmtList(decl.eofBody);
    for (const std::size_t jump : m_eof.back().returnJumps) {
        function.patchJump(jump, eofLabel);
    }
    m_eof.pop_back();

    const std::size_t epilogue = here();
    for (const std::size_t jump : m_epilogueJumps) {
        function.patchJump(jump, epilogue);
    }

    if (info.ret == nullptr || info.ret->isVoid()) {
        function.emit(OpCode::Ret);
    } else if (function.returnsAggregate) {
        function.emit(OpCode::LocalAddr, static_cast<std::int32_t>(m_returnOffset));
        function.emit(OpCode::LocalAddr, static_cast<std::int32_t>(m_destinationOffset));
        function.emit(OpCode::Load, ptrMemType());
        function.emit(OpCode::MemCopy, static_cast<std::int32_t>(function.returnSize));
        function.emit(OpCode::LocalAddr, static_cast<std::int32_t>(m_destinationOffset));
        function.emit(OpCode::Load, ptrMemType());
        function.emit(OpCode::Ret);
    } else {
        function.emit(OpCode::LocalAddr, static_cast<std::int32_t>(m_returnOffset));
        function.emit(OpCode::Load, memTypeOf(info.ret));
        function.emit(OpCode::Ret);
    }

    function.frameSize = alignUp(m_frameSize, 8);
    m_fn = nullptr;
    m_returnType = nullptr;
}

void IRGen::emitEventCondition(Stmt& decl, int funcIndex) {
    Function& function = m_module->functions[static_cast<std::size_t>(funcIndex)];
    m_fn = &function;
    m_functionIndex = funcIndex;
    m_slots.clear();
    m_loops.clear();
    m_eof.clear();
    m_frameSize = 0;
    m_arenaDepth = 0;
    m_returnType = m_sema.types().boolType();

    function.code.clear();
    function.constants.clear();
    function.params.clear();
    function.returnsAggregate = false;
    function.returnMemType = memTypeOf(m_sema.types().boolType());
    function.returnsValue = true;
    m_returnOffset = allocSlot(4, 4);

    m_eof.emplace_back();
    if (decl.eventCond) {
        emitExpr(*decl.eventCond);
    } else {
        emitConstInt(0);
    }
    m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(m_returnOffset));
    m_fn->emit(OpCode::Swap);
    m_fn->emit(OpCode::Store, function.returnMemType);

    const std::size_t eofLabel = here();
    for (const std::size_t jump : m_eof.back().returnJumps) {
        function.patchJump(jump, eofLabel);
    }
    m_eof.pop_back();

    function.emit(OpCode::LocalAddr, static_cast<std::int32_t>(m_returnOffset));
    function.emit(OpCode::Load, function.returnMemType);
    function.emit(OpCode::Ret);
    function.frameSize = alignUp(m_frameSize, 8);
    m_fn = nullptr;
    m_returnType = nullptr;
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

namespace {

/// `p++` / `++p` as a statement: returns the name expression, or nullptr.
Expr* incrementTarget(const Stmt& stmt) {
    if (stmt.kind != StmtKind::ExprStmt || !stmt.init) {
        return nullptr;
    }
    const Expr& expr = *stmt.init;
    if (expr.kind != ExprKind::PostInc && expr.kind != ExprKind::PreInc) {
        return nullptr;
    }
    if (expr.kids.size() != 1 || expr.kids[0]->kind != ExprKind::Name) {
        return nullptr;
    }
    return expr.kids[0].get();
}

} // namespace

std::size_t IRGen::tryEmitCopyStep(std::vector<StmtPtr>& stmts, std::size_t index) {
    if (index + 2 >= stmts.size()) {
        return 0;
    }

    // 1. `@dst = @src;` as an expression statement (result discarded).
    const Stmt& assignStmt = *stmts[index];
    if (assignStmt.kind != StmtKind::ExprStmt || !assignStmt.init) {
        return 0;
    }
    const Expr& assign = *assignStmt.init;
    if (assign.kind != ExprKind::Assign || assign.assignop != AssignOp::Assign ||
        assign.kids.size() != 2) {
        return 0;
    }
    const Expr& target = *assign.kids[0];
    const Expr& value = *assign.kids[1];
    if (target.kind != ExprKind::Unary || target.unop != UnOp::Deref ||
        value.kind != ExprKind::Unary || value.unop != UnOp::Deref) {
        return 0;
    }
    const Expr& destination = *target.kids[0];
    const Expr& source = *value.kids[0];
    if (destination.kind != ExprKind::Name || source.kind != ExprKind::Name) {
        return 0;
    }

    // Both sides must be plain frame-resident pointers of the same scalar type,
    // otherwise the copy would need a conversion and this is not a raw copy.
    const Type* elementType = target.type;
    if (elementType == nullptr || elementType->isAggregate() || elementType->isVoid() ||
        elementType->sizeBytes < 1 || elementType->sizeBytes > 8) {
        return 0;
    }
    if (value.type == nullptr || !m_sema.types().same(value.type, target.type)) {
        return 0;
    }
    const auto destinationSlot = m_slots.find(destination.sym);
    const auto sourceSlot = m_slots.find(source.sym);
    if (destination.sym == nullptr || source.sym == nullptr ||
        destinationSlot == m_slots.end() || sourceSlot == m_slots.end()) {
        return 0;
    }

    // 2 & 3. One increment on each pointer, in either order.
    Expr* firstIncrement = incrementTarget(*stmts[index + 1]);
    Expr* secondIncrement = incrementTarget(*stmts[index + 2]);
    if (firstIncrement == nullptr || secondIncrement == nullptr) {
        return 0;
    }
    const bool forward = firstIncrement->sym == source.sym &&
                         secondIncrement->sym == destination.sym;
    const bool reversed = firstIncrement->sym == destination.sym &&
                          secondIncrement->sym == source.sym;
    if (!forward && !reversed) {
        return 0;
    }

    const int packedSize = static_cast<int>(elementType->sizeBytes) - 1;
    m_fn->emit(OpCode::CopyStep, static_cast<std::int32_t>(sourceSlot->second),
               static_cast<std::int32_t>(destinationSlot->second) | (packedSize << 28));
    return 3;
}

void IRGen::emitStmtList(std::vector<StmtPtr>& stmts) {
    for (std::size_t i = 0; i < stmts.size(); ++i) {
        const std::size_t consumed = tryEmitCopyStep(stmts, i);
        if (consumed != 0) {
            i += consumed - 1;
            continue;
        }
        emitStmt(*stmts[i]);
    }
}

void IRGen::setCurrentLine(const SourceRange& range) {
    if (m_sources == nullptr || m_fn == nullptr) {
        return;
    }
    if (!range.isValid()) {
        m_fn->currentLine = SourceLine{};
        return;
    }
    // Intern the file name in the module: a `.acvm` must be able to print
    // `main.ax:42` later, when the sources may not be on disk.
    const std::string path(m_sources->path(range.begin.file));
    std::uint16_t fileIndex = 0;
    bool found = false;
    for (std::size_t i = 0; i < m_module->sourceFiles.size(); ++i) {
        if (m_module->sourceFiles[i] == path) {
            fileIndex = static_cast<std::uint16_t>(i);
            found = true;
            break;
        }
    }
    if (!found) {
        if (m_module->sourceFiles.size() >= 65535) {
            m_fn->currentLine = SourceLine{};
            return;
        }
        fileIndex = static_cast<std::uint16_t>(m_module->sourceFiles.size());
        m_module->sourceFiles.push_back(path);
    }
    m_fn->currentLine.file = fileIndex;
    m_fn->currentLine.line = m_sources->lineColumn(range.begin).line;
}

/// Emits an expression whose value nobody wants: in statement position, and in the third
/// clause of a `for`.
///
/// Two things differ from `emitExpr`. An increment collapses into a single `StepLocal`,
/// because the store-and-keep form an expression needs is exactly what makes `i++` cost five
/// instructions; and anything else that still leaves a value gets a `Pop`.
///
/// This used to be the body of `StmtKind::ExprStmt` only, so the third clause of a `for` --
/// discarded by definition, since nothing can read it -- went through `emitExpr` and produced
/// `LoadLocal; ConstInt; Add; StoreLocal keep; Pop`. Every loop in every program paid five
/// instructions for its own counter. Found by dumping the IR of an empty loop and counting:
/// ten instructions per iteration, five of them the increment.
void IRGen::emitDiscardedExpr(Expr& expr) {
    switch (expr.kind) {
    case ExprKind::PostInc:
    case ExprKind::PreInc:
        emitIncrement(expr, true, false, /*keepResult=*/false);
        return;
    case ExprKind::PostDec:
    case ExprKind::PreDec:
        emitIncrement(expr, false, false, /*keepResult=*/false);
        return;
    default:
        break;
    }
    m_discardResult = true;
    emitExpr(expr);
    m_discardResult = false;
    if (expr.type == nullptr || !expr.type->isVoid()) {
        // Assignments and increments already avoid leaving a value behind when the result is
        // discarded; everything else still pushes one.
        switch (expr.kind) {
        case ExprKind::Assign:
        case ExprKind::PostInc:
        case ExprKind::PreInc:
        case ExprKind::PostDec:
        case ExprKind::PreDec:
            break;
        default:
            m_fn->emit(OpCode::Pop);
            break;
        }
    }
}

void IRGen::emitStmt(Stmt& stmt) {
    setCurrentLine(stmt.range);
    switch (stmt.kind) {
    case StmtKind::VarDecl: {
        if (stmt.isComptime || stmt.varType == nullptr) {
            return;
        }
        const Type* type = stmt.varType;
        const std::uint32_t offset =
            allocSlot(std::max<std::uint32_t>(type->sizeBytes, 1), type->alignBytes);
        if (stmt.sym != nullptr) {
            m_slots[stmt.sym] = offset;
        }
        // A scalar initialiser overwrites the whole slot, so zeroing first
        // would be a dead store. Without an initialiser the zeroing is what
        // makes uninitialised reads deterministic (docs/LANGUAGE.md, D8).
        if (stmt.init != nullptr && stmt.init->kind != ExprKind::InitList &&
            !type->isAggregate()) {
            emitExpr(*stmt.init);
            emitConvert(stmt.init->type, type);
            m_fn->emit(OpCode::StoreLocal, static_cast<std::int32_t>(offset), memTypeOf(type));
            return;
        }

        emitMemZero(offset, type->sizeBytes);

        if (!stmt.init) {
            return;
        }
        emitInitialize(*stmt.init, offset, type);
        return;
    }

    case StmtKind::FuncDecl:
    case StmtKind::StructDecl:
    case StmtKind::UnionDecl:
    case StmtKind::EnumDecl:
    case StmtKind::ImportDecl:
    case StmtKind::Pass:
    case StmtKind::Empty:
        return;

    case StmtKind::ExprStmt:
        if (stmt.init) {
            emitDiscardedExpr(*stmt.init);
        }
        return;

    case StmtKind::Block:
        emitStmtList(stmt.body);
        return;

    case StmtKind::If: {
        emitExpr(*stmt.cond);
        const std::size_t toElse = m_fn->emitJump(OpCode::JumpIfFalse);
        if (stmt.thenS) {
            emitStmt(*stmt.thenS);
        }
        if (stmt.elseS) {
            const std::size_t toEnd = m_fn->emitJump(OpCode::Jump);
            m_fn->patchJump(toElse, here());
            emitStmt(*stmt.elseS);
            m_fn->patchJump(toEnd, here());
        } else {
            m_fn->patchJump(toElse, here());
        }
        return;
    }

    case StmtKind::While: {
        const std::size_t loopStart = here();
        emitExpr(*stmt.cond);
        const std::size_t exit = m_fn->emitJump(OpCode::JumpIfFalse);

        LoopContext context;
        context.label = stmt.label;
        context.continueTarget = loopStart;
        context.continueTargetKnown = true;
        context.arenaDepth = m_arenaDepth;
        m_loops.push_back(context);

        if (stmt.thenS) {
            emitStmt(*stmt.thenS);
        }
        m_fn->emit(OpCode::Jump, static_cast<std::int32_t>(loopStart));

        const LoopContext finished = m_loops.back();
        m_loops.pop_back();
        for (const std::size_t jump : finished.breakJumps) {
            m_fn->patchJump(jump, here());
        }
        m_fn->patchJump(exit, here());
        return;
    }

    case StmtKind::For: {
        if (stmt.initS) {
            emitStmt(*stmt.initS);
        }        const std::size_t loopStart = here();
        std::size_t exit = 0;
        bool hasCondition = false;
        if (stmt.cond) {
            emitExpr(*stmt.cond);
            exit = m_fn->emitJump(OpCode::JumpIfFalse);
            hasCondition = true;
        }

        LoopContext context;
        context.label = stmt.label;
        context.arenaDepth = m_arenaDepth;
        m_loops.push_back(context);

        if (stmt.thenS) {
            emitStmt(*stmt.thenS);
        }

        const std::size_t continueLabel = here();
        for (const std::size_t jump : m_loops.back().continueJumps) {
            m_fn->patchJump(jump, continueLabel);
        }
        if (stmt.post) {
            // The third clause of a `for` has no reader -- nothing can observe its value --
            // so it is emitted the same way a bare `i++;` statement is.
            emitDiscardedExpr(*stmt.post);
        }
        m_fn->emit(OpCode::Jump, static_cast<std::int32_t>(loopStart));

        const LoopContext finished = m_loops.back();
        m_loops.pop_back();
        for (const std::size_t jump : finished.breakJumps) {
            m_fn->patchJump(jump, here());
        }
        if (hasCondition) {
            m_fn->patchJump(exit, here());
        }
        return;
    }

    case StmtKind::Loop: {
        const int intType = memTypeOf(m_sema.types().intType());
        const std::uint32_t countSlot = allocRawTemp(4, 4);
        emitExpr(*stmt.cond);
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(countSlot));
        m_fn->emit(OpCode::Swap);
        m_fn->emit(OpCode::Store, intType);

        const std::uint32_t indexSlot = allocRawTemp(4, 4);
        emitConstInt(0);
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
        m_fn->emit(OpCode::Swap);
        m_fn->emit(OpCode::Store, intType);

        const std::size_t loopStart = here();
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
        m_fn->emit(OpCode::Load, intType);
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(countSlot));
        m_fn->emit(OpCode::Load, intType);
        m_fn->emit(OpCode::Lt, intType);
        const std::size_t exit = m_fn->emitJump(OpCode::JumpIfFalse);

        LoopContext context;
        context.label = stmt.label;
        context.arenaDepth = m_arenaDepth;
        m_loops.push_back(context);

        if (stmt.thenS) {
            emitStmt(*stmt.thenS);
        }

        const std::size_t continueLabel = here();
        for (const std::size_t jump : m_loops.back().continueJumps) {
            m_fn->patchJump(jump, continueLabel);
        }
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
        m_fn->emit(OpCode::Dup);
        m_fn->emit(OpCode::Load, intType);
        emitConstInt(1);
        m_fn->emit(OpCode::Add, intType);
        m_fn->emit(OpCode::Store, intType);
        m_fn->emit(OpCode::Jump, static_cast<std::int32_t>(loopStart));

        const LoopContext finished = m_loops.back();
        m_loops.pop_back();
        for (const std::size_t jump : finished.breakJumps) {
            m_fn->patchJump(jump, here());
        }
        m_fn->patchJump(exit, here());
        return;
    }

    case StmtKind::ForIn: {
        const Type* elementType = stmt.sym != nullptr ? stmt.sym->type : m_sema.types().intType();
        const std::uint32_t varSlot =
            allocSlot(std::max<std::uint32_t>(elementType->sizeBytes, 1), elementType->alignBytes);
        if (stmt.sym != nullptr) {
            m_slots[stmt.sym] = varSlot;
        }

        const int int64Type = int64MemType();
        const int ptrType = ptrMemType();
        const int elementMem = memTypeOf(elementType);

        const std::uint32_t indexSlot = allocRawTemp(8, 8);
        const std::uint32_t limitSlot = allocRawTemp(8, 8);
        const std::uint32_t baseSlot = allocRawTemp(8, 8);

        bool usesBase = false;
        std::uint32_t stride = 1;

        if (stmt.cond->kind == ExprKind::Range) {
            emitExpr(*stmt.cond->kids[0]);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
            m_fn->emit(OpCode::Swap);
            m_fn->emit(OpCode::Store, int64Type);
            emitExpr(*stmt.cond->kids[1]);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(limitSlot));
            m_fn->emit(OpCode::Swap);
            m_fn->emit(OpCode::Store, int64Type);
        } else {
            const Type* sourceType = stmt.cond->type;
            if (sourceType->kind == TypeKind::String) {
                emitBaseAddress(*stmt.cond);
                m_fn->emit(OpCode::Load, ptrType);
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(baseSlot));
                m_fn->emit(OpCode::Swap);
                m_fn->emit(OpCode::Store, ptrType);

                emitBaseAddress(*stmt.cond);
                emitConstInt(8);
                m_fn->emit(OpCode::Add, ptrType);
                m_fn->emit(OpCode::Load, int64Type);
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(limitSlot));
                m_fn->emit(OpCode::Swap);
                m_fn->emit(OpCode::Store, int64Type);
                stride = 2;
                usesBase = true;
            } else {
                emitBaseAddress(*stmt.cond);
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(baseSlot));
                m_fn->emit(OpCode::Swap);
                m_fn->emit(OpCode::Store, ptrType);

                emitConstInt(static_cast<Int128>(sourceType->length));
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(limitSlot));
                m_fn->emit(OpCode::Swap);
                m_fn->emit(OpCode::Store, int64Type);
                stride = sourceType->elem->sizeBytes;
                usesBase = true;
            }

            emitConstInt(0);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
            m_fn->emit(OpCode::Swap);
            m_fn->emit(OpCode::Store, int64Type);
        }

        const std::size_t loopStart = here();
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
        m_fn->emit(OpCode::Load, int64Type);
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(limitSlot));
        m_fn->emit(OpCode::Load, int64Type);
        m_fn->emit(OpCode::Lt, int64Type);
        const std::size_t exit = m_fn->emitJump(OpCode::JumpIfFalse);

        if (usesBase) {
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(baseSlot));
            m_fn->emit(OpCode::Load, ptrType);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
            m_fn->emit(OpCode::Load, int64Type);
            if (stride != 1) {
                emitConstInt(static_cast<Int128>(stride));
                m_fn->emit(OpCode::Mul, int64Type);
            }
            m_fn->emit(OpCode::Add, ptrType);
            if (elementType->isAggregate()) {
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(varSlot));
                m_fn->emit(OpCode::MemCopy, static_cast<std::int32_t>(elementType->sizeBytes));
            } else {
                m_fn->emit(OpCode::Load, elementMem);
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(varSlot));
                m_fn->emit(OpCode::Swap);
                m_fn->emit(OpCode::Store, elementMem);
            }
        } else {
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
            m_fn->emit(OpCode::Load, int64Type);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(varSlot));
            m_fn->emit(OpCode::Swap);
            m_fn->emit(OpCode::Store, elementMem);
        }

        LoopContext context;
        context.label = stmt.label;
        context.arenaDepth = m_arenaDepth;
        m_loops.push_back(context);

        if (stmt.thenS) {
            emitStmt(*stmt.thenS);
        }

        const std::size_t continueLabel = here();
        for (const std::size_t jump : m_loops.back().continueJumps) {
            m_fn->patchJump(jump, continueLabel);
        }
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(indexSlot));
        m_fn->emit(OpCode::Dup);
        m_fn->emit(OpCode::Load, int64Type);
        emitConstInt(1);
        m_fn->emit(OpCode::Add, int64Type);
        m_fn->emit(OpCode::Store, int64Type);
        m_fn->emit(OpCode::Jump, static_cast<std::int32_t>(loopStart));

        const LoopContext finished = m_loops.back();
        m_loops.pop_back();
        for (const std::size_t jump : finished.breakJumps) {
            m_fn->patchJump(jump, here());
        }
        m_fn->patchJump(exit, here());
        return;
    }

    case StmtKind::Switch: {
        // Tests first, then the bodies in source order, so that `fallthrough`
        // enters the next body directly instead of re-testing the value.
        const Type* subjectType = stmt.cond->type;
        const int subjectMem = memTypeOf(subjectType);
        const std::uint32_t subject = allocTemp(subjectType);
        emitExpr(*stmt.cond);
        m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(subject));
        m_fn->emit(OpCode::Swap);
        m_fn->emit(OpCode::Store, subjectMem);

        std::vector<std::size_t> bodyJumps;   // parallel to non-default clauses
        std::size_t defaultIndex = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < stmt.cases.size(); ++i) {
            CaseClause& clause = stmt.cases[i];
            if (!clause.value) {
                defaultIndex = i;
                continue;
            }
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(subject));
            m_fn->emit(OpCode::Load, subjectMem);
            emitExpr(*clause.value);
            emitConvert(clause.value->type, subjectType);
            m_fn->emit(OpCode::Eq, subjectMem);
            bodyJumps.push_back(m_fn->emitJump(OpCode::JumpIfTrue));
        }

        const std::size_t toDefault = m_fn->emitJump(OpCode::Jump);

        std::vector<std::size_t> endJumps;
        std::size_t nextBodyJump = 0;
        for (std::size_t i = 0; i < stmt.cases.size(); ++i) {
            CaseClause& clause = stmt.cases[i];
            if (clause.value) {
                m_fn->patchJump(bodyJumps[nextBodyJump++], here());
            } else if (i == defaultIndex) {
                m_fn->patchJump(toDefault, here());
            }
            emitStmtList(clause.body);
            if (!clause.hasFallthrough) {
                endJumps.push_back(m_fn->emitJump(OpCode::Jump));
            }
        }

        if (defaultIndex == static_cast<std::size_t>(-1)) {
            m_fn->patchJump(toDefault, here());
        }
        for (const std::size_t jump : endJumps) {
            m_fn->patchJump(jump, here());
        }
        return;
    }

    case StmtKind::Break:
    case StmtKind::Continue: {
        const bool isBreak = stmt.kind == StmtKind::Break;
        int target = -1;
        if (stmt.label.empty()) {
            target = static_cast<int>(m_loops.size()) - 1;
        } else {
            for (int i = static_cast<int>(m_loops.size()) - 1; i >= 0; --i) {
                if (m_loops[static_cast<std::size_t>(i)].label == stmt.label) {
                    target = i;
                    break;
                }
            }
        }
        if (target < 0) {
            return;
        }
        LoopContext& context = m_loops[static_cast<std::size_t>(target)];
        emitReleaseArenas(m_arenaDepth - context.arenaDepth);
        if (!isBreak && context.continueTargetKnown) {
            m_fn->emit(OpCode::Jump, static_cast<std::int32_t>(context.continueTarget));
            return;
        }
        const std::size_t jump = m_fn->emitJump(OpCode::Jump);
        if (isBreak) {
            context.breakJumps.push_back(jump);
        } else {
            context.continueJumps.push_back(jump);
        }
        return;
    }

    case StmtKind::Return: {
        if (m_returnType == nullptr || !m_returnType->isVoid()) {
            if (stmt.init) {
                if (stmt.init->type->isAggregate()) {
                    emitBaseAddress(*stmt.init);
                    m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(m_returnOffset));
                    m_fn->emit(OpCode::MemCopy,
                               static_cast<std::int32_t>(stmt.init->type->sizeBytes));
                } else {
                    emitExpr(*stmt.init);
                    emitConvert(stmt.init->type, m_returnType);
                    m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(m_returnOffset));
                    m_fn->emit(OpCode::Swap);
                    m_fn->emit(OpCode::Store, m_fn->returnMemType);
                }
            }
        }
        emitReleaseArenas(m_arenaDepth);
        m_eof.back().returnJumps.push_back(m_fn->emitJump(OpCode::Jump));
        return;
    }

    case StmtKind::Next:
        if (m_fn->isProcess) {
            m_fn->emit(OpCode::Jump, static_cast<std::int32_t>(m_processLoop));
        }
        return;

    case StmtKind::ArenaScope:
        m_fn->emit(OpCode::ArenaMark);
        ++m_arenaDepth;
        emitStmtList(stmt.body);
        --m_arenaDepth;
        m_fn->emit(OpCode::ArenaRelease);
        return;

    case StmtKind::Virtual: {
        if (!stmt.hasEof) {
            // No eof of its own: the block still terminates the function and
            // the enclosing cleanup runs (spec §2.2.5).
            emitStmtList(stmt.body);
            m_eof.back().returnJumps.push_back(m_fn->emitJump(OpCode::Jump));
            return;
        }

        m_eof.emplace_back();
        emitStmtList(stmt.body);
        const std::size_t target = here();
        for (const std::size_t jump : m_eof.back().returnJumps) {
            m_fn->patchJump(jump, target);
        }
        emitStmtList(stmt.eofBody);
        m_eof.pop_back();
        m_epilogueJumps.push_back(m_fn->emitJump(OpCode::Jump));
        return;
    }

    case StmtKind::Destructure: {
        const Type* tupleType = stmt.init->type;
        if (tupleType->kind != TypeKind::Tuple) {
            return;
        }
        const std::uint32_t temp = allocTemp(tupleType);
        emitMemZero(temp, tupleType->sizeBytes);
        emitInitialize(*stmt.init, temp, tupleType);

        for (std::size_t i = 0; i < stmt.targetSymbols.size() && i < tupleType->elems.size(); ++i) {
            Symbol* symbol = stmt.targetSymbols[i];
            if (symbol == nullptr) {
                continue;
            }
            const Type* elementType = tupleType->elems[i];
            const std::uint32_t elementOffset = temp + tupleType->fields[i].offset;
            const std::uint32_t slot = slotFor(symbol);
            if (elementType->isAggregate()) {
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(elementOffset));
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(slot));
                m_fn->emit(OpCode::MemCopy, static_cast<std::int32_t>(elementType->sizeBytes));
            } else {
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(elementOffset));
                m_fn->emit(OpCode::Load, memTypeOf(elementType));
                m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(slot));
                m_fn->emit(OpCode::Swap);
                m_fn->emit(OpCode::Store, memTypeOf(elementType));
            }
        }
        return;
    }
    }
}

void IRGen::emitInitialize(Expr& init, std::uint32_t offset, const Type* type) {
    if (type == nullptr || type->isError()) {
        return;
    }

    if (init.kind != ExprKind::InitList) {
        if (init.aggregateZeroFill) {
            // `struct P p[200] = {0}`: the slot was zeroed at declaration, so there is
            // nothing to copy. Emitting a memcpy here would read from address 0.
            return;
        }
        if (type->isAggregate()) {
            emitBaseAddress(init);
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(offset));
            m_fn->emit(OpCode::MemCopy, static_cast<std::int32_t>(type->sizeBytes));
        } else {
            m_fn->emit(OpCode::LocalAddr, static_cast<std::int32_t>(offset));
            emitExpr(init);
            emitConvert(init.type, type);
            m_fn->emit(OpCode::Store, memTypeOf(type));
        }
        return;
    }

    if (type->kind == TypeKind::Array) {
        for (std::size_t i = 0; i < init.kids.size() && i < type->length; ++i) {
            const std::uint32_t elementOffset =
                offset + static_cast<std::uint32_t>(i) * type->elem->sizeBytes;
            emitInitialize(*init.kids[i], elementOffset, type->elem);
        }
        return;
    }

    // A union initialiser carries at most one value (Sema enforces that) and it
    // initialises the first member. Without this branch the initialiser would be
    // silently dropped: the slot is zeroed first, so `{0}` would look correct and
    // `{5}` would quietly become 0.
    if (type->kind == TypeKind::Union) {
        if (!init.kids.empty() && !type->fields.empty()) {
            emitInitialize(*init.kids[0], offset + type->fields[0].offset, type->fields[0].type);
        }
        return;
    }

    if (type->kind == TypeKind::Struct || type->kind == TypeKind::Tuple) {
        for (std::size_t i = 0; i < init.kids.size(); ++i) {
            const Type* memberType = nullptr;
            std::uint32_t memberOffset = offset;
            if (type->kind == TypeKind::Struct) {
                if (i >= type->fields.size()) {
                    break;
                }
                memberType = type->fields[i].type;
                memberOffset = offset + type->fields[i].offset;
            } else {
                if (i >= type->elems.size()) {
                    break;
                }
                memberType = type->elems[i];
                memberOffset = offset + type->fields[i].offset;
            }
            emitInitialize(*init.kids[i], memberOffset, memberType);
        }
    }
}

} // namespace acvm
