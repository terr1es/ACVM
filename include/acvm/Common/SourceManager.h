//===--- SourceManager.h - Owns source buffers and maps offsets to lines --===//
#pragma once

#include "acvm/Common/SourceLocation.h"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace acvm {

/// Owns the text of every file the compiler has seen. Backed by a deque so
/// that string_views handed out to tokens stay valid.
class SourceManager {
public:
    struct LineColumn {
        std::uint32_t line = 1;
        std::uint32_t column = 1;
    };

    FileID addFile(std::string path, std::string text);
    FileID loadFile(const std::string& path, std::string& error);

    [[nodiscard]] std::string_view text(FileID id) const;
    [[nodiscard]] std::string_view path(FileID id) const;
    [[nodiscard]] std::string_view lineText(FileID id, std::uint32_t line) const;
    [[nodiscard]] LineColumn lineColumn(SourceLocation loc) const;

private:
    struct File {
        std::string path;
        std::string text;
        std::vector<std::uint32_t> lineStarts;
    };

    static File build(std::string path, std::string text);
    [[nodiscard]] const File* get(FileID id) const;

    std::deque<File> m_files;
};

} // namespace acvm
