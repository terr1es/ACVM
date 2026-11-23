//===--- Diagnostic.cpp -------------------------------------------------===//
#include "acvm/Common/Diagnostic.h"

#include "acvm/Common/SourceManager.h"

#include <algorithm>
#include <ostream>
#include <string>
#include <utility>

namespace acvm {
namespace {

/// Prefixes `prefix` and indents every continuation line of `message` so that
/// multi-line diagnostics stay visually attached to their header instead of
/// looking like stray source code.
std::string indentContinuation(const std::string& prefix, const std::string& message) {
    std::string result = prefix;
    for (const char c : message) {
        result.push_back(c);
        if (c == '\n') {
            result.append(prefix.size(), ' ');
        }
    }
    return result;
}

} // namespace

const char* diagLevelName(DiagLevel level) {    switch (level) {
    case DiagLevel::Note:
        return "note";
    case DiagLevel::Warning:
        return "warning";
    case DiagLevel::Error:
        return "error";
    }
    return "error";
}

DiagnosticEngine::DiagnosticEngine(SourceManager& sources, std::ostream& out)
    : m_sources(sources), m_out(out) {}

std::string_view DiagnosticEngine::path(FileID file) const { return m_sources.path(file); }

void DiagnosticEngine::note(SourceLocation loc, std::string message) {
    render(DiagLevel::Note, SourceRange{loc, loc}, message);
}

void DiagnosticEngine::warning(SourceLocation loc, std::string message) {
    render(DiagLevel::Warning, SourceRange{loc, loc}, message);
}

void DiagnosticEngine::warning(SourceRange range, std::string message) {
    render(DiagLevel::Warning, range, message);
}

void DiagnosticEngine::error(SourceLocation loc, std::string message) {
    render(DiagLevel::Error, SourceRange{loc, loc}, message);
}

void DiagnosticEngine::error(SourceRange range, std::string message) {
    render(DiagLevel::Error, range, message);
}

void DiagnosticEngine::help(SourceLocation loc, std::string message) {
    render(DiagLevel::Note, SourceRange{loc, loc}, "help: " + message);
}

void DiagnosticEngine::reset() {
    m_errors = 0;
    m_warnings = 0;
}

void DiagnosticEngine::render(DiagLevel level, SourceRange range, const std::string& message) {
    // A front end that has lost its place can produce one diagnostic per token.
    // Emitting them all buries the first (useful) one and costs seconds of I/O,
    // so stop after a round number and say so, like other compilers do.
    if (level == DiagLevel::Error && m_errors >= kMaxErrors) {
        if (m_errors == kMaxErrors) {
            ++m_errors;
            m_out << "too many errors; stopping now\n";
        }
        return;
    }

    if (level == DiagLevel::Error) {
        ++m_errors;
    } else if (level == DiagLevel::Warning) {
        ++m_warnings;
    }

    const SourceLocation begin = range.begin;
    if (!begin.isValid()) {
        m_out << indentContinuation(std::string(diagLevelName(level)) + ": ", message) << '\n';
        return;
    }

    const SourceManager::LineColumn lc = m_sources.lineColumn(begin);
    const std::string_view filePath = m_sources.path(begin.file);
    const std::string prefix = (filePath.empty() ? std::string("<input>") : std::string(filePath)) +
                               ':' + std::to_string(lc.line) + ':' + std::to_string(lc.column) +
                               ": " + diagLevelName(level) + ": ";
    m_out << indentContinuation(prefix, message) << '\n';

    const std::string_view line = m_sources.lineText(begin.file, lc.line);
    if (line.empty()) {
        return;
    }

    const std::string number = std::to_string(lc.line);
    const std::string gutter(number.size(), ' ');
    m_out << number << " | " << line << '\n';

    std::uint32_t span = 1;
    if (range.end.isValid() && range.end.offset > begin.offset) {
        span = range.end.offset - begin.offset;
    }
    const std::uint32_t column = lc.column == 0 ? 1 : lc.column;
    const std::uint32_t remaining =
        (column - 1 < line.size()) ? static_cast<std::uint32_t>(line.size()) - (column - 1) : 0;
    span = std::min(std::max<std::uint32_t>(span, 1), std::max<std::uint32_t>(remaining, 1));

    m_out << gutter << " | " << std::string(column - 1, ' ') << std::string(span, '^') << '\n';
}

} // namespace acvm
