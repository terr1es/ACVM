//  os.axd — the part of the system library that must call the operating system.
//
//  `library "os"` means these functions live in os.dll (Windows) / libos.so.
//  Axiom reaches them by importing the module:
//
//      function import (return:(os));    // resolves to os.axd + os.ax
//
//  ── importing this module is a promise ────────────────────────────────────
//
//  Unlike `algorithm` and `random`, nothing here has an Axiom fallback: asking for
//  the current directory is not an expression the language can evaluate. A program
//  that imports `os` needs os.dll, and without it the first call fails with
//  `cannot load library 'os'`. Keep it out of portable code that only needs `str`
//  or `vec`.
//
//  ── two layers, one import ────────────────────────────────────────────────
//
//  The functions declared here are the raw form: they take a buffer and return a
//  byte count. stdlib/os.ax wraps the text-producing ones so that the common case
//  reads as a single call:
//
//      char buf[256];
//      if (cwdInto(#buf[0], 256) >= 0) { ... }      // raw
//
//      string dir = cwd();                          // wrapped, same module
//
//  The wrapper is what you normally call. The raw form is public because the
//  wrapper has a fixed buffer size (256 bytes), and a caller with a genuinely long
//  path needs to supply a bigger one.
//
//  ── buffer convention ─────────────────────────────────────────────────────
//
//  `capacity` is a **byte** count. Element `i` of the `ptr char` array holds byte
//  `i` in its low half, which is what `makeString` expects, so:
//
//      int n = cwdInto(#buf[0], 256);
//      string text = makeString(#buf[0], n);
//
//  is exact for any UTF-8 text. A buffer that is too small returns -1 rather than
//  truncating: a shortened path is a wrong path, not a shorter one.
//
//  ── no function here traps ────────────────────────────────────────────────
//
//  Failure is -1, or false, or a null out-parameter left untouched. The one
//  exception is `exitProcess`, which by definition does not return.
//
//  ── what is deliberately absent ───────────────────────────────────────────
//
//  Threads, dynamic library loading, signals and ptrace. Each needs a handle type
//  the language cannot express (`ptr void` would carry it, but every function
//  taking one becomes uncheckable) and an ownership rule that `arena` does not
//  provide. Files, directories and processes are enough to write a real tool;
//  those four are enough to write a real operating system, which is not the goal
//  of a standard library.
//
//  Sockets used to be on that list, with the same reasoning. They moved to their
//  own module (`net`) once it became clear the handle type does exist: a socket is
//  an integer, and `int64` covers Windows' 64 bit `SOCKET` as well as a POSIX fd.
//  The ownership problem did not go away -- nothing here closes a socket for you --
//  so `net.axd` documents the discipline and `net.ax` offers calls that cannot
//  leak. Keeping them out of `os` means a program that only wants `cwd` does not
//  carry Winsock.

module "os" version "0.1.0";

library "os";

// ─── environment ──────────────────────────────────────────────────────────

//  Reads variable `name` into `buffer`. Returns the byte count, or -1 when the
//  variable is unset **or** the value does not fit.
//
//  An unset variable and one set to "" both give -1 -- the language has no
//  nullable string, so "absent" and "empty" cannot be told apart here. Use
//  `envHas` when the difference matters, or `env()` which returns "" for both.
function envGetInto(string name, ptr char buffer, int capacity):int;

//  True when `name` is set, even to the empty string. This is the only way to
//  distinguish "no such variable" from "set to nothing".
function envHas(string name):bool;

//  Sets `name` to `value`. Returns 0 on success, -1 on failure (a name containing
//  '=' is the usual reason).
function envSet(string name, string value):int;

//  Removes `name`. Returns 0 when removed, -1 when it was not set -- C's
//  `unsetenv` cannot report that, so the presence test is done here.
function envUnset(string name):int;

//  Number of environment variables, or -1 when the environment cannot be read.
//
//  Takes no buffer: counting walks the environment block instead of copying it.
//  An earlier design had the caller pass a flat block, which silently reported
//  zero variables whenever the block did not fit in the caller's buffer -- and a
//  Windows environment can reach 32 KB, so no fixed size is safe.
function envCount():int;

//  Writes environment entry `index` as `NAME=VALUE` into `buffer`. Returns the
//  byte count, or -1 when out of range or too big for the buffer.
//
//  One entry per call. That re-walks the block each time, which is O(n^2) for a
//  full dump and still far cheaper than the `makeString` the caller does per
//  entry. Use `envEntryAt` in stdlib/os.ax, which wraps this and splits the pair.
function envEntryInto(int index, ptr char buffer, int capacity):int;

// ─── process ──────────────────────────────────────────────────────────────

//  The operating system's id for this process.
function processId():int;

