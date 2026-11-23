//===--- Optimize.cpp - the four optimization levels --------------------===//
//
// Everything here works on the bytecode `IRGen` produced. The VM is a stack
// machine, so most of what a register allocator would do for a normal compiler
// is not needed -- but the stack form is also where the wins are: a value pushed
// and immediately popped, an address computed only to be dereferenced, a
// constant that reaches a branch.
//
// ── the invariant every pass preserves ──────────────────────────────────────
//
// A `SafePoint` is where the scheduler runs `event` conditions and where
// `process` switches. The instructions between two safe points are an
// observable atomicity boundary (spec §5.8), so:
//
//   * no pass removes a SafePoint;
//   * no pass folds, forwards or reorders anything *across* one.
//
// That is why `isBarrier` exists and why the constant folder refuses to look
// through an instruction that can observe the stack.
//
// The second invariant is about traps: the VM traps on integer overflow and on
// division by zero. A fold that would compute such a value must not happen --
// the trap has to stay where the programmer can see it. `tryFold` returns false
// in exactly those cases, and the original three instructions stay.

#include "acvm/IR/Optimize.h"

#include "acvm/IR/IntRangeMemType.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <vector>

namespace acvm {

namespace {

//===--- helpers ---------------------------------------------------------===//

bool isJump(OpCode op) {
    return op == OpCode::Jump || op == OpCode::JumpIfFalse || op == OpCode::JumpIfTrue;
}

/// True when control does not fall through to the next instruction. `Trap`
/// counts: the VM never returns from it.
bool endsBlock(OpCode op) {
    return op == OpCode::Jump || op == OpCode::Ret || op == OpCode::Halt || op == OpCode::Trap;
}

/// Instructions that can observe or change machine state beyond the value stack.
/// Nothing may be folded, forwarded or deleted across one of these.
bool isBarrier(OpCode op) {
    switch (op) {
    case OpCode::SafePoint:
    case OpCode::Call:
    case OpCode::Builtin:
    case OpCode::Store:
    case OpCode::StoreLocal:
    case OpCode::StoreGlobal:
    case OpCode::MemCopy:
    case OpCode::MemZero:
    case OpCode::ArenaRelease:
    case OpCode::CopyStep:
    case OpCode::DerefLocal:
        return true;
    default:
        return false;
    }
}

/// A push with no observable effect when its result is immediately discarded.
bool isPurePush(OpCode op) {
    return op == OpCode::ConstInt || op == OpCode::ConstFloat || op == OpCode::ConstStr ||
           op == OpCode::LocalAddr || op == OpCode::GlobalAddr || op == OpCode::Dup;
}

const Value* constantAt(const Function& fn, std::int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= fn.constants.size()) {
        return nullptr;
    }
    return &fn.constants[static_cast<std::size_t>(index)];
}

/// Keep-bit of a fused store's `b` operand.
constexpr std::int32_t kKeepBit = 0x100;

/// Marks every instruction a jump can land on.
///
/// **Nothing may be deleted or rewritten at a marked index.** A jump asserts that
/// the stack has a particular shape when it arrives; deleting the instruction it
/// targets removes the push that shape depends on, and the result is a stack
/// underflow that only shows up on the second time round a loop.
///
/// This is not hypothetical: forwarding `StoreLocal x; LoadLocal x` into a single
/// keeping store deleted the load that a loop's back edge jumped to, and the loop
/// died on its second iteration. Restricting rewrites to untargeted instructions
/// costs the loop-header case -- which is where the wins would be -- but a wrong
/// answer is worse than a missed optimization. A block-level analysis (only
/// untargeted *interior* instructions, plus dominance) would recover the loss.
std::vector<char> jumpTargets(const Function& fn) {
    std::vector<char> targets(fn.code.size(), 0);
    for (const Instruction& ins : fn.code) {
        if (isJump(ins.op) && ins.a >= 0 && static_cast<std::size_t>(ins.a) < fn.code.size()) {
            targets[static_cast<std::size_t>(ins.a)] = 1;
        }
    }
    return targets;
}

//===--- compaction ------------------------------------------------------===//

/// Deletes every instruction marked in `drop`, then retargets jumps.
///
/// Jump operands are absolute indices, so removal has to remap them. A target
/// that was dropped is redirected to the next surviving instruction -- which is
/// what falling through would have done anyway, because a pass only drops an
/// instruction after proving it has no effect.
///
/// Returns the number of instructions removed.
int compact(Function& fn, const std::vector<char>& drop) {
    const std::size_t n = fn.code.size();
    if (n == 0) {
        return 0;
    }

    // newIndexOf[i] = position in the compacted code, or -1 when dropped.
    std::vector<std::int32_t> newIndexOf(n, -1);
    std::int32_t kept = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!drop[i]) {
            newIndexOf[i] = kept++;
        }
    }
    if (kept == static_cast<std::int32_t>(n)) {
        return 0;   // nothing dropped
    }

    // nextKept[i] = first surviving index at or after i, or n.
    std::vector<std::size_t> nextKept(n + 1, n);
    for (std::size_t i = n; i-- > 0;) {
        nextKept[i] = drop[i] ? nextKept[i + 1] : i;
    }

    std::vector<Instruction> code;
    code.reserve(static_cast<std::size_t>(kept));
    for (std::size_t i = 0; i < n; ++i) {
        if (drop[i]) {
            continue;
        }
        Instruction ins = fn.code[i];
        if (isJump(ins.op)) {
            const std::int32_t target = ins.a;
            if (target < 0 || static_cast<std::size_t>(target) > n) {
                continue;   // malformed; leave unchanged rather than guess
            }
            const std::size_t survivor = nextKept[static_cast<std::size_t>(target)];
            // A target past the end can only mean "end of function"; clamp to the
            // last instruction so the VM's bounds check never sees an out-of-range
            // jump. Functions always end in Ret/Halt, so this does not arise in
            // practice -- it is here so a malformed input cannot become a crash.
            ins.a = survivor < n ? newIndexOf[survivor]
                                 : (kept > 0 ? kept - 1 : 0);
        }
        code.push_back(ins);
    }

    const int removed = static_cast<int>(n) - kept;
    fn.code = std::move(code);
    return removed;
}

