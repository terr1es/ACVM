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
| `empty` | 1.365 s | 1.280 s | 1.07× slower |
| `arith` | 0.698 s | 0.777 s | 1.11× **faster** |
| `array` | 0.338 s | 0.394 s | 1.17× **faster** |
| `calls` | 5.226 s | 2.090 s | 2.50× slower |

Two consecutive runs, so the spread is visible rather than hidden behind one number:

| benchmark | ratio, two runs |
|---|---|
| `empty` | 1.17×, 1.07× |
| `arith` | 0.79×, 0.90× |
| `array` | 0.85×, 0.86× |
| `calls` | 2.40×, 2.50× |

Arithmetic and array access are now faster than CPython rather than level with it. `empty` is
within the noise of parity. `calls` is the one benchmark still clearly behind, and the rest of
this document is largely about why.

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

## What is still on the table

In the order I would do them, most valuable first.

**1. `Value` is 32 bytes.** This is now the top item, and the reason is quantitative rather than
aesthetic: it is the remaining explanation for `calls` being 2.4× behind. It holds an `Int128`,
which forces 16-byte alignment, so a stack slot is 32 bytes and every push or pop moves that.
CPython's stack holds 8-byte pointers. `CONST_INT` alone is 16% of `fib`'s instructions and does
nothing but push 32 bytes. Making the common case (`int32`, `int64`, `float`, pointer) 16 bytes
would halve the traffic on the hottest data path in the VM. It is also the largest change here:
`Value` is the type everything else is written against, and the 128-bit cases would have to move
to a side path. The previous round deferred it on the grounds that memory access was the real
cost; that turned out to be right, and it has now been fixed, which promotes this to the front.

**2. Specialise arithmetic.** `computeBinary` is one function with arms for float, pointer,
native-width int, generic 128-bit and the six comparisons. Every arithmetic instruction pays that
branch ladder because the arms cannot be selected per call site. CPython's specialised
`BINARY_OP_INT_ADD` and friends are separate code paths, chosen once. Two obvious cases here:
`ADD`/`SUB` with a memType of `int32` are almost all of real arithmetic and could be a single
compare-and-add at the top of the dispatch arm, before the general function is called.

**3. `SAFE_POINT` before every `CALL`.** 8% of `fib`'s instructions. It is cheap now — one branch
when nothing is watching — but it is still a dispatch. The predicate is computed from four members
on every execution; caching it in one `bool` that is updated at the four places those members
change would make it a single load. The reason it was written as a recomputation is recorded in
`VM.h`, and any caching has to update it at `setDebugHooks`, at process creation, when the JIT
raises `m_frameFloor`, and on module load.

**4. A comparison with an immediate.** `LOAD_LOCAL i; CONST_INT 2; CMP_BRANCH` is 24% of `fib`'s
instructions and does nothing but ask "is `i` less than two". An opcode carrying the immediate in
its own operand would make it one instruction, saving 16% of `fib` and about the same in any loop
with a constant bound.

**5. The frame re-snapshot on call and return.** Every `CALL` and every `RET` sets `reload`, which
unwinds to the top of the interpreter loop and re-derives the frame reference, the code pointer and
the base address. Twice per call. The cost did not show up in the ablation (the call mechanism
measures faster than CPython's) so it is not the priority the earlier draft of this document
guessed it was — but it is still two full re-snapshots per call that a fixed frame array could
avoid.

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
consecutive runs. A single number would look more precise and mean less.
