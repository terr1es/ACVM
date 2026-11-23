//===--- Driver.h - Orchestrates the ACVM pipeline ----------------------===//
#pragma once

#include "acvm/IR/Bytecode.h"
#include "acvm/IR/Optimize.h"

#include <iosfwd>
#include <string>

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