//===--- pass: unreachable code -------------------------------------------===//

/// Drops instructions no path from the entry can reach.
///
/// Reachability is a plain walk from index 0. `Call` falls through (the callee
/// returns), `Trap` does not, and both arms of a conditional are live.
int removeUnreachable(Function& fn) {
    const std::size_t n = fn.code.size();
    if (n == 0) {
        return 0;
    }
    std::vector<char> reachable(n, 0);
    std::vector<std::size_t> work;
    work.push_back(0);
    reachable[0] = 1;

    while (!work.empty()) {
        const std::size_t i = work.back();
        work.pop_back();
        const Instruction& ins = fn.code[i];
        const auto visit = [&](std::size_t target) {
            if (target < n && !reachable[target]) {
                reachable[target] = 1;
                work.push_back(target);
            }
        };
        if (isJump(ins.op) && ins.a >= 0 && static_cast<std::size_t>(ins.a) < n) {
            visit(static_cast<std::size_t>(ins.a));
        }
        if (!endsBlock(ins.op)) {
            visit(i + 1);
        }
    }

    std::vector<char> drop(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        drop[i] = reachable[i] ? 0 : 1;
    }
    return compact(fn, drop);
}

//===--- pass: jump threading ---------------------------------------------===//

/// Retargets a jump whose destination is itself an unconditional jump.
///
/// Stops at a `SafePoint`: threading past one would execute the jump earlier
/// than written, and the safe point in between is where the scheduler gets to
/// look at the world.
std::int32_t threadTarget(const Function& fn, std::int32_t target) {
    int guard = 0;
    while (target >= 0 && static_cast<std::size_t>(target) < fn.code.size() && guard++ < 4096) {
        if (fn.code[static_cast<std::size_t>(target)].op != OpCode::Jump) {
            break;
        }
        const std::int32_t next = fn.code[static_cast<std::size_t>(target)].a;
        if (next == target) {
            break;   // self jump: leave it, the walker would spin
        }
        target = next;
    }
    return target;
}

int threadJumps(Function& fn) {
    int changed = 0;
    for (Instruction& ins : fn.code) {
        if (!isJump(ins.op)) {
            continue;
        }
        const std::int32_t resolved = threadTarget(fn, ins.a);
        if (resolved != ins.a) {
            ins.a = resolved;
            ++changed;
        }
    }
    return changed;
}

//===--- pass: peepholes --------------------------------------------------===//

/// The rewrites that need only the pattern under the cursor.
/// Returns instructions removed; `forwardedOut` counts the subset that came from
/// folding a store and a load into one keeping store.
int peephole(Function& fn, int& forwardedOut) {
    const std::size_t n = fn.code.size();
    forwardedOut = 0;
    if (n < 2) {
        return 0;
    }
    const std::vector<char> targets = jumpTargets(fn);
    std::vector<char> drop(n, 0);
    int changes = 0;
    int forwarded = 0;

    std::size_t i = 0;
    while (i < n) {
        if (drop[i]) {
            ++i;
            continue;
        }
        // A rewrite is only safe on instructions no jump can reach.
        if (targets[i] || (i + 1 < n && targets[i + 1])) {
            ++i;
            continue;
        }
        Instruction& first = fn.code[i];

        // `push; pop` -- the value is computed and immediately discarded.
        if (i + 1 < n && !drop[i + 1] && isPurePush(first.op) &&
            fn.code[i + 1].op == OpCode::Pop) {
            drop[i] = 1;
            drop[i + 1] = 1;
            changes += 1;
            i += 2;
            continue;
        }

        // `jump` to the instruction that follows it.
        if (first.op == OpCode::Jump && first.a == static_cast<std::int32_t>(i + 1)) {
            drop[i] = 1;
            changes += 1;
            ++i;
            continue;
        }

        // A load whose result is discarded. Safe because a frame slot always
        // holds a valid address -- unlike a generic `Load`, which could fault.
        if (i + 1 < n && !drop[i + 1] && first.op == OpCode::LoadLocal &&
            fn.code[i + 1].op == OpCode::Pop) {
            drop[i] = 1;
            drop[i + 1] = 1;
            changes += 1;
            i += 2;
            continue;
        }

        if (i + 1 < n && !drop[i + 1]) {
            const Instruction& second = fn.code[i + 1];

            // `LoadLocal x; StoreLocal x` -- reading a slot and writing the same
            // value back. With the keep bit absent both vanish; with it present
            // the store is what leaves the value on the stack, so the load alone
            // does the same job.
            if (first.op == OpCode::LoadLocal && second.op == OpCode::StoreLocal &&
                first.a == second.a && (second.b & ~kKeepBit) == first.b) {
                if ((second.b & kKeepBit) != 0) {
                    drop[i + 1] = 1;   // keep the load, drop the redundant store
                } else {
                    drop[i] = 1;
                    drop[i + 1] = 1;
                }
                changes += 1;
                i += 2;
                continue;
            }

            // Address-then-dereference, the pair the fused opcodes exist for.
            if (first.op == OpCode::LocalAddr && second.op == OpCode::Load) {
                fn.code[i] = Instruction{OpCode::LoadLocal, first.a, second.a};
                drop[i + 1] = 1;
                changes += 1;
                i += 2;
                continue;
            }
            // `LocalAddr x; Store` is NOT `StoreLocal x`, and fusing it was a bug:
            // `Store` pops the value first and the address second, so with the two
            // instructions adjacent the address on top *is* the value being stored,
            // and the destination is whatever is underneath. `StoreLocal x` instead
            // pops that address and writes it into x. The two agree only when the
            // stack holds nothing else, which is exactly when `Store` would
            // underflow.
            //
            // It stayed invisible because the common form `local = #x` takes the
            // fused-variable path in `emitAssign` and never produces this pair. It
            // fires for `member = #x` and `arr[i] = #x`, where it silently stored
            // the destination address into the source variable and left the field
            // zero — a linked-list link that is always null.
            if (first.op == OpCode::GlobalAddr && second.op == OpCode::Load) {
                fn.code[i] = Instruction{OpCode::LoadGlobal, first.a, second.a};
                drop[i + 1] = 1;
                changes += 1;
                i += 2;
                continue;
            }
            // Same reasoning as the `LocalAddr` case above: `GlobalAddr x; Store`
            // stores through an address on the stack, it does not store into x.
        }

        // Store-to-load forwarding (O3, gated by the caller's level check below
        // via `allowForwarding`): writing a slot and immediately reading it back
        // is one keeping store instead of two instructions.
        if (i + 1 < n && !drop[i + 1] && first.op == OpCode::StoreLocal &&
            (first.b & kKeepBit) == 0 && fn.code[i + 1].op == OpCode::LoadLocal &&
            first.a == fn.code[i + 1].a && first.b == fn.code[i + 1].b) {
            first.b |= kKeepBit;
            drop[i + 1] = 1;
            changes += 1;
            forwarded += 1;
            i += 2;
            continue;
        }

        ++i;
    }

    const int removed = compact(fn, drop);
    forwardedOut = forwarded;
    return changes > 0 ? removed : 0;
}

