//  os.c — system-level queries and process control that Axiom cannot express.
//
//  Built into os.dll / libos.so and reached from Axiom through stdlib/os.axd.
//
//  ── what belongs here ─────────────────────────────────────────────────────
//
//  Only things that require asking the operating system. Everything that can be
//  computed from their results lives in stdlib/os.ax: turning a platform id into
//  a name, splitting an environment entry into name and value, joining a temp
//  directory with a file name. The split is the same one `io` uses, and it is
//  invisible at the call site because both halves are one import.
//
//  ── the buffer convention ─────────────────────────────────────────────────
//
//  A library function cannot return a `string`: allocating one requires the
//  arena, which belongs to the VM and not to this file. So every function that
//  produces text takes a caller buffer, exactly like `io.readTextFile`:
//
//      char buf[256];
//      int n = cwdInto(#buf[0], 256);       // n = byte count, or -1
//      if (n >= 0) { print(makeString(#buf[0], n)); }
//
//  `capacity` is a **byte** count, and element `i` of the `ptr char` array holds
//  byte `i` in its low half. That is what `makeString` expects — it truncates
//  each 2-byte code unit to one byte — so the round trip is exact.
//
//  **Too small is an error, not a truncation.** Each of these returns -1 when the
//  text does not fit. A truncated path or hostname is not a shorter answer, it is
//  a wrong one, and the caller would have no way to notice. The only exception is
//  `runCaptureInto`, which explains itself at its definition.
//
//  ── error reporting ───────────────────────────────────────────────────────
//
//  Nothing here traps. Failures are -1 (or false). Same reasoning as io.c: a
//  library cannot decide that the caller's program should die, and a program that
//  wants to die has `exitProcess`.
//
//  ── UTF-8 on Windows ──────────────────────────────────────────────────────
//
//  Axiom strings are UTF-8 bytes. The Windows API is UTF-16. Every path and every
//  environment name therefore crosses two conversions, and doing them by hand at
//  sixteen call sites is how a library ends up mishandling a non-ASCII user name.
//  The two helpers below are the only places that convert.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
// GlobalMemoryStatusEx needs at least Windows 2000; 0x0600 also covers the
// GetDiskFreeSpaceExW / GetUserNameW variants used below. Must come before
// <windows.h>, and MinGW honours it only if nothing included it yet.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#include <wchar.h>
#else
#include <errno.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

// ─── platform and architecture ids ─────────────────────────────────────────
//
// Returned as small integers rather than strings so that a switch in Axiom is a
// switch on an int, and so that adding a platform does not change any signature.
// stdlib/os.ax maps them to names; the numbers are part of the interface and must
// not be renumbered.

#define OS_PLATFORM_UNKNOWN 0
#define OS_PLATFORM_WINDOWS 1
#define OS_PLATFORM_LINUX 2
#define OS_PLATFORM_MACOS 3
#define OS_PLATFORM_FREEBSD 4

#if defined(_WIN32)
#define OS_THIS_PLATFORM OS_PLATFORM_WINDOWS
#elif defined(__APPLE__)
#define OS_THIS_PLATFORM OS_PLATFORM_MACOS
#elif defined(__FreeBSD__)
#define OS_THIS_PLATFORM OS_PLATFORM_FREEBSD
#elif defined(__linux__)
#define OS_THIS_PLATFORM OS_PLATFORM_LINUX
#else
#define OS_THIS_PLATFORM OS_PLATFORM_UNKNOWN
#endif

#define OS_ARCH_UNKNOWN 0
#define OS_ARCH_X86 1
#define OS_ARCH_X86_64 2
#define OS_ARCH_ARM 3
#define OS_ARCH_ARM64 4

#if defined(__aarch64__) || defined(_M_ARM64)
#define OS_THIS_ARCH OS_ARCH_ARM64
#elif defined(__x86_64__) || defined(_M_X64)
#define OS_THIS_ARCH OS_ARCH_X86_64
#elif defined(__i386__) || defined(_M_IX86)
#define OS_THIS_ARCH OS_ARCH_X86
#elif defined(__arm__) || defined(_M_ARM)
#define OS_THIS_ARCH OS_ARCH_ARM
#else
#define OS_THIS_ARCH OS_ARCH_UNKNOWN
#endif

// ─── text conversion helpers ───────────────────────────────────────────────

