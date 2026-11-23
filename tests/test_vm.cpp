//===--- test_vm.cpp -----------------------------------------------------===//
#include "TestHarness.hpp"
#include "TestSupport.hpp"

namespace {

const char* kPrologue = "function main():void {\n";
const char* kEpilogue = "\n    eof: pass;\n}\n";

std::string program(const std::string& body) {
    return std::string(kPrologue) + body + kEpilogue;
}

std::string runFull(const std::string& source) {
    const auto result = testsupport::runSource(source);
    if (!result.ok) {
        return std::string("ERROR: ") + result.diagnostics;
    }
    return result.output;
}

std::string runBody(const std::string& body) {
    const auto result = testsupport::runSource(program(body));
    if (!result.ok) {
        return std::string("ERROR: ") + result.diagnostics;
    }
    return result.output;
}

} // namespace

TEST(arithmetic_precedence_and_integer_division) {
    CHECK_EQ(runBody("print(1 + 2 * 3);"), std::string("7"));
    CHECK_EQ(runBody("print(7 / 2);"), std::string("3"));
    CHECK_EQ(runBody("print(7 % 2);"), std::string("1"));
    CHECK_EQ(runBody("print(-7 / 2);"), std::string("-3"));
}

TEST(fixed_width_integers_behave_as_specified) {
    CHECK_EQ(runBody("int8 a = 100; int8 b = 27; print(a + b);"), std::string("127"));
    CHECK_EQ(runBody("unsigned int8 u = 255; u = u + 1; print(u);"), std::string("0"));
    CHECK_EQ(runBody("int8 n = -128; print(n);"), std::string("-128"));
    CHECK_EQ(runBody("int127 big = 85070591730234615865843651857942052863; print(big);"),
             std::string("85070591730234615865843651857942052863"));
}

TEST(signed_overflow_traps) {
    const auto result = testsupport::runSource(program("int8 a = 127; a = a + 1; print(a);"));
    CHECK(!result.ok);
    CHECK(result.diagnostics.find("integer overflow") != std::string::npos);
}

TEST(division_by_zero_traps) {
    const auto result = testsupport::runSource(program("int z = 0; print(1 / z);"));
    CHECK(!result.ok);
    CHECK(result.diagnostics.find("division by zero") != std::string::npos);
}

TEST(casts_check_their_range) {
    const auto failing = testsupport::runSource(program("int v = 300; int8 s = v as int8; print(s);"));
    CHECK(!failing.ok);
    CHECK(failing.diagnostics.find("cast out of range") != std::string::npos);
    CHECK_EQ(runBody("double d = 1.9; print(d as int);"), std::string("1"));
    CHECK_EQ(runBody("double d = -1.9; print(d as int);"), std::string("-1"));
}

TEST(float_arithmetic_and_formatting) {
    CHECK_EQ(runBody("print(7.0 / 2.0);"), std::string("3.5"));
    CHECK_EQ(runBody("print(1.0);"), std::string("1"));
    CHECK_EQ(runBody("double d = 1.0 / 0.0; print(d);"), std::string("inf"));
}

TEST(short_circuit_evaluation) {
    const auto result = testsupport::runSource(
        "function boom():bool { print(\"boom\"); return true; eof: pass; }\n"
        "function main():void {\n"
        "    if (false && boom()) { print(\"x\"); }\n"
        "    if (true || boom()) { print(\"ok\"); }\n"
        "    eof: pass;\n"
        "}\n");
    CHECK(result.ok);
    CHECK_EQ(result.output, std::string("ok"));
}

TEST(loops_and_jumps) {
    CHECK_EQ(runBody("int sum = 0; for i in 0..5 { sum = sum + i; } print(sum);"),
             std::string("10"));
    CHECK_EQ(runBody("int n = 0; loop (3) { n = n + 1; } print(n);"), std::string("3"));
    CHECK_EQ(runBody("int s = 0; int i = 0; while (i < 10) { i = i + 1; "
                     "if (i % 2 == 0) { continue; } if (i > 7) { break; } s = s + i; } print(s);"),
             std::string("16"));
    CHECK_EQ(runBody("int total = 0; outer: for (int i = 0; i < 3; i++) { "
                     "for (int j = 0; j < 3; j++) { if (j == 2) { continue outer; } total = total + 1; } } "
                     "print(total);"),
             std::string("6"));
}

