//===--- Jit.cpp - lowering bytecode to x86-64 ---------------------------===//
//
// The JIT compiles a whole function or refuses it. Refusal is a normal outcome: the
// interpreter keeps running it and the program behaves identically either way. That
// all-or-nothing rule is what keeps this pass reviewable -- a half-compiled function would
// have to flush every live register back to memory at the unsupported instruction and
// reload afterwards, which is where a JIT stops being simple and starts producing wrong
// answers at the edges.
//
// Generated shape:
//
//   push  rbx, rbp, r12..r15     everything the ABI says is ours to save
//   sub   rsp, kFrameBytes
//   mov   rbx, rcx               VM*
//   mov   r14, rdx               Process*
//   mov   r12, r8                frame base, invariant for the call
//   mov   r13, r9                std::string* receiving a trap message
//   mov   r15, r8 & 0xFFFF       the frame's offset within its page
//
//   ... body; page hosts in the page table, values in 8 byte slots ...
//
//   add   rsp, kFrameBytes
//   pop   r15..rbx
//   ret
//
// Two decisions explain most of the code below.
//
// **Slot access does not call the interpreter.** The first version routed every local
// variable read through the VM's own `loadScalar`, which made the compiled path apply
// exactly the interpreter's width and sign rules -- good for reaching correctness, bad for
// speed, because that call re-derives the memory type, the access width and the sign
// extension on every access, none of which changes within a function. The type is known at
// compile time, so the width and the extension are emitted as instructions, and the page's
// host pointer is pinned once at entry. Pages are never reclaimed or moved, which is what
// makes holding that pointer for the function's lifetime sound.
//
// **Every access keeps a guard.** A frame slot can straddle a page boundary depending on
// where `stackTop` happened to be, which is not known at compile time. The fast path
// assumes it does not; a compare sends the rare straddling access to the helper call. That
// guard is the price of not being able to assume frames are page-relative, and they are
// not: a frame base is whatever the previous frame ended at.

#include "acvm/VM/Jit.h"

#include "acvm/JIT/X64Assembler.h"
#include "acvm/VM/VM.h"

#include <limits>
#include <string>
#include <vector>

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

void returnToCaller(void* process, bool hasResult, std::int64_t result,
                    bool resultIsPtr) {
    VM::JitBridge::returnToCaller(process, hasResult, result, resultIsPtr);
}

bool readRaw64(VM& vm, std::uint64_t address, std::int64_t& out) {
    return VM::JitBridge::readRaw64(vm, address, out);
}

int pinFramePages(VM& vm, std::uint64_t frameBase, std::uint64_t frameSize,
                  void** out, int maxPages) {
    return VM::JitBridge::pinFramePages(vm, frameBase, frameSize, out, maxPages);
}

bool constantString(VM& vm, int index, std::int64_t& out) {
    return VM::JitBridge::constantString(vm, index, out);
}

void zeroThroughPointer(VM& vm, std::uint64_t address, std::uint64_t size) {
    VM::JitBridge::zeroThroughPointer(vm, address, size);
}

void copyThroughPointers(VM& vm, std::uint64_t destination, std::uint64_t source,
                         std::uint64_t size) {
    VM::JitBridge::copyThroughPointers(vm, destination, source, size);
}

bool callFunction(VM& vm, void* process, int functionIndex, int argc,
                  std::string* error) {
    return VM::JitBridge::callFunction(vm, process, functionIndex, argc, error);
}

bool pushValue(void* process, std::int64_t value, bool isPtr) {
    return VM::JitBridge::pushValue(process, value, isPtr);
}

int safePoint(VM& vm, void* process, std::uint64_t nextIp, std::string* error) {
    return VM::JitBridge::safePoint(vm, process, nextIp, error);
}

void bailToInterpreter(void* process, std::uint64_t ip) {
    VM::JitBridge::bailToInterpreter(process, ip);
}

bool consumeBudget(VM& vm, std::uint64_t amount) {
    return VM::JitBridge::consumeBudget(vm, amount);
}

bool popResult(void* process, std::int64_t* out, bool isPtr) {
    return VM::JitBridge::popResult(process, out, isPtr);
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
using jit::memIndex;
using jit::Reg;
using jit::Width;
using jit::X64Assembler;

constexpr std::size_t kMaxValueSlots = 48;
constexpr std::size_t kMaxPinnedPages = 8;
constexpr std::size_t kSlotBytes = 8;
constexpr std::size_t kShadowSpace = 32;
constexpr std::size_t kPageTableOffset = kShadowSpace;
constexpr std::size_t kValueBase = kPageTableOffset + kMaxPinnedPages * 8;
/// One slot past the value stack, for parking a value across the address computation --
/// which clobbers every scratch register, so a value cannot be kept in one.
constexpr std::size_t kScratchSlot = kMaxValueSlots;
/// Counts down a chunk of the runaway-loop budget. Kept in the frame rather than in a
/// register so it survives the calls a loop body makes.
constexpr std::size_t kBudgetSlot = kMaxValueSlots + 1;
constexpr std::int32_t kBudgetChunk = 4096;

/// The prologue pushes six registers, so rsp is 8 mod 16 when the frame is subtracted.
/// Subtracting a size that is also 8 mod 16 lands rsp on a 16 byte boundary, which the ABI
/// requires at every call site: a misaligned stack breaks SSE spills in unrelated code and
/// shows up as a crash far from here.
constexpr std::size_t kFrameBytes =
    ((kValueBase + (kMaxValueSlots + 1) * kSlotBytes + 8 + 15) / 16) * 16 + 8;
static_assert(kFrameBytes % 16 == 8, "the frame must leave rsp 16 byte aligned");

constexpr Reg kRegVm = Reg::Rbx;
constexpr Reg kRegProcess = Reg::R14;
constexpr Reg kRegBase = Reg::R12;
constexpr Reg kRegError = Reg::R13;
/// `frameBase & 0xFFFF`, needed by every slot access.
constexpr Reg kRegPageOffset = Reg::R15;

constexpr Reg kArg1 = Reg::Rcx;
constexpr Reg kArg2 = Reg::Rdx;
constexpr Reg kArg3 = Reg::R8;
constexpr Reg kArg4 = Reg::R9;

constexpr Reg kTmp1 = Reg::R10;
constexpr Reg kTmp2 = Reg::R11;
constexpr Reg kRegA = Reg::Rax;

std::int32_t slotOffset(std::size_t index) {
    return static_cast<std::int32_t>(kValueBase + index * kSlotBytes);
}

constexpr const char kOverflowMessage[] = "integer overflow in compiled code";
constexpr const char kNullPointerMessage[] = "null pointer in compiled code";
constexpr const char kDivideByZeroMessage[] = "division by zero in compiled code";
constexpr const char kBudgetMessage[] =
    "instruction budget exhausted (possible infinite loop)";

/// The integer properties the lowering needs, resolved once per memory type.
struct IntInfo {
    bool supported = false;
    bool isUnsigned = false;
    unsigned bits = 0;
};

IntInfo intInfoOf(const Module& module, int typeIndex) {
    IntInfo info;
    if (typeIndex < 0 || static_cast<std::size_t>(typeIndex) >= module.memTypes.size()) {
        return info;
    }
    const MemType& type = module.memTypes[static_cast<std::size_t>(typeIndex)];
    if (type.isPtr()) {
        // A pointer is a 64 bit unsigned value that never traps on arithmetic. Treating it
        // as its own kind would only add branches: pointer arithmetic in Axiom is
        // `base + index`, and the interpreter wraps it rather than trapping.
        info.bits = 64;
        info.isUnsigned = true;
        info.supported = true;
        return info;
    }
    if (!type.isInteger()) {
        return info;
    }
    info.bits = type.bits == 0 ? static_cast<unsigned>(type.size * 8) : type.bits;
    if (info.bits > 64 || info.bits == 0) {
        return info;
    }
    info.isUnsigned = type.isUnsigned;
    info.supported = true;
    return info;
}

/// True when values of this type carry the Ptr tag rather than the Int tag. Strings and
/// pointers share a payload but not a tag, and a string printed as an integer prints
/// nothing -- which is what a lost tag looks like from the outside.
bool isPointerType(const Module& module, int typeIndex) {
    if (typeIndex < 0 || static_cast<std::size_t>(typeIndex) >= module.memTypes.size()) {
        return false;
    }
    return module.memTypes[static_cast<std::size_t>(typeIndex)].isPtr();
}

Width widthOfBits(unsigned bits) {
    if (bits <= 8) {
        return Width::Byte;
    }
    if (bits <= 16) {
        return Width::Word;
    }
    if (bits <= 32) {
        return Width::Dword;
    }
    return Width::Qword;
}

/// Truncates rax to the declared width and extends it back out the way the type reads.
///
/// Signed narrow types hold their value sign-extended; unsigned ones hold it zero-extended.
/// Using the signed form for both turns an `unsigned int32` result of 0x80000000 into
/// 0xFFFFFFFF80000000, and the next comparison or shift is then wrong.
void emitTruncateToWidth(X64Assembler& assembler, unsigned bits, bool isUnsigned) {
    if (bits >= 64) {
        return;
    }
    if (isUnsigned) {
        assembler.movImm(Reg::R11, Width::Qword,
                         static_cast<std::int64_t>((static_cast<std::uint64_t>(1) << bits) - 1));
        assembler.and_(Reg::Rax, Reg::R11, Width::Qword);
        return;
    }
    assembler.shl(Reg::Rax, static_cast<std::uint8_t>(64 - bits));
    assembler.sar(Reg::Rax, static_cast<std::uint8_t>(64 - bits));
}

/// The range test the interpreter applies after every signed operation: the result must
/// survive sign extension from the declared width. A 64 bit overflow flag alone does not
/// notice an `int32` sum that left its type.
void emitWidthCheck(X64Assembler& assembler, X64Assembler::Label trapExit,
                    unsigned bits) {
    if (bits >= 64) {
        return;
    }
    if (bits == 32) {
        assembler.movsxd(Reg::R11, Reg::Rax);
    } else if (bits == 16) {
        assembler.movsx(Reg::R11, Reg::Rax, Width::Word);
    } else {
        assembler.movsx(Reg::R11, Reg::Rax, Width::Byte);
    }
    assembler.cmp(Reg::R11, Reg::Rax, Width::Qword);
    assembler.jcc(Cond::NotEqual, trapExit);
}

} // namespace

