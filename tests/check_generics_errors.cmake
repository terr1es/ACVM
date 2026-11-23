# Asserts that the generic-function diagnostics say the right thing.
#
# Error messages are the whole interface here: a third-party library author meets
# generics through them, not through a spec. So each case below checks both that
# the compiler rejects the program and that the message names the actual problem.
# A diagnostic that fires for the wrong reason is worse than none, because it
# sends the reader after a bug that does not exist.
#
#     cmake -DACVM=<acvm> -DSOURCE_DIR=<tests/generics_errors> -P check_generics_errors.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "check_generics_errors.cmake needs -DACVM=<path> and -DSOURCE_DIR=<path>")
endif()

# Runs `acvm check` on one file and requires a non-zero exit plus a message
# fragment. `forbidden` (optional) must NOT appear -- that is how a wrong-but-
# plausible message gets caught.
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

# A type parameter that no argument can pin down.
expect_error(no_infer.ax "cannot infer 'T'")

# The same `T` on two parameters, with arguments of different types.
expect_error(type_conflict.ax "does not match the declared parameter")

# A generic with no body: nothing to specialise.
expect_error(no_body.ax "has no body")

# `T` inferred as a struct, and the template does arithmetic on it. The message
# has to name the operator and the substituted type, and must not blame
# signedness -- two identical struct types cannot have a signedness clash.
expect_error(struct_arith.ax "requires a numeric operand" "mixed signed and unsigned")

message(STATUS "${SOURCE_DIR}: 4 generic diagnostics verified")
