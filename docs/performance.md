# Dispatch performance

How fast the interpreter runs a program, what was measured, and what is still on the table.

The goal set for this work was CPython 3.13. Arithmetic and array access now beat it; a bare loop
is within the noise of parity; function calls are about two and a half times behind, and that is
the one place the analysis below is still an open problem rather than a solved one.

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
| `empty` | 0.537 s | 1.322 s | 2.46× **faster** |
| `arith` | 0.490 s | 0.813 s | 1.66× **faster** |
| `array` | 0.510 s | 0.477 s | at parity (±10%) |
| `calls` | 3.246 s | 2.244 s | 1.45× slower |

Three of four are at or ahead of CPython. The one that is not does almost nothing but call
functions, and the rest of this document is mostly about why that is the hard one.

Per-instruction cost, computed from the same runs:

| benchmark | instructions | ns/instruction |
|---|---|---|
| `empty` | 100 000 006 | 5.4 (two per iteration) |
| `arith` | 70 000 010 | 7.0 |
| `array` | 36 026 013 | 14.2 |
| `calls` | 399 900 010 | 8.1 |

CPython spends 4.1 ns per bytecode on `calls` (2.418 s for 589M). Axiom's floor on a tight
loop is lower still, and its cost with a call in it is higher — so the interpreter is not
uniformly slower, it is slower exactly where the frame changes. `array` moved the other way in
this revision: the array walk lost fusion opportunities to `RET_LOCAL`'s extra instruction per
call, and its per-instruction cost now reads high because the count fell faster than the time.

## What closed the gap

Two changes, and both of them were about memory rather than instructions.

### Three fusions

`CMP_LOCAL_IMM` is `LOAD_LOCAL slot; CONST c; CMP_BRANCH` in one instruction — the shape of
every loop bound and every `if (n < 2)`. `ADD_SUB_LOCAL_IMM` is the same for
`LOAD_LOCAL; CONST c; ADD|SUB`, which `fib` emits twice per call. `COPY_LOCAL` is
`LOAD_LOCAL src; STORE_LOCAL dst keep`, which is how every scalar `return` writes its value to
the return slot. Together they took `fib` from 665M instructions to 453M.

The instruction count is not the larger half of the gain. The fused forms also stop calling
`computeBinary` — a function with arms for float, pointer, native-width int and generic
128-bit — and stop pushing two 32-byte `Value`s through the stack to compare them. `empty`
went from 1.09× behind CPython to 2.84× ahead on this alone.

### The page lookup

`residentInt32` returns a pointer into a page, and to find that page it called
`PageStore::get`: a modulo, a cache-index compare and a load, on **every local access**. A
frame is one allocation of a few hundred bytes, so every access in a function hits the same
64 KB page — the lookup was answering the same question thousands of times in a row.

It now remembers the last page and compares indices before asking again. Keeping that cache
across a `CALL` rather than dropping it with the frame was worth another 15%, because frames
are contiguous VM memory and consecutive calls usually land in the same page. `PageStore` only
ever `find`s or `emplace`s, so a page is never moved and never freed, which is what makes the
retained pointer safe.

`calls` went from 5.31 s to 3.37 s across the two changes.

## Why `calls` is still 1.39× behind

| | Axiom | CPython 3.13 |
|---|---|---|
| time per call | 63.5 ns | 45.5 ns |
| items per call | 8.5 instructions | 11.1 bytecodes |
| cost per item | 7.4 ns | 4.1 ns |

Axiom executes fewer operations per call and pays more for each. The gap is not uniform: a
tight loop runs at 2.4 ns per instruction, and a loop with a call in it at 7.4. So the cost is
in changing frames — the snapshot the interpreter rebuilds after every `CALL` and every `RET`
(`frames.back()`, the function, its code pointer and size, its constant pool, the base
address) plus whatever the call does to the branch predictor.

