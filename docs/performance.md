# Dispatch performance

How fast the interpreter runs a program, what was measured, and what is still on the table.

The goal set for this work was CPython 3.13. Arithmetic and array access reached parity;
function calls are still about three times behind. Everything below is measured rather than
estimated, and the numbers that are exact (instruction counts) are marked as such.

## Method

Four programs, each available in both languages, in `bench/` beside this file:

| benchmark | shape |
|---|---|
| `empty` | `for (int i = 0; i < 50'000'000; i++) { }` |
| `arith` | 5M iterations of two moduli, a multiply, a subtract |
| `calls` | `fib(10)` called 300 000 times — 53.1M calls |
| `array` | 2 000 × 1 000 reads from a 1 000 element array |

Wall-clock is the minimum of five runs, with the two languages alternating so that drift in
machine load cancels. CPython is 3.13.14, the build on this machine. Instruction counts come
from `--stats` and are exact and reproducible; where the two disagree about a change, the
counts are the ones to believe — the wall-clock numbers move by ±10% between sessions.

## Where it stands

| benchmark | Axiom | CPython 3.13 | ratio |
|---|---|---|---|
| `empty` | 2.653 s | 1.213 s | 2.19× slower |
| `arith` | 0.832 s | 0.734 s | 1.13× slower |
| `array` | 0.421 s | 0.421 s | at parity (±10%) |
| `calls` | 6.300 s | 2.137 s | 2.95× slower |

Three consecutive runs, so the spread is visible rather than hidden behind one number:

| benchmark | ratio, three runs |
|---|---|
| `empty` | 2.19×, 2.18×, 2.17× |
| `arith` | 1.13×, 0.84×, 1.11× |
| `array` | 1.00×, 0.89×, 0.96× |
| `calls` | 2.95×, 2.97×, 2.83× |

`arith` and `array` sit on either side of CPython depending on the run, so they are reported as
parity rather than as wins. `empty` and `calls` are consistently behind, and consistently by
about that much.

Per-instruction cost, computed from the same runs:

| benchmark | instructions | ns/instruction |
|---|---|---|
| `empty` | 200 000 008 | 13.3 |
| `arith` | 80 000 012 | 10.4 |
| `array` | 40 036 017 | 10.5 |
| `calls` | 665 400 012 | 9.5 |

CPython spends about 13.7 ns per bytecode on `empty` (1.213 s for 50M iterations of two
bytecodes). Axiom's dispatch is therefore in the same range as CPython's per operation, and often
cheaper. **What remains is mostly the number of instructions per unit of work**, not the cost of
one — with `empty` as the exception, discussed below.

## What changed

### Instruction counts

| benchmark | before | after | change |
|---|---|---|---|
| `empty` | 500 000 009 | 200 000 008 | −60.0% |
| `arith` | 110 000 013 | 80 000 012 | −27.3% |
| `array` | 48 046 019 | 40 036 017 | −16.7% |
| `calls` | 906 000 013 | 665 400 012 | −26.6% |

A loop iteration went from ten instructions to four:

```
0002  LOAD_LOCAL   frame+0 i32
0003  CONST_INT    [1] = 50000000
0004  CMP_BRANCH   i32 < else goto 6
0005  STEP_JUMP    frame+0 i32 += 1 goto 2
```

Five separate causes, in the order they were found.

**1. The peephole pass was doing nothing.** `--stats` reported `0 rewrites` on every program,
including ones with obvious redundancies. The cause was one guard at the top of the pass:

```cpp
if (targets[i] || (i + 1 < n && targets[i + 1])) { ++i; continue; }
```

It rejected any position where a jump landed. That is right for a rule that *removes* an
instruction — the jump operand is an index, so the jump would arrive at whatever moved into its
place — and wrong for a rule that *rewrites in place*, which is what a fusion is. The epilogue of
every function is a jump target, so `LocalAddr slot; Load; Ret` was never fused, and neither was
anything downstream of that.

**2. `return` cost seven instructions.** The return value went through a frame slot: store it,
jump to the epilogue, load it back, return. Adding `LocalAddr x; Swap; Store` → `StoreLocal x`
and letting the existing address-then-dereference rule fire at a target brought it to four.

**3. Every loop paid five instructions for its own counter.** `for (...; ...; i++)` emitted the
increment through the expression path — `LoadLocal; ConstInt; Add; StoreLocal keep; Pop` —
because only `StmtKind::ExprStmt` knew the result was discarded. The third clause of a `for` has
no reader either. Both paths now share `emitDiscardedExpr`, which collapses to `StepLocal`.

**4. The loop header and the loop tail are now one instruction each.** `CMP_BRANCH` replaces a
comparison followed by a conditional jump, with the comparison and the direction packed into one
operand; `STEP_JUMP` replaces `STEP_LOCAL; JUMP` for a step of ±1. Both are appended at the end
of the `OpCode` enum, because the enum's values are what a serialized module stores.

**5. The retired-instruction counters are optional.** `m_instructions` and `m_opcodeCounts` were
maintained on every instruction so that `--stats` would have them. The comment above them said
the update was "below the noise floor". It was not: removing it from the default path is worth
9–11%. They are now maintained only after `setProfile(true)`.

### The call path

`calls` went from 11.1 s to 6.3 s — 44% off the largest remaining gap. Three causes, and none of
them was the one I expected.

