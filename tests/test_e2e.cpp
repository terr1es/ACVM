//===--- test_e2e.cpp - Whole toolchain through the Driver -------------===//
#include "TestHarness.hpp"

#include "acvm/Driver/Driver.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

class TempFile {
public:
    explicit TempFile(const std::string& name) : m_path(fs::temp_directory_path() / name) {}
    ~TempFile() {
        std::error_code ignored;
        fs::remove(m_path, ignored);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] std::string path() const { return m_path.string(); }

    void write(const std::string& text) const {
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        out << text;
    }

private:
    fs::path m_path;
};

const char* kFizzBuzz =
    "function main():void {\n"
    "    for i in 1..16 {\n"
    "        if (i % 15 == 0) {\n"
    "            print(\"FizzBuzz\");\n"
    "        } else if (i % 3 == 0) {\n"
    "            print(\"Fizz\");\n"
    "        } else if (i % 5 == 0) {\n"
    "            print(\"Buzz\");\n"
    "        } else {\n"
    "            print(i);\n"
    "        }\n"
    "        print(\"\\n\");\n"
    "    }\n"
    "    eof: pass;\n"
    "}\n";

} // namespace

TEST(builds_runs_and_disassembles_a_module) {
    TempFile source("acvm_e2e_build.ax");
    TempFile module("acvm_e2e_build.acvm");
    source.write("function main():void { print(6 * 7); eof: pass; }\n");

    std::ostringstream buildOut;
    std::ostringstream buildErr;
    acvm::Driver builder(buildOut, buildErr);
    CHECK_EQ(builder.buildSource(source.path(), module.path(), {}), 0);
    CHECK(fs::exists(module.path()));
    CHECK(buildErr.str().empty());

    std::ostringstream runOut;
    std::ostringstream runErr;
    acvm::Driver runner(runOut, runErr);
    CHECK_EQ(runner.runBytecode(module.path()), 0);
    CHECK_EQ(runOut.str(), std::string("42"));

    std::ostringstream disasmOut;
    std::ostringstream disasmErr;
    acvm::Driver disassembler(disasmOut, disasmErr);
    CHECK_EQ(disassembler.disassembleBytecode(module.path()), 0);
    CHECK(disasmOut.str().find("function main") != std::string::npos);
    CHECK(disasmOut.str().find("BUILTIN") != std::string::npos);
}

TEST(runs_a_complete_program) {
    TempFile source("acvm_e2e_fizzbuzz.ax");
    source.write(kFizzBuzz);

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource(source.path(), {}), 0);

    std::istringstream lines(out.str());
    std::string line;
    std::vector<std::string> produced;
    while (std::getline(lines, line)) {
        produced.push_back(line);
    }
    CHECK_EQ(produced.size(), std::size_t(15));
    CHECK_EQ(produced[0], std::string("1"));
    CHECK_EQ(produced[2], std::string("Fizz"));
    CHECK_EQ(produced[4], std::string("Buzz"));
    CHECK_EQ(produced[14], std::string("FizzBuzz"));
}

TEST(check_reports_success_and_failure) {
    TempFile good("acvm_e2e_good.ax");
    good.write("function main():void { int x = 1; print(x); eof: pass; }\n");
    TempFile bad("acvm_e2e_bad.ax");
    bad.write("function main():void { int x = \"no\"; eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.checkSource(good.path(), {}), 0);
    CHECK(out.str().find("ok") != std::string::npos);

    std::ostringstream out2;
    std::ostringstream err2;
    acvm::Driver driver2(out2, err2);
    CHECK_EQ(driver2.checkSource(bad.path(), {}), 1);
    CHECK(err2.str().find("cannot initialise") != std::string::npos);
}

TEST(reports_a_missing_input_file) {
    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource("this_file_does_not_exist.ax", {}), 1);
    CHECK(err.str().find("cannot open file") != std::string::npos);
}

TEST(rejects_a_corrupt_module) {
    TempFile module("acvm_e2e_corrupt.acvm");
    module.write("not a module");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runBytecode(module.path()), 1);
    CHECK(err.str().find("not an ACVM module") != std::string::npos);
}