//===--- pass: local constant propagation ---------------------------------===//

/// Replaces `LoadLocal x` with the constant that every path to that load has
/// stored into `x`.
///
/// This is what makes unreachable-code detection reach anything. Without it,
/// `int a = 2 + 3 * 4; if (a == 99999) { ... }` keeps the dead branch: the test
/// reads a variable and nothing knows what it holds. With it the load becomes
/// `const 14`, folding turns the test into `const 0`, branch resolution deletes
/// the arm, and the reachability walk removes what was inside.
///
/// ── why a dataflow and not a single pass ──────────────────────────────────
///
/// The first version cleared everything it knew at every jump target. That is
/// sound but useless in the one place it matters: `JUMP_IF_FALSE` always targets
/// the instruction after the guarded arm, so *every* if-statement wiped the map
/// just before the code that follows it. In the example above the load sits
/// exactly on that target and nothing was ever propagated.
///
/// So this computes, per instruction, the set of slots whose value is agreed on
/// by all incoming edges. A slot survives a merge only when every predecessor
/// agrees on the same constant at the same width, which needs no proof
/// obligation: disagreement simply drops the entry.
///
/// ── what invalidates a slot ───────────────────────────────────────────────
///
///   * `StoreLocal x` -- the slot holds something new (tracked if the value came
///                       from a constant push immediately before, which is the
///                       shape `int a = 14;` produces);
///   * `StepLocal x`  -- `x += delta`, which is what `i++` becomes;
///   * `CopyStep`     -- post-increments both pointer slots it walks;
///   * `LocalAddr x`  -- the address escapes, so anything downstream could write
///                       through it.
///
/// That list has to be complete. Leaving `StepLocal` out does not lose an
/// optimization, it produces a wrong program: a loop counter gets replaced by the
/// constant it was initialised with. The strsort check caught exactly that with
/// `sortStringsDesc`'s reversing loop.
///
/// A `Call` invalidates nothing: each frame has its own locals and parameters are
/// copies, so a callee cannot reach its caller's slots. `SafePoint` likewise --
/// it lets a *different* process run, and that process has a different frame.
int propagateLocals(Function& fn) {
    const std::size_t n = fn.code.size();
    if (n < 2) {
        return 0;
    }

    struct SlotValue {
        std::int32_t slot = 0;
        std::int32_t constant = 0;   ///< index into fn.constants
        std::int32_t memType = 0;

        [[nodiscard]] bool operator==(const SlotValue& other) const {
            return slot == other.slot && constant == other.constant &&
                   memType == other.memType;
        }
    };
    using State = std::vector<SlotValue>;   // sorted by slot

    // Successors of each instruction, then predecessors derived from them.
    std::vector<std::vector<std::int32_t>> preds(n);
    const auto addEdge = [&preds](std::size_t from, std::size_t to) {
        if (to < preds.size()) {
            preds[to].push_back(static_cast<std::int32_t>(from));
        }
    };
    for (std::size_t i = 0; i < n; ++i) {
        const Instruction& ins = fn.code[i];
        switch (ins.op) {
        case OpCode::Jump:
            if (ins.a >= 0) {
                addEdge(i, static_cast<std::size_t>(ins.a));
            }
            break;
        case OpCode::JumpIfFalse:
        case OpCode::JumpIfTrue:
            if (ins.a >= 0) {
                addEdge(i, static_cast<std::size_t>(ins.a));
            }
            addEdge(i, i + 1);
            break;
        case OpCode::Ret:
        case OpCode::Halt:
        case OpCode::Trap:
            break;   // no successors
        default:
            addEdge(i, i + 1);
            break;
        }
    }

    const auto setSlot = [](State& state, const SlotValue& value) {
        for (std::size_t k = 0; k < state.size(); ++k) {
            if (state[k].slot == value.slot) {
                state[k] = value;
                return;
            }
            if (state[k].slot > value.slot) {
                state.insert(state.begin() + static_cast<std::ptrdiff_t>(k), value);
                return;
            }
        }
        state.push_back(value);
    };
    const auto clearSlot = [](State& state, std::int32_t slot) {
        for (std::size_t k = 0; k < state.size(); ++k) {
            if (state[k].slot == slot) {
                state.erase(state.begin() + static_cast<std::ptrdiff_t>(k));
                return;
            }
        }
    };
    const auto findSlot = [](const State& state, std::int32_t slot) -> const SlotValue* {
        for (const SlotValue& value : state) {
            if (value.slot == slot) {
                return &value;
            }
        }
        return nullptr;
    };

    // Transfer function: what is known after executing `index`.
    const auto transfer = [&](const State& in, std::size_t index) {
        State out = in;
        const Instruction& ins = fn.code[index];
        if (ins.op == OpCode::StoreLocal) {
            clearSlot(out, ins.a);
            // `const; StoreLocal` -- the slot now holds that constant.
            //
            // Lexical adjacency is not enough: the const has to be the store's
            // *only* predecessor. `cond ? 3 : 5` stored into a local lowers to
            //
            //     ...
            //     JUMP_IF_FALSE -> else
            //     CONST 3
            //     JUMP          -> merge
            //   else:
            //     CONST 5
            //   merge:
            //     STORE_LOCAL a
            //
            // where `CONST 5` sits immediately before the store without reaching
            // it on the true path. Reading that as "a now holds 5" made every
            // conditional assignment whose last arm was a constant fold to that
            // arm at O2 and above; `int a = (x > 0) ? 3 : 5;` printed 5 for x=1.
            //
            // Requiring a single incoming edge is exactly the missing proof: with
            // one predecessor the const is on the only path that reaches the
            // store, so whatever it pushed is what the store pops.
            const bool solePredecessor =
                index > 0 && preds[index].size() == 1 &&
                preds[index][0] == static_cast<std::int32_t>(index - 1);
            if (solePredecessor) {
                const Instruction& prev = fn.code[index - 1];
                if (prev.op == OpCode::ConstInt || prev.op == OpCode::ConstFloat) {
                    setSlot(out, SlotValue{ins.a, prev.a, ins.b & ~kKeepBit});
                }
            }
        } else if (ins.op == OpCode::LocalAddr) {
            clearSlot(out, ins.a);   // the address escapes
        } else if (ins.op == OpCode::StepLocal) {
            // `local += delta`, i.e. `i++`. This is the single most common write
            // to a local slot in a loop, and forgetting it is not a missed
            // optimization: the loop counter would be replaced by the constant it
            // started at, and the loop would run the wrong number of times.
            clearSlot(out, ins.a);
        } else if (ins.op == OpCode::CopyStep) {
            // Steps *both* pointer slots after copying through them.
            clearSlot(out, ins.a);
            clearSlot(out, ins.b & 0x0FFF'FFFF);   // top nibble holds the element size
        }
        return out;
    };

    std::vector<State> in(n);
    std::vector<char> outReady(n, 0);
    // No separate "reached" set: an instruction's IN state stays empty unless some
    // predecessor has produced one, so unreachable code is left untouched by
    // construction. The first version did carry a `reached` vector and only ever
    // set index 0, which silently skipped every other instruction.

    // Fixpoint. The cap is generous: a straight-line body converges in two or
    // three sweeps, and loops need one extra pass per nested level.
    const int maxSweeps = 40;
    for (int sweep = 0; sweep < maxSweeps; ++sweep) {
        bool changed = false;
        for (std::size_t i = 0; i < n; ++i) {
            // IN = intersection of the predecessors' OUT states.
            if (i == 0) {
                in[i].clear();
            } else {
                State merged;
                bool first = true;
                for (const std::int32_t p : preds[i]) {
                    if (p < 0 || !outReady[static_cast<std::size_t>(p)]) {
                        continue;   // that edge's state is not known yet
                    }
                    const State outState = transfer(in[static_cast<std::size_t>(p)],
                                                    static_cast<std::size_t>(p));
                    if (first) {
                        merged = outState;
                        first = false;
                        continue;
                    }
                    // Keep only what both sides agree on.
                    State intersection;
                    for (const SlotValue& value : merged) {
                        if (const SlotValue* other = findSlot(outState, value.slot);
                            other != nullptr && *other == value) {
                            intersection.push_back(value);
                        }
                    }
                    merged = std::move(intersection);
                }
                if (!first && merged != in[i]) {
                    in[i] = std::move(merged);
                    changed = true;
                }
            }
            outReady[i] = 1;
        }
        if (!changed) {
            break;
        }
    }

    // Rewrite the loads. This is a replacement, not a deletion: the instruction
    // still pushes exactly one value, so it is safe even when a jump targets it.
    int changes = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (fn.code[i].op != OpCode::LoadLocal) {
            continue;
        }
        const SlotValue* value = findSlot(in[i], fn.code[i].a);
        if (value == nullptr || value->memType != fn.code[i].b) {
            continue;
        }
        const Value* constant = constantAt(fn, value->constant);
        if (constant == nullptr) {
            continue;
        }
        fn.code[i] = Instruction{constant->tag() == Value::Tag::Float ? OpCode::ConstFloat
                                                                     : OpCode::ConstInt,
                                 value->constant, 0};
        ++changes;
    }
    return changes;
}