//  Writes the current working directory into `buffer`. Byte count, or -1.
function cwdInto(ptr char buffer, int capacity):int;

//  Changes the working directory. Returns 0 on success, -1 on failure.
function setCurrentDir(string path):int;

//  Writes the full path of the running executable into `buffer`. Byte count, or
//  -1. This is how a tool locates resources that sit next to its own binary,
//  which is the only reliable way when it can be launched from any directory.
function exePathInto(ptr char buffer, int capacity):int;

//  Terminates the process with `code` as the exit status.
//
//  **Nothing after this call runs** -- not the `eof` block, not arena release.
//  Output already passed to `print` is flushed, because `exit` flushes it, but a
//  file the program was midway through writing is left as it is.
//
//  For a command-line tool this replaces threading a "should stop" flag through
//  every function. For a library it is never the right answer: a library that
//  exits kills its host.
function exitProcess(int code):void;

//  Runs `command` through the system shell and returns its exit code, or -1 when
//  the shell could not start.
//
//  Output goes to the program's own output: this is "run this and let the user
//  watch", not "run this and read the result". Quoting follows the platform's
//  shell rules (`cmd.exe /c` on Windows, `/bin/sh -c` elsewhere), not Axiom's.
//
//  On POSIX the raw wait status is decoded, so a command that exited 1 returns 1
//  rather than 256, and one killed by a signal returns -1.
function runCommand(string command):int;

//  Runs `command` and captures its standard output into `buffer`.
//
//  Returns the byte count, or -1 when the command could not run **or** its output
//  did not fit in `capacity`. `exitCode` receives the command's exit status, so
//  "produced nothing" and "failed" stay distinguishable.
//
//  Truncation is an error rather than a partial result: half a JSON document
//  parses as garbage. Retry with a larger buffer.
//
//  Standard error is **not** captured -- a failing command explains itself there,
//  and swallowing it would leave the caller with an empty buffer and no clue.
//
//  `exitCode` is an array of one: the boundary marshals a `ptr` only together with
//  an element count, so it has to be spelled `ptr int, int`. The six slots this
//  costs (command, buffer, its count, exit code, its count) is exactly the
//  boundary's limit -- adding one more parameter here would not compile.
function runCaptureInto(string command, ptr char buffer, int capacity, ptr int exitCode,
                        int exitCodeCount):int;

// ─── system information ───────────────────────────────────────────────────

//  Logical processors available to this process (1 when unknown). Processors the
//  platform has masked away from this process are not counted.
function cpuCount():int;

//  Memory page size in bytes (4096 when unknown).
function pageSize():int;

//  Installed physical memory in bytes, or -1 when unknown. This is the machine's
//  total, not what is free right now.
function physicalMemoryBytes():int64;

//  Platform id: 1 Windows, 2 Linux, 3 macOS, 4 FreeBSD, 0 unknown.
//  These numbers are part of the interface; `platformName` in stdlib/os.ax maps
//  them and new platforms must take new values, never reuse these.
function platformId():int;

//  Architecture id: 1 x86, 2 x86-64, 3 ARM, 4 ARM64, 0 unknown.
function archId():int;

//  Writes the machine's network name into `buffer`. Byte count, or -1.
function hostNameInto(ptr char buffer, int capacity):int;

//  Writes the current user's **login** name into `buffer` (not a display name:
//  that one contains spaces and cannot be used to build a path). Byte count, or -1.
function userNameInto(ptr char buffer, int capacity):int;

// ─── well-known directories ───────────────────────────────────────────────

//  The temporary directory, **without** a trailing separator, so that
//  `pathJoin` in stdlib/io.ax gives the same result on every platform.
function tempDirInto(ptr char buffer, int capacity):int;

//  The current user's home directory, without a trailing separator.
function homeDirInto(ptr char buffer, int capacity):int;

// ─── filesystem operations io does not cover ──────────────────────────────

//  Copies `from` to `to`, replacing `to`. Returns 0 on success, -1 on failure.
//
//  Streams through a 64 KB buffer instead of reading the whole file, because the
//  Axiom version would need two copies in memory and would fail on anything
//  larger than the 16 MB arena.
//
//  Permissions and timestamps are not preserved: this moves bytes, which is what
//  a build step wants.
function copyFile(string from, string to):int;

//  File size as a 64-bit value, or -1 when it does not exist or is a directory.
//  `io.fileSize` returns an `int` and therefore caps at 2 GB; this one does not.
function fileSizeBytes(string path):int64;

//  Free space in bytes on the volume holding `path`, or -1 when unknown. Reports
//  space available to this user, which on Unix excludes reserved blocks -- the
//  smaller number, and the one that answers "will my write fit".
function diskFreeBytes(string path):int64;
