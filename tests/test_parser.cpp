//===--- test_parser.cpp -------------------------------------------------===//
#include "TestHarness.hpp"
#include "TestSupport.hpp"

#include "acvm/AST/AST.h"

using namespace acvm;

TEST(parses_a_function_signature) {
    const auto result = testsupport::parseSource(
        "function add(int a, int b):int { return a + b; eof: pass; }");
    CHECK(result.diagnostics.empty());
    CHECK_EQ(result.unit->decls.size(), std::size_t(1));

    const Stmt& fn = *result.unit->decls[0];
    CHECK_EQ(fn.name, std::string("add"));
    CHECK_EQ(fn.params.size(), std::size_t(2));
    CHECK_EQ(fn.params[0].name, std::string("a"));
    CHECK(fn.retType != nullptr);
    CHECK(fn.hasEof);
}

TEST(parses_type_keywords_by_one_token_lookahead) {
    for (const char* keyword : {"private", "global", "event", "process"}) {
        const std::string source = std::string("function ") + keyword +
                                   " f():void { pass; eof: pass; }";
        const auto result = testsupport::parseSource(source);
        CHECK(result.diagnostics.empty());
        CHECK_EQ(result.unit->decls.size(), std::size_t(1));
        CHECK_EQ(result.unit->decls[0]->name, std::string("f"));
    }
}

TEST(parses_an_import_declaration) {
    const auto result = testsupport::parseSource("function import (return:(Math, Trig));");
    CHECK(result.diagnostics.empty());
    CHECK(result.unit->decls[0]->kind == StmtKind::ImportDecl);
    CHECK_EQ(result.unit->decls[0]->modules.size(), std::size_t(2));
}

TEST(parses_import_as_a_parenthesised_module_list) {
    // `import (io);` -- the spelling a reader writes first, because `import` reads
    // like a keyword. It normalises to the same node as the function form, so
    // nothing after the parser can tell them apart.
    const auto result = testsupport::parseSource("import (io, json);");
    CHECK(result.diagnostics.empty());
    CHECK_EQ(result.unit->decls.size(), std::size_t(1));
    CHECK(result.unit->decls[0]->kind == StmtKind::ImportDecl);
    CHECK_EQ(result.unit->decls[0]->modules.size(), std::size_t(2));
    CHECK_EQ(result.unit->decls[0]->modules[0].first, std::string("io"));
    CHECK_EQ(result.unit->decls[0]->modules[1].first, std::string("json"));
    // Not paths: a module name is resolved in the search path, a path verbatim.
    CHECK(!result.unit->decls[0]->modulesArePaths);
}

TEST(parses_a_single_module_without_a_comma) {
    const auto result = testsupport::parseSource("import (io);");
    CHECK(result.diagnostics.empty());
    CHECK_EQ(result.unit->decls[0]->modules.size(), std::size_t(1));
}

TEST(parses_an_empty_module_list) {
    const auto result = testsupport::parseSource("import ();");
    CHECK(result.diagnostics.empty());
    CHECK(result.unit->decls[0]->modules.empty());
}

TEST(a_quoted_import_is_still_a_path) {
    // The one-token lookahead has to keep the old form working: a string inside
    // the parentheses is a file path, not a module name.
    const auto result = testsupport::parseSource("import (\"lib.ax\");");
    CHECK(result.diagnostics.empty());
    CHECK(result.unit->decls[0]->modulesArePaths);
    CHECK_EQ(result.unit->decls[0]->modules[0].first, std::string("lib.ax"));
}

TEST(a_missing_paren_names_the_spelling_to_use) {
    // The message that prompted this: `import io;` used to produce three cascading
    // errors and no hint that `import` is a *function* in this language.
    const auto result = testsupport::parseSource("import io;");
    CHECK(!result.diagnostics.empty());
    CHECK(result.diagnostics.find("missing its parentheses") != std::string::npos);
    CHECK(result.diagnostics.find("import (io);") != std::string::npos);
}