TEST(strings_are_values_with_content_equality) {
    CHECK_EQ(runBody("string a = \"he\"; string b = a + \"llo\"; print(b);"),
             std::string("hello"));
    CHECK_EQ(runBody("string a = \"hi\"; string b = \"hi\"; print(a == b);"),
             std::string("true"));
    CHECK_EQ(runBody("string a = \"hello\"; print(a.length);"), std::string("5"));
    CHECK_EQ(runBody("string a = \"hello\"; print(a[1..3]);"), std::string("el"));
    CHECK_EQ(runBody("print(\"n=\" + 42);"), std::string("n=42"));
}

TEST(string_index_bounds_trap) {
    const auto result = testsupport::runSource(program("string s = \"hi\"; print(s[5]);"));
    CHECK(!result.ok);
    CHECK(result.diagnostics.find("index out of bounds") != std::string::npos);
}

TEST(array_and_struct_semantics) {
    CHECK_EQ(runBody("int a[3] = {1, 2, 3}; print(a[1] + a[2]);"), std::string("5"));
    CHECK_EQ(runBody("int a[3] = {1}; print(a[2]);"), std::string("0"));
    CHECK_EQ(runFull("struct P { int x; int y; };\n"
                     "function main():void { P p = {3, 4}; P q = p; q.x = 9; print(p.x); "
                     "print(q.x); eof: pass; }\n"),
             std::string("39"));
    const auto failing =
        testsupport::runSource(program("int a[3] = {1,2,3}; int i = 3; print(a[i]);"));
    CHECK(!failing.ok);
    CHECK(failing.diagnostics.find("index out of bounds") != std::string::npos);
}

TEST(struct_equality_is_member_wise) {
    const char* prefix = "struct P { int x; int y; };\nfunction main():void { ";
    CHECK_EQ(runFull(std::string(prefix) + "P a = {1, 2}; P b = {1, 2}; print(a == b); eof: pass; }\n"),
             std::string("true"));
    CHECK_EQ(runFull(std::string(prefix) + "P a = {1, 2}; P b = {1, 3}; print(a == b); eof: pass; }\n"),
             std::string("false"));
}

TEST(pointers_and_arrows) {
    CHECK_EQ(runBody("int x = 1; ptr int p = #x; @p = 7; print(x);"), std::string("7"));
    CHECK_EQ(runFull("struct P { int x; };\nfunction main():void { P p = {1}; ptr P q = #p; "
                     "q->x = 5; print(p.x); eof: pass; }\n"),
             std::string("5"));
    CHECK_EQ(runBody("ptr int p = null; print(p == null);"), std::string("true"));
    const auto failing = testsupport::runSource(program("ptr int p = null; print(@p);"));
    CHECK(!failing.ok);
    CHECK(failing.diagnostics.find("null pointer") != std::string::npos);
}

TEST(heap_allocation_round_trip) {
    CHECK_EQ(runBody("ptr int p = alloc(4 * sizeof(int)) as ptr int; @p = 3; "
                     "@(p + 1) = 4; print(@p + @(p + 1)); free(p as ptr void);"),
             std::string("7"));
}

TEST(eof_runs_on_the_normal_path_only) {
    const auto result = testsupport::runSource(
        "function f(int x):int {\n"
        "    if (x < 0) { return -1; }\n"
        "    return 0;\n"
        "    eof: print(\"cleanup \");\n"
        "}\n"
        "function main():void { print(f(-1)); print(f(0)); eof: pass; }\n");
    CHECK(result.ok);
    CHECK_EQ(result.output, std::string("cleanup -1cleanup 0"));
}

TEST(virtual_with_its_own_eof_replaces_the_outer_cleanup) {
    const auto result = testsupport::runSource(
        "function f(int x):int {\n"
        "    if (x < 0) {\n"
        "        virtual { return -1; eof: print(\"inner \"); }\n"
        "    }\n"
        "    return 0;\n"
        "    eof: print(\"outer \");\n"
        "}\n"
        "function main():void { print(f(-1)); print(f(0)); eof: pass; }\n");
    CHECK(result.ok);
    CHECK_EQ(result.output, std::string("inner -1outer 0"));
}

