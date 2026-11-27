# Dispatch performance

How fast the interpreter runs a program, what was measured, and what is still on the table.

The goal set for this work was CPython 3.13. It is close for the two shapes that dominate
ordinary code, and still far for one. Everything below is measured rather than estimated, and
the numbers that are exact (instruction counts) are marked as such.

## Method

Four programs, each available in both languages, in `bench/` beside this file:

| benchmark | shape |
|---|---|
| `empty` | `for (int i = 0; i < 50'000'000; i++) { }` |
| `arith` | 5M iterations of two moduli, a multiply, a subtract |
| `calls` | `fib(10)` called 300 000 times — 53.1M calls |
| `array` | 2 000 × 1 000 reads from a 1 000 element array |

Wall-clock is the minimum of five runs, with the two builds alternating so that drift in
machine load cancels. CPython is 3.13.14, the build on this machine. Instruction counts come
from `--stats` and are exact and reproducible; where the two disagree about a change, the
counts are the ones to believe — the wall-clock numbers move by ±5% between sessions.

## Where it stands

| benchmark | Axiom | CPython 3.13 | ratio |
|---|---|---|---|
| `empty` | 2.987 s | 1.372 s | 2.18× slower |
| `arith` | 0.916 s | 0.837 s | 1.09× slower |
| `array` | 0.445 s | 0.493 s | at parity (±10%) |
| `calls` | 11.119 s | 2.343 s | 4.75× slower |

`array` lands on either side of CPython depending on the session — 0.90× and 1.06× on two
runs an hour apart — so it is reported as parity rather than as a win. That is the honest
reading of a difference smaller than the noise.

Arithmetic and array access are at parity. Function calls are not, and that is the shape the
rest of this document is about.

Per-instruction cost, computed from the same runs:

| benchmark | instructions | ns/instruction |
|---|---|---|
| `empty` | 300 000 009 | 9.96 |
| `arith` | 90 000 013 | 10.18 |
| `calls` | 719 100 013 | 15.46 |
| `array` | 48 046 019 | 9.26 |

So dispatch is about 10 ns — roughly 30 cycles — per instruction on straight-line code, and
15 ns in call-heavy code. CPython spends about 17 ns per bytecode on `empty`, so Axiom's
dispatch is already the cheaper of the two per operation. **The remaining gap is the number of
instructions per unit of work**, not the cost of one.

## What changed

### Instruction counts

| benchmark | before | after | change |
|---|---|---|---|
| `empty` | 500 000 009 | 300 000 009 | −40.0% |
| `arith` | 110 000 013 | 90 000 013 | −18.2% |
| `calls` | 906 000 013 | 719 100 013 | −20.6% |

Three changes account for it.

**The peephole pass was doing nothing.** `--stats` reported `0 rewrites` on every program,
including ones with obvious redundancies. The cause was one guard at the top of the pass:

```cpp
if (targets[i] || (i + 1 < n && targets[i + 1])) { ++i; continue; }
```

It rejected any position where a jump landed. That is right for a rule that *removes* an
instruction — the jump operand is an index, so the jump would arrive at whatever moved into
its place — and wrong for a rule that *rewrites in place*, which is what a fusion is. The
epilogue of every function is a jump target, so `LocalAddr slot; Load; Ret` was never fused,
and neither was anything else downstream of that.

The guard now rejects only what it must, and the four rules that drop `code[i]` test
`targets[i]` where they are.

**`return` cost seven instructions.** The return value went through a frame slot: store it,
jump to the epilogue, load it back, return. With fusion enabled and a `LocalAddr x; Swap; Store`
rule added, that is four: `StoreLocal; LoadLocal; Ret` and one jump, and the jump is removed
when the epilogue follows it directly.

**Every loop paid five instructions for its own counter.** `for (...; ...; i++)` emitted the
increment through the expression path — `LoadLocal; ConstInt; Add; StoreLocal keep; Pop` —
because only `StmtKind::ExprStmt` knew the result was discarded. The third clause of a `for`
has no reader either. It now goes through the same path, which collapses to the `StepLocal`
opcode that already existed for exactly this shape:

```
0002  LOAD_LOCAL   frame+0 i32
0003  CONST_INT    [1] = 50000000
0004  LT           i32
0005  JUMP_IF_FALSE-> 8
0006  STEP_LOCAL   frame+0 += 1 i32
0007  JUMP         -> 2
```

