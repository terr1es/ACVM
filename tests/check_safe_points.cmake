# Asserts that -O4 really removes the safe points.
#
# O4's contract is "same results, not necessarily the same execution flow", and the
# mechanism is deleting safe points. Without this check that contract would be a
# claim about code nobody looks at: a future pass could start re-inserting safe
# points, or the level could silently stop applying, and every other test would
# still pass.
#
#     cmake -DACVM=<acvm> -DSOURCE=<file with a process> -P check_safe_points.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SOURCE)
    message(FATAL_ERROR "check_safe_points.cmake needs -DACVM=<path> and -DSOURCE=<path>")
endif()

foreach(level O3 O4)
    execute_process(
        COMMAND "${ACVM}" ir "${SOURCE}" "-${level}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "${SOURCE}: 'ir -${level}' failed with exit code ${rc}\nstderr:\n${err}")
    endif()
    string(FIND "${out}" "SAFE_POINT" where_${level})
    set(has_${level} FALSE)
    if(NOT where_${level} EQUAL -1)
        set(has_${level} TRUE)
    endif()
endforeach()

if(NOT has_O3)
    message(FATAL_ERROR
        "${SOURCE}: -O3 emitted no SAFE_POINT at all, so this test cannot tell whether "
        "-O4 removes them. Does the program still have a `process` function?")
endif()

if(has_O4)
    message(FATAL_ERROR
        "${SOURCE}: -O4 still emits SAFE_POINT; safe-point removal is not running.")
endif()

message(STATUS "${SOURCE}: has safe points at -O3 and none at -O4")