/// A write cursor over the caller's `ptr char` buffer. `used` counts bytes.
///
/// The bytes are packed **tightly in memory**, one per address -- not one per
/// 2-byte element. That is the same layout `io.c` produces with
/// `fread(buffer, 1, room, file)`, and it is what `makeString` expects: it copies
/// raw bytes straight out of the buffer, it does not decode code units. Writing
/// one byte per element instead would interleave a NUL after each character, which
/// is exactly what the first version of this file did -- see the note below.
///
/// The consequence for `capacity`: it counts bytes, and the caller's array needs
/// only half as many elements for the same storage. Passing `capacity` as the
/// element count is still correct, because the array declared with that many
/// elements has twice the bytes the function will touch.
typedef struct {
    uint16_t* data;
    int64_t capacity;
    int64_t used;
} OsBuffer;

/// Appends raw bytes, packed one per address. Returns 0 when they do not all fit,
/// leaving the buffer untouched.
static int os_buffer_append(OsBuffer* buffer, const char* bytes, int64_t length) {
    if (buffer == NULL || (length > 0 && bytes == NULL)) {
        return 0;
    }
    if (buffer->used + length > buffer->capacity) {
        return 0;
    }
    memcpy((unsigned char*)buffer->data + buffer->used, bytes, (size_t)length);
    buffer->used += length;
    return 1;
}

#ifdef _WIN32

/// UTF-16 to freshly allocated UTF-8. Caller frees. NULL on failure.
static char* os_utf8_from_wide(const wchar_t* wide) {
    if (wide == NULL) {
        return NULL;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (bytes <= 0) {
        return NULL;
    }
    char* out = (char*)malloc((size_t)bytes);
    if (out == NULL) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, bytes, NULL, NULL) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

/// UTF-8 to freshly allocated UTF-16. Caller frees. NULL on failure.
static wchar_t* os_wide_from_utf8(const char* text) {
    if (text == NULL) {
        return NULL;
    }
    const int units = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (units <= 0) {
        return NULL;
    }
    wchar_t* out = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)units);
    if (out == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, out, units) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

/// Writes `wide` into the caller's buffer as UTF-8. Returns the byte count, or -1
/// when it does not fit. `wide == NULL` is a failure too, which is what lets the
/// callers below read as "ask Windows, convert, write, report".
static int64_t os_write_wide(const wchar_t* wide, uint16_t* buffer, int64_t capacity) {
    if (wide == NULL || buffer == NULL) {
        return -1;
    }
    char* utf8 = os_utf8_from_wide(wide);
    if (utf8 == NULL) {
        return -1;
    }
    OsBuffer out = {buffer, capacity, 0};
    const int ok = os_buffer_append(&out, utf8, (int64_t)strlen(utf8));
    free(utf8);
    return ok ? out.used : -1;
}

#else

/// Writes `text` (already UTF-8 on POSIX) into the caller's buffer.
/// Returns the byte count, or -1 when it does not fit or is NULL.
static int64_t os_write_text(const char* text, uint16_t* buffer, int64_t capacity) {
    if (text == NULL || buffer == NULL) {
        return -1;
    }
    OsBuffer out = {buffer, capacity, 0};
    return os_buffer_append(&out, text, (int64_t)strlen(text)) ? out.used : -1;
}

#endif

// ─── environment ───────────────────────────────────────────────────────────

/// Reads variable `name` into `buffer`. Returns the byte count, or -1 when the
/// variable is unset, empty of capacity, or the value does not fit.
///
/// An empty value and an unset variable both give -1 (or 0 bytes). Use `envHas`
/// when the difference matters -- the language has no nullable string, so this is
/// the same compromise `readLine` makes at end of input.
int64_t envGetInto(const char* name, int64_t nameLength, uint16_t* buffer, int64_t capacity) {
    (void)nameLength;
    if (name == NULL || buffer == NULL || capacity <= 0) {
        return -1;
    }
#ifdef _WIN32
    wchar_t* wideName = os_wide_from_utf8(name);
    if (wideName == NULL) {
        return -1;
    }
    // Ask for the size first: the value can be 32 KB, so a fixed stack buffer
    // would be either wrong or wasteful.
    const DWORD needed = GetEnvironmentVariableW(wideName, NULL, 0);
    if (needed == 0) {
        free(wideName);
        return -1;              // unset, or the empty string
    }
    wchar_t* value = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)needed);
    if (value == NULL) {
        free(wideName);
        return -1;
    }
    const DWORD written = GetEnvironmentVariableW(wideName, value, needed);
    free(wideName);
    if (written == 0 || written >= needed) {
        free(value);
        return -1;
    }
    const int64_t result = os_write_wide(value, buffer, capacity);
    free(value);
    return result;
