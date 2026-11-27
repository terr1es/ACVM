cmake_minimum_required(VERSION 3.20)

project(ACVM
    VERSION 0.2.0
    DESCRIPTION "ACVM - Axiom compiler and virtual machine"
    LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# An optimised build is the default: the VM is a hot interpreter loop and is
# roughly 20x slower when compiled without -O2, which makes every measurement
# meaningless. Configure with -DCMAKE_BUILD_TYPE=Debug when debugging.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "No build type given; defaulting to Release (-O2). "
                   "Use -DCMAKE_BUILD_TYPE=Debug for a debug build.")
endif()

option(ACVM_BUILD_TESTS "Build the ACVM test suite" ON)
option(ACVM_WERROR "Treat compiler warnings as errors" OFF)

# Does this host execute the code the JIT emits?
#
# The backend targets x86-64 and the assembler is portable C++ that would happily produce
# those bytes anywhere, so the architecture has to be checked explicitly wherever the
# output is going to be run: `compileFunction` refuses at run time (the CLI turns `--jit`
# into an error), and the JIT tests are skipped here rather than reported as failures on
# hardware that was never a target.
#
# Three spellings because the value comes from the platform rather than from CMake:
# Windows/MSVC and some MinGW builds report `AMD64`, Linux and macOS report `x86_64`, and
# `x64` shows up from a few generators.
string(TOUPPER "${CMAKE_SYSTEM_PROCESSOR}" ACVM_PROCESSOR_UPPER)
# Two conditions, not one. The backend emits Microsoft x64 code, so an x86-64 Linux or
# macOS host cannot run its output even though the architecture matches -- see the comment
# on `kJitSupported` in `include/acvm/VM/Jit.h`. Gating only on the processor is what let
# the JIT tests run on Linux and fail there, which looked like a broken test suite rather
# than a backend that does not target that ABI.
if(ACVM_PROCESSOR_UPPER MATCHES "^(X86_64|AMD64|X64|EM64T)$" AND WIN32)
    set(ACVM_HOST_IS_X64 TRUE CACHE INTERNAL "Host can run the JIT's output")
else()
    set(ACVM_HOST_IS_X64 FALSE CACHE INTERNAL "Host can run the JIT's output")
    message(STATUS
        "Skipping JIT tests: the backend emits x86-64 Microsoft ABI code, and this "
        "platform is ${CMAKE_SYSTEM_PROCESSOR} / ${CMAKE_SYSTEM_NAME}")
endif()

if(MSVC)
    add_compile_options(/W3 /utf-8 /permissive-)
    if(ACVM_WERROR)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wshadow)
    if(ACVM_WERROR)
        add_compile_options(-Werror)
    endif()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Make the `acvm` command available everywhere.
#
# The driver is also copied into the per-user local bin directory, which is
# already on PATH on both Windows (%USERPROFILE%\.local\bin) and Unix
# (~/.local/bin). After a normal build you can therefore type
#
#     acvm run examples/tour.ax
#
# from any directory, and it always runs the binary you just built.
# Configure with -DACVM_INSTALL_COMMAND=OFF to keep the build tree only.
# ---------------------------------------------------------------------------
option(ACVM_INSTALL_COMMAND "Copy acvm into the user's local bin directory" ON)

if(ACVM_INSTALL_COMMAND)
    if(WIN32)
        set(ACVM_USER_BIN "$ENV{USERPROFILE}/.local/bin")
    else()
        set(ACVM_USER_BIN "$ENV{HOME}/.local/bin")
    endif()

    if(ACVM_USER_BIN STREQUAL "/.local/bin" OR ACVM_USER_BIN STREQUAL "")
        message(WARNING "Cannot determine the home directory; `acvm` will not be "
                        "installed on PATH. Run `cmake --install` manually.")
    else()
        message(STATUS "acvm command will be installed to ${ACVM_USER_BIN}")
    endif()
endif()

# ---------------------------------------------------------------------------
# One static library per stage. Dependencies flow downwards only.
# ---------------------------------------------------------------------------
add_library(acvm_common STATIC
    src/Common/SourceManager.cpp
    src/Common/Diagnostic.cpp
    src/Common/Value.cpp)
