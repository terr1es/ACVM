//  io.c — file access and line input that Axiom cannot express on its own.
//
//  Built into io.dll / libio.so and reached from Axiom through stdlib/io.axd.
//
//  ── why this file exists at all ───────────────────────────────────────────
//
//  Everything in stdlib/io.ax is pure Axiom: path splitting and padding are just
//  `lastIndexOf` and `+`. What cannot be done in Axiom is *reaching the operating
//  system* -- there is no way to open a file, and the language has no facility for
//  adding one without a compiler change. So the boundary here is not a performance
//  decision, it is the only door available.
//
//  That is also why these functions have no Axiom fallback: there is nothing to
//  fall back to. A program that touches files needs io.dll, and the linker error
//  says so plainly.
//
//  ── the 2 byte char problem ───────────────────────────────────────────────
//
//  Axiom's `char` is a UTF-16 code unit, 2 bytes wide, but a text file is a byte
//  stream. Rather than refuse the mismatch or invent a byte type, the buffer
//  contract here is: element `i` of a `ptr char` array holds byte `i` of the file
//  in its *low* byte, with the high byte zero. That is exactly what `makeString`
//  expects -- it truncates each code unit to a byte -- so the round trip is
//
//      readTextFile(path, #buf[0], cap)   →  bytes in buf, one per element
//      makeString(#buf[0], count)         →  the text, byte for byte
//
//  and writing goes the other way. The cost is that a buffer for N bytes needs
//  2N bytes of Axiom storage, which is the price of having one character type.
//
//  ── why every path takes a length ─────────────────────────────────────────
//
//  A `string` parameter is marshalled as **two** native arguments, (char*, length),
//  exactly like an array is (buffer, count) -- one Axiom value, two slots. The VM
//  also appends a NUL so `const char*` works directly, which is why the length
//  parameters are unused here, but the C prototypes still have to declare them or
//  every argument after the first path would be read from the wrong slot.
//
//  Declaring them is not bookkeeping: it documents the calling convention at the
//  place where getting it wrong is silent corruption rather than a compile error.
//
//  ── error reporting ───────────────────────────────────────────────────────
//
//  No function traps: a failure is a return code. -1 means "failed", and the
//  functions that return a length return -1 for both "cannot open" and "buffer too
//  small", because distinguishing them would mean another out parameter for a case
//  the caller handles identically. Check `fileExists` first when it matters.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

//===--- path handling ----------------------------------------------------===//

/// Opens `path` for reading. On Windows the path is UTF-8 and has to be widened:
/// `fopen` reads it in the ANSI code page, so any non-ASCII directory -- which is
/// most of them on a Chinese system -- would fail on a name that is perfectly
/// valid. Elsewhere `fopen` already takes UTF-8.
static FILE* acvm_open_read(const char* path) {
#ifdef _WIN32
    const int needed = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (needed <= 0) {
        return NULL;
    }
    wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)needed);
    if (wide == NULL) {
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, needed);
    wchar_t mode[3] = {L'r', L'b', 0};
    FILE* file = _wfopen(wide, mode);
    free(wide);
    return file;
#else
    return fopen(path, "rb");
#endif
}

static FILE* acvm_open_write(const char* path, int append) {
#ifdef _WIN32
    const int needed = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (needed <= 0) {
        return NULL;
    }
    wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)needed);
    if (wide == NULL) {
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, needed);
    wchar_t mode[4] = {L'a', L'b', 0, 0};
    if (!append) {
        mode[0] = L'w';
    }
    FILE* file = _wfopen(wide, mode);
    free(wide);
    return file;
#else
    return fopen(path, append ? "ab" : "wb");
#endif
}

//===--- reading and writing ----------------------------------------------===//

