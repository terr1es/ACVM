# Checks the dependency rules *inside* extralib.
#
# The other script (`check_extralib_independence.cmake`) enforces stdlib -> extralib
# is forbidden: the optional layer may be deleted. This one covers the direction
# that only exists because modules in this directory are allowed to depend on each
# other (`http` needs `net`, `inflate` needs `sha256`).
#
# Internal dependencies are permitted, on three conditions, all checked here:
#
#   1. Every module declares them in a `//  depends: a, b` line. A dependency that
#      is only visible by reading `import` statements is a dependency that the next
#      person will not notice before deleting the module it needs.
#
#   2. Every declared name corresponds to a module that exists. A declaration that
#      points at nothing is worse than no declaration: it looks like documentation
#      and is wrong.
#
#   3. The graph has no cycle. A cycle means neither module can be deleted, which
#      makes the "delete any of it" promise false for both.
#
# Plus a fourth, quieter one: the declared set must match the imported set. A module
# that imports something it did not declare is exactly the drift this file exists to
# catch, and it is easy to introduce by adding one line.
#
#   cmake -DEXTRALIB_DIR=... -P check_extralib_dependencies.cmake

if(NOT DEFINED EXTRALIB_DIR)
    message(FATAL_ERROR "check_extralib_dependencies.cmake needs -DEXTRALIB_DIR")
endif()

file(GLOB extralib_files "${EXTRALIB_DIR}/*.ax" "${EXTRALIB_DIR}/*.axd")

# `depends:` lists every module a file reaches for, including stdlib ones (`io`,
# `codec`). Those are always present, so they are not edges in the graph this
# script checks -- but a name that exists in neither directory is still a mistake,
# which is why the stdlib set is collected as well.
set(stdlib_names "")
if(DEFINED STDLIB_DIR)
    file(GLOB stdlib_files "${STDLIB_DIR}/*.ax" "${STDLIB_DIR}/*.axd")
    foreach(file IN LISTS stdlib_files)
        get_filename_component(base "${file}" NAME_WE)
        list(APPEND stdlib_names "${base}")
    endforeach()
    list(REMOVE_DUPLICATES stdlib_names)
endif()

# Module name -> source file. `.axd` and `.ax` of the same name are one module; the
# declaration file is skipped in favour of the implementation when both exist,
# because the `depends:` line lives in the `.ax` header.
set(modules "")
foreach(file IN LISTS extralib_files)
    get_filename_component(base "${file}" NAME_WE)
    get_filename_component(ext "${file}" EXT)
    if(ext STREQUAL ".ax")
        list(APPEND modules "${base}")
    elseif(NOT EXISTS "${EXTRALIB_DIR}/${base}.ax")
        list(APPEND modules "${base}")
    endif()
endforeach()
list(REMOVE_DUPLICATES modules)
list(SORT modules)

if(modules STREQUAL "")
    message(STATUS "extralib/ has no modules: no internal dependencies to check")
    return()
endif()

set(problems "")