TEST(arena_scope_reclaims_strings) {
    CHECK_EQ(runBody("arena_scope { string s = \"a\" + \"b\"; print(s); } print(\"!\");"),
             std::string("ab!"));
}

TEST(process_and_event_are_scheduled) {
    const auto result = testsupport::runSource(
        "global int counter = 0;\n"
        "function process worker():void {\n"
        "    while (true) { counter = counter + 1; if (counter >= 3) { return; } yield(); }\n"
        "    eof: pass;\n"
        "}\n"
        "function event reached()(counter == 2):void { print(\"[e2] \"); eof: pass; }\n"
        "function main():void {\n"
        "    loop (10) { yield(); }\n"
        "    print(counter);\n"
        "    eof: pass;\n"
        "}\n");
    CHECK(result.ok);
    CHECK_EQ(result.output, std::string("[e2] 3"));
}

TEST(library_functions) {
    CHECK_EQ(runBody("print(itoa(255, 16));"), std::string("ff"));
    CHECK_EQ(runBody("print(ftoa(1.5));"), std::string("1.5"));
    CHECK_EQ(runBody("print(atoi(\"41\") + 1);"), std::string("42"));
    CHECK_EQ(runBody("print(indexOf(\"hello\", \"ll\"));"), std::string("2"));
    CHECK_EQ(runBody("print(indexOf(\"hello\", \"zz\"));"), std::string("-1"));
    CHECK_EQ(runBody("print(substring(\"hello\", 1, 3));"), std::string("el"));
    CHECK_EQ(runBody("print(trim(\"  hi \\t\"));"), std::string("hi"));
    CHECK_EQ(runBody("print(toUpper(\"ab\"));"), std::string("AB"));
    CHECK_EQ(runBody("print(startsWith(\"hello\", \"he\"));"), std::string("true"));
    CHECK_EQ(runBody("print(sqrt(9.0));"), std::string("3"));
    CHECK_EQ(runBody("print(pow(2.0, 10.0));"), std::string("1024"));
    CHECK_EQ(runBody("print(min(3, 4));"), std::string("3"));
    CHECK_EQ(runBody("print(abs(-5));"), std::string("5"));
}

TEST(atoi_traps_on_trailing_junk) {
    const auto result = testsupport::runSource(program("print(atoi(\"12abc\"));"));
    CHECK(!result.ok);
}

TEST(assert_traps_when_false) {
    const auto result = testsupport::runSource(program("assert(1 == 2);"));
    CHECK(!result.ok);
    CHECK(result.diagnostics.find("assertion failed") != std::string::npos);
}

TEST(print_formats_scalars) {
    CHECK_EQ(runBody("print(true); print(\" \"); print('A'); print(\" \"); print(3.5);"),
             std::string("true A 3.5"));
}

TEST(pointer_arithmetic_scales_by_element_size) {
    CHECK_EQ(runBody("int a[4] = {10,20,30,40}; ptr int p = #a[0]; "
                     "print(@(p + 1)); print(\" \"); print(@(p + 3));"),
             std::string("20 40"));
    // Prefix and postfix stepping both advance one element.
    CHECK_EQ(runBody("int a[4] = {10,20,30,40}; ptr int p = #a[0]; "
                     "p++; print(@p); print(\" \"); ++p; print(@p); print(\" \"); "
                     "p--; print(@p);"),
             std::string("20 30 20"));
    // char elements are two bytes wide.
    CHECK_EQ(runBody("char c[3] = {'a','b','c'}; ptr char p = #c[0]; "
                     "p++; p++; print(@p);"),
             std::string("c"));
}

TEST(pointer_difference_counts_elements) {
    CHECK_EQ(runBody("int a[10]; ptr int p = #a[0]; ptr int q = #a[3]; "
                     "print(q - p); print(\" \"); print(p - q);"),
             std::string("3 -3"));
    CHECK_EQ(runBody("char c[10]; ptr char p = #c[0]; ptr char q = #c[5]; print(q - p);"),
             std::string("5"));
}

