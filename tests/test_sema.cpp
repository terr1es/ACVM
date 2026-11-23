//===--- test_sema.cpp ---------------------------------------------------===//
#include "TestHarness.hpp"
#include "TestSupport.hpp"

TEST(accepts_a_valid_program) {
    const auto diagnostics = testsupport::checkSource(
        "function fib(int n):int {\n"
        "    if (n < 2) { return n; }\n"
        "    return fib(n - 1) + fib(n - 2);\n"
        "    eof: pass;\n"
        "}\n"
        "function main():void { print(fib(10)); eof: pass; }\n");
    CHECK(diagnostics.empty());
}

TEST(requires_main_with_void_return) {
    CHECK(testsupport::checkSource("function helper():void { eof: pass; }")
              .find("main") != std::string::npos);
    CHECK(testsupport::checkSource("function main():int { return 0; eof: pass; }")
              .find("main") != std::string::npos);
}

TEST(reports_undefined_names_with_a_suggestion) {
    const auto diagnostics = testsupport::checkSource(
        "function main():void { int count = 1; print(cuont); eof: pass; }");
    CHECK(diagnostics.find("undefined variable") != std::string::npos);
    CHECK(diagnostics.find("did you mean 'count'") != std::string::npos);
}

TEST(enforces_const_semantics) {
    CHECK(testsupport::checkSource(
              "function main():void { const int X = 1; X = 2; eof: pass; }")
              .find("cannot assign to const") != std::string::npos);
    CHECK(testsupport::checkSource("function main():void { const int X; eof: pass; }")
              .find("must be initialised") != std::string::npos);
    CHECK(testsupport::checkSource(
              "function f(const int x):void { x = 1; eof: pass; }\n"
              "function main():void { f(1); eof: pass; }")
              .find("cannot assign to const parameter") != std::string::npos);
    CHECK(testsupport::checkSource(
              "function main():void { const int X = 1; ptr int p = #X; eof: pass; }")
              .find("cannot take the address of const") != std::string::npos);
}

TEST(comptime_values_are_compile_time_constants) {
    CHECK(testsupport::checkSource(
              "function main():void { comptime int N = 4; int a[N]; eof: pass; }")
              .empty());
    CHECK(testsupport::checkSource(
              "function f():int { return 1; eof: pass; }\n"
              "function main():void { comptime int N = f(); eof: pass; }")
              .find("compile time constant") != std::string::npos);
    CHECK(testsupport::checkSource(
              "function main():void { const int N = 4; int a[N]; eof: pass; }")
              .find("comptime") != std::string::npos);
}

TEST(array_length_must_be_positive) {
    CHECK(testsupport::checkSource("function main():void { int a[0]; eof: pass; }")
              .find("greater than zero") != std::string::npos);
}

TEST(rejects_mixed_signedness) {
    const auto diagnostics = testsupport::checkSource(
        "function main():void { int8 a = 1; unsigned int8 b = 2; int x = a + b; eof: pass; }");
    CHECK(diagnostics.find("mixed signed and unsigned") != std::string::npos);
}

TEST(no_implicit_narrowing) {
    CHECK(testsupport::checkSource(
              "function main():void { int a = 1; int16 b = a; eof: pass; }")
              .find("cannot initialise") != std::string::npos);
    CHECK(testsupport::checkSource("function main():void { int8 a = 300; eof: pass; }")
              .find("out of range") != std::string::npos);
}

TEST(weak_enums_convert_both_ways) {
    CHECK(testsupport::checkSource(
              "enum Color { RED, GREEN };\n"
              "function main():void { Color c = 999; int n = c + 1; eof: pass; }")
              .empty());
}

TEST(overload_resolution_is_exact) {
    const char* twoOverloads =
        "function f(int x):int { return 0; eof: pass; }\n"
        "function f(int16 x):int { return 0; eof: pass; }\n";
    CHECK(testsupport::checkSource(std::string(twoOverloads) +
                                   "function main():void { print(f(1)); eof: pass; }")
              .empty());
    CHECK(testsupport::checkSource(std::string(twoOverloads) +
                                   "function main():void { print(f(1i16)); eof: pass; }")
              .empty());
    const auto ambiguous =
        testsupport::checkSource(std::string(twoOverloads) +
                                 "function main():void { int8 v = 1; print(f(v)); eof: pass; }");
    CHECK(ambiguous.find("no overload") != std::string::npos);
}