#else
    const char* value = getenv(name);
    if (value == NULL) {
        return -1;
    }
    return os_write_text(value, buffer, capacity);
#endif
}

/// True when `name` is set, even if its value is empty.
///
/// GetEnvironmentVariableW returns 0 both for "unset" and for "set to the empty
/// string", so on Windows presence has to be tested through the environment block
/// itself. GetLastError is no help: the API does not set it for this case.
int64_t envHas(const char* name, int64_t nameLength) {
    (void)nameLength;
    if (name == NULL) {
        return 0;
    }
#ifdef _WIN32
    wchar_t* wideName = os_wide_from_utf8(name);
    if (wideName == NULL) {
        return 0;
    }
    const wchar_t* block = GetEnvironmentStringsW();
    if (block == NULL) {
        free(wideName);
        return 0;
    }
    const size_t nameLengthWide = wcslen(wideName);
    int64_t found = 0;
    for (const wchar_t* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) {
        if (wcsncmp(entry, wideName, nameLengthWide) == 0 && entry[nameLengthWide] == L'=') {
            found = 1;
            break;
        }
    }
    FreeEnvironmentStringsW((LPWCH)block);
    free(wideName);
    return found;
#else
    return getenv(name) != NULL ? 1 : 0;
#endif
}

/// Sets `name` to `value`. Returns 0 on success, -1 on failure.
///
/// The change reaches child processes started afterwards, and `envCount` /
/// `envEntryInto` see it immediately: on Windows `SetEnvironmentVariableW` updates
/// the same block `GetEnvironmentStringsW` returns, and on POSIX `setenv` updates
/// `environ`. Nothing else in the system is affected -- a parent shell does not
/// learn about it.
int64_t envSet(const char* name, int64_t nameLength, const char* value, int64_t valueLength) {
    (void)nameLength;
    (void)valueLength;
    if (name == NULL || value == NULL) {
        return -1;
    }
#ifdef _WIN32
    wchar_t* wideName = os_wide_from_utf8(name);
    wchar_t* wideValue = os_wide_from_utf8(value);
    if (wideName == NULL || wideValue == NULL) {
        free(wideName);
        free(wideValue);
        return -1;
    }
    const BOOL ok = SetEnvironmentVariableW(wideName, wideValue);
    free(wideName);
    free(wideValue);
    return ok ? 0 : -1;
#else
    return setenv(name, value, 1) == 0 ? 0 : -1;
#endif
}

/// Removes `name`. Returns 0 on success, -1 when it was not set or cannot be
/// removed. Unsetting a variable that is not there is reported as failure so a
/// caller can tell "removed" from "there was nothing to remove" -- C's
/// `unsetenv` refuses to say.
int64_t envUnset(const char* name, int64_t nameLength) {
    (void)nameLength;
    if (name == NULL) {
        return -1;
    }
    // Presence is tested first on purpose: `SetEnvironmentVariableW(name, NULL)`
    // returns TRUE for a variable that was never there, so on Windows the API alone
    // cannot tell "removed" from "nothing to remove" -- and the caller can.
    if (!envHas(name, nameLength)) {
        return -1;
    }
#ifdef _WIN32
    wchar_t* wideName = os_wide_from_utf8(name);
    if (wideName == NULL) {
        return -1;
    }
    const BOOL ok = SetEnvironmentVariableW(wideName, NULL);
    free(wideName);
    return ok ? 0 : -1;
#else
    return unsetenv(name) == 0 ? 0 : -1;
#endif
}

/// Number of environment variables. Exact, with no buffer involved -- an earlier
/// version of this file had the caller pass a flat block and counted the entries
/// in it, which silently returned 0 whenever the block did not fit. A Windows
/// environment can reach 32 KB, so no fixed buffer size is safe; counting walks,
/// it does not copy.
int64_t envCount(void) {
#ifdef _WIN32
    const wchar_t* block = GetEnvironmentStringsW();
    if (block == NULL) {
        return -1;
    }
    int64_t count = 0;
    for (const wchar_t* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) {
        // Names starting with '=' are per-drive current directories ("=C:=C://work"),
        // an artefact of the API rather than a variable the user set.
        if (entry[0] != L'=') {
            count += 1;
        }
    }
    FreeEnvironmentStringsW((LPWCH)block);
    return count;
#else
    extern char** environ;
    if (environ == NULL) {
        return -1;
    }
    int64_t count = 0;
    for (char** entry = environ; *entry != NULL; ++entry) {
        count += 1;
    }
    return count;
#endif
}

