//===--- VM.h - ACVM bytecode interpreter -------------------------------===//
#pragma once

#include "acvm/IR/Bytecode.h"
#include "acvm/VM/Jit.h"
#include "acvm/VM/DebugApi.h"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace acvm {

//===----------------------------------------------------------------------===//
// Dynamic loading
//===----------------------------------------------------------------------===//
//
// Exposed because the debugger is loaded the same way a native library module is,
// and there must be exactly one implementation of "open a .dll on this platform":
// a second copy in the driver would be a second set of #ifdefs to get wrong.

/// Opens a shared library by file name. Returns null on failure.
void* vmOpenLibrary(const std::string& name);
/// Resolves a symbol, or null.
void* vmLookupSymbol(void* handle, const std::string& symbol);
void vmCloseLibrary(void* handle);

/// Handle for the running executable, for looking up symbols in itself.
///
/// A statically linked build has no `io.dll` to open: the standard library is part of the
/// executable and exported from it. Symbol resolution falls back to this handle so that
/// such a build needs no separate code path -- the same `GetProcAddress` finds the same
/// function, one module earlier.
void* vmMainModule();

/// Absolute path of the running executable, or "" when the platform will not say.
///
/// Declared here rather than in a utility header because this is the one file that
/// already includes the platform's loader headers; a second copy elsewhere would be
/// a second set of `#ifdef`s to get wrong. The driver uses it to find the module
/// directory that sits beside an installed `acvm`, so that a copy of the tool outside
/// its build tree still resolves `import` against its own stdlib rather than against
/// whichever tree happened to be present when it was compiled.
std::string vmExecutablePath();

/// The file names a `library "name"` could have on this platform, in the order to try:
/// `name.dll` on Windows, `libname.so` then `name.so` on Linux, `libname.dylib` then
/// `name.dylib` on macOS. A name that already carries one of those extensions comes back
/// unchanged.
///
/// Shared with the driver, which loads `adb` by name for `--adb`. That call site used to
/// spell the names itself -- `adb.dll` twice and `libadb.so` once -- which was wrong on
/// every platform but Windows: the first looked for a `.dll` under a POSIX lib directory,
/// and the second named a file the build does not produce, because the modules set
/// `PREFIX ""` and come out as `adb.so`.
std::vector<std::string> libraryFileNames(const std::string& name);

class VM {
public:
    VM(std::ostream& out, std::ostream& err);
    ~VM();
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    /// Loads the module, runs the global initialiser, then `main` together with
    /// every `process` function. Returns false and fills `error` on a trap.
    bool run(const Module& module, std::string& error);

    void setAssertionsEnabled(bool enabled) { m_assertions = enabled; }

    /// Turns code generation on. `Compare` runs both implementations and traps on any
    /// difference, which is how the lowering is validated.
    void setJitMode(JitMode mode) { m_jit.mode = mode; }
    [[nodiscard]] JitMode jitMode() const { return m_jit.mode; }
    /// Functions that produced machine code, and functions that were refused.
    [[nodiscard]] std::size_t jitCompiledCount() const { return m_jit.compiledCount(); }
    [[nodiscard]] std::size_t jitRefusedCount() const { return m_jit.refusedCount(); }
    /// Why the most recently refused function was refused. Empty when none was.
    [[nodiscard]] const std::string& jitRefusal() const { return m_jitRefusal; }

    /// Command-line arguments the host passes to the program, i.e. everything
    /// after `--`. Held here rather than read from the process because the
    /// bytecode can also be executed by an embedder with no command line.
    void setProgramArgs(std::vector<std::string> args) { m_programArgs = std::move(args); }

    /// Where to look for shared libraries named by `library "x";`, in order.
    /// The module's own directory comes first so a program and its .dll can
    /// travel together; the toolchain's library directory follows, so anything
    /// built with the compiler is found from any working directory. The
    /// platform loader path is always tried last.
    void setLibrarySearchPath(std::vector<std::string> directories) {
        m_libraryPath = std::move(directories);
    }

    /// Lowers the resident-memory cap. Tests use a tiny one so the trap can be
    /// reached in a few pages instead of allocating the full default.
    void setMemoryLimit(std::uint64_t bytes);

    /// Installs or removes the debugger. Pass nullptr to remove.
    ///
    /// The VM copies the struct, so the caller may let it go after the call --
    /// but the `user` pointer and the function pointers must outlive the run.
    void setDebugHooks(const DebugHooks* hooks);

