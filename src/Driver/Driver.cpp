//===--- Driver.cpp -----------------------------------------------------===//
#include "acvm/Driver/Driver.h"

#include "OfficialExtraModules.h"
#include "acvm/Common/TextDistance.h"

#include "acvm/AST/ASTPrinter.h"
#include "acvm/Common/Diagnostic.h"
#include "acvm/Common/SourceManager.h"
#include "acvm/IR/Disassembler.h"
#include "acvm/IR/IRGen.h"
#include "acvm/IR/Optimize.h"
#include "acvm/IR/Serializer.h"
#include "acvm/Lex/Lexer.h"
#include "acvm/Parse/Parser.h"
#include "acvm/Sema/Sema.h"
#include "acvm/VM/VM.h"
#include "acvm/VM/DebugApi.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace acvm {


namespace {

/// Absolute, lexically normal form of `path`. Used as the identity of a module
/// so that the same file reached by two different routes is loaded once.
std::string canonicalPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        result = std::filesystem::absolute(path, ec);
    }
    return result.generic_string();
}

/// Directories searched for `import (name)`, in order: the importing file's own
/// directory, then the standard library, then the extra library.
///
/// `ACVM_STDLIB` overrides the built-in path, which is what lets a checkout run
/// the tests from anywhere and an installed toolchain point at its own stdlib.
/// `ACVM_EXTRALIB` does the same for the optional layer; without it the extra
/// directory is taken to be a sibling of the standard one, which is how both a
/// checkout and an installed toolchain are laid out.
///
/// `extralib` comes last on purpose. A name that exists in both places must
/// resolve to `stdlib` -- the optional layer may be replaced or deleted, and a
/// program's meaning must not change because of that.
/// Directory holding the running executable, or empty when the platform will not
/// say.
///
/// The installed layout is derived from this: `acvm` in `.../bin` with its modules
/// in `.../share/acvm/stdlib`. Without that fallback an installed toolchain would
/// only work on the machine that built it, because the compile-time default below
/// points into the source tree -- which is exactly the bug this exists to fix. A
/// copy of `acvm` in another directory used to fail with a parse error from a
/// *stale* stdlib it found on the build machine, or with "module not found" once
/// that tree was gone.
std::filesystem::path executableDir() {
    // The platform headers live in VM.cpp, which already needs them for dynamic
    // loading. Asking it for the path keeps this file free of `windows.h` -- whose
    // `near` and `far` macros are empty and silently break any variable of that
    // name (there is one below, in a diagnostic).
    const std::string path = vmExecutablePath();
    if (path.empty()) {
        return {};
    }
    return std::filesystem::path(path).parent_path();
}

/// Appends `candidate` when it is a directory and not already present.
///
/// Duplicates matter here: a build tree can have the executable's directory and the
/// compile-time default be the same path, and searching it twice would make the
/// diagnostic about a missing module list the same directory twice.
void addDirIfPresent(std::vector<std::filesystem::path>& dirs,
                     const std::filesystem::path& candidate) {
    if (candidate.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(candidate, ec)) {
        return;
    }
    const std::filesystem::path normal = std::filesystem::weakly_canonical(candidate, ec);
    for (const std::filesystem::path& existing : dirs) {
        const std::filesystem::path other = std::filesystem::weakly_canonical(existing, ec);
        if (other == normal) {
            return;
        }
    }
    dirs.push_back(normal);
}

/// One module directory, resolved in the order a user can predict:
///
///   1. `$ACVM_STDLIB` / `$ACVM_EXTRALIB` -- an explicit override always wins
///   2. next to the executable, in the installed layout (`../share/acvm/<name>` and
///      a plain `<exe>/<name>` sibling)
///   3. the compile-time default, which is the source tree in a checkout
///
/// Installed before default, not the other way round: an installed toolchain must
/// be self-consistent, and its compile-time default still names the machine that
/// built it. In a checkout the installed layout does not exist, so the default is
/// what gets used and nothing changes.
void addModuleDir(std::vector<std::filesystem::path>& dirs, const char* envName,
                  const char* baked, const char* leaf,
                  const std::filesystem::path& exeDir) {
    if (const char* env = std::getenv(envName); env != nullptr && env[0] != '\0') {
        addDirIfPresent(dirs, env);
        return;
    }
    if (!exeDir.empty()) {
        addDirIfPresent(dirs, exeDir / leaf);
        addDirIfPresent(dirs, exeDir / ".." / "share" / "acvm" / leaf);
    }
    if (baked != nullptr && baked[0] != '\0') {
        addDirIfPresent(dirs, baked);
    }
}