/// Writes entry `index` as `NAME=VALUE` into `buffer`. Returns the byte count, or
/// -1 when the index is out of range, the entry does not fit, or the environment
/// cannot be read.
///
/// One entry per call, which re-walks the block each time. That is O(n) per entry
/// and O(n^2) for a full dump -- and irrelevant: a hundred entries of a few dozen
/// bytes each is a rounding error next to the `makeString` the caller does with
/// the result. The alternative (a flat block in a caller buffer) needs the caller
/// to guess a size, and guessing wrong is the bug this replaced.
int64_t envEntryInto(int64_t index, uint16_t* buffer, int64_t capacity) {
    if (buffer == NULL || capacity <= 0 || index < 0) {
        return -1;
    }
#ifdef _WIN32
    const wchar_t* block = GetEnvironmentStringsW();
    if (block == NULL) {
        return -1;
    }
    int64_t seen = 0;
    int64_t result = -1;
    for (const wchar_t* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) {
        if (entry[0] == L'=') {
            continue;
        }
        if (seen == index) {
            char* utf8 = os_utf8_from_wide(entry);
            if (utf8 != NULL) {
                OsBuffer out = {buffer, capacity, 0};
                result = os_buffer_append(&out, utf8, (int64_t)strlen(utf8)) ? out.used : -1;
                free(utf8);
            }
            break;
        }
        seen += 1;
    }
    FreeEnvironmentStringsW((LPWCH)block);
    return result;
#else
    extern char** environ;
    if (environ == NULL) {
        return -1;
    }
    int64_t seen = 0;
    for (char** entry = environ; *entry != NULL; ++entry) {
        if (seen == index) {
            OsBuffer out = {buffer, capacity, 0};
            return os_buffer_append(&out, *entry, (int64_t)strlen(*entry)) ? out.used : -1;
        }
        seen += 1;
    }
    return -1;
#endif
}

// ─── process ───────────────────────────────────────────────────────────────

/// The operating system's id for this process.
int64_t processId(void) {
#ifdef _WIN32
    return (int64_t)GetCurrentProcessId();
#else
    return (int64_t)getpid();
#endif
}

/// Writes the current working directory into `buffer`. Returns the byte count or
/// -1 when it does not fit.
int64_t cwdInto(uint16_t* buffer, int64_t capacity) {
    if (buffer == NULL || capacity <= 0) {
        return -1;
    }
#ifdef _WIN32
    // GROW-BY-DOUBLING: the path can exceed MAX_PATH, and the required size is
    // only knowable by trying. Starting at 512 covers the overwhelming majority
    // in one call.
    DWORD size = 512;
    for (int attempt = 0; attempt < 8; ++attempt) {
        wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)size);
        if (wide == NULL) {
            return -1;
        }
        const DWORD written = GetCurrentDirectoryW(size, wide);
        if (written == 0) {
            free(wide);
            return -1;
        }
        if (written < size) {
            const int64_t result = os_write_wide(wide, buffer, capacity);
            free(wide);
            return result;
        }
        free(wide);
        size = written + 1;
    }
    return -1;
#else
    char text[4096];
    if (getcwd(text, sizeof(text)) == NULL) {
        return -1;
    }
    return os_write_text(text, buffer, capacity);
#endif
}

/// Changes the working directory. Returns 0 on success, -1 on failure.
int64_t setCurrentDir(const char* path, int64_t pathLength) {
    (void)pathLength;
    if (path == NULL) {
        return -1;
    }
#ifdef _WIN32
    wchar_t* wide = os_wide_from_utf8(path);
    if (wide == NULL) {
        return -1;
    }
    const BOOL ok = SetCurrentDirectoryW(wide);
    free(wide);
    return ok ? 0 : -1;
#else
    return chdir(path) == 0 ? 0 : -1;
#endif
}

/// Writes the full path of the running executable into `buffer`.
///
/// This is what lets a tool find files that sit next to its own binary, which is
/// the only reliable way to locate resources for a program that may be launched
/// from any directory.
int64_t exePathInto(uint16_t* buffer, int64_t capacity) {
    if (buffer == NULL || capacity <= 0) {
        return -1;
    }
#ifdef _WIN32
    DWORD size = 512;
    for (int attempt = 0; attempt < 8; ++attempt) {
        wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)size);
        if (wide == NULL) {
            return -1;
        }
        const DWORD written = GetModuleFileNameW(NULL, wide, size);
        if (written == 0) {
            free(wide);
            return -1;
        }
        // Equal to `size` means truncated, not "exactly fits".
        if (written < size) {
            const int64_t result = os_write_wide(wide, buffer, capacity);
            free(wide);
            return result;
        }
        free(wide);
        size = size * 2;
    }
    return -1;
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);          // first call reports the size
    char* text = (char*)malloc(size > 0 ? size : 1);
    if (text == NULL) {
        return -1;
    }
    if (_NSGetExecutablePath(text, &size) != 0) {
        free(text);
        return -1;
    }
    const int64_t result = os_write_text(text, buffer, capacity);
    free(text);
    return result;
