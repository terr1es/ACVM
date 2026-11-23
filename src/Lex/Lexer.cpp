//===--- Lexer.cpp ------------------------------------------------------===//
#include "acvm/Lex/Lexer.h"

#include "acvm/Common/Diagnostic.h"
#include "acvm/Common/SourceManager.h"

#include <array>
#include <cctype>
#include <utility>

namespace acvm {
namespace {

struct KeywordEntry {
    std::string_view word;
    TokenKind kind;
};

constexpr KeywordEntry kKeywords[] = {
    {"unsigned", TokenKind::KwUnsigned},       {"const", TokenKind::KwConst},
    {"comptime", TokenKind::KwComptime},       {"global", TokenKind::KwGlobal},
    {"function", TokenKind::KwFunction},       {"import", TokenKind::KwImport},
    {"private", TokenKind::KwPrivate},         {"library", TokenKind::KwLibrary},
    {"module", TokenKind::KwModule},
    {"event", TokenKind::KwEvent},
    {"process", TokenKind::KwProcess},         {"is_cf", TokenKind::KwIsCf},
    {"eof", TokenKind::KwEof},                 {"virtual", TokenKind::KwVirtual},
    {"arena_scope", TokenKind::KwArenaScope},  {"struct", TokenKind::KwStruct},
    {"enum", TokenKind::KwEnum},               {"union", TokenKind::KwUnion},
    {"return", TokenKind::KwReturn},
    {"pass", TokenKind::KwPass},
    {"next", TokenKind::KwNext},               {"if", TokenKind::KwIf},
    {"else", TokenKind::KwElse},               {"while", TokenKind::KwWhile},
    {"for", TokenKind::KwFor},                 {"in", TokenKind::KwIn},
    {"loop", TokenKind::KwLoop},               {"switch", TokenKind::KwSwitch},
    {"case", TokenKind::KwCase},               {"default", TokenKind::KwDefault},
    {"fallthrough", TokenKind::KwFallthrough}, {"break", TokenKind::KwBreak},
    {"continue", TokenKind::KwContinue},       {"as", TokenKind::KwAs},
    {"true", TokenKind::KwTrue},               {"false", TokenKind::KwFalse},
    {"null", TokenKind::KwNull},               {"sizeof", TokenKind::KwSizeof},
    {"alignof", TokenKind::KwAlignof},
};

constexpr std::string_view kTypeNames[] = {"int",  "float",  "double", "char", "bool",
                                           "void", "string", "ptr",    "vector"};

bool isIdentStart(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') || c == '_';
}

bool isIdentContinue(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return isIdentStart(c) || (uc >= '0' && uc <= '9');
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isHexDigit(char c) {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isBinDigit(char c) { return c == '0' || c == '1'; }

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

} // namespace

std::string_view tokenKindSpelling(TokenKind kind) {
    switch (kind) {
    case TokenKind::EndOfFile: return "<eof>";
    case TokenKind::Invalid: return "<invalid>";
    case TokenKind::Ident: return "identifier";
    case TokenKind::TypeName: return "type name";
    case TokenKind::IntLit: return "integer literal";
    case TokenKind::FloatLit: return "float literal";
    case TokenKind::CharLit: return "character literal";
    case TokenKind::StringLit: return "string literal";
    case TokenKind::KwUnsigned: return "unsigned";
    case TokenKind::KwConst: return "const";
    case TokenKind::KwComptime: return "comptime";
    case TokenKind::KwGlobal: return "global";
    case TokenKind::KwFunction: return "function";
    case TokenKind::KwImport: return "import";
    case TokenKind::KwLibrary: return "library";
    case TokenKind::KwModule: return "module";
    case TokenKind::KwPrivate: return "private";
    case TokenKind::KwEvent: return "event";
    case TokenKind::KwProcess: return "process";
    case TokenKind::KwIsCf: return "is_cf";
    case TokenKind::KwEof: return "eof";
    case TokenKind::KwVirtual: return "virtual";
    case TokenKind::KwArenaScope: return "arena_scope";
    case TokenKind::KwStruct: return "struct";
    case TokenKind::KwEnum: return "enum";
    case TokenKind::KwUnion: return "union";
    case TokenKind::KwVector: return "vector";
    case TokenKind::KwReturn: return "return";
    case TokenKind::KwPass: return "pass";
    case TokenKind::KwNext: return "next";
    case TokenKind::KwIf: return "if";
    case TokenKind::KwElse: return "else";
    case TokenKind::KwWhile: return "while";
    case TokenKind::KwFor: return "for";
    case TokenKind::KwIn: return "in";
    case TokenKind::KwLoop: return "loop";
    case TokenKind::KwSwitch: return "switch";
    case TokenKind::KwCase: return "case";
    case TokenKind::KwDefault: return "default";
    case TokenKind::KwFallthrough: return "fallthrough";
    case TokenKind::KwBreak: return "break";
    case TokenKind::KwContinue: return "continue";
    case TokenKind::KwAs: return "as";
    case TokenKind::KwTrue: return "true";
    case TokenKind::KwFalse: return "false";
    case TokenKind::KwNull: return "null";
    case TokenKind::KwSizeof: return "sizeof";
    case TokenKind::KwAlignof: return "alignof";
    case TokenKind::Plus: return "+";
    case TokenKind::Minus: return "-";
    case TokenKind::Star: return "*";
    case TokenKind::Slash: return "/";
    case TokenKind::Percent: return "%";
    case TokenKind::PlusPlus: return "++";
    case TokenKind::MinusMinus: return "--";
    case TokenKind::Assign: return "=";
    case TokenKind::PlusAssign: return "+=";
    case TokenKind::MinusAssign: return "-=";
    case TokenKind::StarAssign: return "*=";
    case TokenKind::SlashAssign: return "/=";
    case TokenKind::PercentAssign: return "%=";
    case TokenKind::AmpAssign: return "&=";
    case TokenKind::PipeAssign: return "|=";
    case TokenKind::CaretAssign: return "^=";
    case TokenKind::ShlAssign: return "<<=";
    case TokenKind::ShrAssign: return ">>=";
    case TokenKind::EqualEqual: return "==";
    case TokenKind::BangEqual: return "!=";
    case TokenKind::Less: return "<";
    case TokenKind::LessEqual: return "<=";
    case TokenKind::Greater: return ">";
    case TokenKind::GreaterEqual: return ">=";
    case TokenKind::AmpAmp: return "&&";
    case TokenKind::PipePipe: return "||";
    case TokenKind::Bang: return "!";
    case TokenKind::Tilde: return "~";
    case TokenKind::Amp: return "&";
    case TokenKind::Pipe: return "|";
    case TokenKind::Caret: return "^";
    case TokenKind::Shl: return "<<";
    case TokenKind::Shr: return ">>";
    case TokenKind::At: return "@";
    case TokenKind::Hash: return "#";
    case TokenKind::Dot: return ".";
    case TokenKind::Arrow: return "->";
    case TokenKind::DotDot: return "..";
    case TokenKind::Question: return "?";
    case TokenKind::Colon: return ":";
    case TokenKind::Comma: return ",";
    case TokenKind::Semicolon: return ";";
    case TokenKind::LParen: return "(";
    case TokenKind::RParen: return ")";
    case TokenKind::LBracket: return "[";
    case TokenKind::RBracket: return "]";
    case TokenKind::LBrace: return "{";
    case TokenKind::RBrace: return "}";
    }
    return "<unknown>";
}

std::string_view tokenKindName(TokenKind kind) {
    switch (kind) {
    case TokenKind::Ident:
    case TokenKind::TypeName:
    case TokenKind::IntLit:
    case TokenKind::FloatLit:
    case TokenKind::CharLit:
    case TokenKind::StringLit:
        return tokenKindSpelling(kind);
    default:
        break;
    }
    if (isKeywordKind(kind)) {
        return std::string_view("keyword");
    }
    return tokenKindSpelling(kind);
}

bool isKeywordKind(TokenKind kind) {
    switch (kind) {
    case TokenKind::KwUnsigned:
    case TokenKind::KwConst:
    case TokenKind::KwComptime:
    case TokenKind::KwGlobal:
    case TokenKind::KwFunction:
    case TokenKind::KwImport:
    case TokenKind::KwPrivate:
    case TokenKind::KwEvent:
    case TokenKind::KwProcess:
    case TokenKind::KwIsCf:
    case TokenKind::KwEof:
    case TokenKind::KwVirtual:
    case TokenKind::KwArenaScope:
    case TokenKind::KwStruct:
    case TokenKind::KwEnum:
    case TokenKind::KwVector:
    case TokenKind::KwReturn:
    case TokenKind::KwPass:
    case TokenKind::KwNext:
    case TokenKind::KwIf:
    case TokenKind::KwElse:
    case TokenKind::KwWhile:
    case TokenKind::KwFor:
    case TokenKind::KwIn:
    case TokenKind::KwLoop:
    case TokenKind::KwSwitch:
    case TokenKind::KwCase:
    case TokenKind::KwDefault:
    case TokenKind::KwFallthrough:
    case TokenKind::KwBreak:
    case TokenKind::KwContinue:
    case TokenKind::KwAs:
    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
    case TokenKind::KwNull:
    case TokenKind::KwSizeof:
    case TokenKind::KwAlignof:
        return true;
    default:
        return false;
    }
}

bool isTypeNameKeyword(std::string_view word) {
    for (const std::string_view name : kTypeNames) {
        if (name == word) {
            return true;
        }
    }

    // `int8`, `int127`, `float16`, `double52.11`, ... : a width / precision
    // suffix glued to the type keyword (spec §2.3).
    const auto matchesWidth = [&](std::string_view prefix) {
        if (word.rfind(prefix, 0) != 0) {
            return false;
        }
        std::string_view rest = word.substr(prefix.size());
        if (rest.empty()) {
            return false;
        }
        std::size_t index = 0;
        while (index < rest.size() && isDigit(rest[index])) {
            ++index;
        }
        if (index == 0) {
            return false;
        }
        if (index == rest.size()) {
            return true;
        }
        if (prefix != "double" || rest[index] != '.') {
            return false;
        }
        ++index;
        std::size_t digits = 0;
        while (index < rest.size() && isDigit(rest[index])) {
            ++index;
            ++digits;
        }
        return digits > 0 && index == rest.size();
    };

    return matchesWidth("int") || matchesWidth("float") || matchesWidth("double");
}

namespace {

/// `int8x` / `float32_t` must be a hard error instead of silently splitting
/// into a type name and an identifier (spec §2.3).
bool typeNameWithTrailingJunk(std::string_view word) {
    const std::string_view prefixes[] = {"int", "float", "double"};
    for (const std::string_view prefix : prefixes) {
        if (word.rfind(prefix, 0) != 0) {
            continue;
        }
        const std::string_view rest = word.substr(prefix.size());
        std::size_t index = 0;
        while (index < rest.size() && isDigit(rest[index])) {
            ++index;
        }
        if (index > 0 && index < rest.size()) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string decodeEscapes(std::string_view body) {
    std::string result;
    result.reserve(body.size());

    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c != '\\' || i + 1 >= body.size()) {
            result.push_back(c);
            continue;
        }

        const char esc = body[++i];
        switch (esc) {
        case 'n': result.push_back('\n'); break;
        case 't': result.push_back('\t'); break;
        case 'r': result.push_back('\r'); break;
        case '0': result.push_back('\0'); break;
        case '\\': result.push_back('\\'); break;
        case '"': result.push_back('"'); break;
        case '\'': result.push_back('\''); break;
        case 'x': {
            if (i + 2 < body.size() && isHexDigit(body[i + 1]) && isHexDigit(body[i + 2])) {
                const int value = hexValue(body[i + 1]) * 16 + hexValue(body[i + 2]);
                result.push_back(static_cast<char>(value));
                i += 2;
            } else {
                result.push_back('\\');
                result.push_back(esc);
            }
            break;
        }
        case 'u': {
            if (i + 4 < body.size() && isHexDigit(body[i + 1]) && isHexDigit(body[i + 2]) &&
                isHexDigit(body[i + 3]) && isHexDigit(body[i + 4])) {
                unsigned value = 0;
                for (int k = 1; k <= 4; ++k) {
                    value = value * 16 + static_cast<unsigned>(hexValue(body[i + static_cast<std::size_t>(k)]));
                }
                // Encoded as UTF-16 code units in a char literal context; for
                // strings we store the code unit as two UTF-8 bytes when it is
                // representable, otherwise as a 3 byte sequence.
                if (value < 0x80) {
                    result.push_back(static_cast<char>(value));
                } else if (value < 0x800) {
                    result.push_back(static_cast<char>(0xC0 | (value >> 6)));
                    result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                } else {
                    result.push_back(static_cast<char>(0xE0 | (value >> 12)));
                    result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                }
                i += 4;
            } else {
                result.push_back('\\');
                result.push_back(esc);
            }
            break;
        }
        default:
            result.push_back('\\');
            result.push_back(esc);
            break;
        }
    }
    return result;
}

Lexer::Lexer(SourceManager& sources, FileID file, DiagnosticEngine& diagnostics)
    : m_sources(sources), m_diagnostics(diagnostics), m_file(file),
      m_source(sources.text(file)) {}

bool Lexer::atEnd() const { return m_pos >= m_source.size(); }

char Lexer::peek(std::size_t ahead) const {
    const std::size_t index = m_pos + ahead;
    return index < m_source.size() ? m_source[index] : '\0';
}

char Lexer::advance() {
    if (atEnd()) {
        return '\0';
    }
    return m_source[m_pos++];
}

bool Lexer::match(char expected) {
    if (peek() != expected) {
        return false;
    }
    ++m_pos;
    return true;
}

void Lexer::skipTrivia() {
    while (!atEnd()) {
        const char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            ++m_pos;
            continue;
        }
        if (c == '/' && peek(1) == '/') {
            while (!atEnd() && peek() != '\n') {
                ++m_pos;
            }
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            const std::uint32_t start = m_pos;
            m_pos += 2;
            bool closed = false;
            while (!atEnd()) {
                if (peek() == '*' && peek(1) == '/') {
                    m_pos += 2;
                    closed = true;
                    break;
                }
                ++m_pos;
            }
            if (!closed) {
                m_diagnostics.error(SourceRange{SourceLocation{m_file, start},
                                                SourceLocation{m_file, static_cast<std::uint32_t>(m_source.size())}},
                                    "unterminated block comment");
            }
            continue;
        }
        break;
    }
}

Token Lexer::makeToken(TokenKind kind, std::uint32_t start) const {
    Token token;
    token.kind = kind;
    token.range.begin = SourceLocation{m_file, start};
    token.range.end = SourceLocation{m_file, static_cast<std::uint32_t>(m_pos)};
    token.text = m_source.substr(start, m_pos - start);
    return token;
}

bool Lexer::boundaryAfterSuffix(std::uint32_t start) {
    if (!isIdentContinue(peek())) {
        return true;
    }
    // `int8x` / `100i8x` must not be silently split.
    while (isIdentContinue(peek())) {
        ++m_pos;
    }
    const std::string word(m_source.substr(start, m_pos - start));
    m_diagnostics.error(SourceRange{SourceLocation{m_file, start},
                                    SourceLocation{m_file, static_cast<std::uint32_t>(m_pos)}},
                        "expected a delimiter after '" + word +
                            "'; a type name or literal suffix must not be followed by an identifier character");
    return false;
}

Token Lexer::lexIdentifier(std::uint32_t start) {
    while (isIdentContinue(peek())) {
        ++m_pos;
    }
    const std::string_view word = m_source.substr(start, m_pos - start);

    for (const KeywordEntry& entry : kKeywords) {
        if (entry.word == word) {
            return makeToken(entry.kind, start);
        }
    }

    if (isTypeNameKeyword(word)) {
        // `double52.11`: absorb the fractional part of the precision suffix.
        if (word.rfind("double", 0) == 0 && word.size() > 6 && peek() == '.' &&
            isDigit(peek(1))) {
            ++m_pos;
            while (isDigit(peek())) {
                ++m_pos;
            }
        }
        return makeToken(TokenKind::TypeName, start);
    }

    if (typeNameWithTrailingJunk(word)) {
        m_diagnostics.error(
            SourceRange{SourceLocation{m_file, start},
                        SourceLocation{m_file, static_cast<std::uint32_t>(m_pos)}},
            "expected a delimiter after the type name '" + std::string(word) +
                "'; a type name must not be followed by an identifier character");
        return makeToken(TokenKind::TypeName, start);
    }

    return makeToken(TokenKind::Ident, start);
}

Token Lexer::lexNumber(std::uint32_t start) {
    bool isFloat = false;
    bool isHex = false;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        isHex = true;
        m_pos += 2;
        const std::uint32_t digitsStart = m_pos;
        while (isHexDigit(peek()) || peek() == '_') {
            // `_` must sit between digits.
            if (peek() == '_' && (!isHexDigit(peek(1)) || m_pos == digitsStart)) {
                break;
            }
            ++m_pos;
        }
        if (m_pos == digitsStart) {
            m_diagnostics.error(SourceRange{SourceLocation{m_file, start},
                                            SourceLocation{m_file, static_cast<std::uint32_t>(m_pos)}},
                                "expected hexadecimal digits after '0x'");
            return makeToken(TokenKind::Invalid, start);
        }
    } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        m_pos += 2;
        const std::uint32_t digitsStart = m_pos;
        while (isBinDigit(peek()) || peek() == '_') {
            if (peek() == '_' && (!isBinDigit(peek(1)) || m_pos == digitsStart)) {
                break;
            }
            ++m_pos;
        }
        if (m_pos == digitsStart) {
            m_diagnostics.error(SourceRange{SourceLocation{m_file, start},
                                            SourceLocation{m_file, static_cast<std::uint32_t>(m_pos)}},
                                "expected binary digits after '0b'");
            return makeToken(TokenKind::Invalid, start);
        }
    } else {
        while (isDigit(peek()) || peek() == '_') {
            if (peek() == '_' && !isDigit(peek(1))) {
                break;
            }
            ++m_pos;
        }
        if (peek() == '.' && isDigit(peek(1))) {
            isFloat = true;
            ++m_pos;
            while (isDigit(peek()) || peek() == '_') {
                if (peek() == '_' && !isDigit(peek(1))) {
                    break;
                }
                ++m_pos;
            }
        } else if (peek() == '.' && !isDigit(peek(1)) && peek(1) != '.') {
            // `1.` is a valid float literal (spec §2.4.1).
            isFloat = true;
            ++m_pos;
        }
        if (peek() == 'e' || peek() == 'E') {
            const char next = peek(1);
            if (isDigit(next) || ((next == '+' || next == '-') && isDigit(peek(2)))) {
                isFloat = true;
                ++m_pos;
                if (peek() == '+' || peek() == '-') {
                    ++m_pos;
                }
                while (isDigit(peek()) || peek() == '_') {
                    if (peek() == '_' && !isDigit(peek(1))) {
                        break;
                    }
                    ++m_pos;
                }
            }
        }
    }