std::vector<std::filesystem::path> moduleSearchDirs(const std::filesystem::path& importingDir) {
    std::vector<std::filesystem::path> dirs;

    // An empty `importingDir` means the source was named without a directory --
    // `acvm run prog.ax` -- whose parent_path() is "" and which therefore means the
    // current directory. Skipping it as "absent" silently removed the caller's own
    // directory from the search path, so `import (mylib)` only worked when the file
    // was named by an absolute path. "." is spelled out so `addDirIfPresent` can
    // still reject a genuinely missing directory.
    addDirIfPresent(dirs, importingDir.empty() ? std::filesystem::path(".") : importingDir);

    // The executable's directory is looked up once and shared by both layers.
    const std::filesystem::path exeDir = executableDir();

#ifdef ACVM_STDLIB_DIR
    const char* bakedStdlib = ACVM_STDLIB_DIR;
#else
    const char* bakedStdlib = nullptr;
#endif
#ifdef ACVM_EXTRALIB_DIR
    const char* bakedExtralib = ACVM_EXTRALIB_DIR;
#else
    const char* bakedExtralib = nullptr;
#endif

    addModuleDir(dirs, "ACVM_STDLIB", bakedStdlib, "stdlib", exeDir);
    addModuleDir(dirs, "ACVM_EXTRALIB", bakedExtralib, "extralib", exeDir);
    return dirs;
}

/// The official extra modules this compiler was built with, from a header CMake
/// generates out of `extralib/`.
///
/// They exist only to improve one diagnostic. Nothing here requires any of them
/// to be installed, and a name that is not in this list is simply unknown -- a
/// user's own module of the same name is indistinguishable from the official one,
/// which is correct: the list describes a *release*, not the user's disk.
[[nodiscard]] bool isOfficialExtraModule(const std::string& name) {
    for (const std::string_view candidate : kOfficialExtraModules) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

/// Closest official extra module within a small edit distance, for `import
/// (jason)`. Returns an empty string when nothing is close enough to be worth
/// suggesting -- a wrong guess is worse than no guess.
std::string nearestOfficialExtraModule(const std::string& name) {
    std::string best;
    std::size_t bestDistance = 0;
    for (const std::string_view candidate : kOfficialExtraModules) {
        const std::size_t distance = editDistance(name, std::string(candidate));
        // Two edits covers a transposition or a single wrong character; longer
        // names tolerate a little more.
        const std::size_t limit = candidate.size() >= 5 ? 2 : 1;
        if (distance <= limit && (best.empty() || distance < bestDistance)) {
            best = std::string(candidate);
            bestDistance = distance;
        }
    }
    return best;
}

/// Extensions an `import (name)` may resolve to, in priority order:
///
///   .axd  declarations for a shared library (`library "x";`), no code
///   .ax   ordinary Axiom source
///
/// `.axd` comes first so a declaration file is preferred over a same-named
/// implementation, but the names rarely collide: a module is normally one or
/// the other.
constexpr const char* kModuleExtensions[] = {".axd", ".ax"};

/// True when `path` ends in `.axd`, i.e. it is meant to be a declaration file.
bool hasDeclarationExtension(const std::string& path) {
    return path.size() >= 4 && path.compare(path.size() - 4, 4, ".axd") == 0;
}

/// `Math` -> `Math.axd`, `Math.ax`, then the lowercase variants, in each search
/// directory. Spec §6.1 makes one file a compilation unit; the convention is
/// the module name as the file name.
std::vector<std::filesystem::path> moduleCandidates(const std::string& name,
                                                    const std::vector<std::filesystem::path>& dirs) {
    std::string lower = name;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::vector<std::filesystem::path> candidates;
    for (const char* extension : kModuleExtensions) {
        for (const std::filesystem::path& dir : dirs) {
            candidates.push_back(dir / (name + extension));
        }
    }
    if (lower != name) {
        for (const char* extension : kModuleExtensions) {
            for (const std::filesystem::path& dir : dirs) {
                candidates.push_back(dir / (lower + extension));
            }
        }
    }
    return candidates;
}

/// True when `name` resolves to a file the loader would accept. Used by the
/// suggestion path: "'net' is installed" and "'net' is an official module you do
/// not have" need different words, and guessing wrong makes the note a lie.
bool moduleIsPresent(const std::string& name, const std::filesystem::path& importingDir) {
    for (const std::filesystem::path& candidate :
         moduleCandidates(name, moduleSearchDirs(importingDir))) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return true;
        }
    }
    return false;
}