#else
    char text[4096];
    const ssize_t length = readlink("/proc/self/exe", text, sizeof(text) - 1);
    if (length < 0) {
        return -1;
    }
    text[length] = '\0';
    return os_write_text(text, buffer, capacity);
#endif
}

/// Terminates the process with `code` as the exit status.
///
/// **Nothing after this call runs**: no `eof` block, no arena release, no output
/// flush the runtime would have done at the end of `main`. Buffered output is
/// flushed by `exit` itself, so `print` output does appear -- but a temporary
/// file the program was midway through writing is left as it is.
///
/// This exists because the alternative for a command-line tool is to thread a
/// "should stop" flag through every function, which is worse in every way. For a
/// library, return an error code instead: a library that exits kills its host.
void exitProcess(int64_t code) {
    exit((int)code);
}

/// Runs `command` through the system shell and returns its exit code, or -1 when
/// the shell could not be started.
///
/// The command's output goes wherever the program's output goes: this is for
/// "run this tool and let the user watch", not for reading results. Use
/// `runCaptureInto` for that.
///
/// Quoting is the platform's shell rules, not Axiom's: `system` passes the string
/// to `cmd.exe /c` on Windows and `/bin/sh -c` elsewhere. A path with spaces needs
/// whatever quoting that shell expects.
int64_t runCommand(const char* command, int64_t commandLength) {
    (void)commandLength;
    if (command == NULL) {
        return -1;
    }
    const int status = system(command);
    if (status == -1) {
        return -1;              // the shell itself could not be started
    }
#ifdef _WIN32
    // cmd.exe's exit code is already the value the caller wants.
    return (int64_t)status;
#else
    // POSIX encodes the child's status: exit code in bits 8-15. Returning the raw
    // wait status would make a failure look like 256 to a caller testing != 0,
    // and would hide the difference between "exited 1" and "killed by signal".
    if (WIFEXITED(status)) {
        return (int64_t)WEXITSTATUS(status);
    }
    return -1;                  // killed by a signal: no exit code to report
#endif
}

/// Runs `command` and captures its standard output into `buffer`.
///
/// Returns the byte count, or -1 when the command could not be run **or** its
/// output did not fit in `capacity`. `exitCode` (when not null) receives the
/// command's exit status, so a caller can distinguish "no output" from "failed".
///
/// Unlike the other buffer functions, this one cannot know the output size in
/// advance, so it reads until the pipe closes and only then discovers the total.
/// Truncating would hand back half a JSON document that parses as garbage, so it
/// returns -1 instead; the caller retries with a bigger buffer. Reading one extra
/// byte is how "exactly fits" is told from "one byte too many".
///
/// Standard error is **not** captured. A failing command usually explains itself
/// on stderr, and swallowing that would leave the caller with an empty buffer and
/// no clue; letting it through puts the explanation in front of the user.
int64_t runCaptureInto(const char* command, int64_t commandLength, uint16_t* buffer,
                       int64_t capacity, int32_t* exitCode, int64_t exitCodeCount) {
    (void)commandLength;
    if (exitCode != NULL && exitCodeCount >= 1) {
        *exitCode = -1;
    }
    if (command == NULL || buffer == NULL || capacity <= 0) {
        return -1;
    }

#ifdef _WIN32
    FILE* pipe = _popen(command, "rb");
#else
    FILE* pipe = popen(command, "r");
#endif
    if (pipe == NULL) {
        return -1;
    }

    int64_t used = 0;
    int overflowed = 0;
    char chunk[4096];
    for (;;) {
        const size_t got = fread(chunk, 1, sizeof(chunk), pipe);
        if (got > 0) {
            if (used + (int64_t)got > capacity) {
                overflowed = 1;     // keep reading so the child is not left blocked
            } else {
                OsBuffer out = {buffer, capacity, used};
                os_buffer_append(&out, chunk, (int64_t)got);
                used = out.used;
            }
        }
        if (got < sizeof(chunk)) {
            break;                  // short read: EOF or error
        }
    }

#ifdef _WIN32
    const int status = _pclose(pipe);
#else
    const int status = pclose(pipe);
#endif

    if (overflowed) {
        return -1;
    }
    if (exitCode != NULL && exitCodeCount >= 1) {
#ifdef _WIN32
        *exitCode = (int32_t)status;
#else
        *exitCode = WIFEXITED(status) ? (int32_t)WEXITSTATUS(status) : -1;
#endif
    }
    return used;
}