    /// The info table a debugger reads the program through. Filled in by the VM;
    /// valid between `prepare` and the end of the run.
    [[nodiscard]] const DebugInfo& debugInfo() const { return m_debugInfo; }

    /// Builds the info table for the module currently loaded. Called from
    /// `prepare`, so a debugger can read the program *before* it starts --
    /// attaching after the first instruction would make breakpoint resolution
    /// depend on execution order.
    void refreshDebugInfo();

    /// Instructions retired by the last run(). Useful when optimising a
    /// program: on a dispatch loop the count is close to a linear predictor of
    /// wall-clock time, and unlike timing it is perfectly reproducible.
    [[nodiscard]] std::uint64_t instructionCount() const { return m_instructions; }

    /// Per-opcode retired counts, indexed by OpCode.
    ///
    /// Both this and `instructionCount` are only maintained after
    /// `setProfile(true)` -- see `m_countInstructions`. The comment that used to
    /// stand here said the update was "below the noise floor" because it is one
    /// increment into a 384 byte table that stays in cache. That was wrong, and
    /// measurably so: the increment is a read-modify-write to the VM object plus
    /// an indexed store, on the critical path of every instruction, and the
    /// second counter next to it was another. See the benchmark table in
    /// `docs/performance.md`.
    [[nodiscard]] const std::uint64_t* opcodeHistogram() const { return m_opcodeCounts; }

    /// Turns the retired-instruction counters on. Called by `--stats`, and by the
    /// debugger, which reports a retired count with every step.
    void setProfile(bool on) { m_countInstructions = on; }

    // Address space layout (sparse, page backed).
    static constexpr std::uint64_t kGlobalBase = 0x0001'0000;
    static constexpr std::uint64_t kStringBase = 0x0004'0000;
    static constexpr std::uint64_t kStackBase = 0x1000'0000;
    static constexpr std::uint64_t kStackStride = 0x0100'0000;
    static constexpr std::uint64_t kHeapBase = 0x4000'0000;
    static constexpr std::uint64_t kArenaBase = 0x8000'0000;
    static constexpr std::uint64_t kArenaStride = 0x0100'0000;

    /// Resolves a function that lives in a shared library (`library "x";` in
    /// the .ax file). The library is loaded on first use and kept for the
    /// lifetime of the VM; symbols are looked up once and cached. Returns null
    /// and fills `error` when the library or symbol cannot be found.
    /// Resolves (and caches) the native symbol for `functionIndex`. Caching
    /// matters: `GetProcAddress` / `dlsym` costs microseconds, which dwarfed the
    /// actual work of a small library call.
    [[nodiscard]] void* resolveExternal(const Function& function, std::size_t functionIndex,
                                        std::string& error);


    /// Recursion limit. Well past what a sensible program needs (quicksort
    /// recurses O(log n)) and far below anything that could exhaust memory.
    static constexpr std::size_t kMaxCallDepth = 10'000;

    /// Pages are handed out on first touch, so a program that walks a pointer
    /// across the address space would keep asking for 64 KB blocks until the
    /// machine swaps to death. Cap the total; the interpreter traps once this
    /// is crossed instead of taking the operating system down.
    static constexpr std::uint64_t kMaxMemoryBytes = 256ull * 1024 * 1024;

    [[nodiscard]] bool memoryExhausted() const;

