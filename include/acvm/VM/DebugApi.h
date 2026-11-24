//===--- DebugApi.h - The VM's debugger ABI -----------------------------===//
#pragma once

//  A debugger has to see every instruction, so it cannot be a library the
//  bytecode calls: the call would have to be emitted into the program. It also
//  cannot live inside the VM, because that would make every build carry a command
//  loop and a disassembler.
//
//  So the VM owns a *slot* and a debugger fills it. This header is the contract
//  between them, and it is deliberately plain C: the debugger is a shared library
//  (`stdlib/adb.c` -> `adb.dll`), and a C++ interface would tie the two to the
//  same compiler and the same STL version.
//
//  ── what a hook may and may not do ────────────────────────────────────────
//
//  The hook runs *inside* the interpreter loop, so it must not call back into the
//  VM to run bytecode. Reading (names, code, positions, frames) is fine and is what
//  the callbacks are for. Anything that needs to resume execution returns control
//  to `step()` instead.
//
//  ── the cost when no debugger is attached ─────────────────────────────────
//
//  One pointer comparison per instruction. `VM::setDebugHooks(nullptr)` is the
//  default, so a normal run pays nothing beyond a predictable branch.

#include <cstdint>

namespace acvm {

/// Returned by `beforeInstruction`: whether the VM should keep going.
enum class DebugAction : std::int32_t {
    Continue = 0,   ///< run the instruction
    Abort = 1,      ///< stop execution as if by a trap the debugger asked for
};

/// One instruction about to execute.
///
/// Flat on purpose: the debugger may total the fields, print them, put them in a
/// ring buffer, but it must not interpret pointers into the VM.
struct DebugStepInfo {
    /// Index into the module's function table.
    std::int32_t functionIndex;
    /// Bytecode offset inside that function.
    std::int32_t ip;
    /// Opcode number (matches `OpCode` in Bytecode.h, passed as an integer so the
    /// debugger does not include the VM's headers).
    std::int32_t opcode;
    /// Operands as encoded, for a debugger that wants to show them.
    std::int32_t operandA;
    std::int32_t operandB;
    /// 1 for the outermost frame, 2 inside a call, ...
    std::int32_t frameDepth;
    /// Instructions retired so far, including this one's predecessors.
    std::int64_t instructionsRetired;
};

/// What the debugger can ask the VM about the program.
///
/// Every callback returns a value the caller owns only until the next call:
/// strings point into the module, which outlives the run.
struct DebugInfo {
    /// Bumped when a field is added at the end. A debugger built against an older
    /// header sees a smaller number and refuses to attach instead of reading past
    /// the end of the struct.
    std::int32_t apiVersion;

    void* host;

    /// Number of functions in the module.
    std::int32_t (*functionCount)(void* host);
    /// Function name, or "" for an out-of-range index.
    const char* (*functionName)(void* host, std::int32_t functionIndex);
    /// True when the function has bytecode (an external library binding has none).
    std::int32_t (*functionHasCode)(void* host, std::int32_t functionIndex);
    /// Instruction count of a function, or 0.
    std::int32_t (*instructionCount)(void* host, std::int32_t functionIndex);

    /// Mnemonic for an opcode number ("LOAD_LOCAL"), or "?".
    const char* (*opcodeName)(void* host, std::int32_t opcode);

    /// Renders instruction `ip` of `functionIndex` the way `acvm disasm` does,
    /// without the leading address. Returns "" when out of range.
    ///
    /// Formatting inside the VM (rather than in the debugger) is what lets the two
    /// listings stay identical: one implementation, one format.
    const char* (*instructionText)(void* host, std::int32_t functionIndex, std::int32_t ip);

    /// Source file of an instruction, or "".
    const char* (*sourceFile)(void* host, std::int32_t functionIndex, std::int32_t ip);
    /// 1-based source line of an instruction, or 0 when unknown.
    std::int32_t (*sourceLine)(void* host, std::int32_t functionIndex, std::int32_t ip);

    /// Name of the module's entry point function, or "".
    const char* (*entryFunctionName)(void* host);
};

/// The hook table a debugger installs.
///
/// `beforeInstruction` is called before each instruction executes. Returning
/// `Abort` stops the program; the debugger is expected to have printed why.
/// `onTrap` is called once when the VM traps, with the message the VM would print.
/// `onFinish` is called once when execution ends normally.
struct DebugHooks {
    std::int32_t apiVersion;
    void* user;

    /// Called once, after the module is loaded and before the first instruction.
    ///
    /// This is the moment the program becomes known: names, code and positions
    /// all exist, so a breakpoint can be resolved and a "does that function
    /// exist" question can be answered. Hooks installed before the run have no
    /// `DebugInfo` to read -- the module has not been attached yet -- which is
    /// exactly why the table is handed over here instead of at install time.
    void (*onStart)(void* user, const DebugInfo* info);

    DebugAction (*beforeInstruction)(void* user, const DebugStepInfo* info);
    void (*onTrap)(void* user, const char* message);
    void (*onFinish)(void* user);
};

inline constexpr std::int32_t kDebugApiVersion = 1;

} // namespace acvm
