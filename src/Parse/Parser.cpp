//===--- Parser.cpp -----------------------------------------------------===//
#include "acvm/Parse/Parser.h"

#include "acvm/Common/Diagnostic.h"
#include "acvm/Common/Value.h"

#include <cstdlib>
#include <string>
#include <utility>

namespace acvm {
namespace {

std::string describe(const Token& token) {
    if (!token.text.empty()) {
        return std::string(token.text);
    }
    return std::string(tokenKindSpelling(token.kind));
}

/// The characters of a string literal token, without the quotes and with escape
/// sequences decoded. Three sites did this inline; the module directive made a
/// fourth, so it is a function.
std::string decodeStringLiteral(const Token& token) {
    const std::string_view raw = token.text;
    const std::string_view body =
        raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : std::string_view{};
    return decodeEscapes(body);
}

/// `MAJOR.MINOR.PATCH`, each component one or more decimal digits.
///
/// Strict rather than lenient because the point of declaring a version is that
/// tooling compares it. `"1.0"` would compare as neither `1.0.0` nor an error, and
/// `"v1.0.0"` would sort after `"1.0.0"` in every string comparison. Rejecting both
/// now costs one diagnostic; accepting them costs a resolution bug later that shows
/// up as two incompatible versions being considered compatible.
bool isValidModuleVersion(std::string_view version) {
    int components = 0;
    std::size_t digitsInComponent = 0;
    for (const char c : version) {
        if (c >= '0' && c <= '9') {
            ++digitsInComponent;
            continue;
        }
        if (c != '.' || digitsInComponent == 0) {
            return false;
        }
        ++components;
        digitsInComponent = 0;
    }
    if (digitsInComponent == 0) {
        return false;
    }
    return components == 2;   // three components: two separators seen
}

SourceRange join(const SourceRange& begin, const SourceRange& end) {
    SourceRange range;
    range.begin = begin.begin;
    range.end = end.end;
    return range;
}

int precedenceOf(TokenKind kind) {
    // Larger binds tighter. The spec table has level 1 (postfix) at the top;
    // binary operators occupy levels 4..14, so the value is 15 - level.
    switch (kind) {
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
        return 11;
    case TokenKind::Plus:
    case TokenKind::Minus:
        return 10;
    case TokenKind::Shl:
    case TokenKind::Shr:
        return 9;
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual:
        return 8;
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual:
        return 7;
    case TokenKind::Amp:
        return 6;
    case TokenKind::Caret:
        return 5;
    case TokenKind::Pipe:
        return 4;
    case TokenKind::AmpAmp:
        return 3;
    case TokenKind::PipePipe:
        return 2;
    case TokenKind::DotDot:
        return 1;
    default:
        return -1;
    }
}

BinOp binOpOf(TokenKind kind) {
    switch (kind) {
    case TokenKind::Star: return BinOp::Mul;
    case TokenKind::Slash: return BinOp::Div;
    case TokenKind::Percent: return BinOp::Mod;
    case TokenKind::Plus: return BinOp::Add;
    case TokenKind::Minus: return BinOp::Sub;
    case TokenKind::Shl: return BinOp::Shl;
    case TokenKind::Shr: return BinOp::Shr;
    case TokenKind::Less: return BinOp::Lt;
    case TokenKind::LessEqual: return BinOp::Le;
    case TokenKind::Greater: return BinOp::Gt;
    case TokenKind::GreaterEqual: return BinOp::Ge;
    case TokenKind::EqualEqual: return BinOp::Eq;
    case TokenKind::BangEqual: return BinOp::Ne;
    case TokenKind::Amp: return BinOp::BitAnd;
    case TokenKind::Caret: return BinOp::BitXor;
    case TokenKind::Pipe: return BinOp::BitOr;
    case TokenKind::AmpAmp: return BinOp::LogAnd;
    case TokenKind::PipePipe: return BinOp::LogOr;
    default: return BinOp::Add;
    }
}

bool isAssignOp(TokenKind kind) {
    switch (kind) {
    case TokenKind::Assign:
    case TokenKind::PlusAssign:
    case TokenKind::MinusAssign:
    case TokenKind::StarAssign:
    case TokenKind::SlashAssign:
    case TokenKind::PercentAssign:
    case TokenKind::AmpAssign:
    case TokenKind::PipeAssign:
    case TokenKind::CaretAssign:
    case TokenKind::ShlAssign:
    case TokenKind::ShrAssign:
        return true;
    default:
        return false;
    }
}

AssignOp assignOpOf(TokenKind kind) {
    switch (kind) {
    case TokenKind::PlusAssign: return AssignOp::Add;
    case TokenKind::MinusAssign: return AssignOp::Sub;
    case TokenKind::StarAssign: return AssignOp::Mul;
    case TokenKind::SlashAssign: return AssignOp::Div;
    case TokenKind::PercentAssign: return AssignOp::Mod;
    case TokenKind::AmpAssign: return AssignOp::BitAnd;
    case TokenKind::PipeAssign: return AssignOp::BitOr;
    case TokenKind::CaretAssign: return AssignOp::BitXor;
    case TokenKind::ShlAssign: return AssignOp::Shl;
    case TokenKind::ShrAssign: return AssignOp::Shr;
    default: return AssignOp::Assign;
    }
}

bool isDigitChar(char c) { return c >= '0' && c <= '9'; }

/// Splits a literal token into its numeric body and optional type suffix
/// (`100i8`, `0xFFu8`, `1.5f32`, `1.5d`).
void splitLiteralSuffix(std::string_view text, std::string& body, LitValue::Suffix& suffix,
                        unsigned& bits) {
    body = std::string(text);
    suffix = LitValue::Suffix::None;
    bits = 0;

    const bool hex = body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X');
    const bool bin = body.size() > 2 && body[0] == '0' && (body[1] == 'b' || body[1] == 'B');

    if (hex || bin) {
        for (std::size_t i = 2; i < body.size(); ++i) {
            if (body[i] == 'i' || body[i] == 'u') {
                suffix = body[i] == 'i' ? LitValue::Suffix::I : LitValue::Suffix::U;
                bits = static_cast<unsigned>(std::strtoul(body.c_str() + i + 1, nullptr, 10));
                body.resize(i);
                return;
            }
        }
        return;
    }

    std::size_t digitsStart = body.size();
    while (digitsStart > 0 && isDigitChar(body[digitsStart - 1])) {
        --digitsStart;
    }

    if (digitsStart > 0) {
        const char c = body[digitsStart - 1];
        if (c == 'i' || c == 'u' || c == 'f') {
            suffix = c == 'i' ? LitValue::Suffix::I
                              : (c == 'u' ? LitValue::Suffix::U : LitValue::Suffix::F);
            bits = static_cast<unsigned>(std::strtoul(body.c_str() + digitsStart, nullptr, 10));
            body.resize(digitsStart - 1);
            return;
        }
        if (c == 'd') {
            suffix = LitValue::Suffix::D;
            body.resize(digitsStart - 1);
            return;
        }
        return;
    }

    if (!body.empty() && body.back() == 'd') {
        suffix = LitValue::Suffix::D;
        body.pop_back();
    }
}

/// Decodes a character literal body into a UTF-16 code unit.
std::uint32_t decodeCharBody(std::string_view body) {
    if (body.empty()) {
        return 0;
    }
    if (body[0] != '\\') {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(body[0]));
    }
    if (body.size() < 2) {
        return static_cast<std::uint32_t>(body[0]);
    }
    const char esc = body[1];
    switch (esc) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return 0;
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case 'x': {
        if (body.size() >= 4) {
            return static_cast<std::uint32_t>(std::strtoul(std::string(body.substr(2, 2)).c_str(),
                                                           nullptr, 16));
        }
        return 0;
    }
    case 'u': {
        if (body.size() >= 6) {
            return static_cast<std::uint32_t>(std::strtoul(std::string(body.substr(2, 4)).c_str(),
                                                           nullptr, 16));
        }
        return 0;
    }
    default:
        return static_cast<std::uint32_t>(static_cast<unsigned char>(esc));
    }
}

} // namespace

Parser::Parser(std::vector<Token> tokens, DiagnosticEngine& diagnostics)
    : m_tokens(std::move(tokens)), m_diagnostics(diagnostics) {
    if (m_tokens.empty()) {
        m_tokens.push_back(Token{});
    }
}

const Token& Parser::peek(std::size_t ahead) const {
    const std::size_t index = m_index + ahead;
    return index < m_tokens.size() ? m_tokens[index] : m_tokens.back();
}

const Token& Parser::previous() const { return m_index > 0 ? m_tokens[m_index - 1] : m_tokens.front(); }

bool Parser::check(TokenKind kind) const { return peek().kind == kind; }

bool Parser::atEnd() const { return peek().kind == TokenKind::EndOfFile; }

const Token& Parser::advance() {
    if (!atEnd()) {
        ++m_index;
    }
    return previous();
}

