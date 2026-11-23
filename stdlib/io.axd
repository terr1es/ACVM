//  io.axd — the part of the I/O library that has to be native.
//
//  `library "io"` means these functions live in io.dll (Windows) / libio.so.
//  Axiom reaches them by importing this module:
//
//      function import (return:(io));    // resolves to io.axd + io.ax
//
//  ── what is and is not in this file ──────────────────────────────────────
//
//  Only the functions that must call the operating system. Everything else in
//  `io` -- path splitting, padding, number formatting -- is ordinary Axiom and
//  lives in stdlib/io.ax, where it costs neither a DLL nor a marshalling round
//  trip. Importing the module gives you both; the split is invisible at the call
//  site.
//
//  ── no fallback ──────────────────────────────────────────────────────────
//
//  Unlike `algorithm` and `random`, these functions have no Axiom body to fall
//  back on: opening a file is not something the language can express. A program
//  that uses them needs io.dll, and without it the call fails with
//  `external call failed: cannot load library 'io'`. That is the honest outcome,
//  not a gap to paper over.
//
//  ── reading needs a buffer, writing does not ──────────────────────────────
//
//  A `string` argument crosses as `(char*, length)`, so writing is direct:
//
//      writeTextFile("notes.txt", "line one\n");
//
//  Reading cannot return a `string`: the library has no way to allocate in the
//  VM's arena. It fills a caller buffer instead, and the bytes are packed tightly
//  (one byte after another), which is exactly what `makeString` copies back out:
//
//      char buf[4096];
//      int n = readTextFile("notes.txt", #buf[0], 4096);
//      if (n >= 0) {
//          string text = makeString(#buf[0], n);
//      }
//
//  `capacity` is a **byte** count, and the buffer needs at least that many `char`
//  elements. `char` is 2 bytes wide and only half of each element is used, which
//  is the cost of the language having a single character type; it also means a
//  `char buf[4096]` holds up to 4096 bytes, not 8192.
//
//  Every function reports failure as -1 rather than trapping. A length-returning
//  function returns -1 for both "cannot open" and "buffer too small"; call
//  `fileSize` first when the distinction matters.

module "io" version "0.1.0";

library "io";

// ─── reading and writing ───────────────────────────────────────────────────
//
// For `readTextFile`, `capacity` is both the buffer size and the largest file
// that will fit. A file that does not fit is refused (-1) rather than truncated:
// a silently shortened read looks like a shorter file and surfaces much later.

//  Reads the whole file into `buffer`. Returns the byte count, or -1 when the
//  file cannot be opened or is larger than `capacity` (bytes).
function readTextFile(string path, ptr char buffer, int capacity):int;

//  Writes `content` to `path`, truncating it first. Returns the byte count
//  written, or -1 on failure.
//
//  Content is a `string`, not a buffer: a string already crosses the boundary as
//  UTF-8 bytes, while a `ptr char` buffer is 2 bytes per element and would need
//  the caller to pack bytes by hand. Writing is therefore
//      writeTextFile("out.txt", "hello" + "\n");
//  and reading is the only direction that needs a buffer.
function writeTextFile(string path, string content):int;

//  Appends `content` to `path`, creating it if needed.
function appendTextFile(string path, string content):int;

// ─── metadata ──────────────────────────────────────────────────────────────

//  Size in bytes, or -1 when the path does not exist or is a directory.
function fileSize(string path):int;

//  True when the path names something that can be opened for reading.
function fileExists(string path):bool;

//  True when the path is a directory.
function isDirectory(string path):bool;

// ─── mutation ──────────────────────────────────────────────────────────────

//  Deletes a file. Returns 0 on success, -1 on failure.
function removeFile(string path):int;

//  Moves or renames a file. Returns 0 on success, -1 on failure.
function renameFile(string from, string to):int;

//  Creates one directory. Not recursive: the parent must exist. Succeeds when the
//  directory is already there, so "make sure this exists" is a single call.
function makeDir(string path):int;

// ─── directory listing ─────────────────────────────────────────────────────

//  Writes every entry name in `path` into `buffer` as NUL-terminated byte
//  strings, ending with an extra NUL, and returns the entry count. `.` and `..`
//  are skipped: a caller that wants "the files here" never wants them.
//
//  Returns -1 when the directory cannot be read **or** the buffer is too small --
//  a partial listing would look like a smaller directory. `capacity` is bytes.
//
//  One call returns every name, rather than `entryAt(index)` reopening the
//  directory each time -- the latter turns one syscall into n. To parse the
//  buffer use `dirCount` / `dirEntryAt` from io.ax: pure Axiom over bytes in memory.
//
//      char names[4096];
//      int count = listDir(".", #names[0], 4096);
//      for (int i = 0; i < count; i++) { print(dirEntryAt(#names[0], 4096, i)); }
function listDir(string path, ptr char buffer, int capacity):int;

//  Removes an **empty** directory. Not recursive: a recursive delete is one stray
//  argument away from destroying a tree, and a caller who wants it can list the
//  entries and remove them in an order they chose. Returns 0 on success.
function removeDir(string path):int;

// ─── line input ────────────────────────────────────────────────────────────
//
// `readLine` returns a `string`, and an empty string cannot be told apart from
// end of file -- a gap spec §8.3 records. `readLineInto` closes it by returning a
// number: >= 0 is the line length (0 for an empty line), **-1 is end of file**.
//
//      char line[512];
//      while (readLineInto(#line[0], 512) >= 0) { ... }
//
// A line longer than the buffer is refused (-1) rather than truncated, and a
// trailing CR from a CRLF file is stripped.

//  Reads one line from stdin, without the newline. -1 at end of file.
function readLineInto(ptr char buffer, int capacity):int;

//  Reads everything left on stdin. Returns the byte count, or -1 when the buffer
//  is too small.
function readStdinAll(ptr char buffer, int capacity):int;
