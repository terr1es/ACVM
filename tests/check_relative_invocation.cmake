# Checks module resolution when the program is named by a *relative* path.
#
# The regression this exists for: `moduleSearchDirs` is given the importing file's
# directory, and for `acvm run prog.ax` that directory is `prog.ax`.parent_path()` ==
# "" -- which means the current directory. A change that skipped empty candidates (so
# that a genuinely missing directory would not be searched) therefore removed the
# caller's own directory from the search path whenever the path was relative.
#
# What that looked like from the outside:
#
#     $ acvm run prog.ax
#     error: cannot find module 'mylib' (looked for .../share/acvm/stdlib/mylib.axd, ...)
#
# -- the directory the file was sitting in was not even listed. Renaming the call to
# an absolute path made it work, which is exactly the kind of workaround that hides a
# bug instead of reporting it.
#
# Every existing check passed throughout, because they all invoke `acvm` with an
# absolute `${CMAKE_CURRENT_SOURCE_DIR}/tests/x.ax`. That is the gap being closed
# here: the invocation form itself is the variable.
#
#     cmake -DACVM=<exe> -DWORK=<scratch dir> -P check_relative_invocation.cmake

if(NOT DEFINED ACVM OR NOT DEFINED WORK)
    message(FATAL_ERROR "check_relative_invocation.cmake needs ACVM and WORK")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/sub")

file(WRITE "${WORK}/mylib.ax" [=[
module "mylib" version "0.1.0";

function triple(int x):int {
    return x * 3;
    eof: pass;
}
]=])

# One program, copied into both directories, importing a local module and a stdlib
# module by name.
file(WRITE "${WORK}/prog.ax" [=[
import (mylib);
import (io);

function main():void {
    print(pathJoin("k", "v"));
    print(" ");
    print(triple(7));
    print("\n");
    eof: pass;
}
]=])

file(COPY "${WORK}/prog.ax" "${WORK}/mylib.ax" DESTINATION "${WORK}/sub")

# Runs one invocation and requires "k/v 21", the output of the program above.
function(run_case label cwd program)
    execute_process(
        COMMAND "${ACVM}" run "${program}"
        WORKING_DIRECTORY "${cwd}"
        OUTPUT_VARIABLE out
        ERROR_VARIABLE errs
        RESULT_VARIABLE status)

    if(NOT status EQUAL 0)
        message(FATAL_ERROR "${label}: failed (${status})\n stdouts: ${out}\n stderr: ${errs}")
    endif()
    if(NOT out MATCHES "k/v 21")
        message(FATAL_ERROR "${label}: wrong output [${out}]")
    endif()
    message(STATUS "${label}: ok")
endfunction()

# 1. `acvm run prog.ax` -- the form that was broken; parent_path() is "".
run_case("bare relative path" "${WORK}" "prog.ax")

# 2. `acvm run ./prog.ax` -- parent_path() is ".", which never regressed. Kept so the
#    two forms stay pinned together: they must behave identically.
run_case("dot-slash relative path" "${WORK}" "./prog.ax")

# 3. `acvm run sub/prog.ax` -- parent_path() is a real directory.
run_case("subdirectory relative path" "${WORK}" "sub/prog.ax")

# 4. From inside the subdirectory, a bare name again, with the module beside it.
run_case("bare name inside a subdirectory" "${WORK}/sub" "prog.ax")

# 5. The diagnostic must list the importing directory first. A message that only lists
#    the installation directories sends the reader hunting in the wrong place.
file(WRITE "${WORK}/missing.ax" [=[
import (nosuchmodule);

function main():void {
    eof: pass;
}
]=])

execute_process(
    COMMAND "${ACVM}" run missing.ax
    WORKING_DIRECTORY "${WORK}"
    OUTPUT_VARIABLE missing_out
    ERROR_VARIABLE missing_errs)

if(NOT missing_errs MATCHES "nosuchmodule")
    message(FATAL_ERROR "a missing module was not reported:/n${missing_errs}")
endif()
string(FIND "${missing_errs}" "${WORK}" local_position)
string(FIND "${missing_errs}" "share/acvm/stdlib" install_position)
if(local_position LESS 0)
    message(FATAL_ERROR "the diagnostic does not mention the importing file's directory "
                        "(${WORK}):\n${missing_errs}")
endif()
if(install_position GREATER -1 AND local_position GREATER install_position)
    message(FATAL_ERROR "the importing file's directory is listed after the installation "
                        "directories, which is not where a reader looks first:/n${missing_errs}")
endif()

message(STATUS "a missing module names the importing directory first")
