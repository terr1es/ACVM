# Asserts the `module` directive's rules, and that a valid declaration is inert.
#
# The last case matters as much as the rejections: a declaration that changed how a
# file runs would be a parsing bug, and one that reserved `version` globally would
# break source that never uses the directive.
#
#     cmake -DACVM=<acvm> -DSOURCE_DIR=<tests/module_errors> -P check_module.cmake

if(NOT DEFINED ACVM OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "check_module.cmake needs -DACVM=<path> and -DSOURCE_DIR=<path>")
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

# The name is checked against the file it is written in.
expect_error(mismatch.ax "declares module 'other', but it is 'mismatch.ax'")

# A version is MAJOR.MINOR.PATCH, all three.
expect_error(badversion.ax "a module version is 'MAJOR.MINOR.PATCH'")

# One identity per file.
expect_error(duplicate.ax "already declares module 'duplicate'")

# A valid declaration must be completely inert, and must not reserve `version`
# as an identifier.
execute_process(
    COMMAND "${ACVM}" run "${SOURCE_DIR}/good.ax"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "good.ax: expected it to run, exit code ${rc}:\n${out}\n${err}")
endif()

string(STRIP "${out}" actual)
if(NOT actual STREQUAL "value 5")
    message(FATAL_ERROR "good.ax: wrong output, expected 'value 5', got '${actual}'")
endif()

message(STATUS
    "${SOURCE_DIR}: module directive rejects bad names/versions/duplicates and stays inert")