bool Parser::match(TokenKind kind) {
    if (!check(kind)) {
        return false;
    }
    advance();
    return true;
}

const Token& Parser::expect(TokenKind kind, std::string_view context) {
    if (check(kind)) {
        return advance();
    }
    m_diagnostics.error(peek().range, "expected '" + std::string(tokenKindSpelling(kind)) + "' " +
                                          std::string(context) + ", found '" + describe(peek()) +
                                          "'");
    return peek();
}

void Parser::errorAt(const Token& token, std::string message) {
    m_diagnostics.error(token.range, std::move(message));
}

//===----------------------------------------------------------------------===//
// Unit and declarations
//===----------------------------------------------------------------------===//

std::unique_ptr<Unit> Parser::parseUnit() {
    auto unit = std::make_unique<Unit>();

    while (!atEnd()) {
        const std::size_t before = m_index;

        // `library "name";` marks the file as a set of external declarations.
        // It is checked here rather than in parseTopLevel because it is a
        // property of the unit, not a declaration that produces code.
        if (check(TokenKind::KwLibrary)) {
            const Token keyword = advance();
            if (check(TokenKind::StringLit)) {
                const Token path = advance();
                const std::string_view raw = path.text;
                const std::string_view body =
                    raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : std::string_view{};
                if (unit->isExternalLibrary()) {
                    errorAt(keyword, "this file already declares library '" +
                                         unit->libraryName + "'");
                } else {
                    unit->libraryName = decodeEscapes(body);
                    unit->libraryRange = join(keyword.range, path.range);
                }
            } else {
                errorAt(peek(), "expected a quoted library name, e.g. library \"mylib\";");
            }
            if (check(TokenKind::Semicolon)) {
                advance();
            } else {
                errorAt(peek(), "expected ';' after the library declaration");
            }
            if (m_index == before) {
                advance();
            }
            continue;
        }

        // `module "json" version "0.1.0";`
        //
        // Package identity of the file itself, for the tooling that will one day
        // resolve versions. Like `library`, it describes the unit rather than
        // declaring anything that produces code, so it is handled here.
        //
        // `version` is a *contextual* keyword: it is the natural name for a
        // variable, and reserving it globally to read one directive would break
        // source that never uses the directive. `module` is reserved, because it
        // only ever means this.
        if (check(TokenKind::KwModule)) {
            const Token keyword = advance();
            if (!check(TokenKind::StringLit)) {
                errorAt(peek(), "expected a quoted module name, e.g. module \"json\";");
            } else {
                const Token nameToken = advance();
                const std::string name = decodeStringLiteral(nameToken);
                const bool hasVersion =
                    peek().kind == TokenKind::Ident && peek().text == "version";
                if (hasVersion) {
                    advance();
                }
                if (!check(TokenKind::StringLit)) {
                    errorAt(peek(), hasVersion
                                        ? "expected a quoted version, e.g. version \"0.1.0\""
                                        : "expected 'version \"x.y.z\"' after the module name");
                } else {
                    const Token versionToken = advance();
                    const std::string version = decodeStringLiteral(versionToken);
                    if (!isValidModuleVersion(version)) {
                        errorAt(versionToken,
                                "a module version is 'MAJOR.MINOR.PATCH' with decimal numbers, "
                                "for example \"1.2.0\"; found \"" +
                                    version + "\"");
                    }
                    if (unit->declaresModule()) {
                        errorAt(keyword, "this file already declares module '" +
                                             unit->moduleName + "'");
                    } else {
                        unit->moduleName = name;
                        unit->moduleVersion = version;
                        unit->moduleRange = join(keyword.range, versionToken.range);
                    }
                }
            }
            if (check(TokenKind::Semicolon)) {
                advance();
            } else {
                errorAt(peek(), "expected ';' after the module declaration");
            }
            if (m_index == before) {
                advance();
            }
            continue;
        }

        StmtPtr decl = parseTopLevel();
        if (decl) {
            unit->decls.push_back(std::move(decl));
        }

        // Progress is measured in tokens consumed, never in whether a node came
        // back. An error path that reports a diagnostic without advancing used
        // to spin here forever, appending a node on every turn until the
        // machine ran out of memory.
        if (m_index == before) {
            advance();
        }
    }
    return unit;
}

StmtPtr Parser::parseTopLevel() {
    switch (peek().kind) {
    case TokenKind::KwFunction:
        return parseFunction();
    case TokenKind::KwStruct:
        return parseStructLike(StmtKind::StructDecl, "struct");
    case TokenKind::KwUnion:
        return parseStructLike(StmtKind::UnionDecl, "union");
    case TokenKind::KwEnum:
        return parseEnum();
    case TokenKind::KwGlobal: {
        advance();
        bool isConst = match(TokenKind::KwConst);
        bool isComptime = !isConst && match(TokenKind::KwComptime);
        return parseGlobal(isConst, isComptime);
    }
    case TokenKind::KwConst: {
        advance();
        return parseVarDecl(true, true, false);
    }
    case TokenKind::KwComptime: {
        advance();
        return parseVarDecl(true, false, true);
    }
    default:
        break;
    }

    if (looksLikeDeclaration()) {
        return parseVarDecl(true, false, false);
    }

    // `import "file.ax";` at file scope: the short form of a file import.
    if (peek().kind == TokenKind::KwImport) {
        const Token keyword = advance();
        auto decl = std::make_unique<Stmt>(StmtKind::ImportDecl, keyword.range);
        decl->funcKind = FuncKind::Import;
        decl->modulesArePaths = true;

        const auto takePath = [&]() {
            const Token module = advance();
            const std::string_view raw = module.text;
            const std::string_view body =
                raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : std::string_view{};
            decl->modules.emplace_back(decodeEscapes(body), module.range);
        };
        if (check(TokenKind::StringLit)) {
            takePath();
        } else if (check(TokenKind::LParen)) {
            // `import("file.ax");` -- accepted so a stray pair of parens is not
            // a hard error.
            advance();
            if (check(TokenKind::StringLit)) {
                takePath();
            }
            expect(TokenKind::RParen, "after the imported file path");
        } else {
            errorAt(peek(), "expected a quoted file path, e.g. import \"lib.ax\";");
        }
        if (check(TokenKind::Semicolon)) {
            advance();
        } else {
            errorAt(peek(), "expected ';' after the import");
        }
        decl->range = join(keyword.range, previous().range);
        return decl;
    }

    errorAt(peek(), "expected a top level declaration ('function', 'struct', 'enum', 'global' "
                    "or a variable declaration), found '" + describe(peek()) + "'");
    const Token where = peek();
    synchronize();
    return std::make_unique<Stmt>(StmtKind::Empty, where.range);
}

StmtPtr Parser::parseGlobal(bool isConst, bool isComptime) {
    return parseVarDecl(true, isConst, isComptime);
}

void Parser::parseTypeParamList(std::vector<std::string>& out, const char* what) {
    expect(TokenKind::Less, "to start the type parameter list");
    if (check(TokenKind::Greater)) {
        errorAt(peek(), std::string("a ") + what + "'s type parameter list cannot be empty");
    }
    while (true) {
        if (!check(TokenKind::Ident)) {
            errorAt(peek(), "a type parameter must be a name such as 'T'");
            break;
        }
        const Token param = advance();
        const std::string paramName(param.text);
        for (const std::string& existing : out) {
            if (existing == paramName) {
                errorAt(param, "duplicate type parameter '" + paramName + "'");
            }
        }
        out.push_back(paramName);
        if (!match(TokenKind::Comma)) {
            break;
        }
    }
    expect(TokenKind::Greater, "to close the type parameter list");
}

const Token& Parser::expectTypeArgClose() {
    if (check(TokenKind::Shr)) {
        // `Pair<Pair<int>>` lexes the two closes as one `>>`. Rewrite that token
        // into two `>` tokens: a lexer would have to know it was inside a type to
        // do this itself, and threading that context through it would be a much
        // larger change than two lines here.
        const Token both = m_tokens[m_index];
        Token first = both;
        first.kind = TokenKind::Greater;
        first.text = ">";
        first.range.end = SourceLocation{both.range.begin.file, both.range.begin.offset + 1};
        Token second = both;
        second.kind = TokenKind::Greater;
        second.text = ">";
        second.range.begin = first.range.end;
        m_tokens[m_index] = first;
        m_tokens.insert(m_tokens.begin() + static_cast<std::ptrdiff_t>(m_index) + 1, second);
    }
    return expect(TokenKind::Greater, "to close the type argument list");
}

void Parser::parseTypeArgumentList(TypeNode& node) {
    expect(TokenKind::Less, "to start the type argument list");
    if (check(TokenKind::Greater) || check(TokenKind::Shr)) {
        errorAt(peek(), "a type argument list cannot be empty");
    }
    while (true) {
        node.typeArgs.push_back(parseType());
        if (!match(TokenKind::Comma)) {
            break;
        }
    }
    const Token close = expectTypeArgClose();
    node.range = join(node.range, close.range);
}

