//===--- Tooling.h - commands that inspect a program rather than run it ---===//
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace acvm::tooling {

/// `acvm size <file.ax>` -- where a module's bytes went.
///
/// The question it answers is asked when a module is too big to ship or too slow to load:
/// which function is the big one, and is it code or constants. Reading `function.code.size()`
/// from a dump answers it for one function at a time; this answers it for all of them, sorted,
/// so the answer is the first line.
int size(const std::string& path, std::ostream& out, std::ostream& err);

/// `acvm doc <path...> [--out <file>]` -- Markdown from the comments above a declaration.
///
/// Reads the source, not the syntax tree. A doc generator that depended on the AST would stop
/// working on a file that does not compile, which is exactly when someone wants to read the
/// signatures -- and the comment-to-declaration adjacency it needs is a property of the text.
int doc(const std::vector<std::string>& paths, const std::string& output, std::ostream& out,
        std::ostream& err);

/// `acvm test [dir]` -- run every `*_check.ax` and `test_*.ax` under a directory.
///
/// Each file is run in its own process. In-process would be faster and wrong: a test that
/// traps leaves the VM in whatever state it trapped in, and one process per file means a
/// failure is contained and the rest still run.
int test(const std::string& directory, bool verbose, std::ostream& out, std::ostream& err);

/// `acvm init <dir>` -- a project with one program and one test that fails first.
int init(const std::string& directory, std::ostream& out, std::ostream& err);

} // namespace acvm::tooling