TEST(requires_return_on_every_path) {
    CHECK(testsupport::checkSource(
              "function f(int x):int { if (x > 0) { return 1; } eof: pass; }\n"
              "function main():void { print(f(1)); eof: pass; }")
              .find("not all paths") != std::string::npos);
    CHECK(testsupport::checkSource(
              "function f(int x):int { if (x > 0) { return 1; } return 0; eof: pass; }\n"
              "function main():void { print(f(1)); eof: pass; }")
              .empty());
    CHECK(testsupport::checkSource(
              "function f(int x, return:0):int { if (x > 0) { return 1; } eof: pass; }\n"
              "function main():void { print(f(1)); eof: pass; }")
              .empty());
}

TEST(void_functions_may_return_a_value) {
    CHECK(testsupport::checkSource(
              "function f():void { return 1 + 2; eof: pass; }\n"
              "function main():void { f(); eof: pass; }")
              .empty());
}

TEST(eof_rules) {
    CHECK(testsupport::checkSource(
              "function f():void { eof: return; }\n"
              "function main():void { f(); eof: pass; }")
              .find("not allowed inside the eof block") != std::string::npos);
    CHECK(testsupport::checkSource(
              "function f():void { eof: virtual { return; } }\n"
              "function main():void { f(); eof: pass; }")
              .find("virtual") != std::string::npos);
}

TEST(virtual_rules) {
    CHECK(testsupport::checkSource(
              "function f():int { virtual { return 1; } }\n"
              "function main():void { print(f()); eof: pass; }")
              .find("inside a branch") != std::string::npos);
    CHECK(testsupport::checkSource(
              "function f(int x):int { if (x > 0) { virtual { return 1; "
              "eof: pass; } } return 0; eof: pass; }\n"
              "function main():void { print(f(1)); eof: pass; }")
              .empty());
    CHECK(testsupport::checkSource(
              "function f(int x):int { if (x > 0) { virtual { virtual { return 1; } } } "
              "return 0; eof: pass; }\n"
              "function main():void { print(f(1)); eof: pass; }")
              .find("cannot be nested") != std::string::npos);
}

TEST(definite_assignment) {
    CHECK(testsupport::checkSource(
              "function main():void { int x; print(x); eof: pass; }")
              .find("may be used before initialisation") != std::string::npos);
    CHECK(testsupport::checkSource(
              "function main():void { int x; if (true) { x = 1; } else { x = 2; } print(x); "
              "eof: pass; }")
              .empty());
    CHECK(testsupport::checkSource(
              "function main():void { int x; if (true) { x = 1; } print(x); eof: pass; }")
              .find("may be used before initialisation") != std::string::npos);
}

TEST(process_and_next) {
    CHECK(testsupport::checkSource(
              "function process p():void { next; return; eof: pass; }\n"
              "function main():void { eof: pass; }")
              .empty());
    CHECK(testsupport::checkSource(
              "function main():void { next; eof: pass; }")
              .find("only allowed inside a 'process'") != std::string::npos);
    CHECK(testsupport::checkSource(
              "function process p():void { pass; eof: pass; }\n"
              "function main():void { eof: pass; }")
              .find("at least one reachable 'return'") != std::string::npos);
}

TEST(event_condition_cannot_read_parameters) {
    const auto diagnostics = testsupport::checkSource(
        "function event e(int x)(x > 0):void { pass; eof: pass; }\n"
        "function main():void { eof: pass; }");
    CHECK(diagnostics.find("cannot reference the parameter") != std::string::npos);
}

TEST(struct_rules) {
    CHECK(testsupport::checkSource(
              "struct Point { int x; int y; };\n"
              "function main():void { Point p = {1, 2}; print(p.x); eof: pass; }")
              .empty());
    CHECK(testsupport::checkSource(
              "struct Bad { int v; Bad link; };\n"
              "function main():void { eof: pass; }")
              .find("cannot contain itself") != std::string::npos);
    CHECK(testsupport::checkSource(
              "struct Node { int v; ptr Node link; };\n"
              "function main():void { Node n = {1, null}; eof: pass; }")
              .empty());
}

TEST(aggregate_initialisers_must_fit) {
    CHECK(testsupport::checkSource(
              "function main():void { int a[3] = {1, 2, 3, 4}; eof: pass; }")
              .find("too many initialisers") != std::string::npos);
    CHECK(testsupport::checkSource("function main():void { int a = {1}; eof: pass; }")
              .find("cannot initialise") != std::string::npos);
}

