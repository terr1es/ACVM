//===--- test_lexer.cpp --------------------------------------------------===//
#include "TestHarness.hpp"
#include "TestSupport.hpp"

using acvm::TokenKind;

TEST(scans_a_minimal_function) {
    const auto result = testsupport::lexSource("function main():void { pass; }");
    CHECK(result.diagnostics.empty());
    CHECK(result.tokens[0].kind == TokenKind::KwFunction);
    CHECK(result.tokens[1].kind == TokenKind::Ident);
    CHECK(result.tokens[2].kind == TokenKind::LParen);
    CHECK(result.tokens[3].kind == TokenKind::RParen);
    CHECK(result.tokens[4].kind == TokenKind::Colon);
    CHECK(result.tokens[5].kind == TokenKind::TypeName);
    CHECK(result.tokens[6].kind == TokenKind::LBrace);
}

TEST(type_names_include_their_width) {
    const auto result = testsupport::lexSource("int8 float16 double52.11 char bool void string int127");
    CHECK(result.diagnostics.empty());
    CHECK_EQ(result.tokens.size(), std::size_t(9));
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(result.tokens[i].kind == TokenKind::TypeName);
    }
    CHECK_EQ(std::string(result.tokens[0].text), std::string("int8"));
    CHECK_EQ(std::string(result.tokens[2].text), std::string("double52.11"));
    CHECK_EQ(std::string(result.tokens[7].text), std::string("int127"));
}

TEST(type_name_followed_by_identifier_is_rejected) {
    const auto result = testsupport::lexSource("int8x");
    CHECK(!result.diagnostics.empty());
    CHECK(result.diagnostics.find("delimiter") != std::string::npos);
}

TEST(literals_and_suffixes) {
    const auto result =
        testsupport::lexSource("123 1_000 0xFF 0b1010 1.5 .5 1e10 100i8 200u8 1.5f32 1.5d");
    CHECK(result.diagnostics.empty());
    CHECK(result.tokens[0].kind == TokenKind::IntLit);
    CHECK(result.tokens[1].kind == TokenKind::IntLit);
    CHECK(result.tokens[2].kind == TokenKind::IntLit);
    CHECK(result.tokens[3].kind == TokenKind::IntLit);
    CHECK(result.tokens[4].kind == TokenKind::FloatLit);
    CHECK(result.tokens[5].kind == TokenKind::FloatLit);
    CHECK(result.tokens[6].kind == TokenKind::FloatLit);
    CHECK(result.tokens[7].kind == TokenKind::IntLit);
    CHECK_EQ(std::string(result.tokens[7].text), std::string("100i8"));
    CHECK(result.tokens[9].kind == TokenKind::FloatLit);
    CHECK(result.tokens[10].kind == TokenKind::FloatLit);
}

TEST(character_and_string_escapes) {
    const auto result = testsupport::lexSource("'a' '\\n' '\\u4e2d' \"a\\nb\" r\"raw\\n\" \"\"\"block\"\"\"");
    CHECK(result.diagnostics.empty());
    CHECK(result.tokens[0].kind == TokenKind::CharLit);
    CHECK(result.tokens[1].kind == TokenKind::CharLit);
    CHECK(result.tokens[2].kind == TokenKind::CharLit);
    CHECK(result.tokens[3].kind == TokenKind::StringLit);
    CHECK(result.tokens[4].kind == TokenKind::StringLit);
    CHECK(result.tokens[5].kind == TokenKind::StringLit);
    CHECK_EQ(acvm::decodeEscapes("a\\nb"), std::string("a\nb"));
    CHECK_EQ(acvm::decodeEscapes("\\x41"), std::string("A"));
}

TEST(at_and_hash_are_operators) {
    const auto result = testsupport::lexSource("@p #x &|");
    CHECK(result.tokens[0].kind == TokenKind::At);
    CHECK(result.tokens[1].kind == TokenKind::Ident);
    CHECK(result.tokens[2].kind == TokenKind::Hash);
    CHECK(result.tokens[3].kind == TokenKind::Ident);
    CHECK(result.tokens[4].kind == TokenKind::Amp);
    CHECK(result.tokens[5].kind == TokenKind::Pipe);
}

TEST(operator_longest_match) {
    const auto result = testsupport::lexSource("<<= >>= <- ++ -- -> .. <= >= == !=");
    CHECK(result.tokens[0].kind == TokenKind::ShlAssign);
    CHECK(result.tokens[1].kind == TokenKind::ShrAssign);
    CHECK(result.tokens[2].kind == TokenKind::Less);
    CHECK(result.tokens[3].kind == TokenKind::Minus);
    CHECK(result.tokens[4].kind == TokenKind::PlusPlus);
    CHECK(result.tokens[5].kind == TokenKind::MinusMinus);
    CHECK(result.tokens[6].kind == TokenKind::Arrow);
    CHECK(result.tokens[7].kind == TokenKind::DotDot);
}

TEST(comments_are_skipped) {
    const auto result = testsupport::lexSource("// line\n/* block */ 1");
    CHECK(result.diagnostics.empty());
    CHECK_EQ(result.tokens.size(), std::size_t(2));
}

TEST(block_comments_do_not_nest) {
    const auto result = testsupport::lexSource("/* a /* b */ 1");
    CHECK(result.diagnostics.empty());
    CHECK_EQ(result.tokens.size(), std::size_t(2));
}

TEST(unterminated_constructs_are_reported) {
    CHECK(!testsupport::lexSource("\"oops").diagnostics.empty());
    CHECK(!testsupport::lexSource("/* oops").diagnostics.empty());
    CHECK(!testsupport::lexSource("'ab'").diagnostics.empty());
}

TEST(tracks_line_and_column) {
    const auto result = testsupport::lexSource("int x;\n  int y;");
    const auto position = result.sources->lineColumn(result.tokens[3].range.begin);
    CHECK_EQ(position.line, 2u);
    CHECK_EQ(position.column, 3u);
}

TEST(line_text_strips_the_terminator_of_the_last_line) {
    // A file that ends with a newline: the final line must not carry it, or
    // every diagnostic on the last line would render an extra blank line.
    const auto lf = testsupport::lexSource("int a;\nint b;\n");
    const acvm::FileID file = lf.tokens[0].range.begin.file;
    CHECK_EQ(std::string(lf.sources->lineText(file, 2)), std::string("int b;"));

    const auto crlf = testsupport::lexSource("int a;\r\nint b;\r\n");
    const acvm::FileID crlfFile = crlf.tokens[0].range.begin.file;
    CHECK_EQ(std::string(crlf.sources->lineText(crlfFile, 1)), std::string("int a;"));
    CHECK_EQ(std::string(crlf.sources->lineText(crlfFile, 2)), std::string("int b;"));

    // No trailing newline at all.
    const auto bare = testsupport::lexSource("int a;\nint b;");
    const acvm::FileID bareFile = bare.tokens[0].range.begin.file;
    CHECK_EQ(std::string(bare.sources->lineText(bareFile, 2)), std::string("int b;"));
}

TEST_MAIN()

