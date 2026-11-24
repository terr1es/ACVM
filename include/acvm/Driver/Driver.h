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

/// Wires the stages together:
///
///   source -> Lexer -> Parser -> Sema -> IRGen -> Module -> VM / container
///
/// Every entry point returns a process exit code (0 = success).
class Driver {
public:
    Driver(std::ostream& out, std::ostream& err);

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
    int disassembleBytecode(const std::string& path);

private:
    bool compile(const std::string& path, Module& module, const DriverOptions& options);

    /// Loads `adb` next to the standard library and installs its hooks into the
    /// VM. Returns false with a message when `--adb` was requested but the library
    /// is missing -- a debugger that silently does not attach is worse than one
    /// that refuses to start.
    bool attachDebugger(VM& vm, const DriverOptions& options, std::string& error);

    std::vector<std::string> m_programArgs;
    /// Loads every file reachable from `root`'s import declarations and merges
    /// their declarations into `root`, dependencies first (spec §6.3).
    bool expandImports(Unit& root, const std::string& rootPath, SourceManager& sources,
                       DiagnosticEngine& diagnostics);
    void reportStats(const VM& vm, const Module& module);

    std::ostream& m_out;
    std::ostream& m_err;
    bool m_stats = false;
    bool m_statsDetail = false;
};

} // namespace acvm
