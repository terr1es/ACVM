//===--- SourceLocation.h - Positions inside Axiom source files ---------===//
#pragma once

#include <cstdint>

namespace acvm {

using FileID = std::uint32_t;
inline constexpr FileID kInvalidFileID = static_cast<FileID>(-1);

/// A single point in the program. `offset` is a byte offset into the file.
struct SourceLocation {
    FileID file = kInvalidFileID;
    std::uint32_t offset = 0;

    [[nodiscard]] bool isValid() const { return file != kInvalidFileID; }
    friend bool operator==(const SourceLocation&, const SourceLocation&) = default;
};

/// Half open byte range [begin, end) inside a single file.
struct SourceRange {
    SourceLocation begin{};
    SourceLocation end{};

    [[nodiscard]] bool isValid() const { return begin.isValid(); }
    static SourceRange invalid() { return SourceRange{}; }
};

} // namespace acvm