TEST(pointer_member_access_needs_arrow) {
    const auto diagnostics = testsupport::checkSource(
        "struct P { int x; };\n"
        "function main():void { P p = {1}; ptr P q = #p; print(q.x); eof: pass; }");
    CHECK(diagnostics.find("is a pointer") != std::string::npos);
}

TEST(warns_about_suspicious_conditions) {
    CHECK(testsupport::checkSource(
              "function main():void { int a = 0; if (a = 1) { pass; } eof: pass; }")
              .find("suspicious assignment") != std::string::npos);
    CHECK(testsupport::checkSource("function main():void { if (true); eof: pass; }")
              .find("empty statement") != std::string::npos);
}

TEST(switch_case_values_are_constants) {
    CHECK(testsupport::checkSource(
              "function main():void { int x = 1; switch (x) { case 1: pass; case 1: pass; } "
              "eof: pass; }")
              .find("duplicate 'case'") != std::string::npos);
    CHECK(testsupport::checkSource(
              "function main():void { int y = 1; switch (y) { case y: pass; } eof: pass; }")
              .find("compile time constant") != std::string::npos);
    CHECK(testsupport::checkSource(
              "function main():void { int x = 1; switch (x) { case 1: fallthrough; } eof: pass; }")
              .find("must not be the last statement") != std::string::npos);
}

TEST(vector_is_not_implemented) {
    CHECK(testsupport::checkSource("function main():void { vector int v; eof: pass; }")
              .find("not implemented") != std::string::npos);
}

TEST(entry_point_diagnostics_name_the_actual_problem) {
    // A wrong signature must not be reported as "no main".
    const auto wrongReturn = testsupport::checkSource(
        "function main():int { return 0; eof: pass; }");
    CHECK(wrongReturn.find("must return 'void'") != std::string::npos);
    CHECK(wrongReturn.find("no entry point") == std::string::npos);
    CHECK(wrongReturn.find("returns 'int32'") != std::string::npos);

    const auto withParams = testsupport::checkSource(
        "function main(int argc):void { eof: pass; }");
    CHECK(withParams.find("must not take parameters") != std::string::npos);

    const auto neverDefined = testsupport::checkSource("function main():void;");
    CHECK(neverDefined.find("never defined") != std::string::npos);

    const auto overloaded = testsupport::checkSource(
        "function main():void { eof: pass; }\n"
        "function main(int x):void { eof: pass; }\n");
    CHECK(overloaded.find("must not be overloaded") != std::string::npos);

    // Only a genuinely missing entry point mentions adding one.
    const auto missing = testsupport::checkSource("function helper():void { eof: pass; }");
    CHECK(missing.find("no entry point") != std::string::npos);
    CHECK(missing.find("function main():void") != std::string::npos);
}

TEST(entry_point_accepts_qualifiers_and_forward_declarations) {
    CHECK(testsupport::checkSource("function private main():void { eof: pass; }").empty());
    CHECK(testsupport::checkSource("function global main():void { eof: pass; }").empty());
    CHECK(testsupport::checkSource(
              "function main():void;\nfunction main():void { eof: pass; }\n")
              .empty());
}

TEST(an_empty_file_is_reported_as_empty) {
    const auto diagnostics = testsupport::checkSource("");
    CHECK(diagnostics.find("no code to compile") != std::string::npos);
    // The skeleton hint would be noise here: nothing has been written yet.
    CHECK(diagnostics.find("no entry point") == std::string::npos);

    const auto commentsOnly = testsupport::checkSource("// just a comment\n");
    CHECK(commentsOnly.find("no code to compile") != std::string::npos);
}

