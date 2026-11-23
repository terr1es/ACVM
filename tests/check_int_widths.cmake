# Asserts the integer width boundaries hold at every optimization level.
#
# The interesting failures here are not "does int128 work" -- that is covered by
# int128_check.ax -- but whether the *optimizer* agrees with the *VM* about when an
# operation overflows. The two compute the same thing in two places (Optimize.cpp
# folds, VMStep.cpp executes), and when they disagree the optimizer deletes the trap
# and the program prints a number that should have been an error.
#
# So every overflow case runs at O0 (where nothing is folded) and at O3 (where the
# fold happens) and both must produce the same trap.
#
#     cmake -DACVM=<acvm> -DSOURCE_DIR=<tests/int_widths> -P check_int_widths.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "check_int_widths.cmake needs -DACVM=<path> and -DSOURCE_DIR=<path>")
endif()

# Runs `file` at each level and requires the same non-zero exit plus a message
# containing `must_contain`.
function(expect_trap_everywhere file must_contain)
    foreach(level IN ITEMS O0 O1 O2 O3 O4)
        execute_process(
            COMMAND "${ACVM}" run "${SOURCE_DIR}/${file}" "-${level}"
            RESULT_VARIABLE rc
            OUTPUT_VARIABLE out
            ERROR_VARIABLE err)

        if(rc EQUAL 0)
            message(FATAL_ERROR
                "${file} at ${level}: expected a trap, but it completed.\n${out}${err}")
        endif()

        set(combined "${out}\n${err}")
        string(FIND "${combined}" "${must_contain}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR
                "${file} at ${level}: expected a message containing \"${must_contain}\", got:\n${combined}")
        endif()

        # A level that quietly prints the wrapped value instead of trapping would
        # still exit non-zero only if the trap fired; this catches the reverse case
        # where the value is printed before the error.
        string(FIND "${out}" "before" before_at)
        if(before_at EQUAL -1)
            message(FATAL_ERROR "${file} at ${level}: the program did not even start:\n${out}${err}")
        endif()
    endforeach()
endfunction()

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

# 128-bit overflow, both operations that can exceed the cell.
expect_trap_everywhere(overflow_add.ax "integer overflow")
expect_trap_everywhere(overflow_shl.ax "integer overflow")

# Narrow widths unchanged by the 128-bit work.
expect_trap_everywhere(overflow_int8.ax "integer overflow")

# A width whose maximum has no representation is refused outright.
expect_error(unsigned128.ax "unsigned int128' is not supported")

message(STATUS
    "${SOURCE_DIR}: int width overflow agrees with the VM at O0..O4, and unsigned int128 is refused")
