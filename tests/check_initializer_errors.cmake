# Asserts the initialiser diagnostics say what to write instead.
#
# Every case here used to be something else, and in each case the something else was worse
# than the message this checks for:
#
#   * a non-zero scalar against an aggregate was *accepted*, and at run time the scalar was
#     used as the source address of a memcpy -- a null-pointer trap for `= 0`, a wild read
#     for anything else. So the failure mode was not a bad message but a crash.
#   * `s.startsWith(x)` reported "has no method 'startsWith'", which sends the reader
#     looking for a typo in a method that was never going to exist.
#
# The `_ok` programs next to them are compiled too, and must succeed: a check that only
# asserts rejection would be satisfied by a compiler that rejects everything.
#
#     cmake -DACVM=<acvm> -DSOURCE_DIR=<tests/initializer_errors> -P check_initializer_errors.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "check_initializer_errors.cmake needs -DACVM=<path> and -DSOURCE_DIR=<path>")
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

# Both spellings carry the same advice, because the reader's next question is the same.
expect_error(bit_nonzero.ax "use '0' or '{0}' to zero it")
expect_error(struct_nonzero.ax "use '0' or '{0}' to zero it")

# The rewrite, with the receiver named, so the fix is mechanical.
expect_error(string_builtin_as_method.ax "'startsWith' is a builtin function, not a method")

# And the rejection must not be a blanket one: the zero forms compile.
execute_process(
    COMMAND "${ACVM}" check "${SOURCE_DIR}/zero_forms.ax"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "zero_forms.ax: the zero initialisers were refused, which they must not be:\n${out}${err}")
endif()

message(STATUS "initialiser diagnostics name the fix")
