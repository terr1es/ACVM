//===--- Jit.cpp - lowering bytecode to x86-64 ---------------------------===//
//
// Level 1 of the JIT: straight-line integer code, local variables, branches, and a
// return. No calls, no aggregates, no strings, no builtins, no floating point. A
// function that contains any of those is refused as a whole and stays interpreted --
// which keeps this pass small enough to review, and makes the differential test
// meaningful, because a refused function is exactly the case the interpreter still has
// to handle correctly.
//
// The generated shape is:
//
//   push  rbx, r12, r13, r14      save what the ABI says is ours to save
//   sub   rsp, kFrameBytes        shadow space for calls, plus the value stack
//   mov   rbx, rcx                VM*
//   mov   r14, rdx                Process*
//   mov   r12, r8                 frame base, constant for the whole function
//   mov   r13, r9                 std::string* receiving a trap message
//
//   ... body; values live in 8 byte slots on the native stack ...
//
//   add   rsp, kFrameBytes
//   pop   r14, r13, r12, rbx
//   ret
//
// Values sit in native stack slots rather than registers at this level. That is not where
// the design ends -- the measurement said the cost is the 32 byte `Value` copy and the
// generic memory path, not the dispatch, so registers are the point -- but a stack slot
// is a place where a register allocator can be dropped in later without changing a single
// instruction selection, and it makes the first *correct* version reachable today.
//
// Frame slots are read and written by calling back into the VM. That is slow on purpose:
// it makes the compiled path apply exactly the same width, sign extension and range rules
// as the interpreter, so any difference the differential test reports is a bug in this
// file rather than in a second implementation of the memory model. Replacing these calls
// with a page cache is the next step, and it is the one that pays.

#include "acvm/VM/Jit.h"

#include "acvm/JIT/X64Assembler.h"
#include "acvm/VM/VM.h"

#include <limits>
#include <string>

