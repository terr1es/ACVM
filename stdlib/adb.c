//  adb.c — the Axiom Debugger.
//
//  A debugger has to see every instruction before it runs, which bytecode cannot
//  ask for: a call emitted into the program would change the program. So the VM
//  owns a hook slot (include/acvm/VM/DebugApi.h), this library fills it, and the
//  `acvm run --adb` flag is what closes the circuit. Nothing here knows the VM's
//  internals; everything it reads goes through the flat callbacks in that header.
//
//  ── what it does ──────────────────────────────────────────────────────────
//
//    breakpoints     by function name, by `function:ip`, or by source line
//                    (`file.ax:42`, `:42` for the whole program)
//    stepping        step into, step over, step out, run to the next line
//    inspection      disassembly around the current instruction, backtrace,
//                    the current instruction's source position
//    capture         a ring buffer of the last N instructions, dumped on demand
//                    or when the program traps. This is the "what was it doing"
//                    answer that a breakpoint cannot give after the fact.
//    scripted runs   every command can come from a file, so a debug session can
//                    be a test instead of a transcript.
//
//  ── why the command loop lives here and not in the driver ─────────────────
//
//  The hook runs *inside* the interpreter loop. A breakpoint therefore has to
//  block the VM from within the callback -- that is what "stopped" means for a
//  single-threaded interpreter. Putting the loop anywhere else would mean the VM
//  had to know what a prompt is.
//
//  ── cost when attached but idle ──────────────────────────────────────────
//
//  One function call and a few comparisons per instruction. The capture ring is
//  only written when capture is on, which is why it is off by default: a trace of
//  every instruction is exactly what you want *after* a crash and too expensive
//  to keep on all the time.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===--- the VM's ABI -----------------------------------------------------===//
//
//  Declared here rather than included so this file has no C++ dependency: the
//  library is built by the project's C compiler and must not pull in the VM's
//  headers, which are C++ only. The numbers are checked at attach time by
//  comparing `apiVersion`, so a mismatch is a clear error rather than a crash.

#define ADB_API_VERSION 1

typedef struct AdbStepInfo {
    int32_t functionIndex;
    int32_t ip;
    int32_t opcode;
    int32_t operandA;
    int32_t operandB;
    int32_t frameDepth;
    int64_t instructionsRetired;
} AdbStepInfo;

typedef struct AdbInfo {
    int32_t apiVersion;
    void* host;
    int32_t (*functionCount)(void* host);
    const char* (*functionName)(void* host, int32_t functionIndex);
    int32_t (*functionHasCode)(void* host, int32_t functionIndex);
    int32_t (*instructionCount)(void* host, int32_t functionIndex);
    const char* (*opcodeName)(void* host, int32_t opcode);
    const char* (*instructionText)(void* host, int32_t functionIndex, int32_t ip);
    const char* (*sourceFile)(void* host, int32_t functionIndex, int32_t ip);
    int32_t (*sourceLine)(void* host, int32_t functionIndex, int32_t ip);
    const char* (*entryFunctionName)(void* host);
} AdbInfo;

typedef struct AdbHooks {
    int32_t apiVersion;
    void* user;
    void (*onStart)(void* user, const AdbInfo* info);
    int32_t (*beforeInstruction)(void* user, const AdbStepInfo* info);
    void (*onTrap)(void* user, const char* message);
    void (*onFinish)(void* user);
} AdbHooks;

//===--- limits ----------------------------------------------------------===//

#define ADB_MAX_BREAKPOINTS 64
#define ADB_MAX_FRAMES 64
#define ADB_CAPTURE_CAPACITY 4096
#define ADB_LINE_LENGTH 512

typedef enum { BP_FUNCTION = 0, BP_ADDRESS = 1, BP_LINE = 2 } BreakpointKind;

typedef struct {
    BreakpointKind kind;
    char name[128];      /* function name, or the file for BP_LINE */
    int32_t ip;          /* -1 for "function entry" */
    int32_t line;        /* BP_LINE only */
    int32_t hits;
    int enabled;
    int resolvedFunction; /* function index once matched, -1 until then */
} Breakpoint;

typedef struct {
    int32_t functionIndex;
    int32_t ip;
    int32_t opcode;
    int32_t frameDepth;
} CapturedStep;

typedef enum {
    MODE_RUN = 0,    /* stop only at breakpoints */
    MODE_STEP,       /* stop at the next instruction */
    MODE_OVER,       /* stop at the next instruction in this frame or shallower */
    MODE_OUT,        /* stop when the current frame returns */
    MODE_LINE,       /* stop at the next instruction on a different line */
} StopMode;

