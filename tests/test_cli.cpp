//===--- test_cli.cpp - Command line argument resolution ---------------===//
#include "TestHarness.hpp"

#include "acvm/Driver/CommandLine.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// A real file whose name contains spaces, removed on destruction.
class SpacedFile {
public:
    explicit SpacedFile(const std::string& name) : m_path(fs::temp_directory_path() / name) {
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        out << "function main():void { eof: pass; }\n";
    }
    ~SpacedFile() {
        std::error_code ignored;
        fs::remove(m_path, ignored);
    }

    SpacedFile(const SpacedFile&) = delete;
    SpacedFile& operator=(const SpacedFile&) = delete;

    [[nodiscard]] std::string path() const { return m_path.string(); }

private:
    fs::path m_path;
};

/// Mimics a shell: splits on every space, exactly what argv receives when the
/// user forgets to quote a path.
std::vector<std::string> shellSplit(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : text) {
        if (c == ' ') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

} // namespace

TEST(a_single_argument_is_used_as_is) {
    const auto resolved = acvm::resolvePositionalPath({"examples/tour.ax"});
    CHECK_EQ(resolved.path, std::string("examples/tour.ax"));
    CHECK_EQ(resolved.consumed, std::size_t(1));
}

TEST(split_arguments_are_rejoined_when_the_file_exists) {
    SpacedFile file("acvm cli split test.ax");
    const std::vector<std::string> parts = shellSplit(file.path());
    CHECK(parts.size() > 1); // the temporary directory may itself add no spaces

    const auto resolved = acvm::resolvePositionalPath(parts);
    CHECK_EQ(resolved.path, file.path());
    CHECK_EQ(resolved.consumed, parts.size());
}

TEST(a_trailing_argument_is_not_swallowed) {
    SpacedFile file("acvm cli trailing test.ax");
    std::vector<std::string> parts = shellSplit(file.path());
    parts.push_back("extra");

    const auto resolved = acvm::resolvePositionalPath(parts);
    CHECK_EQ(resolved.path, file.path());
    CHECK_EQ(resolved.consumed, parts.size() - 1);
}

TEST(a_quoted_first_argument_still_reports_the_extra_one) {
    SpacedFile file("acvm cli quoted test.ax");
    const std::vector<std::string> parts = {file.path(), "extra"};

    const auto resolved = acvm::resolvePositionalPath(parts);
    CHECK_EQ(resolved.path, file.path());
    CHECK_EQ(resolved.consumed, std::size_t(1));
}

TEST(unresolvable_arguments_report_nothing) {
    const std::vector<std::string> parts = {"acvm", "definitely", "not", "here.ax"};
    const auto resolved = acvm::resolvePositionalPath(parts);
    CHECK(resolved.path.empty());
    CHECK_EQ(resolved.consumed, std::size_t(0));
}

TEST(empty_input_is_handled) {
    const auto resolved = acvm::resolvePositionalPath({});
    CHECK(resolved.path.empty());
    CHECK_EQ(resolved.consumed, std::size_t(0));
}

TEST(the_hint_quotes_the_whole_path) {
    const std::string hint = acvm::quotingHint("run", {"my", "dir", "prog.ax"});
    CHECK(hint.find("acvm run \"my dir prog.ax\"") != std::string::npos);
}

TEST(joined_arguments_use_single_spaces) {
    CHECK_EQ(acvm::joinedArguments({"a", "b", "c"}), std::string("a b c"));
}

TEST_MAIN()
