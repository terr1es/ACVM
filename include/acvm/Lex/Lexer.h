//===--- Lexer.h - Turns Axiom source text into tokens ------------------===//
#pragma once

#include "acvm/Common/SourceLocation.h"
#include "acvm/Lex/Token.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace acvm {

class DiagnosticEngine;
class SourceManager;

/// Hand written lexer. The grammar dependent parts are limited to the keyword
/// table, `lexIdentifier`, `lexNumber`, `lexString` and `lexOperator`.
class Lexer {
public:
    Lexer(SourceManager& sources, FileID file, DiagnosticEngine& diagnostics);

    Token next();
    std::vector<Token> tokenize();

private:
    [[nodiscard]] bool atEnd() const;
    [[nodiscard]] char peek(std::size_t ahead = 0) const;
    char advance();
    bool match(char expected);
    void skipTrivia();

    Token makeToken(TokenKind kind, std::uint32_t start) const;
    Token lexIdentifier(std::uint32_t start);
    Token lexNumber(std::uint32_t start);
    Token lexChar(std::uint32_t start);
    Token lexString(std::uint32_t start, bool raw, bool multiline);
    Token lexOperator(std::uint32_t start);

    bool boundaryAfterSuffix(std::uint32_t start);

    [[maybe_unused]] SourceManager& m_sources;
    [[maybe_unused]] DiagnosticEngine& m_diagnostics;
    FileID m_file;
    std::string_view m_source;
    std::uint32_t m_pos = 0;
};

} // namespace acvm
