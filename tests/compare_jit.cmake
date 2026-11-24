# Runs one Axiom program twice -- interpreted, then with the JIT enabled -- and fails
# unless the two produce identical output and the same exit status.
#
# This is the check that holds the lowering to its contract. An encoder bug produces code
# that runs and computes something slightly different; nothing about the program's source
# would reveal it, and no unit test of a single instruction would catch it either. Running
# the whole program both ways is the only test that covers "compiling a function did not
# change what the program means".
#
#     cmake -DACVM=<acvm> -DSOURCE=<file.ax> [-DARGS=a,b] -P compare_jit.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SOURCE)
    message(FATAL_ERROR "compare_jit.cmake needs -DACVM=<path> and -DSOURCE=<path>")
endif()

execute_process(
    COMMAND "${ACVM}" run "${SOURCE}" -O2 ${ARGS}
    RESULT_VARIABLE interpretedRc
    OUTPUT_VARIABLE interpretedOut
    ERROR_VARIABLE interpretedErr)

execute_process(
    COMMAND "${ACVM}" run "${SOURCE}" -O2 --jit ${ARGS}
    RESULT_VARIABLE compiledRc
    OUTPUT_VARIABLE compiledOut
    ERROR_VARIABLE compiledErr)

if(NOT interpretedRc EQUAL compiledRc)
    message(FATAL_ERROR
        "${SOURCE}: exit status differs -- interpreted ${interpretedRc}, compiled ${compiledRc}\n"
        "interpreted stderr:/n${interpretedErr}/ncompiled stderr:/n${compiledErr}")
endif()

# Agreeing is not the same as passing, and this is the gap that made every assertion in
# these tests decorative. A program whose checks all fail exits non-zero *in both modes*,
# so the two runs agree perfectly and the comparison above is satisfied. Test programs are
# expected to succeed, so a non-zero status is a failure of the test regardless of whether
# the JIT reproduced it faithfully.
#
# -DEXPECT_RC overrides this for a test that is meant to fail.
if(DEFINED EXPECT_RC)
    set(expectedRc "${EXPECT_RC}")
else()
    set(expectedRc 0)
endif()
if(NOT interpretedRc EQUAL expectedRc)
    message(FATAL_ERROR
        "${SOURCE}: the interpreter exited ${interpretedRc}, expected ${expectedRc}.\n"
        "The program failed on its own terms -- a failing check, a trap, or a bad exit -- "
        "so there is nothing for the JIT comparison to prove. stderr:\n${interpretedErr}")
endif()

if(NOT interpretedOut STREQUAL compiledOut)
    message(FATAL_ERROR
        "${SOURCE}: output differs between the interpreter and the JIT.\n"
        "--- interpreted ---\n${interpretedOut}\n--- compiled ---\n${compiledOut}")
endif()

# A program where everything is refused also agrees with the interpreter, so the check
# above can pass without compiling a single function. Callers that care -- a test written
# specifically to exercise the compiled path -- pass -DEXPECT_COMPILED=<min> and the run
# fails unless at least that many functions were compiled.
if(DEFINED EXPECT_COMPILED)
    execute_process(
        COMMAND "${ACVM}" run "${SOURCE}" -O2 --jit --stats ${ARGS}
        RESULT_VARIABLE statsRc
        OUTPUT_QUIET
        ERROR_VARIABLE statsErr)

    string(REGEX MATCH "jit: ([0-9]+) compiled" compiledMatch "${statsErr}")
    set(compiledCount "${CMAKE_MATCH_1}")
    if(NOT compiledCount)
        message(FATAL_ERROR
            "${SOURCE}: could not read the compiled count from --stats.\n${statsErr}")
    endif()
    if(compiledCount LESS EXPECT_COMPILED)
        message(FATAL_ERROR
            "${SOURCE}: only ${compiledCount} function(s) were compiled, expected at "
            "least ${EXPECT_COMPILED}.\nThe test would have passed without exercising "
            "compiled code. Refusals:\n${statsErr}")
    endif()
endif()

message(STATUS "${SOURCE}: the interpreter and the JIT agree")