StmtPtr Parser::parseStruct() { return parseStructLike(StmtKind::StructDecl, "struct"); }

StmtPtr Parser::parseStructLike(StmtKind kind, const char* keyword) {
    const Token keywordToken = advance(); // 'struct' / 'union'
    auto decl = std::make_unique<Stmt>(kind, keywordToken.range);

    // `keyword` names the construct in the diagnostics. Saying "as the struct
    // name" inside a `union` would make the reader check the wrong declaration.
    // The message is a named local, not a temporary: `expect` takes a
    // `string_view`, and a view into a destroyed temporary dangles.
    const std::string nameContext = std::string("as the ") + keyword + " name";
    const Token name = expect(TokenKind::Ident, nameContext);
    decl->name = std::string(name.text);
    decl->nameRange = name.range;

    // `struct Pair<T> { T first; T second; }`. Same shape as a generic function's
    // parameter list, and for the same reason: a `<` here cannot begin anything
    // else, so no lookahead is needed.
    if (check(TokenKind::Less)) {
        parseTypeParamList(decl->typeParams, keyword);
    }

    const std::string bodyContext = std::string("to start the ") + keyword + " body";
    expect(TokenKind::LBrace, bodyContext);
    while (!check(TokenKind::RBrace) && !atEnd()) {
        // A method travels with the type it belongs to, and `function` cannot
        // start a field declaration, so the two forms never collide.
        if (check(TokenKind::KwFunction)) {
            decl->methods.push_back(parseFunction(/*inStructBody=*/true));
            continue;
        }
        FieldDecl field;
        field.type = parseType();
        const Token fieldName = expect(TokenKind::Ident, "as the member name");
        field.name = std::string(fieldName.text);
        field.range = fieldName.range;
        const std::string memberContext = std::string("after the ") + keyword + " member";
        expect(TokenKind::Semicolon, memberContext);
        decl->fields.push_back(std::move(field));
    }

    const std::string closeContext = std::string("to close the ") + keyword + " body";
    const Token rbrace = expect(TokenKind::RBrace, closeContext);
    const std::string semiContext = std::string("after the ") + keyword + " declaration";
    expect(TokenKind::Semicolon, semiContext);
    decl->range = join(keywordToken.range, rbrace.range);
    return decl;
}

StmtPtr Parser::parseEnum() {
    const Token keyword = advance(); // 'enum'
    auto decl = std::make_unique<Stmt>(StmtKind::EnumDecl, keyword.range);

    const Token name = expect(TokenKind::Ident, "as the enum name");
    decl->name = std::string(name.text);
    decl->nameRange = name.range;

    expect(TokenKind::LBrace, "to start the enum body");
    while (!check(TokenKind::RBrace) && !atEnd()) {
        EnumItem item;
        const Token itemName = expect(TokenKind::Ident, "as the enum member name");
        item.name = std::string(itemName.text);
        item.range = itemName.range;
        if (match(TokenKind::Assign)) {
            item.value = parseExpression();
        }
        decl->items.push_back(std::move(item));
        if (!match(TokenKind::Comma)) {
            break;
        }
    }

    const Token rbrace = expect(TokenKind::RBrace, "to close the enum body");
    expect(TokenKind::Semicolon, "after the enum declaration");
    decl->range = join(keyword.range, rbrace.range);
    return decl;
}

StmtPtr Parser::parseVarDecl(bool global, bool isConst, bool isComptime, bool expectSemi) {
    auto decl = std::make_unique<Stmt>(StmtKind::VarDecl, peek().range);
    decl->isGlobal = global;
    decl->isConst = isConst;
    decl->isComptime = isComptime;
    parseVarDeclRest(*decl, expectSemi);
    return decl;
}

void Parser::parseVarDeclRest(Stmt& decl, bool expectSemi) {
    decl.type = parseType();

    const Token name = expect(TokenKind::Ident, "as the variable name");
    decl.name = std::string(name.text);
    decl.nameRange = name.range;

    while (check(TokenKind::LBracket)) {
        advance();
        ExprPtr length = parseExpression();
        expect(TokenKind::RBracket, "after the array length");
        decl.dims.push_back(std::move(length));
    }

    if (match(TokenKind::Assign)) {
        if (check(TokenKind::LBrace)) {
            decl.init = parseInitList();
        } else {
            decl.init = parseExpression();
        }
    }

    if (expectSemi) {
        const Token semi = expect(TokenKind::Semicolon, "after the variable declaration");
        decl.range = join(decl.range, semi.range);
    } else {
        decl.range = join(decl.range, previous().range);
    }
}

Param Parser::parseParam(bool allowUnnamed) {
    Param param;
    param.isConst = match(TokenKind::KwConst);
    param.type = parseType();
    // A method receiver is written as a bare type -- `self` or `ptr self` --
    // because there is nothing useful to call it: the body always says `self`.
    // The absence of a name is what distinguishes a receiver from an ordinary
    // parameter, and it is only allowed in the first position of a method, so no
    // existing function can be reinterpreted.
    if (allowUnnamed && (check(TokenKind::Comma) || check(TokenKind::RParen))) {
        param.isReceiver = true;
        param.name = "self";
        param.range = param.type->range;
        return param;
    }
    // Reaching here means a name is required. If what was written looks like a
    // receiver (`self` / `ptr self`), say where receivers are allowed instead of
    // complaining about a missing identifier -- that message would send the
    // reader to the wrong place entirely.
    if (check(TokenKind::Comma) || check(TokenKind::RParen)) {
        const bool looksLikeReceiver =
            param.type != nullptr &&
            ((param.type->kind == TypeNode::Kind::Named && param.type->name == "self") ||
             (param.type->kind == TypeNode::Kind::Ptr && param.type->inner != nullptr &&
              param.type->inner->kind == TypeNode::Kind::Named &&
              param.type->inner->name == "self"));
        if (looksLikeReceiver) {
            errorAt(peek(),
                    "a method receiver is only allowed as the first parameter inside a struct "
                    "body; here the parameter needs a name");
            param.name = "self";
            param.range = param.type->range;
            return param;
        }
    }

    const Token name = expect(TokenKind::Ident, "as the parameter name");
    param.name = std::string(name.text);
    param.range = name.range;

    while (check(TokenKind::LBracket)) {
        advance();
        ExprPtr length = parseExpression();
        expect(TokenKind::RBracket, "after the array length");
        auto wrap = std::make_unique<TypeNode>();
        wrap->kind = TypeNode::Kind::Array;
        wrap->range = join(param.type->range, previous().range);
        wrap->inner = std::move(param.type);
        wrap->arrayLen = std::move(length);
        param.type = std::move(wrap);
    }
    return param;
}

void Parser::parseModuleList(Stmt& decl) {
    expect(TokenKind::LParen, "to start the module list");
    while (!check(TokenKind::RParen) && !atEnd()) {
        if (check(TokenKind::StringLit)) {
            const Token module = advance();
            decl.modulesArePaths = true;
            // `text` is the raw lexeme including quotes; paths need the body.
            const std::string_view raw = module.text;
            const std::string_view body =
                raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : std::string_view{};
            decl.modules.emplace_back(decodeEscapes(body), module.range);
        } else {
            // `Math` is a module name; `quicksort.ax` is a dotted name, which
            // can only sensibly be a file -- accept it as such.
            const Token module = expect(TokenKind::Ident, "as a module name");
            std::string name(module.text);
            bool dotted = false;
            while (check(TokenKind::Dot)) {
                advance();
                const Token part = expect(TokenKind::Ident, "after '.' in a module path");
                name += '.';
                name += std::string(part.text);
                dotted = true;
            }
            if (dotted) {
                decl.modulesArePaths = true;
            }
            decl.modules.emplace_back(std::move(name), module.range);
        }
        if (!match(TokenKind::Comma)) {
            break;
        }
    }
    expect(TokenKind::RParen, "after the module list");
}

void Parser::parseImportParams(Stmt& decl) {
    while (!check(TokenKind::RParen) && !atEnd()) {
        if (match(TokenKind::KwIsCf)) {
            expect(TokenKind::Colon, "after 'is_cf'");
            if (match(TokenKind::KwTrue)) {
                decl.isCf = true;
            } else if (match(TokenKind::KwFalse)) {
                decl.isCf = false;
            } else {
                errorAt(peek(), "expected 'true' or 'false' after 'is_cf:'");
                advance();
            }
        } else if (match(TokenKind::KwReturn)) {
            expect(TokenKind::Colon, "after 'return'");
            parseModuleList(decl);
        } else {
            // Configuration parameter: parsed and ignored at run time.
            Param param = parseParam();
            decl.params.push_back(std::move(param));
        }

        if (!match(TokenKind::Comma)) {
            break;
        }
    }
    expect(TokenKind::RParen, "after the import parameter list");
}