typedef struct {
    const AdbInfo* info;
    void* host;

    Breakpoint breaks[ADB_MAX_BREAKPOINTS];
    int breakCount;
    /* Specs given before the module was known: `--break` on the command line
     * arrives before `onStart`, so they are parked here and resolved then. */
    char pending[ADB_MAX_BREAKPOINTS][128];
    int pendingCount;

    StopMode mode;
    int32_t modeDepth;   /* frame depth captured when the mode was set */
    int32_t modeLine;    /* line captured for MODE_LINE */
    int32_t modeFile;    /* file captured for MODE_LINE (0 = unknown) */

    /* Capture ring. Written only while `capturing`, dumped with `capture dump`. */
    CapturedStep* capture;
    int captureCapacity;
    int captureCount;
    int captureNext;
    int capturing;

    /* Last stop, kept so `bt` and `list` work after the fact. */
    int32_t stoppedFunction;
    int32_t stoppedIp;
    int32_t stoppedDepth;
    int32_t stopCount;

    /* Line of the instruction executed just before the current one. A line
     * breakpoint fires on entering the line, not on every instruction that
     * happens to carry the same line number -- the compiler emits several per
     * statement. */
    int32_t previousLine;
    int32_t previousFunction;

    int quitting;
    int finished;
    int trapped;
    /* Stop before the entry point's first instruction. Without this the prompt
     * could never open: breakpoints typed at the prompt need a stop to be typed
     * at, and a script's first `break` would arrive after the program had already
     * run. Every debugger solves this the same way -- stop once, then listen. */
    int stopAtEntry;
    int entryReached;
    char trapMessage[ADB_LINE_LENGTH];

    /* Frames of the current backtrace: (functionIndex, ip). The VM does not expose
     * the stack of frames, so this is maintained from the hook's depth: every time
     * depth grows the caller is remembered, every time it shrinks it is popped.
     * That is enough for `bt` and costs nothing when it is never asked for. */
    int32_t frameFunction[ADB_MAX_FRAMES];
    int32_t frameIp[ADB_MAX_FRAMES];
    int frameCount;

    FILE* in;
    FILE* out;
    /* True while reading from a script: a command that would wait for input
     * (like `step`) must not, so scripted runs only stop at breakpoints. */
    int scripting;
    int quiet;   /* suppress the banner and the per-stop chatter */
} Adb;

static Adb g_adb;

/*===--- small helpers --------------------------------------------------===*/

static FILE* adbOut(void) { return g_adb.out != NULL ? g_adb.out : stdout; }

/// Prints `text` with control bytes escaped. Instruction text can contain a
/// literal newline (`CONST_STR "\n"` holds the two characters, but a string
/// constant can also hold a real one), and a raw newline inside a table row turns
/// the capture dump into something unreadable.
static void fputsEscaped(FILE* out, const char* text) {
    if (text == NULL) {
        return;
    }
    for (const char* p = text; *p != '\0'; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c == '\n') {
            fputs("\\n", out);
        } else if (c == '\r') {
            fputs("\\r", out);
        } else if (c == '\t') {
            fputs("\\t", out);
        } else if (c < 32) {
            fprintf(out, "\\x%02x", c);
        } else {
            fputc(c, out);
        }
    }
}

static const char* functionName(int32_t index) {
    if (g_adb.info == NULL || index < 0) {
        return "?";
    }
    return g_adb.info->functionName(g_adb.host, index);
}

/* Base name of a path, so `C:/x/y/main.ax` matches a breakpoint on `main.ax`. */
static const char* pathTail(const char* path) {
    const char* tail = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            tail = p + 1;
        }
    }
    return tail;
}

static int sameFileName(const char* a, const char* b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (strcmp(a, b) == 0) {
        return 1;
    }
    return strcmp(pathTail(a), pathTail(b)) == 0;
}

static int32_t findFunctionByName(const char* name) {
    if (g_adb.info == NULL) {
        return -1;
    }
    const int32_t count = g_adb.info->functionCount(g_adb.host);
    for (int32_t i = 0; i < count; ++i) {
        if (strcmp(g_adb.info->functionName(g_adb.host, i), name) == 0) {
            return i;
        }
    }
    return -1;
}

/*===--- capture --------------------------------------------------------===*/

static void captureRecord(const AdbStepInfo* step) {
    if (!g_adb.capturing || g_adb.capture == NULL) {
        return;
    }
    CapturedStep* slot = &g_adb.capture[g_adb.captureNext];
    slot->functionIndex = step->functionIndex;
    slot->ip = step->ip;
    slot->opcode = step->opcode;
    slot->frameDepth = step->frameDepth;
    g_adb.captureNext = (g_adb.captureNext + 1) % g_adb.captureCapacity;
    if (g_adb.captureCount < g_adb.captureCapacity) {
        g_adb.captureCount += 1;
    }
}

static void captureDump(void) {
    FILE* out = adbOut();
    if (g_adb.captureCount == 0) {
        fprintf(out, "(capture is empty%s)\n", g_adb.capturing ? "" : "; it is off");
        return;
    }
    /* The ring starts at `next` once it wrapped, so the dump is in time order. */
    const int start = (g_adb.captureCount == g_adb.captureCapacity) ? g_adb.captureNext : 0;
    fprintf(out, "== capture: %d instruction(s) ==\n", g_adb.captureCount);
    for (int i = 0; i < g_adb.captureCount; ++i) {
        const CapturedStep* step = &g_adb.capture[(start + i) % g_adb.captureCapacity];
        const char* file = g_adb.info->sourceFile(g_adb.host, step->functionIndex, step->ip);
        const int32_t line = g_adb.info->sourceLine(g_adb.host, step->functionIndex, step->ip);
        fprintf(out, "  %-20s %4d  ", functionName(step->functionIndex), step->ip);
        if (file != NULL && file[0] != '\0' && line > 0) {
            fprintf(out, "%-16s ", pathTail(file));
            fprintf(out, ":%-6d", line);
        } else {
            fprintf(out, "%-23s", "-");
        }
        fputsEscaped(out, g_adb.info->instructionText(g_adb.host, step->functionIndex, step->ip));
        fputc('\n', out);
    }
}