// ─── system information ────────────────────────────────────────────────────

/// Number of logical processors available to this process. 1 when unknown.
///
/// "Available to this process" rather than "on the machine": a process limited by
/// a CPU affinity mask should size its thread pool to what it may use.
int64_t cpuCount(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors > 0 ? (int64_t)info.dwNumberOfProcessors : 1;
#else
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int64_t)count : 1;
#endif
}

/// Memory page size in bytes. 4096 when unknown, which is right on every platform
/// Axiom currently targets.
int64_t pageSize(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize > 0 ? (int64_t)info.dwPageSize : 4096;
#else
    const long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? (int64_t)size : 4096;
#endif
}

/// Installed physical memory in bytes, or -1 when unknown.
///
/// This is the machine's total, not the amount available: on Windows
/// `ullTotalPhys` excludes memory the firmware reserved. For "how much may I use"
/// there is no portable answer, which is why this returns the total and says so.
int64_t physicalMemoryBytes(void) {
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return -1;
    }
    return (int64_t)status.ullTotalPhys;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long size = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || size <= 0) {
        return -1;
    }
    return (int64_t)pages * (int64_t)size;
#endif
}

int64_t platformId(void) {
    return (int64_t)OS_THIS_PLATFORM;
}

int64_t archId(void) {
    return (int64_t)OS_THIS_ARCH;
}

int64_t hostNameInto(uint16_t* buffer, int64_t capacity) {
    if (buffer == NULL || capacity <= 0) {
        return -1;
    }
#ifdef _WIN32
    wchar_t wide[256];
    DWORD size = (DWORD)(sizeof(wide) / sizeof(wide[0]));
    if (!GetComputerNameW(wide, &size)) {
        return -1;
    }
    wide[size] = L'\0';         // size is set to the length on success
    return os_write_wide(wide, buffer, capacity);
#else
    char text[256];
    if (gethostname(text, sizeof(text)) != 0) {
        return -1;
    }
    text[sizeof(text) - 1] = '\0';   // may be unterminated on truncation
    return os_write_text(text, buffer, capacity);
#endif
}

/// The name the current user logs in with, not their display name.
///
/// Deliberately the login name: it is what file paths and configuration
/// directories are built from, and it is the same string on every platform. A
/// display name ("Ada Lovelace") contains spaces and cannot be used that way.
int64_t userNameInto(uint16_t* buffer, int64_t capacity) {
    if (buffer == NULL || capacity <= 0) {
        return -1;
    }
#ifdef _WIN32
    wchar_t wide[256];
    DWORD size = (DWORD)(sizeof(wide) / sizeof(wide[0]));
    if (!GetUserNameW(wide, &size)) {
        return -1;
    }
    return os_write_wide(wide, buffer, capacity);
#else
    // getpwuid is not thread-safe; the reentrant form needs a caller buffer whose
    // required size is only knowable by trying, so it retries upward.
    long hint = sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t size = hint > 0 ? (size_t)hint : 1024;
    for (int attempt = 0; attempt < 4; ++attempt) {
        struct passwd entry;
        struct passwd* result = NULL;
        char* scratch = (char*)malloc(size);
        if (scratch == NULL) {
            return -1;
        }
        const int rc = getpwuid_r(getuid(), &entry, scratch, size, &result);
        if (rc == ERANGE) {
            free(scratch);
            size = size * 2;
            continue;               // buffer too small: retry
        }
        if (rc != 0 || result == NULL) {
            free(scratch);
            return -1;
        }
        const int64_t written = os_write_text(entry.pw_name, buffer, capacity);
        free(scratch);
        return written;
    }
    return -1;
#endif
}

// ─── well-known directories ────────────────────────────────────────────────