# Declared dependencies, per module.
set(declared_map "")
foreach(module IN LISTS modules)
    if(NOT EXISTS "${EXTRALIB_DIR}/${module}.ax")
        # A module with only a `.axd` (a pure native binding) has no place to put a
        # `depends:` line and cannot import anything either.
        continue()
    endif()
    file(READ "${EXTRALIB_DIR}/${module}.ax" contents)

    if(NOT contents MATCHES "//[ \t]*depends:")
        list(APPEND problems
            "${module}.ax has no '//  depends:' line (write '//  depends: (none)' when it has none)")
        continue()
    endif()

    # The line looks like:  //  depends: value, net
    string(REGEX MATCH "//[ \t]*depends:([^\n]*)" depends_line "${contents}")
    string(REGEX REPLACE "^[^:]*:" "" depends_body "${depends_line}")
    string(STRIP "${depends_body}" depends_body)

    set(declared "")
    if(NOT depends_body STREQUAL "(none)" AND NOT depends_body STREQUAL "")
        string(REPLACE "," ";" parts "${depends_body}")
        foreach(part IN LISTS parts)
            string(STRIP "${part}" part)
            if(NOT part STREQUAL "")
                list(APPEND declared "${part}")
            endif()
        endforeach()
    endif()
    set(declared_map "${declared_map};${module}=${declared}")
    # The graph only has extralib nodes: a stdlib dependency is always present and
    # therefore cannot participate in a cycle that makes a module undeletable.
    set(local_deps "")
    foreach(dep IN LISTS declared)
        if(dep IN_LIST modules AND NOT dep STREQUAL "${module}")
            list(APPEND local_deps "${dep}")
        endif()
    endforeach()
    set(deps_of_${module} "${local_deps}")

    # Every declared name must exist in one of the two directories.
    foreach(dep IN LISTS declared)
        if(NOT dep IN_LIST modules AND NOT dep IN_LIST stdlib_names)
            list(APPEND problems
                "${module}.ax declares a dependency on '${dep}', which is neither an extralib nor a stdlib module")
        endif()
    endforeach()

    # Imports, in both spellings the loader accepts.
    set(imported "")
    string(REGEX MATCHALL "return:\\([^)]*\\)" paren_imports "${contents}")
    foreach(statement IN LISTS paren_imports)
        string(REGEX REPLACE "^return:\\(|\\)$" "" inner "${statement}")
        string(REPLACE "," ";" parts "${inner}")
        foreach(part IN LISTS parts)
            string(STRIP "${part}" part)
            # Only names that are extralib modules matter here; `io`, `str` and the
            # rest are stdlib and are allowed from anywhere.
            if(part IN_LIST modules AND NOT part STREQUAL "${module}")
                list(APPEND imported "${part}")
            endif()
        endforeach()
    endforeach()
    string(REGEX MATCHALL "import[ \t]*\"[^\"]*\"" path_imports "${contents}")
    foreach(statement IN LISTS path_imports)
        string(REGEX REPLACE "^import[ \t]*\"|\"$" "" path "${statement}")
        get_filename_component(base "${path}" NAME_WE)
        if(base IN_LIST modules AND NOT base STREQUAL "${module}")
            list(APPEND imported "${base}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES imported)
    list(SORT imported)

    # Imports and declarations must agree.
    foreach(dep IN LISTS imported)
        if(NOT dep IN_LIST declared)
            list(APPEND problems
                "${module}.ax imports '${dep}' but does not declare it ('//  depends: …')")
        endif()
    endforeach()
endforeach()

# Self-dependency is a sign of a copy-paste in the depends line.
foreach(module IN LISTS modules)
    if(EXISTS "${EXTRALIB_DIR}/${module}.ax")
        file(READ "${EXTRALIB_DIR}/${module}.ax" contents)
        string(REGEX MATCH "//[ \t]*depends:([^\n]*)" depends_line "${contents}")
        string(REGEX REPLACE "^[^:]*:" "" depends_body "${depends_line}")
        string(REPLACE "," ";" parts "${depends_body}")
        foreach(part IN LISTS parts)
            string(STRIP "${part}" part)
            if(part STREQUAL "${module}")
                list(APPEND problems "${module}.ax declares a dependency on itself")
            endif()
        endforeach()
    endif()
endforeach()

# Cycle detection: breadth-first from every module, looking for a way back to the
# starting point. Iterative on purpose -- CMake's `PARENT_SCOPE` in a recursive
# function only reaches one level up, so a recursive walk silently loses the
# visiting set and reports no cycle (measured: it missed `glob <-> http`).
foreach(module IN LISTS modules)
    set(frontier "${module}")
    set(seen "")
    while(NOT frontier STREQUAL "")
        list(GET frontier 0 node)
        list(REMOVE_AT frontier 0)
        if(NOT DEFINED deps_of_${node})
            continue()
        endif()
        foreach(dep IN LISTS deps_of_${node})
            if(dep STREQUAL "${module}")
                list(APPEND problems "dependency cycle: ${module} -> ... -> ${module}")
                set(frontier "")
                break()
            endif()
            if(NOT dep IN_LIST seen)
                list(APPEND seen "${dep}")
                list(APPEND frontier "${dep}")
            endif()
        endforeach()
    endwhile()
endforeach()
list(REMOVE_DUPLICATES problems)

if(NOT problems STREQUAL "")
    string(REPLACE ";" "\n  " report "${problems}")
    message(FATAL_ERROR
        "extralib dependency declarations are inconsistent:\n  ${report}\n\n"
        "Internal dependencies are allowed, but each one must be written down in a\n"
        "'//  depends: a, b' line at the top of the module, so deleting a module shows\n"
        "who needs it. The check exists because a written-down dependency is the only\n"
        "thing that breaks when a module is deleted, and prose does not.")
endif()

string(REPLACE ";" ", " names "${modules}")
message(STATUS "extralib dependencies are consistent (${names})")