Six instructions per iteration instead of ten. `StepLocal` was in the opcode table the whole
time, with a comment saying the generic form costs four instructions.

### Per-instruction cost

**The retired-instruction counters are now optional.** `m_instructions` and
`m_opcodeCounts` were maintained on every instruction so that `--stats` would have them. The
comment above them said the update was "below the noise floor". It was not: removing it from
the default path is worth 9–11% on all four benchmarks, which is a factor of ten away from
noise. They are now maintained only after `setProfile(true)`, which `--stats` and the debugger
both do.

**Link-time optimisation is on for release builds.** The dispatch loop and the memory model it
drives live in different translation units, so `LOAD_LOCAL` → `loadScalar` → `readBytes` →
page cache was four calls that the compiler could not see through. Worth 2.6–6% depending on
the benchmark — less than expected, because the work inside those functions dominates the call
overhead.

**`SafePoint` no longer leaves the interpreter when nothing is watching.** A safe point is
emitted before every `Call` so the scheduler can preempt, `event` conditions can be evaluated,
and the debugger can see the program. With one process, no events and no debugger, none of
those is true, and returning to the scheduler cost a frame write, an unwind to `runQuantum`,
a call to `evaluateEvents` that returned immediately, and a re-entry. Now it is a branch.
Worth about 3% on `calls` — smaller than it looks, because the interpreter's exit and re-entry
turned out not to be as expensive as the instruction count suggested.

## What is still on the table

In the order I would do them, with what each is worth.

**1. Compare-and-branch fusion.** `LOAD_LOCAL i; CONST_INT N; LT; JUMP_IF_FALSE` is four
instructions to ask one question, and it is the header of every loop. A `LtBranch` opcode
taking the target in one operand and the memType in the other would make it three. A loop
iteration would go from six instructions to five, so about 17% on `empty`. This is the
cheapest remaining win and it is mechanical: append to the `OpCode` enum, add a case, add a
peephole.

**2. The call path.** 13.5 instructions per call, at 15.5 ns each — 210 ns per call against
CPython's 44 ns. The instructions are mostly honest (read a parameter, subtract, call), so the
cost is in `CALL` and `RET` themselves: `frames.push_back`, a full re-snapshot of the frame on
`reload`, and `stackTop` bookkeeping. Profiling this is the next step, not guessing.

Two things to try, in this order: a fixed-capacity frame array with a depth counter instead of
`std::vector<Frame>`, and keeping the caller's `ip` in the callee's frame so that `Ret` does
not have to re-derive it.

**3. `Value` is 32 bytes.** It holds an `Int128`, which forces 16-byte alignment, so a stack
slot is 32 bytes and every push or pop moves that. CPython's stack holds 8-byte pointers.
Making the common case (`int32`, `int64`, `float`, pointer) 16 bytes would halve the traffic on
the hottest data path in the VM. It is also the largest change here: `Value` is the type
everything else is written against.

**4. The page-translated memory model.** Every variable read is
`loadScalar → readBytes → PageStore::get → memcpy`, then a mask and a sign extension. That is
the 30 cycles a `LOAD_LOCAL` costs, and it is a design decision, not an accident: the address
space is sparse and a stray pointer must trap rather than fault. A fast path that recognises
"address is in the current frame, type is 4 or 8 bytes, page is cache-resident" and reads
straight through would cut the common access to a few cycles. It is worth doing carefully —
the checks are the safety property, so they have to stay for every other case.

## A note on the `array` result

`array` reads a global `int[1000]` through `LoadGlobal`, which is a `memcpy` of four bytes and
a sign extension. CPython looks up a `list` object, checks its type, then boxes the result into
a new `PyLong` — and for values below 257 it reuses a candidate, which is why it is as close as
it is. That the two land within noise of each other is a reminder that the gaps are in specific
shapes, not in the interpreter as a whole: one shape can be at parity while a neighbouring one
is five times behind.

## What not to conclude

Three of the four ratios moved by less than the noise when measured by wall clock, while their
instruction counts changed by 18–40%. That is not a contradiction: the machine this was
measured on drifts by ±5% between runs, and a 20% instruction reduction on a program that is
already memory-bound shows up as less than 20% of time. Instruction count is the honest metric
for a change to the instruction stream; wall clock is the honest metric for the user.
