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

    /// A 32-bit signed integer -- the type of every loop counter, every `int` local, and every
    /// `if (n < 2)`.
    ///
    /// Named once because five sites asked the same four questions in a row: the optimizer
    /// before fusing a branch, and the VM's fast paths for `LOAD_LOCAL`, `STORE_LOCAL`,
    /// `STEP_JUMP` and parameter passing. Five copies of a predicate is five chances for one of
    /// them to be a condition short, and the failure would be a fast path that is subtly wrong
    /// for `unsigned int` rather than one that is slow.
    [[nodiscard]] bool isPlainInt32() const {
        return kind == Kind::Int && size == 4 && bits == 32 && !isUnsigned;
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

    /// Compare two stack values and branch, replacing a comparison followed by a conditional
    /// jump.
    ///
    /// `a` is the jump target. `b` packs three fields, because an `Instruction` has two
    /// operands and this fuses three instructions into one:
    ///
    ///     bits 0-7    memType index
    ///     bits 8-10   which comparison, in the order Eq, Ne, Lt, Le, Gt, Ge
    ///     bit  11     branch when the comparison is *true* rather than false
    ///
    /// The direction bit exists because both `JUMP_IF_TRUE` and `JUMP_IF_FALSE` are emitted.
    /// Inverting the comparison instead would be equivalent -- all six have a complement in
    /// the set -- but it would make the fused instruction not a faithful record of what the
    /// two original instructions did, and the disassembly is read by people.
    ///
    /// The shape it replaces is the header of every loop, so it is four instructions of every
    /// iteration: `LOAD_LOCAL i; CONST_INT N; LT; JUMP_IF_FALSE`. Appended at the end of the
    /// enum for the reason given above `Pow`.
    CompareBranch,

    /// `STEP_LOCAL; JUMP` -- step a local by one and jump.
    ///
    /// This is the closing pair of every `for` loop: the third clause is a step, and the loop
    /// ends with a jump back to its header. `a` is the jump target -- kept in `a` because every
    /// other branch in this instruction set puts it there, and `jumpTargets`/`compact` rely on
    /// that. `b` packs what the step needs; see `stepJump` below.
    ///
    /// Only ±1 is encoded, and the peephole refuses anything else: a general delta would not
    /// fit beside the slot and the type, and the loops worth fusing count by one.
    StepJump,

    /// `LOAD_LOCAL slot; CONST c; CMP_BRANCH ...` -- a local against a constant, in one.
    ///
    /// The most common branch in any program, because it is the shape of every loop bound and
    /// every `if (n < 2)`: load a variable, compare it to a literal, branch. Three instructions
    /// become one, and the comparison becomes a 32-bit compare instead of a call into
    /// `computeBinary` with its float/pointer/128-bit arms.
    ///
    /// Only `int32` operands are fused -- that is what a loop counter and an `int` local are,
    /// and covering every type would mean carrying the memType and giving up the immediate.
    /// Anything else keeps the three-instruction form and runs exactly as before.
    CmpLocalImm,

    /// `LOAD_LOCAL src; STORE_LOCAL dst keep` -- copy one local to another and leave it on the
    /// stack, in one instruction.
    ///
    /// This is how every scalar `return` ends: the value is put in the return slot and also
    /// left on the stack for `RET` to take. Two instructions that both touch the value stack,
    /// replaced by one that reads a frame slot, writes a frame slot, and pushes.
    CopyLocal,

    /// `LOAD_LOCAL slot; CONST c; ADD|SUB` -- a local plus or minus a constant, in one.
    ///
    /// `fib` emits two of these per call (`n - 1`, `n - 2`) and so does most arithmetic that
    /// walks a sequence. As with `CMP_LOCAL_IMM`, the operands are `int32` only, checked by the
    /// optimizer, and the constant has to fit 32 bits.
    ///
    /// One opcode for both operations: `b` carries the sign, which is one bit rather than a
    /// second instruction. A separate `AddLocalImm` would cost a jump-table slot to save a
    /// shift.
    AddSubLocalImm,

    /// `LOAD_LOCAL slot; RET` -- return the value in a frame slot.
    ///
    /// Every function that returns a scalar ends by storing into the return slot, jumping to
    /// one shared epilogue, and having that epilogue load the slot back so `RET` can take the
    /// value off the value stack. The load exists only because `RET` reads the stack; the
    /// epilogue is shared, so the value has to get back there somehow.
    ///
    /// The first attempt at removing it set the store's *keep* bit instead, which is
    /// equivalent in the interpreter and ambiguous to the JIT: the compiled code models the
    /// value stack separately, and a store that both writes a slot and leaves a value behind
    /// is one thing to the interpreter and two to a translator. This is the same optimisation
    /// without that ambiguity -- one instruction, one meaning, and nothing left on the stack.
    ///
    /// `a` is the frame offset. Appended at the end of the enum for the reason given above
    /// `Pow`.
    RetLocal,
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

//===--- CompareBranch operand packing -----------------------------------===//
//
// Named rather than written by hand at each site: the two halves of this encoding are set in
// the optimizer and read in the VM and the disassembler, and a mask that disagrees by one bit
// between them is a wrong comparison rather than a crash.
namespace compareBranch {

inline constexpr int kMemTypeShift = 0;
inline constexpr int kComparisonShift = 8;
inline constexpr int kBranchOnTrueShift = 11;

inline constexpr std::int32_t kMemTypeMask = 0xFF;
inline constexpr std::int32_t kComparisonMask = 0x7 << kComparisonShift;

/// The comparisons this instruction can carry, in the order the packed field stores them.
enum class Comparison : std::int32_t { Eq = 0, Ne = 1, Lt = 2, Le = 3, Gt = 4, Ge = 5 };

[[nodiscard]] inline OpCode toOpCode(Comparison comparison) {
    switch (comparison) {
    case Comparison::Eq: return OpCode::Eq;
    case Comparison::Ne: return OpCode::Ne;
    case Comparison::Lt: return OpCode::Lt;
    case Comparison::Le: return OpCode::Le;
    case Comparison::Gt: return OpCode::Gt;
    case Comparison::Ge: return OpCode::Ge;
    }
    return OpCode::Eq;
}

[[nodiscard]] inline std::int32_t pack(int memType, Comparison comparison, bool branchOnTrue) {
    return ((memType & kMemTypeMask) << kMemTypeShift) |
           ((static_cast<std::int32_t>(comparison) << kComparisonShift) & kComparisonMask) |
           (branchOnTrue ? (1 << kBranchOnTrueShift) : 0);
}

[[nodiscard]] inline int memTypeOf(std::int32_t b) {
    return (b >> kMemTypeShift) & kMemTypeMask;
}
[[nodiscard]] inline Comparison comparisonOf(std::int32_t b) {
    return static_cast<Comparison>((b & kComparisonMask) >> kComparisonShift);
}
[[nodiscard]] inline bool branchesOnTrue(std::int32_t b) {
    return (b & (1 << kBranchOnTrueShift)) != 0;
}

} // namespace compareBranch

//===--- StepJump operand packing ----------------------------------------===//
//
// `b` carries three fields, because a 32 bit operand is all there is and a step needs to know
// where it writes, what width it writes at, and which direction it counts:
//
//     bits 0-23    frame offset              (frames up to 16 MB)
//     bits 24-29   memType index             (64 distinct types)
//     bit  30      step is negative
//
// The optimizer refuses to fuse anything that does not fit, rather than truncating: a loop
// whose counter lives 16 MB into a frame, or whose type is the 65th interned one, keeps the
// unfused pair and runs at the old speed. Silently masking here would be the third variant of
// the mistake that truncated int64 values in an earlier fusion.
namespace stepJump {

inline constexpr int kSlotBits = 24;
inline constexpr std::int32_t kSlotMask = (1 << kSlotBits) - 1;
inline constexpr int kMemTypeShift = kSlotBits;
inline constexpr std::int32_t kMemTypeMask = 0x3F << kMemTypeShift;
inline constexpr std::int32_t kNegativeStepBit = 1 << 30;
inline constexpr int kMaxMemType = 0x3F;

/// True when this instruction can carry the requested step. False means "do not fuse".
[[nodiscard]] inline bool encodable(std::int32_t slot, int memType) {
    return slot >= 0 && slot <= kSlotMask && memType >= 0 && memType <= kMaxMemType;
}

[[nodiscard]] inline std::int32_t pack(std::int32_t slot, int memType, bool negative) {
    return (slot & kSlotMask) | ((memType << kMemTypeShift) & kMemTypeMask) |
           (negative ? kNegativeStepBit : 0);
}
[[nodiscard]] inline std::int32_t slotOf(std::int32_t b) { return b & kSlotMask; }
[[nodiscard]] inline int memTypeOf(std::int32_t b) {
    return (b & kMemTypeMask) >> kMemTypeShift;
}
[[nodiscard]] inline bool isNegative(std::int32_t b) {
    return (b & kNegativeStepBit) != 0;
}

} // namespace stepJump

//===--- CmpLocalImm operand packing -------------------------------------===//
//
// `a` is the branch target, as it is for every other branch in this instruction set.
// `b` carries the rest:
//
//     bits 0-15    frame slot                 (frames up to 64 KB)
//     bits 16-26   constant index             (2048 constants per function)
//     bits 27-29   comparison                 (Eq..Ge, as in `compareBranch`)
//     bit  30      branch when the comparison is true
//
// 31 bits, so `b` is never negative and `>>` needs no care. Anything that does not fit is not
// fused: a slot past 64 KB, a constant past 2048, an operand that is not `int32`. The
// optimizer refuses those rather than masking, for the reason the two previous fusions here
// taught: masking truncates silently, and a truncated operand is a wrong answer, not a slow one.
namespace compareLocal {

inline constexpr int kSlotBits = 16;
inline constexpr std::int32_t kSlotMask = (1 << kSlotBits) - 1;
inline constexpr int kConstantShift = kSlotBits;
inline constexpr int kConstantBits = 11;
inline constexpr std::int32_t kConstantMask = ((1 << kConstantBits) - 1) << kConstantShift;
inline constexpr int kComparisonShift = kConstantShift + kConstantBits;
inline constexpr std::int32_t kComparisonMask = 0x7 << kComparisonShift;
inline constexpr std::int32_t kBranchOnTrueBit = 1 << 30;

inline constexpr std::int32_t kMaxSlot = kSlotMask;
inline constexpr std::int32_t kMaxConstant = (1 << kConstantBits) - 1;

/// True when this comparison can be carried. False means "do not fuse".
[[nodiscard]] inline bool encodable(std::int32_t slot, std::int32_t constant) {
    return slot >= 0 && slot <= kMaxSlot && constant >= 0 && constant <= kMaxConstant;
}

[[nodiscard]] inline std::int32_t pack(std::int32_t slot, std::int32_t constant,
                                       compareBranch::Comparison comparison, bool branchOnTrue) {
    return (slot & kSlotMask) | ((constant << kConstantShift) & kConstantMask) |
           ((static_cast<std::int32_t>(comparison) << kComparisonShift) & kComparisonMask) |
           (branchOnTrue ? kBranchOnTrueBit : 0);
}
[[nodiscard]] inline std::int32_t slotOf(std::int32_t b) { return b & kSlotMask; }
[[nodiscard]] inline std::int32_t constantOf(std::int32_t b) {
    return (b & kConstantMask) >> kConstantShift;
}
[[nodiscard]] inline compareBranch::Comparison comparisonOf(std::int32_t b) {
    return static_cast<compareBranch::Comparison>((b & kComparisonMask) >> kComparisonShift);
}
[[nodiscard]] inline bool branchesOnTrue(std::int32_t b) {
    return (b & kBranchOnTrueBit) != 0;
}

/// The answer of `lhs <cmp> rhs` for a 32-bit signed comparison.
///
/// Written out rather than routed through `computeBinary`: the point of this instruction is to
/// not call it. The six cases match `computeBinary`'s, and `compare_local_check.ax` pins all
/// six against the three-instruction form, so the two cannot drift.
[[nodiscard]] inline bool compare32(compareBranch::Comparison comparison, std::int32_t lhs,
                                   std::int32_t rhs) {
    switch (comparison) {
    case compareBranch::Comparison::Eq: return lhs == rhs;
    case compareBranch::Comparison::Ne: return lhs != rhs;
    case compareBranch::Comparison::Lt: return lhs < rhs;
    case compareBranch::Comparison::Le: return lhs <= rhs;
    case compareBranch::Comparison::Gt: return lhs > rhs;
    case compareBranch::Comparison::Ge: return lhs >= rhs;
    }
    return false;
}

} // namespace compareLocal

//===--- AddSubLocalImm operand packing ----------------------------------===//
//
// `a` is the frame slot to read. `b` carries the constant and the operation:
//
//     bits 0-30   constant index
//     bit  31     subtract rather than add
//
// The constant index is checked against `kMaxConstant` rather than masked, so a function with
// more constants than the field holds keeps the three-instruction form. That is the same rule
// as everywhere else here: refuse to fuse, never truncate a field.
namespace addSubLocal {

inline constexpr std::int32_t kSubtractBit = static_cast<std::int32_t>(1u << 31);
inline constexpr std::int32_t kConstantMask = ~kSubtractBit;

inline constexpr std::int32_t kMaxConstant = kConstantMask;

[[nodiscard]] inline bool encodable(std::int32_t slot, std::int32_t constant) {
    return slot >= 0 && constant >= 0 && constant <= kMaxConstant;
}

[[nodiscard]] inline std::int32_t pack(std::int32_t constant, bool subtract) {
    return (constant & kConstantMask) | (subtract ? kSubtractBit : 0);
}
[[nodiscard]] inline std::int32_t constantOf(std::int32_t b) { return b & kConstantMask; }
[[nodiscard]] inline bool isSubtract(std::int32_t b) { return (b & kSubtractBit) != 0; }

} // namespace addSubLocal

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
