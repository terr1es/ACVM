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

/// True when this host can execute the code the lowering emits.
///
/// The backend targets x86-64 and nothing checks the CPU at run time -- there is no
/// CPUID probe, because a 64 bit x86 CPU missing the handful of instructions the
/// lowering uses does not exist. Everywhere else the *encoder* would still produce
/// bytes happily (it is portable C++), the block would be allocated executable, and the
/// process would immediately run something that is not its own program. The guard
/// therefore has to sit in front of `compileFunction`, not in the assembler.
///
/// Refusing to compile is what keeps the interpreter the whole implementation on those
/// hosts. The CLI turns an explicit `--jit` there into an error rather than a silent
/// fallback, because asking for the JIT and silently getting the interpreter reads as
/// "the JIT made no difference".
/// A second constraint sits on top of the architecture, and it is the one that is easy to
/// miss: the generated code uses the **Microsoft x64** convention -- arguments in
/// RCX, RDX, R8, R9, 32 bytes of shadow space reserved by every caller, and a return value
/// in RAX. System V, which is what Linux and macOS x86-64 use, passes arguments in RDI,
/// RSI, RDX, RCX, R8, R9 and reserves no shadow space.
///
/// So an x86-64 Linux or macOS host is *not* a target, even though the encoder would
/// produce bytes for it. Those bytes would call the VM's C++ entry points with the
/// arguments in registers the callee never reads, and the callee would return whatever
/// happened to be in RAX. That is worse than a refusal: the first differential test to
/// run found it, but only because `func_check_jitdiff` compares output.
///
/// `kTargetIsX64` was the old name and it was the reason this went unnoticed -- the check
/// answered a narrower question than the code required.
#if (defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)) && defined(_WIN32)
inline constexpr bool kJitSupported = true;
#else
inline constexpr bool kJitSupported = false;
#endif

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
/// Returns 0 for a trap, 1 for a normal finish, 2 for "yielded at a safe point". An int
/// rather than a bool because the third outcome exists and has to survive a nested call.
using JitFunction = std::int64_t (*)(VM* vm, void* process, std::uint64_t frameBase,
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
/// `resultIsPtr` matters: a string and a pointer share the same 64 bit payload but not the
/// same tag, and printing a string that arrived as an integer prints nothing. The tag is
/// known at compile time from the function's return type, so it is carried here rather than
/// guessed.
void returnToCaller(void* process, bool hasResult, std::int64_t result,
                    bool resultIsPtr = false);

/// Host pointers for every memory page `[frameBase, frameBase + frameSize)` touches.
///
/// Pages are never reclaimed or moved once allocated, which is what makes this sound: a
/// compiled function can hold these for its whole lifetime. Returns the count written, or
/// 0 if the frame needs more than `maxPages` (or a page could not be created).
/// Reads eight raw bytes with no type interpretation. Needed for the rare frame slot that
/// straddles a page boundary and holds a pointer: the slot's type index describes what the
/// pointer points at, not the pointer itself, so passing it to `loadSlot` would interpret
/// the address as data.
bool readRaw64(VM& vm, std::uint64_t address, std::int64_t& out);

/// Address of the `{ptr,len}` descriptor for string literal number `index`.
///
/// A literal is not copied or allocated per execution: `VM::prepare` lays every one of
/// them out once and the address is stable for the life of the process. Returning that
/// address is therefore a lookup, and the tag is a pointer -- the same tag the
/// interpreter's `CONST_STR` pushes. A function containing a string literal used to be
/// refused outright, which is what kept most real programs out of the JIT entirely.
bool constantString(VM& vm, int index, std::int64_t& out);

/// Zeroes `size` bytes at a VM address. The null check is the caller's, because generated
/// code has to branch to its own trap exit rather than learn about failure through a
/// return value -- and because MEM_ZERO only traps when there is something to write.
void zeroThroughPointer(VM& vm, std::uint64_t address, std::uint64_t size);

/// Copies `size` bytes from one VM address to another. `MEM_COPY` is how a value type
/// larger than a register moves -- a string is a 16 byte `{ptr,len}` descriptor, so
/// `string s = "x";` is CONST_STR followed by a 16 byte copy into the local's slot.
void copyThroughPointers(VM& vm, std::uint64_t destination, std::uint64_t source,
                         std::uint64_t size);

int pinFramePages(VM& vm, std::uint64_t frameBase, std::uint64_t frameSize,
                  void** out, int maxPages);

/// Performs a full CALL: pushes the arguments onto the interpreter's value stack, sets up
/// the callee's frame, and runs it -- through its compiled entry when it has one, through
/// the interpreter when it does not. Pops the frame on return and leaves any result on the
/// value stack, exactly as the interpreter's CALL does.
bool callFunction(VM& vm, void* process, int functionIndex, int argc,
                  std::string* error);

/// Moves the value stack's top into `out` and pops it. Used after a call returns.
bool popResult(void* process, std::int64_t* out, bool isPtr = false);

/// Performs a CALL from compiled code, reading the arguments out of the caller's native
/// slots instead of pushing them onto the interpreter's value stack first.
///
/// `args` points at `argc` slots of 8 bytes, in the order the callee declares them, and
/// `pointerMask` carries a bit per argument: set means the slot holds an address rather than
/// an integer, which the callee's frame needs to know because it stores a tagged `Value`.
///
/// The result is written to `out` when the callee returns one. That is the same slot the
/// first argument came from -- the caller's expression stack shrinks by `argc` and grows by
/// one, and this way that slot is already in the right place.
///
/// This replaces three round trips per call (`pushValue` for each argument, then
/// `callFunction`, then `popResult`). Those three were measurable: at 53.1M calls, removing
/// the heap allocation inside `callFunction` alone took compiled `fib` from 4.81 s to 2.80 s.
bool callFromSlots(VM& vm, void* process, int functionIndex, int argc,
                   const std::int64_t* args, std::uint32_t pointerMask, std::int64_t* out,
                   std::string* error);

/// Pushes one value onto the interpreter's stack. Compiled code keeps its values in
/// native slots, but CALL reads its arguments from `process.stack`, so this is how the two
/// representations meet.
bool pushValue(void* process, std::int64_t value, bool isPtr = false);

/// A safe point from compiled code. Records where the interpreter should resume and says
/// whether the scheduler has to run now.
///
/// A safe point is where the interpreter hands control back so other processes can run.
/// With one process there is nothing to hand control to, so the compiled path continues --
/// which is why a single-process program never pays for this. With more than one it yields
/// exactly where the interpreter would.
int safePoint(VM& vm, void* process, std::uint64_t nextIp, std::string* error);
void bailToInterpreter(void* process, std::uint64_t ip);

/// Charges `amount` instructions against the runaway-loop budget and reports whether it is
/// exhausted. Called from compiled loop back edges, so an infinite loop still stops with a
/// diagnosable trap rather than hanging.
bool consumeBudget(VM& vm, std::uint64_t amount);

} // namespace jitbridge

} // namespace acvm