Four things were measured this round and did not help:

  * **A smaller `Value`.** 32 bytes to 24 (a `lo`/`hi`/tag struct instead of a 16-byte-aligned
    union) is worth 1.4% on `calls` and 7.6% on `arith`, while making `asInt()` two loads and
    a shift. Reverted. The copy was never the bottleneck.
  * **Deleting `SAFE_POINT`.** `-O4` removes all 53.1M of them, 11.7% of the program, for 0.6%
    of the time. They are the cheapest instruction in the VM.
  * **The JIT.** It refuses `fib` because the first instruction is `CMP_LOCAL_IMM`, and it does
    not know `CompareBranch` or `StepJump` at all — so every program compiled at `-O1` or above
    is invisible to it. On the `-O0` bytecode it *can* compile, it is 40% faster than the
    interpreter and still slower than `-O2` interpretation (4.20 s against 3.28 s), because the
    extra instructions cost more than the compiled code saves.
  * **Hoisting `module.memTypes` or the constant pool out of the dispatch loop.** The compiler
    had already done it; the versions that spelled it out explicitly were measurably slower,
    from register pressure.

And one that worked once its diagnosis was corrected: forwarding the return slot, which
rewrites `STORE_LOCAL 0; JUMP epilogue` / `epilogue: LOAD_LOCAL 0; RET` into one `RET_LOCAL 0`.
It is shipped now, worth 12% of `fib`'s instructions. The note that used to stand here blamed the
three failing `--jit` tests on a `keep` bit leaving a stack the compiled frame disagreed about.
That was wrong. The rewrite was correct; the JIT was silently dropping every instruction the
optimiser had fused (`effectOf` returned "unknown", the depth analysis skipped them rather than
refusing, and the lowering emitted the body up to them) and marking the function compiled
anyway. `chanTryPush` reported a missing return value because the instruction that produced it
had never been emitted.

## Why it was slow, and why the first answer was wrong

The first round of this work reduced instruction counts by 60% on `empty` and produced 11% of
time. That ratio — a big drop in instructions, a small drop in seconds — was the actual finding,
and it took a second round to read it correctly.

**Instruction count had stopped predicting time.** Fitting time against instruction count over six
variants of the same loop (0 to 5 extra statements in the body, 20M iterations each) gave:

| body statements | instructions | time | ns/instruction |
|---|---|---|---|
| 0 | 80 000 014 | 1.188 s | 14.85 |
| 1 | 160 000 014 | 1.893 s | 11.83 |
| 2 | 220 000 014 | 2.315 s | 10.52 |
| 3 | 280 000 014 | 2.863 s | 10.23 |
| 4 | 340 000 014 | 3.321 s | 9.77 |
| 5 | 400 000 014 | 3.760 s | 9.40 |

The marginal instruction costs **8.05 ns**. The four-instruction loop body cost **60.9 ns**, which
at the marginal rate would be 32 ns. Twenty-nine nanoseconds per iteration were unaccounted for,
and the amount did not change when the body grew — so it belonged to the control sequence, not to
instruction count.

Ablating that sequence opcode by opcode put **30 ns on `STEP_JUMP` alone**. A single `i++` and a
jump, at 90 cycles. And the confirmation: each fused opcode replaced three or four general ones
while costing three times as much as each of them —

| shape | instructions per iteration | time |
|---|---|---|
| `a++` at `-O0` (LOAD/ADD/STORE) | 7 | 90.2 ns |
| `a++` at `-O1` (fused `STEP_LOCAL`) | 5 | 96.7 ns |
| empty loop at `-O0` | 6 | 59.2 ns |
| empty loop at `-O1` (`CMP_BRANCH`+`STEP_JUMP`) | 4 | 57.0 ns |

Fusing `a++` made it **slower**. The fusions were hiding work from the instruction count, not from
the clock.

### What the opcodes were doing

`STEP_JUMP`, `STEP_LOCAL`, `LOAD_LOCAL` and `STORE_LOCAL` all went through the general scalar path:

```
memType (bounds check)
  -> loadScalar (page lookup, memcpy, mask, sign-extend)
    -> Value (32 bytes)
      -> computeBinary (a large non-inlined call, with float/pointer/int/native-width/generic arms)
        -> storeScalar (the mirror image)
```

Twenty-seven to thirty cycles to add one to an `int32`. Nothing there is wrong — every step is
needed for *some* type — but a loop counter is the one case that needs none of it.