std::unique_ptr<jit::ExecutableBlock> compileFunction(const Module& module,
                                                     const Function& function,
                                                     std::string& whyNot) {
    if (!kJitSupported) {
        // First check, so the reason names the real cause on any host that cannot run the
        // output. Every other refusal below would be about the program, not the machine.
        whyNot = "no JIT for this platform (the backend emits x86-64 Microsoft ABI code)";
        return nullptr;
    }
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
    // A process body must yield at its safe point and compiled code does not yield, so
    // leaving these interpreted is what keeps the scheduler able to preempt them.
    if (function.isProcess) {
        whyNot = "process body (must remain preemptible)";
        return nullptr;
    }
    // The frame's pages are pinned once, at entry, into a table the whole body addresses
    // through -- so the frame has to fit in the table. Worst case the frame starts one byte
    // before a page boundary, so a frame of N bytes spans at most N/65536 + 1 pages.
    //
    // Refusing here rather than letting `pinFramePages` fail at run time: that failure came
    // out as `runtime error: ` with an empty message, because the exit path had nothing to
    // say. A function with a frame in this range is one with enormous stack arrays, which is
    // not the shape the JIT is for anyway.
    {
        constexpr std::uint64_t kPageBytes = 65536;
        const std::uint64_t worstCasePages = function.frameSize / kPageBytes + 1;
        if (worstCasePages > kMaxPinnedPages) {
            whyNot = "in '" + function.name + "': frame of " +
                     std::to_string(function.frameSize) +
                     " bytes spans more pages than the JIT pins";
            return nullptr;
        }
    }


    const std::size_t codeSize = function.code.size();
    constexpr std::size_t kNoLabel = std::numeric_limits<std::size_t>::max();

    X64Assembler assembler;
    X64Assembler::Label trapExit = assembler.newLabel();
    X64Assembler::Label nullExit = assembler.newLabel();
    X64Assembler::Label divZeroExit = assembler.newLabel();
    X64Assembler::Label failureExit = assembler.newLabel();
    X64Assembler::Label budgetExit = assembler.newLabel();

    assembler.push(Reg::Rbx);
    assembler.push(Reg::Rbp);
    assembler.push(Reg::R12);
    assembler.push(Reg::R13);
    assembler.push(Reg::R14);
    assembler.push(Reg::R15);
    assembler.subImm(Reg::Rsp, static_cast<std::int32_t>(kFrameBytes));
    assembler.mov(kRegVm, kArg1);
    assembler.mov(kRegProcess, kArg2);
    assembler.mov(kRegBase, kArg3);
    assembler.mov(kRegError, kArg4);

    assembler.movImm(kRegA, Width::Dword, kBudgetChunk);
    assembler.mov(mem(Reg::Rsp, slotOffset(kBudgetSlot)), kRegA);

    assembler.mov(kRegPageOffset, kRegBase);
    assembler.movImm(kTmp1, Width::Qword, 0xFFFF);
    assembler.and_(kRegPageOffset, kTmp1, Width::Qword);

    // Pin every page the frame touches: one call for the whole function. The pointers stay
    // valid because a page is never reclaimed or moved once it exists.
    {
        assembler.mov(kArg1, kRegVm);
        assembler.mov(kArg2, kRegBase);
        assembler.movImm(kArg3, Width::Qword,
                         static_cast<std::int64_t>(function.frameSize));
        assembler.lea(kArg4, mem(Reg::Rsp, static_cast<std::int32_t>(kPageTableOffset)));
        assembler.movImm(kRegA, Width::Dword, static_cast<std::int32_t>(kMaxPinnedPages));
        // The fifth argument goes on the stack, at [rsp+32] from the callee's view: 32
        // bytes of shadow space first. 48 rather than 40 keeps rsp 16 byte aligned, which
        // the ABI requires at every call.
        assembler.subImm(Reg::Rsp, 48);
        assembler.mov(mem(Reg::Rsp, 32), kRegA);
        assembler.callAbsolute(reinterpret_cast<const void*>(&jitbridge::pinFramePages));
        assembler.addImm(Reg::Rsp, 48);
        assembler.test(kRegA, kRegA, Width::Byte);
        assembler.jcc(Cond::Equal, failureExit);
    }

    std::vector<X64Assembler::Label> labels(codeSize, kNoLabel);
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

    // -- stack depth per instruction --------------------------------------
    //
    // Not trackable linearly. A ternary merges two arms, so walking instructions in order
    // counts both pushes and the merge point looks one deep too many. This is a forward
    // pass over the control flow graph; two edges disagreeing means the bytecode is not
    // something this level can compile, and it is refused rather than guessed.
    constexpr int kUnknown = -1;
    constexpr int kPoisoned = -2;
    // Back edges -- a jump to an earlier instruction -- are where unbounded execution
    // happens, so they are where the runaway-loop budget is charged.
    std::vector<bool> isBackEdge(codeSize, false);
    for (std::size_t i = 0; i < codeSize; ++i) {
        const Instruction& candidate = function.code[i];
        if ((candidate.op == OpCode::Jump || candidate.op == OpCode::JumpIfFalse ||
             candidate.op == OpCode::JumpIfTrue) &&
            candidate.a >= 0 && static_cast<std::size_t>(candidate.a) <= i) {
            isBackEdge[i] = true;
        }
    }

    // Filled in by the hand-off analysis, which needs `effectOf` and therefore lives
    // inside the depth pass below.
    std::size_t bailIp = std::numeric_limits<std::size_t>::max();

    std::vector<int> depthAt(codeSize, kUnknown);
    depthAt[0] = 0;
    {
        auto effectOf = [&](const Instruction& inst, int& pops, int& pushes) -> bool {
            pops = 0;
            pushes = 0;
            switch (inst.op) {
            case OpCode::ConstInt: case OpCode::ConstFloat: case OpCode::ConstStr:
            case OpCode::LocalAddr: case OpCode::GlobalAddr: case OpCode::LoadLocal:
            case OpCode::LoadGlobal: case OpCode::DerefLocal: case OpCode::ArenaMark:
                pushes = 1;
                return true;
            case OpCode::Load: pops = 1; pushes = 1; return true;
            case OpCode::MemZero: pops = 1; return true;
            case OpCode::MemCopy: pops = 2; return true;
            case OpCode::Store: pops = 2; return true;
            case OpCode::StoreKeep: pops = 1; return true;
            case OpCode::StoreLocal: case OpCode::StoreGlobal:
                // `b` holds the memory type in the low byte and a keep bit at 0x100:
                // with the bit set the stored value stays on the stack, which is how
                // `i++` yields the old value. Modelling the two alike made valid
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
                pops = 2; pushes = 1; return true;
            case OpCode::Neg: case OpCode::BitNot: case OpCode::Not: case OpCode::Cast:
                pops = 1; pushes = 1; return true;
            case OpCode::StepLocal: case OpCode::CopyStep: return true;
            case OpCode::JumpIfFalse: case OpCode::JumpIfTrue: pops = 1; return true;
            case OpCode::Jump: case OpCode::Halt: case OpCode::Trap: return true;
            case OpCode::SafePoint:
                // No stack effect, and it *does* flow on. Leaving it out of this switch
                // made everything after the first safe point unreachable in the analysis
                // -- and IRGen emits a safe point before every call, so that was every
                // function containing a call: its epilogue stopped being emitted, and the
                // function fell off the end with a value on the stack.
                return true;
            case OpCode::Ret: pops = function.returnsValue ? 1 : 0; return true;
            case OpCode::Builtin: return false;
            case OpCode::Call:
                pops = inst.b < 0 ? 0 : inst.b;
                if (inst.a >= 0 &&
                    static_cast<std::size_t>(inst.a) < module.functions.size()) {
                    const Function& callee =
                        module.functions[static_cast<std::size_t>(inst.a)];
                    if (callee.returnsAggregate) {
                        pops += 1;
                    }
                    // An aggregate return pops a destination *and* pushes a result: `Ret`
                    // pushes the returned value whatever its type, and the value it pushes
                    // for an aggregate is a pointer to that destination, which is what the
                    // MEM_COPY after the call copies from. Modelling the push as zero made
                    // every `T x = f();` where `f` returns a struct or string look like it
                    // underflowed two instructions later -- so the refusal named the wrong
                    // instruction, and the real reason (an aggregate return the lowering
                    // does not implement) was nowhere in the message.
                    pushes = callee.returnsValue ? 1 : 0;
                }
                return true;
            default:
                return false;
            }
        };

        std::vector<std::size_t> work{0};
        while (!work.empty()) {
            const std::size_t ip = work.back();
            work.pop_back();
            if (depthAt[ip] < 0) {
                continue;
            }
            const Instruction& inst = function.code[ip];
            int pops = 0;
            int pushes = 0;
            if (!effectOf(inst, pops, pushes)) {
                continue;
            }
            const int after = depthAt[ip] - pops + pushes;
            if (after < 0) {
                whyNot = "in '" + function.name + "': expression stack underflow at " +
                         std::to_string(ip);
                return nullptr;
            }
            auto reach = [&](std::size_t target) {
                if (target >= codeSize) {
                    return;
                }
                if (depthAt[target] == kUnknown) {
                    depthAt[target] = after;
                    work.push_back(target);
                } else if (depthAt[target] != after) {
                    depthAt[target] = kPoisoned;
                    work.push_back(target);
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
        for (std::size_t i = 0; i < codeSize; ++i) {
            if (depthAt[i] == kPoisoned) {
                whyNot = "in '" + function.name + "': control flow merges with two "
                         "different stack depths at " + std::to_string(i);
                return nullptr;
            }
        }

        // -- where to hand the frame back to the interpreter -------------------
        //
        // A builtin (`print`, `atoi`, ...) and a call into a shared library are executed
        // by the interpreter, which is the only implementation of them. Refusing the
        // whole function for that was the biggest reason real programs stayed
        // interpreted: across `examples/`, 146 of 381 functions compiled and `print` was
        // the most common refusal -- and the refusal counted functions whose hot loop had
        // nothing to do with it.
        //
        // Instead the body is compiled *up to* the first instruction the lowering cannot
        // emit, and execution falls back there with the frame untouched. The restart
        // point must have an empty expression stack: a compiled function keeps operands
        // in frame slots with no type tags, and the interpreter reads `process.stack`, so
        // an empty stack is the only state that needs no translation. That is a real
        // limit -- `print` in the middle of an expression is not a restart point -- and a
        // function with no empty-stack instruction before its first such instruction is
        // still refused, exactly as it was before.
        for (std::size_t i = 0; i < codeSize; ++i) {
            if (depthAt[i] < 0) {
                continue;
            }
            const Instruction& candidate = function.code[i];
            int pops = 0;
            int pushes = 0;
            bool needsInterpreter = !effectOf(candidate, pops, pushes);
            switch (candidate.op) {
            // `effectOf` knows these have a defined stack effect but the lowering has no
            // case for them, so without this they fall to the `default:` refusal instead
            // of becoming a hand-off point. Every one of them is safe to hand over
            // *before*: none has partially run by then, and the interpreter then does
            // exactly what it would have done in a fully interpreted programme -- which
            // is what keeps the differential test meaningful, trap messages included.
            //
            // This list has to stay in step with the lowering's switch. A new opcode that
            // `effectOf` models but the lowering does not emit belongs here, and the
            // symptom of forgetting is a refusal naming it rather than anything subtler.
            case OpCode::Trap:
            case OpCode::ConstFloat:
            case OpCode::ArenaMark:
            case OpCode::CopyStep:
            case OpCode::GlobalAddr:
            case OpCode::LoadGlobal:
            case OpCode::StoreGlobal:
                needsInterpreter = true;
                break;
            default:
                break;
            }
            if (!needsInterpreter && candidate.op == OpCode::Call && candidate.a >= 0 &&
                static_cast<std::size_t>(candidate.a) < module.functions.size()) {
                needsInterpreter =
                    module.functions[static_cast<std::size_t>(candidate.a)].isExternal();
            }
            if (!needsInterpreter) {
                continue;
            }
            // Walk back to the nearest instruction that starts with an empty stack. A
            // jump target in between ends the search: another path arrives there, so
            // stepping over it would change what that path executes.
            std::size_t at = i;
            bool found = false;
            while (true) {
                if (depthAt[at] == 0) {
                    found = true;
                    break;
                }
                if (at == 0 || labels[at] != kNoLabel) {
                    break;
                }
                --at;
            }
            if (!found) {
                whyNot = "in '" + function.name + "': needs the interpreter at " +
                         std::to_string(i) +
                         " and no empty-stack instruction precedes it";
                return nullptr;
            }
            if (bailIp == std::numeric_limits<std::size_t>::max() || at < bailIp) {
                bailIp = at;
            }
        }
        // Handing over at the first instruction would compile a prologue and nothing
        // else, which is the interpreter's work plus the cost of compiling it.
        if (bailIp == 0) {
            bailIp = std::numeric_limits<std::size_t>::max();
            whyNot = "in '" + function.name +
                     "': needs the interpreter at its first instruction";
            return nullptr;
        }
    }

    std::size_t depth = 0;
    std::string refusal;
    bool emittedReturn = false;
    // Which jump targets have been bound. An early hand-off to the interpreter leaves
    // targets past it unbound, and `finalize` rejects a block with an unbound label.
    std::vector<bool> labelBound(codeSize, false);

    auto pushA = [&]() {
        if (depth >= kMaxValueSlots) {
            refusal = "expression stack deeper than the JIT supports";
            return false;
        }
        assembler.mov(mem(Reg::Rsp, slotOffset(depth)), kRegA);
        ++depth;
        return true;
    };

    /// Leaves the address of `frame + offset` in kTmp1, or jumps to `slow` when the slot
    /// straddles a page boundary and the address cannot be formed from a pinned page.
    ///
    /// Clobbers rax, r10 and r11 -- every scratch register there is. A caller that needs a
    /// value to survive has to park it in the scratch slot; keeping it in a register is
    /// what made `i++` store 0xFFFF, the mask this uses, instead of the value.
    auto emitSlotAddress = [&](std::int32_t offset, unsigned bits,
                               X64Assembler::Label slow) {
        const unsigned bytes = (bits + 7) / 8;
        assembler.lea(kTmp1, mem(kRegPageOffset, offset));
        assembler.mov(kTmp2, kTmp1);
        assembler.movImm(kRegA, Width::Qword, 0xFFFF);
        assembler.and_(kTmp2, kRegA, Width::Qword);
        assembler.shr(kTmp1, 16, Width::Qword);
        assembler.mov(kTmp1, memIndex(Reg::Rsp, kTmp1, 8,
                                     static_cast<std::int32_t>(kPageTableOffset)));
        assembler.cmpImm(kTmp2, static_cast<std::int32_t>(65536 - bytes));
        assembler.jcc(Cond::Above, slow);
        assembler.lea(kTmp1, memIndex(kTmp1, kTmp2, 1, 0));
    };

    auto emitHelperLoad = [&](std::int32_t offset, int typeIndex,
                              std::size_t targetSlot) {
        assembler.mov(kArg1, kRegVm);
        assembler.lea(kArg2, mem(kRegBase, offset));
        assembler.movImm(kArg3, Width::Dword, typeIndex);
        assembler.lea(kArg4, mem(Reg::Rsp, slotOffset(targetSlot)));
        assembler.callAbsolute(reinterpret_cast<const void*>(&jitbridge::loadSlot));
        assembler.test(kRegA, kRegA, Width::Byte);
        assembler.jcc(Cond::Equal, failureExit);
    };

    /// Charges the runaway-loop budget once per chunk instead of once per iteration: a
    /// decremented frame slot, one predictable branch, and a call only every 4096 loops.
    /// The interpreter charges one per instruction, which a compiled loop cannot afford;
    /// this keeps the same guard at a fixed cost per iteration.
    auto emitBudgetTick = [&]() {
        assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(kBudgetSlot)));
        assembler.subImm(kRegA, 1);
        assembler.mov(mem(Reg::Rsp, slotOffset(kBudgetSlot)), kRegA);
        auto enough = assembler.newLabel();
        assembler.test(kRegA, kRegA, Width::Byte);
        assembler.jcc(Cond::NotEqual, enough);
        assembler.movImm(kRegA, Width::Dword, kBudgetChunk);
        assembler.mov(mem(Reg::Rsp, slotOffset(kBudgetSlot)), kRegA);
        assembler.mov(kArg1, kRegVm);
        assembler.movImm(kArg2, Width::Qword, kBudgetChunk);
        assembler.callAbsolute(
            reinterpret_cast<const void*>(&jitbridge::consumeBudget));
        assembler.test(kRegA, kRegA, Width::Byte);
        assembler.jcc(Cond::NotEqual, budgetExit);
        assembler.bind(enough);
    };

    /// Loads `frame + offset` at the type's width into kRegA, honouring the declared sign.
    auto emitScalarLoad = [&](std::int32_t offset, unsigned bits, bool isUnsigned,
                              X64Assembler::Label slow) {
        emitSlotAddress(offset, bits, slow);
        const Width width = widthOfBits(bits);
        if (width == Width::Qword) {
            assembler.mov(kRegA, mem(kTmp1, 0), Width::Qword);
        } else if (isUnsigned) {
            assembler.movzx(kRegA, mem(kTmp1, 0), width);
        } else {
            assembler.movsx(kRegA, mem(kTmp1, 0), width);
        }
    };

    for (std::size_t ip = 0; ip < codeSize && refusal.empty(); ++ip) {
        if (labels[ip] != kNoLabel) {
            assembler.bind(labels[ip]);
            labelBound[ip] = true;
        }
        // Everything from here on is the interpreter's, so this is a return as far as the
        // compiled body is concerned: emit the hand-off and stop lowering. Instructions
        // past this point may not even have a known stack depth, which is why they must
        // not be reached by falling through.
        if (ip == bailIp) {
            assembler.mov(kArg1, kRegProcess);
            assembler.movImm(kArg2, Width::Qword, static_cast<std::int64_t>(ip));
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::bailToInterpreter));
            assembler.movImm(kRegA, Width::Dword, 3);
            assembler.jmp(failureExit);
            emittedReturn = true;
            break;
        }
        if (depthAt[ip] < 0) {
            continue;   // unreachable, or past an instruction whose effect is unknown
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

        case OpCode::ConstStr: {
            // The string table is laid out once, by `prepare()`, and never moves -- so
            // this is a lookup, not an allocation, and the address stays valid for as
            // long as the process runs. The helper returns the same descriptor address
            // the interpreter's CONST_STR pushes, which is what keeps the two paths from
            // disagreeing about where a literal lives.
            //
            // Refusing this opcode is what kept most real programs out of the JIT: a
            // single string in a function -- an unused label, a format for a message --
            // sent the whole function back to the interpreter, and compilation is all or
            // nothing per function. It was the most common refusal by a wide margin.
            if (inst.a < 0 ||
                static_cast<std::size_t>(inst.a) >= module.strings.size()) {
                refusal = "string index out of range";
                break;
            }
            if (depth >= kMaxValueSlots) {
                refusal = "expression stack deeper than the JIT supports";
                break;
            }
            assembler.mov(kArg1, kRegVm);
            assembler.movImm(kArg2, Width::Dword, inst.a);
            assembler.lea(kArg3, mem(Reg::Rsp, slotOffset(depth)));
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::constantString));
            assembler.test(kRegA, kRegA, Width::Byte);
            assembler.jcc(Cond::Equal, failureExit);
            ++depth;
            break;
        }

        case OpCode::MemZero: {
            // Pops a destination and zeroes `a` bytes through it, exactly as the
            // interpreter does -- including its null check, which applies only when there
            // is something to write. Zeroing nothing is a no-op, so that case emits no
            // code at all rather than a call that would return immediately.
            //
            // This matters more than it looks: declaring `string s = "...";` zeroes the
            // local's slot before storing, so a function whose only string is an unused
            // label needs both this and CONST_STR to compile. With CONST_STR done and this
            // missing, the refusal merely moved one opcode along.
            const std::int32_t bytes = inst.a > 0 ? inst.a : 0;
            if (depth < 1) {
                refusal = "stack underflow in MEM_ZERO";
                break;
            }
            --depth;
            if (bytes == 0) {
                break;
            }
            assembler.mov(kArg2, mem(Reg::Rsp, slotOffset(depth)));
            assembler.test(kArg2, kArg2, Width::Qword);
            assembler.jcc(Cond::Equal, nullExit);
            assembler.mov(kArg1, kRegVm);
            assembler.movImm(kArg3, Width::Qword, bytes);
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::zeroThroughPointer));
            break;
        }

        case OpCode::MemCopy: {
            // `destination` was pushed first, so it sits one slot below the top. Both
            // addresses get the interpreter's null check before anything is written --
            // order matters, because the interpreter rejects the pair before it copies.
            const std::int32_t bytes = inst.a > 0 ? inst.a : 0;
            if (depth < 2) {
                refusal = "stack underflow in MEM_COPY";
                break;
            }
            if (bytes == 0) {
                depth -= 2;
                break;
            }
            assembler.mov(kArg2, mem(Reg::Rsp, slotOffset(depth - 1)));
            assembler.mov(kArg3, mem(Reg::Rsp, slotOffset(depth - 2)));
            assembler.test(kArg2, kArg2, Width::Qword);
            assembler.jcc(Cond::Equal, nullExit);
            assembler.test(kArg3, kArg3, Width::Qword);
            assembler.jcc(Cond::Equal, nullExit);
            assembler.mov(kArg1, kRegVm);
            assembler.movImm(kArg4, Width::Qword, bytes);
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::copyThroughPointers));
            depth -= 2;
            break;
        }

        case OpCode::LoadLocal: {
            const IntInfo info = intInfoOf(module, inst.b & 0xFF);
            if (!info.supported) {
                refusal = "local variable of a type the JIT does not inline";
                break;
            }
            if (depth >= kMaxValueSlots) {
                refusal = "expression stack deeper than the JIT supports";
                break;
            }
            auto slow = assembler.newLabel();
            auto done = assembler.newLabel();
            emitScalarLoad(inst.a, info.bits, info.isUnsigned, slow);
            assembler.mov(mem(Reg::Rsp, slotOffset(depth)), kRegA);
            assembler.jmp(done);
            assembler.bind(slow);
            emitHelperLoad(inst.a, inst.b & 0xFF, depth);
            assembler.bind(done);
            ++depth;
            break;
        }

        case OpCode::StoreLocal: {
            const IntInfo info = intInfoOf(module, inst.b & 0xFF);
            if (!info.supported) {
                refusal = "local variable of a type the JIT does not inline";
                break;
            }
            if (depth == 0) {
                refusal = "stack underflow in STORE_LOCAL";
                break;
            }
            // The value is always the top of stack. With the keep bit set it also stays
            // there afterwards, so only the bookkeeping differs -- reading from an
            // already-decremented slot instead stored an empty one, which is how `a++`
            // came out as 1 rather than 7.
            const bool keepValue = (inst.b & 0x100) != 0;
            const std::size_t valueSlot = depth - 1;
            if (!keepValue) {
                --depth;
            }
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(valueSlot)));
            assembler.mov(mem(Reg::Rsp, slotOffset(kScratchSlot)), kRegA);

            auto slow = assembler.newLabel();
            auto done = assembler.newLabel();
            emitSlotAddress(inst.a, info.bits, slow);
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(kScratchSlot)));
            assembler.mov(mem(kTmp1, 0), kRegA, widthOfBits(info.bits));
            assembler.jmp(done);
            assembler.bind(slow);
            assembler.mov(kArg1, kRegVm);
            assembler.lea(kArg2, mem(kRegBase, inst.a));
            assembler.movImm(kArg3, Width::Dword, inst.b & 0xFF);
            assembler.mov(kArg4, mem(Reg::Rsp, slotOffset(valueSlot)));
            assembler.callAbsolute(reinterpret_cast<const void*>(&jitbridge::storeSlot));
            assembler.test(kRegA, kRegA, Width::Byte);
            assembler.jcc(Cond::Equal, failureExit);
            assembler.bind(done);
            break;
        }

        case OpCode::DerefLocal: {
            // A frame slot holding a pointer, then a load through it: how an array
            // element reaches its value, so it is on the hot path of anything indexing a
            // local array.
            const IntInfo info = intInfoOf(module, inst.b);
            if (!info.supported) {
                refusal = "array element of a type the JIT does not inline";
                break;
            }
            if (depth >= kMaxValueSlots) {
                refusal = "expression stack deeper than the JIT supports";
                break;
            }
            // The pointer itself lives in a frame slot, so it uses the pinned page path.
            auto slowPointer = assembler.newLabel();
            auto afterPointer = assembler.newLabel();
            emitSlotAddress(inst.a, 64, slowPointer);
            assembler.mov(kTmp2, mem(kTmp1, 0), Width::Qword);
            assembler.jmp(afterPointer);
            assembler.bind(slowPointer);
            assembler.mov(kArg1, kRegVm);
            assembler.lea(kArg2, mem(kRegBase, inst.a));
            assembler.lea(kArg3, mem(Reg::Rsp, slotOffset(depth)));
            assembler.callAbsolute(reinterpret_cast<const void*>(&jitbridge::readRaw64));
            assembler.test(kRegA, kRegA, Width::Byte);
            assembler.jcc(Cond::Equal, failureExit);
            assembler.mov(kTmp2, mem(Reg::Rsp, slotOffset(depth)));
            assembler.bind(afterPointer);

            assembler.test(kTmp2, kTmp2);
            assembler.jcc(Cond::Equal, nullExit);
            assembler.mov(kArg1, kRegVm);
            assembler.mov(kArg2, kTmp2);
            assembler.movImm(kArg3, Width::Dword, inst.b);
            assembler.lea(kArg4, mem(Reg::Rsp, slotOffset(depth)));
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::loadThroughPointer));
            assembler.test(kRegA, kRegA, Width::Byte);
            assembler.jcc(Cond::Equal, failureExit);
            ++depth;
            break;
        }

        case OpCode::StepLocal: {
            // `i++` as one instruction: read, add a small constant, write back.
            const int typeIndex = inst.b & 0xFF;
            const IntInfo info = intInfoOf(module, typeIndex);
            if (!info.supported) {
                refusal = "step on a type the JIT does not inline";
                break;
            }
            const std::int32_t delta = inst.b >> 8;
            auto slow = assembler.newLabel();
            auto done = assembler.newLabel();
            emitScalarLoad(inst.a, info.bits, info.isUnsigned, slow);
            assembler.addImm(kRegA, delta, Width::Qword);
            if (!info.isUnsigned) {
                assembler.jcc(Cond::Overflow, trapExit);
                emitWidthCheck(assembler, trapExit, info.bits);
            } else if (info.bits < 64) {
                assembler.movImm(kTmp1, Width::Qword,
                                 static_cast<std::int64_t>(
                                     (static_cast<std::uint64_t>(1) << info.bits) - 1));
                assembler.mov(kTmp2, kRegA);
                assembler.and_(kTmp2, kTmp1, Width::Qword);
                assembler.cmp(kTmp2, kRegA, Width::Qword);
                assembler.jcc(Cond::NotEqual, trapExit);
            }
            assembler.mov(mem(Reg::Rsp, slotOffset(kScratchSlot)), kRegA);
            emitSlotAddress(inst.a, info.bits, slow);
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(kScratchSlot)));
            assembler.mov(mem(kTmp1, 0), kRegA, widthOfBits(info.bits));
            assembler.jmp(done);
            assembler.bind(slow);
            // Straddling: let the interpreter do the whole step, then read it back.
            assembler.mov(kArg1, kRegVm);
            assembler.lea(kArg2, mem(kRegBase, inst.a));
            assembler.movImm(kArg3, Width::Dword, typeIndex);
            assembler.mov(kArg4, mem(Reg::Rsp, slotOffset(depth)));
            assembler.callAbsolute(reinterpret_cast<const void*>(&jitbridge::loadSlot));
            assembler.bind(done);
            break;
        }

        case OpCode::Add: case OpCode::Sub: case OpCode::Mul:
        case OpCode::And: case OpCode::Or: case OpCode::Xor:
        case OpCode::Shl: case OpCode::Shr:
        case OpCode::Neg: case OpCode::Not: case OpCode::BitNot: {
            const IntInfo info = intInfoOf(module, inst.a);
            if (!info.supported) {
                refusal = "arithmetic on a type the JIT does not inline";
                break;
            }
            if (depth == 0) {
                refusal = "stack underflow in arithmetic";
                break;
            }
            if (inst.op == OpCode::Neg || inst.op == OpCode::Not ||
                inst.op == OpCode::BitNot) {
                assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth - 1)));
                if (inst.op == OpCode::Neg) {
                    assembler.neg(kRegA, Width::Qword);
                    assembler.jcc(Cond::Overflow, trapExit);
                    if (!info.isUnsigned) {
                        emitWidthCheck(assembler, trapExit, info.bits);
                    }
                } else if (inst.op == OpCode::Not) {
                    // Logical negation, not a bitwise complement: the interpreter pushes
                    // `truthy ? 0 : 1`, so the result is always exactly 0 or 1. Sharing the
                    // BitNot path here made `!x` return ~x -- nonzero for every false x, so
                    // `if (!ok)` took the wrong branch and the JIT silently disagreed with
                    // the interpreter on every program that negated a condition.
                    assembler.test(kRegA, kRegA, Width::Qword);
                    assembler.setcc(Cond::Equal, kRegA);
                    assembler.movzx(kRegA, kRegA, Width::Byte);
                } else {
                    assembler.not_(kRegA, Width::Qword);
                    if (info.bits < 64) {
                        emitTruncateToWidth(assembler, info.bits, info.isUnsigned);
                    }
                }
                assembler.mov(mem(Reg::Rsp, slotOffset(depth - 1)), kRegA);
                break;
            }
            if (depth < 2) {
                refusal = "stack underflow in arithmetic";
                break;
            }
            // lhs into rax, rhs into r11. Subtraction is order-sensitive, and swapping
            // these turns `a - b` into `b - a` -- invisible for `+` and `*`, wrong here.
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth - 2)));
            assembler.mov(kTmp2, mem(Reg::Rsp, slotOffset(depth - 1)));
            switch (inst.op) {
            case OpCode::Add: assembler.add(kRegA, kTmp2); break;
            case OpCode::Sub: assembler.sub(kRegA, kTmp2); break;
            case OpCode::Mul: assembler.imul(kRegA, kTmp2); break;
            case OpCode::And: assembler.and_(kRegA, kTmp2); break;
            case OpCode::Or: assembler.or_(kRegA, kTmp2); break;
            case OpCode::Xor: assembler.xor_(kRegA, kTmp2); break;
            case OpCode::Shl:
            case OpCode::Shr: {
                // A runtime shift count goes in cl -- the instruction names that register
                // implicitly, so the count has to be *moved* there. The first version put
                // it in r10 and the shift used whatever cl happened to hold, which is how
                // SHA-256 produced plausible-looking but wrong digests.
                assembler.mov(kArg1, kTmp2);
                if (inst.op == OpCode::Shl) {
                    assembler.shlCl(kRegA);
                } else if (info.isUnsigned) {
                    assembler.shrCl(kRegA);
                } else {
                    assembler.sarCl(kRegA);
                }
                break;
            }
            default: break;
            }
            if (inst.op == OpCode::Add || inst.op == OpCode::Sub ||
                inst.op == OpCode::Mul) {
                if (info.isUnsigned) {
                    // Unsigned arithmetic wraps at the declared width and never traps, so
                    // the result has to be truncated -- leaving the 64 bit sum in place is
                    // what made a 32 bit LCG produce a 64 bit state and a different
                    // sequence from the interpreter.
                    emitTruncateToWidth(assembler, info.bits, true);
                } else {
                    assembler.jcc(Cond::Overflow, trapExit);
                    emitWidthCheck(assembler, trapExit, info.bits);
                }
            } else if (info.bits < 64) {
                // Bitwise results and shifts have to stay inside the declared width, and
                // the interpreter masks them there. Without this an `int8` or `int32`
                // bitwise result keeps its high bits and later comparisons disagree.
                emitTruncateToWidth(assembler, info.bits, info.isUnsigned);
            }
            --depth;
            assembler.mov(mem(Reg::Rsp, slotOffset(depth - 1)), kRegA);
            break;
        }

        case OpCode::Div:
        case OpCode::Mod: {
            const IntInfo info = intInfoOf(module, inst.a);
            if (!info.supported) {
                refusal = "division on a type the JIT does not inline";
                break;
            }
            if (info.isUnsigned) {
                refusal = "unsigned division";
                break;
            }
            if (depth < 2) {
                refusal = "stack underflow in division";
                break;
            }
            auto divDone = assembler.newLabel();
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth - 2)));
            // The divisor goes to r10, never rdx: `cqo` writes rdx and `idiv` reads
            // rax:rdx, so a divisor left there would divide by its own sign.
            assembler.mov(kTmp1, mem(Reg::Rsp, slotOffset(depth - 1)));
            assembler.test(kTmp1, kTmp1);
            assembler.jcc(Cond::Equal, divZeroExit);

            if (inst.op == OpCode::Mod) {
                // `x % -1` is 0 mathematically, and the interpreter returns 0 rather than
                // dividing -- which also avoids the one input `idiv` traps on.
                auto notMinusOne = assembler.newLabel();
                assembler.cmpImm(kTmp1, -1);
                assembler.jcc(Cond::NotEqual, notMinusOne);
                assembler.movImm(kRegA, Width::Qword, 0);
                assembler.jmp(divDone);
                assembler.bind(notMinusOne);
            } else if (info.bits == 64) {
                // `INT64_MIN / -1` raises #DE, which kills the process instead of
                // trapping through the VM. Checked before the divide for that reason.
                auto notMin = assembler.newLabel();
                assembler.movImm(kTmp2, Width::Qword,
                                 std::numeric_limits<std::int64_t>::min());
                assembler.cmp(kRegA, kTmp2, Width::Qword);
                assembler.jcc(Cond::NotEqual, notMin);
                assembler.cmpImm(kTmp1, -1);
                assembler.jcc(Cond::Equal, trapExit);
                assembler.bind(notMin);
            }

            if (info.bits == 32) {
                assembler.cdq();
                assembler.idiv(kTmp1, Width::Dword);
            } else {
                assembler.cqo();
                assembler.idiv(kTmp1, Width::Qword);
            }
            if (inst.op == OpCode::Mod) {
                assembler.mov(kRegA, Reg::Rdx);
                if (info.bits == 32) {
                    assembler.movsxd(kRegA, kRegA);
                }
            } else if (info.bits == 32) {
                assembler.movsxd(kRegA, kRegA);
            }
            emitWidthCheck(assembler, trapExit, info.bits);
            assembler.bind(divDone);
            --depth;
            assembler.mov(mem(Reg::Rsp, slotOffset(depth - 1)), kRegA);
            break;
        }

        case OpCode::Eq: case OpCode::Ne: case OpCode::Lt:
        case OpCode::Le: case OpCode::Gt: case OpCode::Ge: {
            const IntInfo info = intInfoOf(module, inst.a);
            if (!info.supported) {
                refusal = "comparison on a type the JIT does not inline";
                break;
            }
            if (depth < 2) {
                refusal = "stack underflow in a comparison";
                break;
            }
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth - 2)));
            assembler.mov(kTmp2, mem(Reg::Rsp, slotOffset(depth - 1)));
            assembler.cmp(kRegA, kTmp2, Width::Qword);
            const bool u = info.isUnsigned;
            Cond cond = Cond::Equal;
            switch (inst.op) {
            case OpCode::Eq: cond = Cond::Equal; break;
            case OpCode::Ne: cond = Cond::NotEqual; break;
            case OpCode::Lt: cond = u ? Cond::Below : Cond::Less; break;
            case OpCode::Le: cond = u ? Cond::BelowEqual : Cond::LessEqual; break;
            case OpCode::Gt: cond = u ? Cond::Above : Cond::Greater; break;
            case OpCode::Ge: cond = u ? Cond::AboveEqual : Cond::GreaterEqual; break;
            default: break;
            }
            assembler.setcc(cond, Reg::Rax);
            assembler.movzx(kRegA, kRegA, Width::Byte);
            --depth;
            assembler.mov(mem(Reg::Rsp, slotOffset(depth - 1)), kRegA);
            break;
        }

        case OpCode::Cast: {
            const IntInfo from = intInfoOf(module, inst.a);
            const IntInfo to = intInfoOf(module, inst.b);
            if (!from.supported || !to.supported) {
                refusal = "cast between types the JIT does not inline";
                break;
            }
            if (depth == 0) {
                refusal = "stack underflow in CAST";
                break;
            }
            // Widening is a no-op on a value already sign- or zero-extended into 64 bits;
            // narrowing keeps the low bits, which is what the interpreter's mask does.
            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth - 1)));
            if (to.isUnsigned) {
                if (to.bits < 64) {
                    assembler.movImm(kTmp1, Width::Qword,
                                     static_cast<std::int64_t>(
                                         (static_cast<std::uint64_t>(1) << to.bits) - 1));
                    assembler.and_(kRegA, kTmp1, Width::Qword);
                }
            } else if (to.bits < 64) {
                emitTruncateToWidth(assembler, to.bits, false);
            }
            assembler.mov(mem(Reg::Rsp, slotOffset(depth - 1)), kRegA);
            break;
        }

        case OpCode::LocalAddr: {
            // Fuse `LOCAL_ADDR k` with the access that consumes it.
            //
            // IRGen emits the *address* of a local for every store and for the generic
            // pointer load, and `for i in 0..n` is built almost entirely from those two
            // pairs -- 216 `LOCAL_ADDR; SWAP; STORE` and 112 `LOCAL_ADDR; LOAD` across
            // the example programs counted. Each one went through
            // `jitbridge::loadThroughPointer`, which is a C++ call, a memType lookup and
            // a 32 byte `Value` construction, per local variable access. Measured on a
            // 20 000 000 iteration range loop: 4.7x, against 21x for the same loop
            // written with LOAD_LOCAL. The whole difference was this.
            //
            // Safe because the address is dead the moment the access happens: nothing
            // can observe it between the two instructions, and a jump target in the
            // middle would be the only way another path could arrive there. A slot that
            // straddles a page still takes the bridge, so the page-boundary rule is
            // unchanged.
            if (ip + 1 < codeSize && labels[ip + 1] == kNoLabel) {
                const Instruction& next = function.code[ip + 1];
                if (next.op == OpCode::Load) {
                    const IntInfo info = intInfoOf(module, next.a);
                    if (info.supported && depth < kMaxValueSlots) {
                        auto slow = assembler.newLabel();
                        auto done = assembler.newLabel();
                        emitScalarLoad(inst.a, info.bits, info.isUnsigned, slow);
                        assembler.mov(mem(Reg::Rsp, slotOffset(depth)), kRegA);
                        assembler.jmp(done);
                        assembler.bind(slow);
                        emitHelperLoad(inst.a, next.a, depth);
                        assembler.bind(done);
                        ++depth;
                        ++ip;   // the LOAD has been emitted
                        break;
                    }
                } else if (next.op == OpCode::Swap && ip + 2 < codeSize &&
                           labels[ip + 2] == kNoLabel && depth >= 1) {
                    const Instruction& store = function.code[ip + 2];
                    if (store.op == OpCode::Store) {
                        const IntInfo info = intInfoOf(module, store.a);
                        if (info.supported) {
                            // The value being stored sits one slot below the address
                            // that SWAP would have moved it above; parking it in the
                            // scratch slot first is what `STORE_LOCAL` does too, because
                            // forming the address clobbers every scratch register.
                            const std::size_t valueSlot = depth - 1;
                            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(valueSlot)));
                            assembler.mov(mem(Reg::Rsp, slotOffset(kScratchSlot)), kRegA);

                            auto slow = assembler.newLabel();
                            auto done = assembler.newLabel();
                            emitSlotAddress(inst.a, info.bits, slow);
                            assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(kScratchSlot)));
                            assembler.mov(mem(kTmp1, 0), kRegA, widthOfBits(info.bits));
                            assembler.jmp(done);
                            assembler.bind(slow);
                            assembler.mov(kArg1, kRegVm);
                            assembler.lea(kArg2, mem(kRegBase, inst.a));
                            assembler.movImm(kArg3, Width::Dword, store.a);
                            assembler.mov(kArg4, mem(Reg::Rsp, slotOffset(valueSlot)));
                            assembler.callAbsolute(
                                reinterpret_cast<const void*>(&jitbridge::storeSlot));
                            assembler.test(kRegA, kRegA, Width::Byte);
                            assembler.jcc(Cond::Equal, failureExit);
                            assembler.bind(done);

                            depth -= 1;   // the address and the value both go away
                            ip += 2;
                            break;
                        }
                    }
                }
            }
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
            const IntInfo info = intInfoOf(module, inst.a);
            if (!info.supported) {
                refusal = "load of a type the JIT does not inline";
                break;
            }
            if (depth == 0) {
                refusal = "stack underflow in LOAD";
                break;
            }
            assembler.mov(kTmp1, mem(Reg::Rsp, slotOffset(depth - 1)));
            assembler.test(kTmp1, kTmp1);
            assembler.jcc(Cond::Equal, nullExit);
            assembler.mov(kArg1, kRegVm);
            assembler.mov(kArg2, kTmp1);
            assembler.movImm(kArg3, Width::Dword, inst.a);
            assembler.lea(kArg4, mem(Reg::Rsp, slotOffset(depth - 1)));
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::loadThroughPointer));
            assembler.test(kRegA, kRegA, Width::Byte);
            assembler.jcc(Cond::Equal, failureExit);
            break;
        }

        case OpCode::Store:
        case OpCode::StoreKeep: {
            const IntInfo info = intInfoOf(module, inst.a);
            if (!info.supported) {
                refusal = "store of a type the JIT does not inline";
                break;
            }
            if (depth < 2) {
                refusal = "stack underflow in STORE";
                break;
            }
            assembler.mov(kArg4, mem(Reg::Rsp, slotOffset(depth - 1)));
            assembler.mov(kTmp1, mem(Reg::Rsp, slotOffset(depth - 2)));
            assembler.test(kTmp1, kTmp1);
            assembler.jcc(Cond::Equal, nullExit);
            assembler.mov(kArg1, kRegVm);
            assembler.mov(kArg2, kTmp1);
            assembler.movImm(kArg3, Width::Dword, inst.a);
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::storeThroughPointer));
            assembler.test(kRegA, kRegA, Width::Byte);
            assembler.jcc(Cond::Equal, failureExit);
            if (inst.op == OpCode::StoreKeep) {
                // Both operands leave the stack and the value is pushed back, so the net
                // effect is one slot still holding it.
                assembler.mov(kRegA, mem(Reg::Rsp, slotOffset(depth - 1)));
                --depth;
                assembler.mov(mem(Reg::Rsp, slotOffset(depth - 1)), kRegA);
            } else {
                depth -= 2;
            }
            break;
        }

        case OpCode::Call: {
            if (inst.a < 0 ||
                static_cast<std::size_t>(inst.a) >= module.functions.size()) {
                refusal = "invalid call target";
                break;
            }
            const Function& callee = module.functions[static_cast<std::size_t>(inst.a)];
            // Arguments cross through the interpreter's value stack: recorded here into
            // `process.stack`, popped by `callFunction`, which then dispatches to the
            // callee's compiled entry when it has one and to `step()` when it does not.
            // The interop carries aggregates (a string arrives as the address of its
            // descriptor, which is why the push below tags it) -- getting that tag wrong
            // is what made a callee read an empty string.
            //
            // A `library` call never reaches here: its arguments are marshalled by the
            // interpreter's external-call path, which the bridge does not reproduce, so the
            // hand-off analysis makes it a bail point before lowering gets this far.
            if (callee.returnsAggregate) {
                refusal = "call returning an aggregate";
                break;
            }
            const int argc = inst.b < 0 ? 0 : inst.b;
            if (depth < static_cast<std::size_t>(argc)) {
                refusal = "stack underflow in CALL";
                break;
            }
            // The interpreter's CALL reads its arguments from the value stack, so they
            // have to be moved there. They are already contiguous in the native frame.
            const std::size_t firstArgument = depth - static_cast<std::size_t>(argc);
            for (int i = 0; i < argc; ++i) {
                // An aggregate parameter (a string is one) arrives as the *address* of
                // its descriptor, so it carries the pointer tag too -- pushing it as an
                // integer made the callee read an empty string.
                const ParamInfo* param =
                    i < static_cast<int>(callee.params.size())
                        ? &callee.params[static_cast<std::size_t>(i)]
                        : nullptr;
                const bool argumentIsPtr =
                    param != nullptr &&
                    (param->aggregate || isPointerType(module, param->memType));
                assembler.mov(kRegA,
                              mem(Reg::Rsp, slotOffset(firstArgument +
                                                       static_cast<std::size_t>(i))));
                assembler.mov(kArg1, kRegProcess);
                assembler.mov(kArg2, kRegA);
                assembler.movImm(kArg3, Width::Dword, argumentIsPtr ? 1 : 0);
                assembler.callAbsolute(
                    reinterpret_cast<const void*>(&jitbridge::pushValue));
            }
            assembler.mov(kArg1, kRegVm);
            assembler.mov(kArg2, kRegProcess);
            assembler.movImm(kArg3, Width::Dword, inst.a);
            assembler.movImm(kArg4, Width::Dword, argc);
            assembler.subImm(Reg::Rsp, 48);
            assembler.mov(mem(Reg::Rsp, 32), kRegError);   // fifth argument
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::callFunction));
            assembler.addImm(Reg::Rsp, 48);
            assembler.test(kRegA, kRegA, Width::Byte);
            assembler.jcc(Cond::Equal, failureExit);
            depth = firstArgument;
            if (callee.returnsValue) {
                if (depth >= kMaxValueSlots) {
                    refusal = "expression stack deeper than the JIT supports";
                    break;
                }
                assembler.mov(kArg1, kRegProcess);
                assembler.lea(kArg2, mem(Reg::Rsp, slotOffset(depth)));
                assembler.movImm(kArg3, Width::Dword,
                                 isPointerType(module, callee.returnMemType) ? 1 : 0);
                assembler.callAbsolute(
                    reinterpret_cast<const void*>(&jitbridge::popResult));
                assembler.test(kRegA, kRegA, Width::Byte);
                assembler.jcc(Cond::Equal, failureExit);
                ++depth;
            }
            break;
        }

        case OpCode::Jump: {
            if (isBackEdge[ip]) {
                emitBudgetTick();
            }
            assembler.jmp(labels[static_cast<std::size_t>(inst.a)]);
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
            assembler.test(kRegA, kRegA, Width::Byte);
            const Cond taken = inst.op == OpCode::JumpIfTrue ? Cond::NotEqual : Cond::Equal;
            if (isBackEdge[ip]) {
                // Only the taken branch loops, so the charge goes on that path.
                auto skip = assembler.newLabel();
                assembler.jcc(taken == Cond::NotEqual ? Cond::Equal : Cond::NotEqual, skip);
                emitBudgetTick();
                assembler.jmp(labels[static_cast<std::size_t>(inst.a)]);
                assembler.bind(skip);
            } else {
                assembler.jcc(taken, labels[static_cast<std::size_t>(inst.a)]);
            }
            break;
        }

        case OpCode::Ret: {
            // returnToCaller(process, hasResult, result, resultIsPtr): r8 carries the
            // value, r9 the tag flag. The value goes straight from its slot into r8 --
            // there is nothing to compute -- so no scratch register is involved.
            if (function.returnsValue) {
                if (depth == 0) {
                    refusal = "missing return value";
                    break;
                }
                --depth;
                assembler.mov(kArg3, mem(Reg::Rsp, slotOffset(depth)));
            }
            assembler.mov(kArg1, kRegProcess);
            assembler.movImm(kArg2, Width::Dword, function.returnsValue ? 1 : 0);
            assembler.movImm(kArg4, Width::Dword,
                             isPointerType(module, function.returnMemType) ? 1 : 0);
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::returnToCaller));
            assembler.movImm(kRegA, Width::Qword, 1);
            assembler.jmp(failureExit);
            emittedReturn = true;
            break;
        }

        case OpCode::Halt: {
            assembler.mov(kArg1, kRegProcess);
            assembler.movImm(kArg2, Width::Dword, 0);
            assembler.movImm(kArg3, Width::Qword, 0);
            assembler.movImm(kArg4, Width::Dword, 0);
            assembler.callAbsolute(
                reinterpret_cast<const void*>(&jitbridge::returnToCaller));
            assembler.movImm(kRegA, Width::Qword, 1);
            assembler.jmp(failureExit);
            emittedReturn = true;
            break;
        }

        case OpCode::SafePoint: {
            // A yield is only safe with an empty expression stack. The compiled body keeps
            // its operands in frame slots, but the interpreter resumes at `ip + 1` from
            // `process.stack` -- which knows nothing about them. IRGen emits a safe point
            // before *every* call, precisely where the arguments are already pushed, so
            // yielding there handed the interpreter an empty value stack and the CALL it
            // resumed on popped from it: "stack underflow in CALL". A single-process
            // program never yielded, which is why this only appeared once calls were
            // compiled inside a program that also ran a second process.
            //
            // The event phase runs at every safe point, yield or not, because the
            // interpreter runs it at every safe point. Only the *yield* needs an empty
            // expression stack -- the interpreter resumes at `ip + 1` from
            // `process.stack`, which knows nothing about this function's frame slots.
            //
            // Resume point for the interpreter if we yield; the interpreter's own handler
            // has already advanced past the instruction when it writes ip back.
            assembler.mov(kArg1, kRegVm);
            assembler.mov(kArg2, kRegProcess);
            assembler.movImm(kArg3, Width::Qword, static_cast<std::int64_t>(ip + 1));
            assembler.mov(kArg4, kRegError);
            assembler.callAbsolute(reinterpret_cast<const void*>(&jitbridge::safePoint));

            auto after = assembler.newLabel();
            auto isTrap = assembler.newLabel();
            // 2 -- an event handler trapped; `*error` already says what.
            assembler.cmpImm(kRegA, 2);
            assembler.jcc(Cond::Equal, isTrap);
            // 1 -- a yield was asked for. Honoured only with an empty stack; skipped
            // otherwise, which costs preemption granularity and nothing else.
            if (depth != 0) {
                assembler.jmp(after);
            } else {
                assembler.test(kRegA, kRegA, Width::Byte);
                assembler.jcc(Cond::Equal, after);
                assembler.movImm(kRegA, Width::Dword, 2);
                assembler.jmp(failureExit);
            }
            assembler.bind(isTrap);
            assembler.movImm(kRegA, Width::Dword, 0);
            assembler.jmp(failureExit);
            assembler.bind(after);
            break;
        }

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

    // Anything the early hand-off left behind needs its own hand-off, not a shared
    // unreachable address. A conditional jump whose target is past `bailIp` -- the
    // false branch of `if (rare) { print(...); }`, say -- arrives at that target
    // directly, and binding it to code that falls into the epilogue would return a
    // garbage result code. Each stub names its own ip, so the interpreter resumes at
    // the instruction the compiled path was about to run.
    for (std::size_t i = 0; i < codeSize; ++i) {
        if (labels[i] == kNoLabel || labelBound[i]) {
            continue;
        }
        assembler.bind(labels[i]);
        assembler.mov(kArg1, kRegProcess);
        assembler.movImm(kArg2, Width::Qword, static_cast<std::int64_t>(i));
        assembler.callAbsolute(reinterpret_cast<const void*>(&jitbridge::bailToInterpreter));
        assembler.movImm(kRegA, Width::Dword, 3);
        assembler.jmp(failureExit);
    }

    if (!emittedReturn) {
        if (depth != 0 || function.returnsValue) {
            whyNot = "in '" + function.name +
                     "': falls off the end with a value still on the stack";
            return nullptr;
        }
        assembler.mov(kArg1, kRegProcess);
        assembler.movImm(kArg2, Width::Dword, 0);
        assembler.movImm(kArg3, Width::Qword, 0);
        assembler.movImm(kArg4, Width::Dword, 0);
        assembler.callAbsolute(
            reinterpret_cast<const void*>(&jitbridge::returnToCaller));
        assembler.movImm(kRegA, Width::Qword, 1);
    }

    // Each trap block ends with a jump. Without one it falls through into the next and the
    // last message wins -- which is how an integer overflow once reported itself as a null
    // pointer while the trap itself was correct.
    auto emitTrapBlock = [&](X64Assembler::Label label, const char* message) {
        assembler.bind(label);
        assembler.mov(kArg1, kRegError);
        assembler.movImm(kArg2, Width::Qword,
                         static_cast<std::int64_t>(
                             reinterpret_cast<std::uintptr_t>(message)));
        assembler.callAbsolute(
            reinterpret_cast<const void*>(&jitbridge::setTrapMessage));
        assembler.movImm(kRegA, Width::Qword, 0);
        assembler.jmp(failureExit);
    };
    assembler.bind(budgetExit);
    assembler.mov(kArg1, kRegError);
    assembler.movImm(kArg2, Width::Qword,
                     static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(
                         kBudgetMessage)));
    assembler.callAbsolute(reinterpret_cast<const void*>(&jitbridge::setTrapMessage));
    assembler.movImm(kRegA, Width::Dword, 0);
    assembler.jmp(failureExit);

    emitTrapBlock(trapExit, kOverflowMessage);
    emitTrapBlock(divZeroExit, kDivideByZeroMessage);
    emitTrapBlock(nullExit, kNullPointerMessage);

    assembler.bind(failureExit);
    assembler.addImm(Reg::Rsp, static_cast<std::int32_t>(kFrameBytes));
    assembler.pop(Reg::R15);
    assembler.pop(Reg::R14);
    assembler.pop(Reg::R13);
    assembler.pop(Reg::R12);
    assembler.pop(Reg::Rbp);
    assembler.pop(Reg::Rbx);
    assembler.ret();

    auto block = assembler.finalize();
    if (!block) {
        whyNot = "in '" + function.name + "': the assembler could not produce "
                 "executable code";
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
        // The reason does not change between calls -- an unsupported opcode stays
        // unsupported -- so a refusal is permanent and is not retried.
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