void Parser::parseImportBody(Stmt& decl) {
    expect(TokenKind::LBrace, "to start the import body");
    if (check(TokenKind::KwReturn)) {
        advance();
        parseModuleList(decl);
        expect(TokenKind::Semicolon, "after the import return list");
    } else if (!check(TokenKind::RBrace)) {
        errorAt(peek(),
                "an import body may only contain comments and a single 'return (Module, ...);'");
        // Skip the offending tokens so the closing brace is found.
        int depth = 0;
        while (!atEnd()) {
            if (check(TokenKind::LBrace)) {
                ++depth;
            } else if (check(TokenKind::RBrace)) {
                if (depth == 0) {
                    break;
                }
                --depth;
            }
            advance();
        }
    }
    expect(TokenKind::RBrace, "to close the import body");
}


StmtPtr Parser::parseImportDecl() {
    const Token keyword = advance(); // 'function'
    advance();                       // 'import'
    auto decl = std::make_unique<Stmt>(StmtKind::ImportDecl, keyword.range);
    decl->funcKind = FuncKind::Import;

    if (check(TokenKind::Ident)) {
        const Token name = advance();
        decl->name = std::string(name.text);
        decl->nameRange = name.range;
    }

    expect(TokenKind::LParen, "after 'function import'");
    parseImportParams(*decl);

    if (match(TokenKind::Semicolon)) {
        decl->range = join(keyword.range, previous().range);
        return decl;
    }
    if (check(TokenKind::LBrace)) {
        parseImportBody(*decl);
        decl->range = join(keyword.range, previous().range);
        return decl;
    }

    // `function import():void { ... }` -- a function signature pasted onto an
    // import. `import` has no return type and is reserved (spec §2.2.1), so the
    // ': void' is the only thing to remove; keep parsing the body so the module
    // list is still collected and the rest of the file does not cascade.
    if (check(TokenKind::Colon)) {
        errorAt(peek(),
                "'import' has no return type and 'import' is reserved, so a function cannot be "
                "named 'import' either; write 'function import (return:(Module));' -- delete the "
                "':void'");
        while (!atEnd() && !check(TokenKind::LBrace) && !check(TokenKind::Semicolon) &&
               !check(TokenKind::RBrace)) {
            advance();
        }
        // Treat the body as an import body only when it looks like one, so a
        // pasted function signature yields this single diagnostic instead of a
        // second one about the statements inside.
        if (check(TokenKind::LBrace) &&
            (peek(1).kind == TokenKind::KwReturn || peek(1).kind == TokenKind::RBrace)) {
            parseImportBody(*decl);
        } else if (check(TokenKind::LBrace)) {
            int depth = 0;
            while (!atEnd()) {
                if (check(TokenKind::LBrace)) {
                    ++depth;
                } else if (check(TokenKind::RBrace)) {
                    if (depth == 0) {
                        advance();
                        break;
                    }
                    --depth;
                }
                advance();
            }
        } else if (check(TokenKind::Semicolon)) {
            advance();
        }
        decl->range = join(keyword.range, previous().range);
        return decl;
    }

    errorAt(peek(), "expected ';' or '{' after an import declaration");
    return decl;
}