/// `module "json" version "0.1.0";` must name the file it is written in.
///
/// A declaration that can disagree with reality is worse than no declaration: the
/// package tooling of the future will trust this string, and a stale one would make
/// it resolve the wrong module -- silently, since both files compile. Checking the
/// stem costs one string compare and turns that into a diagnostic at the moment the
/// file is renamed.
///
/// The extension is stripped because a module may be `.ax` or `.axd`: `net` is
/// declared in `net.axd`, and both spell the module `net`.
bool checkModuleIdentity(const Unit& unit, const std::filesystem::path& path,
                         DiagnosticEngine& diagnostics) {
    if (!unit.declaresModule()) {
        return true;
    }
    const std::string stem = path.stem().generic_string();
    if (unit.moduleName == stem) {
        return true;
    }
    diagnostics.error(unit.moduleRange,
                      "this file declares module '" + unit.moduleName + "', but it is '" +
                          path.filename().generic_string() +
                          "'; a module's name must match its file name, because that is what "
                          "'import (" +
                          unit.moduleName + ")' searches for");
    return false;
}

struct ModuleGraph {
    std::vector<std::unique_ptr<Unit>> order;   // dependencies before dependents
    std::map<std::string, int> state;           // 1 = on the stack, 2 = done
    std::vector<std::string> stack;             // for cycle reporting
};

} // namespace

