//===--- Diagnostic.h - Error reporting with source snippets -------------===//
#pragma once

#include "acvm/Common/SourceLocation.h"

#include <iosfwd>
#include <string>

namespace acvm {

class SourceManager;

enum class DiagLevel { Note, Warning, Error };

const char* diagLevelName(DiagLevel level);

/// Renders clang style diagnostics:
///
///   main.ax:3:11: error: cannot assign to const variable 'X'
///       3 |     X = 2;
///         |     ^
class DiagnosticEngine {
public:
    DiagnosticEngine(SourceManager& sources, std::ostream& out);

    void note(SourceLocation loc, std::string message);
    void warning(SourceLocation loc, std::string message);
    void warning(SourceRange range, std::string message);
    void error(SourceLocation loc, std::string message);
    void error(SourceRange range, std::string message);
    void help(SourceLocation loc, std::string message);

    [[nodiscard]] unsigned errorCount() const { return m_errors; }
    /// Path of `file`, for diagnostics built by callers that only have an id.
    [[nodiscard]] std::string_view path(FileID file) const;
    [[nodiscard]] unsigned warningCount() const { return m_warnings; }
    [[nodiscard]] bool hasErrors() const { return m_errors > 0; }

    void reset();

    /// Errors beyond this many are counted but not printed: a front end that
    /// has lost its place would otherwise emit one diagnostic per token.
    static constexpr unsigned kMaxErrors = 32;

private:
    void render(DiagLevel level, SourceRange range, const std::string& message);

    SourceManager& m_sources;
    std::ostream& m_out;
    unsigned m_errors = 0;
    unsigned m_warnings = 0;
};

} // namespace acvm
