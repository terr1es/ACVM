//===--- Bytecode.h - ACVM instruction set and module layout -----------===//
#pragma once

#include "acvm/Common/Value.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace acvm {

/// Description of a scalar value as it is stored in memory. Load / Store and
/// every arithmetic instruction carry one of these so the VM knows how many
/// bytes to touch and whether to sign extend.
struct MemType {
    enum class Kind : std::uint8_t { Int, Float, Ptr, Bool, Char };

    Kind kind = Kind::Int;
    std::uint8_t size = 4;
    std::uint8_t bits = 32;
    bool isUnsigned = false;

    [[nodiscard]] bool isFloat() const { return kind == Kind::Float; }
    [[nodiscard]] bool isPtr() const { return kind == Kind::Ptr; }
    [[nodiscard]] bool isInteger() const {
        return kind == Kind::Int || kind == Kind::Bool || kind == Kind::Char;
    }
};

enum class OpCode : std::uint8_t {
    ConstInt,     // a = constant index
    ConstFloat,   // a = constant index
    ConstStr,     // a = string index, pushes the address of {ptr,len}
    LocalAddr,    // a = frame offset
    GlobalAddr,   // a = global index
    Load,         // a = memType; pop addr, push value
    Store,        // a = memType; pop value, pop addr
    StoreKeep,    // a = memType; like Store but pushes the stored value
    Dup,
    Swap,
    Pop,

    Add, Sub, Mul, Div, Mod, Neg, BitNot,
    Not,
    And, Or, Xor, Shl, Shr,      // a = memType
    Eq, Ne, Lt, Le, Gt, Ge,      // a = memType (operand kind)
    StrEq,                       // pop b, pop a (string addrs), push bool
    StrCmp,                      // pop b, pop a (string addrs), push -1 / 0 / 1
    MemEq,                       // a = byte count; pop b, pop a, push bool

    Cast,                        // a = from memType, b = to memType
    Jump,                        // a = target
    JumpIfFalse,                 // a = target
    JumpIfTrue,                  // a = target
    Call,                        // a = function index, b = argument count
    /// A call whose target is a run-time value: `b` = argument count, and the callee
    /// handle is popped from the stack after the arguments. The handle is the function
    /// index plus one, so 0 -- which a zeroed slot holds -- means "not a function" and
    /// traps, instead of silently calling the first function in the module.
    CallIndirect,
    Ret,
    MemCopy,                     // a = byte count; pop dst, pop src
    MemZero,                     // a = byte count; pop dst
    Builtin,                     // a = Builtin id, b = memType or -1 for string
    Concat,                      // a = memType of rhs, or -1 for string
    ArenaMark,                   // push current arena offset
    ArenaRelease,                // pop offset and roll the arena back
    SafePoint,
    Trap,                        // a = TrapKind
    Halt,

    // Fused variable access. `a` is the frame offset or global index, `b` is
    // the memory type index with an optional keep bit (0x100) meaning "leave
    // the stored value on the stack", i.e. the value of an assignment.
    //
    // Without these, every variable read is two instructions (push the
    // address, dereference it) and they account for over half of the
    // instructions a typical program executes.
    LoadLocal,
    StoreLocal,
    LoadGlobal,
    StoreGlobal,

    /// Read through the pointer held in a frame slot (a = offset, b = type).
    /// `@p` is the most common operation in pointer-walking code, and the
    /// generic form spends two instructions on it.
    DerefLocal,

    /// `local += immediate` in one step (a = offset, b = type | (imm << 8)).
    /// Used by `x++`, `x += n` and above all by pointer stepping, which is
    /// four instructions in the generic form.
    StepLocal,

    /// `*dst++ = *src++` in one instruction. Both pointers live in frame
    /// slots: `a` is the source slot, `b` is the destination slot with the
    /// element size (1..8) packed into the top nibble.
    ///
    /// This idiom is the inner loop of every merge, copy and filter: in the
    /// generic encoding it costs five instructions (load dst, load *src,
    /// store, step src, step dst). The compiler only emits it when it has
    /// matched the three-statement sequence, so nothing else changes.
    CopyStep,

    /// Integer exponentiation: pop the exponent, pop the base, push `base ** exp`.
    /// `a` is the memType of both operands, exactly like ADD.
    ///
    /// Appended at the end rather than next to ADD/MUL because the enum's values are
    /// what a serialized module stores: inserting in the middle would silently
    /// reinterpret every opcode after it in an already compiled `.acvm` file.
    ///
    /// Traps on a negative exponent (no integer result) and on overflow, so the
    /// exponentiation does not need a wrapping variant.
    Pow,
};

enum class TrapKind : std::int32_t {
    Explicit = 0,       // trap()
    IntegerOverflow,
    DivisionByZero,
    CastOutOfRange,
    IndexOutOfBounds,
    NullPointer,
    InvalidPointer,
    AllocationFailed,
    AssertionFailed,
    Unreachable,
    /// A function from a shared library could not be resolved: the library is
    /// missing or does not export the symbol. Distinct from Unreachable so the
    /// message reads as a deployment problem, not a program bug.
    ExternalCallFailed,
    /// `**` was given a negative exponent at runtime. Its own kind rather than
    /// IntegerOverflow: nothing overflowed, the result is a fraction and there is no
    /// integer to produce. Appended at the end so existing serialized modules keep
    /// their trap numbers.
    NegativeExponent,
};

std::string_view opCodeName(OpCode op);
const char* trapKindName(TrapKind kind);

struct Instruction {
    OpCode op = OpCode::Halt;
    std::int32_t a = 0;
    std::int32_t b = 0;
};