/// Locates the Axiom fallback for a declaration file: `library "stdlib"` in
/// stdlib.axd pairs with stdlib.ax, which implements the same functions in
/// portable Axiom so a program still runs where the shared library is absent.
/// Returns an empty path when there is no such file, which is not an error --
/// not every C function has a sensible Axiom equivalent.
std::filesystem::path findLibraryFallback(const std::string& libraryName,
                                          const std::filesystem::path& declarationFile) {
    const std::vector<std::filesystem::path> dirs =
        moduleSearchDirs(declarationFile.parent_path());
    for (const std::filesystem::path& dir : dirs) {
        const std::filesystem::path candidate = dir / (libraryName + ".ax");
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

bool Driver::expandImports(Unit& root, const std::string& rootPath, SourceManager& sources,
                           DiagnosticEngine& diagnostics) {
    ModuleGraph graph;
    m_dependencies.clear();

    // Depth first, post order: a module's own imports are emitted before it, so
    // the merged declaration list follows the load order of spec §6.3.
    std::function<bool(Unit&, const std::filesystem::path&)> visit =
        [&](Unit& unit, const std::filesystem::path& self) -> bool {
        const std::filesystem::path dir = self.parent_path();
        const std::string selfKey = canonicalPath(self);
        // Recorded here rather than after the merge because the merge throws away which
        // file each declaration came from, and a packager needs the file to find the
        // shared object next to it. `self` is the path this unit was actually loaded
        // from, which is the only answer that accounts for the search order.
        m_dependencies.push_back(ModuleDependency{unit.libraryName, selfKey, unit.moduleName,
                                                  unit.moduleVersion});
        bool ok = true;
        for (StmtPtr& decl : unit.decls) {
            if (!decl || decl->kind != StmtKind::ImportDecl) {
                continue;
            }
            for (const auto& [name, range] : decl->modules) {
                std::filesystem::path resolved;
                bool found = false;

                // A file may import a module that has the same name as itself
                // (`examples/algorithm.ax` importing `algorithm`); that means
                // the library, not this file, so the file itself is skipped
                // while searching rather than reported as a self cycle.
                bool sawSelf = false;
                const auto acceptable = [&](const std::filesystem::path& candidate) {
                    std::error_code ec;
                    if (!std::filesystem::exists(candidate, ec)) {
                        return false;
                    }
                    if (canonicalPath(candidate) == selfKey) {
                        sawSelf = true;
                        return false;
                    }
                    return true;
                };

                if (decl->modulesArePaths) {
                    const std::filesystem::path given(name);
                    const std::filesystem::path candidate = given.is_absolute() ? given : dir / given;
                    if (acceptable(candidate)) {
                        resolved = candidate;
                        found = true;
                    }
                } else {
                    const std::vector<std::filesystem::path> search = moduleSearchDirs(dir);
                    for (const std::filesystem::path& candidate : moduleCandidates(name, search)) {
                        if (acceptable(candidate)) {
                            resolved = candidate;
                            found = true;
                            break;
                        }
                    }
                }

                if (!found && sawSelf) {
                    diagnostics.error(range, "'" + name + "' resolves to this file itself; "
                                                     "a file cannot import itself");
                    ok = false;
                    continue;
                }
                if (!found) {
                    if (decl->modulesArePaths) {
                        const std::string where =
                            dir.empty() ? std::string(".") : dir.generic_string();
                        diagnostics.error(range, "cannot find module '" + name + "' (looked for '" +
                                                     where + "/" + name + "')");
                    } else if (isOfficialExtraModule(name)) {
                        // The module is part of this release but not on disk.
                        // Deleting an extra module is a supported thing to do, so
                        // this is not a mistake in the program -- saying "cannot
                        // find" and listing eight paths would send the reader
                        // looking for a typo that is not there.
                        diagnostics.error(range, "module '" + name + "' is not installed");
                        diagnostics.help(range.begin,
                                         "'" + name +
                                             "' is an official extra module; reinstall it, or "
                                             "write " +
                                             name + ".ax in an 'extralib' directory");
                    } else if (const std::string near = nearestOfficialExtraModule(name);
                               !near.empty()) {
                        // Almost certainly a typo. Both halves are useful: the
                        // correction, and the fact that it is not installed, so
                        // the next message is not a surprise.
                        diagnostics.error(range, "cannot find module '" + name + "'");
                        // Only mention installation when it is actually missing.
                        // The typo may well be in a name that is installed, and
                        // telling the reader to reinstall it would be a lie.
                        diagnostics.help(range.begin,
                                         "did you mean '" + near + "'?" +
                                             (moduleIsPresent(near, dir)
                                                  ? std::string()
                                                  : " (an official extra module, not installed)"));
                    } else {
                        std::string searched;
                        for (const std::filesystem::path& candidate :
                             moduleCandidates(name, moduleSearchDirs(dir))) {
                            if (!searched.empty()) {
                                searched += ", ";
                            }
                            searched += candidate.generic_string();
                        }
                        diagnostics.error(range,
                                          "cannot find module '" + name + "' (looked for " +
                                              searched +
                                              "; set ACVM_STDLIB to add a library directory)");
                    }
                    ok = false;
                    continue;
                }

                const std::string key = canonicalPath(resolved);

                // Cycle: the file is already being loaded further up the stack.
                const auto state = graph.state.find(key);
                if (state != graph.state.end() && state->second == 1) {
                    const auto first = std::find(graph.stack.begin(), graph.stack.end(), key);
                    std::string chain;
                    for (auto it = first; it != graph.stack.end(); ++it) {
                        chain += std::filesystem::path(*it).filename().generic_string();
                        chain += " -> ";
                    }
                    chain += std::filesystem::path(key).filename().generic_string();
                    diagnostics.error(range, "circular import: " + chain);
                    ok = false;
                    continue;
                }
                if (state != graph.state.end() && state->second == 2) {
                    continue;   // already loaded by another route
                }

                std::string readError;
                const FileID file = sources.loadFile(key, readError);
                if (file == kInvalidFileID) {
                    diagnostics.error(range, readError);
                    ok = false;
                    continue;
                }

                const unsigned errorsBefore = diagnostics.errorCount();
                Lexer lexer(sources, file, diagnostics);
                Parser parser(lexer.tokenize(), diagnostics);
                std::unique_ptr<Unit> imported = parser.parseUnit();
                if (diagnostics.errorCount() != errorsBefore) {
                    ok = false;
                    continue;
                }
                if (!checkModuleIdentity(*imported, key, diagnostics)) {
                    ok = false;
                    continue;
                }

                // A module may define `main`; the flat-scope rule of spec Q43
                // lets the importing file's own definition override it.

                // The same rule as for the root file: an `.axd` that does not
                // declare a library is a mistake, and silently compiling it as
                // ordinary Axiom would hide it behind "no entry point" later.
                if (hasDeclarationExtension(key) && !imported->isExternalLibrary()) {
                    diagnostics.error(
                        range, "'" + std::filesystem::path(key).filename().generic_string() +
                                   "' has the .axd extension but does not declare a library; "
                                   "add 'library \"name\";' at the top, or rename it to .ax");
                    ok = false;
                    continue;
                }

                graph.state[key] = 1;
                graph.stack.push_back(key);
                if (!visit(*imported, resolved)) {
                    ok = false;
                }
                graph.stack.pop_back();
                graph.state[key] = 2;

                // Read what is needed before the unit is moved into the order:
                // after `std::move(imported)` the pointer is null.
                const bool declaresLibrary = imported->isExternalLibrary();
                const std::string declaredLibraryName = imported->libraryName;
                graph.order.push_back(std::move(imported));

                // A declaration file for a shared library may be accompanied by
                // an Axiom implementation of the same module name: stdlib.axd
                // declares `library "stdlib"`, and stdlib.ax then supplies a
                // portable fallback. It is compiled in and shipped inside the
                // .acvm, so the program still runs where the .dll is missing
                // (docs/PLUGIN.md). Sema keeps both: the forward declaration
                // carries the symbol, the definition supplies the body.
                if (declaresLibrary) {
                    const std::filesystem::path fallbackPath =
                        findLibraryFallback(declaredLibraryName, resolved);
                    const std::string fallbackKey =
                        fallbackPath.empty() ? std::string() : canonicalPath(fallbackPath);
                    const bool alreadyLoaded =
                        !fallbackKey.empty() && graph.state.find(fallbackKey) != graph.state.end();
                    if (!fallbackPath.empty() && !alreadyLoaded) {
                        std::string fallbackError;
                        const FileID fallbackFile = sources.loadFile(fallbackKey, fallbackError);
                        if (fallbackFile == kInvalidFileID) {
                            diagnostics.error(range, fallbackError);
                            ok = false;
                        } else {
                            const unsigned before = diagnostics.errorCount();
                            Lexer fallbackLexer(sources, fallbackFile, diagnostics);
                            Parser fallbackParser(fallbackLexer.tokenize(), diagnostics);
                            std::unique_ptr<Unit> fallback = fallbackParser.parseUnit();
                            if (diagnostics.errorCount() != before) {
                                ok = false;
                            } else if (fallback->isExternalLibrary()) {
                                diagnostics.error(
                                    range, "'" + fallbackKey + "' is the Axiom fallback for library '" +
                                               declaredLibraryName +
                                               "', so it must not declare a library of its own");
                                ok = false;
                            } else {
                                graph.state[fallbackKey] = 1;
                                graph.stack.push_back(fallbackKey);
                                if (!visit(*fallback, fallbackKey)) {
                                    ok = false;
                                }
                                graph.stack.pop_back();
                                graph.state[fallbackKey] = 2;
                                graph.order.push_back(std::move(fallback));
                            }
                        }
                    }
                }
            }
        }
        return ok;
    };

    if (!visit(root, rootPath)) {
        return false;
    }

    // The root file can itself be a library declaration file, in which case its
    // own declarations are external too -- tag them on the same path as the
    // imported ones, so the origin is recorded exactly once.
    if (root.isExternalLibrary()) {
        for (StmtPtr& decl : root.decls) {
            if (decl) {
                decl->externalLibrary = root.libraryName;
            }
        }
    }

    // Dependencies first, then the root file's own declarations. A file that
    // declared `library "x";` contributes symbols implemented in that shared
    // library, so every declaration it exports is tagged here: after the splice
    // there is nothing left to say which file it came from.
    std::vector<StmtPtr> merged;
    for (std::unique_ptr<Unit>& unit : graph.order) {
        for (StmtPtr& decl : unit->decls) {
            if (unit->isExternalLibrary() && decl) {
                decl->externalLibrary = unit->libraryName;
            }
            merged.push_back(std::move(decl));
        }
    }
    for (StmtPtr& decl : root.decls) {
        merged.push_back(std::move(decl));
    }
    root.decls = std::move(merged);
    return true;
}

Driver::Driver(std::ostream& out, std::ostream& err) : m_out(out), m_err(err) {}

bool Driver::compile(const std::string& path, Module& module, const DriverOptions& options) {
    SourceManager sources;
    DiagnosticEngine diagnostics(sources, m_err);

    std::string readError;
    const FileID file = sources.loadFile(path, readError);
    if (file == kInvalidFileID) {
        diagnostics.error(SourceLocation{}, readError);
        return false;
    }

    Lexer lexer(sources, file, diagnostics);
    std::vector<Token> tokens = lexer.tokenize();

    if (options.dumpTokens) {
        for (const Token& token : tokens) {
            const SourceManager::LineColumn lc = sources.lineColumn(token.range.begin);
            m_out << lc.line << ':' << lc.column << "  " << tokenKindSpelling(token.kind);
            if (!token.text.empty()) {
                m_out << "  '" << token.text << "'";
            }
            m_out << '\n';
        }
        m_out << '\n';
    }

    if (diagnostics.hasErrors()) {
        return false;
    }

    Parser parser(std::move(tokens), diagnostics);
    std::unique_ptr<Unit> unit = parser.parseUnit();
    if (unit != nullptr) {
        checkModuleIdentity(*unit, path, diagnostics);
    }

    if (options.dumpAst) {
        dumpAst(*unit, m_out);
        m_out << '\n';
    }

    if (diagnostics.hasErrors()) {
        return false;
    }

    if (!expandImports(*unit, path, sources, diagnostics)) {
        return false;
    }
    if (diagnostics.hasErrors()) {
        return false;
    }

    // A `.axd` file without `library "x";` is almost always a mistake: the
    // declarations are then compiled as ordinary Axiom, so the run fails later
    // with "no entry point" or a trap stub instead of saying what is missing.
    if (hasDeclarationExtension(path) && !unit->isExternalLibrary() && !diagnostics.hasErrors()) {
        diagnostics.error(SourceRange{},
                          "'" + std::filesystem::path(path).filename().generic_string() +
                              "' has the .axd extension but does not declare a library; add "
                              "'library \"name\";' at the top, or rename it to .ax");
        return false;
    }

    TypeContext types;
    Sema sema(diagnostics, types);
    sema.setRootFile(file);
    sema.setExternalLibrary(unit->isExternalLibrary(), unit->libraryName);
    if (!sema.check(*unit)) {
        return false;
    }

    IRGen generator(diagnostics, sema);
    // O4 deletes safety checks, and `assert` is one of them: it exists to stop a
    // program that has already gone wrong. Disabling it at this level is the
    // compile-time equivalent of the deletion the optimizer does to safe points,
    // and it is why O4 does not promise the same execution flow.
    generator.setAssertionsEnabled(options.assertions &&
                                  !optLevelMayChangeFlow(options.optimize));
    // Line tables: the debugger needs them, the optimizer moves them, and a
    // module built without a source manager simply records "unknown" positions.
    generator.setSourceManager(&sources);
    if (!generator.generate(*unit, module)) {
        return false;
    }

    // Global data lives in [kGlobalBase, kStringBase) and the string literal
    // table starts at kStringBase. Nothing between them bounds the globals, so a
    // large global array used to run straight into the literal descriptors and
    // overwrite them: a program with `global string g[20000]` and 16000 writes
    // printed 4 GB of NUL bytes instead of its output, with no diagnostic
    // anywhere. Refuse it here rather than let the VM corrupt itself.
    constexpr std::uint64_t kGlobalCapacity = VM::kStringBase - VM::kGlobalBase;
    if (module.globalDataSize > kGlobalCapacity) {
        diagnostics.error(SourceRange{},
                          "global data needs " + std::to_string(module.globalDataSize) +
                              " bytes but only " + std::to_string(kGlobalCapacity) +
                              " are available before the string literal table; move large "
                              "arrays into a function's local scope or allocate them with "
                              "'alloc'");
        return false;
    }

    // Optimization runs after IRGen and before anything reads the module, so
    // --ir shows the code the VM will actually execute.
    const OptimizeStats optimized = optimizeModule(module, options.optimize);
    if (options.stats && options.optimize != OptLevel::O0) {
        printOptimizeStats(optimized, options.optimize, m_err);
    }

    if (options.dumpIr) {
        dumpModule(module, m_out);
        m_out << '\n';
    }

    return true;
}

/// Where `library "x"` looks for x.dll / libx.so.
///
/// The module's own directory comes first so a program can ship a private copy of a
/// library; then the directory holding `acvm` itself, which is where an installed
/// toolchain keeps the shared half of the standard library; then the build tree
/// default. The executable's directory has to be listed explicitly rather than left
/// to the loader: Windows searches the application directory, Linux does not, and a
/// relocated toolchain must behave the same on both.
std::vector<std::string> librarySearchDirs(const std::string& sourcePath) {
    std::vector<std::string> dirs;
    dirs.push_back(std::filesystem::path(sourcePath).parent_path().generic_string());
    const std::filesystem::path exeDir = executableDir();
    if (!exeDir.empty()) {
        dirs.push_back(exeDir.generic_string());
    }
#ifdef ACVM_LIB_DIR
    dirs.push_back(ACVM_LIB_DIR);
#endif
    return dirs;
}

//===----------------------------------------------------------------------===//
// Debugger attachment
//===----------------------------------------------------------------------===//

/// The debugger's own ABI, mirroring `stdlib/adb.c`.
///
/// Declared here rather than in a shared header because the direction of the
/// dependency matters: the VM must not know that `adb` exists, and `adb` must not
/// include the VM's C++ headers. The driver is the only place that needs both, so
/// it is the only place that names both.
namespace {

using AdbInstallFn = std::int64_t (*)(void* infoTable, void* user, void* in, void* out,
                                      std::int32_t scripting);
using AdbHooksFn = std::int64_t (*)(void* hookTable);
using AdbBreakFn = std::int64_t (*)(const char* spec);

struct AdbBindings {
    void* handle = nullptr;
    AdbInstallFn install = nullptr;
    AdbHooksFn hooks = nullptr;
    AdbBreakFn addBreak = nullptr;
};

/// Loads `adb.dll` from the same places the VM looks for a module library.
AdbBindings loadAdb(std::string& error) {
    AdbBindings bindings;
    std::vector<std::string> candidates;
#ifdef ACVM_LIB_DIR
    candidates.push_back(std::string(ACVM_LIB_DIR) +
                         (sizeof(void*) == 8 ? "/adb.dll" : "/adb.dll"));
#endif
    candidates.push_back("adb.dll");
    candidates.push_back("libadb.so");

    for (const std::string& path : candidates) {
        void* handle = vmOpenLibrary(path);
        if (handle == nullptr) {
            continue;
        }
        bindings.handle = handle;
        bindings.install = reinterpret_cast<AdbInstallFn>(vmLookupSymbol(handle, "adbInstall"));
        bindings.hooks = reinterpret_cast<AdbHooksFn>(vmLookupSymbol(handle, "adbHooks"));
        bindings.addBreak = reinterpret_cast<AdbBreakFn>(vmLookupSymbol(handle, "adbBreak"));
        if (bindings.install != nullptr && bindings.hooks != nullptr) {
            return bindings;
        }
        vmCloseLibrary(handle);
        bindings = AdbBindings{};
    }
    error = "cannot load 'adb' (build adb.dll next to the standard library, or drop --adb)";
    return bindings;
}

} // namespace

bool Driver::attachDebugger(VM& vm, const DriverOptions& options, std::string& error) {
    AdbBindings bindings = loadAdb(error);
    if (bindings.handle == nullptr) {
        return false;
    }

    // The debugger reads its commands from a file when one was given, so a test
    // can assert on the transcript. Interactive use keeps stdin.
    void* input = stdin;
    FILE* script = nullptr;
    if (!options.adbScript.empty()) {
        script = std::fopen(options.adbScript.c_str(), "r");
        if (script == nullptr) {
            error = "cannot open the debugger script '" + options.adbScript + "'";
            return false;
        }
        input = script;
    }

    // The info table has to exist before the debugger starts: `adbBreak` resolves
    // function names through it, which is what turns a typo into an immediate
    // message instead of a breakpoint that never fires.
    const DebugInfo& info = vm.debugInfo();
    bindings.install(const_cast<DebugInfo*>(&info), nullptr, input, stdout,
                     script != nullptr ? 1 : 0);

    for (const std::string& spec : options.adbBreaks) {
        if (bindings.addBreak(spec.c_str()) != 0) {
            error = "adb: cannot set a breakpoint at '" + spec + "'";
            return false;
        }
    }

    DebugHooks hooks;
    bindings.hooks(&hooks);
    vm.setDebugHooks(&hooks);
    return true;
}

int Driver::runSource(const std::string& path, const DriverOptions& options) {
    m_stats = options.stats;
    m_statsDetail = options.statsDetail;
    Module module;
    if (!compile(path, module, options)) {
        return 1;
    }

    VM vm(m_out, m_err);
    vm.setAssertionsEnabled(options.assertions);
    vm.setProgramArgs(m_programArgs);
    vm.setProfile(options.stats);
    // A breakpoint inside compiled code cannot stop anything: the lowering emits whole
    // functions and has no mapping back to instruction addresses. Debugging is not the
    // moment to be fast, so asking for both turns the JIT off and says so, rather than
    // letting the debugger silently miss breakpoints in the very function being debugged.
    if (options.jit && options.adb) {
        m_err << "note: --jit is off while --adb is attached (compiled code has no "
                  "breakpoints)\n";
    }
    vm.setJitMode(options.jit && !options.adb ? JitMode::On : JitMode::Off);
    vm.setLibrarySearchPath(librarySearchDirs(path));
    std::string error;
    if (options.adb) {
        if (!attachDebugger(vm, options, error)) {
            m_err << "error: " << error << '\n';
            return 1;
        }
    }
    const bool ok = vm.run(module, error);
    if (options.stats) {
        reportStats(vm, module);
    }
    if (!ok) {
        m_err << "runtime error: " << error << '\n';
        return 1;
    }
    return 0;
}

void Driver::reportStats(const VM& vm, const Module& module) {
    std::uint64_t codeBytes = 0;
    for (const Function& function : module.functions) {
        codeBytes += function.code.size() * sizeof(Instruction);
    }
    m_err << "stats: " << vm.instructionCount() << " instructions, " << module.functions.size()
          << " function(s), " << codeBytes << " bytes of code\n";

    if (vm.jitMode() == JitMode::On) {
        m_err << "jit: " << vm.jitCompiledCount() << " compiled, " << vm.jitRefusedCount()
              << " refused";
        if (!vm.jitRefusal().empty()) {
            m_err << " (first refusal: " << vm.jitRefusal() << ')';
        }
        m_err << '\n';
    }

    if (m_statsDetail) {
        struct Entry {
            std::uint64_t count;
            unsigned op;
        };
        std::vector<Entry> entries;
        const std::uint64_t* histogram = vm.opcodeHistogram();
        for (unsigned i = 0; i < 64; ++i) {
            if (histogram[i] != 0) {
                entries.push_back(Entry{histogram[i], i});
            }
        }
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& lhs, const Entry& rhs) { return lhs.count > rhs.count; });
        const std::uint64_t total = vm.instructionCount();
        for (const Entry& entry : entries) {
            const double share =
                total == 0 ? 0.0 : 100.0 * static_cast<double>(entry.count) / static_cast<double>(total);
            m_err << "  " << std::setw(13) << std::left
                  << opCodeName(static_cast<OpCode>(entry.op)) << std::right << std::setw(12)
                  << entry.count << "  " << std::fixed << std::setprecision(1) << std::setw(5)
                  << share << "%\n";
        }
        m_err.unsetf(std::ios::fixed);
    }
}

