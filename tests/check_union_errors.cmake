# Asserts the union diagnostics name the actual problem.
#
# The important one is `by_value_native`: passing an aggregate by value across the
# native boundary would be a silent truncation, so it must be a compile error and
# the message has to point at the workaround. The others guard against messages
# that fire for the wrong reason -- a union that denies being a union sends the
# reader to the wrong declaration.
#
#     cmake -DACVM=<acvm> -DSOURCE_DIR=<tests/union_errors> -P check_union_errors.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "check_union_errors.cmake needs -DACVM=<path> and -DSOURCE_DIR=<path>")
endif()

function(expect_error file must_contain)
    set(forbidden "")
    if(ARGC GREATER 2)
        set(forbidden "${ARGV2}")
    endif()

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

    if(NOT forbidden STREQUAL "")
        string(FIND "${combined}" "${forbidden}" wrong)
        if(NOT wrong EQUAL -1)
            message(FATAL_ERROR
                "${file}: the message mentions \"${forbidden}\", which is not the problem:\n${combined}")
        endif()
    endif()
endfunction()

# A union that contains itself has no size.
expect_error(self_contain.ax "cannot contain itself by value")

# A union with no members has no size either -- and must not silently become 0 bytes.
expect_error(empty.ax "has no members")

# One storage slot, so one initialiser.
expect_error(too_many_init.ax "at most one value")

# A wrong member name has to be reported as a member problem, and the message must
# say "union", not "struct".
expect_error(no_member.ax "union 'Num' has no member 'count'" "struct 'Num'")

# The silent-truncation trap: aggregates cannot cross the native boundary by value.
expect_error(by_value_native.ax "cannot cross that boundary by value" "float")

# The other half of that rule: `string` is an aggregate but the VM marshals it
# specially, so it must keep crossing. An earlier version of the check above
# rejected it too and the whole standard library stopped compiling.
function(expect_ok file)
    execute_process(
        COMMAND "${ACVM}" check "${SOURCE_DIR}/${file}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)

    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "${file}: expected the compiler to accept it, but it failed:\n${out}\n${err}")
    endif()
endfunction()

expect_ok(string_still_ok.ax)

message(STATUS "${SOURCE_DIR}: 5 union diagnostics verified, string marshalling intact")
