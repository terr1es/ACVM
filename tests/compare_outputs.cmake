# Runs an Axiom program at several optimization levels and fails unless every
# level prints exactly what the reference level prints.
#
# This is the check that holds the optimizer to its contract. A pass can be wrong
# in ways no unit test of the pass itself would notice: the program still runs, it
# just computes something slightly different. Comparing the unoptimized run with
# the optimized ones is the only test that covers "no rewrite changed what the
# program means" without hand-writing the expected output of every program.
#
# The level list matters. -O4 deletes safe points, so it is *not* flow-preserving
# and must not be applied to programs that depend on scheduling; those run with the
# default list only.
#
#     cmake -DACVM=<acvm> -DSOURCE=<file.ax> [-DLEVELS="O0,O3,O4"] [-DARGS=a,b]
#           -P compare_outputs.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SOURCE)
    message(FATAL_ERROR "compare_outputs.cmake needs -DACVM=<path> and -DSOURCE=<path>")
endif()

if(NOT DEFINED LEVELS)
    set(LEVELS "O0,O3")
endif()

# Comma separated on the way in: CMake treats `;` in a -D value as a list
# separator, so a semicolon list either splits into stray arguments or arrives as
# one literal string depending on how it is escaped. Commas sidestep both.
string(REPLACE "," ";" LEVELS "${LEVELS}")

list(GET LEVELS 0 REFERENCE_LEVEL)

foreach(level IN LISTS LEVELS)
    execute_process(
        COMMAND "${ACVM}" run "${SOURCE}" "-${level}" ${ARGS}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "${SOURCE} failed at -${level} with exit code ${rc}\nstderr:\n${err}")
    endif()
    set(output_${level} "${out}")
    set(stderr_${level} "${err}")
endforeach()

foreach(level IN LISTS LEVELS)
    if(level STREQUAL REFERENCE_LEVEL)
        continue()
    endif()
    if(NOT output_${REFERENCE_LEVEL} STREQUAL output_${level})
        message(FATAL_ERROR
            "${SOURCE}: -${REFERENCE_LEVEL} and -${level} produce different output.\n"
            "--- ${REFERENCE_LEVEL} ---\n${output_${REFERENCE_LEVEL}}\n"
            "--- ${level} ---\n${output_${level}}")
    endif()
    if(NOT stderr_${REFERENCE_LEVEL} STREQUAL stderr_${level})
        message(FATAL_ERROR
            "${SOURCE}: -${REFERENCE_LEVEL} and -${level} differ on stderr.\n"
            "--- ${REFERENCE_LEVEL} ---\n${stderr_${REFERENCE_LEVEL}}\n"
            "--- ${level} ---\n${stderr_${level}}")
    endif()
endforeach()

message(STATUS "${SOURCE}: identical at ${LEVELS}")
