//===--- TextDistance.h - How far apart two names are ---------------------===//
#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

namespace acvm {

/// Levenshtein distance: the number of single-character insertions, deletions
/// and substitutions that turn `lhs` into `rhs`.
///
/// Two callers want this, for the same reason and at different layers:
///
///   * Sema, when a name is not found and one in scope is close (`did you mean
///     'sortInt64s'?`)
///   * The module loader, when an `import` fails and an official extra module is
///     close (`did you mean 'json'?`)
///
/// It lived in Sema's anonymous namespace until the second caller appeared. A
/// copy in the loader would have been the easier edit and the wrong one: two
/// spellings of "close enough" is how the two diagnostics end up disagreeing.
///
/// O(n*m) time and O(m) space. Both inputs are identifiers, so the constant is
/// tiny; this must not be reached with arbitrary text.
inline int editDistance(std::string_view lhs, std::string_view rhs) {
    const std::size_t n = rhs.size();
    std::vector<int> previous(n + 1);
    std::vector<int> current(n + 1);
    for (std::size_t j = 0; j <= n; ++j) {
        previous[j] = static_cast<int>(j);
    }
    for (std::size_t i = 1; i <= lhs.size(); ++i) {
        current[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= n; ++j) {
            const int cost = lhs[i - 1] == rhs[j - 1] ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
        }
        previous.swap(current);
    }
    return previous[n];
}

} // namespace acvm