TEST(an_address_expression_survives_arithmetic) {
    // Chained pointer arithmetic used to lose the address inside the VM.
    CHECK_EQ(runBody("int a[5] = {10,20,30,40,50}; ptr int p = #a[0]; "
                     "print(@(p + 2 - 1)); print(\" \"); print(@(p + 4 - 2 + 1));"),
             std::string("20 40"));
}

TEST(increment_as_a_statement_leaves_no_residue) {
    // `i++;` discards its result; the stack must stay balanced across many of them.
    CHECK_EQ(runBody("int i = 0; for (int k = 0; k < 100; k++) { i++; } print(i);"),
             std::string("100"));
}

TEST(fused_variable_instructions_behave_like_the_generic_form) {
    // `x` reads through LOAD_LOCAL, `x = v` through STORE_LOCAL, and both must
    // agree with the address-based form for every width and signedness.
    CHECK_EQ(runBody("int8 a = -1; print(a); print(\" \"); print(a + 1);"),
             std::string("-1 0"));
    CHECK_EQ(runBody("unsigned int8 u = 250; u++; print(u);"), std::string("251"));
    CHECK_EQ(runBody("int16 m = 1000; m = m + 24; print(m);"), std::string("1024"));
    CHECK_EQ(runBody("int64 big = 4294967296; big++; print(big);"),
             std::string("4294967297"));
    CHECK_EQ(runBody("double d = 1.5; d++; print(d);"), std::string("2.5"));
    CHECK_EQ(runBody("bool b = false; b = true; print(b);"), std::string("true"));
    CHECK_EQ(runBody("char c = 'a'; c++; print(c);"), std::string("b"));
}

TEST(compound_assignment_goes_through_the_same_path) {
    CHECK_EQ(runBody("int x = 10; x += 5; print(x);"), std::string("15"));
    CHECK_EQ(runBody("int x = 10; x -= 4; print(x);"), std::string("6"));
    CHECK_EQ(runBody("int x = 3; x *= 7; print(x);"), std::string("21"));
    CHECK_EQ(runBody("unsigned int8 u = 250; u += 10; print(u);"), std::string("4"));
    // The value of an assignment used as an expression must survive.
    CHECK_EQ(runBody("int x = 0; int y = 0; y = x = 5; print(x); print(y);"),
             std::string("55"));
}

TEST(pointer_stepping_still_traps_on_bad_types) {
    // `ptr void` has no size, so stepping it must be rejected at compile time;
    // we already test that in test_sema, this checks the runtime side is sane.
    CHECK_EQ(runBody("int a[4] = {1,2,3,4}; ptr int p = #a[0]; "
                     "p++; p++; print(@p); print(\" \"); p--; print(@p);"),
             std::string("3 2"));
}