TEST(char_and_bool_are_not_arithmetic_types) {
    // `count + '\n'` used to add 10 and print a number (spec §4.2.2 makes
    // `bool` + `intN` a hard error; `char` follows the same rule).
    const auto plusChar = testsupport::checkSource(
        "function main():void { int n = 1; int m = n + '\\n'; eof: pass; }");
    CHECK(plusChar.find("cannot be applied to 'char'") != std::string::npos);
    CHECK(plusChar.find("as int") != std::string::npos);

    CHECK(testsupport::checkSource(
              "function main():void { int n = 1 + true; eof: pass; }")
              .find("cannot be applied to 'bool'") != std::string::npos);

    CHECK(testsupport::checkSource(
              "function main():void { char c = 'a'; int n = c + 1; eof: pass; }")
              .find("cannot be applied to 'char'") != std::string::npos);

    CHECK(testsupport::checkSource(
              "function main():void { char c = 'a'; char d = c * c; eof: pass; }")
              .find("cannot be applied to 'char'") != std::string::npos);

    CHECK(testsupport::checkSource(
              "function main():void { char c = 'a'; int n = ~c; eof: pass; }")
              .find("'~' requires an integer") != std::string::npos);
}

TEST(char_literals_do_not_decay_into_integers) {
    // `int n = 'a';` must not silently become 97.
    const auto decay = testsupport::checkSource(
        "function main():void { int n = 'a'; eof: pass; }");
    CHECK(decay.find("cannot initialise") != std::string::npos);
    CHECK(decay.find("convert it explicitly with 'as ") != std::string::npos);

    // The other direction stays available: an integer literal may name a code unit.
    CHECK(testsupport::checkSource(
              "function main():void { char c = 65; print(c); eof: pass; }")
              .empty());

    // Comparisons and string concatenation keep working.
    CHECK(testsupport::checkSource(
              "function main():void { char c = 'a'; bool b = c == 'a'; eof: pass; }")
              .empty());
    CHECK(testsupport::checkSource(
              "function main():void { int n = 1; string s = \"\" + n + '\\n'; eof: pass; }")
              .empty());
}

TEST(explicit_char_conversions_still_work) {
    CHECK(testsupport::checkSource(
              "function main():void { int n = 'a' as int; print(n); eof: pass; }")
              .empty());
    CHECK(testsupport::checkSource(
              "function main():void { char c = 'a'; int n = c as int + 1; eof: pass; }")
              .find("expected") == std::string::npos);
}

TEST(writing_is_not_reading) {
    // `#buf` and `buf[0] = x` compute an address or store; neither reads the
    // buffer, so an uninitialised array may be filled through a pointer.
    CHECK(testsupport::checkSource(
              "function fill(ptr int p, int n):void { @p = n; eof: pass; }\n"
              "function main():void { int buf[4]; fill(#buf[0], 1); eof: pass; }\n")
              .empty());
    CHECK(testsupport::checkSource(
              "function main():void { int a[4]; a[2] = 7; print(a[2]); eof: pass; }")
              .empty());
    CHECK(testsupport::checkSource(
              "struct P { int x; };\n"
              "function main():void { P p; p.x = 1; print(p.x); eof: pass; }\n")
              .empty());

    // Reading is still rejected.
    CHECK(testsupport::checkSource(
              "function main():void { int a[4]; print(a[2]); eof: pass; }")
              .find("may be used before initialisation") != std::string::npos);
}

TEST(if_bodies_have_their_own_scope) {
    // Spec §2.5.1: if / while / for / loop bodies are scopes, bare blocks are not.
    CHECK(testsupport::checkSource(
              "function main():void {\n"
              "    if (true) { int t = 1; print(t); }\n"
              "    if (true) { int t = 2; print(t); }\n"
              "    eof: pass;\n"
              "}\n")
              .empty());
    // Shadowing an outer variable is allowed.
    CHECK(testsupport::checkSource(
              "function main():void { int x = 1; if (true) { int x = 2; print(x); } "
              "print(x); eof: pass; }")
              .empty());
    // Bare blocks still share the enclosing scope.
    CHECK(testsupport::checkSource(
              "function main():void { { int t = 1; } { int t = 2; } eof: pass; }")
              .find("already declared") != std::string::npos);
}

TEST(pointers_can_be_stepped) {
    CHECK(testsupport::checkSource(
              "function main():void { int a[4]; ptr int p = #a[0]; p++; p--; eof: pass; }")
              .empty());
    CHECK(testsupport::checkSource(
              "function step(ptr int p):void { ptr int q = p; q++; eof: pass; }\n"
              "function main():void { int a[2]; step(#a[0]); eof: pass; }\n")
              .empty());
    // `ptr void` has no element size, so it cannot be stepped.
    CHECK(testsupport::checkSource(
              "function main():void { ptr void p = null; p++; eof: pass; }")
              .find("has no size") != std::string::npos);
}

TEST_MAIN()