//===--- pass: safe points (O4 only) --------------------------------------===//

/// Deletes every `SafePoint`.
///
/// Safe points exist so the scheduler can preempt a `process` and evaluate
/// `event` conditions. Removing them means:
///
///   * a `process` body runs to completion without ever yielding, so several
///     processes no longer interleave -- one finishes before the next starts;
///   * `event` conditions are never tested, so handlers never fire;
///   * `yield()` still exists as an instruction of its own, but nothing else
///     preempts.
///
/// Every other level must keep them (spec: "safe points are inserted by the compiler
/// automatically and may not be removed by an optimizer"),
/// which is why this is gated on O4 and reported separately in the statistics.
///
/// This runs first at O4, before the other passes: with the safe points gone they
/// no longer act as barriers, so folding and forwarding can cross positions they
/// previously could not.
int removeSafePoints(Function& fn) {
    const std::size_t n = fn.code.size();
    if (n == 0) {
        return 0;
    }
    std::vector<char> drop(n, 0);
    int removed = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (fn.code[i].op == OpCode::SafePoint) {
            drop[i] = 1;
            ++removed;
        }
    }
    if (removed == 0) {
        return 0;
    }
    compact(fn, drop);
    return removed;
}

//===--- pass: dead stores ------------------------------------------------===//

