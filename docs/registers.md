# A register machine: an assessment

The question was whether Axiom's VM should be register-based rather than stack-based. This is
the answer I would give with the measurements in hand, including the part where the answer
turned out to be "not for the reason you would expect".

## What a register machine buys, in general

The textbook argument is about instructions, not about registers. `a = b + c` is one
instruction in a register machine and four in a naive stack machine:

```
stack:    LOAD b; LOAD c; ADD; STORE a
register: ADD r1, r2, r3
```

Fewer instructions, and no temporary traffic through a stack. Both matter, and on a
straightforward interpreter they are worth a lot -- CPython 3.11's specializing adaptive
interpreter got most of its speed from exactly this direction, and Lua 5.x is register-based
for the same reason.

## What it would buy here

Fusion has already taken most of it.

Axiom's optimizer replaces the stack idioms with instructions that read and write frame slots
directly. The profile of `fib(10)` called 300 000 times, after that work:

| opcode | share |
|---|---|
| `CALL` | 13.3% |
| `SAFE_POINT` | 13.3% |
| `RET_LOCAL` | 13.3% |
| `ADD_SUB_LOCAL_IMM` | 13.2% |
| `LOAD_LOCAL` | 6.8% |
| `ADD` | 6.7% |
| `JUMP` | 6.7% |
| `CONST_INT`, `STEP_JUMP`, `BUILTIN` | 0.1% |

`STORE`, `POP`, `SWAP` and `DUP` do not appear at all. The four fused forms are, in effect,
already register instructions: `ADD_SUB_LOCAL_IMM r1, #1` writes a slot and leaves the result
in a slot, which is what `ADD r1, r1, 1` is. The stack still exists, but the hot path no longer
travels through it.

So the instruction-count argument is mostly spent. What is left is the cost of the dispatch
itself -- and there the numbers say something else:

| | Axiom | CPython 3.13 |
|---|---|---|
| time per call | 63.5 ns | 45.5 ns |
| instructions per call | 8.5 | 11.1 |
| cost per instruction | 7.4 ns | 4.1 ns |
| cost per instruction, tight loop | 2.4 ns | — |

Axiom runs **fewer** instructions per call than CPython and pays **more** for each. A tight
loop costs 2.4 ns per instruction; a loop with a call in it costs 7.4. The difference is not
stack traffic -- it is the frame switch, and the branch-predictor and cache damage that comes
with it. A register machine does not remove a frame switch. It might make it slightly cheaper
to save and restore (fewer live values, if the allocation is good) or slightly more expensive
(more live values to spill, if it is not).

**That is the honest summary: the change would buy a few percent on instruction count and
nothing measurable on the thing that is actually slow.**

## What it would cost

Not a rewrite of the VM. A rewrite of everything that touches bytecode, which is most of the
tree:

- `IRGen` and its expression emitter -- everything that pushes and pops.
- Every optimizer pass. The stack machine's invariants ("this sequence leaves the stack one
  deeper") are what most of the peepholes are written against; `jumpTargets`, `writesSlot`,
  `compact` and the depth analysis in the JIT all assume a stack.
- The JIT, which is a translator from stack bytecode to native code. It would become a
  translator from register bytecode -- easier in principle, but it is 1700 lines of working
  code that would be rewritten.
- The serializer and every `.acvm` file ever written. The `OpCode` enum's values are what a
  module stores, which is why this tree appends and never inserts.
- 124 tests, of which every one that inspects the disassembly or the instruction count would
  need its expectations rewritten.

There is no incremental version of that. A bytecode format is either stack or register.

## The one cheap version of the idea

If the goal is "fewer stack operations" rather than "a register machine", there is a
targeted version worth trying, and it is small: **promote the hottest frame slots into
registers across a basic block, inside the JIT.**

The JIT already assigns a native register to the page base and keeps the operand stack in
native slots. Extending that to say "frame slot 4 lives in `rbx` for the next twelve
instructions" is local, does not change the bytecode format, and is where the remaining 7.4 ns
per instruction actually goes. It would be measured the same way everything else here was.

That is the version I would do. It gets the benefit the question was reaching for without
committing the compiler to a format it cannot back out of.

## The recommendation

**Do not convert to a register machine.** Not because register machines are worse -- they are
better for this kind of workload in general -- but because:

1. The advantage has already been collected by fusion. Stack temporaries are under 1% of the
   instructions in the hot path.
2. The remaining gap is frame switching, which registers do not fix.
3. The cost is the whole bytecode layer, with no partial version and no way back.

If the measurement ever changes -- if a future profile shows `LOAD`/`STORE` traffic back above
10% -- the answer should be revisited, because the reasoning above is tied to these numbers.