/*===--- frames ----------------------------------------------------------===*/

/* Keeps the synthetic backtrace in step with the VM's frame depth.
 *
 * A debugger normally reads the frame stack from the VM. This ABI deliberately
 * does not expose it -- the stack layout is internal -- so the depth is tracked
 * here instead. The information is thinner (no locals, no arguments) but it is
 * enough to answer "who called this", and it cannot go stale because it is
 * derived from the same number the VM uses. */
static void trackFrame(const AdbStepInfo* step) {
    if (step->frameDepth <= 0) {
        return;
    }
    if (step->frameDepth > g_adb.frameCount) {
        while (g_adb.frameCount < step->frameDepth && g_adb.frameCount < ADB_MAX_FRAMES) {
            g_adb.frameFunction[g_adb.frameCount] = step->functionIndex;
            g_adb.frameIp[g_adb.frameCount] = step->ip;
            g_adb.frameCount += 1;
        }
    }
    if (step->frameDepth - 1 < g_adb.frameCount) {
        /* Refresh the innermost entry: it is where execution actually is. */
        const int at = step->frameDepth - 1;
        g_adb.frameFunction[at] = step->functionIndex;
        g_adb.frameIp[at] = step->ip;
    }
    if (g_adb.frameCount > step->frameDepth) {
        g_adb.frameCount = step->frameDepth;
    }
}

static void printBacktrace(void) {
    FILE* out = adbOut();
    fprintf(out, "== backtrace ==\n");
    if (g_adb.frameCount == 0) {
        fprintf(out, "  (no frames)\n");
        return;
    }
    for (int i = g_adb.frameCount - 1; i >= 0; --i) {
        const int32_t functionIndex = g_adb.frameFunction[i];
        const int32_t ip = g_adb.frameIp[i];
        const char* file = g_adb.info->sourceFile(g_adb.host, functionIndex, ip);
        const int32_t line = g_adb.info->sourceLine(g_adb.host, functionIndex, ip);
        fprintf(out, "  #%d %s(%d)  at %s", g_adb.frameCount - 1 - i,
                functionName(functionIndex), ip,
                (file != NULL && file[0] != '\0') ? pathTail(file) : "?");
        if (line > 0) {
            fprintf(out, ":%d", line);
        }
        fprintf(out, "\n");
    }
}

/*===--- listing --------------------------------------------------------===*/

static void printListing(int32_t functionIndex, int32_t around, int before, int after) {
    FILE* out = adbOut();
    if (functionIndex < 0 || !g_adb.info->functionHasCode(g_adb.host, functionIndex)) {
        fprintf(out, "(no code at that position)\n");
        return;
    }
    const int32_t count = g_adb.info->instructionCount(g_adb.host, functionIndex);
    int32_t from = around - before;
    if (from < 0) {
        from = 0;
    }
    int32_t to = around + after;
    if (to > count - 1) {
        to = count - 1;
    }

    const char* file = g_adb.info->sourceFile(g_adb.host, functionIndex, around);
    const int32_t line = g_adb.info->sourceLine(g_adb.host, functionIndex, around);
    fprintf(out, "== %s", functionName(functionIndex));
    if (file != NULL && file[0] != '\0' && line > 0) {
        fprintf(out, " (%s:%d)", pathTail(file), line);
    }
    fprintf(out, " ==\n");

    for (int32_t ip = from; ip <= to; ++ip) {
        const int isCurrent = ip == around;
        const char* at = g_adb.info->sourceFile(g_adb.host, functionIndex, ip);
        const int32_t atLine = g_adb.info->sourceLine(g_adb.host, functionIndex, ip);
        fprintf(out, "%c %4d  ", isCurrent ? '>' : ' ', ip);
        if (at != NULL && at[0] != '\0' && atLine > 0) {
            char where[160];
            snprintf(where, sizeof(where), "%s:%d", pathTail(at), atLine);
            fprintf(out, "%-18s ", where);
        } else {
            fprintf(out, "%-18s ", "-");
        }
        fputsEscaped(out, g_adb.info->instructionText(g_adb.host, functionIndex, ip));
        fputc('\n', out);
    }
}

static void printPosition(int32_t functionIndex, int32_t ip) {
    FILE* out = adbOut();
    const char* file = g_adb.info->sourceFile(g_adb.host, functionIndex, ip);
    const int32_t line = g_adb.info->sourceLine(g_adb.host, functionIndex, ip);
    if (file != NULL && file[0] != '\0' && line > 0) {
        fprintf(out, "%s:%d  %s", pathTail(file), line, functionName(functionIndex));
    } else {
        fprintf(out, "%s+%d", functionName(functionIndex), ip);
    }
    fprintf(out, "\n");
}

/*===--- breakpoints ----------------------------------------------------===*/