StmtPtr Parser::parseFunction(bool inStructBody) {
    if (peek(1).kind == TokenKind::KwImport) {
        return parseImportDecl();
    }

    const Token keyword = advance(); // 'function'
    auto decl = std::make_unique<Stmt>(StmtKind::FuncDecl, keyword.range);

    if (match(TokenKind::KwPrivate)) {
        decl->funcKind = FuncKind::Private;
    } else if (match(TokenKind::KwGlobal)) {
        decl->funcKind = FuncKind::Global;
    } else if (match(TokenKind::KwEvent)) {
        decl->funcKind = FuncKind::Event;
    } else if (match(TokenKind::KwProcess)) {
        decl->funcKind = FuncKind::Process;
    }

    const Token name = expect(TokenKind::Ident, "as the function name");
    decl->name = std::string(name.text);
    decl->nameRange = name.range;

    // `function name<T, U>(...)`. A `<` straight after the declared name can only
    // be a type parameter list -- no expression can start here -- so this needs
    // no lookahead tricks. (Call sites are a different matter: there, `f<int>(x)`
    // would be indistinguishable from a comparison, which is why type arguments
    // are inferred at calls and not spelled out. See Sema's inferTypeArguments.)
    if (check(TokenKind::Less)) {
        parseTypeParamList(decl->typeParams, "function");
    }

    expect(TokenKind::LParen, "after the function name");
    if (!check(TokenKind::RParen)) {
        while (true) {
            if (check(TokenKind::KwReturn)) {
                advance();
                expect(TokenKind::Colon, "after 'return' in the parameter list");
                decl->defaultRet = parseExpression();
                break;
            }
            decl->params.push_back(parseParam(inStructBody && decl->params.empty()));
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
    }
    expect(TokenKind::RParen, "after the parameter list");

    if (check(TokenKind::LParen)) {
        advance();
        decl->eventCond = parseExpression();
        expect(TokenKind::RParen, "after the event trigger condition");
        if (decl->funcKind != FuncKind::Event) {
            errorAt(keyword, "only an 'event' function may declare a trigger condition");
        }
    }

    expect(TokenKind::Colon,
           "and the return type after the parameter list; write ':void' if the function "
           "returns nothing");
    decl->retType = parseType();

    if (match(TokenKind::Semicolon)) {
        decl->isForward = true;
        decl->range = join(keyword.range, previous().range);
        return decl;
    }

    if (!check(TokenKind::LBrace)) {
        errorAt(peek(), "expected '{' to start the body of '" + decl->name + "' or ';' for a "
                        "forward declaration, found '" + describe(peek()) + "'");
        decl->range = join(keyword.range, previous().range);
        return decl;
    }

    StmtPtr bodyHolder = parseBodyBlock(decl->body, decl->eofBody, decl->hasEof,
                                        decl->eofExplicitEmpty);
    decl->range = join(keyword.range, bodyHolder->range);
    return decl;
}

StmtPtr Parser::parseBodyBlock(std::vector<StmtPtr>& body, std::vector<StmtPtr>& eofBody,
                               bool& hasEof, bool& eofExplicitEmpty) {
    const Token lbrace = expect(TokenKind::LBrace, "to start the block");
    auto holder = std::make_unique<Stmt>(StmtKind::Block, lbrace.range);

    while (!check(TokenKind::RBrace) && !atEnd()) {
        if (check(TokenKind::KwEof)) {
            advance();
            expect(TokenKind::Colon, "after 'eof'");
            hasEof = true;
            while (!check(TokenKind::RBrace) && !atEnd()) {
                if (check(TokenKind::KwEof)) {
                    errorAt(peek(), "'eof' may appear at most once, at the end of the function");
                    advance();
                    continue;
                }
                if (StmtPtr stmt = parseStmt()) {
                    eofBody.push_back(std::move(stmt));
                }
            }
            eofExplicitEmpty = eofBody.empty();
            break;
        }
        if (StmtPtr stmt = parseStmt()) {
            body.push_back(std::move(stmt));
        }
    }

    const Token rbrace = expect(TokenKind::RBrace, "to close the block");
    holder->range = join(lbrace.range, rbrace.range);
    return holder;
}

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

TypeNodePtr Parser::makeScalarType(const Token& token) {
    const std::string text(token.text);
    auto node = std::make_unique<TypeNode>();
    node->range = token.range;
    node->spelling = text;

    if (text == "char") {
        node->base = ScalarBase::Char;
        return node;
    }
    if (text == "bool") {
        node->base = ScalarBase::Bool;
        return node;
    }
    if (text == "void") {
        node->base = ScalarBase::Void;
        return node;
    }
    if (text == "string") {
        node->kind = TypeNode::Kind::Named;
        node->name = "string";
        return node;
    }
    if (text == "ptr") {
        node->kind = TypeNode::Kind::Ptr;
        // `ptr const T` marks a pointer a native function will not write
        // through, so the VM can skip copying the array back after the call.
        if (check(TokenKind::KwConst)) {
            advance();
            node->isReadOnlyPtr = true;
        }
        node->inner = parseTypeAtom();
        node->range = join(token.range, previous().range);
        return node;
    }
    if (text == "vector") {
        node->kind = TypeNode::Kind::Vector;
        node->inner = parseTypeAtom();
        node->range = join(token.range, previous().range);
        return node;
    }

    if (text.rfind("double", 0) == 0) {
        node->base = ScalarBase::Float;
        const std::size_t dot = text.find('.');
        if (dot == std::string::npos) {
            node->bits = 64;
        } else {
            node->mantissa = static_cast<unsigned>(
                std::strtoul(text.substr(6, dot - 6).c_str(), nullptr, 10));
            node->exponent = static_cast<unsigned>(
                std::strtoul(text.substr(dot + 1).c_str(), nullptr, 10));
            node->bits = 0;
        }
        return node;
    }

    if (text.rfind("float", 0) == 0) {
        node->base = ScalarBase::Float;
        node->bits = text.size() > 5 ? static_cast<unsigned>(std::strtoul(text.c_str() + 5, nullptr, 10)) : 32;
        return node;
    }

    // `int` / `intN`
    node->base = ScalarBase::Int;
    node->bits = text.size() > 3 ? static_cast<unsigned>(std::strtoul(text.c_str() + 3, nullptr, 10)) : 32;
    return node;
}

TypeNodePtr Parser::parseTypeAtom() {
    const Token token = peek();

    if (token.kind == TokenKind::TypeName) {
        advance();
        return makeScalarType(token);
    }

    if (token.kind == TokenKind::KwUnsigned) {
        advance();
        if (peek().kind != TokenKind::TypeName) {
            errorAt(peek(), "'unsigned' must be followed by 'int', 'intN' or 'char'");
            auto node = std::make_unique<TypeNode>();
            node->range = token.range;
            node->base = ScalarBase::Int;
            node->bits = 32;
            node->isUnsigned = true;
            return node;
        }
        const Token base = advance();
        TypeNodePtr node = makeScalarType(base);
        node->isUnsigned = true;
        node->range = join(token.range, base.range);
        const std::string spelling(base.text);
        if (!(spelling.rfind("int", 0) == 0 || spelling == "char")) {
            errorAt(base, "'unsigned' may only modify 'int', 'intN' or 'char', not '" + spelling +
                              "'");
        }
        return node;
    }

    if (token.kind == TokenKind::LParen) {
        advance();
        auto first = parseType();
        if (check(TokenKind::Comma)) {
            auto tuple = std::make_unique<TypeNode>();
            tuple->kind = TypeNode::Kind::Tuple;
            tuple->range = token.range;
            tuple->elems.push_back(std::move(first));
            while (match(TokenKind::Comma)) {
                tuple->elems.push_back(parseType());
            }
            const Token rparen = expect(TokenKind::RParen, "after the tuple type");
            tuple->range = join(token.range, rparen.range);
            return tuple;
        }
        expect(TokenKind::RParen, "after the parenthesised type");
        return first;
    }

    if (token.kind == TokenKind::Ident) {
        advance();
        auto node = std::make_unique<TypeNode>();
        node->kind = TypeNode::Kind::Named;
        node->name = std::string(token.text);
        node->range = token.range;
        // `Pair<int>` / `Map<string, int>`. A `<` in type position can only start
        // a type argument list, so this needs no disambiguation from comparison.
        if (check(TokenKind::Less)) {
            parseTypeArgumentList(*node);
        }
        return node;
    }

    errorAt(token, "expected a type, found '" + describe(token) + "'");
    if (!atEnd()) {
        advance();
    }
    auto node = std::make_unique<TypeNode>();
    node->range = token.range;
    node->base = ScalarBase::Int;
    node->bits = 32;
    return node;
}

TypeNodePtr Parser::parseType() {
    TypeNodePtr node = parseTypeAtom();

    while (check(TokenKind::LBracket)) {
        advance();
        ExprPtr length = parseExpression();
        const Token rbrace = expect(TokenKind::RBracket, "after the array length");
        auto wrap = std::make_unique<TypeNode>();
        wrap->kind = TypeNode::Kind::Array;
        wrap->range = join(node->range, rbrace.range);
        wrap->inner = std::move(node);
        wrap->arrayLen = std::move(length);
        node = std::move(wrap);
    }
    return node;
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

bool Parser::isTypeStart(const Token& token) const {
    switch (token.kind) {
    case TokenKind::TypeName:
    case TokenKind::KwUnsigned:
    case TokenKind::KwConst:
    case TokenKind::KwComptime:
        return true;
    default:
        return false;
    }
}

bool Parser::isLabelAhead() const {
    if (!check(TokenKind::Ident) || peek(1).kind != TokenKind::Colon) {
        return false;
    }
    const TokenKind next = peek(2).kind;
    return next == TokenKind::KwWhile || next == TokenKind::KwFor || next == TokenKind::KwLoop;
}

bool Parser::isTupleTypeDeclAhead() const {
    if (!check(TokenKind::LParen)) {
        return false;
    }

    int depth = 0;
    bool topLevelComma = false;
    std::size_t index = m_index;
    for (; index < m_tokens.size(); ++index) {
        const TokenKind kind = m_tokens[index].kind;
        if (kind == TokenKind::LParen) {
            ++depth;
        } else if (kind == TokenKind::RParen) {
            --depth;
            if (depth == 0) {
                break;
            }
        } else if (kind == TokenKind::Comma && depth == 1) {
            topLevelComma = true;
        } else if (kind == TokenKind::EndOfFile) {
            return false;
        }
    }
    if (depth != 0 || !topLevelComma || index + 1 >= m_tokens.size()) {
        return false;
    }

    std::size_t next = index + 1;
    if (m_tokens[next].kind != TokenKind::Ident) {
        return false;
    }
    ++next;
    while (next < m_tokens.size() && m_tokens[next].kind == TokenKind::LBracket) {
        // Skip a balanced [ ... ].
        int nested = 0;
        for (; next < m_tokens.size(); ++next) {
            if (m_tokens[next].kind == TokenKind::LBracket) {
                ++nested;
            } else if (m_tokens[next].kind == TokenKind::RBracket) {
                --nested;
                if (nested == 0) {
                    ++next;
                    break;
                }
            }
        }
    }
    if (next >= m_tokens.size()) {
        return false;
    }
    return m_tokens[next].kind == TokenKind::Assign || m_tokens[next].kind == TokenKind::Semicolon;
}

bool Parser::looksLikeDeclaration() const {
    if (check(TokenKind::TypeName) || check(TokenKind::KwUnsigned) || check(TokenKind::KwConst) ||
        check(TokenKind::KwComptime)) {
        return true;
    }
    if (check(TokenKind::Ident)) {
        if (peek(1).kind == TokenKind::Ident) {
            return true;
        }
        // `Pair<int> p = ...`. This is the one place where a generic type in a
        // declaration has to be told apart from a comparison, and the parser has
        // no symbol table to ask. The rule below is deliberately narrow: a name,
        // a `<`, then only tokens that can appear in a type, the matching `>`, and
        // a name after it. Anything else -- an operator, a `>=`, an unbalanced
        // list -- means this is an expression, so `x < y > z;` keeps parsing as a
        // comparison.
        if (peek(1).kind == TokenKind::Less) {
            return looksLikeGenericTypeDecl();
        }
    }
    return false;
}

bool Parser::scanGenericTypeRef(std::size_t& afterClose) const {
    std::size_t ahead = 2;   // past the name and the '<'
    int depth = 1;
    while (true) {
        const Token& token = peek(ahead);
        switch (token.kind) {
        case TokenKind::Less:
            depth += 1;
            break;
        case TokenKind::Greater:
            depth -= 1;
            break;
        case TokenKind::Shr:
            depth -= 2;
            break;
        // Tokens a type argument list can be built from. An array length is a
        // constant expression, so integer literals and the arithmetic that goes
        // with them are allowed too.
        case TokenKind::Ident:
        case TokenKind::TypeName:
        case TokenKind::KwUnsigned:
        case TokenKind::KwConst:
        case TokenKind::Comma:
        case TokenKind::LBracket:
        case TokenKind::RBracket:
        case TokenKind::LParen:
        case TokenKind::RParen:
        case TokenKind::IntLit:
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
            break;
        default:
            return false;
        }
        if (depth <= 0) {
            afterClose = ahead + 1;
            return true;
        }
        ahead += 1;
        if (ahead > 256) {
            // Runaway scan on malformed input; let the normal parser report it.
            return false;
        }
    }
}

bool Parser::looksLikeGenericTypeDecl() const {
    std::size_t afterClose = 0;
    // `X<a> b`: a declaration needs a name right after the closes.
    return scanGenericTypeRef(afterClose) && peek(afterClose).kind == TokenKind::Ident;
}

bool Parser::looksLikeGenericTypeRef() const {
    std::size_t afterClose = 0;
    // `X<a>.f(...)`: a static method on an instantiated type. The `.` is what
    // tells this apart from a comparison -- `a < b > c.d()` has a name after the
    // closes, not a dot.
    return scanGenericTypeRef(afterClose) && peek(afterClose).kind == TokenKind::Dot;
}

StmtPtr Parser::parseStmt() {
    if (atEnd()) {
        return nullptr;
    }

    switch (peek().kind) {
    case TokenKind::LBrace: {
        std::vector<StmtPtr> body;
        std::vector<StmtPtr> eofBody;
        bool hasEof = false;
        bool emptyEof = false;
        StmtPtr holder = parseBodyBlock(body, eofBody, hasEof, emptyEof);
        if (hasEof) {
            m_diagnostics.error(previous().range, "'eof' is only allowed at the end of a function");
        }
        auto block = std::make_unique<Stmt>(StmtKind::Block, holder->range);
        block->body = std::move(body);
        return block;
    }
    case TokenKind::KwIf:
        return parseIf();
    case TokenKind::KwWhile:
        return parseWhile();
    case TokenKind::KwFor:
        return parseFor();
    case TokenKind::KwLoop:
        return parseLoop();
    case TokenKind::KwSwitch:
        return parseSwitch();
    case TokenKind::KwReturn:
        return parseReturn();
    case TokenKind::KwBreak:
    case TokenKind::KwContinue: {
        const Token keyword = advance();
        auto stmt = std::make_unique<Stmt>(
            keyword.kind == TokenKind::KwBreak ? StmtKind::Break : StmtKind::Continue,
            keyword.range);
        if (check(TokenKind::Ident)) {
            stmt->label = std::string(advance().text);
        }
        const Token semi = expect(TokenKind::Semicolon, "after the jump statement");
        stmt->range = join(keyword.range, semi.range);
        return stmt;
    }
    case TokenKind::KwPass:
    case TokenKind::KwNext: {
        const Token keyword = advance();
        auto stmt = std::make_unique<Stmt>(
            keyword.kind == TokenKind::KwPass ? StmtKind::Pass : StmtKind::Next, keyword.range);
        const Token semi = expect(TokenKind::Semicolon, "after the statement");
        stmt->range = join(keyword.range, semi.range);
        return stmt;
    }
    case TokenKind::KwVirtual:
        return parseVirtual();
    case TokenKind::KwArenaScope: {
        const Token keyword = advance();
        std::vector<StmtPtr> body;
        std::vector<StmtPtr> eofBody;
        bool hasEof = false;
        bool emptyEof = false;
        StmtPtr holder = parseBodyBlock(body, eofBody, hasEof, emptyEof);
        if (hasEof) {
            m_diagnostics.error(keyword.range, "'eof' is only allowed at the end of a function");
        }
        auto stmt = std::make_unique<Stmt>(StmtKind::ArenaScope, join(keyword.range, holder->range));
        stmt->body = std::move(body);
        return stmt;
    }
    case TokenKind::KwFunction:
        return parseFunction();
    case TokenKind::KwStruct:
        return parseStructLike(StmtKind::StructDecl, "struct");
    case TokenKind::KwUnion:
        return parseStructLike(StmtKind::UnionDecl, "union");
    case TokenKind::KwEnum:
        return parseEnum();
    case TokenKind::KwConst:
        advance();
        return parseVarDecl(false, true, false);
    case TokenKind::KwComptime:
        advance();
        return parseVarDecl(false, false, true);
    case TokenKind::KwGlobal:
        errorAt(peek(), "'global' is only allowed at file scope");
        advance();
        return nullptr;
    case TokenKind::Semicolon: {
        const Token semi = advance();
        return std::make_unique<Stmt>(StmtKind::Empty, semi.range);
    }
    case TokenKind::KwEof:
        errorAt(peek(), "'eof' is only allowed at the end of a function body");
        advance();
        return nullptr;
    case TokenKind::LParen:
        if (isTupleTypeDeclAhead()) {
            return parseVarDecl(false, false, false);
        }
        break;
    default:
        break;
    }

    if (isLabelAhead()) {
        const Token label = advance();
        advance(); // ':'
        StmtPtr loop = parseStmt();
        if (loop) {
            if (loop->kind == StmtKind::While || loop->kind == StmtKind::For ||
                loop->kind == StmtKind::ForIn || loop->kind == StmtKind::Loop) {
                loop->label = std::string(label.text);
            } else {
                m_diagnostics.error(label.range,
                                    "a label may only precede a loop statement");
            }
        }
        return loop;
    }

    if (looksLikeDeclaration()) {
        return parseVarDecl(false, false, false);
    }

    // Tuple destructuring: `(a, b) = value;`
    if (check(TokenKind::LParen)) {
        const std::size_t save = m_index;
        advance();
        std::vector<std::string> names;
        std::vector<SourceRange> ranges;
        bool onlyNames = true;
        while (!check(TokenKind::RParen) && !atEnd()) {
            if (check(TokenKind::Ident)) {
                const Token name = advance();
                names.push_back(std::string(name.text));
                ranges.push_back(name.range);
            } else {
                onlyNames = false;
                break;
            }
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        if (onlyNames && check(TokenKind::RParen) && peek(1).kind == TokenKind::Assign &&
            names.size() >= 2) {
            const Token lparen = m_tokens[save];
            advance(); // ')'
            advance(); // '='
            auto stmt = std::make_unique<Stmt>(StmtKind::Destructure, lparen.range);
            stmt->targets = std::move(names);
            stmt->targetRanges = std::move(ranges);
            stmt->init = parseAssignment();
            const Token semi = expect(TokenKind::Semicolon, "after the destructuring assignment");
            stmt->range = join(lparen.range, semi.range);
            return stmt;
        }
        m_index = save;
    }

    ExprPtr expr = parseExpression();
    const Token semi = expect(TokenKind::Semicolon, "after the expression");
    auto stmt = std::make_unique<Stmt>(StmtKind::ExprStmt, join(expr->range, semi.range));
    stmt->init = std::move(expr);
    return stmt;
}

StmtPtr Parser::parseIf() {
    const Token keyword = advance();
    expect(TokenKind::LParen, "after 'if'");
    ExprPtr condition = parseExpression();
    expect(TokenKind::RParen, "after the if condition");

    StmtPtr thenBranch = parseStmt();
    StmtPtr elseBranch;
    if (match(TokenKind::KwElse)) {
        elseBranch = parseStmt();
    }

    auto stmt = std::make_unique<Stmt>(StmtKind::If, keyword.range);
    stmt->cond = std::move(condition);
    stmt->thenS = std::move(thenBranch);
    stmt->elseS = std::move(elseBranch);
    SourceRange range = keyword.range;
    if (stmt->elseS) {
        range.end = stmt->elseS->range.end;
    } else if (stmt->thenS) {
        range.end = stmt->thenS->range.end;
    }
    stmt->range = range;
    return stmt;
}

StmtPtr Parser::parseWhile() {
    const Token keyword = advance();
    expect(TokenKind::LParen, "after 'while'");
    ExprPtr condition = parseExpression();
    expect(TokenKind::RParen, "after the while condition");
    StmtPtr body = parseStmt();

    auto stmt = std::make_unique<Stmt>(StmtKind::While, keyword.range);
    stmt->cond = std::move(condition);
    stmt->thenS = std::move(body);
    if (stmt->thenS) {
        stmt->range = join(keyword.range, stmt->thenS->range);
    }
    return stmt;
}

StmtPtr Parser::parseFor() {
    const Token keyword = advance();

    if (check(TokenKind::Ident) && peek(1).kind == TokenKind::KwIn) {
        const Token name = advance();
        advance(); // 'in'
        auto stmt = std::make_unique<Stmt>(StmtKind::ForIn, keyword.range);
        stmt->name = std::string(name.text);
        stmt->nameRange = name.range;
        stmt->cond = parseExpression();
        stmt->thenS = parseStmt();
        if (stmt->thenS) {
            stmt->range = join(keyword.range, stmt->thenS->range);
        }
        return stmt;
    }

    expect(TokenKind::LParen, "after 'for'");
    auto stmt = std::make_unique<Stmt>(StmtKind::For, keyword.range);

    if (!check(TokenKind::Semicolon)) {
        if (looksLikeDeclaration()) {
            stmt->initS = parseVarDecl(false, false, false, false);
        } else if (check(TokenKind::KwConst) || check(TokenKind::KwComptime)) {
            const bool isConst = check(TokenKind::KwConst);
            advance();
            stmt->initS = parseVarDecl(false, isConst, !isConst, false);
        } else {
            auto initExpr = parseExpression();
            auto initStmt = std::make_unique<Stmt>(StmtKind::ExprStmt, initExpr->range);
            initStmt->init = std::move(initExpr);
            stmt->initS = std::move(initStmt);
        }
    }
    expect(TokenKind::Semicolon, "after the for initialiser");

    if (!check(TokenKind::Semicolon)) {
        stmt->cond = parseExpression();
    }
    expect(TokenKind::Semicolon, "after the for condition");

    if (!check(TokenKind::RParen)) {
        stmt->post = parseExpression();
    }
    expect(TokenKind::RParen, "after the for clause");

    stmt->thenS = parseStmt();
    if (stmt->thenS) {
        stmt->range = join(keyword.range, stmt->thenS->range);
    }
    return stmt;
}

StmtPtr Parser::parseLoop() {
    const Token keyword = advance();
    expect(TokenKind::LParen, "after 'loop'");
    ExprPtr count = parseExpression();
    expect(TokenKind::RParen, "after the loop count");
    StmtPtr body = parseStmt();

    auto stmt = std::make_unique<Stmt>(StmtKind::Loop, keyword.range);
    stmt->cond = std::move(count);
    stmt->thenS = std::move(body);
    if (stmt->thenS) {
        stmt->range = join(keyword.range, stmt->thenS->range);
    }
    return stmt;
}

StmtPtr Parser::parseSwitch() {
    const Token keyword = advance();
    expect(TokenKind::LParen, "after 'switch'");
    ExprPtr subject = parseExpression();
    expect(TokenKind::RParen, "after the switch expression");
    expect(TokenKind::LBrace, "to start the switch body");

    auto stmt = std::make_unique<Stmt>(StmtKind::Switch, keyword.range);
    stmt->cond = std::move(subject);

    while (!check(TokenKind::RBrace) && !atEnd()) {
        CaseClause clause;
        if (match(TokenKind::KwCase)) {
            clause.range = previous().range;
            clause.value = parseExpression();
            expect(TokenKind::Colon, "after the case value");
        } else if (match(TokenKind::KwDefault)) {
            clause.range = previous().range;
            expect(TokenKind::Colon, "after 'default'");
        } else {
            errorAt(peek(), "expected 'case' or 'default' in the switch body, found '" +
                                describe(peek()) + "'");
            advance();
            continue;
        }

        while (!check(TokenKind::KwCase) && !check(TokenKind::KwDefault) &&
               !check(TokenKind::RBrace) && !check(TokenKind::KwFallthrough) && !atEnd()) {
            if (StmtPtr body = parseStmt()) {
                clause.body.push_back(std::move(body));
            }
        }

        if (match(TokenKind::KwFallthrough)) {
            expect(TokenKind::Semicolon, "after 'fallthrough'");
            clause.hasFallthrough = true;
        }
        stmt->cases.push_back(std::move(clause));
    }

    const Token rbrace = expect(TokenKind::RBrace, "to close the switch body");
    stmt->range = join(keyword.range, rbrace.range);
    return stmt;
}

StmtPtr Parser::parseReturn() {
    const Token keyword = advance();
    ExprPtr value;
    if (!check(TokenKind::Semicolon) && !check(TokenKind::RBrace) && !atEnd()) {
        value = parseExpression();
    }
    const Token semi = expect(TokenKind::Semicolon, "after the return statement");
    auto stmt = std::make_unique<Stmt>(StmtKind::Return, join(keyword.range, semi.range));
    stmt->init = std::move(value);
    return stmt;
}

StmtPtr Parser::parseVirtual() {
    const Token keyword = advance();
    auto stmt = std::make_unique<Stmt>(StmtKind::Virtual, keyword.range);

    if (!check(TokenKind::LBrace)) {
        errorAt(peek(), "expected '{' after 'virtual'");
        return stmt;
    }
    StmtPtr holder =
        parseBodyBlock(stmt->body, stmt->eofBody, stmt->hasEof, stmt->eofExplicitEmpty);
    stmt->range = join(keyword.range, holder->range);
    return stmt;
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

ExprPtr Parser::parseExpression() { return parseAssignment(); }

ExprPtr Parser::parseAssignment() {
    ExprPtr lhs = parseTernary();

    if (isAssignOp(peek().kind)) {
        const Token op = advance();
        ExprPtr rhs = parseAssignment();
        auto expr = std::make_unique<Expr>(ExprKind::Assign, join(lhs->range, rhs->range));
        expr->assignop = assignOpOf(op.kind);
        expr->kids.push_back(std::move(lhs));
        expr->kids.push_back(std::move(rhs));
        return expr;
    }
    return lhs;
}

ExprPtr Parser::parseTernary() {
    ExprPtr condition = parseBinary(1);
    if (!match(TokenKind::Question)) {
        return condition;
    }

    ExprPtr thenValue = parseAssignment();
    expect(TokenKind::Colon, "in the conditional expression");
    ExprPtr elseValue = parseTernary();

    auto expr = std::make_unique<Expr>(ExprKind::Ternary, join(condition->range, elseValue->range));
    expr->kids.push_back(std::move(condition));
    expr->kids.push_back(std::move(thenValue));
    expr->kids.push_back(std::move(elseValue));
    return expr;
}

ExprPtr Parser::parseBinary(int minPrecedence) {
    ExprPtr lhs = parseCast();

    while (true) {
        const int precedence = precedenceOf(peek().kind);
        if (precedence < minPrecedence) {
            break;
        }

        const Token op = advance();
        ExprPtr rhs = parseBinary(precedence + 1);

        if (op.kind == TokenKind::DotDot) {
            auto range = std::make_unique<Expr>(ExprKind::Range, join(lhs->range, rhs->range));
            range->kids.push_back(std::move(lhs));
            range->kids.push_back(std::move(rhs));
            lhs = std::move(range);
            continue;
        }

        auto expr = std::make_unique<Expr>(ExprKind::Binary, join(lhs->range, rhs->range));
        expr->binop = binOpOf(op.kind);
        expr->kids.push_back(std::move(lhs));
        expr->kids.push_back(std::move(rhs));
        lhs = std::move(expr);
    }

    return lhs;
}

ExprPtr Parser::parseCast() {
    ExprPtr expr = parseUnary();

    while (match(TokenKind::KwAs)) {
        TypeNodePtr target = parseType();
        auto cast = std::make_unique<Expr>(ExprKind::Cast, expr->range);
        cast->typeNode = std::move(target);
        cast->kids.push_back(std::move(expr));
        expr = std::move(cast);
    }

    return expr;
}

ExprPtr Parser::parseUnary() {
    const Token token = peek();

    UnOp op;
    bool isUnary = true;
    switch (token.kind) {
    case TokenKind::Plus: op = UnOp::Plus; break;
    case TokenKind::Minus: op = UnOp::Neg; break;
    case TokenKind::Bang: op = UnOp::Not; break;
    case TokenKind::Tilde: op = UnOp::BitNot; break;
    case TokenKind::At: op = UnOp::Deref; break;
    case TokenKind::Hash: op = UnOp::AddrOf; break;
    default: isUnary = false; op = UnOp::Plus; break;
    }

    if (isUnary) {
        advance();
        ExprPtr operand = parseUnary();
        auto expr = std::make_unique<Expr>(ExprKind::Unary, join(token.range, operand->range));
        expr->unop = op;
        expr->kids.push_back(std::move(operand));
        return expr;
    }

    if (check(TokenKind::PlusPlus) || check(TokenKind::MinusMinus)) {
        const Token opToken = advance();
        ExprPtr operand = parseUnary();
        auto expr = std::make_unique<Expr>(
            opToken.kind == TokenKind::PlusPlus ? ExprKind::PreInc : ExprKind::PreDec,
            join(opToken.range, operand->range));
        expr->kids.push_back(std::move(operand));
        return expr;
    }

    if (check(TokenKind::KwSizeof) || check(TokenKind::KwAlignof)) {
        const Token keyword = advance();
        const ExprKind kind = keyword.kind == TokenKind::KwSizeof ? ExprKind::SizeOf
                                                                 : ExprKind::AlignOf;
        expect(TokenKind::LParen, "after the operator");

        // Try a type first: `sizeof(ptr int)`. Backtrack when it turns out to be
        // an expression such as `sizeof(a + b)`.
        if (isTypeStart(peek()) || check(TokenKind::Ident) || check(TokenKind::LParen)) {
            const std::size_t save = m_index;
            TypeNodePtr type = parseType();
            if (check(TokenKind::RParen)) {
                const Token rparen = advance();
                auto expr = std::make_unique<Expr>(kind, join(keyword.range, rparen.range));
                expr->typeNode = std::move(type);
                return expr;
            }
            m_index = save;
        }

        ExprPtr operand = parseExpression();
        const Token rparen = expect(TokenKind::RParen, "after the operand");
        auto expr = std::make_unique<Expr>(kind, join(keyword.range, rparen.range));
        expr->kids.push_back(std::move(operand));
        return expr;
    }

    return parsePostfix();
}

ExprPtr Parser::parsePostfix() {
    ExprPtr expr = parsePrimary();

    while (true) {
        if (match(TokenKind::LParen)) {
            std::vector<ExprPtr> args;
            if (!check(TokenKind::RParen)) {
                while (true) {
                    args.push_back(parseAssignment());
                    if (!match(TokenKind::Comma)) {
                        break;
                    }
                }
            }
            const Token rparen = expect(TokenKind::RParen, "after the arguments");
            auto call = std::make_unique<Expr>(ExprKind::Call, join(expr->range, rparen.range));
            call->kids.push_back(std::move(expr));
            for (ExprPtr& arg : args) {
                call->kids.push_back(std::move(arg));
            }
            expr = std::move(call);
            continue;
        }

        if (match(TokenKind::LBracket)) {
            // Parse the lower bound above the range level so that `a[1..3]` is
            // recognised as a slice rather than a range expression.
            ExprPtr first = parseBinary(2);
            if (match(TokenKind::DotDot)) {
                ExprPtr upper = parseExpression();
                const Token rbracket = expect(TokenKind::RBracket, "after the slice");
                auto slice =
                    std::make_unique<Expr>(ExprKind::Slice, join(expr->range, rbracket.range));
                slice->kids.push_back(std::move(expr));
                slice->kids.push_back(std::move(first));
                slice->kids.push_back(std::move(upper));
                expr = std::move(slice);
                continue;
            }
            const Token rbracket = expect(TokenKind::RBracket, "after the index");
            auto index = std::make_unique<Expr>(ExprKind::Index, join(expr->range, rbracket.range));
            index->kids.push_back(std::move(expr));
            index->kids.push_back(std::move(first));
            expr = std::move(index);
            continue;
        }

        if (match(TokenKind::Dot)) {
            const Token name = expect(TokenKind::Ident, "after '.'");
            auto member = std::make_unique<Expr>(ExprKind::Member, join(expr->range, name.range));
            member->text = std::string(name.text);
            member->kids.push_back(std::move(expr));
            expr = std::move(member);
            continue;
        }

        if (match(TokenKind::Arrow)) {
            const Token name = expect(TokenKind::Ident, "after '->'");
            auto member = std::make_unique<Expr>(ExprKind::Arrow, join(expr->range, name.range));
            member->text = std::string(name.text);
            member->kids.push_back(std::move(expr));
            expr = std::move(member);
            continue;
        }

        if (match(TokenKind::PlusPlus)) {
            auto post = std::make_unique<Expr>(ExprKind::PostInc, join(expr->range, previous().range));
            post->kids.push_back(std::move(expr));
            expr = std::move(post);
            continue;
        }

        if (match(TokenKind::MinusMinus)) {
            auto post = std::make_unique<Expr>(ExprKind::PostDec, join(expr->range, previous().range));
            post->kids.push_back(std::move(expr));
            expr = std::move(post);
            continue;
        }

        break;
    }

    return expr;
}

ExprPtr Parser::parseInitList() {
    const Token lbrace = expect(TokenKind::LBrace, "to start the initialiser list");
    auto expr = std::make_unique<Expr>(ExprKind::InitList, lbrace.range);

    if (!check(TokenKind::RBrace)) {
        while (true) {
            if (check(TokenKind::LBrace)) {
                expr->kids.push_back(parseInitList());
            } else {
                expr->kids.push_back(parseAssignment());
            }
            if (!match(TokenKind::Comma)) {
                break;
            }
            if (check(TokenKind::RBrace)) {
                break;
            }
        }
    }

    const Token rbrace = expect(TokenKind::RBrace, "to close the initialiser list");
    expr->range = join(lbrace.range, rbrace.range);
    return expr;
}

ExprPtr Parser::parsePrimary() {
    const Token token = peek();

    switch (token.kind) {
    case TokenKind::IntLit: {
        advance();
        auto expr = std::make_unique<Expr>(ExprKind::IntLit, token.range);
        std::string body;
        splitLiteralSuffix(token.text, body, expr->lit.suffix, expr->lit.suffixBits);
        expr->lit.text = std::string(token.text);
        expr->lit.fits128 = parseInt128(body, expr->lit.intValue);
        if (!expr->lit.fits128) {
            m_diagnostics.error(token.range,
                                "integer literal '" + std::string(token.text) +
                                    "' does not fit in 128 bits");
        }
        if (expr->lit.suffix == LitValue::Suffix::F) {
            expr->kind = ExprKind::FloatLit;
            expr->lit.floatValue = static_cast<double>(expr->lit.intValue);
        } else if (expr->lit.suffix == LitValue::Suffix::D) {
            expr->kind = ExprKind::FloatLit;
            expr->lit.floatValue = static_cast<double>(expr->lit.intValue);
        }
        return expr;
    }
    case TokenKind::FloatLit: {
        advance();
        auto expr = std::make_unique<Expr>(ExprKind::FloatLit, token.range);
        std::string body;
        splitLiteralSuffix(token.text, body, expr->lit.suffix, expr->lit.suffixBits);
        expr->lit.text = std::string(token.text);
        expr->lit.floatValue = std::strtod(body.c_str(), nullptr);
        return expr;
    }
    case TokenKind::CharLit: {
        advance();
        auto expr = std::make_unique<Expr>(ExprKind::CharLit, token.range);
        std::string_view text = token.text;
        if (text.size() >= 2) {
            expr->lit.charValue = decodeCharBody(text.substr(1, text.size() - 2));
        }
        return expr;
    }
    case TokenKind::StringLit: {
        advance();
        auto expr = std::make_unique<Expr>(ExprKind::StringLit, token.range);
        std::string_view text = token.text;

        bool raw = false;
        bool multiline = false;
        if (text.size() > 1 && text[0] == 'r' && text[1] == '"') {
            raw = true;
            text.remove_prefix(2);
            if (!text.empty() && text.back() == '"') {
                text.remove_suffix(1);
            }
        } else if (text.size() >= 6 && text.substr(0, 3) == "\"\"\"" &&
                   text.substr(text.size() - 3) == "\"\"\"") {
            multiline = true;
            text.remove_prefix(3);
            text.remove_suffix(3);
        } else if (text.size() >= 2) {
            text.remove_prefix(1);
            text.remove_suffix(1);
        }
        expr->lit.stringValue = raw ? std::string(text) : decodeEscapes(text);
        (void)multiline;
        return expr;
    }
    case TokenKind::KwTrue:
    case TokenKind::KwFalse: {
        advance();
        auto expr = std::make_unique<Expr>(ExprKind::BoolLit, token.range);
        expr->lit.intValue = token.kind == TokenKind::KwTrue ? 1 : 0;
        return expr;
    }
    case TokenKind::KwNull: {
        advance();
        return std::make_unique<Expr>(ExprKind::NullLit, token.range);
    }
    case TokenKind::Ident: {
        advance();
        auto expr = std::make_unique<Expr>(ExprKind::Name, token.range);
        expr->text = std::string(token.text);
        // `Pair<int>.zero()`: a static method on an instantiated generic type.
        // The name node carries the type, and the ordinary postfix loop builds the
        // `.zero()` part on top of it.
        if (check(TokenKind::Less) && looksLikeGenericTypeRef()) {
            auto typeNode = std::make_unique<TypeNode>();
            typeNode->kind = TypeNode::Kind::Named;
            typeNode->name = expr->text;
            typeNode->range = token.range;
            parseTypeArgumentList(*typeNode);
            expr->range = join(token.range, previous().range);
            expr->typeNode = std::move(typeNode);
        }
        return expr;
    }
    case TokenKind::LParen: {
        const Token lparen = advance();
        ExprPtr first = parseAssignment();
        if (check(TokenKind::Comma)) {
            auto tuple = std::make_unique<Expr>(ExprKind::TupleLit, lparen.range);
            tuple->kids.push_back(std::move(first));
            while (match(TokenKind::Comma)) {
                if (check(TokenKind::RParen)) {
                    break;
                }
                tuple->kids.push_back(parseAssignment());
            }
            const Token rparen = expect(TokenKind::RParen, "after the tuple");
            tuple->range = join(lparen.range, rparen.range);
            return tuple;
        }
        const Token rparen = expect(TokenKind::RParen, "after the expression");
        first->range = join(lparen.range, rparen.range);
        return first;
    }
    case TokenKind::KwImport: {
        // `import "x.ax";` or `import();` used as a statement. Both are attempts
        // to use `import` as a call, which it is not: it is a declaration, and
        // this build does not load modules at all. Saying
        // "expected an expression" here would explain none of that.
        if (peek(1).kind == TokenKind::StringLit) {
            errorAt(token,
                    "'import' takes a module list, not a file path, and this build does not "
                    "load modules; a program is compiled as a single file");
        } else {
            errorAt(token,
                    "'import' is a declaration, not a function to call: write "
                    "'function import (return:(Module));' at file scope. This build parses "
                    "that and ignores it; it does not load anything");
        }
        advance();
        while (!atEnd() && !check(TokenKind::Semicolon) && !check(TokenKind::RBrace)) {
            advance();
        }
        auto expr = std::make_unique<Expr>(ExprKind::IntLit, token.range);
        expr->lit.intValue = 0;
        return expr;
    }
    default:
        break;
    }

    errorAt(token, "expected an expression, found '" + describe(token) + "'");
    // Keep ';' and '}' : they are the caller's synchronisation points, and
    // eating them turns one mistake into a bogus error on the following line.
    if (!atEnd() && !check(TokenKind::Semicolon) && !check(TokenKind::RBrace)) {
        advance();
    }
    auto expr = std::make_unique<Expr>(ExprKind::IntLit, token.range);
    expr->lit.intValue = 0;
    return expr;
}

void Parser::synchronize() {
    while (!atEnd()) {
        if (check(TokenKind::Semicolon)) {
            advance();
            return;
        }
        if (check(TokenKind::RBrace) || check(TokenKind::LBrace)) {
            return;
        }
        if (check(TokenKind::KwFunction) || check(TokenKind::KwStruct) ||
            check(TokenKind::KwUnion) || check(TokenKind::KwEnum) ||
            check(TokenKind::KwGlobal)) {
            return;
        }
        advance();
    }
}

} // namespace acvm
