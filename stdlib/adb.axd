//  adb.axd — declarations for the Axiom Debugger's native half.
//
//  `library "adb"` means these functions are implemented in adb.dll / libadb.so,
//  built from stdlib/adb.c.
//
//  ── what a program can and cannot do ──────────────────────────────────────
//
//  A debugger needs to see every instruction *before* it runs, and no Axiom
//  function can arrange that: a call emitted into the program would be part of the
//  program. So the hook lives in the VM and this module is the engine that fills
//  it. Attaching is the driver's job:
//
//      acvm run --adb prog.ax                  interactive session
//      acvm run --adb --break main prog.ax     stop at a function
//      acvm run --adb --adb-script s.txt p.ax  replay a session from a file
//
//  A program only needs the functions below for the things it knows about itself:
//  a breakpoint on a computed name, a capture window around a suspicious section,
//  or a hard stop where a `print` would have gone.
//
//  ── nothing here has an Axiom fallback ────────────────────────────────────
//
//  Debugging *is* the ability to stop the interpreter. Without the library there
//  is nothing to fall back to, so every call reports "not attached" (-1 / 0)
//  instead of pretending. A program that calls `adbTrap()` with no debugger simply
//  carries on -- the alternative (trapping anyway) would turn a diagnostic aid
//  into a crash.

library "adb";

//  1 when a debugger is attached, 0 otherwise. Cheap: one pointer test.
//
//  Use it to guard expensive diagnostics, not to decide whether a program works:
//
//      if (adbIsAttached() != 0) { adbCapture(true); }
function adbIsAttached():int;

//  Adds a breakpoint at run time. `spec` is one of:
//
//      main              a function name
//      main:12           function `main`, instruction 12
//      prog.ax:42        source line 42 in that file
//      :42               source line 42 anywhere in the program
//
//  Returns 0 on success, -1 when the specification is not understood (an unknown
//  function, an instruction past the end, more than 64 breakpoints).
//
//  Resolution happens immediately, so a typo is reported where it is written
//  rather than at the first call that never stops.
function adbBreak(string spec):int;

//  Turns the instruction capture ring on (1) or off (0). Returns the previous
//  state.
//
//  While on, every retired instruction is recorded into a 4096-entry ring, and
//  the ring is printed automatically when the program traps. That is the answer
//  to "what was it doing just before it died", which a breakpoint cannot give
//  after the fact.
//
//  Turn it off again before a hot loop: recording costs roughly twice a bare
//  instruction.
function adbCapture(int enable):int;

//  Stops the program at the next instruction and hands control to the debugger,
//  as if a breakpoint had been hit. Returns 1 when the user asked to abort the
//  run from the prompt, 0 when it should continue, -1 when no debugger is
//  attached.
//
//  This is the supported replacement for `print`-based debugging: the state at
//  the stop is real, and there is no need to recompile to move it.
function adbTrap():int;

//  How many times execution has stopped so far (breakpoints, steps, `adbTrap`).
//  Useful in a capture: `print(adbStopCount())` tells whether the run was ever
//  interrupted.
function adbStopCount():int;