/* Adds a breakpoint. `spec` is one of:
 *
 *     name            a function name (all modules share one flat scope)
 *     name:12         function `name`, instruction 12
 *     file.ax:42      source line 42 in that file
 *     :42             source line 42 anywhere
 *
 * Returns 0 on success, -1 with a message in `why`. */
static int addBreakpoint(const char* spec, char* why, size_t whySize) {
    if (g_adb.info == NULL) {
        if (strlen(spec) >= sizeof(g_adb.pending[0])) {
            snprintf(why, whySize, "specification is too long");
            return -1;
        }
        if (g_adb.pendingCount >= ADB_MAX_BREAKPOINTS) {
            snprintf(why, whySize, "too many breakpoints (limit %d)", ADB_MAX_BREAKPOINTS);
            return -1;
        }
        snprintf(g_adb.pending[g_adb.pendingCount], sizeof(g_adb.pending[0]), "%s", spec);
        g_adb.pendingCount += 1;
        return 0;
    }
    if (g_adb.breakCount >= ADB_MAX_BREAKPOINTS) {
        snprintf(why, whySize, "too many breakpoints (limit %d)", ADB_MAX_BREAKPOINTS);
        return -1;
    }
    Breakpoint* bp = &g_adb.breaks[g_adb.breakCount];
    memset(bp, 0, sizeof(*bp));
    bp->enabled = 1;
    bp->resolvedFunction = -1;
    bp->ip = -1;

    const char* colon = strrchr(spec, ':');
    if (colon != NULL && colon[1] != '\0') {
        /* Everything after the last ':' must be a number. A function name that
         * contains a colon is not a thing in this language, so no ambiguity. */
        char* end = NULL;
        const long value = strtol(colon + 1, &end, 10);
        if (end != NULL && *end == '\0' && value > 0) {
            if (colon == spec) {
                /* `:42` -- a line anywhere. */
                bp->kind = BP_LINE;
                bp->line = (int32_t)value;
                bp->name[0] = '\0';
            } else if (strchr(spec, '.') != NULL || strchr(spec, '/') != NULL ||
                       strchr(spec, '\\') != NULL) {
                /* `file.ax:42` -- a line in a named file. */
                const size_t length = (size_t)(colon - spec);
                if (length >= sizeof(bp->name)) {
                    snprintf(why, whySize, "file name is too long");
                    return -1;
                }
                memcpy(bp->name, spec, length);
                bp->name[length] = '\0';
                bp->kind = BP_LINE;
                bp->line = (int32_t)value;
            } else {
                /* `func:12` -- an exact instruction. */
                const size_t length = (size_t)(colon - spec);
                if (length >= sizeof(bp->name)) {
                    snprintf(why, whySize, "function name is too long");
                    return -1;
                }
                memcpy(bp->name, spec, length);
                bp->name[length] = '\0';
                bp->kind = BP_ADDRESS;
                bp->ip = (int32_t)value;
                bp->resolvedFunction = findFunctionByName(bp->name);
                if (bp->resolvedFunction < 0) {
                    snprintf(why, whySize, "no function named '%s'", bp->name);
                    return -1;
                }
                const int32_t count =
                    g_adb.info->instructionCount(g_adb.host, bp->resolvedFunction);
                if (value >= count) {
                    snprintf(why, whySize, "'%s' has %d instructions", bp->name, count);
                    return -1;
                }
            }
            g_adb.breakCount += 1;
            return 0;
        }
    }

    /* A bare name: a function. */
    if (strlen(spec) >= sizeof(bp->name)) {
        snprintf(why, whySize, "function name is too long");
        return -1;
    }
    strcpy(bp->name, spec);
    bp->kind = BP_FUNCTION;
    bp->resolvedFunction = findFunctionByName(bp->name);
    if (bp->resolvedFunction < 0) {
        snprintf(why, whySize, "no function named '%s'", spec);
        return -1;
    }
    g_adb.breakCount += 1;
    return 0;
}

static int listBreakpoints(void) {
    FILE* out = adbOut();
    if (g_adb.breakCount == 0) {
        fprintf(out, "(no breakpoints)\n");
        return 0;
    }
    fprintf(out, "== breakpoints ==\n");
    for (int i = 0; i < g_adb.breakCount; ++i) {
        const Breakpoint* bp = &g_adb.breaks[i];
        fprintf(out, "  %d  %s  ", i, bp->enabled ? "on " : "off");
        if (bp->kind == BP_FUNCTION) {
            fprintf(out, "%s", bp->name);
        } else if (bp->kind == BP_ADDRESS) {
            fprintf(out, "%s:%d", bp->name, bp->ip);
        } else if (bp->name[0] != '\0') {
            fprintf(out, "%s:%d", bp->name, bp->line);
        } else {
            fprintf(out, ":%d", bp->line);
        }
        fprintf(out, "   hits=%d\n", bp->hits);
    }
    return 0;
}

static int deleteBreakpoint(const char* argument) {
    if (argument == NULL || argument[0] == '\0') {
        /* `delete` with no argument removes all of them, like gdb. */
        g_adb.breakCount = 0;
        fprintf(adbOut(), "all breakpoints deleted\n");
        return 0;
    }
    const int index = atoi(argument);
    if (index < 0 || index >= g_adb.breakCount) {
        fprintf(adbOut(), "no breakpoint %d\n", index);
        return -1;
    }
    for (int i = index; i + 1 < g_adb.breakCount; ++i) {
        g_adb.breaks[i] = g_adb.breaks[i + 1];
    }
    g_adb.breakCount -= 1;
    fprintf(adbOut(), "breakpoint %d deleted\n", index);
    return 0;
}

