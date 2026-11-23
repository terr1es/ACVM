# Asserts that constant expressions certain to trap are reported at compile time.
#
# Two things are being checked, and the second matters as much as the first:
#
#   * the five trapping lines in `const_trap.ax` produce exactly five warnings,
#     each naming the reason;
#   * the `safe` lines next to them produce none. A compiler that flags
#     `2147483646 + 1` trains people to ignore the diagnostic, at which point it
#     is worth nothing.
#
# It also checks that `check` still succeeds: these are warnings, not errors,
# because the expressions sit inside a branch that never runs.
#
#     cmake -DACVM=<acvm> -DSOURCE=<file.ax> -P check_const_trap.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SOURCE)
    message(FATAL_ERROR "check_const_trap.cmake needs -DACVM=<path> and -DSOURCE=<path>")
endif()

execute_process(
    COMMAND "${ACVM}" check "${SOURCE}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "check failed with exit code ${rc}; constant traps must be warnings, not errors.\n"
        "stdout:\n${out}\nstderr:\n${err}")
endif()

set(combined "${out}\n${err}")

# Counting by splitting on the needle: `string(FIND ... offset)` rejects some
# offset arguments in this CMake version, and a helper that works everywhere is
# worth more here than a clever one.
# A unique token, not a newline: the text being searched is compiler output, which
# has plenty of newlines of its own.
macro(count_occurrences text needle out_var)
    string(REPLACE "${needle}" "@@MARK@@" occurrences "${text}")
    string(REGEX MATCHALL "@@MARK@@" matches "${occurrences}")
    list(LENGTH matches ${out_var})
endmacro()

count_occurrences("${combined}" "constant zero divisor" zero_count)
count_occurrences("${combined}" "constant arithmetic overflows" overflow_count)
count_occurrences("${combined}" "constant shift amount is out of range" shift_count)

if(NOT zero_count EQUAL 2)
    message(FATAL_ERROR "expected 2 zero-divisor warnings, got ${zero_count}:\n${combined}")
endif()
if(NOT overflow_count EQUAL 2)
    message(FATAL_ERROR "expected 2 overflow warnings, got ${overflow_count}:\n${combined}")
endif()
if(NOT shift_count EQUAL 1)
    message(FATAL_ERROR "expected 1 shift warning, got ${shift_count}:\n${combined}")
endif()

# No false positives: the `safe:` lines in the source are all in that one file, so
# a total of exactly five warnings proves none of them was flagged.
count_occurrences("${combined}" "warning:" total_warnings)
if(NOT total_warnings EQUAL 5)
    message(FATAL_ERROR
        "expected exactly 5 warnings (one per trapping line, none for the safe ones), "
        "got ${total_warnings}:\n${combined}")
endif()

message(STATUS "${SOURCE}: 5 constant traps reported, no false positives")
