//===--- VM.h - ACVM bytecode interpreter -------------------------------===//
#pragma once

#include "acvm/IR/Bytecode.h"
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

    /// Per-opcode retired counts, indexed by OpCode. The hot loop keeps this
    /// up to date unconditionally: it is one increment into a 384 byte table
    /// that stays in cache, which measured below the noise floor.
    [[nodiscard]] const std::uint64_t* opcodeHistogram() const { return m_opcodeCounts; }

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
    };

    enum class Step { Continue, SafePoint, Finished, Trap };

    // -- memory -----------------------------------------------------------
    bool readBytes(std::uint64_t addr, void* dst, std::size_t len) const;
    bool writeBytes(std::uint64_t addr, const void* src, std::size_t len);
    bool zeroBytes(std::uint64_t addr, std::size_t len);
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

    std::vector<bool> m_eventWasTrue;
    bool m_insideEvent = false;
    bool m_assertions = true;
    std::uint64_t m_budget = 0;
    std::uint64_t m_instructions = 0;
    std::uint64_t m_opcodeCounts[64] = {};
};

} // namespace acvm