/// Where an instruction came from: a file index into `Module::sourceFiles` and a
/// 1-based line. Both zero means "unknown", which is what an optimizer-generated
/// instruction and a module without source information carry.
///
/// 16 bits for the file (65535 files is beyond any single program) and 32 for the
/// line: the pair fits in 6 bytes and is stored as two numbers, so the format does
/// not depend on struct padding.
struct SourceLine {
    std::uint16_t file = 0;
    std::uint32_t line = 0;

    [[nodiscard]] bool isValid() const { return line != 0; }
    friend bool operator==(const SourceLine&, const SourceLine&) = default;
};

/// One parameter as the calling convention sees it.
struct ParamInfo {
    std::uint32_t offset = 0;   // frame offset of the parameter slot
    std::uint32_t size = 0;
    std::int32_t memType = 0;
    bool aggregate = false;
};

/// One parameter of an externally implemented function, as the dynamic call
/// sees it. Everything crosses the boundary in one 64 bit register, so the
/// kinds are deliberately few: a scalar is passed by value, an array is passed
/// as a pointer to a contiguous copy plus its element count.
struct ExternalParam {
    /// True when the Axiom parameter was `ptr T` and the next parameter is its
    /// element count. The VM then copies `count * elemSize` bytes into a
    /// scratch buffer and passes (buffer, count).
    bool isArray = false;
    std::uint32_t elemSize = 0;
    /// True when the Axiom parameter was `string`. A string is a descriptor --
    /// an address of `{data, length}` -- so passing it as one 64 bit value would
    /// hand C a pointer into VM bookkeeping rather than the characters. The VM
    /// instead copies the bytes out and passes (char pointer, length), which also
    /// consumes two slots. Read only: a string is immutable, so the buffer is
    /// never copied back.
    bool isString = false;
    /// `ptr const T`: the callee does not write through the pointer, so the array
    /// is copied in but never copied back. Worth having because the write-back
    /// costs about four times the read: a store to a cache line the copy has just
    /// evicted pulls it back in first (write-allocate), so 8 MB out is 16 MB of
    /// traffic. Measured at 5000 us of a 6400 us call for a 2M element array.
    bool arrayReadOnly = false;
};

struct Function {
    std::string name;
    std::vector<ParamInfo> params;
    std::uint32_t frameSize = 0;
    bool returnsAggregate = false;
    std::uint32_t returnSize = 0;
    std::int32_t returnMemType = 0;
    bool isProcess = false;
    bool isEventCondition = false;
    bool returnsValue = true;
    /// True for functions the compiler generated itself (the global
    /// initialiser, event trigger conditions). They occupy a function index
    /// like any other, but are not user code and are reported separately.
    bool isSynthetic = false;

    /// Set when the function lives in a shared library (`library "x";` file).
    /// `externalLibrary` indexes Module::externalLibraries; the code vector is
    /// empty and the VM resolves `externalSymbol` on first call.
    int externalLibrary = -1;
    std::string externalSymbol;
    std::vector<ExternalParam> externalParams;
    /// Return value kind: 0 = none (void), 1 = integer, 2 = double.
    int externalReturnKind = 0;

    [[nodiscard]] bool isExternal() const { return externalLibrary >= 0; }

    std::vector<Instruction> code;
    std::vector<Value> constants;

    /// Source position of every instruction, parallel to `code`.
    ///
    /// Two vectors rather than a field inside `Instruction`: the optimizer rewrites
    /// instructions in place (`code[i] = Instruction{...}`) far more often than it
    /// changes the length of the vector, and a parallel array keeps the line for
    /// free at every such site. The only pass that has to move it is `compact()`.
    ///
    /// Empty when the module was built without source information (a hand-written
    /// module, or a `.acvm` from before format v4). Every reader must treat empty
    /// as "unknown", never as line 0.
    std::vector<SourceLine> lines;

    /// Set by the front end before emitting a statement; `emit()` copies it into
    /// `lines`. Zero means "no position known" and is stored as such.
    SourceLine currentLine;

    std::size_t addConstant(Value value);
    std::size_t emit(OpCode op, std::int32_t a = 0, std::int32_t b = 0);

    /// Source position of the instruction at `index`, or an invalid one.
    [[nodiscard]] SourceLine lineAt(std::size_t index) const;
    std::size_t emitJump(OpCode op);
    void patchJump(std::size_t position, std::size_t target);
};

struct GlobalSlot {
    std::string name;
    std::uint32_t size = 0;
    std::uint32_t align = 1;
    std::uint32_t offset = 0;
};

/// A compiled translation unit. Completely self contained: no pointers into
/// the AST or the source buffers.
struct Module {
    std::vector<Function> functions;
    std::vector<GlobalSlot> globals;
    std::vector<MemType> memTypes;
    std::vector<std::string> strings;

    /// Source files referenced by the line tables, in first-use order. A file
    /// index stored in `SourceLine` points here, so a compiled module carries the
    /// names it needs to print `main.ax:42` without the sources being present.
    std::vector<std::string> sourceFiles;

    std::uint32_t globalDataSize = 0;
    int entryPoint = -1;
    int initFunction = -1;
    std::vector<int> processes;
    std::vector<int> events;          // event function indices
    std::vector<int> eventConditions; // parallel: condition function indices

    /// Shared libraries this module needs at run time, in first-use order.
    /// Absolute paths are kept as written; bare names are resolved by the
    /// platform loader next to the module or on the system path.
    std::vector<std::string> externalLibraries;

    int addFunction(Function function);
    [[nodiscard]] int findFunction(std::string_view name) const;
    int internMemoryType(const MemType& type);
    int addString(std::string text);
    int addExternalLibrary(std::string path);
};

} // namespace acvm