/* Which breakpoint, if any, fires at this step. Returns the index or -1. */
static int matchBreakpoint(const AdbStepInfo* step) {
    for (int i = 0; i < g_adb.breakCount; ++i) {
        Breakpoint* bp = &g_adb.breaks[i];
        if (!bp->enabled) {
            continue;
        }
        if (bp->kind == BP_FUNCTION) {
            if (bp->resolvedFunction < 0) {
                /* Names are only known after the module loads, so resolution is
                 * retried here instead of failing at attach time. */
                bp->resolvedFunction = findFunctionByName(bp->name);
                if (bp->resolvedFunction < 0) {
                    continue;
                }
            }
            if (bp->resolvedFunction == step->functionIndex && step->ip == 0) {
                bp->hits += 1;
                return i;
            }
        } else if (bp->kind == BP_ADDRESS) {
            if (bp->resolvedFunction == step->functionIndex && bp->ip == step->ip) {
                bp->hits += 1;
                return i;
            }
        } else {
            const char* file = g_adb.info->sourceFile(g_adb.host, step->functionIndex, step->ip);
            const int32_t line = g_adb.info->sourceLine(g_adb.host, step->functionIndex, step->ip);
            if (line == 0 || line != bp->line) {
                continue;
            }
            /* Already executing this line? Then the breakpoint has done its job;
             * firing on each of the four instructions a statement compiles to
             * would make `b file:2` behave like a step command. */
            if (line == g_adb.previousLine &&
                step->functionIndex == g_adb.previousFunction) {
                continue;
            }
            if (bp->name[0] != '\0' && !sameFileName(file, bp->name)) {
                continue;
            }
            bp->hits += 1;
            return i;
        }
    }
    return -1;
}

/*===--- stopping -------------------------------------------------------===*/

static int shouldStop(const AdbStepInfo* step, int* viaBreakpoint) {
    *viaBreakpoint = -1;

    if (g_adb.mode == MODE_STEP) {
        return 1;
    }
    if (g_adb.mode == MODE_OVER) {
        if (step->frameDepth <= g_adb.modeDepth) {
            return 1;
        }
    }
    if (g_adb.mode == MODE_OUT) {
        if (step->frameDepth < g_adb.modeDepth) {
            return 1;
        }
    }
    if (g_adb.mode == MODE_LINE) {
        const char* file = g_adb.info->sourceFile(g_adb.host, step->functionIndex, step->ip);
        const int32_t line = g_adb.info->sourceLine(g_adb.host, step->functionIndex, step->ip);
        if (line > 0) {
            if (line != g_adb.modeLine || step->frameDepth < g_adb.modeDepth) {
                return 1;
            }
            (void)file;
        }
    }

    const int hit = matchBreakpoint(step);
    if (hit >= 0) {
        *viaBreakpoint = hit;
        return 1;
    }
    return 0;
}

static int readCommand(char* buffer, size_t size);

/* Runs the stop: prints where we are, then reads commands until one of them
 * resumes execution. Called from inside the interpreter loop. */
