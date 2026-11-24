# Runs a program twice -- once plain, once under the debugger with no stops -- and
# requires the two outputs to match.
#
#     cmake -DACVM=... -DPROGRAM=... -P check_adb_passthrough.cmake

if(NOT DEFINED ACVM OR NOT DEFINED PROGRAM)
    message(FATAL_ERROR "check_adb_passthrough.cmake needs ACVM and PROGRAM")
endif()

execute_process(COMMAND "${ACVM}" run "${PROGRAM}"
                OUTPUT_VARIABLE plain
                ERROR_VARIABLE plain_errors RESULT_VARIABLE plain_status)

# An empty script: the debugger attaches, stops at the entry point, reads EOF and
# continues. Everything after that must be identical to the plain run.
set(empty_script "${CMAKE_CURRENT_BINARY_DIR}/adb_empty.txt")
file(WRITE "${empty_script}" "")

execute_process(COMMAND "${ACVM}" run --adb --adb-script "${empty_script}" "${PROGRAM}"
                OUTPUT_VARIABLE debugged
                ERROR_VARIABLE debug_errors RESULT_VARIABLE debug_status)

if(NOT plain_status EQUAL 0)
    message(FATAL_ERROR "the plain run failed (${plain_status}): ${plain_errors}")
endif()
if(NOT debug_status EQUAL 0)
    message(FATAL_ERROR "the debugged run failed (${debug_status}): ${debug_errors}")
endif()

# Line endings are normalised first: the debugger writes through C stdio and the
# program through the VM's stream, and on Windows those two can differ on CRLF --
# an artefact of the CRT, not of the debugger.
string(REPLACE "\r\n" "\n" plain "${plain}")
string(REPLACE "\r\n" "\n" debugged "${debugged}")

# Then the debugger's own lines are removed. A stop is a header plus one indented
# instruction; the closing banner is one line. Nothing else may be touched.
string(REGEX REPLACE "\\[[0-9]+\\] (stop|breakpoint)[^\n]*\n[^\n]*\n" "" debugged "${debugged}")
string(REPLACE "== program finished ==\n" "" debugged "${debugged}")
string(STRIP "${debugged}" debugged)
string(STRIP "${plain}" plain)

if(NOT plain STREQUAL debugged)
    message(FATAL_ERROR "the debugger changed the program's output.\n"
                        "plain:    [${plain}]\n"
                        "debugged: [${debugged}]")
endif()

message(STATUS "the debugger does not perturb the program")