**`zeroBytes` allocated on every call.** It built a `std::vector<uint8_t>` of zeros to copy from:
a heap allocation and a free for every frame a call created, to `memset` eight bytes. Measured by
ablation — an empty function called 20 000 000 times took 2.26 s, and removing only this
allocation took it to 1.48 s. **Thirty-four percent of every call in the program was an allocator
round trip spent preparing to write zeros.**

**The arguments were copied through a `std::vector<Value>`.** A second allocation and free per
call. They sit at the top of the value stack in order, and the frame they are copied into lives
in VM memory — a different address space, so writing the frame cannot disturb them. The copy now
reads them where they are.

**`RET` popped the result and pushed it back.** Two operations that cancel, the second of which
re-checks capacity. The result is already on top of the stack; only the frame stack has to change.

`MEM_COPY` and `MEM_EQ` had the same allocation shape and were rewritten the same way — page to
page, no staging buffer. `MEM_COPY` is how a struct is assigned, so `a = b` for two 40-byte
structs was two allocations, one read pass and one write pass.

### Per-instruction cost

**Link-time optimisation for release builds**, so the dispatch loop can see through to the memory
model it calls: 2.6–6%.

**A `SafePoint` no longer leaves the interpreter when nothing is watching.** With one process, no
events and no debugger, the scheduler had nothing to hand control to, but returning cost a frame
write, an unwind, a call to `evaluateEvents` that returned immediately, and a re-entry. About 3%
on `calls`.

**The arithmetic group reads its operands in place.** `Value` is 32 bytes — an `Int128` plus a
tag, rounded up by the 16-byte alignment the 128-bit type forces — so popping both operands into
locals was two 32-byte copies, and pushing the result a third. The result is now written over the
left operand. Worth 5–12% depending on the benchmark.

## What is still on the table

**1. `empty` costs 13.3 ns per instruction while everything else costs 9.5–10.5.** That is the
honest anomaly in the table. A four-instruction loop body does nothing but move values through
the stack: two `LOAD_LOCAL`s, a comparison and a step, each touching a page-translated frame slot
and copying 32-byte `Value`s. The other benchmarks have real arithmetic to amortise that against.
Closing it means making the common memory access cheaper, not making dispatch cheaper.

**2. `Value` is 32 bytes.** It holds an `Int128`, which forces 16-byte alignment, so a stack slot
is 32 bytes and every push or pop moves that. CPython's stack holds 8-byte pointers. Making the
common case (`int32`, `int64`, `float`, pointer) 16 bytes would halve the traffic on the hottest
data path in the VM. It is also the largest change here: `Value` is the type everything else is
written against, and the 128-bit cases would have to move to a side path. Not attempted in this
round — the measurement above says to fix memory access first, because that is where the 30-odd
cycles per slot actually go.

**3. The page-translated memory model.** Every variable read is
`loadScalar → readBytes → PageStore::get → memcpy`, then a mask and a sign extension. A fast path
that recognises "address is in the current frame, type is 4 or 8 bytes, page is cache-resident"
and reads straight through would cut the common access to a few cycles. Worth doing carefully: the
checks are the safety property — a stray pointer must trap rather than fault — so they have to
stay for every other case.

**4. The call path, again.** 9.5 ns per instruction at 12.5 instructions per call is about 119 ns
per call, against CPython's 40 ns. The remaining cost is frame setup: `frames.push_back`, a full
re-snapshot of the interpreter state on `reload`, and `stackTop` bookkeeping. Worth profiling
before touching.

## Two bugs found by the work itself

Both are kept as comments where they happened, because both are the kind that ships.

**A mis-packed operand type truncated `int64` to 32 bits.** The first `LocalAddr; Swap; Store`
fusion copied `Store`'s type from `b` instead of `a`. `Store` keeps its memType in `a`;
`StoreLocal` keeps it in `b`. Copying the wrong one left the type zero, which is a 32-bit store,
so `map_check` reported `expected 9000000000000, got 2043514880`. Same class of mistake as an
earlier attempt at fusing `LocalAddr; Store` directly, which stored a pointer over the value being
stored.

**`StepJump` was not registered as a branch.** It carries its target in `a` like every other
branch, so three separate places that ask "is this a jump" — `jumpTargets`, `compact`, and the
predecessor graph — all got it wrong. The symptom was not a wrong jump but a *deleted program*:
`removeUnreachable` walked from index 0, saw no branch out of the loop, and removed everything
after it. The fix put the question in one predicate (`isJump`) that every pass asks, and the same
for "which slot does this write" (`writesSlot`).

## Tests that were added

- `compare_branch_check.ax` — every comparison, at 32-bit, 64-bit and float, on both sides of
  equality, through the fused form: 18 fusions. Verified by breaking the operand packing and
  watching it fail.
- `optimize_check` — asserts the `-O2` disassembly contains `STEP_LOCAL`/`STEP_JUMP` and contains
  no `LOCAL_ADDR`/`SWAP`. Every other test compares what a program prints, and a pass that
  rewrites nothing keeps the output identical by construction.
- `size_check`, `doc_check`, `init_check`, `test_cmd_check` — the reporting and scaffolding
  commands.

## What not to conclude

Three of the four ratios moved by less than the noise between sessions, while their instruction
counts changed by 17–60%. That is not a contradiction: this machine drifts by ±10%, and a 25%
instruction reduction on a program that is partly memory-bound shows up as less than 25% of time.
Instruction count is the honest metric for a change to the instruction stream; wall clock is the
honest metric for the user. Both are reported here, and neither is presented as the other.
