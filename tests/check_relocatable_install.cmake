# Checks that an installed toolchain is self contained.
#
# The bug this exists for: `cmake --install` copied the executable and the `.dll`s
# but not the module sources, and the compiler's compile-time default for "where is
# stdlib" pointed into the source tree of the machine that built it. So a program in
# any other directory failed to `import (io)` -- on the build machine it found a
# stale copy behind that path, everywhere else it found nothing.
#
# Two halves, both checked here:
#
#   1. the install ships the module sources
#   2. the binary searches for them relative to itself, not only through the
#      compile-time default
#
# The second half is checked with a probe module written *only* into the installed
# directory: if the executable did not look there, `import` would fail. Checking it
# any other way would pass on a build machine simply because the source tree happens
# to still be at the baked-in path.
#
#     cmake -DACVM=<exe> -DBUILD_DIR=<build> -DPREFIX=<scratch prefix> \
#           -P check_relocatable_install.cmake

if(NOT DEFINED ACVM OR NOT DEFINED BUILD_DIR OR NOT DEFINED PREFIX)
    message(FATAL_ERROR "check_relocatable_install.cmake needs ACVM, BUILD_DIR and PREFIX")
endif()

file(REMOVE_RECURSE "${PREFIX}")

# `cmake --install` rather than copying by hand: the point is to test the install
# rules themselves, and a hand-rolled copy would keep passing if they regressed.
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}"
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_errors
    RESULT_VARIABLE install_status)
if(NOT install_status EQUAL 0)
    message(FATAL_ERROR "cmake --install failed://n${install_output}//n${install_errors}")
endif()

# 1. the sources are part of the install
foreach(required io.axd str.ax json.ax)
    if(NOT EXISTS "${PREFIX}/share/acvm/stdlib/${required}" AND
       NOT EXISTS "${PREFIX}/share/acvm/extralib/${required}")
        message(FATAL_ERROR
            "the install does not ship ${required}; a compiler needs the module "
            "sources at compile time, not just the executable")
    endif()
endforeach()

# 2. the executable finds modules next to itself.
#
# The probe names a module that exists in no source tree, so the only way the import
# can succeed is through the directory beside the installed binary.
file(WRITE "${PREFIX}/share/acvm/stdlib/relocprobe.ax"
     "module \"relocprobe\" version \"0.1.0\";\n\nfunction relocprobe():int {\n    return 4242;\n    eof: pass;\n}\n")

set(scratch "${PREFIX}/../reloc_scenario")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}")
file(WRITE "${scratch}/prog.ax"
     "function import (return:(io));\nfunction import (return:(relocprobe));\n\nfunction main():void {\n    print(pathJoin(\"a\", \"b\"));\n    print(\" \");\n    print(relocprobe());\n    print(\"\\n\");\n    eof: pass;\n}\n")

# The environment overrides are emptied so a value in the test runner's shell cannot
# stand in for the fallback being checked.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ACVM_STDLIB= ACVM_EXTRALIB=
            "${PREFIX}/bin/acvm${CMAKE_EXECUTABLE_SUFFIX}" run prog.ax
    WORKING_DIRECTORY "${scratch}"
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_errors
    RESULT_VARIABLE run_status)

if(NOT run_status EQUAL 0)
    message(FATAL_ERROR "a program in another directory could not import a module "
                        "from the install://n${run_output}//n${run_errors}")
endif()
if(NOT run_output MATCHES "4242")
    message(FATAL_ERROR "unexpected output from the relocated run: [${run_output}]")
endif()

message(STATUS "the install is self contained (${run_output})")
