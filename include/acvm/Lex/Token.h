//===--- Token.h - Token kinds, keywords and operator spelling ----------===//
#pragma once

#include "acvm/Common/SourceLocation.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace acvm {

/// Every token kind produced by the lexer (spec §1).
enum class TokenKind : std::uint8_t {
    EndOfFile,
    Invalid,

    Ident,
    /// A builtin scalar type name, including its width suffix:
    /// `int`, `int8`, `float16`, `double16.4`, `char`, `bool`, `void`, `string`.
    TypeName,

    IntLit,
    FloatLit,
    CharLit,
    StringLit,

    // keywords
    KwUnsigned,
    KwConst,
    KwComptime,
    KwGlobal,
    KwFunction,
    KwImport,
    KwLibrary,
    /// `module "name" version "x.y.z";` — package identity of the file itself.
    /// A property of the unit, like `KwLibrary`, not a declaration.
    KwModule,
    KwPrivate,
    KwEvent,
    KwProcess,
    KwIsCf,
    KwEof,
    KwVirtual,
    KwArenaScope,
    KwStruct,
    KwEnum,
    KwUnion,
    KwVector,
    KwReturn,
    KwPass,
    KwNext,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwIn,
    KwLoop,
    KwSwitch,
    KwCase,
    KwDefault,
    KwFallthrough,
    KwBreak,
    KwContinue,
    KwAs,
    KwTrue,
    KwFalse,
    KwNull,
    KwSizeof,
    KwAlignof,

    // operators
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    PlusPlus,
    MinusMinus,
    Assign,
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign,
    PercentAssign,
    AmpAssign,
    PipeAssign,
    CaretAssign,
    ShlAssign,
    ShrAssign,
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    AmpAmp,
    PipePipe,
    Bang,
    Tilde,
    Amp,
    Pipe,
    Caret,
    Shl,
    Shr,
    At,
    Hash,
    Dot,
    Arrow,
    DotDot,
    Question,
    Colon,
    Comma,
    Semicolon,
    LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
};

std::string_view tokenKindName(TokenKind kind);
std::string_view tokenKindSpelling(TokenKind kind);
bool isKeywordKind(TokenKind kind);

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    SourceRange range{};

    /// Raw lexeme; points into the SourceManager buffer.
    std::string_view text{};

    [[nodiscard]] bool is(TokenKind expected) const { return kind == expected; }
    [[nodiscard]] std::string str() const { return std::string(text); }
};

/// True for the names that the lexer classifies as builtin type names.
bool isTypeNameKeyword(std::string_view word);

/// Decoded value of a character or string literal body (quotes excluded).
/// `\xNN` and `\uNNNN` are decoded; unknown escapes are kept verbatim and the
/// lexer has already reported them.
std::string decodeEscapes(std::string_view body);

} // namespace acvm
