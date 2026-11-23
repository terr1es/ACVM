# Asserts that name resolution across files is scoped, not order-dependent.
#
# Three distinct failures are covered here, all of which were silent before:
#
#   1. Two imported files defining the same signature. The winner was decided by
#      import order, so adding an unrelated `import` could swap out a function the
#      program depended on -- with no diagnostic and no visible change at the call
#      site. Now an error.
#
#   2. `private` did not scope the *name*, only the calls. Two files each with a
#      `private helper` were merged into one entry that kept the first file's
#      identity and the second file's body, so a library calling its own helper was
#      told it belonged to another library. Importing one library broke another.
#
#   3. The mirror of 2: a program's own function was reported as private to a
#      library it had never heard of.
#
# 2 and 3 are asserted as programs that must run and print exact output -- they
# were never syntax errors, so only running them proves anything.
#
#     cmake -DACVM=<acvm> -DSOURCE_DIR=<tests/scope_errors> -P check_scope.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "check_scope.cmake needs -DACVM=<path> and -DSOURCE_DIR=<path>")
endif()

function(expect_error file must_contain)
    execute_process(
        COMMAND "${ACVM}" check "${SOURCE_DIR}/${file}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)

    if(rc EQUAL 0)
        message(FATAL_ERROR "${file}: expected the compiler to reject it, but it succeeded")
    endif()

    set(combined "${out}\n${err}")
    string(FIND "${combined}" "${must_contain}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR
            "${file}: expected a message containing \"${must_contain}\", got:\n${combined}")
    endif()
endfunction()

function(expect_output file expected)
    execute_process(
        COMMAND "${ACVM}" run "${SOURCE_DIR}/${file}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)

    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${file}: expected it to run, exit code ${rc}:\n${out}\n${err}")
    endif()

    # Trailing newline is whatever print() wrote; compare the rest exactly.
    string(STRIP "${out}" actual)
    string(STRIP "${expected}" wanted)
    if(NOT actual STREQUAL wanted)
        message(FATAL_ERROR "${file}: wrong output.\n  expected: ${wanted}\n  actual:   ${actual}")
    endif()
endfunction()

# 1. Two imported files, same signature, neither is the entry file.
expect_error(collide.ax "defined in two imported files")

# 2. Two libraries, each with its own `private helper`.
expect_output(private_ok.ax "lib_a 1\nlib_b 2")

# 3. The entry file's own function, beside a library's private one.
expect_output(user_helper.ax "mine 7\nlib  1")

# 4. Ordering an aggregate is not defined. It must be refused, not compared by
#    address -- see the fixture for what that produced.
expect_error(aggregate_order.ax "operator '<' is not defined for 'Point'")

message(STATUS
    "${SOURCE_DIR}: scoping and aggregate ordering verified (2 rejections, 2 programs that must run)")
