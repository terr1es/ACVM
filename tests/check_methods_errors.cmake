# Asserts the method diagnostics name the actual problem.
#
# Two of these guard against silent no-ops rather than syntax errors, which is
# where a method feature usually goes wrong:
#
#   * a write through a value receiver would edit a copy the caller never sees,
#     so it has to be rejected instead of compiling into nothing;
#   * calling a static method on a value, or an instance method on a type, would
#     otherwise resolve to nothing and produce a confusing "no method" message.
#
#     cmake -DACVM=<acvm> -DSOURCE_DIR=<tests/methods_errors> -P check_methods_errors.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "check_methods_errors.cmake needs -DACVM=<path> and -DSOURCE_DIR=<path>")
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

# A write through a value receiver edits a copy: must be an error, not a no-op.
expect_error(value_write.ax "cannot assign to const parameter 'self'")

# `ptr self` needs an addressable object.
expect_error(temp_ptr_self.ax "needs a variable it can modify")

# Wrong receiver for the call form, both directions.
expect_error(instance_on_type.ax "needs an object")
expect_error(static_on_value.ax "is a static method")

# A missing method is a method problem, and the message must say "method", not
# "member" or "field".
expect_error(unknown.ax "has no method 'missing'" "has no member")

# A method needs a body to be compiled per instantiation.
expect_error(bodyless.ax "has no body")

# Generic methods are refused explicitly rather than half-supported.
expect_error(generic.ax "cannot be generic yet")

# `.` on a pointer must point at '->' instead of claiming the method is absent.
expect_error(dot_on_ptr.ax "with '->'" "has no method")

# The receiver has one spelling; a named one is almost certainly a mistake.
expect_error(receiver_named.ax "without a name")
expect_error(receiver_wrong_type.ax "must be written as 'self' or 'ptr self'")

# A read-only pointer receiver must stay read-only.
expect_error(const_ptr_write.ax "const parameter 'self'")

# One name cannot mix receiver forms: the receiver is built before overload
# resolution, so which form to build has to be determined by the name alone.
expect_error(mixed_receiver.ax "disagree about their receiver")

message(STATUS "${SOURCE_DIR}: 11 method diagnostics verified")
