# Every extralib module must declare its identity and version.
#
# The compiler already validates the *content* of a declaration when the module is
# imported (name must match the file, version must be MAJOR.MINOR.PATCH). This check
# covers the other half: that the declaration exists at all. A module added without
# one compiles perfectly -- which is exactly why nothing else would notice, and why
# the package tooling would later have to guess.
#
# It is deliberately a text check rather than a compiler invocation: these files have
# no `main`, so `acvm check` on one reports "no entry point", and a check that has to
# work around that would be testing the harness instead of the directive.
#
#     cmake -DSOURCE_DIR=<extralib> -P check_module_versions.cmake

if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "check_module_versions.cmake needs -DSOURCE_DIR=<path>")
endif()

file(GLOB modules "${SOURCE_DIR}/*.ax" "${SOURCE_DIR}/*.axd")
list(SORT modules)

set(count 0)
set(problems "")

foreach(module IN LISTS modules)
    get_filename_component(stem "${module}" NAME_WE)
    file(READ "${module}" text)

    # `module "<stem>" version "<X.Y.Z>";` — the name is checked here, the version
    # format is the compiler's job.
    get_filename_component(extension "${module}" EXT)
    if(text MATCHES "module \"${stem}\" version \"[0-9]+\\.[0-9]+\\.[0-9]+\"")
        math(EXPR count "${count} + 1")
    else()
        # Distinguish "missing" from "present but wrong", because the fix differs.
        if(text MATCHES "module \"")
            string(APPEND problems
                "\n  ${stem}${extension}: declares a module, but not as \"${stem}\" with a "
                "MAJOR.MINOR.PATCH version")
        else()
            string(APPEND problems
                "\n  ${stem}${extension}: no 'module \"${stem}\" version \"x.y.z\";' declaration")
        endif()
    endif()
endforeach()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "extralib modules without a usable version declaration:${problems}")
endif()

message(STATUS "${SOURCE_DIR}: ${count} modules declare a name and version")