static void stopAndPrompt(const AdbStepInfo* step, int viaBreakpoint) {
    FILE* out = adbOut();
    g_adb.stoppedFunction = step->functionIndex;
    g_adb.stoppedIp = step->ip;
    g_adb.stoppedDepth = step->frameDepth;
    g_adb.stopCount += 1;

    if (!g_adb.quiet) {
        /* The blank line separates a stop from program output on a terminal. In a
         * script it would only put a gap at the top of the transcript, so it is
         * printed only when a human is watching. */
        if (!g_adb.scripting) {
            fprintf(out, "\n");
        }
        if (viaBreakpoint >= 0) {
            fprintf(out, "[%d] breakpoint %d", g_adb.stopCount, viaBreakpoint);
        } else {
            fprintf(out, "[%d] stop", g_adb.stopCount);
        }
        fprintf(out, " at ");
        printPosition(step->functionIndex, step->ip);
        /* The disassembly line is what a reader wants next to the position. */
        fprintf(out, "  ");
        fputsEscaped(out, g_adb.info->instructionText(g_adb.host, step->functionIndex,
                                                      step->ip));
        fputc('\n', out);
    }

    g_adb.mode = MODE_RUN;

    while (1) {
        char line[ADB_LINE_LENGTH];
        if (readCommand(line, sizeof(line)) == 0) {
            /* EOF on the command stream: treat it as `continue` when scripting,
             * and as `quit` on a terminal, so a closed stdin cannot hang a run. */
            if (g_adb.scripting) {
                return;
            }
            g_adb.quitting = 1;
            return;
        }

        char* verb = line;
        char* argument = line;
        while (*argument != '\0' && *argument != ' ') {
            argument += 1;
        }
        if (*argument == ' ') {
            *argument = '\0';
            argument += 1;
            while (*argument == ' ') {
                argument += 1;
            }
        }

        if (strcmp(verb, "c") == 0 || strcmp(verb, "cont") == 0 ||
            strcmp(verb, "continue") == 0) {
            return;
        }
        if (strcmp(verb, "s") == 0 || strcmp(verb, "step") == 0 ||
            strcmp(verb, "si") == 0) {
            /* In a script there is nobody to answer the next prompt, so stepping
             * would stop the run dead. Refuse instead of pretending. */
            if (g_adb.scripting) {
                fprintf(out, "step has no meaning in a script; use 'continue'\n");
                continue;
            }
            g_adb.mode = MODE_STEP;
            return;
        }
        if (strcmp(verb, "n") == 0 || strcmp(verb, "next") == 0) {
            if (g_adb.scripting) {
                fprintf(out, "next has no meaning in a script; use 'continue'\n");
                continue;
            }
            g_adb.mode = MODE_OVER;
            g_adb.modeDepth = step->frameDepth;
            return;
        }
        if (strcmp(verb, "finish") == 0 || strcmp(verb, "o") == 0 ||
            strcmp(verb, "out") == 0) {
            if (g_adb.scripting) {
                fprintf(out, "finish has no meaning in a script; use 'continue'\n");
                continue;
            }
            g_adb.mode = MODE_OUT;
            g_adb.modeDepth = step->frameDepth;
            return;
        }
        if (strcmp(verb, "until") == 0 || strcmp(verb, "u") == 0) {
            const int32_t currentLine =
                g_adb.info->sourceLine(g_adb.host, step->functionIndex, step->ip);
            if (currentLine == 0) {
                fprintf(out, "no source position here; use 'next' or 'step'\n");
                continue;
            }
            if (g_adb.scripting) {
                fprintf(out, "until has no meaning in a script\n");
                continue;
            }
            g_adb.mode = MODE_LINE;
            g_adb.modeLine = currentLine;
            g_adb.modeDepth = step->frameDepth;
            return;
        }
        if (strcmp(verb, "b") == 0 || strcmp(verb, "break") == 0) {
            if (argument[0] == '\0') {
                listBreakpoints();
                continue;
            }
            char why[128];
            if (addBreakpoint(argument, why, sizeof(why)) != 0) {
                fprintf(out, "%s\n", why);
            } else {
                fprintf(out, "breakpoint %d set at %s\n", g_adb.breakCount - 1, argument);
            }
            continue;
        }
        if (strcmp(verb, "d") == 0 || strcmp(verb, "delete") == 0) {
            deleteBreakpoint(argument);
            continue;
        }
        if (strcmp(verb, "i") == 0 || strcmp(verb, "info") == 0) {
            if (strcmp(argument, "breakpoints") == 0 || strcmp(argument, "b") == 0) {
                listBreakpoints();
            } else {
                fprintf(out, "info breakpoints | info capture | info registers\n");
            }
            continue;
        }
        if (strcmp(verb, "l") == 0 || strcmp(verb, "list") == 0) {
            const int32_t span = argument[0] != '\0' ? atoi(argument) : 5;
            printListing(step->functionIndex, step->ip, span, span);
            continue;
        }
        if (strcmp(verb, "x") == 0) {
            /* `x/N` lists N instructions from here; `x` alone shows the current. */
            const int32_t span = argument[0] != '\0' ? atoi(argument) : 0;
            printListing(step->functionIndex, step->ip, 0, span);
            continue;
        }
        if (strcmp(verb, "bt") == 0 || strcmp(verb, "where") == 0) {
            printBacktrace();
            continue;
        }
        if (strcmp(verb, "capture") == 0) {
            if (strcmp(argument, "on") == 0 || strcmp(argument, "start") == 0) {
                g_adb.capturing = 1;
                fprintf(out, "capture on (%d instructions)\n", g_adb.captureCapacity);
            } else if (strcmp(argument, "off") == 0 || strcmp(argument, "stop") == 0) {
                g_adb.capturing = 0;
                fprintf(out, "capture off after %d instruction(s)\n", g_adb.captureCount);
            } else if (strcmp(argument, "dump") == 0) {
                captureDump();
            } else if (strcmp(argument, "clear") == 0) {
                g_adb.captureCount = 0;
                g_adb.captureNext = 0;
                fprintf(out, "capture cleared\n");
            } else {
                fprintf(out, "capture on|off|dump|clear  (currently %s, %d step(s))\n",
                        g_adb.capturing ? "on" : "off", g_adb.captureCount);
            }
            continue;
        }
        if (strcmp(verb, "disas") == 0 || strcmp(verb, "disasm") == 0) {
            const int32_t functionIndex =
                argument[0] != '\0' ? findFunctionByName(argument) : step->functionIndex;
            if (functionIndex < 0) {
                fprintf(out, "no function named '%s'\n", argument);
                continue;
            }
            printListing(functionIndex, argument[0] != '\0' ? 0 : step->ip, 0,
                         g_adb.info->instructionCount(g_adb.host, functionIndex));
            continue;
        }
        if (strcmp(verb, "functions") == 0 || strcmp(verb, "fns") == 0) {
            const int32_t count = g_adb.info->functionCount(g_adb.host);
            const char* entry = g_adb.info->entryFunctionName(g_adb.host);
            for (int32_t i = 0; i < count; ++i) {
                const char* name = g_adb.info->functionName(g_adb.host, i);
                const int isEntry = entry != NULL && strcmp(name, entry) == 0;
                fprintf(out, "  %4d  %-32s %4d instruction(s)%s\n", i, name,
                        g_adb.info->instructionCount(g_adb.host, i),
                        isEntry ? "  <- entry" : "");
            }
            continue;
        }
        if (strcmp(verb, "q") == 0 || strcmp(verb, "quit") == 0) {
            g_adb.quitting = 1;
            g_adb.mode = MODE_RUN;
            return;
        }
        if (strcmp(verb, "h") == 0 || strcmp(verb, "help") == 0 || verb[0] == '?') {
            FILE* out2 = adbOut();
            fprintf(out2,
                    "commands:\n"
                    "  c, continue        resume until the next breakpoint\n"
                    "  s, step            run one instruction\n"
                    "  n, next            run to the next instruction in this frame\n"
                    "  finish             run until this frame returns\n"
                    "  until              run to the next source line\n"
                    "  b <spec>           break at a function, `func:ip`, `file:line`, `:line`\n"
                    "  d [n]              delete breakpoint n, or all of them\n"
                    "  info breakpoints   list breakpoints\n"
                    "  l [n]              list instructions around here\n"
                    "  x [n]              list n instructions from here\n"
                    "  disas [function]   disassemble a whole function\n"
                    "  functions          list functions with their instruction counts\n"
                    "  bt                 backtrace\n"
                    "  capture on|off|dump|clear\n"
                    "  q, quit            stop the program\n");
            continue;
        }
        fprintf(out, "unknown command '%s' (try 'help')\n", verb);
    }
}