/// Reads the whole file into `buffer`, one byte per `uint16_t` element.
///
/// `capacity` is a count of *elements*, so it is also the largest file that fits.
/// Returns the number of bytes read, or -1 when the file cannot be opened or is
/// larger than `capacity` (a truncated read would be indistinguishable from a
/// shorter file, so it is refused instead).
int64_t readTextFile(const char* path, int64_t pathLength, uint16_t* buffer, int64_t capacity) {
    (void)pathLength;
    if (path == NULL || buffer == NULL || capacity <= 0) {
        return -1;
    }
    FILE* file = acvm_open_read(path);
    if (file == NULL) {
        return -1;
    }
    // Bytes are written tightly, one after another, so `makeString(buffer, n)`
    // rebuilds the file exactly: that function copies raw bytes, it does not
    // translate code units. `capacity` is therefore a *byte* count, and the caller
    // needs at least that many `char` elements -- half the buffer's 2-byte width
    // goes unused, which is the price of having exactly one character type.
    const size_t room = (size_t)capacity;
    const size_t read = fread(buffer, 1, room, file);
    const int more = fgetc(file) != EOF;
    fclose(file);
    if (more) {
        return -1;   // bigger than the caller's buffer: refuse rather than truncate
    }
    return (int64_t)read;
}

/// Writes `count` elements of `data` to `path`, truncating it first.
/// Each element contributes its low byte. Returns the number of bytes written,
/// or -1 on failure.
int64_t writeTextFile(const char* path, int64_t pathLength, const char* content,
                      int64_t contentLength) {
    (void)pathLength;
    if (path == NULL || content == NULL || contentLength < 0) {
        return -1;
    }
    FILE* file = acvm_open_write(path, /*append=*/0);
    if (file == NULL) {
        return -1;
    }
    int64_t written = 0;
    if (contentLength > 0) {
        written = (int64_t)fwrite(content, 1, (size_t)contentLength, file);
    }
    const int closed = fclose(file);
    if (written != contentLength || closed != 0) {
        return -1;
    }
    return written;
}

int64_t appendTextFile(const char* path, int64_t pathLength, const char* content,
                       int64_t contentLength) {
    (void)pathLength;
    if (path == NULL || content == NULL || contentLength < 0) {
        return -1;
    }
    FILE* file = acvm_open_write(path, /*append=*/1);
    if (file == NULL) {
        return -1;
    }
    int64_t written = 0;
    if (contentLength > 0) {
        written = (int64_t)fwrite(content, 1, (size_t)contentLength, file);
    }
    const int closed = fclose(file);
    if (written != contentLength || closed != 0) {
        return -1;
    }
    return written;
}

//===--- metadata ---------------------------------------------------------===//

/// Size in bytes, or -1 when the file does not exist or is a directory.
int64_t fileSize(const char* path, int64_t pathLength) {
    (void)pathLength;
    if (path == NULL) {
        return -1;
    }
    FILE* file = acvm_open_read(path);
    if (file == NULL) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    const long size = ftell(file);
    fclose(file);
    return size < 0 ? -1 : (int64_t)size;
}

int64_t fileExists(const char* path, int64_t pathLength) {
    (void)pathLength;
    if (path == NULL) {
        return 0;
    }
    FILE* file = acvm_open_read(path);
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

int64_t isDirectory(const char* path, int64_t pathLength) {
    (void)pathLength;
    if (path == NULL) {
        return 0;
    }
#ifdef _WIN32
    const int needed = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (needed <= 0) {
        return 0;
    }
    wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)needed);
    if (wide == NULL) {
        return 0;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, needed);
    const DWORD attributes = GetFileAttributesW(wide);
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat info;
    if (stat(path, &info) != 0) {
        return 0;
    }
    return S_ISDIR(info.st_mode) ? 1 : 0;
#endif
}

//===--- mutation ---------------------------------------------------------===//

int64_t removeFile(const char* path, int64_t pathLength) {
    (void)pathLength;
    if (path == NULL) {
        return -1;
    }
#ifdef _WIN32
    const int needed = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (needed <= 0) {
        return -1;
    }
    wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)needed);
    if (wide == NULL) {
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, needed);
    const int result = _wremove(wide);
    free(wide);
    return result == 0 ? 0 : -1;
#else
    return remove(path) == 0 ? 0 : -1;
#endif
}