int Driver::buildSource(const std::string& path, const std::string& output,
                        const DriverOptions& options) {
    Module module;
    if (!compile(path, module, options)) {
        return 1;
    }

    std::string error;
    if (!saveModule(module, output, error)) {
        m_err << "error: " << error << '\n';
        return 1;
    }
    m_out << "wrote " << output << '\n';
    return 0;
}

int Driver::checkSource(const std::string& path, const DriverOptions& options) {
    Module module;
    if (!compile(path, module, options)) {
        return 1;
    }
    m_out << path << ": ok\n";
    return 0;
}

int Driver::dumpTokenStream(const std::string& path) {
    DriverOptions options;
    options.dumpTokens = true;
    Module module;
    return compile(path, module, options) ? 0 : 1;
}

int Driver::dumpSyntaxTree(const std::string& path) {
    DriverOptions options;
    options.dumpAst = true;
    Module module;
    return compile(path, module, options) ? 0 : 1;
}

int Driver::dumpBytecode(const std::string& path, const DriverOptions& given) {
    // The level matters here: `ir -O0` is how the optimizer's input is inspected
    // and `ir -O2` is its output. This used to build its own default options and
    // so ignored `-O` entirely.
    DriverOptions options = given;
    options.dumpIr = true;
    Module module;
    return compile(path, module, options) ? 0 : 1;
}

