//===--- IRGen.h - AST to bytecode lowering -----------------------------===//
#pragma once

#include "acvm/AST/AST.h"
#include "acvm/Common/SourceManager.h"
#include "acvm/IR/Bytecode.h"
#include "acvm/Sema/Sema.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace acvm {

class DiagnosticEngine;

/// Lowers a type checked AST into a Module of bytecode functions.
///
/// Frame layout of every function:
///
///   [0]        hidden destination pointer   (only when returning an aggregate)
///   [next]     return value storage
///   ...        parameters, in declaration order
///   ...        locals and expression temporaries
///
/// The whole frame is zeroed on entry so that padding bytes inside aggregates
/// are deterministic; that makes byte-wise equality equivalent to member-wise
/// equality (docs/LANGUAGE.md, D8).
class IRGen {
public:
    IRGen(DiagnosticEngine& diagnostics, Sema& sema);

    bool generate(Unit& unit, Module& module);

    void setAssertionsEnabled(bool enabled) { m_assertions = enabled; }

    /// Source of file names and line numbers for the line tables. Optional: a
    /// caller that only wants bytecode (a test, an embedder) leaves it null and
    /// every instruction records an unknown position.
    void setSourceManager(const SourceManager* sources) { m_sources = sources; }

private:
    struct LoopContext {
        std::string label;
        std::size_t continueTarget = 0;
        bool continueTargetKnown = false;
        std::vector<std::size_t> continueJumps;
        std::vector<std::size_t> breakJumps;
        int arenaDepth = 0;
    };

    struct EofContext {
        std::vector<std::size_t> returnJumps;
    };

    void emitFunctionBody(Stmt& decl, int funcIndex);
    void emitGlobalInit(Unit& unit);
    void emitEventCondition(Stmt& decl, int funcIndex);

    const SourceManager* m_sources = nullptr;

    /// Records `range` as the position of everything emitted until the next
    /// statement. Called at the top of `emitStmt`, which is the only place that
    /// knows where a statement starts -- expressions inherit the position of the
    /// statement that contains them, which is what a breakpoint means.
    void setCurrentLine(const SourceRange& range);

    void emitStmt(Stmt& stmt);
    void emitStmtList(std::vector<StmtPtr>& stmts);

    /// Recognises `@dst = @src; src++; dst++;` (the two increments may come in
    /// either order) and emits a single COPY_STEP instead. Returns how many
    /// statements were consumed, 0 when the pattern does not apply.
    std::size_t tryEmitCopyStep(std::vector<StmtPtr>& stmts, std::size_t index);
    void emitExpr(Expr& expr);
    void emitAddress(Expr& expr);
    void emitAssign(Expr& expr);
    void emitIncrement(Expr& expr, bool isIncrement, bool isPrefix, bool keepResult = true);
    void emitConvert(const Type* from, const Type* to);

    void emitConstInt(Int128 value);
    void emitConstFloat(double value);
    void emitMemZero(uint32_t offset, uint32_t size);

    [[nodiscard]] int memTypeOf(const Type* type);
    [[nodiscard]] uint32_t allocSlot(uint32_t size, uint32_t align);
    [[nodiscard]] uint32_t slotFor(Symbol* symbol);
    [[nodiscard]] uint32_t allocTemp(const Type* type);
    [[nodiscard]] uint32_t allocRawTemp(uint32_t size, uint32_t align);

    /// Pushes `base + index * stride` where `base` was already emitted.
    void emitScaledAdd(Expr& index, uint32_t stride);
    void emitBoundsCheck(std::uint32_t indexSlot, bool dynamicLength,
                         std::uint32_t lengthSlotOrValue);
    std::uint32_t stageIndex(Expr& index);
    void emitReleaseArenas(int depth);
    void emitLoadValue(const Type* type);
    void emitBaseAddress(Expr& base);
    void emitInitialize(Expr& init, uint32_t offset, const Type* type);
    [[nodiscard]] int ptrMemType();

    /// True when `expr` names a plain scalar variable that can be read with a
    /// single fused instruction. Sets `code`/`slot` on success.
    [[nodiscard]] bool fusedVariable(const Expr& expr, OpCode& code, std::int32_t& slot,
                                     int& memType) const;
    [[nodiscard]] int int64MemType();

    [[nodiscard]] Function& fn() { return *m_fn; }
    [[nodiscard]] std::size_t here() const { return m_fn->code.size(); }

    DiagnosticEngine& m_diagnostics;
    Sema& m_sema;
    Module* m_module = nullptr;
    Function* m_fn = nullptr;

    std::unordered_map<const Symbol*, uint32_t> m_slots;
    std::vector<LoopContext> m_loops;
    std::vector<EofContext> m_eof;
    std::vector<std::size_t> m_epilogueJumps;

    uint32_t m_frameSize = 0;
    uint32_t m_returnOffset = 0;
    uint32_t m_destinationOffset = 0;
    std::size_t m_processLoop = 0;
    const Type* m_returnType = nullptr;
    bool m_assertions = true;
    /// True while generating an expression whose value nobody consumes (an
    /// expression statement). Assignments then store without leaving the value
    /// behind, which removes a POP per assignment.
    bool m_discardResult = false;
    int m_arenaDepth = 0;
    int m_functionIndex = -1;
};

} // namespace acvm