`VM::residentInt32` now hands back a direct pointer when the slot is a 4-byte signed integer, in a
page that already exists, not straddling a page boundary and not misaligned. On that path
`STEP_JUMP` is an add, an overflow check and a store. Null means "take the general path", so the
shortcut is an optimisation and never the definition of the behaviour: a slot in a page that was
never written still reads as zeros, a misaligned slot still goes through `memcpy`, and a counter
that overflows still traps where it did before (verified: `int i = 2147483646; for (; i < INT32_MAX;
i++)` prints the last value then traps).

`STEP_JUMP` alone took `empty` from 2.653 s to 1.679 s. The same shortcut on `LOAD_LOCAL`,
`STORE_LOCAL` and parameter passing took the rest.

## Why `calls` is still 2.4× behind

This one is now almost entirely a per-instruction story, and the numbers separate cleanly.

| | Axiom | CPython 3.13 |
|---|---|---|
| time per call | 101 ns | 47.6 ns |
| items per call | 12.5 instructions | 11.1 bytecodes |
| cost per item | 8.05 ns | 4.3 ns |

**The call mechanism itself is faster in Axiom.** Measured against the same loop without a call:
Axiom pays 13 ns per call, CPython pays 21.9 ns — a frame push and pop against
`_PyEvalFramePushAndInit` with its freelist handling.

So the 2.4× is `12.5 × 8.05` against `11.1 × 4.3`. Axiom executes a comparable number of
operations and pays about twice as much for each. Two reasons remain:

1. **A `Value` is 32 bytes, CPython's stack slot is a pointer.** Every push and pop moves 32 bytes.
   `CONST_INT` is 16% of `fib`'s instructions and it is nothing but a 32-byte push.
2. **`computeBinary` is one large function** with arms for float, pointer, native-width int,
   generic 128-bit and the six comparisons. It cannot be specialised per call site, so every
   arithmetic instruction pays its branch ladder. CPython's specialised `BINARY_OP` variants are
   separate code paths chosen once, at the call site.

Both are addressable, and neither was attempted here. The first is the `Value` change this document
has been deferring; the second is specialization.

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

## The JIT after the comparison fix

The JIT used to be slower than the interpreter on every benchmark in this file, which made it
a demonstration rather than a tier. It is not any more, on everything except calls.

| benchmark | interpreter | `--jit` | |
|---|---|---|---|
| `empty` | 0.406 s | 0.119 s | **3.4× faster** |
| `arith` | 0.518 s | 0.042 s | **12.2× faster** |
| `array` | 0.311 s | 0.315 s | unchanged |
| `calls` | 3.305 s | 4.805 s | **0.69× — slower** |

Two causes, and only one of them was a missing lowering.

`CMP_LOCAL_IMM` read its frame slot from `a`. But `a` is the jump target -- this instruction is
a branch, like every other branch in this set -- and the slot is packed into `b` with the
constant and the comparison. So a compiled comparison read whatever local happened to live at
the *target index*. For `ternary_check` that was a neighbouring variable holding 5, so
`-1 > 0` evaluated true and the false arm was unreachable: `expected 5, got 3`. The bug had been
attributed to the value stack disagreeing between the interpreter and compiled code, which was
wrong; the disassembly showed `frame+4` and the lowering was reading `frame+12`.

With the slot correct, the loop header compiles, and that is where the 3–12× comes from. `array`
is unchanged because it walks a global array (`LOAD_GLOBAL`), which is still a hand-off point.

`calls` is slower for a different reason: **every `CALL` in compiled code goes through a C++
bridge.** Arguments are pushed onto the interpreter's value stack, `callFunction` pops them,
looks the callee up, zeroes its frame and calls its entry. That is about 29 ns per call, and at
53.1M calls it more than wipes out what the compiled body saves. A direct call path is the next
piece of work; see the list above.

## What did not work, this round

Two changes from the list, both implemented, measured and dropped.

**Caching the frame's decoded state.** `VM::Frame` holds a `Function*`, and the interpreter
reaches `code.data()`, `code.size()` and `constants.data()` through it -- a five-deep dependent
load chain on every `CALL` and every `RET`. Caching all three in the frame (24 more bytes per
frame) measured **1.3% slower**, not faster: the chain was already resolved out of L1 by the
out-of-order engine. Reverted. What survived is `Frame::of`, because three sites were assembling
a frame by hand and a fourth would have got it wrong.

