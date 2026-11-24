//===--- Jit.h - compiling bytecode to machine code ----------------------===//
#pragma once

#include "acvm/IR/Bytecode.h"
#include "acvm/JIT/ExecutableMemory.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace acvm {

class VM;

/// What the VM should do with compiled code.
/// Differential validation is not a mode here. The same program is run twice -- once
/// with the JIT off, once with it on -- and the two outputs are compared byte for byte,
/// which is the mechanism the optimizer's own O0-vs-O3 check already uses. Comparing
/// inside the VM would only cover the call sites it could rewind; running the program
/// twice covers everything, including state that leaks between calls.
enum class JitMode {
    Off,   ///< never compile; the interpreter is the only implementation
    On,    ///< compile a function when it qualifies, and run the result
};

/// The calling convention shared by the VM and generated code.
///
/// Four arguments, all in registers on the x64 ABI, and one boolean result: `false` means
/// a trap was raised and `*error` is set. The frame is already established by the caller
/// -- generated code takes over after `Call` has pushed a Frame, and returns with that
/// frame popped, exactly as the interpreter's `Ret` would. Generated code therefore owns
/// its own return, which keeps the interpreter out of the function body entirely.
///
/// `frameBase` is passed by value rather than re-read from the frame: it is invariant for
/// the whole function, and reading it would need another helper call.
/// `process` is opaque here on purpose: `VM::Process` is a private nested type and the
/// code generator has no business reading it. Everything it needs from the process goes
/// through the bridge helpers below.
using JitFunction = bool (*)(VM* vm, void* process, std::uint64_t frameBase,
                             std::string* error);

/// Per-module cache of compiled functions.
///
/// One entry per function index, and `failed` is sticky: a function that could not be
/// compiled is not retried, because the reason (an unsupported opcode) does not change.
/// The blocks are owned here, so a function's code lives exactly as long as the cache,
/// which is as long as the VM that uses it.
class JitCache {
public:
    /// Returns the native entry for `functionIndex`, compiling it on first use. Returns
    /// null when the function cannot be compiled, in which case `whyNot` says why on the
    /// first refusal and is left alone afterwards.
    JitFunction entryFor(const Module& module, std::size_t functionIndex,
                         std::string& whyNot);

    [[nodiscard]] std::size_t compiledCount() const { return m_compiled; }
    [[nodiscard]] std::size_t refusedCount() const { return m_refused; }

    JitMode mode = JitMode::Off;

private:
    struct Entry {
        JitFunction code = nullptr;
        bool refused = false;
    };
    std::vector<Entry> m_entries;
    std::vector<std::unique_ptr<jit::ExecutableBlock>> m_blocks;
    std::size_t m_compiled = 0;
    std::size_t m_refused = 0;
};

/// Lowers one function to machine code. Returns null and fills `whyNot` when the function
/// uses anything this level does not implement -- the caller then leaves it interpreted.
///
/// All or nothing per function is deliberate: a half-compiled function would need every
/// register value to be flushed back to memory at the point of the unsupported
/// instruction and re-read afterwards, which is where a JIT stops being simple and starts
/// producing wrong answers at the edges.
std::unique_ptr<jit::ExecutableBlock> compileFunction(const Module& module,
                                                     const Function& function,
                                                     std::string& whyNot);

namespace jitbridge {

/// Generated code's entire view of the interpreter. Three helpers, deliberately: every
/// one of them is a place where a semantic mistake would be silent, so the surface is
/// kept small enough to review in one sitting.
///
/// Slot loads and stores go through the VM's `loadScalar` / `storeScalar` rather than
/// addressing the page table directly. That is slower than it needs to be and is the
/// first thing to replace, but it guarantees the compiled path applies the same width,
/// sign extension and range rules as the interpreted one -- which is the property the
/// differential test exists to check.
bool loadSlot(VM& vm, std::uint64_t address, int typeIndex, std::int64_t& out);
bool storeSlot(VM& vm, std::uint64_t address, int typeIndex, std::int64_t value);

/// Read and write through an address the program computed. Separate from the slot
/// helpers because these carry the null check the interpreter's LOAD and STORE perform:
/// a slot address is always valid by construction, a program's pointer is not.
bool loadThroughPointer(VM& vm, std::uint64_t address, int typeIndex,
                        std::int64_t& out);
bool storeThroughPointer(VM& vm, std::uint64_t address, int typeIndex,
                         std::int64_t value);

/// Pops the frame and pushes the result, exactly as the interpreter's `Ret` does.
void returnToCaller(void* process, bool hasResult, std::int64_t result);

} // namespace jitbridge

} // namespace acvm