int Driver::runBytecode(const std::string& path, const DriverOptions& options) {
    m_stats = options.stats;
    m_statsDetail = options.statsDetail;
    Module module;
    std::string error;
    if (!loadModule(module, path, error)) {
        m_err << "error: " << error << '\n';
        return 1;
    }
    return runLoaded(module, path, options);
}

int Driver::runBytecodeFromMemory(const void* data, std::size_t size, const std::string& origin,
                                  const DriverOptions& options) {
    m_stats = options.stats;
    m_statsDetail = options.statsDetail;
    Module module;
    std::string error;
    if (!loadModuleFromMemory(module, data, size, origin, error)) {
        m_err << "error: " << error << '\n';
        return 1;
    }
    return runLoaded(module, origin, options);
}

int Driver::runLoaded(Module& module, const std::string& origin, const DriverOptions& options) {
    std::string error;
    VM vm(m_out, m_err);
    vm.setProgramArgs(m_programArgs);
    vm.setProfile(options.stats);
    if (options.jit && options.adb) {
        m_err << "note: --jit is off while --adb is attached (compiled code has no "
                  "breakpoints)\n";
    }
    vm.setJitMode(options.jit && !options.adb ? JitMode::On : JitMode::Off);
    vm.setLibrarySearchPath(librarySearchDirs(origin));
    if (options.adb) {
        if (!attachDebugger(vm, options, error)) {
            m_err << "error: " << error << '\n';
            return 1;
        }
    }
    const bool ok = vm.run(module, error);
    if (m_stats) {
        reportStats(vm, module);
    }
    if (!ok) {
        m_err << "runtime error: " << error << '\n';
        return 1;
    }
    return 0;
}

int Driver::disassembleBytecode(const std::string& path) {
    Module module;
    std::string error;
    if (!loadModule(module, path, error)) {
        m_err << "error: " << error << '\n';
        return 1;
    }
    dumpModule(module, m_out);
    return 0;
}

} // namespace acvm