/// The directory for temporary files, **without** a trailing separator.
///
/// The platforms disagree here: Windows' `GetTempPathW` ends with a separator (and
/// falls back to the current directory when TMP/TEMP are unset), while POSIX gives
/// `/tmp` plainly. Normalising to "no trailing separator" means `pathJoin` in
/// stdlib/io.ax produces the same result everywhere instead of a doubled slash.
int64_t tempDirInto(uint16_t* buffer, int64_t capacity) {
    if (buffer == NULL || capacity <= 0) {
        return -1;
    }
#ifdef _WIN32
    wchar_t wide[MAX_PATH + 2];
    const DWORD length = GetTempPathW(MAX_PATH + 2, wide);
    if (length == 0 || length > MAX_PATH) {
        return -1;
    }
    // Strip trailing '\' or '/': the caller joins with pathJoin.
    DWORD end = length;
    while (end > 0 && (wide[end - 1] == L'\\' || wide[end - 1] == L'/')) {
        end -= 1;
    }
    if (end == 0) {
        return -1;              // the root directory: no useful temp path
    }
    wide[end] = L'\0';
    return os_write_wide(wide, buffer, capacity);
#else
    const char* dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0') {
        dir = "/tmp";
    }
    size_t length = strlen(dir);
    while (length > 1 && dir[length - 1] == '/') {
        length -= 1;            // keep a lone "/" as it is
    }
    OsBuffer out = {buffer, capacity, 0};
    return os_buffer_append(&out, dir, (int64_t)length) ? out.used : -1;
#endif
}

/// The current user's home directory, without a trailing separator.
int64_t homeDirInto(uint16_t* buffer, int64_t capacity) {
    if (buffer == NULL || capacity <= 0) {
        return -1;
    }
#ifdef _WIN32
    // USERPROFILE is the documented answer on every supported Windows. The
    // HOMEDRIVE+HOMEPATH pair is kept as a fallback because USERPROFILE is
    // inherited from the parent, so a service account can start without it.
    const wchar_t* keys[2] = {L"USERPROFILE", NULL};
    for (int i = 0; keys[i] != NULL; ++i) {
        const DWORD needed = GetEnvironmentVariableW(keys[i], NULL, 0);
        if (needed > 0) {
            wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)needed);
            if (wide == NULL) {
                return -1;
            }
            if (GetEnvironmentVariableW(keys[i], wide, needed) > 0) {
                const int64_t written = os_write_wide(wide, buffer, capacity);
                free(wide);
                return written;
            }
            free(wide);
        }
    }
    // Last resort: HOMEDRIVE + HOMEPATH, the pre-NT pair that a cmd.exe started
    // from a service account still provides. Building the path by hand instead of
    // calling SHGetFolderPathW keeps this library free of shell32 -- a dependency
    // that would have to be declared in CMake and that fails to load on a
    // stripped-down Windows.
    const DWORD driveNeeded = GetEnvironmentVariableW(L"HOMEDRIVE", NULL, 0);
    const DWORD pathNeeded = GetEnvironmentVariableW(L"HOMEPATH", NULL, 0);
    if (driveNeeded == 0 || pathNeeded == 0) {
        return -1;
    }
    wchar_t* drive = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)driveNeeded);
    wchar_t* tail = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)pathNeeded);
    if (drive == NULL || tail == NULL) {
        free(drive);
        free(tail);
        return -1;
    }
    int64_t result = -1;
    if (GetEnvironmentVariableW(L"HOMEDRIVE", drive, driveNeeded) > 0 &&
        GetEnvironmentVariableW(L"HOMEPATH", tail, pathNeeded) > 0) {
        // "C:" + "\Users\name" -- the join needs no separator decision, because
        // HOMEPATH always starts with one. They are concatenated through a buffer
        // rather than a formatted string so the length is checked once.
        const size_t driveLength = wcslen(drive);
        const size_t tailLength = wcslen(tail);
        wchar_t* joined = (wchar_t*)malloc(sizeof(wchar_t) * (driveLength + tailLength + 1));
        if (joined != NULL) {
            memcpy(joined, drive, sizeof(wchar_t) * driveLength);
            memcpy(joined + driveLength, tail, sizeof(wchar_t) * (tailLength + 1));
            result = os_write_wide(joined, buffer, capacity);
            free(joined);
        }
    }
    free(drive);
    free(tail);
    return result;
#else
    const char* home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        // HOME is not set in some service contexts; the password database still
        // knows. Falling back to "/" would be worse than failing: it would make a
        // program write into the root directory.
        struct passwd entry;
        struct passwd* result = NULL;
        char scratch[1024];
        if (getpwuid_r(getuid(), &entry, scratch, sizeof(scratch), &result) != 0 ||
            result == NULL) {
            return -1;
        }
        home = entry.pw_dir;
        if (home == NULL || home[0] == '\0') {
            return -1;
        }
    }
    size_t length = strlen(home);
    while (length > 1 && home[length - 1] == '/') {
        length -= 1;
    }
    OsBuffer out = {buffer, capacity, 0};
    return os_buffer_append(&out, home, (int64_t)length) ? out.used : -1;
