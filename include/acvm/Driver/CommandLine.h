//===--- CommandLine.h - Helpers for the acvm command line --------------===//
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace acvm {

/// Result of resolving positional arguments into one input path.
struct PositionalPath {
    /// The matched path, empty when no combination named an existing file.
    std::string path;
    /// How many leading arguments the path consumed (0 when nothing matched).
    std::size_t consumed = 0;
};

/// Rejoins positional arguments that a shell split on whitespace.
///
/// Every acvm command takes exactly one input path, so `acvm run my dir/p.ax`
/// is unambiguously `acvm run "my dir/p.ax"`. Longer combinations are tried
/// first, which lets a real trailing argument be reported precisely:
///
///   [tmp test dir/my prog.ax, extra]  ->  consumes 1, leaves `extra`
///   [no such file.ax]                 ->  no match
PositionalPath resolvePositionalPath(const std::vector<std::string>& parts);

/// Joins every argument with a single space; used for error messages.
std::string joinedArguments(const std::vector<std::string>& parts);

/// Renders the hint shown when the arguments could not be resolved, e.g.
/// `if the path contains spaces, quote it: acvm run "my dir/prog.ax"`.
std::string quotingHint(const std::string& command, const std::vector<std::string>& parts);

} // namespace acvm
