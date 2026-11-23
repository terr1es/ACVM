//===--- CommandLine.cpp ------------------------------------------------===//
#include "acvm/Driver/CommandLine.h"

#include <filesystem>

namespace acvm {
namespace {

/// Joins `parts[0 .. count)`. Concentrations of whitespace beyond one space
/// cannot be recovered from argv: the shell already collapsed them into a
/// single argument boundary.
std::string concatenate(const std::vector<std::string>& parts, std::size_t count) {
    std::string result;
    for (std::size_t i = 0; i < count && i < parts.size(); ++i) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += parts[i];
    }
    return result;
}

bool fileExists(const std::string& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(std::filesystem::path(path), error);
    return exists && !error;
}

} // namespace

PositionalPath resolvePositionalPath(const std::vector<std::string>& parts) {
    PositionalPath result;
    if (parts.empty()) {
        return result;
    }
    if (parts.size() == 1) {
        result.path = parts[0];
        result.consumed = 1;
        return result;
    }

    // Longest combination first, so `a b c.ax` wins over `a b`. A single
    // argument is also a candidate: it may already be quoted and followed by a
    // genuine extra argument.
    for (std::size_t count = parts.size(); count >= 1; --count) {
        const std::string candidate = concatenate(parts, count);
        if (fileExists(candidate)) {
            result.path = candidate;
            result.consumed = count;
            return result;
        }
    }
    return result;
}

std::string joinedArguments(const std::vector<std::string>& parts) {
    return concatenate(parts, parts.size());
}

std::string quotingHint(const std::string& command, const std::vector<std::string>& parts) {
    return "if the path contains spaces, quote it: acvm " + command + " \"" +
           concatenate(parts, parts.size()) + "\"";
}

} // namespace acvm