    /// Bridge for generated code: the entire surface the JIT is allowed to use. It is
    /// public because `Process` is a private nested type -- living here is what lets
    /// these three functions name it without making the whole process state public.
    ///
    /// Each one mirrors an interpreter path exactly. If they ever disagree, the
    /// differential test reports it as a difference in program output, which is the
    /// point of having it.
    struct JitBridge {
        static bool loadSlot(VM& vm, std::uint64_t address, int typeIndex,
                             std::int64_t& out);
        static bool storeSlot(VM& vm, std::uint64_t address, int typeIndex,
                              std::int64_t value);
        static bool loadThroughPointer(VM& vm, std::uint64_t address, int typeIndex,
                                       std::int64_t& out);
        static bool storeThroughPointer(VM& vm, std::uint64_t address, int typeIndex,
                                        std::int64_t value);
        static void returnToCaller(void* process, bool hasResult, std::int64_t result,
                                   bool resultIsPtr = false);
        static bool readRaw64(VM& vm, std::uint64_t address, std::int64_t& out);
        static bool constantString(VM& vm, int index, std::int64_t& out);
        static void zeroThroughPointer(VM& vm, std::uint64_t address, std::uint64_t size);
        static void copyThroughPointers(VM& vm, std::uint64_t destination,
                                        std::uint64_t source, std::uint64_t size);
        static int pinFramePages(VM& vm, std::uint64_t frameBase, std::uint64_t frameSize,
                                 void** out, int maxPages);
        static bool callFunction(VM& vm, void* process, int functionIndex, int argc,
                                 std::string* error);
        static bool popResult(void* process, std::int64_t* out, bool isPtr = false);
        static bool pushValue(void* process, std::int64_t value, bool isPtr = false);
        /// Runs the event phase of a safe point. Returns 0 to continue, 1 to yield
        /// (let the scheduler run another process), 2 when an event handler trapped --
        /// in which case `*error` is set and the compiled function must return a trap.
        ///
        /// Events are evaluated here rather than only at a yield: the interpreter runs
        /// them at every safe point, and a compiled function that skipped them because
        /// it had nothing to switch to would fire an event fewer times than the same
        /// program interpreted. Only the *yield* needs the expression stack empty, which
        /// is why the caller decides what to do with 1 and this always does the work.
        static int safePoint(VM& vm, void* process, std::uint64_t nextIp,
                             std::string* error);
        /// Leaves `ip` for the interpreter to resume at, and returns. Used when the
        /// compiled body reaches an instruction only the interpreter can execute: the
        /// frame and its slots stay exactly as they are, so the interpreter continues
        /// the same function from that instruction with nothing left to hand over.
        static void bailToInterpreter(void* process, std::uint64_t ip);
        static bool consumeBudget(VM& vm, std::uint64_t amount);
    };

private:
    struct Frame {
        const Function* function = nullptr;
        std::size_t ip = 0;
        std::uint64_t base = 0;
    };

    struct Process {
        std::vector<Frame> frames;
        std::vector<Value> stack;
        std::uint64_t stackTop = 0;
        std::uint64_t stackBase = 0;
        std::uint64_t arenaBase = 0;
        std::uint64_t arenaOffset = 0;
        std::vector<std::uint64_t> arenaMarks;
        bool finished = false;
        bool isMain = false;
        /// Set once a compiled entry point has been tried for this process.
        ///
        /// A compiled function cannot be resumed in the middle -- it owns its own
        /// return -- so a process that yields out of one continues interpreted from
        /// then on. Without this flag the next quantum would re-enter the compiled
        /// entry from the top and re-run everything the process had already done.
        bool jitEntrySpent = false;
    };

    enum class Step { Continue, SafePoint, Finished, Trap };

    // -- memory -----------------------------------------------------------
    bool readBytes(std::uint64_t addr, void* dst, std::size_t len) const;
    bool writeBytes(std::uint64_t addr, const void* src, std::size_t len);
    bool zeroBytes(std::uint64_t addr, std::size_t len);

    /// A direct pointer to a 4-byte slot, or nullptr when the general path is required.
    ///
    /// The scalar path a hot opcode takes is `loadScalar → memType (bounds check) → readBytes
    /// (page lookup, memcpy, mask, sign-extend) → Value (32 bytes)`, and the store side is the
    /// mirror image. That is 27–30 cycles to add one to a loop counter, measured; a plain
    /// `int32` in a resident page needs an add and a store.
    ///
    /// Null means "do not take the shortcut": the page has never been written (it reads as
    /// zeros rather than existing), or the slot straddles a page boundary. Both are rare and
    /// both fall back to the general path, so the shortcut is an optimisation and never the
    /// definition of the behaviour.
    ///
    /// Deliberately narrow — 4 bytes, signed integer, resident page. A caller must have already
    /// checked that its operand is that shape; this only answers "may I write here directly".
    [[nodiscard]] std::int32_t* residentInt32(std::uint64_t addr);
    [[nodiscard]] const std::int32_t* residentInt32(std::uint64_t addr) const;