/// Drops a store to a local that a later store to the same slot overwrites
/// before anything reads it. Scoped to a straight-line run of instructions: a
/// jump or label ends the search, so the analysis stays local and cheap.
int removeDeadStores(Function& fn) {
    const std::size_t n = fn.code.size();
    if (n < 2) {
        return 0;
    }
    const std::vector<char> targets = jumpTargets(fn);
    std::vector<char> drop(n, 0);
    int changes = 0;

    for (std::size_t i = 0; i < n; ++i) {
        if (drop[i] || targets[i] || fn.code[i].op != OpCode::StoreLocal) {
            continue;
        }
        const std::int32_t slot = fn.code[i].a;
        // A keeping store leaves a value for someone; it is not dead.
        if ((fn.code[i].b & kKeepBit) != 0) {
            continue;
        }
        for (std::size_t j = i + 1; j < n; ++j) {
            const Instruction& later = fn.code[j];
            if (isJump(later.op) || endsBlock(later.op) || later.op == OpCode::SafePoint) {
                break;   // control may leave: stop, the store might be observed
            }
            if (later.op == OpCode::StoreLocal && later.a == slot) {
                drop[i] = 1;
                changes += 1;
                break;
            }
            // Any other read of the slot, or any call that could, ends it.
            if (later.op == OpCode::LoadLocal && later.a == slot) {
                break;
            }
            if (isBarrier(later.op)) {
                break;
            }
        }
    }

    return changes > 0 ? compact(fn, drop) : 0;
}

//===--- pass: constant folding ------------------------------------------===//

/// Result of trying to evaluate a binary operation on two constants.
struct Folded {
    bool ok = false;
    Value value;
};

