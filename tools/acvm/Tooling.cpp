//===--- Tooling.cpp - commands that inspect a program rather than run it -===//
#include "Tooling.h"

#include "acvm/Driver/Driver.h"
#include "acvm/IR/Bytecode.h"
#include "acvm/VM/VM.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace acvm::tooling {
namespace {

/// Reads a whole file. Returns false when it cannot, which is the only failure a caller here
/// has to distinguish.
bool readFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

/// Runs `command` and captures stdout. Returns the exit code, or -1 if it could not start.
int capture(const std::string& command, std::string& output) {
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "rb");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return -1;
    }
    char chunk[4096];
    while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr) {
        output += chunk;
    }
#ifdef _WIN32
    const int status = _pclose(pipe);
#else
    const int status = pclose(pipe);
#endif
    return status;
}

/// Quotes a path for the shell, only when it has to.
///
/// `_popen` hands the whole command to `cmd /c`, which wraps it in another pair of quotes. A
/// command that already begins with one therefore arrives with two, cmd strips the wrong pair,
/// and the error names nothing useful. A path without spaces needs no quotes at all.
std::string quoted(const std::string& path) {
    if (path.find(' ') == std::string::npos && path.find('\t') == std::string::npos) {
        return path;
    }
    return "\"" + path + "\"";
}

std::string trimmed(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// Strips a trailing `\r`, which is what every line of a `_popen` read carries on Windows.
std::string withoutCarriageReturn(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

} // namespace

//===--- size -------------------------------------------------------------===//

int size(const std::string& path, std::ostream& out, std::ostream& err) {
    Driver driver(out, err);
    Module module;
    if (!driver.compile(path, module, DriverOptions{})) {
        return 1;
    }

    struct Row {
        std::string name;
        std::size_t instructions = 0;
        std::size_t bytes = 0;
        std::size_t constants = 0;
        std::size_t frame = 0;
    };
    std::vector<Row> rows;
    std::size_t totalBytes = 0;
    std::size_t totalInstructions = 0;
    for (const Function& function : module.functions) {
        Row row;
        row.name = function.name.empty() ? "<anonymous>" : function.name;
        row.instructions = function.code.size();
        row.bytes = function.code.size() * sizeof(Instruction);
        row.constants = function.constants.size();
        row.frame = function.frameSize;
        totalBytes += row.bytes;
        totalInstructions += row.instructions;
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.bytes > b.bytes; });

    // The widest name decides the column, capped so one long name cannot push the numbers off
    // the screen.
    std::size_t nameWidth = 8;
    for (const Row& row : rows) {
        nameWidth = std::max(nameWidth, std::min<std::size_t>(row.name.size(), 40));
    }

    out << "module: " << module.functions.size() << " function(s), " << module.globals.size()
        << " global(s), " << module.strings.size() << " string(s), " << module.memTypes.size()
        << " memory type(s)\n";
    if (!rows.empty()) {
        out << '\n';
        out << "bytes   share  instr   consts  frame   function\n";
        for (const Row& row : rows) {
            const double share = totalBytes == 0 ? 0.0
                                                 : 100.0 * static_cast<double>(row.bytes) /
                                                       static_cast<double>(totalBytes);
            char line[256];
            std::snprintf(line, sizeof(line), "%6zu  %5.1f%%  %6zu  %6zu  %6zu  ",
                          row.bytes, share, row.instructions, row.constants, row.frame);
            out << line;
            out << row.name.substr(0, nameWidth);
            if (row.name.size() > nameWidth) {
                out << "...";
            }
            out << '\n';
        }
    }
    out << '\n';
    out << "total: " << totalBytes << " bytes of bytecode, " << totalInstructions
        << " instructions (" << sizeof(Instruction) << " bytes each)\n";

    // Sorted by size, so the line above the total is the one to look at first. Saying so
    // saves the reader from guessing whether the order is alphabetical.
    if (rows.size() > 1) {
        const double share = 100.0 * static_cast<double>(rows.front().bytes) /
                             static_cast<double>(totalBytes == 0 ? 1 : totalBytes);
        char line[128];
        std::snprintf(line, sizeof(line), "%.1f%%", share);
        out << "largest: " << rows.front().name << " (" << rows.front().bytes << " bytes, " << line
            << ")\n";
    }
    return 0;
}

//===--- doc --------------------------------------------------------------===//

