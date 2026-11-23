//===--- Optimize.h - bytecode optimization levels ----------------------===//
#pragma once

#include "acvm/IR/Bytecode.h"

#include <string>

namespace acvm {

/// The four levels every C compiler exposes, with the same promise: O0 is a
/// faithful transcription of the source, higher levels trade compile time for
/// run time and never change what a program computes.
///
/// What each level turns on:
///
///   O0  nothing. The instructions IRGen emitted, in order. Debugging and
///       --ir output are read at this level, and a `--stats` comparison against
///       a higher level is how the optimizer's effect is measured.
///
///   O1  the local rewrites that need no dataflow: unreachable code after a
///       jump, return or trap; dead pushes (`push; pop`); `jump` to the next
///       instruction; `jump` chains; and the address/dereference pairs IRGen
///       may still leave behind (`LocalAddr x; Load t` -> `LoadLocal`).
///
///   O2  adds the two that need to look at values: constant folding and branch
///       resolution (`const 0; JumpIfFalse L` -> `Jump L`).
///
///   O3  adds store-to-load forwarding and dead-store elimination inside a
///       basic block. Free of side effects because every store it removes is
///       overwritten before any read.
///
///   O4  is not "more of O3": it is the level that **stops preserving observable
///       execution flow**. It deletes every `SafePoint`, so the scheduler has no
///       place to preempt a `process` and `event` conditions are never evaluated
///       at run time, and it drops `assert` checks. Single-threaded programs keep
///       their exact behaviour; anything that relies on `process` scheduling,
///       event conditions, or a failing assertion will behave differently.
///
///       That is a deliberate contract, not a bug, and the specification says so
///       (the language specification, "optimization levels"): O0-O3 must
///       preserve semantics, O4 may not.
///
/// One rule overrides all of them: **a `SafePoint` is never removed, and no
/// rewrite may move work across one.** Safe points are where the scheduler
/// evaluates `event` conditions and where `process` switches, so the set of
/// instructions between two of them is an observable atomicity boundary
/// (spec §5.8). Folding a constant that sits across a safe point would change
/// which values a concurrent `process` can observe.
enum class OptLevel : int {
    O0 = 0,
    O1 = 1,
    O2 = 2,
    O3 = 3,
    /// Deletes safe points and assertions: see the header comment. Only for
    /// programs that do not depend on scheduling.
    O4 = 4,
};

/// What a run of the optimizer changed. Reported by `--stats` so that "the
/// optimizer ran" is a measurement rather than a claim.
struct OptimizeStats {
    int folded = 0;          ///< constant expressions replaced by their value
    int branchesResolved = 0;///< conditional jumps decided at compile time
    int unreachable = 0;     ///< instructions no path can reach
    int peepholes = 0;       ///< dead pushes, jumps to the next instruction, fused accesses
    int threaded = 0;        ///< jumps retargeted past a chain of jumps
    int forwarded = 0;       ///< store/load pairs fused into a keeping store
    int deadStores = 0;      ///< stores overwritten before being read
    int propagated = 0;      ///< loads replaced by a known constant
    /// Safe points deleted (O4 only). Reported separately because it is the one
    /// change that can alter what a concurrent program does.
    int safePointsRemoved = 0;

    [[nodiscard]] int total() const {
        return folded + branchesResolved + unreachable + peepholes + threaded + forwarded +
               deadStores + propagated + safePointsRemoved;
    }
};

/// True when `level` may break the "same execution flow" guarantee. Callers use
/// it to decide whether to enable assertions and whether a scheduling-dependent
/// test can run at this level at all.
[[nodiscard]] bool optLevelMayChangeFlow(OptLevel level);

/// Runs the passes `level` enables over every function in `module`.
///
/// Externally implemented functions have no code and are skipped. Returns the
/// counters; at `O0` they are all zero because nothing runs.
OptimizeStats optimizeModule(Module& module, OptLevel level);

/// `"O0"` .. `"O4"`, for diagnostics.
const char* optLevelName(OptLevel level);

/// Prints the counters in one line, e.g.
/// `optimize: O2 folded=12 branches=3 peepholes=40 (55 rewrites)`.
void printOptimizeStats(const OptimizeStats& stats, OptLevel level, std::ostream& out);

} // namespace acvm