int64_t renameFile(const char* from, int64_t fromLength, const char* to, int64_t toLength) {
    (void)fromLength;
    (void)toLength;
    if (from == NULL || to == NULL) {
        return -1;
    }
#ifdef _WIN32
    const int neededFrom = MultiByteToWideChar(CP_UTF8, 0, from, -1, NULL, 0);
    const int neededTo = MultiByteToWideChar(CP_UTF8, 0, to, -1, NULL, 0);
    if (neededFrom <= 0 || neededTo <= 0) {
        return -1;
    }
    wchar_t* wideFrom = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)neededFrom);
    wchar_t* wideTo = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)neededTo);
    if (wideFrom == NULL || wideTo == NULL) {
        free(wideFrom);
        free(wideTo);
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, from, -1, wideFrom, neededFrom);
    MultiByteToWideChar(CP_UTF8, 0, to, -1, wideTo, neededTo);
    const int result = _wrename(wideFrom, wideTo);
    free(wideFrom);
    free(wideTo);
    return result == 0 ? 0 : -1;
#else
    return rename(from, to) == 0 ? 0 : -1;
#endif
}

/// Creates one directory. Not recursive: the parent has to exist, because
/// silently building a whole tree hides a typo in the path.
int64_t makeDir(const char* path, int64_t pathLength) {
    (void)pathLength;
    if (path == NULL) {
        return -1;
    }
#ifdef _WIN32
    const int needed = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (needed <= 0) {
        return -1;
    }
    wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)needed);
    if (wide == NULL) {
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, needed);
    const int result = _wmkdir(wide);
    free(wide);
#else
    const int result = mkdir(path, 0755);
#endif
    if (result == 0) {
        return 0;
    }
    // Already there is not a failure: "make sure this directory exists" is the
    // common intent, and callers that care can check `isDirectory` first.
    return isDirectory(path, pathLength) ? 0 : -1;
}

//===--- line input -------------------------------------------------------===//

/// Reads one line from stdin into `buffer`, without the newline. One byte per
/// element, like the file functions.
///
/// Returns the number of bytes on the line, which may be 0 for an empty line, or
/// **-1 at end of file**. That distinction is the reason this function exists:
/// `readLine` returns a `string`, and an empty string cannot be told apart from
/// EOF (spec §8.3 records the gap). A caller that loops until -1 can stop at the
/// right place, and trailing `\r` from a CRLF file is stripped.
int64_t readLineInto(uint16_t* buffer, int64_t capacity) {
    if (buffer == NULL || capacity <= 0) {
        return -1;
    }
    int64_t length = 0;
    int sawAnything = 0;
    for (;;) {
        const int ch = fgetc(stdin);
        if (ch == EOF) {
            if (!sawAnything) {
                return -1;   // nothing at all: end of file
            }
            break;           // last line without a trailing newline
        }
        sawAnything = 1;
        if (ch == '\n') {
            break;
        }
        // A line longer than the buffer is refused rather than truncated: a
        // silently shortened line is a data loss bug that surfaces much later.
        if (length >= capacity) {
            return -1;
        }
        ((uint8_t*)buffer)[length++] = (uint8_t)ch;
    }
    if (length > 0 && ((uint8_t*)buffer)[length - 1] == (uint8_t)'\r') {
        --length;   // CRLF input behaves like LF input
    }
    return length;
}

/// Reads everything left on stdin into `buffer`, one byte per element.
/// Returns the byte count, or -1 when the buffer is too small.
int64_t readStdinAll(uint16_t* buffer, int64_t capacity) {
    if (buffer == NULL || capacity <= 0) {
        return -1;
    }
    int64_t length = 0;
    for (;;) {
        const int ch = fgetc(stdin);
        if (ch == EOF) {
            break;
        }
        if (length >= capacity) {
            return -1;
        }
        ((uint8_t*)buffer)[length++] = (uint8_t)ch;
    }
    return length;
}

//===--- directory listing -----------------------------------------------===//