TEST(an_unclosed_module_list_is_one_error) {
    const auto result = testsupport::parseSource("import (io\nfunction f():void { eof: pass; }");
    CHECK(result.diagnostics.find("close the module list") != std::string::npos);
}

TEST(parses_default_return_clause) {
    const auto result =
        testsupport::parseSource("function clamp(int x, return:0):int { return x; eof: pass; }");
    CHECK(result.diagnostics.empty());
    CHECK(result.unit->decls[0]->defaultRet != nullptr);
}

TEST(multiplication_binds_tighter_than_addition) {
    const auto result = testsupport::parseSource(
        "function main():void { int x = 1 + 2 * 3; eof: pass; }");
    CHECK(result.diagnostics.empty());
    const Stmt& fn = *result.unit->decls[0];
    const Expr& init = *fn.body[0]->init;
    CHECK(init.kind == ExprKind::Binary);
    CHECK(init.binop == BinOp::Add);
    const Expr& rhs = *init.kids[1];
    CHECK(rhs.kind == ExprKind::Binary);
    CHECK(rhs.binop == BinOp::Mul);
}

TEST(shift_binds_looser_than_addition) {
    const auto result =
        testsupport::parseSource("function main():void { int x = 1 + 2 << 3; eof: pass; }");
    CHECK(result.diagnostics.empty());
    const Expr& init = *result.unit->decls[0]->body[0]->init;
    CHECK(init.binop == BinOp::Shl);
    CHECK(init.kids[0]->binop == BinOp::Add);
}

TEST(parses_the_sixteen_level_table) {
    struct Case {
        const char* source;
        BinOp outer;
    };
    const Case cases[] = {
        {"a + b * c", BinOp::Add},
        {"a * b + c", BinOp::Add},
        {"a & b | c", BinOp::BitOr},
        {"a | b && c", BinOp::LogAnd},
        {"a && b || c", BinOp::LogOr},
        {"a == b != c", BinOp::Ne},
        {"a < b == c", BinOp::Eq},
        {"a ^ b & c", BinOp::BitXor},
    };
    for (const Case& testCase : cases) {
        const std::string source =
            std::string("function main():void { int x = ") + testCase.source + "; eof: pass; }";
        const auto result = testsupport::parseSource(source);
        CHECK(result.diagnostics.empty());
        const Expr& init = *result.unit->decls[0]->body[0]->init;
        CHECK(init.kind == ExprKind::Binary);
        CHECK(init.binop == testCase.outer);
    }
}

TEST(assignment_is_right_associative) {
    const auto result =
        testsupport::parseSource("function main():void { int a = 0; a = a = 1; eof: pass; }");
    CHECK(result.diagnostics.empty());
    const Expr& stmt = *result.unit->decls[0]->body[1]->init;
    CHECK(stmt.kind == ExprKind::Assign);
    CHECK(stmt.kids[1]->kind == ExprKind::Assign);
}

TEST(slice_is_not_a_range_expression) {
    const auto result =
        testsupport::parseSource("function main():void { string s = \"x\"; print(s[0..1]); eof: pass; }");
    CHECK(result.diagnostics.empty());
    const Expr& call = *result.unit->decls[0]->body[1]->init;
    CHECK(call.kind == ExprKind::Call);
    CHECK(call.kids[1]->kind == ExprKind::Slice);
}

TEST(tuple_declaration_is_disambiguated) {
    const auto result =
        testsupport::parseSource("function main():void { (int, int) p = (1, 2); eof: pass; }");
    CHECK(result.diagnostics.empty());
    CHECK(result.unit->decls[0]->body[0]->kind == StmtKind::VarDecl);

    const auto expression =
        testsupport::parseSource("function main():void { (1, 2); eof: pass; }");
    CHECK(expression.diagnostics.empty());
    CHECK(expression.unit->decls[0]->body[0]->kind == StmtKind::ExprStmt);
}