    /// The last page `residentInt32` resolved, and a way to make it forget.
    ///
    /// A frame is one allocation of at most a few hundred bytes, so every local access in a
    /// function hits the same 64 KB page. `PageStore::get` was a modulo, an index compare and
    /// a load for each of them; one compare against the page index gets the same answer.
    ///
    /// The cache has to be dropped whenever the page table can change -- a new page allocated,
    /// memory reset, or a different frame -- so `forgetCachedPage()` is called from the
    /// interpreter's frame snapshot rather than being woven into the accessor.
    mutable std::uint64_t m_cachedPageIndex = ~std::uint64_t{0};
    mutable std::int32_t* m_cachedPage = nullptr;
    void forgetCachedPage() const {
        m_cachedPageIndex = ~std::uint64_t{0};
        m_cachedPage = nullptr;
    }
    /// Copies within VM memory, memmove-style, without an intermediate buffer.
    bool copyBytes(std::uint64_t dst, std::uint64_t src, std::size_t len);
    /// Compares two ranges of VM memory without copying either out.
    bool equalBytes(std::uint64_t lhs, std::uint64_t rhs, std::size_t len) const;
    [[nodiscard]] bool loadScalar(std::uint64_t addr, int typeIndex, Value& out) const;
    bool storeScalar(std::uint64_t addr, int typeIndex, const Value& value);
    [[nodiscard]] const MemType& memType(int index) const;

    std::uint64_t heapAlloc(std::uint64_t size, bool zeroed);
    void heapFree(std::uint64_t addr);
    std::uint64_t arenaAlloc(Process& process, std::uint64_t size, std::uint64_t align);

    // -- strings ----------------------------------------------------------
    [[nodiscard]] std::string readString(Value value) const;

    /// Lexicographic comparison of two strings, negative / zero / positive.
    ///
    /// `readString` copies the content into a `std::string`, so comparing with it
    /// allocates twice per comparison -- every `s == t` in a loop paid for two heap
    /// allocations plus two memcpys. Sorting 20 000 strings took 79 ms that way,
    /// almost all of it in those allocations.
    ///
    /// This walks the bytes in chunks instead: no allocation, one `readBytes` per
    /// chunk, and the chunk size is picked to cover a whole short string in a single
    /// call. Ordering is by unsigned byte value, which is what `std::string` gives
    /// and what sorts UTF-8 correctly.
    [[nodiscard]] int compareStrings(Value lhs, Value rhs) const;

    /// True when the two strings have identical contents. Same reasoning as above;
    /// exists separately so the common predicate does not have to interpret a
    /// three-way result.
    [[nodiscard]] bool equalStrings(Value lhs, Value rhs) const;
    std::uint64_t makeStringInArena(Process& process, const std::string& text);
    std::uint64_t makeStringOnHeap(const std::string& text);

    // -- execution --------------------------------------------------------
    /// Result of trying to call into a shared library.
    enum class ExternalOutcome {
        Called,        ///< the native call ran; its result is on the stack
        Unavailable,   ///< the library or symbol is missing; the stack is untouched
        Failed,        ///< a real error; a trap has been raised
    };

    /// Marshals the arguments on the stack, performs one native call, and pushes
    /// the result. `argc` is the Axiom argument count from the CALL instruction.
    ///
    /// Returns Unavailable *without consuming the arguments* when the library
    /// cannot be resolved, so the caller can fall back to the Axiom body of the
    /// same function (the .ax half of a library module).
    template <typename FailFn>
    ExternalOutcome callExternal(Process& process, const Function& function,
                                 std::size_t functionIndex, std::size_t argc,
                                 std::string& error, FailFn&& fail);
    bool prepare(const Module& module, std::string& error);
    bool startProcess(Process& process, const Function* entry, std::uint64_t stackBase,
                      std::uint64_t arenaBase);
    Step step(Process& process, std::string& error);
    bool runQuantum(Process& process, std::string& error);
    bool runFunctionToCompletion(int functionIndex, std::string& error);
    bool evaluateEvents(Process& current, std::string& error);

    bool executeBuiltin(Process& process, const Instruction& inst, std::string& error, Step& step);


    bool trap(std::string& error, TrapKind kind, const std::string& detail);

    const Module* m_module = nullptr;