TEST(the_copy_step_peephole_matches_the_long_form) {
    // IRGen folds `@dst = @src; src++; dst++;` into a single COPY_STEP. The
    // result must be indistinguishable from spelling out the three statements,
    // for every element size the peephole accepts, in either increment order.
    const auto program = [](const char* body, const char* check) {
        return std::string("function copy(ptr int src, ptr int dst, int n):void {\n"
                           "    int i = 0;\n"
                           "    while (i < n) {\n") +
               body +
               "        i++;\n"
               "    }\n"
               "    eof: pass;\n"
               "}\n"
               "function main():void {\n"
               "    int a[3] = {7, 8, 9};\n"
               "    int b[3] = {0, 0, 0};\n"
               "    copy(#a[0], #b[0], 3);\n" +
               check +
               "    eof: pass;\n"
               "}\n";
    };

    // Folded: copy then both steps.
    CHECK_EQ(runFull(program("        @dst = @src;\n        src++;\n        dst++;\n",
                             "    print(b[0]); print(\" \"); print(b[1]); print(\" \"); "
                             "print(b[2]);\n")),
             std::string("7 8 9"));

    // Folded: reversed increment order.
    CHECK_EQ(runFull(program("        @dst = @src;\n        dst++;\n        src++;\n",
                             "    print(b[0]); print(b[1]); print(b[2]);\n")),
             std::string("789"));

    // Not folded (an unrelated statement sits between): still correct.
    CHECK_EQ(runFull(program("        @dst = @src;\n        i++;\n        i--;\n"
                             "        src++;\n        dst++;\n",
                             "    print(b[0]); print(b[1]); print(b[2]);\n")),
             std::string("789"));

    // Not a plain copy (`+ 1` on the value): must not fold, must stay correct.
    CHECK_EQ(runFull(program("        @dst = @src + 1;\n        src++;\n        dst++;\n",
                             "    print(b[0]); print(b[1]); print(b[2]);\n")),
             std::string("8910"));

    // Two-byte elements take the same path with a different size encoding.
    CHECK_EQ(runFull("function copyChar(ptr char src, ptr char dst, int n):void {\n"
                     "    int i = 0;\n"
                     "    while (i < n) {\n"
                     "        @dst = @src;\n"
                     "        src++;\n"
                     "        dst++;\n"
                     "        i++;\n"
                     "    }\n"
                     "    eof: pass;\n"
                     "}\n"
                     "function main():void {\n"
                     "    char a[3] = {'x', 'y', 'z'};\n"
                     "    char b[3] = {'-', '-', '-'};\n"
                     "    copyChar(#a[0], #b[0], 3);\n"
                     "    print(b[0]); print(b[1]); print(b[2]);\n"
                     "    eof: pass;\n"
                     "}\n"),
             std::string("xyz"));
}

TEST(unbounded_recursion_traps_instead_of_eating_memory) {
    // A recursive function with no base case compiles fine. It used to grow the
    // frame stack without limit until the machine died; now it stops with a
    // diagnosable trap.
    const auto result = testsupport::runSource(
        "function f(int n):int { return f(n + 1); }\n"
        "function main():void { print(f(0)); eof: pass; }\n");
    CHECK(!result.ok);
    CHECK(result.diagnostics.find("call depth exceeded") != std::string::npos);
    CHECK(result.diagnostics.find("f") != std::string::npos);
}

TEST(a_runaway_pointer_traps_instead_of_eating_memory) {
    // Pages are allocated on first touch, so sweeping a pointer across the
    // address space used to request 64 KB blocks forever. With a tiny cap the
    // trap is reached in a handful of pages.
    const auto result = testsupport::runSource(
        "function main():void {\n"
        "    ptr int p = 0x10000000 as ptr int;\n"
        "    int i = 0;\n"
        "    while (i < 100000) { @(p + i * 16384) = 1; i++; }\n"
        "    eof: pass;\n"
        "}\n",
        /*memoryLimit=*/256 * 1024);
    CHECK(!result.ok);
    CHECK(result.diagnostics.find("MB of memory") != std::string::npos);
}

TEST(a_normal_program_stays_far_below_the_caps) {
    // Sanity: the limits must not fire for ordinary code. This allocates a
    // few pages and recurses a few hundred frames deep.
    const auto result = testsupport::runSource(
        "function down(int n):int {\n"
        "    if (n <= 0) { return 0; }\n"
        "    return 1 + down(n - 1);\n"
        "}\n"
        "function main():void {\n"
        "    int a[10000];\n"
        "    ptr int p = #a[0];\n"
        "    for i in 0..10000 { @(p + i) = i; }\n"
        "    int sum = 0;\n"
        "    for i in 0..10000 { sum = sum + @(p + i); }\n"
        "    print(sum); print(\" \"); print(down(500));\n"
        "    eof: pass;\n"
        "}\n");
    CHECK(result.ok);
    CHECK_EQ(result.output, std::string("49995000 500"));
}