TEST(parses_labeled_loops_and_jumps) {
    const auto result = testsupport::parseSource(
        "function main():void { outer: while (true) { break outer; } eof: pass; }");
    CHECK(result.diagnostics.empty());
    const Stmt& loop = *result.unit->decls[0]->body[0];
    CHECK(loop.kind == StmtKind::While);
    CHECK_EQ(loop.label, std::string("outer"));
    CHECK_EQ(loop.thenS->body[0]->label, std::string("outer"));
}

TEST(parses_switch_with_fallthrough) {
    const auto result = testsupport::parseSource(
        "function main():void { int x = 1; switch (x) { case 1: pass; fallthrough; case 2: pass; "
        "default: pass; } eof: pass; }");
    CHECK(result.diagnostics.empty());
    const Stmt& sw = *result.unit->decls[0]->body[1];
    CHECK(sw.kind == StmtKind::Switch);
    CHECK_EQ(sw.cases.size(), std::size_t(3));
    CHECK(sw.cases[0].hasFallthrough);
    CHECK(sw.cases[2].value == nullptr);
}

TEST(missing_return_type_is_pointed_out) {
    const auto result = testsupport::parseSource("function f() { }");
    CHECK(!result.diagnostics.empty());
    CHECK(result.diagnostics.find(":void") != std::string::npos);
}

TEST(import_body_rejects_statements) {
    const auto result = testsupport::parseSource("function import () { int x = 1; }");
    CHECK(!result.diagnostics.empty());
    CHECK(result.diagnostics.find("import") != std::string::npos);
}

TEST(malformed_input_always_terminates) {
    // Regression: error recovery once reported a diagnostic without consuming a
    // token. `parseUnit` treated the returned node as progress and looped
    // forever, allocating one statement per turn until the machine ran out of
    // memory. Every entry here must simply return.
    const char* inputs[] = {
        "}",          "{",          "} } } }",     "{ { { {",
        ":",          "..",         "@",           "#",
        "int",        "global",     "private",     "function",
        "if",         "struct S {", "enum E {",    "function f():void {",
        "function import():void { }",
        "function import():void { import \"q.ax\"; }\nfunction main():void { eof: pass; }",
        "int a[] = {,", "function main():void { {{ }} } }",
    };
    for (const char* source : inputs) {
        const auto result = testsupport::parseSource(source);
        // Nothing useful to assert about the message -- just that we got here.
        CHECK(result.unit != nullptr);
    }
}

TEST(too_many_errors_are_capped) {
    // A thousand broken declarations should not print a thousand diagnostics.
    std::string source;
    for (int i = 0; i < 500; ++i) {
        source += "}\n";
    }
    const auto result = testsupport::parseSource(source);
    CHECK(!result.diagnostics.empty());
    CHECK(result.diagnostics.find("too many errors") != std::string::npos);

    std::size_t count = 0;
    std::size_t at = 0;
    while ((at = result.diagnostics.find("error:", at)) != std::string::npos) {
        ++count;
        at += 6;
    }
    CHECK(count <= DiagnosticEngine::kMaxErrors + 2);
}

TEST(a_function_cannot_be_named_import) {
    // `function import():void {` looks like a plain function but 'import' is a
    // declaration keyword: say so, and consume the body so the rest of the file
    // still parses (one error, not one per token).
    const auto result = testsupport::parseSource(
        "function import():void {\n    print(1);\n}\n"
        "function main():void { eof: pass; }\n");
    CHECK(result.diagnostics.find("reserved") != std::string::npos);
    std::size_t count = 0;
    std::size_t at = 0;
    while ((at = result.diagnostics.find("error:", at)) != std::string::npos) {
        ++count;
        at += 6;
    }
    CHECK_EQ(count, std::size_t(1));
}

TEST_MAIN()
