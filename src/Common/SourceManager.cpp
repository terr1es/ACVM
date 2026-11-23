//===--- SourceManager.cpp ----------------------------------------------===//
#include "acvm/Common/SourceManager.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace acvm {

SourceManager::File SourceManager::build(std::string path, std::string text) {
    File file;
    file.path = std::move(path);
    file.text = std::move(text);

    file.lineStarts.push_back(0);
    for (std::size_t i = 0; i < file.text.size(); ++i) {
        if (file.text[i] == '\n' && i + 1 < file.text.size()) {
            file.lineStarts.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }
    return file;
}

FileID SourceManager::addFile(std::string path, std::string text) {
    m_files.push_back(build(std::move(path), std::move(text)));
    return static_cast<FileID>(m_files.size() - 1);
}

FileID SourceManager::loadFile(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open file '" + path + "'";
        return kInvalidFileID;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        error = "failed while reading '" + path + "'";
        return kInvalidFileID;
    }
    return addFile(path, buffer.str());
}

const SourceManager::File* SourceManager::get(FileID id) const {
    return id < m_files.size() ? &m_files[id] : nullptr;
}

std::string_view SourceManager::text(FileID id) const {
    const File* file = get(id);
    return file ? std::string_view(file->text) : std::string_view{};
}

std::string_view SourceManager::path(FileID id) const {
    const File* file = get(id);
    return file ? std::string_view(file->path) : std::string_view{};
}

std::string_view SourceManager::lineText(FileID id, std::uint32_t line) const {
    const File* file = get(id);
    if (!file || line == 0 || line > file->lineStarts.size()) {
        return {};
    }
    const std::uint32_t start = file->lineStarts[line - 1];
    std::uint32_t end = 0;
    if (line < file->lineStarts.size()) {
        // Every line but the last ends at the '\n' that starts the next one.
        end = file->lineStarts[line] - 1;
    } else {
        // The last line may or may not be terminated; strip the terminator when
        // it is there, otherwise the rendered source line gains an empty line.
        end = static_cast<std::uint32_t>(file->text.size());
        if (end > start && file->text[end - 1] == '\n') {
            --end;
        }
    }
    if (end > start && file->text[end - 1] == '\r') {
        --end;
    }
    if (end < start) {
        end = start;
    }
    return std::string_view(file->text).substr(start, end - start);
}

SourceManager::LineColumn SourceManager::lineColumn(SourceLocation loc) const {
    LineColumn result;
    const File* file = get(loc.file);
    if (!file) {
        return result;
    }
    const std::uint32_t offset =
        std::min<std::uint32_t>(loc.offset, static_cast<std::uint32_t>(file->text.size()));
    const auto it = std::upper_bound(file->lineStarts.begin(), file->lineStarts.end(), offset);
    const auto index = static_cast<std::size_t>(it - file->lineStarts.begin()) - 1;
    result.line = static_cast<std::uint32_t>(index + 1);
    result.column = offset - file->lineStarts[index] + 1;
    return result;
}

} // namespace acvm