TEST(native_helpers_sort_and_reverse_in_place) {
    // The native path marshals a range into a contiguous buffer, runs C on it,
    // and copies back. These check the contract, not the algorithm: the buffer
    // must come back sorted, for every element kind the bridge supports.
    CHECK_EQ(runBody("int a[6] = {5, 3, 9, 1, 7, 2}; sortInts(#a[0], 6); "
                     "for i in 0..6 { print(@(#a[0] + i)); }"),
             std::string("123579"));
    CHECK_EQ(runBody("double d[4] = {2.5, -1.0, 3.75, 0.0}; sortFloats(#d[0], 4); "
                     "print(@(#d[0])); print(\" \"); print(@(#d[0] + 1)); print(\" \"); "
                     "print(@(#d[0] + 3));"),
             std::string("-1 0 3.75"));
    CHECK_EQ(runBody("int a[5] = {1, 2, 3, 4, 5}; reverseInts(#a[0], 5); "
                     "for i in 0..5 { print(@(#a[0] + i)); }"),
             std::string("54321"));
}

TEST(native_helpers_handle_the_boundaries) {
    // Already sorted, reversed, all equal, and the degenerate counts. These are
    // where a hand written partition goes wrong (quadratic blowup, out of
    // bounds scan, zero progress), so they are worth pinning.
    CHECK_EQ(runBody("int a[5] = {1, 2, 3, 4, 5}; sortInts(#a[0], 5); "
                     "for i in 0..5 { print(@(#a[0] + i)); }"),
             std::string("12345"));
    CHECK_EQ(runBody("int a[5] = {5, 4, 3, 2, 1}; sortInts(#a[0], 5); "
                     "for i in 0..5 { print(@(#a[0] + i)); }"),
             std::string("12345"));
    CHECK_EQ(runBody("int a[6] = {4, 4, 4, 4, 4, 4}; sortInts(#a[0], 6); "
                     "print(@(#a[0])); print(@(#a[0] + 5));"),
             std::string("44"));
    // count 0 and 1 must not touch memory at all, and may be given null.
    CHECK_EQ(runBody("ptr int p = null; sortInts(p, 0); sortInts(p, 1); print(\"ok\");"),
             std::string("ok"));
    // Larger than the insertion-sort threshold.
    CHECK_EQ(runBody("int a[20]; "
                     "for i in 0..20 { @(#a[0] + i) = (i * 7) % 20; } "
                     "sortInts(#a[0], 20); "
                     "int bad = 0; "
                     "for (int i = 1; i < 20; i++) { "
                     "  if (@(#a[0] + i - 1) > @(#a[0] + i)) { bad++; } } "
                     "print(bad);"),
             std::string("0"));
}

TEST(native_helpers_trap_on_bad_arguments) {
    // A negative count cannot mean anything; a null pointer with a real count
    // would otherwise be a silent write to address zero.
    const auto negative = testsupport::runSource(program("int a[4] = {1}; sortInts(#a[0], -1);"));
    CHECK(!negative.ok);
    CHECK(negative.diagnostics.find("must not be negative") != std::string::npos);

    const auto null = testsupport::runSource(program("ptr int p = null; sortInts(p, 4);"));
    CHECK(!null.ok);
    CHECK(null.diagnostics.find("null pointer") != std::string::npos);
}

TEST(native_helpers_cover_the_64_bit_element_kind) {
    // `int` is 4 bytes and `int64` is 8, so the same bridge takes a different
    // element size and a different comparison; both must still be ascending.
    CHECK_EQ(runBody("int64 w[4] = {9000000000, 3, 7000000000, 1}; "
                     "sortInt64s(#w[0], 4); "
                     "print(@(#w[0])); print(\" \"); print(@(#w[0] + 1)); print(\" \"); "
                     "print(@(#w[0] + 3));"),
             std::string("1 3 9000000000"));
}

TEST(a_string_returned_by_a_call_can_be_used_directly) {
    // Regression: an aggregate-returning call leaves the destination pointer as
    // a `Ptr` value, while a string local is an `Addr`. readString used to
    // accept only Addr, so `print(f(...))` printed nothing -- silently, which is
    // the worst way to be wrong.
    CHECK_EQ(runFull("function cat(string a, string b):string { return a + b; }\n"
                     "function main():void { print(\"[\"); print(cat(\"p\", \"q\")); "
                     "print(\"]\"); eof: pass; }"),
             std::string("[pq]"));
    // Same through an identity and a chained call.
    CHECK_EQ(runFull("function id(string a):string { return a; }\n"
                     "function ex(string a):string { return a + \"!\"; }\n"
                     "function main():void { print(id(\"abc\")); print(\" \"); "
                     "print(ex(\"abc\")); eof: pass; }"),
             std::string("abc abc!"));
    // Binding to a local first always worked; keep it pinned too.
    CHECK_EQ(runFull("function cat(string a, string b):string { return a + b; }\n"
                     "function main():void { string v = cat(\"x\", \"y\"); print(v); "
                     "eof: pass; }"),
             std::string("xy"));
}

