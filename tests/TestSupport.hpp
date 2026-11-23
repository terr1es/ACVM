//===--- TestSupport.hpp - In memory compilation helpers ----------------===//
#pragma once

#include "acvm/Common/Diagnostic.h"
#include "acvm/Common/SourceManager.h"
#include "acvm/IR/IRGen.h"
#include "acvm/Lex/Lexer.h"
#include "acvm/Parse/Parser.h"
#include "acvm/Sema/Sema.h"
#include "acvm/VM/VM.h"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace testsupport {

struct LexResult {
    std::unique_ptr<acvm::SourceManager> sources;
    std::vector<acvm::Token> tokens;
    std::string diagnostics;
};

inline LexResult lexSource(const std::string& source) {
    LexResult result;
    result.sources = std::make_unique<acvm::SourceManager>();
    std::ostringstream diagnostics;
    {
        acvm::DiagnosticEngine engine(*result.sources, diagnostics);
        const acvm::FileID file = result.sources->addFile("test.ax", source);
        acvm::Lexer lexer(*result.sources, file, engine);
        result.tokens = lexer.tokenize();
    }
    result.diagnostics = diagnostics.str();
    return result;
}

struct SyntaxResult {
    std::unique_ptr<acvm::SourceManager> sources;
    std::unique_ptr<acvm::Unit> unit;
    std::string diagnostics;
};

inline SyntaxResult parseSource(const std::string& source) {
    SyntaxResult result;
    result.sources = std::make_unique<acvm::SourceManager>();
    std::ostringstream diagnostics;
    {
        acvm::DiagnosticEngine engine(*result.sources, diagnostics);
        const acvm::FileID file = result.sources->addFile("test.ax", source);
        acvm::Lexer lexer(*result.sources, file, engine);
        std::vector<acvm::Token> tokens = lexer.tokenize();
        acvm::Parser parser(std::move(tokens), engine);
        result.unit = parser.parseUnit();
    }
    result.diagnostics = diagnostics.str();
    return result;
}

struct CompileResult {
    bool ok = false;
    std::string diagnostics;
    acvm::Module module;
};

/// Runs lexer, parser, sema and IRGen over an in memory source string.
inline CompileResult compileSource(const std::string& source) {
    CompileResult result;

    acvm::SourceManager sources;
    std::ostringstream diagnostics;
    acvm::DiagnosticEngine engine(sources, diagnostics);

    const acvm::FileID file = sources.addFile("test.ax", source);
    acvm::Lexer lexer(sources, file, engine);
    std::vector<acvm::Token> tokens = lexer.tokenize();

    acvm::Parser parser(std::move(tokens), engine);
    std::unique_ptr<acvm::Unit> unit = parser.parseUnit();

    if (!engine.hasErrors()) {
        acvm::TypeContext types;
        acvm::Sema sema(engine, types);
        if (sema.check(*unit)) {
            acvm::IRGen generator(engine, sema);
            result.ok = generator.generate(*unit, result.module);
        }
    }

    result.diagnostics = diagnostics.str();
    return result;
}

inline std::string checkSource(const std::string& source) {
    return compileSource(source).diagnostics;
}

struct RunResult {
    bool ok = false;
    std::string diagnostics;
    std::string output;
};

/// Compiles and executes, capturing stdout. A non-zero `memoryLimit` lowers the
/// VM's resident-memory cap so tests can reach the out-of-memory trap quickly.
inline RunResult runSource(const std::string& source, std::uint64_t memoryLimit = 0) {
    RunResult result;
    CompileResult compiled = compileSource(source);
    result.diagnostics = compiled.diagnostics;
    if (!compiled.ok) {
        return result;
    }

    std::ostringstream out;
    std::ostringstream err;
    acvm::VM vm(out, err);
    if (memoryLimit != 0) {
        vm.setMemoryLimit(memoryLimit);
    }
    std::string error;
    result.ok = vm.run(compiled.module, error);
    result.output = out.str();
    if (!result.ok) {
        result.diagnostics += "runtime error: " + error + "\n";
    }
    return result;
}

} // namespace testsupport
