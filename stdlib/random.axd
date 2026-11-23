//  random.axd — declarations for the random library's native half.
//
//  `library "random"` means these functions are implemented in random.dll
//  (Windows) / librandom.so (Linux), built from stdlib/random.c:
//
//      function import (return:(random));
//
//      function main():void {
//          unsigned int seed[1] = {12345u32};
//          int data[100] = {0};
//          for (int i = 0; i < 100; i++) { @(#data[0] + i) = i; }
//          shuffle(#data[0], 100, #seed[0], seed.length);
//          print(randBelow(#seed[0], seed.length, 6));   // a die roll
//          eof: pass;
//      }
//
//  All of it has an Axiom body in stdlib/random.ax, so a program runs either way.
//
//  ── the state is yours ────────────────────────────────────────────────────
//
//  No globals: the generator state lives in a buffer you pass in, because the VM
//  can run several `process` functions and a shared seed would make results depend
//  on interleaving. The buffer must hold at least one `unsigned int`.
//
//      unsigned int seed[1] = {12345u32};
//      unsigned int r = randNext(#seed[0], seed.length);
//
//  Same seed, same sequence -- every draw is reproducible, which is the point of
//  seeding it explicitly rather than reading the clock.
//
//  ── what this is not ──────────────────────────────────────────────────────
//
//  xorshift32: period 2^32-1, adequate for shuffling, sampling and test data,
//  NOT for anything cryptographic. A 64 bit generator would need wrapping 64 bit
//  multiplication, which Axiom traps on as overflow, so the C and Axiom versions
//  could not agree.

module "random" version "0.1.0";

library "random";

//===--- operating-system entropy -----------------------------------------===//

//  Fills `buffer` with `count` unpredictable bytes. Returns 0, or -1 when the
//  platform refuses.
//
//  **This has no Axiom fallback.** Every other function in this module is
//  reproducible from a seed and therefore implementable twice; entropy is not --
//  obtaining it is a syscall, and a language cannot evaluate one. On a build
//  without random.dll the call returns -1 rather than falling back to the
//  xorshift generator. A silent downgrade from unpredictable to predictable is the
//  one failure mode a security primitive must never have.
//
//  Use this for session tokens, salts, nonces, UUIDs and anything an attacker must
//  not guess. Use `randNext` for shuffling, sampling and test data -- it is faster
//  and reproducible, which is what those want.
//
//      unsigned int8 token[32] = {};
//      if (randomSecure(#token[0], 32) != 0) {
//          print("no entropy available");
//      }
function randomSecure(ptr unsigned int8 buffer, int count):int;

//  Advances the state and returns the next value. A zero state is replaced with a
//  non-zero constant on the first call, because all-zero is xorshift's fixed point.
function randNext(ptr unsigned int state, int stateCapacity):unsigned int;

//  Uniform value in [0, bound). Returns 0 when `bound` is not positive. Uses
//  rejection sampling, so the distribution has no modulo bias.
function randBelow(ptr unsigned int state, int stateCapacity, int bound):int;

//  Uniform value in [low, high). Returns `low` when the range is empty.
function randRange(ptr unsigned int state, int stateCapacity, int low, int high):int;

//  Shuffles `count` elements in place (Fisher-Yates). Every permutation is equally
//  likely, and exactly `count - 1` values are drawn from the generator, so the
//  state advances by a predictable amount.
function shuffle(ptr int data, int count, ptr unsigned int state, int stateCapacity):void;

//  FNV-1a fingerprint of the array's values. The result is an `unsigned int` on
//  purpose: Axiom's `as int` is a checked conversion that traps on overflow, so
//  narrowing a 32 bit hash to `int` would abort the program for roughly half of
//  all inputs.
//  Good enough for hash tables and change detection, not for security. Hashes the
//  elements rather than their bytes, because Axiom's `char` is a UTF-16 code unit
//  and cannot express a byte view.
function hashInts(ptr int data, int count):unsigned int;