/// Folds `lhs op rhs` exactly the way the VM would, or refuses.
///
/// The refusal cases are the interesting part: the VM traps on integer overflow
/// and on division by zero, and a fold must leave those traps where the
/// programmer put them. So anything that would trap returns `ok = false` and the
/// three original instructions survive to trap at run time.
Folded tryFold(OpCode op, const MemType& type, const Value& lhs, const Value& rhs) {
    // Comparisons never trap, whatever the operands.
    const bool comparison = op == OpCode::Eq || op == OpCode::Ne || op == OpCode::Lt ||
                            op == OpCode::Le || op == OpCode::Gt || op == OpCode::Ge;

    if (type.isFloat()) {
        if (op == OpCode::Mod) {
            return {};   // fmod on constants is not worth special-casing
        }
        if (!comparison) {
            const double a = lhs.numeric();
            const double b = rhs.numeric();
            double result = 0.0;
            switch (op) {
            case OpCode::Add: result = a + b; break;
            case OpCode::Sub: result = a - b; break;
            case OpCode::Mul: result = a * b; break;
            case OpCode::Div: result = a / b; break;
            default: return {};
            }
            // NaN and infinity fold to something a later comparison behaves oddly
            // with; leave them to run time rather than bake in a surprising value.
            if (!std::isfinite(result)) {
                return {};
            }
            return {true, Value::fromFloat(result)};
        }
        const double a = lhs.numeric();
        const double b = rhs.numeric();
        int truth = 0;
        switch (op) {
        case OpCode::Eq: truth = a == b; break;
        case OpCode::Ne: truth = a != b; break;
        case OpCode::Lt: truth = a < b; break;
        case OpCode::Le: truth = a <= b; break;
        case OpCode::Gt: truth = a > b; break;
        case OpCode::Ge: truth = a >= b; break;
        default: return {};
        }
        return {true, Value::fromInt(truth)};
    }

    if (!type.isInteger()) {
        return {};   // pointers and addresses are not folded
    }
    if (lhs.tag() == Value::Tag::Float || rhs.tag() == Value::Tag::Float) {
        return {};
    }

    const unsigned bits = type.bits == 0 ? 32 : type.bits;
    const bool isUnsigned =
        type.isUnsigned || type.kind == MemType::Kind::Bool || type.kind == MemType::Kind::Char;

    if (isUnsigned) {
        // Unsigned arithmetic wraps; the VM masks after the operation instead of
        // trapping, so folding is always safe here.
        const unsigned __int128 a = static_cast<unsigned __int128>(lhs.asInt()) & maskFor(bits);
        const unsigned __int128 b = static_cast<unsigned __int128>(rhs.asInt()) & maskFor(bits);
        unsigned __int128 result = 0;
        switch (op) {
        case OpCode::Add: result = a + b; break;
        case OpCode::Sub: result = a - b; break;
        case OpCode::Mul: result = a * b; break;
        case OpCode::Div:
            if (b == 0) { return {}; }
            result = a / b;
            break;
        case OpCode::Mod:
            if (b == 0) { return {}; }
            result = a % b;
            break;
        case OpCode::And: result = a & b; break;
        case OpCode::Or: result = a | b; break;
        case OpCode::Xor: result = a ^ b; break;
        case OpCode::Shl: result = b >= bits ? 0 : (a << static_cast<unsigned>(b)); break;
        case OpCode::Shr: result = b >= bits ? 0 : (a >> static_cast<unsigned>(b)); break;
        case OpCode::Eq: return {true, Value::fromInt(a == b ? 1 : 0)};
        case OpCode::Ne: return {true, Value::fromInt(a != b ? 1 : 0)};
        case OpCode::Lt: return {true, Value::fromInt(a < b ? 1 : 0)};
        case OpCode::Le: return {true, Value::fromInt(a <= b ? 1 : 0)};
        case OpCode::Gt: return {true, Value::fromInt(a > b ? 1 : 0)};
        case OpCode::Ge: return {true, Value::fromInt(a >= b ? 1 : 0)};
        default: return {};
        }
        result &= maskFor(bits);
        return {true, Value::fromInt(static_cast<Int128>(result))};
    }

    const Int128 a = lhs.asInt();
    const Int128 b = rhs.asInt();
    switch (op) {
    case OpCode::Eq: return {true, Value::fromInt(a == b ? 1 : 0)};
    case OpCode::Ne: return {true, Value::fromInt(a != b ? 1 : 0)};
    case OpCode::Lt: return {true, Value::fromInt(a < b ? 1 : 0)};
    case OpCode::Le: return {true, Value::fromInt(a <= b ? 1 : 0)};
    case OpCode::Gt: return {true, Value::fromInt(a > b ? 1 : 0)};
    case OpCode::Ge: return {true, Value::fromInt(a >= b ? 1 : 0)};
    default: break;
    }

    Int128 result = 0;
    switch (op) {
    // Checked, not bare arithmetic, for the same reason as the VM's own version
    // (VMStep.cpp): at 128 bits the operands span the whole `Int128`, so `a + b`
    // is signed overflow. Folding it would also put the optimizer and the VM into
    // disagreement -- the VM traps, the folder would produce the wrapped value and
    // delete the trap. Returning {} keeps the instruction so the trap still
    // happens where it should.
    case OpCode::Add:
        if (__builtin_add_overflow(a, b, &result)) {
            return {};
        }
        break;
    case OpCode::Sub:
        if (__builtin_sub_overflow(a, b, &result)) {
            return {};
        }
        break;
    case OpCode::Mul: {
        __int128 product = 0;
        if (__builtin_mul_overflow(a, b, &product)) {
            return {};   // the VM would trap; keep the trap in the program
        }
        result = product;
        break;
    }
    case OpCode::Div:
        if (b == 0) { return {}; }
        if (a == signedMinOf(bits) && b == -1) { return {}; }
        result = a / b;
        break;
    case OpCode::Mod:
        if (b == 0) { return {}; }
        result = (b == -1) ? 0 : a % b;
        break;
    case OpCode::And: result = a & b; break;
    case OpCode::Or: result = a | b; break;
    case OpCode::Xor: result = a ^ b; break;
    case OpCode::Shl: {
        if (b < 0 || b >= static_cast<Int128>(bits)) {
            if (a == 0) {
                result = 0;
                break;
            }
            return {};
        }
        if (bits >= 128) {
            // `~maskFor(128)` is zero: there is no bit above the destination to
            // catch the carry-out, so the test below can never fire. Bound the
            // operand instead, the same way the VM does.
            const unsigned amount = static_cast<unsigned>(b);
            const Int128 limit = a >= 0 ? (signedMaxOf(bits) >> amount)
                                        : (signedMinOf(bits) >> amount);
            if (a >= 0 ? (a > limit) : (a < limit)) {
                return {};
            }
            result = static_cast<Int128>(static_cast<unsigned __int128>(a) << amount);
            break;
        }
        const unsigned __int128 shifted =
            static_cast<unsigned __int128>(a) << static_cast<unsigned>(b);
        if ((shifted & ~maskFor(bits)) != 0) {
            return {};
        }
        result = static_cast<Int128>(shifted & maskFor(bits));
        break;
    }
    case OpCode::Shr: {
        if (b < 0 || b >= static_cast<Int128>(bits)) {
            result = a < 0 ? -1 : 0;
            break;
        }
        result = a >> static_cast<unsigned>(b);
        break;
    }
    default: return {};
    }

    if (!inRangeOf(result, type)) {
        return {};   // overflow: the program is supposed to trap here
    }
    return {true, Value::fromInt(result)};
}