TEST(aggregate_assignment_actually_copies) {
    // Regression: MEM_COPY pops the destination first, so the stack must be
    // [source, destination]. The emitter used to duplicate the destination,
    // leaving [source, destination, destination] -- the copy then read the
    // destination as its own source and the assignment did nothing at all.
    // Silent, because nothing traps and the old value is still valid.
    CHECK_EQ(runBody("string s = \"a\"; string t = \"b\"; s = t; print(s);"),
             std::string("b"));
    CHECK_EQ(runFull("struct P { int x; int y; };\n"
                     "function main():void {\n"
                     "    P p = {1, 2}; P q = {7, 8}; p = q;\n"
                     "    print(p.x); print(p.y); eof: pass; }"),
             std::string("78"));
    // Concatenation in a loop is the shape that exposed it: each iteration
    // assigned a freshly built string back over the accumulator.
    CHECK_EQ(runBody("string out = \"\"; "
                     "for (int i = 0; i < 3; i++) { out = out + \"z\"; } print(out);"),
             std::string("zzz"));
    // Assignment as a value still leaves the result for the outer expression.
    CHECK_EQ(runBody("string a = \"x\"; string b = \"y\"; string c = (a = b); "
                     "print(a); print(b); print(c);"),
             std::string("yyy"));
}

TEST(a_scalar_on_the_left_of_a_concatenation_is_rendered) {
    // `x + "text"` is legal for any scalar x (Sema's isScalarOrString accepts a
    // string on either side), but only the right operand was ever formatted: the
    // left one went through readString, which returns "" for anything that is not
    // a string descriptor. `print(5 + " ")` printed a single space.
    CHECK_EQ(runBody("print(5 + \" \");"), std::string("5 "));
    CHECK_EQ(runBody("int v = 42; print(v + \"!\");"), std::string("42!"));
    CHECK_EQ(runBody("double d = 1.5; print(d + \"|\");"), std::string("1.5|"));
    CHECK_EQ(runBody("print(true + \"|\");"), std::string("true|"));
    CHECK_EQ(runBody("print(false + \"|\");"), std::string("false|"));
    CHECK_EQ(runBody("char c = 65 as char; print(c + \"|\");"), std::string("A|"));
    CHECK_EQ(runBody("int64 big = 5000000000i64; print(big + \"|\");"),
             std::string("5000000000|"));
    // Only when the operand really is a pointer.
    CHECK_EQ(runBody("ptr int p = 0 as ptr int; print(p + \"|\");"), std::string("null|"));
}

TEST(a_string_on_the_left_still_works) {
    // The mirror case, which was already correct: it guards against "fixing" the
    // left operand by breaking the right one.
    CHECK_EQ(runBody("int v = 42; print(\"[\" + v + \"]\");"), std::string("[42]"));
    CHECK_EQ(runBody("print(\"a\" + \"b\");"), std::string("ab"));
}

TEST(concatenation_chains_render_every_operand) {
    // Left-associative: `a + ", " + b` is `(a + ", ") + b`, so the inner result is
    // a string by the time the outer one runs. That hid the bug in chains that
    // start with a literal and showed it in ones that start with a value.
    CHECK_EQ(runBody("int a = 1; int b = 2; print(a + \", \" + b);"),
             std::string("1, 2"));
    CHECK_EQ(runBody("int a = 7; print(\"x=\" + a + \";\");"), std::string("x=7;"));
    CHECK_EQ(runBody("for (int i = 0; i < 3; i++) { print(i + \" \"); }"),
             std::string("0 1 2 "));
}

TEST_MAIN()