namespace {

/// A declaration found in the text, with the comment block above it.
struct DocEntry {
    std::string signature;
    std::vector<std::string> comment;
};

/// One line of comment, with the marker and one space removed, or empty when the line is not
/// a comment.
///
/// Both `///` and `//` are accepted. This tree writes documentation as `//  Text` -- the
/// double-slash style predates any convention for `///`, and `stdlib/strbyte.ax` is the shape
/// every standard module follows -- so a generator that only understood `///` found nothing in
/// the files most worth documenting. I tried that version first and it reported "no documented
/// declarations" for the whole standard library.
bool commentText(const std::string& line, std::string& text) {
    std::string bare = trimmed(line);
    if (startsWith(bare, "///")) {
        bare = bare.substr(3);
    } else if (startsWith(bare, "//")) {
        bare = bare.substr(2);
    } else {
        return false;
    }
    if (!bare.empty() && bare.front() == ' ') {
        bare.erase(bare.begin());
    }
    text = bare;
    return true;
}

bool isDeclaration(const std::string& line) {
    const std::string text = trimmed(line);
    for (const char* keyword : {"function ", "struct ", "enum ", "process "}) {
        if (startsWith(text, keyword)) {
            // A declaration ends in `{` on the same line, or opens a body on the next. Either
            // way the signature is what precedes the opening brace.
            return true;
        }
    }
    return false;
}

/// A signature reduced to one line, up to the opening brace.
std::string signatureOf(const std::string& line) {
    std::string text = trimmed(line);
    const std::size_t brace = text.find('{');
    if (brace != std::string::npos) {
        text = trimmed(text.substr(0, brace));
    }
    if (!text.empty() && text.back() == ':') {
        text.pop_back();   // `function f():void {` keeps the `:void`; a trailing colon does not
    }
    return text;
}

} // namespace

int doc(const std::vector<std::string>& paths, const std::string& output, std::ostream& out,
        std::ostream& err) {
    std::vector<fs::path> files;
    for (const std::string& path : paths) {
        std::error_code error;
        if (fs::is_directory(path, error)) {
            for (const fs::directory_entry& entry : fs::directory_iterator(path, error)) {
                if (entry.is_regular_file(error) && entry.path().extension() == ".ax") {
                    files.push_back(entry.path());
                }
            }
        } else if (fs::exists(path, error)) {
            files.push_back(fs::path(path));
        } else {
            err << "acvm: no such file or directory: " << path << '\n';
            return 1;
        }
    }
    std::sort(files.begin(), files.end());

    std::ostringstream markdown;
    for (const fs::path& file : files) {
        std::string text;
        if (!readFile(file, text)) {
            err << "acvm: cannot read " << file.generic_string() << '\n';
            return 1;
        }
        std::vector<DocEntry> entries;
        std::vector<std::string> pending;
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            std::string comment;
            if (commentText(line, comment)) {
                pending.push_back(comment);
                continue;
            }
            if (isDeclaration(line)) {
                if (!pending.empty()) {
                    entries.push_back(DocEntry{signatureOf(line), pending});
                    pending.clear();
                }
                continue;
            }
            // Anything else ends a comment block, blank lines included. Attachment is the whole
            // rule: a comment separated from its declaration by a blank line is prose about the
            // file (every module here opens with one), and a comment separated by code is
            // commentary on that code. Only adjacency makes it documentation for what follows.
            pending.clear();
        }

        markdown << "## " << file.filename().generic_string() << "\n\n";
        if (entries.empty()) {
            markdown << "_No documented declarations._\n\n";
            continue;
        }
        for (const DocEntry& entry : entries) {
            markdown << "### `" << entry.signature << "`\n\n";
            for (const std::string& comment : entry.comment) {
                if (comment.empty()) {
                    markdown << "\n";
                } else {
                    markdown << comment << '\n';
                }
            }
            markdown << '\n';
        }
    }

    if (output.empty()) {
        out << markdown.str();
        return 0;
    }
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    file << markdown.str();
    if (!file) {
        err << "acvm: cannot write " << output << '\n';
        return 1;
    }
    out << "wrote " << output << " (" << files.size() << " file(s))\n";
    return 0;
}

//===--- test -------------------------------------------------------------===//

