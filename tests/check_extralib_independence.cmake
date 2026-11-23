# Asserts that the standard library never imports an extra library module.
#
# This is the load-bearing wall of the `extralib` design. Its promise is that a
# user may delete any or all of `extralib/` and every program that does not
# import one of those modules behaves identically. One `import (json)` inside
# `stdlib/str.ax` would break that for programs that have nothing to do with
# JSON, and nothing else in the build would notice: it compiles, it links, and it
# only fails on the machine where the module happens to be missing.
#
# A rule that is not checked is a rule that will be broken by accident. This runs
# as a ctest so the accident is a red build instead of a support question.
#
# It parses import statements rather than searching for the module name. The
# first version searched, and reported `stdlib/os.axd` for mentioning `net.axd`
# in a comment explaining why sockets are NOT part of os -- a guard that flags
# documentation is a guard that gets deleted.
#
#     cmake -DSTDLIB_DIR=... -DEXTRALIB_DIR=... -P check_extralib_independence.cmake

if(NOT DEFINED STDLIB_DIR OR NOT DEFINED EXTRALIB_DIR)
    message(FATAL_ERROR "check_extralib_independence.cmake needs -DSTDLIB_DIR and -DEXTRALIB_DIR")
endif()

# What counts as an extra module: any file name in extralib/, minus its
# extension. `net.ax` and `net.axd` are one module, not two.
file(GLOB extralib_files "${EXTRALIB_DIR}/*.ax" "${EXTRALIB_DIR}/*.axd")
set(extra_modules "")
foreach(file IN LISTS extralib_files)
    get_filename_component(base "${file}" NAME_WE)
    list(APPEND extra_modules "${base}")
endforeach()
list(REMOVE_DUPLICATES extra_modules)

if(extra_modules STREQUAL "")
    message(STATUS "extralib/ has no modules: nothing stdlib could depend on")
    return()
endif()

file(GLOB stdlib_files "${STDLIB_DIR}/*.ax" "${STDLIB_DIR}/*.axd")
set(violations "")

foreach(file IN LISTS stdlib_files)
    file(READ "${file}" contents)
    get_filename_component(short "${file}" NAME)

    # Both spellings reach the same loader, so both are collected:
    #   function import (return:(io, str));      -> io, str
    #   import "lib/sort.ax";                    -> sort
    set(imported "")
    string(REGEX MATCHALL "return:\\([^)]*\\)" paren_imports "${contents}")
    foreach(statement IN LISTS paren_imports)
        string(REGEX REPLACE "^return:\\(|\\)$" "" inner "${statement}")
        string(REPLACE "," ";" parts "${inner}")
        foreach(part IN LISTS parts)
            string(STRIP "${part}" part)
            if(NOT part STREQUAL "")
                list(APPEND imported "${part}")
            endif()
        endforeach()
    endforeach()

    string(REGEX MATCHALL "import[ \t]*\"[^\"]*\"" path_imports "${contents}")
    foreach(statement IN LISTS path_imports)
        string(REGEX REPLACE "^import[ \t]*\"|\"$" "" path "${statement}")
        get_filename_component(base "${path}" NAME_WE)
        list(APPEND imported "${base}")
    endforeach()

    foreach(module IN LISTS imported)
        if(module IN_LIST extra_modules)
            list(APPEND violations "${short} imports '${module}'")
        endif()
    endforeach()
endforeach()

if(NOT violations STREQUAL "")
    string(REPLACE ";" "\n  " report "${violations}")
    message(FATAL_ERROR
        "the standard library must not depend on extralib:\n  ${report}\n\n"
        "stdlib is the part that is always present; an extra module may be deleted. "
        "Move the shared code down into stdlib, or keep the dependency inside extralib.")
endif()

string(REPLACE ";" ", " names "${extra_modules}")
message(STATUS "stdlib is independent of extralib (${names})")
