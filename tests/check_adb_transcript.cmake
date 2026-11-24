# Runs a scripted debugger session and diffs the transcript.
#
# A debugger is a conversation, so the test is the transcript: the expected file
# is what a user would see, byte for byte. Asserting on a few regexes instead would
# let the format rot (a missing column, a reordered field) without any test
# noticing, which is the one thing a listing cannot afford.
#
#     cmake -DACVM=... -DSCRIPT=... -DPROGRAM=... -DEXPECTED=... \
#           -P check_adb_transcript.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SCRIPT OR NOT DEFINED PROGRAM OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR "check_adb_transcript.cmake needs ACVM, SCRIPT, PROGRAM and EXPECTED")
endif()

execute_process(
    COMMAND "${ACVM}" run --adb --adb-script "${SCRIPT}" "${PROGRAM}"
    OUTPUT_VARIABLE actual
    ERROR_VARIABLE errors
    RESULT_VARIABLE status)

# The program traps on purpose, so a non-zero exit is expected; what matters is
# that the message reached stderr, because that is what a user without a debugger
# would have seen.
if(NOT errors MATCHES "runtime error: division by zero")
    message(FATAL_ERROR "expected the trap to be reported on stderr, got:/n${errors}")
endif()

# The expected file is checked in with LF endings; the run may produce CRLF on
# Windows because C stdio and the VM's stream can translate differently. Content
# is what the transcript asserts, so endings are normalised first.
string(REPLACE "\r\n" "\n" actual "${actual}")
file(READ "${EXPECTED}" expected)
string(REPLACE "\r\n" "\n" expected "${expected}")

if(NOT actual STREQUAL expected)
    # Report the first differing line rather than 60 lines of both transcripts.
    string(REPLACE "\n" ";" actual_lines "${actual}")
    string(REPLACE "\n" ";" expected_lines "${expected}")
    list(LENGTH actual_lines actual_count)
    list(LENGTH expected_lines expected_count)
    set(index 0)
    while(index LESS actual_count AND index LESS expected_count)
        list(GET actual_lines ${index} got)
        list(GET expected_lines ${index} want)
        if(NOT got STREQUAL want)
            math(EXPR human "${index} + 1")
            message(FATAL_ERROR
                "transcript differs at line ${human}:\n  expected: ${want}\n  actual:   ${got}")
        endif()
        math(EXPR index "${index} + 1")
    endwhile()
    message(FATAL_ERROR
        "transcript length differs: expected ${expected_count} line(s), got ${actual_count}")
endif()

message(STATUS "debugger transcript matches (${status})")