/*===--- command input --------------------------------------------------===*/

static int readCommand(char* buffer, size_t size) {
    FILE* in = g_adb.in != NULL ? g_adb.in : stdin;

    while (1) {
        if (!g_adb.quiet && !g_adb.scripting) {
            fprintf(adbOut(), "(adb) ");
            fflush(adbOut());
        }
        buffer[0] = '\0';
        if (fgets(buffer, (int)size, in) == NULL) {
            return 0;
        }
        size_t length = strlen(buffer);
        while (length > 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r')) {
            buffer[length - 1] = '\0';
            length -= 1;
        }
        /* `#` starts a comment, so a script can explain itself. */
        for (size_t i = 0; i < length; ++i) {
            if (buffer[i] == '#') {
                buffer[i] = '\0';
                length = i;
                break;
            }
        }
        char* from = buffer;
        while (*from == ' ' || *from == '\t') {
            from += 1;
        }
        if (from != buffer) {
            memmove(buffer, from, strlen(from) + 1);
        }
        length = strlen(buffer);
        while (length > 0 && (buffer[length - 1] == ' ' || buffer[length - 1] == '\t')) {
            buffer[length - 1] = '\0';
            length -= 1;
        }
        /* Blank lines and comments are skipped rather than reported as end of
         * input: a script's first line is usually a comment, and treating that as
         * EOF made the debugger continue -- silently running the whole program. */
        if (buffer[0] != '\0') {
            return 1;
        }
    }
}


/*===--- hooks ----------------------------------------------------------===*/

static void onStart(void* user, const AdbInfo* info) {
    (void)user;
    if (info == NULL || info->apiVersion != ADB_API_VERSION) {
        return;
    }
    g_adb.info = info;
    g_adb.host = info->host;
    /* Resolve whatever was specified before the program existed. A failure here
     * is reported once, at the moment it becomes checkable, rather than being
     * silently dropped. */
    for (int i = 0; i < g_adb.pendingCount; ++i) {
        char why[128];
        if (addBreakpoint(g_adb.pending[i], why, sizeof(why)) != 0) {
            fprintf(adbOut(), "adb: %s: %s\n", g_adb.pending[i], why);
        }
    }
    g_adb.pendingCount = 0;
}

static int32_t onBeforeInstruction(void* user, const AdbStepInfo* info) {
    (void)user;
    if (g_adb.quitting) {
        return 1;   /* Abort */
    }
    captureRecord(info);
    trackFrame(info);

    /* A one-time stop at the entry point, before any user breakpoint can fire.
     * `ip == 0` and a matching name is the cheapest reliable test: the entry
     * function is the one `main` the VM will start. */
    if (g_adb.stopAtEntry && !g_adb.entryReached && info->ip == 0) {
        const char* entry = g_adb.info->entryFunctionName(g_adb.host);
        if (entry != NULL && strcmp(functionName(info->functionIndex), entry) == 0) {
            g_adb.entryReached = 1;
            g_adb.stopAtEntry = 0;
            g_adb.modeDepth = info->frameDepth;
            stopAndPrompt(info, -1);
            if (g_adb.quitting) {
                return 1;
            }
        }
    }

    int viaBreakpoint = -1;
    const int stopped = shouldStop(info, &viaBreakpoint);
    if (stopped) {
        stopAndPrompt(info, viaBreakpoint);
        if (g_adb.quitting) {
            return 1;
        }
    }

    /* Record where we are for the next call, after the check: the line
     * breakpoint rule compares the current instruction against the one that ran
     * before it, so updating first would make every line look "already
     * entered". */
    g_adb.previousLine = g_adb.info->sourceLine(g_adb.host, info->functionIndex, info->ip);
    g_adb.previousFunction = info->functionIndex;
    return 0;
}

