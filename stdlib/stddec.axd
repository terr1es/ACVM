//  stddec.axd — declarations for the shared part of the standard library.
//
//  This one file declares every C function the standard library implements in a
//  shared library. Axiom code reaches them by importing this module:
//
//      function import (return:(stddec));        // resolves to stdlib/stddec.axd
//
//      function main():void {
//          int values[500];
//          ...
//          nativeSort(#values[0], 500);
//          eof: pass;
//      }
//
//  `library "stdlib"` names the shared object that implements them: stdlib.dll
//  on Windows, libstdlib.so on Linux, libstdlib.dylib on macOS. It is built from
//  stdlib/stdlib.c and loaded when the first extern function is called, so a
//  program that never imports this module needs no library at all.
//
//  What is NOT here: the compiler primitives (`sortInts`, `sortFloats`,
//  `reverseInts`, `print`, `alloc`, ...). Those are linked into acvm.exe and
//  need no library; they are implemented in stdlib/stddec.c. This file is the
//  other mechanism -- see the header of stdlib/stdlib.c.
//
//  Adding a function: implement it in stdlib/stdlib.c with C linkage, declare
//  it here, rebuild the library. No compiler change, no acvm.exe rebuild.

library "stdlib";

// ─── arrays of int ─────────────────────────────────────────────────────────
//
//  `ptr int data` immediately followed by an integer count is the array form:
//  the VM copies those elements into one contiguous buffer, passes
//  (buffer, count), and copies the buffer back after the call. A function may
//  therefore modify the caller's array in place, exactly like a C `int*`.
//
//  Parameters are declared `int` rather than `int64` on purpose: Axiom does not
//  widen a 32 bit value to 64 implicitly, so `int64` parameters would force a
//  cast at every call site. A count is bounded by the array length anyway, and
//  the C side still receives a full 64 bit register -- it declares `int64_t` and
//  the VM sign extends the `int` into it.
//
//  Results are int64: 0 for success, non-zero for a failure the caller should
//  look at. `void` results work the same way, the value is simply dropped.

//  Sorts `count` integers at `data` ascending, in place. Returns 0.
function nativeSort(ptr int data, int count):int64;

//  Sorts `count` integers at `data` descending, in place. Returns 0.
function nativeSortDesc(ptr int data, int count):int64;

//  Reverses `count` integers at `data`, in place. Returns 0.
function nativeReverse(ptr int data, int count):int64;

//  Returns the sum of `count` integers at `data`. 64 bit, so the total cannot
//  overflow before the element count itself becomes unreasonable.
function nativeSum(ptr int data, int count):int64;

//  Returns the largest of `count` integers, or 0 when `count` is 0.
function nativeMax(ptr int data, int count):int64;

//  Returns the smallest of `count` integers, or 0 when `count` is 0.
function nativeMin(ptr int data, int count):int64;

//  Returns the number of elements equal to `value` (linear scan).
function nativeCount(ptr int data, int count, int value):int64;

// ─── scalars ───────────────────────────────────────────────────────────────
//
//  Scalars cross the boundary in one 64 bit register. A scalar `double` cannot:
//  the ABI would put it in an SSE register, so the compiler rejects it. Use
//  `ptr double` for arrays of doubles.

//  Clamps `value` into [low, high]. Returns the clamped value.
function nativeClamp(int value, int low, int high):int64;

//  Greatest common divisor of two non-negative integers.
function nativeGcd(int a, int b):int64;