    // Optional literal type suffix: iN / uN / fN / d (spec §2.4.1).
    // For hex and binary literals only i/u can start a suffix, since 'f'/'d'
    // are valid hex digits.
    const char c = peek();
    const bool suffixStart = isHex ? (c == 'i' || c == 'u')
                                   : (c == 'i' || c == 'u' || c == 'f' || c == 'd');
    if (suffixStart) {
        if (c == 'd') {
            ++m_pos; // bare `d`
        } else {
            ++m_pos;
            const std::uint32_t digitsStart = m_pos;
            while (isDigit(peek())) {
                ++m_pos;
            }
            if (m_pos == digitsStart) {
                m_diagnostics.error(SourceRange{SourceLocation{m_file, start},
                                                SourceLocation{m_file, static_cast<std::uint32_t>(m_pos)}},
                                    "literal suffix '" + std::string(1, c) +
                                        "' requires a width, e.g. '" + std::string(1, c) + "8'");
            }
        }
        boundaryAfterSuffix(start);
    }

    return makeToken(isFloat ? TokenKind::FloatLit : TokenKind::IntLit, start);
}

Token Lexer::lexChar(std::uint32_t start) {
    ++m_pos; // opening quote
    std::string body;

    if (atEnd()) {
        m_diagnostics.error(SourceRange{SourceLocation{m_file, start},
                                        SourceLocation{m_file, static_cast<std::uint32_t>(m_pos)}},
                            "unterminated character literal");
        return makeToken(TokenKind::Invalid, start);
    }

    if (peek() == '\\') {
        body.push_back(advance());
        if (!atEnd()) {
            body.push_back(advance());
            if ((body.back() == 'x' || body.back() == 'u') && !atEnd()) {
                const int extra = body.back() == 'x' ? 2 : 4;
                for (int i = 0; i < extra && !atEnd() && peek() != '\''; ++i) {
                    body.push_back(advance());
                }
            }
        }
    } else {
        body.push_back(advance());
    }

    if (!match('\'')) {
        m_diagnostics.error(SourceRange{SourceLocation{m_file, start},
                                        SourceLocation{m_file, static_cast<std::uint32_t>(m_pos)}},
                            "unterminated character literal; a char holds exactly one UTF-16 code unit");
        return makeToken(TokenKind::Invalid, start);
    }

    // A char literal is either one raw character or one escape sequence.
    const bool singleEscape = body.size() >= 2 && body[0] == '\\';
    if (body.empty() || (body.size() != 1 && !singleEscape)) {
        m_diagnostics.error(SourceRange{SourceLocation{m_file, start},
                                        SourceLocation{m_file, static_cast<std::uint32_t>(m_pos)}},
                            "a character literal must contain exactly one code unit");
    }
    return makeToken(TokenKind::CharLit, start);
}

Token Lexer::lexString(std::uint32_t start, bool raw, bool multiline) {
    const std::size_t quoteCount = multiline ? 3 : 1;
    m_pos += static_cast<std::uint32_t>(quoteCount);

    bool terminated = false;
    while (!atEnd()) {
        if (multiline) {
            if (peek() == '"' && peek(1) == '"' && peek(2) == '"') {
                m_pos += 3;
                terminated = true;
                break;
            }
            ++m_pos;
            continue;
        }
        const char c = peek();
        if (c == '"') {
            ++m_pos;
            terminated = true;
            break;
        }
        if (c == '\n') {
            break;
        }
        if (!raw && c == '\\') {
            ++m_pos;
            if (atEnd()) {
                break;
            }
            const char esc = peek();
            if (esc == 'x') {
                m_pos += 3;
            } else if (esc == 'u') {
                m_pos += 5;
            } else {
                ++m_pos;
            }
            continue;
        }
        ++m_pos;
    }

    if (!terminated) {
        m_diagnostics.error(SourceRange{SourceLocation{m_file, start},
                                        SourceLocation{m_file, static_cast<std::uint32_t>(m_pos)}},
                            "unterminated string literal");
        return makeToken(TokenKind::Invalid, start);
    }
    return makeToken(TokenKind::StringLit, start);
}

Token Lexer::lexOperator(std::uint32_t start) {
    const char c = advance();
    switch (c) {
    case '+':
        if (match('+')) return makeToken(TokenKind::PlusPlus, start);
        if (match('=')) return makeToken(TokenKind::PlusAssign, start);
        return makeToken(TokenKind::Plus, start);
    case '-':
        if (match('-')) return makeToken(TokenKind::MinusMinus, start);
        if (match('=')) return makeToken(TokenKind::MinusAssign, start);
        if (match('>')) return makeToken(TokenKind::Arrow, start);
        return makeToken(TokenKind::Minus, start);
    case '*':
        if (match('=')) return makeToken(TokenKind::StarAssign, start);
        return makeToken(TokenKind::Star, start);
    case '/':
        if (match('=')) return makeToken(TokenKind::SlashAssign, start);
        return makeToken(TokenKind::Slash, start);
    case '%':
        if (match('=')) return makeToken(TokenKind::PercentAssign, start);
        return makeToken(TokenKind::Percent, start);
    case '=':
        if (match('=')) return makeToken(TokenKind::EqualEqual, start);
        return makeToken(TokenKind::Assign, start);
    case '!':
        if (match('=')) return makeToken(TokenKind::BangEqual, start);
        return makeToken(TokenKind::Bang, start);
    case '<':
        if (match('<')) return match('=') ? makeToken(TokenKind::ShlAssign, start)
                                          : makeToken(TokenKind::Shl, start);
        if (match('=')) return makeToken(TokenKind::LessEqual, start);
        return makeToken(TokenKind::Less, start);
    case '>':
        if (match('>')) return match('=') ? makeToken(TokenKind::ShrAssign, start)
                                          : makeToken(TokenKind::Shr, start);
        if (match('=')) return makeToken(TokenKind::GreaterEqual, start);
        return makeToken(TokenKind::Greater, start);
    case '&':
        if (match('&')) return makeToken(TokenKind::AmpAmp, start);
        if (match('=')) return makeToken(TokenKind::AmpAssign, start);
        return makeToken(TokenKind::Amp, start);
    case '|':
        if (match('|')) return makeToken(TokenKind::PipePipe, start);
        if (match('=')) return makeToken(TokenKind::PipeAssign, start);
        return makeToken(TokenKind::Pipe, start);
    case '^':
        if (match('=')) return makeToken(TokenKind::CaretAssign, start);
        return makeToken(TokenKind::Caret, start);
    case '~': return makeToken(TokenKind::Tilde, start);
    case '@': return makeToken(TokenKind::At, start);
    case '#': return makeToken(TokenKind::Hash, start);
    case '.':
        if (match('.')) return makeToken(TokenKind::DotDot, start);
        return makeToken(TokenKind::Dot, start);
    case '?': return makeToken(TokenKind::Question, start);
    case ':': return makeToken(TokenKind::Colon, start);
    case ',': return makeToken(TokenKind::Comma, start);
    case ';': return makeToken(TokenKind::Semicolon, start);
    case '(': return makeToken(TokenKind::LParen, start);
    case ')': return makeToken(TokenKind::RParen, start);
    case '[': return makeToken(TokenKind::LBracket, start);
    case ']': return makeToken(TokenKind::RBracket, start);
    case '{': return makeToken(TokenKind::LBrace, start);
    case '}': return makeToken(TokenKind::RBrace, start);
    default:
        break;
    }

    m_diagnostics.error(SourceRange{SourceLocation{m_file, start},
                                    SourceLocation{m_file, static_cast<std::uint32_t>(m_pos)}},
                        std::string("unexpected character '") + c + "'");
    return makeToken(TokenKind::Invalid, start);
}

Token Lexer::next() {
    skipTrivia();
    if (atEnd()) {
        Token token;
        token.kind = TokenKind::EndOfFile;
        token.range.begin = SourceLocation{m_file, static_cast<std::uint32_t>(m_source.size())};
        token.range.end = token.range.begin;
        return token;
    }

    const std::uint32_t start = m_pos;
    const char c = peek();

    // Raw and multi-line strings (spec §2.4.1 / Q56).
    if (c == 'r' && peek(1) == '"') {
        ++m_pos;
        return lexString(start, true, false);
    }
    if (c == '"' && peek(1) == '"' && peek(2) == '"') {
        return lexString(start, false, true);
    }

    if (isIdentStart(c)) {
        return lexIdentifier(start);
    }
    if (isDigit(c)) {
        return lexNumber(start);
    }
    if (c == '.') {
        // `.5` starts a float literal.
        if (isDigit(peek(1))) {
            ++m_pos;
            while (isDigit(peek()) || peek() == '_') {
                if (peek() == '_' && !isDigit(peek(1))) {
                    break;
                }
                ++m_pos;
            }
            if (peek() == 'e' || peek() == 'E') {
                if (isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDigit(peek(2)))) {
                    ++m_pos;
                    if (peek() == '+' || peek() == '-') {
                        ++m_pos;
                    }
                    while (isDigit(peek())) {
                        ++m_pos;
                    }
                }
            }
            if (peek() == 'f' || peek() == 'd') {
                if (peek() == 'd') {
                    ++m_pos;
                } else {
                    ++m_pos;
                    while (isDigit(peek())) {
                        ++m_pos;
                    }
                }
                boundaryAfterSuffix(start);
            }
            return makeToken(TokenKind::FloatLit, start);
        }
    }
    if (c == '\'') {
        return lexChar(start);
    }
    if (c == '"') {
        return lexString(start, false, false);
    }
    return lexOperator(start);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    tokens.reserve(m_source.size() / 4 + 8);
    while (true) {
        Token token = next();
        const bool done = token.kind == TokenKind::EndOfFile;
        tokens.push_back(token);
        if (done) {
            break;
        }
    }
    return tokens;
}

} // namespace acvm
