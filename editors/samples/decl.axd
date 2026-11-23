//===--- decl.axd - Sample declaration file -----------------------------===//
//
//  A `.axd` file is an Axiom declaration file: it holds signatures only, never
//  implementations. The `library "..."` line at the top marks the whole unit
//  as external declarations, and every bodyless function below is expected to
//  be provided by that shared library at run time.
//
//  This file exists so the syntax highlighting of `.axd` can be inspected in
//  one screen: it uses the same language as `.ax` (language id `axiom`), and
//  the only construct unique to it is `library`.
//
//  Open it next to samples/selftest.ax and compare colours; they should match.

library "sample";

//  Every declaration here is bodyless on purpose: an implementation would make
//  this an ordinary `.ax` file and the compiler would reject the `library` line
//  at link time.

//  Adds `count` integers at `data`, in place.
function nativeAdd(ptr int data, int64 count):int64;

//  Sums `count` integers at `data`.
function nativeSum(ptr int data, int64 count):int64;

//  Reverses `count` integers at `data`, in place.
function nativeReverse(ptr int data, int64 count):int64;

//  A declaration file may still contain struct and enum definitions: they carry
//  no code, so they cost nothing and let both sides agree on the layout.
struct SampleHeader {
    int32 version;
    int32 flags;
    int64 payload;
};

enum SampleKind {
    KIND_A = 1,
    KIND_B = 2,
};
