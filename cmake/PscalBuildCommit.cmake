# Script mode (cmake -P), run by the pscal_build_commits target on every build.
#
# Writes <OUT_DIR>/<component>/pscal_build_commit.h for each entry in
# COMPONENTS, defining PSCAL_BUILD_COMMIT as that component checkout's short
# commit, with "-dirty" when tracked files have uncommitted changes. Frontends
# append it to their VERSION (core/build_info.h), so `-v` names the exact
# source a binary was built from.
#
# A header is only rewritten when its content changes: an unchanged commit
# must not recompile anything, since this runs on every build.
#
# Inputs: PSCAL_SOURCE_DIR, OUT_DIR, COMPONENTS (comma-separated names under
# components/; commas because a ;-list does not survive a custom command).

string(REPLACE "," ";" COMPONENTS "${COMPONENTS}")
foreach(component IN LISTS COMPONENTS)
    set(dir "${PSCAL_SOURCE_DIR}/components/${component}")
    set(commit "")
    execute_process(
        COMMAND git -C "${dir}" rev-parse --short=7 HEAD
        OUTPUT_VARIABLE commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE rev_result)
    if(rev_result EQUAL 0 AND NOT commit STREQUAL "")
        execute_process(
            COMMAND git -C "${dir}" status --porcelain --untracked-files=no
            OUTPUT_VARIABLE changes
            ERROR_QUIET)
        if(NOT changes STREQUAL "")
            string(APPEND commit "-dirty")
        endif()
        set(content "#define PSCAL_BUILD_COMMIT \"${commit}\"\n")
    else()
        # Source snapshot without git: report the VERSION alone.
        set(content "/* no git checkout: build commit unknown */\n")
    endif()

    set(header "${OUT_DIR}/${component}/pscal_build_commit.h")
    file(WRITE "${header}.tmp" "${content}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${header}.tmp" "${header}")
    file(REMOVE "${header}.tmp")
endforeach()