target_include_directories(acvm_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(acvm_lex STATIC src/Lex/Lexer.cpp)
target_link_libraries(acvm_lex PUBLIC acvm_common)

add_library(acvm_ast STATIC src/AST/ASTPrinter.cpp src/AST/ASTClone.cpp)
target_link_libraries(acvm_ast PUBLIC acvm_lex)

add_library(acvm_parse STATIC src/Parse/Parser.cpp)
target_link_libraries(acvm_parse PUBLIC acvm_ast)

add_library(acvm_sema STATIC
    src/Sema/Builtin.cpp
    src/Sema/Type.cpp
    src/Sema/ConstEval.cpp
    src/Sema/Sema.cpp
    src/Sema/SemaStmt.cpp
    src/Sema/SemaExpr.cpp
    src/Sema/SemaSupport.cpp
    src/Sema/SemaGenerics.cpp
    src/Sema/SemaMethods.cpp)
target_link_libraries(acvm_sema PUBLIC acvm_parse)

add_library(acvm_ir STATIC
    src/IR/Bytecode.cpp
    src/IR/IRGen.cpp
    src/IR/IRGenExpr.cpp
    src/IR/Serializer.cpp
    src/IR/Disassembler.cpp
    src/IR/Optimize.cpp)
target_link_libraries(acvm_ir PUBLIC acvm_sema)

# Machine code generation. No dependency on the VM: the only thing this layer knows
# is how to make a page executable and how to spell an x86-64 instruction.
add_library(acvm_jit STATIC
    src/JIT/ExecutableMemory.cpp
    src/JIT/X64Assembler.cpp)
target_include_directories(acvm_jit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(acvm_vm STATIC
    src/VM/VM.cpp
    src/VM/VMStep.cpp
    src/VM/VMBuiltins.cpp
    src/VM/Jit.cpp
    # Standard library, implemented in C. Compiled as C, not C++: the point of
    # this file is that an outside implementation needs no knowledge of the VM.
    stdlib/algorithm.c)
target_link_libraries(acvm_vm PUBLIC acvm_ir acvm_jit)
# The VM's BUILTIN implementations call into the stdlib modules' C code.
target_include_directories(acvm_vm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stdlib)

# ---------------------------------------------------------------------------
# The official `extralib` module list.
#
# Why this is generated instead of written down: the compiler has to name the
# official extra modules for exactly one reason -- to say "this module exists but
# you do not have it" when an `import` fails. That message is most needed right
# after the module has been deleted, so the list cannot live inside `extralib/`;
# it would be deleted along with the thing it describes.
#
# So the list is derived from the directory at configure time and compiled into
# the driver. Deleting `extralib/json.ax` leaves the knowledge intact, which is
# the whole point. There is no file format, nothing to parse, and nothing that
# can drift: the directory is the only source of truth, and the header is a
# snapshot of it taken when the compiler itself was built.
#
# `CONFIGURE_DEPENDS` makes a new or removed extralib file trigger a reconfigure,
# so this snapshot cannot go stale during a normal build.
# ---------------------------------------------------------------------------
file(GLOB ACVM_EXTRALIB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/extralib/*.ax"
    "${CMAKE_CURRENT_SOURCE_DIR}/extralib/*.axd")

set(ACVM_EXTRALIB_NAMES "")
foreach(source IN LISTS ACVM_EXTRALIB_SOURCES)
    get_filename_component(base "${source}" NAME_WE)
    # Skip the portable fallbacks that pair with a native module: `net.axd` and
    # `net.ax` are one module named `net`, not two.
    list(APPEND ACVM_EXTRALIB_NAMES "${base}")
endforeach()
list(REMOVE_DUPLICATES ACVM_EXTRALIB_NAMES)
list(SORT ACVM_EXTRALIB_NAMES)
list(LENGTH ACVM_EXTRALIB_NAMES ACVM_EXTRALIB_COUNT)

set(ACVM_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${ACVM_GENERATED_DIR}")

# One entry per line, ready to drop into the template. `std::string_view` from a
# plain literal, so no `using namespace std::literals` is needed in the header.
set(ACVM_EXTRALIB_ENTRIES "")
foreach(name IN LISTS ACVM_EXTRALIB_NAMES)
    string(APPEND ACVM_EXTRALIB_ENTRIES "    \"${name}\",\n")
endforeach()
if(ACVM_EXTRALIB_ENTRIES STREQUAL "")
    string(APPEND ACVM_EXTRALIB_ENTRIES "    // extralib/ is empty: this build ships no official extra modules.\n")
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/OfficialExtraModules.h.in"
               "${ACVM_GENERATED_DIR}/OfficialExtraModules.h"
               @ONLY)

add_library(acvm_driver STATIC
    src/Driver/Driver.cpp
    src/Driver/CommandLine.cpp
    src/Driver/Payload.cpp)
target_link_libraries(acvm_driver PUBLIC acvm_vm)
target_include_directories(acvm_driver PRIVATE "${ACVM_GENERATED_DIR}")
# Where `import (name)` looks after the importing file's own directory. The
# build tree points at the source tree; an installed binary would override this.
target_compile_definitions(acvm_driver PRIVATE
    ACVM_STDLIB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/stdlib"
    # The optional layer. Same lookup, one directory later, so a module in here
    # is written and imported exactly like a stdlib one -- the difference is that
    # nothing else depends on it and it may be deleted.
    ACVM_EXTRALIB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/extralib"
    # Where the shared standard library is written, so programs run from any
    # directory find it.
    ACVM_LIB_DIR="${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# The shared half of the standard library.
#
# Each module is one shared object named after its source file, dropped beside
# acvm.exe so that `library "x"` finds x.dll / libx.so.
#
# These twelve lines of output-directory setup used to be copy-pasted per module
# and had already drifted: `add_dependencies` below listed six of the eight, so
# the two newest libraries were not ordered before the driver. One function and
# one list now, so adding a module is one entry plus its source file.
# ---------------------------------------------------------------------------
set(ACVM_STDLIB_MODULES
    stdlib      # acvm.exe's own primitives, declared by stddec.axd
    algorithm   # sort / search / fold over plain arrays
    random      # seeded PRNG
    io          # files, and the only way to read until EOF
    time        # four clock reads; everything else is pure Axiom
    os          # environment, process control, machine information
    parallel    # OS threads over plain data
    threadpool  # persistent workers plus real OS locks
    adb         # the Axiom Debugger: breakpoints, stepping, capture
)

# The official extra modules. Same shape as a stdlib module, different promise:
# nothing in `stdlib/` may depend on any of these, and a program that never
# imports one runs identically whether they are installed or not. That is what
# lets a user delete this whole directory without breaking the build.
#
# `net` lives here rather than in `stdlib`: the toolchain itself never opens a
# socket, so by the rule above it is not part of "what you need to write a
# program". Its transport-not-protocols design is the boundary -- TCP is here,
# HTTP and the rest belong above it.
set(ACVM_EXTRALIB_MODULES
    net         # TCP sockets; the transport only, protocols live in Axiom
)

function(acvm_add_native_module module directory)
    add_library(acvm_${module} SHARED ${directory}/${module}.c)
    set_target_properties(acvm_${module} PROPERTIES
        OUTPUT_NAME "${module}"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(ACVM_STDLIB_OUTPUT_DIR)
        set_target_properties(acvm_${module} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${ACVM_STDLIB_OUTPUT_DIR}"
            RUNTIME_OUTPUT_DIRECTORY "${ACVM_STDLIB_OUTPUT_DIR}")
    endif()
endfunction()

set(ACVM_STDLIB_TARGETS "")
foreach(module IN LISTS ACVM_STDLIB_MODULES)
    acvm_add_native_module(${module} stdlib)
    list(APPEND ACVM_STDLIB_TARGETS acvm_${module})
endforeach()

# A missing extralib source is not a configure error. The property this directory
# promises is that deleting it changes nothing for programs that do not import it,
# so the build has to survive its absence -- otherwise the promise would only hold
# for an installed toolchain and could never be tested from a checkout.
set(ACVM_EXTRALIB_TARGETS "")
foreach(module IN LISTS ACVM_EXTRALIB_MODULES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extralib/${module}.c")
        acvm_add_native_module(${module} extralib)
        list(APPEND ACVM_EXTRALIB_TARGETS acvm_${module})
    else()
        message(STATUS "extralib/${module}.c is absent: building without the '${module}' native module")
    endif()
endforeach()

# Only the two modules that spawn native threads need the platform thread
# library; the rest never touch it.
find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(acvm_parallel PRIVATE Threads::Threads)
    target_link_libraries(acvm_threadpool PRIVATE Threads::Threads)
endif()

# Winsock is not in the default link set on Windows; without this every socket
# call resolves to nothing at link time, and the failure names `socket` rather
# than the missing library.
if(WIN32 AND TARGET acvm_net)
    target_link_libraries(acvm_net PRIVATE ws2_32)
endif()

# BCryptGenRandom lives in bcrypt.lib, which is not in the default link set.
# Without it `randomSecure` fails to resolve at link time.
if(WIN32 AND TARGET acvm_random)
    target_link_libraries(acvm_random PRIVATE bcrypt)
endif()

add_executable(acvm tools/acvm/main.cpp)
target_link_libraries(acvm PRIVATE acvm_driver)

# A release archive has to run on a machine that has never seen this toolchain. With the
# default llvm-mingw settings the executable imports `libc++.dll` and `libunwind.dll`,
# which live in the toolchain's directory -- so a zip built without this starts on the
# build machine and fails on the user's with "the code execution cannot proceed because
# libc++.dll was not found". Found by inspecting the import table of an installed copy,
# not by running it, because running it here finds the DLLs.
#
# Only the executable: the module libraries under `stdlib/` and `extralib/` are C, and
# adding `-static` to a shared library would be ignored on Windows anyway.
if(MINGW)
    target_link_options(acvm PRIVATE -static)
endif()
add_dependencies(acvm ${ACVM_STDLIB_TARGETS} ${ACVM_EXTRALIB_TARGETS})

# The statically linked runtime. `apack --single` copies this one instead of `acvm.exe`, so
# the bundle needs no shared objects beside it: one file, no extraction, no temp directory.
#
# The standard library is compiled in rather than loaded, and every symbol it defines is
# exported so `vmMainModule()` can resolve it. `--export-all-symbols` rather than a
# generated .def: the set to export is exactly what the .c files define, and a list kept by
# hand is a list that goes stale the first time a function is added.
#
# All ten modules are linked rather than only the ones a given program reaches. Selecting
# per program would save a few hundred kilobytes and cost a link step at pack time -- which
# needs a linker on the machine doing the packing, and that machine is the user's.
set(ACVM_STATIC_SOURCES "")
foreach(module IN LISTS ACVM_STDLIB_MODULES)
    list(APPEND ACVM_STATIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/${module}.c")
endforeach()
foreach(module IN LISTS ACVM_EXTRALIB_MODULES)
    list(APPEND ACVM_STATIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/extralib/${module}.c")
endforeach()

add_executable(acvm_static tools/acvm/main.cpp ${ACVM_STATIC_SOURCES})
target_link_libraries(acvm_static PRIVATE acvm_driver)
set_target_properties(acvm_static PROPERTIES OUTPUT_NAME "acvm-static")
# `--export-all-symbols` is a PE/COFF concept. An ELF or Mach-O image exports its
# global symbols by default, and both GNU ld and Apple's ld64 reject the flag outright
# ("unrecognized option"), so leaving it unconditional broke the Linux and macOS builds
# the first time this target was compiled anywhere but a MinGW host.
if(WIN32)
    target_link_options(acvm_static PRIVATE "LINKER:--export-all-symbols")
endif()

# The same platform libraries the individual modules need, gathered here because this
# binary contains all of them.
if(Threads_FOUND)
    target_link_libraries(acvm_static PRIVATE Threads::Threads)
endif()
if(WIN32)
    target_link_libraries(acvm_static PRIVATE bcrypt ws2_32 psapi)
endif()
if(MINGW)
    target_link_options(acvm_static PRIVATE -static)
endif()

# Copying the binaries and the shared libraries to a user directory is a
# convenience, not part of building the project -- so it is its own target.
#
# It used to be a POST_BUILD step on `acvm`, and that was a mistake: the copy
# fails whenever the destination is in use (an `acvm` still running, a scanner
# holding it, a locked file on Windows), and a failing POST_BUILD fails the whole
# target *after* the linker has already deleted the old executable. One locked
# file in ~/.local/bin left the build tree with no acvm.exe at all.
#
# Run it explicitly when you want it:
#     cmake --build build --target install-local
if(ACVM_INSTALL_COMMAND AND NOT ACVM_USER_BIN STREQUAL "/.local/bin" AND NOT ACVM_USER_BIN STREQUAL "")
    set(ACVM_COPY_LIBRARIES "")
    foreach(target IN LISTS ACVM_STDLIB_TARGETS)
        list(APPEND ACVM_COPY_LIBRARIES
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${target}>" "${ACVM_USER_BIN}")
    endforeach()

    # `~/.local/share/acvm` mirrors what `cmake --install` writes under the prefix:
    # the module sources the compiler reads at compile time. Shipping only the
    # executable and the .dlls was the bug: `import (io)` then resolved through the
    # compile-time default, which names the machine that built the binary.
    set(ACVM_USER_SHARE "$ENV{USERPROFILE}/.local/share/acvm")
    if(NOT WIN32)
        set(ACVM_USER_SHARE "$ENV{HOME}/.local/share/acvm")
    endif()

    # Only `*.ax` and `*.axd` are copied, matching `cmake --install` above: the `.c`
    # files are already compiled into the shared libraries, and shipping a second
    # copy invites someone to edit the one that is not used.
    set(ACVM_MODULE_SOURCES "")
    foreach(layer stdlib extralib)
        foreach(pattern "*.ax" "*.axd")
            file(GLOB found "${CMAKE_CURRENT_SOURCE_DIR}/${layer}/${pattern}")
            list(APPEND ACVM_MODULE_SOURCES ${found})
        endforeach()
    endforeach()

    set(ACVM_COPY_MODULES "")
    foreach(source IN LISTS ACVM_MODULE_SOURCES)
        get_filename_component(layer "${source}" DIRECTORY)
        get_filename_component(layer "${layer}" NAME)
        list(APPEND ACVM_COPY_MODULES
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${source}" "${ACVM_USER_SHARE}/${layer}")
    endforeach()

    add_custom_target(install-local
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ACVM_USER_BIN}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:acvm>" "${ACVM_USER_BIN}"
        ${ACVM_COPY_LIBRARIES}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ACVM_USER_SHARE}/stdlib"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ACVM_USER_SHARE}/extralib"
        ${ACVM_COPY_MODULES}
        DEPENDS acvm ${ACVM_STDLIB_TARGETS}
        COMMENT "Installing acvm, its libraries and its modules into ${ACVM_USER_BIN}")
endif()

# Standard install rule, for `cmake --install build --prefix <somewhere>`.
include(GNUInstallDirs)
# `${ACVM_EXTRALIB_TARGETS}` belongs here too. The extralib modules are mostly pure
# Axiom, but `net` has a native half (`library "net"`) and ships as `net.dll` -- and
# leaving it out produced an install where `extralib/net.axd` was present and the
# library it declares was not, so `import (net)` failed on a fresh install while
# working fine in the build tree. A missing extralib source only removes the target
# from this list, so an extralib-less checkout still installs cleanly.
install(TARGETS acvm ${ACVM_STDLIB_TARGETS} ${ACVM_EXTRALIB_TARGETS}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR})

# The modules are source, not data the compiler was built from: `import (io)` reads
# stdlib/io.axd every time a program is compiled. An install that omits them ships a
# compiler that only works where it was built -- it would fall back to the
# compile-time path, which points into the build machine's source tree.
#
# `*.ax` and `*.axd` only: the `.c` files are already compiled into the shared
# libraries and shipping them would invite someone to edit a copy that is not used.
install(DIRECTORY stdlib/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/acvm/stdlib
        FILES_MATCHING PATTERN "*.ax" PATTERN "*.axd")
install(DIRECTORY extralib/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/acvm/extralib
        FILES_MATCHING PATTERN "*.ax" PATTERN "*.axd")
if(ACVM_BUILD_TESTS)
    enable_testing()
    function(acvm_add_test name)
        add_executable(${name} tests/${name}.cpp)
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_link_libraries(${name} PRIVATE acvm_driver)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    acvm_add_test(test_lexer)
    acvm_add_test(test_parser)
    acvm_add_test(test_sema)
    acvm_add_test(test_vm)
    acvm_add_test(test_e2e)
    acvm_add_test(test_cli)

    # The encoder is linked without the VM: a failure here is an encoding bug, never a
    # consequence of something the interpreter did.
    #
    # Guarded by target architecture. The assembler is portable C++ that emits x86-64
    # bytes either way, so it builds anywhere -- but this test *calls* what it emits, so on
    # a non-x86-64 host it would jump into machine code the CPU does not have. The runtime
    # guard in compileFunction cannot help here: that test exercises the encoder directly,
    # below the VM. `acvm --jit` refuses on such a host and the differential tests below
    # would then fail on the error, so the whole group is skipped rather than reported as
    # broken on hardware that was never a target.
    if(ACVM_HOST_IS_X64)
        add_executable(test_jit_assembler tests/test_jit_assembler.cpp)
        target_include_directories(test_jit_assembler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_link_libraries(test_jit_assembler PRIVATE acvm_jit)
        add_test(NAME test_jit_assembler COMMAND test_jit_assembler)
    endif()

    # Script checks come in two flavours.
    #
    # Most call stdlib/test.ax, which traps on failure -- the process exit code is
    # already the verdict, so they need nothing more. (Do NOT add a regex like
    # `[FAIL]` to those: it is a *character class* and matches any word with an
    # A, I, L or F in it, which made every one of them look broken once.)
    #
    # The older ones print `done` on success and their own MISMATCH lines on
    # failure, so they need the regex pair. That pair was repeated per test; one
    # helper each now.
    function(acvm_add_script_check name)
        add_test(NAME ${name}
                 COMMAND $<TARGET_FILE:acvm> run
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.ax ${ARGN})
    endfunction()

    function(acvm_add_legacy_check name)
        add_test(NAME ${name}
                 COMMAND $<TARGET_FILE:acvm> run
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.ax)
        set_tests_properties(${name} PROPERTIES
            PASS_REGULAR_EXPRESSION "done"
            FAIL_REGULAR_EXPRESSION "MISMATCH|FAIL|runtime error")
    endfunction()

    # The algorithm check exercises the C implementation when algorithm.dll is
    # present and the Axiom fallback when it is not, so one test covers both
    # paths. Move the library out of the search path and it still has to pass --
    # that is the portable-to-any-machine promise of the dual implementation.
    acvm_add_legacy_check(algorithm_check)
    # sort / sortInt64 are TimSort: the same shape of dual test -- with the DLL
    # present the C engine runs, without it the Axiom body in algorithm.ax does.
    acvm_add_legacy_check(timsort_check)
    acvm_add_legacy_check(time_check)
    acvm_add_legacy_check(astl_check)   # vector / set / map / hash: pure Axiom
    # `bitN` storage: the layout rule (five bit fields into one byte), the
    # read-modify-write that keeps neighbours intact, and the truncating cast.
    acvm_add_legacy_check(bit_check)
    # `**`: right associativity, precedence against `*` and unary, the 0-exponent
    # edges, and the per-width result types.
    acvm_add_legacy_check(pow_check)
    acvm_add_legacy_check(random_check)

    # The I/O check creates and deletes real files, so it runs in a scratch
    # directory inside the build tree: a failed run leaves its evidence there
    # instead of in the source tree. It is the one check that needs a working
    # directory, hence no helper.
    set(ACVM_IO_WORK "${CMAKE_CURRENT_BINARY_DIR}/io_check_work")
    file(MAKE_DIRECTORY "${ACVM_IO_WORK}")
    add_test(NAME io_check
             COMMAND $<TARGET_FILE:acvm> run ${CMAKE_CURRENT_SOURCE_DIR}/tests/io_check.ax
             WORKING_DIRECTORY "${ACVM_IO_WORK}")
    set_tests_properties(io_check PROPERTIES
        PASS_REGULAR_EXPRESSION "done"
        FAIL_REGULAR_EXPRESSION "MISMATCH|FAIL|runtime error")

    # A check that imports an extra module only exists while that module is
    # installed. Deleting extralib/ has to leave `ctest` green -- otherwise the
    # promise that the directory is optional would be false in the one tree where
    # anyone can actually verify it.
    #
    # Not registered, rather than registered-and-skipped: a test that always
    # reports "skipped" trains people to ignore the skip column. `CONFIGURE_DEPENDS`
    # on the extralib glob up top means adding the file back re-adds the check
    # without anyone having to remember a cmake invocation.
    # One check per extra module, registered only while the module it exercises
    # is present. This used to hardcode `net_check` and test for `net.ax`, which
    # worked for exactly one module: adding a second one silently registered a
    # check whose module could be deleted, and the "deleting extralib/ keeps
    # ctest green" promise broke. The loop keeps the promise for any number of
    # modules, including modules added later.
    # Pairs of "check:module it needs". `formats_check` exercises the whole
    # format family, whose shared foundation is value.ax.
    set(ACVM_EXTRALIB_CHECKS "")
    foreach(pair net_check:net formats_check:value tools_check:cli cli_dispatch_check:cli
                 import_forms_check:json)
        string(REPLACE ":" ";" parts "${pair}")
        list(GET parts 0 extra_check)
        list(GET parts 1 extra_module)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extralib/${extra_module}.ax")
            list(APPEND ACVM_EXTRALIB_CHECKS ${extra_check})
        else()
            message(STATUS "extralib/${extra_module}.ax is absent: ${extra_check} is not registered")
        endif()
    endforeach()

    foreach(check_name func_check algorithm_key_check testcases_check
                            str_check codec_check strsort_check map_check
                            fmt_check sort_index_check os_check sync_check
                            parallel_check threadpool_check generics_check union_check
                            generic_struct_check address_assign_check
                            ternary_check int128_check methods_check ${ACVM_EXTRALIB_CHECKS})
        acvm_add_script_check(${check_name})
    endforeach()

    # The one check that needs real arguments. Every other script check is registered with
    # none, which is why `cliParseArgvWith` could lose the first argument without any test
    # noticing: the others build a command line as a string, program name included.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extralib/cli.ax")
        acvm_add_script_check(cli_argv_check -- alpha -o beta --name gamma)
    endif()

    # Every other check invokes `acvm` with an absolute path, so the invocation form
    # was never a variable -- which is how a regression that dropped the importing
    # file's own directory for relative paths (`acvm run prog.ax`) got in: ctest
    # stayed green while `import (mylib)` failed in every real project.
    add_test(NAME relative_invocation
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DWORK=${CMAKE_BINARY_DIR}/relcheck
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_relative_invocation.cmake)

    # An installed toolchain has to work away from the machine that built it. This
    # installs into a scratch prefix, writes a program into a third directory, and
    # requires it to compile -- which fails if the install omits the module sources
    # or if the binary only looks for them through the compile-time default.
    add_test(NAME install_relocatable
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DBUILD_DIR=${CMAKE_BINARY_DIR}
                     -DPREFIX=${CMAKE_BINARY_DIR}/reloc_prefix
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_relocatable_install.cmake)

    # The debugger's test is a transcript: a scripted session is run and its
    # output is compared with the expected file. `adb` is a stdlib module, so the
    # check exists whenever the module and its library do.
    add_test(NAME adb_check
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/adb_check.txt
                     -DPROGRAM=${CMAKE_CURRENT_SOURCE_DIR}/tests/adb_check.ax
                     -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/adb_check.expected
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_adb_transcript.cmake)

    # A debugger that changes the program's behaviour is worse than no debugger.
    # Running the same program with and without the hook must produce identical
    # output, which also proves the hook costs nothing when it is not a stop.
    add_test(NAME adb_no_interference
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DPROGRAM=${CMAKE_CURRENT_SOURCE_DIR}/tests/adb_passthrough.ax
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_adb_passthrough.cmake)

    # Differential check for the JIT: every program below is run interpreted and then
    # compiled, and the two must agree byte for byte. Only programs with no dependence on
    # the clock or the environment are listed -- those differ between two runs of the
    # *same* mode, so they cannot say anything about the JIT.
    function(acvm_add_jit_diff_check name)
        add_test(NAME ${name}_jitdiff
                 COMMAND ${CMAKE_COMMAND}
                         -DACVM=$<TARGET_FILE:acvm>
                         -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.ax
                         ${ARGN}
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_jit.cmake)
    endfunction()

    # Skipped with the assembler test above, and for the same reason: `--jit` is an error
    # on a host that cannot execute the output, so these would fail on the flag rather
    # than on anything the programs do.
    if(ACVM_HOST_IS_X64)
        foreach(checked
                func_check algorithm_key_check jit_check ternary_check generics_check union_check generic_struct_check
                methods_check int128_check astl_check algorithm_check
                sync_check address_assign_check)
            acvm_add_jit_diff_check(${checked})
        endforeach()

        # This one exists to exercise compiled *calls*, not just compiled arithmetic, so it
        # also asserts that the compiled path was actually taken: every function in it could
        # be refused and the output would still match the interpreter.
        acvm_add_jit_diff_check(jit_call_check -DEXPECT_COMPILED=6)

        # A compiled function calling one the JIT refused. The direction matters: every
        # other program here calls only compilable functions, so the "compiled into
        # interpreted" path was unexercised. `jit_mixed_call_check` also covers mutual
        # recursion, arguments past the fourth (the point where the x64 ABI stops passing
        # them in registers) and a recursion depth that spans several frames.
        #
        # The count is this file's six compilable functions, so a regression that moves one
        # of them back to the interpreter fails here instead of quietly turning the program
        # into another all-interpreted comparison. If the lowering later learns to read a
        # global, `interpretedWorker` starts compiling too -- the count rises and this line
        # needs revisiting, which is the intended signal: the file then no longer contains a
        # refused callee and its whole point has to be re-established.
        acvm_add_jit_diff_check(jit_mixed_call_check -DEXPECT_COMPILED=6)

        # Strings and aggregates: CONST_STR, MEM_ZERO and MEM_COPY. A string is a 16 byte
        # descriptor rather than a register value, so `string s = "x";` is four
        # instructions and any one of them being unsupported used to refuse the whole
        # function -- which is how a single unused label kept arithmetic-heavy loops in
        # the interpreter. The count is the file's five compilable functions; a sixth
        # exists only to be refused, as the callee in the mixed-direction check.
        acvm_add_jit_diff_check(jit_string_check -DEXPECT_COMPILED=5)

        # The entry function. The interpreter only ever reached compiled code through
        # CALL, so `main` was interpreted no matter how much arithmetic it did -- and
        # since `main` is where a small program puts everything, that made the JIT
        # invisible exactly where it was most likely to be tried. Two functions here
        # compile: `main` and `work`.
        #
        # `report` is the third and is deliberately *not* counted: it opens with a
        # `print`, so there is no compiled prefix to speak of, and a hand-off at its first
        # instruction would compile a prologue and hand straight back. That case is
        # refused instead, and the count here is what notices if it stops being refused.
        acvm_add_jit_diff_check(jit_entry_check -DEXPECT_COMPILED=2)

        # A hand-off point inside a conditional inside a loop. The false branch jumps
        # *over* the hand-off, and a jump target past it that was bound to the wrong
        # place fell into the epilogue and returned a garbage result code -- a wrong
        # answer rather than a crash, which is why it needs a test and not an eye. This
        # one failed with "instruction budget exhausted" before the fix.
        acvm_add_jit_diff_check(jit_bail_check -DEXPECT_COMPILED=3)

        # Range loops, which are `LOCAL_ADDR; SWAP; STORE` and `LOCAL_ADDR; LOAD` pairs:
        # 216 and 112 of them across the example programs, each access a bridge call plus
        # a memType lookup plus a 32 byte Value. Those two pairs are now a single move.
        # Four functions compile; a mutation that writes the fused store at the wrong
        # offset fails here, which is the reason the widths and the nesting are varied.
        acvm_add_jit_diff_check(jit_range_check -DEXPECT_COMPILED=4)

        # No EXPECT_COMPILED on purpose: a program that declares a `process` is left to
        # the interpreter in full, because compiled code cannot yield and the scheduler
        # would never run the worker. What this pins is that the policy keeps the
        # program's meaning identical -- compiling `main` here made `counter` come out 0
        # instead of 6.
        acvm_add_jit_diff_check(jit_event_check)

        # A frame too large to pin must be refused at compile time. Before that check
        # existed the program died under `--jit` with an empty trap message, because
        # `pinFramePages` failed at run time and the exit path had nothing to say.
        acvm_add_jit_diff_check(jit_frame_check -DEXPECT_COMPILED=2)
    endif()

    # Passing arguments is the point of this one, so it runs with some.
    acvm_add_script_check(args_check -- alpha beta "two words")

    # Differential optimization checks: every script check above runs again with
    # -O0 and with -O3 and the two outputs must match byte for byte. An optimizer
    # bug that changes a result still "passes" any single-level test, so this is
    # the only thing that actually holds the passes to their contract.
    # `levels` decides which optimization levels run. The default pair proves the
    # optimizing and non-optimizing compilers agree; `O4` is added only for
    # programs that do not depend on scheduling, because O4 deletes safe points.
    function(acvm_add_opt_diff_check name levels)
        # The list has to reach -D as ONE argument. `${levels}` expands the
        # semicolons into separate command arguments, so the script received
        # `-DLEVELS=O0` plus two stray words and compared O0 against itself --
        # a differential test that passed without comparing anything.
        #
        # Commas, not escaped semicolons: `-DLEVELS=O0,O3` arrives as a single
        # list element containing literal semicolons, which is just as useless in
        # the other direction. Commas have no meaning to CMake, so the script can
        # split them itself.
        string(REPLACE ";" "," levels_argument "${levels}")
        add_test(NAME ${name}_optdiff
                 COMMAND ${CMAKE_COMMAND}
                         -DACVM=$<TARGET_FILE:acvm>
                         -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.ax
                         "-DLEVELS=${levels_argument}"
                         ${ARGN}
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_outputs.cmake)
    endfunction()

    # time_check is deliberately absent: it prints wall-clock timestamps and
    # measured sleep durations, so its output differs between two runs of the
    # same binary no matter which level compiled it. Excluding it is not a way to
    # hide a failure -- the first version of this list did include it, and the
    # only differences were `now=<ms>` and `slept ms=<n>`.
    #
    # sync_check is the other exclusion, for the opposite reason: it asserts on
    # scheduling ("no worker saw an incomplete phase"), which is exactly what O4
    # stops providing. It still runs at O0-O3; at O4 its counters are right and
    # only that one scheduling assertion fails, which is the documented trade.
    foreach(check_name func_check algorithm_key_check testcases_check
                            str_check codec_check strsort_check map_check
                            fmt_check sort_index_check os_check
                            parallel_check threadpool_check generics_check union_check
                            generic_struct_check address_assign_check
                            ternary_check int128_check methods_check astl_check
                            random_check ${ACVM_EXTRALIB_CHECKS})
        acvm_add_opt_diff_check(${check_name} "O0;O3;O4")
    endforeach()
    acvm_add_opt_diff_check(sync_check "O0;O3")

    # O4 must actually remove the safe points -- otherwise "not flow-preserving"
    # would be a claim with nothing behind it. `ir -O3` has to contain SAFE_POINT
    # and `ir -O4` must not; a program with a `process` is the one that has them.
    # Generic-function diagnostics: each case must be rejected for the stated
    # reason, and the struct case must not be blamed on signedness.
    add_test(NAME generics_errors_check
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/generics_errors
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_generics_errors.cmake)

    # The tutorial's programs must run, and where it states an output that output must
    # match. A document is the easiest thing in a repository to let rot: it does not
    # fail to compile when the language changes, it just becomes quietly wrong, and
    # readers copy it.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME tutorial_check
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_tutorial.py
                         --acvm $<TARGET_FILE:acvm>
                         --doc ${CMAKE_CURRENT_SOURCE_DIR}/TUTORIAL.md
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    # The TextMate grammar is a derived artifact: when the lexer gains a keyword nothing
    # breaks, it just stops highlighting that word. It had already drifted twice, so the
    # keyword list is read out of the lexer and compared with the grammar, and the
    # grammar's scopeName is checked against the language definition that names it.
    if(Python3_Interpreter_FOUND)
        add_test(NAME grammar_check
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_grammar_keywords.py)
    endif()

    # The `module` directive's own rules: name must match the file, version must be
    # MAJOR.MINOR.PATCH, one per file -- and a valid one must be completely inert.
    add_test(NAME module_check
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/module_errors
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_module.cmake)

    # Every extralib module must declare `module "name" version "x.y.z";`. The
    # compiler validates the content when the module is imported; this covers the
    # case a module forgets to declare one, which would otherwise compile fine and
    # leave the future package tooling to guess.
    add_test(NAME module_versions_check
             COMMAND ${CMAKE_COMMAND}
                     -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/extralib
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_module_versions.cmake)

    # Integer width boundaries, at every optimization level. The point is not that
    # int128 works but that the optimizer and the VM agree on when it overflows --
    # when they disagree the folder deletes the trap and the program prints a number
    # where O0 raises an error.
    add_test(NAME int_widths_check
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/int_widths
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_int_widths.cmake)

    # Name resolution across files: a collision between two imported files is an
    # error instead of an import-order coin flip, and `private` scopes the name
    # rather than merely rejecting the call. The last two cases are programs that
    # must RUN and print exact output -- they were never syntax errors, so only
    # running them proves the fix.
    add_test(NAME scope_check
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/scope_errors
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_scope.cmake)

    # Initialiser diagnostics. Each case used to be worse than a bad message: a
    # non-zero scalar against an aggregate was *accepted* and became a memcpy from
    # that scalar taken as an address, so `bit256 d = 0;` trapped on a null pointer.
    # The `_ok` forms next to them are compiled too, because a rejection-only check
    # is satisfied by a compiler that rejects everything.
    add_test(NAME initializer_errors_check
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/initializer_errors
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_initializer_errors.cmake)

    # Method diagnostics: a write through a value receiver must be rejected rather
    # than compile into a no-op, and each call-form mismatch must name the fix.
    add_test(NAME methods_errors_check
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/methods_errors
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_methods_errors.cmake)

    # Union diagnostics: each case must be rejected for the stated reason, and the
    # by-value native case must name the truncation rather than blame something else.
    add_test(NAME union_errors_check
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/union_errors
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_union_errors.cmake)

    # The rule that makes extralib optional, checked instead of asserted: nothing
    # in stdlib may import anything from extralib. See the script for why this is
    # the load-bearing wall of the design.
    # The other half of the extralib rule: modules inside this directory *may*
    # depend on each other (`http` needs `net`), but every such dependency has to
    # be declared in a `//  depends:` line and the graph has to stay acyclic --
    # otherwise "delete any module you do not import" stops being true for the
    # modules that are left.
    add_test(NAME extralib_dependencies
             COMMAND ${CMAKE_COMMAND}
                     -DEXTRALIB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/extralib
                     -DSTDLIB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/stdlib
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_extralib_dependencies.cmake)

    add_test(NAME extralib_independence
             COMMAND ${CMAKE_COMMAND}
                     -DSTDLIB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/stdlib
                     -DEXTRALIB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/extralib
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_extralib_independence.cmake)

    add_test(NAME generic_struct_errors_check
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_struct_errors
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_generic_struct_errors.cmake)

    # Constant expressions that are certain to trap are reported at compile time,
    # and the ones that merely look alarming must stay quiet.
    add_test(NAME const_trap_check
             COMMAND ${CMAKE_COMMAND}
                     -DACVM=$<TARGET_FILE:acvm>
                     -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/const_trap.ax
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_const_trap.cmake)

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/sync_check.ax)
        add_test(NAME o4_removes_safe_points
                 COMMAND ${CMAKE_COMMAND}
                         -DACVM=$<TARGET_FILE:acvm>
                         -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/sync_check.ax
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_safe_points.cmake)
    endif()

    # Examples exercise the larger programs (pipeline, iterator tour, sorting
    # demos); an optimizer bug that only shows up at scale lands here.
    foreach(example algorithm astl_demo tour quicksort)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/${example}.ax)
            add_test(NAME example_${example}_optdiff
                     COMMAND ${CMAKE_COMMAND}
                             -DACVM=$<TARGET_FILE:acvm>
                             -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/${example}.ax
                             -DLEVELS=O0,O3,O4
                             -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_outputs.cmake)
        endif()
    endforeach()

    # These two use `process`, so they stay on the flow-preserving levels.
    foreach(example concurrent_pipeline threadpool_demo)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/${example}.ax)
            add_test(NAME example_${example}_optdiff
                     COMMAND ${CMAKE_COMMAND}
                             -DACVM=$<TARGET_FILE:acvm>
                             -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/${example}.ax
                             -DLEVELS=O0,O3
                             -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_outputs.cmake)
        endif()
    endforeach()

    # The 64 bit family has no Axiom fallback, so this one needs the library and
    # is skipped when the build did not produce it.
    if(TARGET acvm_algorithm)
        acvm_add_legacy_check(algorithm64_check)
    endif()
endif()