static void onTrap(void* user, const char* message) {
    (void)user;
    g_adb.trapped = 1;
    g_adb.finished = 1;
    if (message != NULL) {
        snprintf(g_adb.trapMessage, sizeof(g_adb.trapMessage), "%s", message);
    }
    /* A trap is the one moment where the capture buffer earns its keep: it holds
     * the instructions that led here, which is what a crash report cannot show. */
    FILE* out = adbOut();
    if (!g_adb.scripting) {
        fprintf(out, "\n");
    }
    fprintf(out, "== trap ==\n  %s\n", g_adb.trapMessage);
    printPosition(g_adb.stoppedFunction, g_adb.stoppedIp);
    /* Dumped only while capture is live. Once the user has turned it off, the
     * window they asked for is already closed; printing it again at the trap
     * would suggest the run had been traced further than it was. */
    if (g_adb.capturing && g_adb.captureCount > 0) {
        captureDump();
    }
}

static void onFinish(void* user) {
    (void)user;
    g_adb.finished = 1;
    if (!g_adb.quiet && !g_adb.trapped) {
        if (!g_adb.scripting) {
            fprintf(adbOut(), "\n");
        }
        fprintf(adbOut(), "== program finished ==\n");
    }
}

/*===--- public entry points (declared in adb.axd) ----------------------===*/

/* The VM calls `adbInstall` once, through the driver's `--adb` flag, passing the
 * hook table it wants filled. Everything the debugger prints goes through `out`,
 * which lets a test capture a session as text. */
int64_t adbInstall(void* infoTable, void* user, void* in, void* out, int32_t scripting) {
    /* `infoTable` is accepted but may be null: the module is not loaded yet, so
     * the real table arrives through `onStart`. */
    (void)infoTable;
    memset(&g_adb, 0, sizeof(g_adb));
    g_adb.in = (FILE*)in;
    g_adb.out = (FILE*)out != NULL ? (FILE*)out : stdout;
    g_adb.scripting = scripting != 0;
    g_adb.mode = MODE_RUN;
    g_adb.stoppedFunction = -1;
    g_adb.stoppedIp = -1;
    /* A script is a sequence of answers to prompts that will happen; the entry
     * stop is the first of them. An interactive session wants the same stop, so
     * it is unconditional -- there is nothing to configure. */
    g_adb.stopAtEntry = 1;

    g_adb.captureCapacity = ADB_CAPTURE_CAPACITY;
    g_adb.capture = (CapturedStep*)calloc((size_t)ADB_CAPTURE_CAPACITY, sizeof(CapturedStep));
    if (g_adb.capture == NULL) {
        g_adb.captureCapacity = 0;
    }
    (void)user;
    return 0;
}

/* Fills the hook table the VM installs. Kept separate from `adbInstall` so the
 * driver can set the info table up first and the CLI options second. */
int64_t adbHooks(void* hookTable) {
    AdbHooks* hooks = (AdbHooks*)hookTable;
    if (hooks == NULL) {
        return -1;
    }
    hooks->apiVersion = ADB_API_VERSION;
    hooks->user = &g_adb;
    hooks->onStart = onStart;
    hooks->beforeInstruction = onBeforeInstruction;
    hooks->onTrap = onTrap;
    hooks->onFinish = onFinish;
    return 0;
}

/* Adds a breakpoint from the command line or from Axiom code. Returns 0 on
 * success, -1 when the specification is not understood. */
int64_t adbBreak(const char* spec) {
    if (spec == NULL) {
        return -1;
    }
    char why[128];
    if (addBreakpoint(spec, why, sizeof(why)) != 0) {
        fprintf(adbOut(), "adb: %s\n", why);
        return -1;
    }
    return 0;
}

int64_t adbCapture(int32_t enable) {
    const int was = g_adb.capturing;
    g_adb.capturing = enable != 0;
    if (g_adb.capturing && !was) {
        g_adb.captureCount = 0;
        g_adb.captureNext = 0;
    }
    return was;
}

int64_t adbIsAttached(void) {
    return g_adb.info != NULL ? 1 : 0;
}

/* Writes the capture buffer to `out` as text. The debugger's own `capture dump`
 * writes to its own stream; this one exists so a program can dump into a file. */
int64_t adbDumpCapture(void* out) {
    FILE* saved = g_adb.out;
    if (out != NULL) {
        g_adb.out = (FILE*)out;
    }
    captureDump();
    g_adb.out = saved;
    return (int64_t)g_adb.captureCount;
}

int64_t adbStopCount(void) { return g_adb.stopCount; }

/* A program can request a stop without a breakpoint, which is the one thing a
 * printf-debugging session cannot do on its own. */
int64_t adbTrap(void) {
    if (g_adb.info == NULL) {
        return -1;
    }
    AdbStepInfo fake;
    memset(&fake, 0, sizeof(fake));
    fake.functionIndex = g_adb.stoppedFunction;
    fake.ip = g_adb.stoppedIp;
    fake.frameDepth = g_adb.stoppedDepth;
    stopAndPrompt(&fake, -1);
    return g_adb.quitting ? 1 : 0;
}