namespace acvm {

namespace jitbridge {

bool loadSlot(VM& vm, std::uint64_t address, int typeIndex, std::int64_t& out) {
    return VM::JitBridge::loadSlot(vm, address, typeIndex, out);
}

bool storeSlot(VM& vm, std::uint64_t address, int typeIndex, std::int64_t value) {
    return VM::JitBridge::storeSlot(vm, address, typeIndex, value);
}

bool loadThroughPointer(VM& vm, std::uint64_t address, int typeIndex,
                        std::int64_t& out) {
    return VM::JitBridge::loadThroughPointer(vm, address, typeIndex, out);
}

bool storeThroughPointer(VM& vm, std::uint64_t address, int typeIndex,
                         std::int64_t value) {
    return VM::JitBridge::storeThroughPointer(vm, address, typeIndex, value);
}

void returnToCaller(void* process, bool hasResult, std::int64_t result) {
    VM::JitBridge::returnToCaller(process, hasResult, result);
}

void setTrapMessage(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace jitbridge

namespace {

using jit::Cond;
using jit::mem;
using jit::Reg;
using jit::Width;
using jit::X64Assembler;

/// Value slots on the native stack. Axiom's expression depth is bounded by the parser and
/// 64 is far past anything a real expression produces; going over refuses the function
/// rather than wrapping around.
constexpr std::size_t kMaxValueSlots = 64;
constexpr std::size_t kSlotBytes = 8;
constexpr std::size_t kShadowSpace = 32;

/// `% 16 == 8` because the prologue pushes four registers, which leaves rsp misaligned by
/// 8; subtracting a further 8 restores the 16 byte alignment the ABI requires at every
/// call site. Getting this wrong produces crashes in unrelated SSE code.
constexpr std::size_t kFrameBytes =
    kShadowSpace + kMaxValueSlots * kSlotBytes + 8;

constexpr Reg kRegVm = Reg::Rbx;
constexpr Reg kRegProcess = Reg::R14;
constexpr Reg kRegBase = Reg::R12;
constexpr Reg kRegError = Reg::R13;

/// Argument registers, named for the ABI role rather than the register. The previous
/// version called rcx "kRegB" and then used it as a temporary in a place where it was
/// also the first argument -- the alias hid the collision, and the parameter was
/// silently replaced by the VM pointer.
constexpr Reg kArg1 = Reg::Rcx;
constexpr Reg kArg2 = Reg::Rdx;
constexpr Reg kArg3 = Reg::R8;
constexpr Reg kArg4 = Reg::R9;

/// Scratch that is neither an argument register nor one of the four reserved above.
/// Caller-saved by the ABI, which is fine: nothing needs to survive a helper call except
/// the four reserved registers.
constexpr Reg kTmp1 = Reg::R10;
/// A second scratch, used where two values have to stay live across one emit (the
/// dividend and the INT64_MIN it is compared against, for instance).
constexpr Reg kTmp2 = Reg::R11;

/// General purpose accumulator, used for the value being pushed or popped.
constexpr Reg kRegA = Reg::Rax;

std::int32_t slotOffset(std::size_t index) {
    return static_cast<std::int32_t>(kShadowSpace + index * kSlotBytes);
}

/// Messages are static storage so the trap path can materialise their address with a
/// single `mov imm64` instead of building a RIP-relative reference to a local.
constexpr const char kOverflowMessage[] = "integer overflow in compiled code";
constexpr const char kNullPointerMessage[] = "null pointer in compiled code";
constexpr const char kDivideByZeroMessage[] = "division by zero in compiled code";


} // namespace

std::unique_ptr<jit::ExecutableBlock> compileFunction(const Module& module,
                                                     const Function& function,
                                                     std::string& whyNot) {
    if (function.isExternal()) {
        whyNot = "function lives in a shared library";
        return nullptr;
    }
    if (function.code.empty()) {
        whyNot = "function has no code";
        return nullptr;
    }
    if (function.returnsAggregate) {
        whyNot = "returns an aggregate";
        return nullptr;
    }
    if (function.returnsValue && function.returnSize > 8) {
        whyNot = "return value wider than a register";
        return nullptr;
    }

    // Axiom functions that are process bodies start with a SAFE_POINT and must yield;
    // compiled code does not yield, so they stay interpreted.
    if (function.isProcess) {
        whyNot = "process body (must remain preemptible)";
        return nullptr;
    }

    X64Assembler assembler;

    X64Assembler::Label trapExit = assembler.newLabel();
    X64Assembler::Label nullExit = assembler.newLabel();
    X64Assembler::Label divZeroExit = assembler.newLabel();
    X64Assembler::Label failureExit = assembler.newLabel();

    assembler.push(Reg::Rbx);
    assembler.push(Reg::R12);
    assembler.push(Reg::R13);
    assembler.push(Reg::R14);
    assembler.subImm(Reg::Rsp, static_cast<std::int32_t>(kFrameBytes));
    assembler.mov(kRegVm, Reg::Rcx);
    assembler.mov(kRegProcess, Reg::Rdx);
    assembler.mov(kRegBase, Reg::R8);
    assembler.mov(kRegError, Reg::R9);

    const std::size_t codeSize = function.code.size();
    constexpr std::size_t kNoLabel = std::numeric_limits<std::size_t>::max();
    std::vector<X64Assembler::Label> labels(codeSize, kNoLabel);

    // Jump targets are labelled up front, before any instruction is emitted. Creating
    // one on first sight looks natural and is wrong: a backward branch names an
    // instruction that has already been emitted, so its label would be created after the
    // only place that could ever bind it. The assembler then refuses the whole function,
    // which is how this bug surfaced -- as "could not produce executable code" rather
    // than as a wrong answer.
    for (const Instruction& candidate : function.code) {
        if (candidate.op != OpCode::Jump && candidate.op != OpCode::JumpIfFalse &&
            candidate.op != OpCode::JumpIfTrue) {
            continue;
        }
        if (candidate.a < 0 || static_cast<std::size_t>(candidate.a) >= codeSize) {
            whyNot = "in '" + function.name + "': jump target outside the function";
            return nullptr;
        }
        const auto index = static_cast<std::size_t>(candidate.a);
        if (labels[index] == kNoLabel) {
            labels[index] = assembler.newLabel();
        }
    }

    // -- stack depth per instruction ---------------------------------------
    //
    // The depth cannot be tracked linearly. A ternary compiles to
    //
    //      ...  JUMP_IF_FALSE -> L      ; pops the condition
    //           push 1                  ; true arm
    //           JUMP -> M
    //      L:   push 1 ; NEG            ; false arm
    //      M:   ...
    //
    // Walking that straight through counts the true arm's push and then the false arm's
    // push as well, so the merge point looks two deep when it is one. The interpreter
    // never notices because its depth is the real stack; a compiled function has to know
    // the depth before it emits anything.
    //
    // A forward pass over the control flow graph fixes it: every instruction gets the
    // depth implied by all the edges that reach it, and a disagreement means the bytecode
    // is not well formed for this level.
    constexpr int kUnknownDepth = -1;
    std::vector<int> depthAt(codeSize, kUnknownDepth);
    depthAt[0] = 0;
    {
        // Effects are (pops, pushes). Anything with a data-dependent effect is not
        // supported yet, and refusing here keeps the emission loop free of depth
        // surprises.
        auto effectOf = [&](const Instruction& inst, int& pops, int& pushes) -> bool {
            pops = 0;
            pushes = 0;
            switch (inst.op) {
            case OpCode::ConstInt:
            case OpCode::ConstFloat:
            case OpCode::ConstStr:
            case OpCode::LocalAddr:
            case OpCode::GlobalAddr:
            case OpCode::LoadLocal:
            case OpCode::LoadGlobal:
            case OpCode::DerefLocal:
            case OpCode::ArenaMark:
                pushes = 1;
                return true;
            case OpCode::Load: pops = 1; pushes = 1; return true;
            case OpCode::Store: pops = 2; return true;
            case OpCode::StoreKeep: pops = 2; pushes = 1; return true;
            case OpCode::StoreLocal:
            case OpCode::StoreGlobal:
                // `b` carries the memory type in the low byte and a keep bit at 0x100:
                // with the bit set the stored value stays on the stack, which is how
                // `i++` yields the old value. Modelling the two the same way made valid
                // bytecode look like it underflowed.
                pops = 1;
                pushes = (inst.b & 0x100) != 0 ? 1 : 0;
                return true;
            case OpCode::Dup: pops = 1; pushes = 2; return true;
            case OpCode::Swap: pops = 2; pushes = 2; return true;
            case OpCode::Pop: pops = 1; return true;
            case OpCode::Add: case OpCode::Sub: case OpCode::Mul: case OpCode::Div:
            case OpCode::Mod: case OpCode::And: case OpCode::Or: case OpCode::Xor:
            case OpCode::Shl: case OpCode::Shr: case OpCode::Eq: case OpCode::Ne:
            case OpCode::Lt: case OpCode::Le: case OpCode::Gt: case OpCode::Ge:
            case OpCode::StrEq: case OpCode::StrCmp: case OpCode::MemEq:
            case OpCode::Concat:
                pops = 2; pushes = 1; return true;
            case OpCode::Neg: case OpCode::BitNot: case OpCode::Not:
            case OpCode::Cast: case OpCode::StepLocal: case OpCode::CopyStep:
            case OpCode::SafePoint: case OpCode::Trap: case OpCode::Halt:
                return true;
            case OpCode::JumpIfFalse: case OpCode::JumpIfTrue: pops = 1; return true;
            case OpCode::Jump: return true;
            case OpCode::Ret:
                // A void function's RET pops nothing. Assuming it always pops one made
                // every void function look like it underflowed at its own return.
                pops = function.returnsValue ? 1 : 0;
                return true;
            case OpCode::MemCopy: pops = 2; return true;
            case OpCode::MemZero: pops = 1; return true;
            case OpCode::ArenaRelease: pops = 1; return true;
            default:
                // Call, Builtin: the effect depends on the callee or the builtin id.
                return false;
            }
        };

        std::vector<std::size_t> work{0};
        while (!work.empty()) {
            const std::size_t ip = work.back();
            work.pop_back();

            // A poisoned entry means two edges disagreed about the depth, or an
            // instruction with a data-dependent effect (Call, Builtin) left the successor
            // unreachable. Either way the depth here is not known, and computing from it
            // would produce a misleading "underflow" -- so stop the analysis instead.
            if (depthAt[ip] < 0) {
                break;
            }

            const Instruction& inst = function.code[ip];

            int pops = 0;
            int pushes = 0;
            if (!effectOf(inst, pops, pushes)) {
                // Unsupported opcodes are reported by the emission loop with a better
                // message; stop the analysis rather than inventing a depth for them.
                depthAt[ip] = depthAt[ip];
                if (inst.op != OpCode::Jump && inst.op != OpCode::JumpIfFalse &&
                    inst.op != OpCode::JumpIfTrue && inst.op != OpCode::Ret &&
                    inst.op != OpCode::Halt) {
                    continue;
                }
            }

            const int after = depthAt[ip] - pops + pushes;
            if (after < 0) {
                whyNot = "in '" + function.name + "': expression stack underflow at " +
                         std::to_string(ip) + " (depth " +
                         std::to_string(depthAt[ip]) + ", pops " + std::to_string(pops) +
                         ", pushes " + std::to_string(pushes) + ")";
                return nullptr;
            }

            auto reach = [&](std::size_t target) {
                if (target >= codeSize) {
                    return;
                }
                if (depthAt[target] == kUnknownDepth) {
                    depthAt[target] = after;
                    work.push_back(target);
                } else if (depthAt[target] != after) {
                    // Two edges disagree about the depth. The interpreter would not
                    // notice -- it uses the runtime stack -- but compiled code must, so
                    // this is refused rather than guessed.
                    depthAt[target] = kUnknownDepth - 1;   // mark as poisoned
                }
            };

            const bool flowsOn = inst.op != OpCode::Jump && inst.op != OpCode::Ret &&
                                 inst.op != OpCode::Halt && inst.op != OpCode::Trap;
            if (flowsOn && ip + 1 < codeSize) {
                reach(ip + 1);
            }
            if (inst.op == OpCode::Jump || inst.op == OpCode::JumpIfFalse ||
                inst.op == OpCode::JumpIfTrue) {
                reach(static_cast<std::size_t>(inst.a));
            }
        }
    }

    std::size_t depth = 0;
    std::string refusal;
    bool emittedReturn = false;

    // Every target already has a label from the pre-scan above, so this is a lookup.
    auto labelFor = [&](std::int32_t target) -> X64Assembler::Label {
        return labels[static_cast<std::size_t>(target)];
    };

    /// Describes a memory type well enough to decide whether this level can handle it.
    /// The two things that matter are "does it fit a 64 bit value" and "is it the signed
    /// integer case whose lowering is implemented".
    auto slotIsSupportedInteger = [&](int typeIndex) {
        if (typeIndex < 0 ||
            static_cast<std::size_t>(typeIndex) >= module.memTypes.size()) {
            return false;
        }
        const MemType& type = module.memTypes[static_cast<std::size_t>(typeIndex)];
        if (!type.isInteger()) {
            return false;
        }
        const unsigned bits = type.bits == 0 ? static_cast<unsigned>(type.size * 8)
                                             : type.bits;
        if (bits > 64) {
            return false;
        }
        // Unsigned arithmetic wraps rather than trapping, which is a different lowering
        // with no overflow test at all. Kept out so the signed path is validated alone.
        return !type.isUnsigned;
    };

    /// Kinds of integer a 64 bit register can hold. Comparisons need to know the
    /// signedness because `jl` and `jb` are different instructions, while the arithmetic
    /// lowering below only implements the signed case.
    enum class IntKind { Unsupported, Signed, Unsigned };

    auto intKindOf = [&](int typeIndex) {
        if (typeIndex < 0 ||
            static_cast<std::size_t>(typeIndex) >= module.memTypes.size()) {
            return IntKind::Unsupported;
        }
        const MemType& type = module.memTypes[static_cast<std::size_t>(typeIndex)];
        if (!type.isInteger()) {
            return IntKind::Unsupported;
        }
        const unsigned bits = type.bits == 0 ? static_cast<unsigned>(type.size * 8)
                                             : type.bits;
        if (bits > 64) {
            return IntKind::Unsupported;
        }
        return type.isUnsigned ? IntKind::Unsigned : IntKind::Signed;
    };

    auto bitWidthOf = [&](int typeIndex) -> unsigned {
        if (typeIndex < 0 ||
            static_cast<std::size_t>(typeIndex) >= module.memTypes.size()) {
            return 64;
        }
        const MemType& type = module.memTypes[static_cast<std::size_t>(typeIndex)];
        return type.bits == 0 ? static_cast<unsigned>(type.size * 8) : type.bits;
    };

    /// Emits the range check the interpreter performs after every signed operation: the
    /// result has to fit the *declared* width, not just 64 bits. Without this an `int32`
    /// sum that leaves the type silently keeps its 64 bit value while the interpreter
    /// traps -- a difference that only shows up on overflow, which is why it has to be
    /// handled here rather than discovered later.
    ///
    /// The test is "does the result survive sign extension from the declared width", done
    /// with one movsx plus a compare: two instructions, no branch beyond the join.
    auto checkFitsWidth = [&](unsigned bits) {
        if (bits >= 64) {
            return;   // the overflow flag from the operation already covered this
        }
        if (bits == 32) {
            assembler.movsxd(kTmp1, kRegA);
        } else if (bits == 16) {
            assembler.movsx(kTmp1, kRegA, Width::Word);
        } else {
            assembler.movsx(kTmp1, kRegA, Width::Byte);
        }
        assembler.cmp(kTmp1, kRegA, Width::Qword);
        assembler.jcc(Cond::NotEqual, trapExit);
    };

    auto pushA = [&]() {
        if (depth >= kMaxValueSlots) {
            refusal = "expression stack deeper than the JIT supports";
            return false;
        }
        assembler.mov(mem(Reg::Rsp, slotOffset(depth)), kRegA);
        ++depth;
        return true;
    };

    for (std::size_t ip = 0; ip < codeSize && refusal.empty(); ++ip) {
        if (labels[ip] != kNoLabel) {
            assembler.bind(labels[ip]);
        }

        // The depth at this instruction comes from the dataflow pass, not from carrying a
        // counter through the linear order: at a branch merge the two differ, and the
        // merge point is exactly where a wrong depth turns into a wrong answer.
        if (depthAt[ip] < 0) {
            refusal = "unreachable instruction with no known stack depth";
            break;
        }
        depth = static_cast<std::size_t>(depthAt[ip]);

        const Instruction& inst = function.code[ip];
        switch (inst.op) {
        case OpCode::ConstInt: {
            if (inst.a < 0 ||
                static_cast<std::size_t>(inst.a) >= function.constants.size()) {
                refusal = "constant index out of range";
                break;
            }
            const Value& value = function.constants[static_cast<std::size_t>(inst.a)];
            if (value.tag() == Value::Tag::Float) {
                refusal = "floating point constant";
                break;
            }
            const Int128 raw = value.asInt();
            if (raw < std::numeric_limits<std::int64_t>::min() ||
                raw > std::numeric_limits<std::int64_t>::max()) {
                refusal = "constant wider than 64 bits";
                break;
            }
            assembler.movImm(kRegA, Width::Qword, static_cast<std::int64_t>(raw));
            pushA();
            break;
        }

        case OpCode::LoadLocal: {
            const int typeIndex = inst.b & 0xFF;
            if (!slotIsSupportedInteger(typeIndex)) {
                refusal = "local variable of an unsupported type";
                break;
            }
            if (depth >= kMaxValueSlots) {
                refusal = "expression stack deeper than the JIT supports";
                break;
            }
            // loadSlot(VM&, address, typeIndex, int64& out) -> bool
            assembler.mov(kTmp1, kRegBase);
            assembler.addImm(kTmp1, inst.a);
            assembler.lea(kArg4, mem(Reg::Rsp, slotOffset(depth)));
            assembler.mov(kArg1, kRegVm);
            assembler.mov(kArg2, kTmp1);
            assembler.movImm(kArg3, Width::Dword, typeIndex);
            assembler.callAbsolute(reinterpret_cast<const void*>(&jitbridge::loadSlot));
            assembler.test(kRegA, kRegA);
            assembler.jcc(Cond::Equal, failureExit);
            ++depth;
            break;
        }

        case OpCode::StoreLocal: {
            const int typeIndex = inst.b & 0xFF;
            if (!slotIsSupportedInteger(typeIndex)) {
                refusal = "local variable of an unsupported type";
                break;
            }
            if (depth == 0) {
                refusal = "stack underflow in STORE_LOCAL";
                break;
            }
            // The value is always the top of stack. With the keep bit set it also stays
            // there afterwards, so only the depth bookkeeping differs -- reading from
            // `slotOffset(depth)` after not decrementing instead of before read an empty
            // slot and stored garbage, which is how `a++` came out as 1 instead of 7.
            const bool keepValue = (inst.b & 0x100) != 0;
            const std::size_t valueSlot = depth - 1;
            if (!keepValue) {
                --depth;
            }
            // storeSlot(VM&, address, typeIndex, value) -> bool
            assembler.mov(kArg4, mem(Reg::Rsp, slotOffset(valueSlot)));
            assembler.mov(kTmp1, kRegBase);
            assembler.addImm(kTmp1, inst.a);
            assembler.mov(kArg1, kRegVm);
            assembler.mov(kArg2, kTmp1);
            assembler.movImm(kArg3, Width::Dword, typeIndex);
            assembler.callAbsolute(reinterpret_cast<const void*>(&jitbridge::storeSlot));
            assembler.test(kRegA, kRegA);
            assembler.jcc(Cond::Equal, failureExit);
            break;
        }

        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Mul:
        case OpCode::Neg:
        case OpCode::Not: {
            const int typeIndex = inst.a;
            if (!slotIsSupportedInteger(typeIndex)) {
                refusal = "arithmetic on an unsupported type";
                break;
            }
            if (depth == 0) {
                refusal = "stack underflow in arithmetic";
                break;
            }
            if (inst.op == OpCode::Neg || inst.op == OpCode::Not) {
                assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth - 1)));
                if (inst.op == OpCode::Neg) {
                    assembler.neg(kRegA, Width::Qword);
                    assembler.jcc(Cond::Overflow, trapExit);
                    checkFitsWidth(bitWidthOf(inst.a));
                } else {
                    assembler.not_(kRegA, Width::Qword);
                }
                assembler.mov(mem(Reg::Rsp, slotOffset(depth - 1)), kRegA);
                break;
            }
            if (depth < 2) {
                refusal = "stack underflow in arithmetic";
                break;
            }
            // lhs into rax, rhs into rcx. Subtraction is order-sensitive and this is
            // where a swapped operand pair would turn `a - b` into `b - a` -- which is
            // invisible for `+` and `*` and wrong for `-`, `/` and comparisons.
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth - 2)));
            assembler.mov(kArg1, mem(Reg::Rsp, slotOffset(depth - 1)));
            switch (inst.op) {
            case OpCode::Add: assembler.add(kRegA, kArg1); break;
            case OpCode::Sub: assembler.sub(kRegA, kArg1); break;
            case OpCode::Mul: assembler.imul(kRegA, kArg1); break;
            default: break;
            }
            // The interpreter traps on overflow at every signed width; so must this.
            assembler.jcc(Cond::Overflow, trapExit);
            checkFitsWidth(bitWidthOf(inst.a));
            --depth;
            assembler.mov(mem(Reg::Rsp, slotOffset(depth - 1)), kRegA);
            break;
        }

        case OpCode::Div:
        case OpCode::Mod: {
            if (intKindOf(inst.a) != IntKind::Signed) {
                refusal = "division on an unsupported type";
                break;
            }
            if (depth < 2) {
                refusal = "stack underflow in division";
                break;
            }
            const unsigned bits = bitWidthOf(inst.a);
            auto divDone = assembler.newLabel();

            // dividend into rax, divisor into r10. r10 rather than rcx: `cqo` writes rdx
            // and `idiv` reads rax:rdx, so the divisor must stay clear of both -- the
            // assembler test for division failed on exactly this and crashed.
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth - 2)));
            assembler.mov(kTmp1, mem(Reg::Rsp, slotOffset(depth - 1)));
            assembler.test(kTmp1, kTmp1);
            assembler.jcc(Cond::Equal, divZeroExit);

            if (inst.op == OpCode::Mod) {
                // `x % -1` is 0 mathematically, and the interpreter returns 0 rather than
                // dividing. Doing the same avoids the one input `idiv` traps on.
                assembler.cmpImm(kTmp1, -1);
                auto notMinusOne = assembler.newLabel();
                assembler.jcc(Cond::NotEqual, notMinusOne);
                assembler.movImm(kRegA, Width::Qword, 0);
                assembler.jmp(divDone);
                assembler.bind(notMinusOne);
            } else if (bits == 64) {
                // `INT64_MIN / -1` overflows; x86 raises #DE and the process dies instead
                // of trapping through the VM. Checked before the divide for that reason.
                assembler.movImm(kTmp2, Width::Qword,
                                 std::numeric_limits<std::int64_t>::min());
                assembler.cmp(kRegA, kTmp2, Width::Qword);
                auto notMin = assembler.newLabel();
                assembler.jcc(Cond::NotEqual, notMin);
                assembler.cmpImm(kTmp1, -1);
                assembler.jcc(Cond::Equal, trapExit);
                assembler.bind(notMin);
            }

            if (bits == 32) {
                assembler.cdq();
                assembler.idiv(kTmp1, Width::Dword);
            } else {
                assembler.cqo();
                assembler.idiv(kTmp1, Width::Qword);
            }
            if (inst.op == OpCode::Mod) {
                assembler.mov(kRegA, Reg::Rdx);
                if (bits == 32) {
                    assembler.movsxd(kRegA, kRegA);
                }
            } else if (bits == 32) {
                assembler.movsxd(kRegA, kRegA);
            }
            checkFitsWidth(bits);
            assembler.bind(divDone);

            --depth;
            assembler.mov(mem(Reg::Rsp, slotOffset(depth - 1)), kRegA);
            break;
        }

        case OpCode::Eq:
        case OpCode::Ne:
        case OpCode::Lt:
        case OpCode::Le:
        case OpCode::Gt:
        case OpCode::Ge: {
            const IntKind kind = intKindOf(inst.a);
            if (kind == IntKind::Unsupported) {
                refusal = "comparison on an unsupported type";
                break;
            }
            if (depth < 2) {
                refusal = "stack underflow in a comparison";
                break;
            }
            // rhs into rax, lhs into rcx, then `cmp lhs, rhs` -- the interpreter's order,
            // because `a < b` and `b < a` differ by which register is the rm operand.
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth - 2)));
            assembler.mov(kArg1, mem(Reg::Rsp, slotOffset(depth - 1)));
            assembler.cmp(kRegA, kArg1, Width::Qword);

            const bool isUnsigned = kind == IntKind::Unsigned;
            Cond cond = Cond::Equal;
            switch (inst.op) {
            case OpCode::Eq: cond = Cond::Equal; break;
            case OpCode::Ne: cond = Cond::NotEqual; break;
            case OpCode::Lt: cond = isUnsigned ? Cond::Below : Cond::Less; break;
            case OpCode::Le: cond = isUnsigned ? Cond::BelowEqual : Cond::LessEqual; break;
            case OpCode::Gt: cond = isUnsigned ? Cond::Above : Cond::Greater; break;
            case OpCode::Ge: cond = isUnsigned ? Cond::AboveEqual : Cond::GreaterEqual; break;
            default: break;
            }
            // setcc writes a byte register; al needs no REX, and rax is dead once the
            // comparison has consumed both operands.
            assembler.setcc(cond, Reg::Rax);
            assembler.movzx(kRegA, kRegA, Width::Byte);
            --depth;
            assembler.mov(mem(Reg::Rsp, slotOffset(depth - 1)), kRegA);
            break;
        }

        case OpCode::LocalAddr: {
            // The address of a frame slot. Axiom's `return` goes through this: the value
            // is written to frame+0 and the epilogue loads it back.
            assembler.lea(kRegA, mem(kRegBase, inst.a));
            pushA();
            break;
        }

        case OpCode::Dup: {
            if (depth == 0) {
                refusal = "stack underflow in DUP";
                break;
            }
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth - 1)));
            pushA();
            break;
        }

        case OpCode::Pop: {
            if (depth == 0) {
                refusal = "stack underflow in POP";
                break;
            }
            --depth;
            break;
        }

        case OpCode::Swap: {
            if (depth < 2) {
                refusal = "stack underflow in SWAP";
                break;
            }
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth - 1)));
            assembler.mov(kTmp1, mem(Reg::Rsp, slotOffset(depth - 2)));
            assembler.mov(mem(Reg::Rsp, slotOffset(depth - 1)), kTmp1);
            assembler.mov(mem(Reg::Rsp, slotOffset(depth - 2)), kRegA);
            break;
        }

        case OpCode::Load: {
            if (!slotIsSupportedInteger(inst.a)) {
                refusal = "load of an unsupported type";
                break;
            }
            if (depth == 0) {
                refusal = "stack underflow in LOAD";
                break;
            }
            if (depth >= kMaxValueSlots) {
                refusal = "expression stack deeper than the JIT supports";
                break;
            }
            // loadThroughPointer(VM&, address, typeIndex, int64& out) -> bool
            // r10 holds the address: rcx is the first argument slot and cannot also
            // carry it, which was the first bug this path had.
            assembler.mov(Reg::R10, mem(Reg::Rsp, slotOffset(depth - 1)));
            assembler.test(Reg::R10, Reg::R10);
            assembler.jcc(Cond::Equal, nullExit);
            assembler.lea(Reg::R9, mem(Reg::Rsp, slotOffset(depth - 1)));
            assembler.mov(Reg::Rcx, kRegVm);
            assembler.mov(Reg::Rdx, Reg::R10);
            assembler.movImm(Reg::R8, Width::Dword, inst.a);
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::loadThroughPointer));
            assembler.test(kRegA, kRegA);
            assembler.jcc(Cond::Equal, failureExit);
            break;
        }

        case OpCode::Store:
        case OpCode::StoreKeep: {
            if (!slotIsSupportedInteger(inst.a)) {
                refusal = "store of an unsupported type";
                break;
            }
            if (depth < 2) {
                refusal = "stack underflow in STORE";
                break;
            }
            // storeThroughPointer(VM&, address, typeIndex, value) -> bool
            assembler.mov(Reg::R9, mem(Reg::Rsp, slotOffset(depth - 1)));
            assembler.mov(Reg::R10, mem(Reg::Rsp, slotOffset(depth - 2)));
            assembler.test(Reg::R10, Reg::R10);
            assembler.jcc(Cond::Equal, nullExit);
            assembler.mov(Reg::Rcx, kRegVm);
            assembler.mov(Reg::Rdx, Reg::R10);
            assembler.movImm(Reg::R8, Width::Dword, inst.a);
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::storeThroughPointer));
            assembler.test(kRegA, kRegA);
            assembler.jcc(Cond::Equal, failureExit);
            depth -= 2;
            if (inst.op == OpCode::StoreKeep) {
                // The value is still sitting in its slot; no register needs to survive
                // the call. Copies it down one slot and pushes.
                assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth + 1)));
                pushA();
            }
            break;
        }

        case OpCode::Jump: {
            assembler.jmp(labelFor(inst.a));
            break;
        }

        case OpCode::JumpIfFalse:
        case OpCode::JumpIfTrue: {
            if (depth == 0) {
                refusal = "stack underflow in a conditional jump";
                break;
            }
            --depth;
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth)));
            assembler.test(kRegA, kRegA);
            const Cond taken = inst.op == OpCode::JumpIfTrue ? Cond::NotEqual : Cond::Equal;
            assembler.jcc(taken, labelFor(inst.a));
            break;
        }

        case OpCode::Ret: {
            if (function.returnsValue) {
                if (depth == 0) {
                    refusal = "missing return value";
                    break;
                }
                --depth;
                assembler.mov(Reg::R8, mem(Reg::Rsp, slotOffset(depth)));
            }
            assembler.mov(Reg::Rcx, kRegProcess);
            assembler.movImm(Reg::Rdx, Width::Dword, function.returnsValue ? 1 : 0);
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::returnToCaller));
            assembler.movImm(kRegA, Width::Qword, 1);
            assembler.jmp(failureExit);
            emittedReturn = true;
            break;
        }

        case OpCode::Halt: {
            assembler.mov(Reg::Rcx, kRegProcess);
            assembler.movImm(Reg::Rdx, Width::Dword, 0);
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::returnToCaller));
            assembler.movImm(kRegA, Width::Qword, 1);
            assembler.jmp(failureExit);
            emittedReturn = true;
            break;
        }

        case OpCode::SafePoint:
            refusal = "safe point (scheduling is not compiled)";
            break;

        default:
            refusal = std::string("unsupported opcode ") +
                      std::string(opCodeName(inst.op));
            break;
        }
    }

    if (!refusal.empty()) {
        whyNot = "in '" + function.name + "': " + refusal;
        return nullptr;
    }

    // Reaching the end of the code without a `Ret` is what the interpreter treats as the
    // end of the function; do the same rather than returning whatever rax happens to hold.
    if (!emittedReturn) {
        if (depth != 0 || function.returnsValue) {
            whyNot = "function falls off the end with a value still on the stack";
            return nullptr;
        }
        assembler.mov(Reg::Rcx, kRegProcess);
        assembler.movImm(Reg::Rdx, Width::Dword, 0);
        assembler.callAbsolute(
            reinterpret_cast<const void*>(&jitbridge::returnToCaller));
        assembler.movImm(kRegA, Width::Qword, 1);
    }

    // trapExit publishes the message and forces the failure result; failureExit is the
    // single epilogue both paths share.
    // Each trap block ends with a jump: without one it falls through into the next and
    // the last message wins. That is how an int32 overflow first reported itself as a
    // null pointer -- the message was overwritten on the way out, while the trap itself
    // was correct.
    auto emitTrapBlock = [&](X64Assembler::Label label, const char* message) {
        assembler.bind(label);
        assembler.mov(Reg::Rcx, kRegError);
        assembler.movImm(Reg::Rdx, Width::Qword,
                         static_cast<std::int64_t>(
                             reinterpret_cast<std::uintptr_t>(message)));
        assembler.callAbsolute(
            reinterpret_cast<const void*>(&jitbridge::setTrapMessage));
        assembler.movImm(kRegA, Width::Qword, 0);
        assembler.jmp(failureExit);
    };

    emitTrapBlock(trapExit, kOverflowMessage);
    emitTrapBlock(divZeroExit, kDivideByZeroMessage);
    emitTrapBlock(nullExit, kNullPointerMessage);

    assembler.bind(failureExit);
    assembler.addImm(Reg::Rsp, static_cast<std::int32_t>(kFrameBytes));
    assembler.pop(Reg::R14);
    assembler.pop(Reg::R13);
    assembler.pop(Reg::R12);
    assembler.pop(Reg::Rbx);
    assembler.ret();

    auto block = assembler.finalize();
    if (!block) {
        whyNot = "assembler could not produce executable code "
                 "(an unbound label, or the platform refused the mapping)";
        return nullptr;
    }
    return block;
}

} // namespace acvm

namespace acvm {

JitFunction JitCache::entryFor(const Module& module, std::size_t functionIndex,
                               std::string& whyNot) {
    if (mode == JitMode::Off) {
        return nullptr;
    }
    if (functionIndex >= module.functions.size()) {
        return nullptr;
    }
    if (m_entries.size() < module.functions.size()) {
        m_entries.resize(module.functions.size());
    }

    Entry& entry = m_entries[functionIndex];
    if (entry.code != nullptr) {
        return entry.code;
    }
    if (entry.refused) {
        return nullptr;
    }

    std::string reason;
    auto block = compileFunction(module, module.functions[functionIndex], reason);
    if (!block) {
        entry.refused = true;
        ++m_refused;
        if (whyNot.empty()) {
            whyNot = std::move(reason);
        }
        return nullptr;
    }

    entry.code = reinterpret_cast<JitFunction>(block->entry());
    m_blocks.push_back(std::move(block));
    ++m_compiled;
    return entry.code;
}

} // namespace acvm