#endif
}

// ─── filesystem operations that io does not cover ──────────────────────────

/// Copies `from` to `to`, replacing `to`. Returns 0 on success, -1 on failure.
///
/// Why not read the file and write it from Axiom: that needs the whole file in
/// memory at once, twice (the read buffer and the arena string), and a 200 MB
/// file would exceed the 16 MB arena. Streaming through a fixed buffer here is
/// the only way to copy a file that is larger than the buffer.
///
/// Permissions and timestamps are not preserved: this is "get the bytes there",
/// which is what a build step or a backup wants. A caller who needs attributes
/// kept needs a platform-specific tool.
int64_t copyFile(const char* from, int64_t fromLength, const char* to, int64_t toLength) {
    (void)fromLength;
    (void)toLength;
    if (from == NULL || to == NULL) {
        return -1;
    }

#ifdef _WIN32
    wchar_t* wideFrom = os_wide_from_utf8(from);
    wchar_t* wideTo = os_wide_from_utf8(to);
    if (wideFrom == NULL || wideTo == NULL) {
        free(wideFrom);
        free(wideTo);
        return -1;
    }
    FILE* in = _wfopen(wideFrom, L"rb");
    if (in == NULL) {
        free(wideFrom);
        free(wideTo);
        return -1;
    }
    FILE* out = _wfopen(wideTo, L"wb");
    free(wideFrom);
    free(wideTo);
#else
    FILE* in = fopen(from, "rb");
    if (in == NULL) {
        return -1;
    }
    FILE* out = fopen(to, "wb");
#endif
    if (out == NULL) {
        fclose(in);
        return -1;
    }

    // 64 KB: large enough that the per-call overhead disappears, small enough to
    // stay off the heap's large-allocation path.
    static const size_t kChunk = 64 * 1024;
    char* buffer = (char*)malloc(kChunk);
    if (buffer == NULL) {
        fclose(in);
        fclose(out);
        return -1;
    }

    int64_t result = 0;
    for (;;) {
        const size_t got = fread(buffer, 1, kChunk, in);
        if (got > 0 && fwrite(buffer, 1, got, out) != got) {
            result = -1;        // disk full, or the target went away
            break;
        }
        if (got < kChunk) {
            if (ferror(in)) {
                result = -1;
            }
            break;
        }
    }

    free(buffer);
    fclose(in);
    // A close failure on the write side (buffered data that never landed) is a
    // real failure even when every fwrite reported success.
    if (fclose(out) != 0) {
        result = -1;
    }
    return result;
}

/// File size in bytes as a 64-bit value, or -1 when it does not exist.
///
/// `io.fileSize` returns an `int`, which caps it at 2 GB. This one exists for
/// large files; for anything smaller the two agree.
int64_t fileSizeBytes(const char* path, int64_t pathLength) {
    (void)pathLength;
    if (path == NULL) {
        return -1;
    }
#ifdef _WIN32
    wchar_t* wide = os_wide_from_utf8(path);
    if (wide == NULL) {
        return -1;
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
        free(wide);
        return -1;
    }
    free(wide);
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return -1;              // a directory has no file size
    }
    // Two 32-bit halves: the API has never had a 64-bit size field.
    return (int64_t)(((uint64_t)data.nFileSizeHigh << 32) | (uint64_t)data.nFileSizeLow);
#else
    struct stat info;
    if (stat(path, &info) != 0) {
        return -1;
    }
    if (S_ISDIR(info.st_mode)) {
        return -1;
    }
    return (int64_t)info.st_size;
#endif
}

/// Free space in bytes on the filesystem holding `path`, or -1 when unknown.
///
/// `path` may be any file or directory on the volume; what matters is which
/// volume it is on. Reports free space **available to this user**, not total free
/// space: on a Unix with reserved blocks the two differ, and the smaller one is
/// the answer to "will my write fit".
int64_t diskFreeBytes(const char* path, int64_t pathLength) {
    (void)pathLength;
    if (path == NULL) {
        return -1;
    }
#ifdef _WIN32
    wchar_t* wide = os_wide_from_utf8(path);
    if (wide == NULL) {
        return -1;
    }
    ULARGE_INTEGER available;
    if (!GetDiskFreeSpaceExW(wide, &available, NULL, NULL)) {
        free(wide);
        return -1;
    }
    free(wide);
    return (int64_t)available.QuadPart;
#else
    struct statvfs info;
    if (statvfs(path, &info) != 0) {
        return -1;
    }
    return (int64_t)info.f_bavail * (int64_t)info.f_frsize;
#endif
}
