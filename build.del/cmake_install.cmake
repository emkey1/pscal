# Install script for directory: /Users/mke/PBuild

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/mke/PBuild/build/src/ext_builtins/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/mke/PBuild/build/Examples/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/Users/mke/PBuild/build/bin/pascal")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pascal" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pascal")
    execute_process(COMMAND /usr/bin/install_name_tool
      -delete_rpath "/opt/homebrew/Cellar/sdl2_image/2.8.8/lib"
      -delete_rpath "/opt/homebrew/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_mixer/2.8.1_1/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_ttf/2.24.0/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2/2.32.10/lib"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pascal")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" -u -r "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pascal")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/Users/mke/PBuild/build/bin/pscalvm")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pscalvm" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pscalvm")
    execute_process(COMMAND /usr/bin/install_name_tool
      -delete_rpath "/opt/homebrew/Cellar/sdl2_image/2.8.8/lib"
      -delete_rpath "/opt/homebrew/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_mixer/2.8.1_1/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_ttf/2.24.0/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2/2.32.10/lib"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pscalvm")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" -u -r "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pscalvm")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/Users/mke/PBuild/build/bin/clike")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/clike" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/clike")
    execute_process(COMMAND /usr/bin/install_name_tool
      -delete_rpath "/opt/homebrew/Cellar/sdl2_image/2.8.8/lib"
      -delete_rpath "/opt/homebrew/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_mixer/2.8.1_1/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_ttf/2.24.0/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2/2.32.10/lib"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/clike")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" -u -r "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/clike")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/Users/mke/PBuild/build/bin/clike-repl")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/clike-repl" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/clike-repl")
    execute_process(COMMAND /usr/bin/install_name_tool
      -delete_rpath "/opt/homebrew/Cellar/sdl2_image/2.8.8/lib"
      -delete_rpath "/opt/homebrew/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_mixer/2.8.1_1/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_ttf/2.24.0/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2/2.32.10/lib"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/clike-repl")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" -u -r "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/clike-repl")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/Users/mke/PBuild/build/bin/rea")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rea" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rea")
    execute_process(COMMAND /usr/bin/install_name_tool
      -delete_rpath "/opt/homebrew/Cellar/sdl2_image/2.8.8/lib"
      -delete_rpath "/opt/homebrew/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_mixer/2.8.1_1/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_ttf/2.24.0/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2/2.32.10/lib"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rea")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" -u -r "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rea")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/Users/mke/PBuild/build/bin/pscald")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pscald" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pscald")
    execute_process(COMMAND /usr/bin/install_name_tool
      -delete_rpath "/opt/homebrew/Cellar/sdl2_image/2.8.8/lib"
      -delete_rpath "/opt/homebrew/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_mixer/2.8.1_1/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_ttf/2.24.0/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2/2.32.10/lib"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pscald")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" -u -r "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pscald")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
          set(exsh_path "$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}/exsh")
        if(EXISTS "${exsh_path}" OR IS_SYMLINK "${exsh_path}")
            set(exsh_backup "${exsh_path}.previous")
            if(EXISTS "${exsh_backup}" OR IS_SYMLINK "${exsh_backup}")
                file(REMOVE "${exsh_backup}")
            endif()
            message(STATUS "Backing up existing exsh to ${exsh_backup}")
            file(RENAME "${exsh_path}" "${exsh_backup}")
        endif()
    
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/Users/mke/PBuild/build/bin/exsh")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/exsh" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/exsh")
    execute_process(COMMAND /usr/bin/install_name_tool
      -delete_rpath "/opt/homebrew/Cellar/sdl2_image/2.8.8/lib"
      -delete_rpath "/opt/homebrew/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_mixer/2.8.1_1/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_ttf/2.24.0/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2/2.32.10/lib"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/exsh")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" -u -r "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/exsh")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/lib" TYPE DIRECTORY FILES "/Users/mke/PBuild/lib/" USE_SOURCE_PERMISSIONS)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/fonts" TYPE DIRECTORY FILES "/Users/mke/PBuild/fonts/" USE_SOURCE_PERMISSIONS)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/etc" TYPE DIRECTORY FILES "/Users/mke/PBuild/etc/" USE_SOURCE_PERMISSIONS)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples" TYPE DIRECTORY FILES "/Users/mke/PBuild/Examples/" USE_SOURCE_PERMISSIONS)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
              set(_pscal_build_cmd "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target configured_examples)
            if(CMAKE_CONFIGURATION_TYPES)
                list(APPEND _pscal_build_cmd --config "${CMAKE_INSTALL_CONFIG_NAME}")
            endif()
            execute_process(COMMAND ${_pscal_build_cmd}
                RESULT_VARIABLE _pscal_configured_examples_result)
            if(NOT _pscal_configured_examples_result EQUAL 0)
                message(FATAL_ERROR "Failed to generate configured example files (configured_examples target)")
            endif()
        
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/clike/base" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/clike/base/hangman5")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/clike/base" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/clike/base/simple_web_server")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/clike/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/clike/sdl/mandelbrot_interactive")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/clike/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/clike/sdl/spacegame")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/base" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/base/OOWebServer")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/base" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/base/hangman")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/base" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/base/hangman2")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/base" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/base/hangman3")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/base" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/base/hangman4")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/base" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/base/hangman5")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/base" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/base/weather")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/base" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/base/weather_json")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/sdl/FireWorks")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/sdl/GMaze2")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/sdl/SoundSimplePong")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/sdl/mando")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/pascal/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/pascal/sdl/mando_Linux")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/rea/base/archive" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/rea/base/archive/openweather_forecast")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/rea/base" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/rea/base/hangman5")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/rea/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/rea/sdl/block_game")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/rea/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/rea/sdl/mandelbrot_interactive")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/rea/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/rea/sdl/mandelbrot_interactive_ext")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/rea/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/rea/sdl/planets")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/rea/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/rea/sdl/planets_earth")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/Examples/rea/sdl" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE FILES "/Users/mke/PBuild/build/configured_examples/Examples/rea/sdl/planets_inner")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/docs" TYPE DIRECTORY FILES "/Users/mke/PBuild/Docs/")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/pscal/etc/tests" TYPE DIRECTORY FILES "/Users/mke/PBuild/Tests/libs/" USE_SOURCE_PERMISSIONS)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/rea" TYPE DIRECTORY FILES "/Users/mke/PBuild/lib/rea/" USE_SOURCE_PERMISSIONS)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
          set(ps_root "")
        if("${PSCAL_INSTALL_ROOT_DESTINATION}" STREQUAL ".")
            set(ps_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
        elseif(IS_ABSOLUTE "${PSCAL_INSTALL_ROOT_DESTINATION}")
            set(ps_root "$ENV{DESTDIR}${PSCAL_INSTALL_ROOT_DESTINATION}")
        else()
            if("${PSCAL_INSTALL_ROOT_DESTINATION}" STREQUAL "")
                set(ps_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
            else()
                set(ps_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${PSCAL_INSTALL_ROOT_DESTINATION}")
            endif()
        endif()
        if(ps_root STREQUAL "")
            return()
        endif()

        set(compat_targets
            "${ps_root}/lib/pascal"
            "${ps_root}/lib/clike"
            "${ps_root}/lib/rea"
            "${ps_root}/lib/misc")
        set(compat_links
            "${ps_root}/pascal/lib"
            "${ps_root}/clike/lib"
            "${ps_root}/rea/lib"
            "${ps_root}/misc")

        list(LENGTH compat_targets compat_count)
        if(compat_count EQUAL 0)
            return()
        endif()

        math(EXPR compat_last "${compat_count} - 1")
        foreach(idx RANGE 0 ${compat_last})
            list(GET compat_targets ${idx} compat_target)
            list(GET compat_links ${idx} compat_link)

            if(NOT EXISTS "${compat_target}" AND NOT IS_SYMLINK "${compat_target}")
                continue()
            endif()
            if(EXISTS "${compat_link}" OR IS_SYMLINK "${compat_link}")
                continue()
            endif()

            get_filename_component(compat_parent "${compat_link}" DIRECTORY)
            file(MAKE_DIRECTORY "${compat_parent}")
            file(CREATE_LINK "${compat_target}" "${compat_link}" SYMBOLIC)
        endforeach()
    
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/Users/mke/PBuild/build/bin/pscaljson2bc")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pscaljson2bc" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pscaljson2bc")
    execute_process(COMMAND /usr/bin/install_name_tool
      -delete_rpath "/opt/homebrew/Cellar/sdl2_image/2.8.8/lib"
      -delete_rpath "/opt/homebrew/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_mixer/2.8.1_1/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2_ttf/2.24.0/lib"
      -delete_rpath "/opt/homebrew/Cellar/sdl2/2.32.10/lib"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pscaljson2bc")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" -u -r "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/pscaljson2bc")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/bash-completion/completions" TYPE FILE RENAME "pscaljson2bc" FILES "/Users/mke/PBuild/tools/completions/pscaljson2bc.bash")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/zsh/site-functions" TYPE FILE FILES "/Users/mke/PBuild/tools/completions/_pscaljson2bc")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/mke/PBuild/build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/mke/PBuild/build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