**Removing work from `CALL`'s body.** Ablating the argument store cost nothing (3.232 s to
3.251 s), ablating `zeroBytes(frameBase, frameSize)` cost nothing (3.253 s to 3.262 s), and
`-O4`, which deletes all 53.1M safe points, is worth 3.3%. Whatever the call path's cost is, it
is not in the work those functions do -- which is consistent with the bridge finding above: it is
in the transfer, not the setup.

## What is still on the table

In the order I would do them, and with the reason each is not already done.

**1. Give compiled code a direct call path.** Every `CALL` in compiled code marshals its
arguments onto the interpreter's value stack and goes through `jitbridge::callFunction`, which
looks the callee up and sets up its frame. That is roughly 29 ns of overhead per call, and it is
why compiled `fib` is *slower* than interpreted `fib` (4.81 s against 3.31 s for 53.1M calls)
once the JIT can compile it at all. Inlining the frame setup and jumping straight to the
callee's entry would fix it; it is also the largest remaining piece of work in the JIT, because
the callee's prologue, the frame zeroing and the argument copy all have to agree with what
`callFunction` does today.

**2. `CMP_BRANCH` in the JIT.** Still handed to the interpreter, for the reason recorded below
under "what did not work". `CMP_LOCAL_IMM` was in the same state until its operand bug was found,
and fixing that one turned the JIT from "never faster than the interpreter" into 3.4× on a bare
loop and 12× on an arithmetic one. `CMP_BRANCH` is the same opportunity for branches whose
operand is not a local plus a constant.

**3. Promote more than one slot.** One frame slot's address is cached in `rbp`; worth 11.8% on a
compiled arithmetic loop and nothing on the others. There are no more free callee-saved
registers, so a second slot means spilling something else -- which is where this stops being a
cheap trick and becomes an allocator.

**3. Promote hot frame slots into registers inside the JIT.** The one cheap version of a
register machine, and the one worth doing — see `docs/registers.md`, which also records why a
full register VM is not worth it here.

**4. Specialise the remaining arithmetic.** `ADD` and `SUB` still go through `computeBinary`.
They are 6.7% of `fib`'s instructions now that `ADD_SUB_LOCAL_IMM` exists, so this is worth a few
percent rather than the 20% an earlier draft of this document guessed.

**Done since the last revision:** `forwardReturnSlot`, in the form of `RET_LOCAL` — 453M
instructions to 400M on `fib`, 12% of its time. The `keep`-bit version described in earlier
revisions of this file is gone, replaced by an instruction that says one thing instead of two.

**4. Specialise the remaining arithmetic.** `ADD` and `SUB` still go through `computeBinary`.
They are only 5.9% of `fib`'s instructions now that `ADD_SUB_LOCAL_IMM` exists, so this is worth
a few percent rather than the 20% the previous draft of this document guessed.

What is **not** on the list, because it was measured and did not work: shrinking `Value`
(1.4%), deleting `SAFE_POINT` (0.6%), and hoisting the module-level tables by hand (the compiler
had already done it, and spelling it out was slower).

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

**Instruction count is not a proxy for time, and this document has the receipts.** A 60% cut in
`empty`'s instructions bought 11% of its time, because the instructions that remained were the
expensive ones. A later round cut 0% of the instructions and took 37% of the time, by making the
same four instructions cheaper.

Both numbers are honest, and they answer different questions. Instruction count says how much work
the program asked for. Wall clock says how much it cost. A change can improve either without the
other, so a report that shows one and infers the other is not a measurement — it is a guess with a
number attached. Every claim in this document is one or the other, labelled, and where the two
disagreed the disagreement is written down rather than smoothed over.

The machine also drifts by ±10% between sessions, so the ratios are given as ranges across
consecutive runs. A single number would look more precise and mean less. One measurement trap
worth naming, because it happened here: a table of ratios was collected against a stale binary —
`build/` had been rebuilt and `build-lto/` had not — and reported `calls` at 1.55× when the
optimised build was at 1.39×. The tell was that a direct timing of the same program disagreed
with the table. When two measurements of the same thing disagree, one of them is measuring
something else.