TEST(runtime_errors_exit_with_status_one) {
    TempFile source("acvm_e2e_trap.ax");
    source.write("function main():void { int z = 0; print(1 / z); eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource(source.path(), {}), 1);
    CHECK(err.str().find("division by zero") != std::string::npos);
}

TEST(verbose_dumping_does_not_change_the_result) {
    TempFile source("acvm_e2e_verbose.ax");
    source.write("function main():void { print(1 + 1); eof: pass; }\n");

    acvm::DriverOptions options;
    options.dumpTokens = true;
    options.dumpAst = true;
    options.dumpIr = true;

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource(source.path(), options), 0);
    CHECK(out.str().find("Unit") != std::string::npos);
    CHECK(out.str().find("function main") != std::string::npos);
    CHECK(out.str().find("CONST_INT") != std::string::npos);
}

TEST(no_assert_removes_the_check_and_its_argument) {
    TempFile source("acvm_e2e_assert.ax");
    source.write("function main():void { assert(false); print(\"done\"); eof: pass; }\n");

    acvm::DriverOptions options;
    options.assertions = false;

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource(source.path(), options), 0);
    CHECK_EQ(out.str(), std::string("done"));
}

TEST(ir_counts_only_user_functions) {
    // A program with a single function must report a single function: the
    // compiler's own global initialiser is not user code.
    TempFile source("acvm_e2e_ir_count.ax");
    source.write("function main():void { print(1); eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.dumpBytecode(source.path()), 0);

    const std::string text = out.str();
    CHECK(text.find("module: 1 function(s)") != std::string::npos);
    CHECK(text.find("__acvm_init_globals") == std::string::npos);
    CHECK(text.find("compiler-generated") == std::string::npos);
}

TEST(ir_lists_generated_code_separately) {
    TempFile source("acvm_e2e_ir_global.ax");
    source.write("global int counter = 5;\n"
                 "function main():void { print(counter); eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.dumpBytecode(source.path()), 0);

    const std::string text = out.str();
    CHECK(text.find("module: 1 function(s), 1 global(s) (+1 compiler-generated)") !=
          std::string::npos);
    // The initialiser is shown, but under its own heading.
    CHECK(text.find("compiler-generated code") != std::string::npos);
    CHECK(text.find("__acvm_init_globals") != std::string::npos);
    // ...and it comes after main.
    CHECK(text.find("function main") < text.find("__acvm_init_globals"));
}

TEST(the_synthetic_flag_survives_serialisation) {
    TempFile source("acvm_e2e_ir_roundtrip.ax");
    TempFile module("acvm_e2e_ir_roundtrip.acvm");
    source.write("global int counter = 5;\n"
                 "function main():void { print(counter); eof: pass; }\n");

    {
        std::ostringstream out;
        std::ostringstream err;
        acvm::Driver builder(out, err);
        CHECK_EQ(builder.buildSource(source.path(), module.path(), {}), 0);
    }

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.disassembleBytecode(module.path()), 0);
    CHECK(out.str().find("module: 1 function(s), 1 global(s) (+1 compiler-generated)") !=
          std::string::npos);
}

TEST(imports_merge_module_members_into_the_file_scope) {
    TempFile lib("acvm_e2e_implib_mathx.ax");
    lib.write("function square(int x):int { return x * x; }\n"
              "function cube(int x):int { return x * x * x; }\n");

    TempFile source("acvm_e2e_imroot.ax");
    source.write("function import (return:(acvm_e2e_implib_mathx));\n"
                 "function main():void {\n"
                 "    print(square(7)); print(\" \"); print(cube(3));\n"
                 "    eof: pass;\n"
                 "}\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource(source.path(), {}), 0);
    CHECK_EQ(out.str(), std::string("49 27"));
    CHECK(err.str().empty());
}

TEST(a_quoted_path_is_an_alternative_way_to_import) {
    TempFile lib("acvm_e2e_implib_path.ax");
    lib.write("function twice(int x):int { return x + x; }\n");

    TempFile source("acvm_e2e_impath.ax");
    source.write("import \"acvm_e2e_implib_path.ax\";\n"
                 "function main():void { print(twice(21)); eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource(source.path(), {}), 0);
    CHECK_EQ(out.str(), std::string("42"));
}

TEST(a_missing_or_circular_import_is_a_compile_error) {
    TempFile source("acvm_e2e_imbad.ax");
    source.write("function import (return:(acvm_e2e_no_such_module));\n"
                 "function main():void { eof: pass; }\n");

    {
        std::ostringstream out;
        std::ostringstream err;
        acvm::Driver driver(out, err);
        CHECK(driver.checkSource(source.path(), {}) != 0);
        CHECK(err.str().find("cannot find module") != std::string::npos);
    }

    // Two files that import each other: the chain has to name both.
    TempFile a("acvm_e2e_cycle_a.ax");
    TempFile b("acvm_e2e_cycle_b.ax");
    a.write("function import (return:(acvm_e2e_cycle_b));\nfunction fa():void { eof: pass; }\n");
    b.write("function import (return:(acvm_e2e_cycle_a));\nfunction fb():void { eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK(driver.checkSource(a.path(), {}) != 0);
    CHECK(err.str().find("circular import") != std::string::npos);
    CHECK(err.str().find("acvm_e2e_cycle_b.ax") != std::string::npos);
}

TEST(the_importing_file_wins_name_clashes) {
    // Spec Q43: members form one flat scope; a local definition overrides an
    // imported one, and two files defining the same signature is not an error.
    TempFile lib("acvm_e2e_implib_clash.ax");
    lib.write("function pick():int { return 1; }\n");

    TempFile source("acvm_e2e_imclash.ax");
    source.write("function import (return:(acvm_e2e_implib_clash));\n"
                 "function pick():int { return 2; }\n"
                 "function main():void { print(pick()); eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource(source.path(), {}), 0);
    CHECK_EQ(out.str(), std::string("2"));
    CHECK(err.str().empty());
}

TEST(private_is_file_scoped_and_import_respects_it) {
    // Spec §2.2.1.2: `private` is visible in its own file only. `import` merges
    // the name into the flat scope, so the check has to be by call site, not by
    // "is the symbol in scope" -- otherwise the importing file would see it.
    TempFile lib("acvm_e2e_implib_priv.ax");
    lib.write("function private helper(int x):int { return x * 2; }\n"
              "function api(int x):int { return helper(x) + 1; }\n");   // same file: fine

    // The library can call its own private function.
    {
        TempFile source("acvm_e2e_impriv_ok.ax");
        source.write("function import (return:(acvm_e2e_implib_priv));\n"
                     "function main():void { print(api(20)); eof: pass; }\n");
        std::ostringstream out;
        std::ostringstream err;
        acvm::Driver driver(out, err);
        CHECK_EQ(driver.runSource(source.path(), {}), 0);
        CHECK_EQ(out.str(), std::string("41"));
        CHECK(err.str().empty());
    }

    // The importing file cannot, and the message names the owning file.
    {
        TempFile source("acvm_e2e_impriv_bad.ax");
        source.write("function import (return:(acvm_e2e_implib_priv));\n"
                     "function main():void { print(helper(1)); eof: pass; }\n");
        std::ostringstream out;
        std::ostringstream err;
        acvm::Driver driver(out, err);
        CHECK(driver.checkSource(source.path(), {}) != 0);
        CHECK(err.str().find("is private to acvm_e2e_implib_priv.ax") != std::string::npos);
        // One diagnostic, not a cascade through `print`.
        CHECK(err.str().find("cannot print") == std::string::npos);
    }
}

TEST(private_functions_are_visible_within_one_file) {
    TempFile source("acvm_e2e_priv_same.ax");
    source.write("function private twice(int x):int { return x + x; }\n"
                 "function main():void { print(twice(21)); eof: pass; }\n");
    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource(source.path(), {}), 0);
    CHECK_EQ(out.str(), std::string("42"));
}

TEST(the_standard_library_is_on_the_module_search_path) {
    // `import (algorithm)` has to work from any directory, not only from next
    // to the file: stdlib/ is searched after the importing file's own folder.
    TempFile source("acvm_e2e_stdlib_algorithm.ax");
    source.write("function import (return:(algorithm));\n"
                 "function main():void {\n"
                 "    int a[5] = {5, 4, 1, 3, 2};\n"
                 "    sort(#a[0], 5);\n"
                 "    for i in 0..5 { print(@(#a[0] + i)); }\n"
                 "    eof: pass;\n"
                 "}\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource(source.path(), {}), 0);
    CHECK_EQ(out.str(), std::string("12345"));
    CHECK(err.str().empty());
}

TEST(a_module_sharing_the_programs_name_resolves_to_the_library) {
    // `examples/algorithm.ax` imports `algorithm`. Searching the importing
    // file's own directory first would find the file itself and report a
    // cycle, so the file being compiled is skipped while resolving.
    TempFile source("acvm_e2e_algorithm.ax");   // same name as the module
    source.write("function import (return:(algorithm));\n"
                 "function main():void {\n"
                 "    double d[3] = {2.5, -1.0, 0.0};\n"
                 "    sortDouble(#d[0], 3);\n"
                 "    print(@(#d[0])); print(\" \"); print(@(#d[0] + 2));\n"
                 "    eof: pass;\n"
                 "}\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource(source.path(), {}), 0);
    CHECK_EQ(out.str(), std::string("-1 2.5"));
}

TEST(a_file_importing_itself_is_reported_as_such) {
    TempFile source("acvm_e2e_selfimport.ax");
    source.write("import \"acvm_e2e_selfimport.ax\";\n"
                 "function main():void { eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK(driver.checkSource(source.path(), {}) != 0);
    CHECK(err.str().find("cannot import itself") != std::string::npos);
}

TEST(a_library_file_declares_external_functions) {
    // `library "x";` turns bodyless declarations into external ones: no
    // bytecode, no entry point, and the symbol is recorded for the loader.
    TempFile lib("acvm_e2e_extdecl.ax");
    lib.write("library \"acvm_e2e_extdecl\";\n"
              "function extWork(ptr int data, int64 count):int64;\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.checkSource(lib.path(), {}), 0);
    CHECK(err.str().empty());
}

TEST(a_scalar_double_cannot_cross_the_dynamic_boundary) {
    // The ABI passes everything in one 64 bit integer register, so a double
    // would land in an SSE register and be lost. Rejected where the caret can
    // point at the parameter.
    TempFile lib("acvm_e2e_extdouble.ax");
    lib.write("library \"acvm_e2e_extdouble\";\n"
              "function takesDouble(double x):int64;\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK(driver.checkSource(lib.path(), {}) != 0);
    CHECK(err.str().find("cannot cross that boundary") != std::string::npos);
    CHECK(err.str().find("ptr double") != std::string::npos);
}

TEST(a_double_result_cannot_cross_either) {
    // Same reason as the parameter: the result would be in xmm0, not rax.
    TempFile lib("acvm_e2e_extdret.ax");
    lib.write("library \"acvm_e2e_extdret\";\n"
              "function returnsDouble():double;\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK(driver.checkSource(lib.path(), {}) != 0);
    CHECK(err.str().find("double' result cannot cross") != std::string::npos);
}

TEST(a_missing_shared_library_fails_at_the_first_call) {
    // The declaration compiles: whether the .dll exists is a run time fact.
    // The error has to name the library and say what to do about it.
    TempFile lib("acvm_e2e_extmissing.ax");
    lib.write("library \"acvm_e2e_no_such_library\";\n"
              "function extWork(ptr int data, int64 count):int64;\n");

    TempFile source("acvm_e2e_extuser.ax");
    source.write("import \"acvm_e2e_extmissing.ax\";\n"
                 "function main():void {\n"
                 "    int a[2] = {1, 2};\n"
                 "    extWork(#a[0], 2);\n"
                 "    eof: pass;\n"
                 "}\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK(driver.runSource(source.path(), {}) != 0);
    CHECK(err.str().find("external call failed") != std::string::npos);
    CHECK(err.str().find("acvm_e2e_no_such_library") != std::string::npos);
    CHECK(err.str().find("loader search path") != std::string::npos);
}

TEST(a_plain_program_stays_self_contained) {
    // Nothing external: the module must load with no library at all. This is
    // the guarantee the dynamic path gives up, so it has to hold for everyone
    // else.
    TempFile source("acvm_e2e_selfcontained.ax");
    source.write("function main():void { print(\"ok\"); eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource(source.path(), {}), 0);
    CHECK_EQ(out.str(), std::string("ok"));
    CHECK(err.str().empty());
}

TEST(declaration_files_use_the_axd_extension) {
    // `import (name)` looks for name.axd before name.ax: a declaration file is
    // what a shared library ships with, so it should win over a same-named
    // source file.
    TempFile decl("acvm_e2e_axddecl.axd");
    decl.write("library \"acvm_e2e_axddecl\";\n"
               "function fromDeclaration(ptr int data, int64 count):int64;\n");

    TempFile source("acvm_e2e_axduser.ax");
    source.write("function import (return:(acvm_e2e_axddecl));\n"
                 "function main():void { eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    // The declaration resolves and compiles; only the call would need the .dll.
    CHECK_EQ(driver.checkSource(source.path(), {}), 0);
    CHECK(err.str().empty());
}

TEST(an_axd_without_a_library_declaration_is_reported) {
    // Without this the file would be compiled as ordinary Axiom and fail later
    // with "no entry point", which says nothing about the real mistake.
    TempFile bad("acvm_e2e_axdnolib.axd");
    bad.write("function orphan(ptr int data, int64 count):int64;\n");

    {
        std::ostringstream out;
        std::ostringstream err;
        acvm::Driver driver(out, err);
        CHECK(driver.checkSource(bad.path(), {}) != 0);
        CHECK(err.str().find("does not declare a library") != std::string::npos);
        CHECK(err.str().find("library") != std::string::npos);
    }

    // Same check when it is reached through an import rather than directly.
    TempFile source("acvm_e2e_axdnoimb.ax");
    source.write("function import (return:(acvm_e2e_axdnolib));\n"
                 "function main():void { eof: pass; }\n");
    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK(driver.checkSource(source.path(), {}) != 0);
    CHECK(err.str().find("does not declare a library") != std::string::npos);
}

TEST(a_short_library_name_does_not_crash_the_loader_lookup) {
    // Regression: the suffix test computed `name.size() - 6`, which wraps
    // around for short names and makes std::string::compare throw -- an
    // uncaught std::out_of_range that killed the process instead of reporting
    // a missing library.
    TempFile lib("acvm_e2e_shortlib.axd");
    lib.write("library \"abcde\";\n"            // five characters: the old crash
              "function extWork(ptr int data, int64 count):int64;\n");

    TempFile source("acvm_e2e_shortuser.ax");
    source.write("import \"acvm_e2e_shortlib.axd\";\n"
                 "function main():void {\n"
                 "    int a[2] = {1, 2};\n"
                 "    extWork(#a[0], 2);\n"
                 "    eof: pass;\n"
                 "}\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK(driver.runSource(source.path(), {}) != 0);
    CHECK(err.str().find("cannot load library") != std::string::npos);
}

TEST(the_shared_standard_library_is_callable) {
    // End-to-end through the dynamic boundary: stddec.axd declares the
    // functions, stdlib.dll implements them, and the loader resolves them on
    // first call. Skipped when the library was not built (a static-only
    // configuration), because there is nothing to load.
    TempFile source("acvm_e2e_stddec.ax");
    source.write("function import (return:(stddec));\n"
                 "function main():void {\n"
                 "    int a[5] = {5, 1, 4, 2, 3};\n"
                 "    nativeSort(#a[0], 5);\n"
                 "    nativeReverse(#a[0], 5);\n"
                 "    print(nativeSum(#a[0], 5));\n"
                 "    print(\" \");\n"
                 "    print(nativeMax(#a[0], 5));\n"
                 "    print(\" \");\n"
                 "    print(nativeClamp(7, 0, 5));\n"
                 "    eof: pass;\n"
                 "}\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    const int status = driver.runSource(source.path(), {});
    if (status != 0 && err.str().find("cannot load library") != std::string::npos) {
        return;   // library not built; the declaration tests cover the rest
    }
    CHECK_EQ(status, 0);
    // sum 15, max 5 (after reverse the first is 5), clamp(7,0,5) = 5
    CHECK_EQ(out.str(), std::string("15 5 5"));
}

TEST(a_missing_library_falls_back_to_the_same_named_ax_file) {
    // A library module can ship two implementations: the .axd declares a shared
    // library, and a same-named .ax implements the functions in Axiom. Both are
    // compiled in, so the program runs whether or not the .dll exists.
    // The library here can never be found, which is exactly the fallback path.
    TempFile decl("acvm_e2e_fbdecl.axd");
    decl.write("library \"acvm_e2e_fbdecl_impl\";\n"       // matches the .ax name
               "function fbAdd(int a, int b):int;\n"
               "function fbScale(int x):int;\n");

    TempFile fallback("acvm_e2e_fbdecl_impl.ax");
    fallback.write("function fbAdd(int a, int b):int {\n"
                   "    return a + b;\n"
                   "}\n"
                   "function fbScale(int x):int {\n"
                   "    return fbAdd(x, x);\n"              // calls its own fallback
                   "}\n");

    TempFile source("acvm_e2e_fbuser.ax");
    source.write("function import (return:(acvm_e2e_fbdecl));\n"
                 "function main():void {\n"
                 "    print(fbAdd(2, 3)); print(\" \"); print(fbScale(7));\n"
                 "    eof: pass;\n"
                 "}\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.runSource(source.path(), {}), 0);
    CHECK_EQ(out.str(), std::string("5 14"));
    // Falling back is not an error, so nothing should be reported.
    CHECK(err.str().empty());
}

TEST(a_library_without_a_fallback_still_requires_the_shared_object) {
    // The pairing is optional: a function that genuinely needs C is simply left
    // out of the .ax, and then the library really is required.
    TempFile decl("acvm_e2e_nofb.axd");
    decl.write("library \"acvm_e2e_nofb_no_such_lib\";\n"
               "function needsNative(ptr int data, int count):int64;\n");

    TempFile source("acvm_e2e_nofbuser.ax");
    source.write("import \"acvm_e2e_nofb.axd\";\n"
                 "function main():void {\n"
                 "    int a[2] = {1, 2};\n"
                 "    needsNative(#a[0], 2);\n"
                 "    eof: pass;\n"
                 "}\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK(driver.runSource(source.path(), {}) != 0);
    CHECK(err.str().find("external call failed") != std::string::npos);
}

TEST(a_fallback_file_may_not_declare_its_own_library) {
    TempFile decl("acvm_e2e_conflictdecl.axd");
    decl.write("library \"acvm_e2e_conflictimpl\";\n"
               "function clash(int x):int;\n");

    TempFile fallback("acvm_e2e_conflictimpl.ax");
    fallback.write("library \"acvm_e2e_conflictimpl\";\n"   // wrong: a .ax fallback
                   "function clash(int x):int;\n");

    TempFile source("acvm_e2e_conflictuser.ax");
    source.write("function import (return:(acvm_e2e_conflictdecl));\n"
                 "function main():void { eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK(driver.checkSource(source.path(), {}) != 0);
    CHECK(err.str().find("must not declare a library of its own") != std::string::npos);
}

TEST(a_fallback_with_the_wrong_return_type_is_reported_once) {
    // The declaration and the Axiom fallback share one FuncInfo, so a mismatch
    // means the fallback does not implement what was declared. One clear error,
    // not a cascade from checking the body against the declared type.
    TempFile decl("acvm_e2e_sigmismatch.axd");
    decl.write("library \"acvm_e2e_sigmismatch_impl\";\n"
               "function sigFn(int a):int64;\n");

    TempFile fallback("acvm_e2e_sigmismatch_impl.ax");
    fallback.write("function sigFn(int a):void {\n"
                   "    print(a);\n"
                   "}\n");

    TempFile source("acvm_e2e_siguser.ax");
    source.write("function import (return:(acvm_e2e_sigmismatch));\n"
                 "function main():void { eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK(driver.checkSource(source.path(), {}) != 0);
    CHECK(err.str().find("returns 'void', but the library declaration returns 'int64'") !=
          std::string::npos);
    // Exactly one "error:" line: no follow-up from the body check.
    std::size_t errors = 0;
    for (std::size_t at = err.str().find("error:"); at != std::string::npos;
         at = err.str().find("error:", at + 1)) {
        ++errors;
    }
    CHECK_EQ(errors, std::size_t(1));
}

TEST(a_pointer_parameter_without_a_count_is_rejected) {
    // Only `ptr T` immediately followed by an integer becomes an array pair.
    // A bare `ptr` would cross as a raw VM address, and the first dereference in
    // the native code would crash the process with no diagnostic at all -- which
    // is how this rule came to exist.
    TempFile decl("acvm_e2e_bareptr.axd");
    decl.write("library \"acvm_e2e_bareptr_lib\";\n"
               "function takesTwoPointers(ptr int first, ptr int second, int count):int64;\n");

    TempFile source("acvm_e2e_bareptruser.ax");
    source.write("import \"acvm_e2e_bareptr.axd\";\n"
                 "function main():void { eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK(driver.checkSource(source.path(), {}) != 0);
    CHECK(err.str().find("with no element count after it") != std::string::npos);
    CHECK(err.str().find("ptr T name, int count") != std::string::npos);
}

TEST(a_pointer_followed_by_a_count_is_accepted) {
    // The control for the rule above: two proper pairs are fine.
    TempFile decl("acvm_e2e_pairptr.axd");
    decl.write("library \"acvm_e2e_pairptr_lib\";\n"
               "function compare(ptr int first, int firstCount, ptr int second, int secondCount):bool;\n");

    TempFile source("acvm_e2e_pairptruser.ax");
    source.write("import \"acvm_e2e_pairptr.axd\";\n"
                 "function main():void { eof: pass; }\n");

    std::ostringstream out;
    std::ostringstream err;
    acvm::Driver driver(out, err);
    CHECK_EQ(driver.checkSource(source.path(), {}), 0);
    CHECK(err.str().empty());
}

TEST_MAIN()










