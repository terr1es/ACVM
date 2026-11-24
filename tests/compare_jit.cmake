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

if(NOT interpretedOut STREQUAL compiledOut)
    message(FATAL_ERROR
        "${SOURCE}: output differs between the interpreter and the JIT.\n"
        "--- interpreted ---\n${interpretedOut}\n--- compiled ---\n${compiledOut}")
endif()

message(STATUS "${SOURCE}: the interpreter and the JIT agree")