/// `const k; StoreLocal x keep; const b; <op>` -> `const k; StoreLocal x; const (k op b)`.
///
/// The keeping store is a shape the forwarding peephole creates: `StoreLocal x;
/// LoadLocal x` becomes one store that leaves the value on the stack. That saves an
/// instruction, but it also hides the value from the folder -- an operand produced
/// by a store is not a constant push, so `a == 99999` with `a` known to be 14 used
/// to stay a run-time comparison.
///
/// Nothing is inserted here: the store simply stops keeping the value (it still
/// writes the slot), and the constant that the operation would have produced takes
/// the place of its right operand. Four instructions become three, and the stack
/// ends up with exactly the one value the original pushed.
bool foldThroughKeepingStore(Function& fn, const Module& module, std::size_t p,
                             const std::vector<char>& targets, std::vector<char>& drop) {
    const std::size_t n = fn.code.size();
    if (p + 3 >= n) {
        return false;
    }
    const Instruction& pushK = fn.code[p];
    const Instruction& store = fn.code[p + 1];
    const Instruction& pushB = fn.code[p + 2];
    Instruction& oper = fn.code[p + 3];

    if (pushK.op != OpCode::ConstInt && pushK.op != OpCode::ConstFloat) {
        return false;
    }
    if (store.op != OpCode::StoreLocal || (store.b & kKeepBit) == 0) {
        return false;
    }
    if (pushB.op != OpCode::ConstInt && pushB.op != OpCode::ConstFloat) {
        return false;
    }
    // The window's interior must not be a jump target: a jump landing on the store
    // or on the right operand would otherwise see a different stack shape.
    if (targets[p + 1] || targets[p + 2] || targets[p + 3]) {
        return false;
    }
    if (pushK.op != pushB.op) {
        return false;   // mixed kinds: leave it to the normal path
    }
    if (oper.a < 0 || static_cast<std::size_t>(oper.a) >= module.memTypes.size()) {
        return false;
    }
    const MemType& type = module.memTypes[static_cast<std::size_t>(oper.a)];
    if (type.isFloat() != (pushK.op == OpCode::ConstFloat)) {
        return false;
    }
    const Value* lhs = constantAt(fn, pushK.a);
    const Value* rhs = constantAt(fn, pushB.a);
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    const Folded folded = tryFold(oper.op, type, *lhs, *rhs);
    if (!folded.ok) {
        return false;
    }

    const std::size_t index = fn.addConstant(folded.value);
    fn.code[p + 1] = Instruction{OpCode::StoreLocal, store.a, store.b & ~kKeepBit};
    fn.code[p + 2] = Instruction{folded.value.tag() == Value::Tag::Float ? OpCode::ConstFloat
                                                                       : OpCode::ConstInt,
                                 static_cast<std::int32_t>(index), 0};
    drop[p + 3] = 1;
    return true;
}

/// `const a; const b; <binary op>` -> `const (a op b)`.
///
/// Only the memType of the operating instruction is needed; the two constants
/// must be the same kind it operates on.
int foldConstants(Function& fn, const Module& module) {
    const std::size_t n = fn.code.size();
    if (n < 3) {
        return 0;
    }
    const std::vector<char> targets = jumpTargets(fn);
    std::vector<char> drop(n, 0);
    int changes = 0;

    for (std::size_t i = 0; i + 2 < n; ++i) {
        if (drop[i] || drop[i + 1] || drop[i + 2]) {
            continue;
        }
        // A jump landing inside the window expects the pushes it contains.
        if (targets[i] || targets[i + 1] || targets[i + 2]) {
            continue;
        }
        // The longer pattern first: a keeping store between the two pushes.
        if (foldThroughKeepingStore(fn, module, i, targets, drop)) {
            changes += 1;
            i += 3;
            continue;
        }

        const Instruction& pushA = fn.code[i];
        const Instruction& pushB = fn.code[i + 1];
        Instruction& oper = fn.code[i + 2];

        const bool intPushes = pushA.op == OpCode::ConstInt && pushB.op == OpCode::ConstInt;
        const bool floatPushes = pushA.op == OpCode::ConstFloat && pushB.op == OpCode::ConstFloat;
        if (!intPushes && !floatPushes) {
            continue;
        }
        if (oper.op != OpCode::Add && oper.op != OpCode::Sub && oper.op != OpCode::Mul &&
            oper.op != OpCode::Div && oper.op != OpCode::Mod && oper.op != OpCode::And &&
            oper.op != OpCode::Or && oper.op != OpCode::Xor && oper.op != OpCode::Shl &&
            oper.op != OpCode::Shr && oper.op != OpCode::Eq && oper.op != OpCode::Ne &&
            oper.op != OpCode::Lt && oper.op != OpCode::Le && oper.op != OpCode::Gt &&
            oper.op != OpCode::Ge) {
            continue;
        }
        if (oper.a < 0 || static_cast<std::size_t>(oper.a) >= module.memTypes.size()) {
            continue;
        }
        const MemType& type = module.memTypes[static_cast<std::size_t>(oper.a)];
        if (type.isFloat() != floatPushes) {
            continue;   // integer pushes feeding a float operation (or the reverse)
        }
        const Value* lhs = constantAt(fn, pushA.a);
        const Value* rhs = constantAt(fn, pushB.a);
        if (lhs == nullptr || rhs == nullptr) {
            continue;
        }
        const Folded folded = tryFold(oper.op, type, *lhs, *rhs);
        if (!folded.ok) {
            continue;
        }
        // The comparison result is an int even when the operands were doubles;
        // the constant pool and the opcode have to agree on that.
        const std::size_t index = fn.addConstant(folded.value);
        if (folded.value.tag() == Value::Tag::Float) {
            fn.code[i] = Instruction{OpCode::ConstFloat, static_cast<std::int32_t>(index), 0};
        } else {
            fn.code[i] = Instruction{OpCode::ConstInt, static_cast<std::int32_t>(index), 0};
        }
        drop[i + 1] = 1;
        drop[i + 2] = 1;
        changes += 1;
        i += 2;
    }

    return changes > 0 ? compact(fn, drop) : 0;
}