/// Writes every entry name in `path` into `buffer` as a run of NUL-terminated
/// byte strings, ending with an extra NUL. Returns the entry count, or -1 when
/// the directory cannot be read or the buffer is too small.
///
/// Why this shape rather than a callback: Axiom has no function pointer type, so
/// "call me for each entry" is not expressible. Why one call rather than
/// `entryAt(index)`: the latter would reopen the directory for every entry,
/// turning one syscall into n^2. The caller walks the buffer instead -- see
/// `dirEntryAt` in io.ax for that walk.
///
/// `.` and `..` are skipped. They are real directory entries, but a caller that
/// wants "the files in this folder" never wants them, and every caller would
/// otherwise have to filter two magic names.
///
/// Byte convention matches the rest of this file: one byte per element, packed
/// tightly, so `makeString` can read any name back out directly.
int64_t listDir(const char* path, int64_t pathLength, uint16_t* buffer, int64_t capacity) {
    (void)pathLength;
    if (path == NULL || buffer == NULL || capacity <= 1) {
        return -1;
    }
    uint8_t* out = (uint8_t*)buffer;
    int64_t used = 0;
    int64_t count = 0;

#ifdef _WIN32
    // FindFirstFileW needs a `dir\*` pattern, and is UTF-16, so the path has to
    // be widened first.
    const int needed = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (needed <= 0) {
        return -1;
    }
    wchar_t* pattern = (wchar_t*)malloc(sizeof(wchar_t) * ((size_t)needed + 2));
    if (pattern == NULL) {
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, pattern, needed);
    size_t length = wcslen(pattern);
    // One separator if the caller did not leave one.
    if (length > 0 && pattern[length - 1] != L'/' && pattern[length - 1] != L'\\') {
        pattern[length++] = L'\\';
    }
    pattern[length++] = L'*';
    pattern[length] = 0;

    WIN32_FIND_DATAW data;
    HANDLE found = FindFirstFileW(pattern, &data);
    free(pattern);
    if (found == INVALID_HANDLE_VALUE) {
        return -1;
    }

    for (;;) {
        const WCHAR* name = data.cFileName;
        const int skip = (wcscmp(name, L".") == 0) || (wcscmp(name, L"..") == 0);
        if (!skip) {
            // UTF-8 length first, so a name that does not fit is caught before
            // any of it is written.
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, -1, NULL, 0, NULL, NULL);
            if (bytes <= 0) {
                FindClose(found);
                return -1;
            }
            const int64_t need = (int64_t)(bytes - 1) + 1;   // name without NUL, plus NUL
            if (used + need + 1 > capacity) {
                FindClose(found);
                return -1;
            }
            WideCharToMultiByte(CP_UTF8, 0, name, -1, (char*)out + used, bytes, NULL, NULL);
            used += need;
            count += 1;
        }
        if (!FindNextFileW(found, &data)) {
            break;
        }
    }
    FindClose(found);
#else
    DIR* directory = opendir(path);
    if (directory == NULL) {
        return -1;
    }
    struct dirent* entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        const size_t bytes = strlen(entry->d_name);
        if (used + (int64_t)bytes + 2 > capacity) {
            closedir(directory);
            return -1;
        }
        memcpy(out + used, entry->d_name, bytes + 1);   // includes the NUL
        used += (int64_t)bytes + 1;
        count += 1;
    }
    closedir(directory);
#endif

    // Extra NUL so a zero-length name marks the end. Without it the caller would
    // have to know the byte count, which this return value does not carry.
    out[used] = 0;
    return count;
}

/// Removes an empty directory. Not recursive on purpose: a recursive delete is
/// one stray argument away from destroying a tree, and the caller who wants it
/// can list the entries and delete them in an order they chose.
/// Returns 0 on success, -1 when the directory is missing or not empty.
int64_t removeDir(const char* path, int64_t pathLength) {
    (void)pathLength;
    if (path == NULL) {
        return -1;
    }
#ifdef _WIN32
    const int needed = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (needed <= 0) {
        return -1;
    }
    wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)needed);
    if (wide == NULL) {
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, needed);
    const int result = _wrmdir(wide);
    free(wide);
    return result == 0 ? 0 : -1;
#else
    return rmdir(path) == 0 ? 0 : -1;
#endif
}