int test(const std::string& directory, bool verbose, std::ostream& out, std::ostream& err) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) {
        err << "acvm: not a directory: " << directory << '\n';
        return 1;
    }

    // The same discovery `ctest` uses in this tree, so a file that runs there runs here.
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".ax") {
            continue;
        }
        const std::string name = entry.path().filename().generic_string();
        if (endsWith(name, "_check.ax") || startsWith(name, "test_")) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    if (files.empty()) {
        err << "acvm: no *_check.ax or test_*.ax found in " << directory << '\n';
        return 1;
    }

    const std::string self = vmExecutablePath();
    if (self.empty()) {
        err << "acvm: cannot locate my own executable to run the tests\n";
        return 1;
    }

    // Each test runs in a scratch directory, not in the caller's.
    //
    // A check that writes a file writes it relative to the current directory: `io_check.ax`
    // creates `io_check_scratch.txt` and removes it again, and when it fails in between, the
    // file stays. Running the suite from a project root therefore left three stray files in
    // this repository -- the first version of this command did exactly that, and they are why
    // this exists. `ctest` already gives `io_check` a working directory of its own for the
    // same reason.
    //
    // One directory for the whole run rather than one per file: a suite may want a test to
    // observe what an earlier one wrote, and a shared scratch area is the smaller surprise of
    // the two.
    std::error_code scratchError;
    const fs::path scratch =
        fs::temp_directory_path(scratchError) / "acvm-test-scratch";
    if (!scratchError) {
        fs::remove_all(scratch, scratchError);
        fs::create_directories(scratch, scratchError);
    }
    const std::string scratchPath = scratchError ? std::string() : scratch.string();
    if (!scratchPath.empty()) {
        out << "running in " << scratchPath << "\n\n";
    }

    int failures = 0;
    for (const fs::path& file : files) {
        std::string output;
        // `2>&1` because a failing check reports through the VM's error stream: a `trap()` or
        // a division by zero never reaches stdout, so capturing only stdout printed the file
        // name of every failure and no reason for any of them. The first version did exactly
        // that, and a `FAIL` line with nothing under it is worse than no line -- it says a test
        // broke and withholds the one thing the reader needs.
        //
        // The absolute path to the test file is unchanged by the `cd`, so the test still finds
        // its own source, its imports and its neighbour modules.
        std::string command;
        if (!scratchPath.empty()) {
#ifdef _WIN32
            command = "cd /d " + quoted(scratchPath) + " && ";
#else
            command = "cd " + quoted(scratchPath) + " && ";
#endif
        }
        command += quoted(self) + " run " + quoted(fs::absolute(file).string()) + " 2>&1";
        const int status = capture(command, output);
        const bool ok = status == 0;
        if (!ok) {
            ++failures;
        }
        out << (ok ? "  ok    " : "  FAIL  ") << file.filename().generic_string() << '\n';
        if (!ok || verbose) {
            std::istringstream lines(output);
            std::string line;
            while (std::getline(lines, line)) {
                out << "        " << withoutCarriageReturn(line) << '\n';
            }
        }
    }

    out << '\n' << (files.size() - static_cast<std::size_t>(failures)) << '/' << files.size()
        << " passed\n";

    // Left in place on failure, with its path already printed: a check that fails while writing
    // a file leaves the file as evidence, and deleting it would destroy the only thing that
    // explains the failure. Removed only when everything passed and there is nothing to look at.
    if (failures == 0 && !scratchPath.empty()) {
        fs::remove_all(scratch, scratchError);
    }
    return failures == 0 ? 0 : 1;
}

//===--- init -------------------------------------------------------------===//

int init(const std::string& directory, std::ostream& out, std::ostream& err) {
    std::error_code error;
    const fs::path root(directory);
    if (fs::exists(root, error) && !fs::is_directory(root, error)) {
        err << "acvm: " << directory << " exists and is not a directory\n";
        return 1;
    }
    fs::create_directories(root / "tests", error);
    if (error) {
        err << "acvm: cannot create " << directory << ": " << error.message() << '\n';
        return 1;
    }

    // Neither file is created if it is already there: `init` in an existing project should not
    // overwrite the program someone is working on.
    const fs::path mainFile = root / "main.ax";
    if (!fs::exists(mainFile, error)) {
        std::ofstream file(mainFile, std::ios::binary);
        file << "function main():void {\n"
             << "    print(\"hello\\n\");\n"
             << "    eof: pass;\n"
             << "}\n";
    }

    const fs::path testFile = root / "tests" / "main_check.ax";
    if (!fs::exists(testFile, error)) {
        std::ofstream file(testFile, std::ios::binary);
        file << "//  main_check.ax -- run: acvm test tests\n"
             << "//\n"
             << "//  A check reports failure by trapping, so the runner only has to look at the\n"
             << "//  exit code. `MISMATCH` in the output is what the tree's own checks print.\n"
             << "\n"
             << "function main():void {\n"
             << "    print(\"done\\n\");\n"
             << "    eof: pass;\n"
             << "}\n";
    }

    out << "created " << directory << "/\n";
    out << "  main.ax\n";
    out << "  tests/main_check.ax\n";
    out << "\nnext:\n";
    out << "  acvm run main.ax\n";
    out << "  acvm test tests\n";
    return 0;
}

} // namespace acvm::tooling