/// `const v; JumpIfFalse L` -> `Jump L` when v is falsy, nothing when truthy.
int resolveBranches(Function& fn) {
    const std::size_t n = fn.code.size();
    if (n < 2) {
        return 0;
    }
    const std::vector<char> targets = jumpTargets(fn);
    std::vector<char> drop(n, 0);
    int changes = 0;

    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (drop[i] || targets[i] || targets[i + 1]) {
            continue;
        }
        Instruction& push = fn.code[i];
        Instruction& branch = fn.code[i + 1];
        if (branch.op != OpCode::JumpIfFalse && branch.op != OpCode::JumpIfTrue) {
            continue;
        }
        bool truthy = false;
        if (push.op == OpCode::ConstInt) {
            const Value* value = constantAt(fn, push.a);
            if (value == nullptr) {
                continue;
            }
            truthy = value->asInt() != 0;
        } else if (push.op == OpCode::ConstFloat) {
            const Value* value = constantAt(fn, push.a);
            if (value == nullptr) {
                continue;
            }
            truthy = value->numeric() != 0.0;
        } else {
            continue;
        }

        // `JumpIfFalse` taken means the value was falsy -- and vice versa.
        const bool taken = (branch.op == OpCode::JumpIfFalse) ? !truthy : truthy;
        if (taken) {
            // The push and the test collapse into an unconditional jump.
            fn.code[i] = Instruction{OpCode::Jump, branch.a, 0};
            drop[i + 1] = 1;
        } else {
            // Never taken: both instructions go away and control falls through.
            drop[i] = 1;
            drop[i + 1] = 1;
        }
        changes += 1;
        i += 1;
    }

    return changes > 0 ? compact(fn, drop) : 0;
}

//===--- driver -----------------------------------------------------------===//

/// One round of every pass the level enables, repeated until nothing changes.
///
/// The repeat is needed because the passes feed each other: folding a constant
/// exposes a branch, resolving the branch makes code unreachable, and removing
/// it can expose another peephole. The iteration cap keeps a bug that oscillates
/// from becoming a hang.
void optimizeFunction(Function& fn, const Module& module, OptLevel level, OptimizeStats& stats) {
    if (fn.isExternal() || fn.code.empty()) {
        return;
    }

    // O4 first, and outside the round loop: safe points are barriers for every
    // later pass, so deleting them before anything else is what makes the rest of
    // O4 more than "O3 plus a deletion". Cheap enough not to need repeating.
    if (level >= OptLevel::O4) {
        const int removed = removeSafePoints(fn);
        stats.safePointsRemoved += removed;
    }

    const int maxRounds = level == OptLevel::O1 ? 4 : 12;
    for (int round = 0; round < maxRounds; ++round) {
        int changes = 0;

        const int unreachable = removeUnreachable(fn);
        stats.unreachable += unreachable;
        changes += unreachable;

        if (level >= OptLevel::O2) {
            // Propagation first: it turns loads into constants, which is what
            // gives the folder and the branch resolver something to bite on.
            const int propagated = propagateLocals(fn);
            stats.propagated += propagated;
            changes += propagated;

            const int folded = foldConstants(fn, module);
            stats.folded += folded;
            changes += folded;

            const int branches = resolveBranches(fn);
            stats.branchesResolved += branches;
            changes += branches;
        }

        const int threaded = threadJumps(fn);
        stats.threaded += threaded;
        changes += threaded;

        if (level >= OptLevel::O3) {
            const int deadStores = removeDeadStores(fn);
            stats.deadStores += deadStores;
            changes += deadStores;
        }

        int forwarded = 0;
        const int peepholed = peephole(fn, forwarded);
        stats.forwarded += forwarded;
        stats.peepholes += peepholed - forwarded;
        changes += peepholed;

        if (changes == 0) {
            break;
        }
    }
}

} // namespace

//===--- public entry point ----------------------------------------------===//

const char* optLevelName(OptLevel level) {
    switch (level) {
    case OptLevel::O0: return "O0";
    case OptLevel::O1: return "O1";
    case OptLevel::O2: return "O2";
    case OptLevel::O3: return "O3";
    case OptLevel::O4: return "O4";
    }
    return "O?";
}

bool optLevelMayChangeFlow(OptLevel level) {
    return level >= OptLevel::O4;
}

OptimizeStats optimizeModule(Module& module, OptLevel level) {
    OptimizeStats total;
    if (level == OptLevel::O0) {
        return total;
    }
    for (Function& fn : module.functions) {
        optimizeFunction(fn, module, level, total);
    }
    return total;
}

void printOptimizeStats(const OptimizeStats& stats, OptLevel level, std::ostream& out) {
    out << "optimize: " << optLevelName(level) << " folded=" << stats.folded
        << " branches=" << stats.branchesResolved << " peepholes=" << stats.peepholes
        << " threaded=" << stats.threaded << " dead=" << stats.unreachable
        << " forwarded=" << stats.forwarded << " deadStores=" << stats.deadStores
        << " propagated=" << stats.propagated;
    if (stats.safePointsRemoved > 0) {
        // Called out separately: this is the one count that changes what a
        // concurrent program does, so it must not hide inside a total.
        out << " safePoints=" << stats.safePointsRemoved << " (flow not preserved)";
    }
    out << " (" << stats.total() << " rewrites)\n";
}

} // namespace acvm
