//===--- Driver.h - Orchestrates the ACVM pipeline ----------------------===//
#pragma once

#include "acvm/IR/Bytecode.h"
#include "acvm/IR/Optimize.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace acvm {

class VM;
class SourceManager;
class DiagnosticEngine;
struct Module;
struct Unit;

struct DriverOptions {
    bool dumpTokens = false;
    bool dumpAst = false;
    bool dumpIr = false;
    bool assertions = true;
    /// Compile hot-qualifying functions to machine code. Off by default: the JIT is
    /// opt-in until a differential run has shown it agrees with the interpreter on
    /// everything the test suite covers.
    bool jit = false;
    /// Print retired instruction count after a run (performance work).
    bool stats = false;
    /// Also print the per-opcode breakdown.
    bool statsDetail = false;
    /// How hard to optimize. Defaults to O2: the same default clang and gcc use,
    /// and the level at which constant folding and branch resolution are on but
    /// nothing speculative happens.
    OptLevel optimize = OptLevel::O2;

    /// Attach the Axiom Debugger (`--adb`). The debugger itself lives in
    /// `stdlib/adb.c`; this flag only closes the circuit between the VM's hook
    /// slot and that library, so a run without it pays one pointer test per
    /// instruction and nothing else.
    bool adb = false;
    /// Breakpoints given on the command line, in `adbBreak` syntax.
    std::vector<std::string> adbBreaks;
    /// Read debugger commands from this file instead of the terminal. A scripted
    /// session is how the debugger is tested: the transcript is the assertion.
    std::string adbScript;
    /// Suppress the banner and the per-stop chatter (for scripted runs).
    bool adbQuiet = false;
};

/// One module a compile pulled in, and where it was found.
///
/// Recorded by the same walk that resolves imports, which is the point: a packager that
/// recomputed this from the sources would be a second implementation of module resolution,
/// and the two would drift the first time a search rule changed. The compiler already knows
/// the answer, so it says it.
struct ModuleDependency {
    /// The `library "name";` the module declares, empty when the module is pure Axiom.
    /// A non-empty value names a shared library that must ship with the program.
    std::string library;
    /// Absolute path of the `.ax` / `.axd` file, which is also where a project-local
    /// library's shared object lives.
    std::string path;
    /// `module "name" version "x.y.z";`, when the file declared one.
    std::string module;
    std::string version;
};

/// Wires the stages together:
///
///   source -> Lexer -> Parser -> Sema -> IRGen -> Module -> VM / container
///
/// Every entry point returns a process exit code (0 = success).
class Driver {
public:
    Driver(std::ostream& out, std::ostream& err);

    /// Every module the last compile loaded, in load order. Empty before a compile, and
    /// left untouched by an entry point that failed before resolution.
    [[nodiscard]] const std::vector<ModuleDependency>& dependencies() const {
        return m_dependencies;
    }

    /// Program arguments given after `--`; visible to the program through
    /// `argCount()` / `argAt(i)`. Applies to `runSource` and `runBytecode`.
    void setProgramArgs(std::vector<std::string> args) { m_programArgs = std::move(args); }

    int runSource(const std::string& path, const DriverOptions& options);
    int buildSource(const std::string& path, const std::string& output,
                    const DriverOptions& options);
    int checkSource(const std::string& path, const DriverOptions& options);
    int dumpTokenStream(const std::string& path);
    int dumpSyntaxTree(const std::string& path);
    int dumpBytecode(const std::string& path, const DriverOptions& options = {});
    int runBytecode(const std::string& path, const DriverOptions& options = {});

    /// Runs a module that is already in memory.
    ///
    /// `origin` names the payload in diagnostics and, more importantly, is what the shared
    /// library search path is derived from -- so a payload appended to an executable looks
    /// for its libraries beside that executable, exactly as a file beside it would.
    int runBytecodeFromMemory(const void* data, std::size_t size, const std::string& origin,
                              const DriverOptions& options = {});
    int disassembleBytecode(const std::string& path);

private:
    bool compile(const std::string& path, Module& module, const DriverOptions& options);

    /// Loads `adb` next to the standard library and installs its hooks into the
    /// VM. Returns false with a message when `--adb` was requested but the library
    /// is missing -- a debugger that silently does not attach is worse than one
    /// that refuses to start.
    bool attachDebugger(VM& vm, const DriverOptions& options, std::string& error);

    std::vector<std::string> m_programArgs;
    std::vector<ModuleDependency> m_dependencies;
    /// Loads every file reachable from `root`'s import declarations and merges
    /// their declarations into `root`, dependencies first (spec §6.3).
    bool expandImports(Unit& root, const std::string& rootPath, SourceManager& sources,
                       DiagnosticEngine& diagnostics);
    void reportStats(const VM& vm, const Module& module);

    std::ostream& m_out;
    std::ostream& m_err;
    bool m_stats = false;
    bool m_statsDetail = false;

    /// The part of running a module that does not depend on where its bytes came from.
    int runLoaded(Module& module, const std::string& origin, const DriverOptions& options);
};

} // namespace acvm
