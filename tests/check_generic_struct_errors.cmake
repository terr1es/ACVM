# Asserts the generic-struct diagnostics name the actual problem.
#
# `method_needs_op` is the interesting one: it proves method bodies are checked
# lazily. `Pair<Point>` instantiates fine, and only calling `sum` — whose body
# needs `+`, which a `Point` payload does not have — reports the operator. Eager
# instantiation would have made merely *naming* `Pair<Point>` an error, which
# would rule out any container with a method that only some element types support.
#
#     cmake -DACVM=<acvm> -DSOURCE_DIR=<tests/generic_struct_errors> -P check_generic_struct_errors.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "check_generic_struct_errors.cmake needs -DACVM=<path> and -DSOURCE_DIR=<path>")
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

# A generic type used without arguments.
expect_error(bare.ax "needs type arguments")

# The wrong number of them.
expect_error(arg_count.ax "takes 1 type argument(s)")

# Arguments given to something that is not generic.
expect_error(not_generic.ax "takes no type arguments")

# A generic type cannot live in a native library: its layout would depend on
# Axiom types, which the library was compiled without.
expect_error(in_library.ax "declared in library")

# Lazy method bodies: the error appears at the operator inside the body, and only
# because the method was called.
expect_error(method_needs_op.ax "requires a numeric operand")

message(STATUS "${SOURCE_DIR}: 5 generic-struct diagnostics verified")
