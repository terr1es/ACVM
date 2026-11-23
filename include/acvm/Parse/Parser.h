//===--- Parser.h - Recursive descent parser for Axiom ------------------===//
#pragma once

#include "acvm/AST/AST.h"
#include "acvm/Lex/Token.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace acvm {

class DiagnosticEngine;

/// Hand written recursive descent parser with precedence climbing for
/// expressions. Always returns a tree; syntax errors are reported through the
/// diagnostic engine and the driver stops before Sema when any occurred.
class Parser {
public:
    Parser(std::vector<Token> tokens, DiagnosticEngine& diagnostics);

    std::unique_ptr<Unit> parseUnit();

private:
    // -- token stream ------------------------------------------------------
    [[nodiscard]] const Token& peek(std::size_t ahead = 0) const;
    [[nodiscard]] const Token& previous() const;
    [[nodiscard]] bool check(TokenKind kind) const;
    [[nodiscard]] bool atEnd() const;
    const Token& advance();
    bool match(TokenKind kind);
    const Token& expect(TokenKind kind, std::string_view context);
    /// Consumes the `>` that closes a type argument list. `Pair<Pair<int>>` ends
    /// in one `>>` token, which has to count as two closes, so that token is
    /// rewritten in place into two one-character tokens.
    const Token& expectTypeArgClose();
    void errorAt(const Token& token, std::string message);

    // -- declarations ------------------------------------------------------
    StmtPtr parseTopLevel();
    /// `inStructBody` allows a leading unnamed parameter -- the method receiver.
    StmtPtr parseFunction(bool inStructBody = false);
    StmtPtr parseImportDecl();
    /// `struct` and `union` differ only in how the members are laid out, which
    /// Sema decides from the statement kind, so they share one parser.
    StmtPtr parseStructLike(StmtKind kind, const char* keyword);
    StmtPtr parseStruct();
    StmtPtr parseEnum();
    /// Parses `<T, U>` after a declaration's name. `what` names the construct in
    /// the diagnostics ("struct" / "function").
    void parseTypeParamList(std::vector<std::string>& out, const char* what);
    /// Parses `<int, Pair<T>>` in a type. `>>` lexes as one token, so the two
    /// closes of a nested list are split apart here.
    void parseTypeArgumentList(TypeNode& node);
    StmtPtr parseGlobal(bool isConst, bool isComptime);
    StmtPtr parseVarDecl(bool global, bool isConst, bool isComptime, bool expectSemi = true);
    void parseVarDeclRest(Stmt& decl, bool expectSemi);
    /// `allowUnnamed` lets the first parameter of a method be written as just a
    /// type (`self` or `ptr self`), which is what marks it as the receiver.
    Param parseParam(bool allowUnnamed = false);
    void parseImportParams(Stmt& decl);
    void parseImportBody(Stmt& decl);
    /// Parses `(Name, Name)` or `("path.ax", ...)` into `decl.modules`.
    void parseModuleList(Stmt& decl);

    // -- types -------------------------------------------------------------
    TypeNodePtr parseType();
    TypeNodePtr parseTypeAtom();
    TypeNodePtr makeScalarType(const Token& token);

    // -- statements --------------------------------------------------------
    StmtPtr parseStmt();
    StmtPtr parseIf();
    StmtPtr parseWhile();
    StmtPtr parseFor();
    StmtPtr parseLoop();
    StmtPtr parseSwitch();
    StmtPtr parseReturn();
    StmtPtr parseVirtual();
    StmtPtr parseBodyBlock(std::vector<StmtPtr>& body, std::vector<StmtPtr>& eofBody,
                           bool& hasEof, bool& eofExplicitEmpty);

    // -- expressions -------------------------------------------------------
    ExprPtr parseExpression();
    ExprPtr parseAssignment();
    ExprPtr parseTernary();
    ExprPtr parseBinary(int minPrecedence);
    ExprPtr parseCast();
    ExprPtr parseUnary();
    ExprPtr parsePostfix();
    ExprPtr parsePrimary();
    ExprPtr parseInitList();

    [[nodiscard]] bool looksLikeDeclaration() const;
    [[nodiscard]] bool isTupleTypeDeclAhead() const;
    [[nodiscard]] bool isLabelAhead() const;
    [[nodiscard]] bool isTypeStart(const Token& token) const;
    /// True when the tokens ahead read as `Name<...> identifier`, i.e. a
    /// declaration whose type has type arguments. See the definition for why the
    /// rule has to be this narrow.
    [[nodiscard]] bool looksLikeGenericTypeDecl() const;
    /// True when the tokens ahead read as `Name<...>.`, i.e. a static method on an
    /// instantiated generic type.
    [[nodiscard]] bool looksLikeGenericTypeRef() const;
    /// Scans `Name<...>` and reports the index just past the closing `>`. False
    /// when the tokens inside cannot be a type argument list.
    bool scanGenericTypeRef(std::size_t& afterClose) const;

    void synchronize();

    std::vector<Token> m_tokens;
    std::size_t m_index = 0;
    DiagnosticEngine& m_diagnostics;
};

} // namespace acvm