    /// True when a `SafePoint` has somewhere to go.
    ///
    /// A safe point exists for three reasons: the scheduler may have another process to run,
    /// an `event` condition may have become true, or a debugger may want to look at the
    /// program. When none of those is the case the instruction has no observer, and
    /// returning to the scheduler costs a full teardown and restart of the interpreter --
    /// snapshot the frame, write `ip` back, unwind to `runQuantum`, evaluate events that do
    /// not exist, and come back in.
    ///
    /// Measured on a call-heavy benchmark, where a `SafePoint` is emitted before every
    /// `Call`: 13.2 s against CPython's 2.3 s. The same benchmark's loop body, which has no
    /// `SafePoint` in it, was already within 12%. The whole difference was this exit.
    ///
    /// Deliberately computed rather than cached in a flag: the three answers change at
    /// different moments (a process is spawned, a debugger attaches, a JIT frame floor is
    /// raised) and a flag that was not updated at one of them is a bug that shows up as a
    /// missing event rather than as a crash. These are a handful of loads from one cache
    /// line, against a hundred cycles of leaving and re-entering.
    [[nodiscard]] bool safePointsMatter() const {
        return m_debugAttached || m_frameFloor != 0 || m_processes.size() > 1 ||
               (m_module != nullptr && !m_module->events.empty());
    }


    /// Debugger attachment. `m_debugHooks` is null unless someone installed one,
    /// and the step loop tests that pointer once per instruction.
    DebugHooks m_debugHooks{};
    bool m_debugAttached = false;
    bool m_debugAborted = false;
    DebugInfo m_debugInfo{};
    /// Set while the VM formats text for a callback, so the returned `const char*`
    /// stays alive until the next callback. A single buffer is enough because the
    /// debugger consumes each string before asking for the next.
    std::string m_debugScratch;

    std::ostream& m_out;
    std::ostream& m_err;

    struct PageStore;
    PageStore* m_memory = nullptr;
    std::vector<std::uint64_t> m_stringAddresses;
    /// `argv` for the running program. Owned here rather than read from the
    /// process because the bytecode may be executed by an embedder that has no
    /// command line at all.
    std::vector<std::string> m_programArgs;

    struct HeapBlock {
        std::uint64_t addr = 0;
        std::uint64_t size = 0;
        bool free = false;
    };
    std::vector<HeapBlock> m_heapBlocks;
    std::uint64_t m_heapTop = kHeapBase;

    std::vector<Process> m_processes;
    Process m_scratch;
    /// Staging buffer for native helpers: the VM copies a range in, the C
    /// function works on contiguous memory, the result is copied back.
    std::vector<std::uint8_t> m_nativeBuffer;

    /// Shared library handles, keyed by Module::externalLibraries index, plus
    /// the resolved symbol for each external function. Both are opaque pointers
    /// so the header stays free of platform loader types.
    std::vector<std::string> m_libraryPath;
    std::vector<void*> m_libraryHandles;
    /// Resolved symbol per function index. Was declared and resized but never
    /// read or written; the lookup was redone on every call instead.
    std::vector<void*> m_externalSymbols;
    /// 1 when the entry above holds a resolved symbol.
    std::vector<char> m_externalResolved;

    /// Compiled code, per module. Kept here rather than in the module so the cache
    /// dies with the VM, which is the lifetime the executable blocks need.
    JitCache m_jit;
    /// Reason the last function refused to compile, for `--jit=compare` diagnostics.
    std::string m_jitRefusal;

    std::vector<bool> m_eventWasTrue;
    bool m_insideEvent = false;
    bool m_assertions = true;
    std::uint64_t m_budget = 0;
    /// `step()` returns as soon as the frame stack drops below this depth. Zero normally,
    /// which nothing can fall below; raised by the JIT while it runs an interpreted callee,
    /// so that the run stops when *that* frame returns instead of carrying on into the
    /// caller's bytecode -- which is compiled, so interpreting it would execute it twice.
    std::size_t m_frameFloor = 0;
    std::uint64_t m_instructions = 0;
    std::uint64_t m_opcodeCounts[64] = {};

    /// Whether the two counters above are maintained.
    ///
    /// They exist for `--stats` and for the debugger's retired-instruction count, and both
    /// are optional -- yet maintaining them unconditionally costs a load, an add and a store
    /// to two members per instruction, plus an indexed access for the histogram. On a loop
    /// body of eleven instructions that was measurable, and the work is pure overhead unless
    /// someone asked for the numbers.
    ///
    /// Set once before the program runs, so the test is a predictable branch on a value that
    /// does not change during a run.
    bool m_countInstructions = false;
};

} // namespace acvm
