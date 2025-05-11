# CMakeLists.txt – top of your repo
cmake_minimum_required(VERSION 3.24) # CMake ≥3.24 recommended on macOS
project(pscal C) # pure‑C project

# ---- options you used everywhere ------------------------------------------
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall)

# Put all executables in <repo>/bin (matches your Makefile)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# ---- gather sources -------------------------------------------------------
# Assuming src/audio.c and src/audio.h are in the src/ directory,
# the GLOB command should pick them up. If not, add them explicitly.
file(GLOB SRC_FILES CONFIGURE_DEPENDS src/*.c src/*.h)

# ---- Release‑style build : pscal ------------------------------------------
add_executable(pscal ${SRC_FILES})
target_compile_options(pscal PRIVATE -O3)

# ---- Address‑Sanitized debug build : dscal -------------------------------
add_executable(dscal ${SRC_FILES})
# You might want to add DEBUG here: target_compile_definitions(dscal PRIVATE DEBUG)
target_compile_definitions(dscal PRIVATE DEBUG)
target_compile_options( dscal PRIVATE -fsanitize=address -g)
target_link_options( dscal PRIVATE -fsanitize=address)

# ---- “no‑exit” build : hscal (formerly nscal) -----------------------------
add_executable(hscal ${SRC_FILES})
target_compile_definitions(hscal PRIVATE SUPPRESS_EXIT)
target_compile_options( hscal PRIVATE -g)

# ---- optional install rule (matches `make install`) -----------------------
include(GNUInstallDirs)
install(TARGETS pscal dscal RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# --------------------------------------------------------------------------
# SDL Support

# --- Find SDL2 (Core) ---
# This one seems to work with find_package
find_package(SDL2 REQUIRED) # REQUIRED makes build fail if SDL2 not found
message(STATUS "SDL2_INCLUDE_DIRS = ${SDL2_INCLUDE_DIRS}")
message(STATUS "SDL2_LIBRARIES = ${SDL2_LIBRARIES}")

# --- Platform-Specific SDL2_ttf and SDL2_mixer Configuration ---
# Re-introducing platform-specific search logic as find_package alone doesn't populate vars.

# Initialize variables to hold the found paths/libraries
set(PLATFORM_SDL2_TTF_INCLUDE_DIRS "")
set(PLATFORM_SDL2_TTF_LIBRARIES "")
set(PLATFORM_SDL2_MIXER_INCLUDE_DIRS "") # ADDED variable for mixer includes
set(PLATFORM_SDL2_MIXER_LIBRARIES "")   # ADDED variable for mixer libraries

if(APPLE)
    message(STATUS "Platform: Apple macOS - Finding SDL2_ttf and SDL2_mixer...")

    # Try standard find_package first (might find some components or set cache variables)
    # Use QUIET so it doesn't fail immediately if it doesn't find everything
    find_package(SDL2_ttf QUIET)
    find_package(SDL2_mixer QUIET) # ADDED: Try finding mixer quietly

    # --- Search for SDL2_ttf ---
    # Prioritize paths found by the quiet find_package if they are set
    set(SDL2_TTF_SEARCH_INCLUDE_DIRS ${SDL2_TTF_INCLUDE_DIRS})
    set(SDL2_TTF_SEARCH_LIBRARIES ${SDL2_TTF_LIBRARIES})

    # Fallback using Homebrew if standard find failed or was incomplete
    if(NOT SDL2_TTF_FOUND OR NOT SDL2_TTF_SEARCH_INCLUDE_DIRS) # Check if find_package clearly failed or didn't get include dirs
        message(STATUS "Standard find_package for SDL2_ttf inconclusive, trying brew --prefix...")
        execute_process(
            COMMAND brew --prefix sdl2_ttf
            OUTPUT_VARIABLE SDL2_TTF_BREW_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE SDL2_TTF_BREW_RESULT
            ERROR_QUIET # Don't show brew errors if the package isn't installed
        )
        # Check if brew command succeeded and the include file exists at the expected location
        if(SDL2_TTF_BREW_RESULT EQUAL 0 AND SDL2_TTF_BREW_PREFIX AND EXISTS "${SDL2_TTF_BREW_PREFIX}/include/SDL2/SDL_ttf.h")
            message(STATUS "Found SDL2_ttf include path via brew --prefix: ${SDL2_TTF_BREW_PREFIX}/include")
            # Use list(APPEND ...) to add this path, in case find_package found other paths
            list(APPEND SDL2_TTF_SEARCH_INCLUDE_DIRS "${SDL2_TTF_BREW_PREFIX}/include")

            # Explicitly find the library within the brew prefix
            find_library(SDL2_TTF_LIBRARY_PATH_BREW NAMES SDL2_ttf PATHS "${SDL2_TTF_BREW_PREFIX}/lib" NO_DEFAULT_PATH)
            if(SDL2_TTF_LIBRARY_PATH_BREW)
                message(STATUS "Found SDL2_ttf library via brew path: ${SDL2_TTF_LIBRARY_PATH_BREW}")
                list(APPEND SDL2_TTF_SEARCH_LIBRARIES ${SDL2_TTF_LIBRARY_PATH_BREW}) # Append the found full path
                mark_as_advanced(SDL2_TTF_LIBRARY_PATH_BREW) # Hide this variable from basic CMake GUI
            else()
                # If find_library fails even in the brew path, add the library name as a fallback (might fail linking)
                message(WARNING "Could not find SDL2_ttf library in brew path (${SDL2_TTF_BREW_PREFIX}/lib). Using name 'SDL2_ttf'.")
                list(APPEND SDL2_TTF_SEARCH_LIBRARIES "SDL2_ttf")
            endif()
        else()
            message(WARNING "Could not find SDL2_ttf via standard CMake find or brew on macOS.")
             # As a last resort, ensure the library name is in the list if nothing else was found
             if(NOT SDL2_TTF_SEARCH_LIBRARIES)
                 list(APPEND SDL2_TTF_SEARCH_LIBRARIES "SDL2_ttf")
             endif()
        endif()
    endif() # End SDL2_ttf search logic block

    # --- Search for SDL2_mixer ---
    # ADDED: Logic for SDL2_mixer, similar to SDL2_ttf

    # Prioritize paths found by the quiet find_package for mixer if they are set
    set(SDL2_MIXER_SEARCH_INCLUDE_DIRS ${SDL2_mixer_INCLUDE_DIRS})
    set(SDL2_MIXER_SEARCH_LIBRARIES ${SDL2_mixer_LIBRARIES})

    # Fallback using Homebrew for mixer if standard find failed or was incomplete
    if(NOT SDL2_mixer_FOUND OR NOT SDL2_MIXER_SEARCH_INCLUDE_DIRS) # Check if find_package clearly failed or didn't get include dirs for mixer
        message(STATUS "Standard find_package for SDL2_mixer inconclusive, trying brew --prefix...")
        execute_process(
            COMMAND brew --prefix sdl2_mixer # Command to get brew prefix for sdl2_mixer
            OUTPUT_VARIABLE SDL2_MIXER_BREW_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE SDL2_MIXER_BREW_RESULT
            ERROR_QUIET
        )
         # Check if brew command succeeded and the include file exists at the expected location for mixer
        if(SDL2_MIXER_BREW_RESULT EQUAL 0 AND SDL2_MIXER_BREW_PREFIX AND EXISTS "${SDL2_MIXER_BREW_PREFIX}/include/SDL2/SDL_mixer.h")
            message(STATUS "Found SDL2_mixer include path via brew --prefix: ${SDL2_MIXER_BREW_PREFIX}/include")
            # Use list(APPEND ...) to add this path
            list(APPEND SDL2_MIXER_SEARCH_INCLUDE_DIRS "${SDL2_MIXER_BREW_PREFIX}/include")

            # Explicitly find the library within the brew prefix for mixer
            find_library(SDL2_MIXER_LIBRARY_PATH_BREW NAMES SDL2_mixer PATHS "${SDL2_MIXER_BREW_PREFIX}/lib" NO_DEFAULT_PATH)
            if(SDL2_MIXER_LIBRARY_PATH_BREW)
                message(STATUS "Found SDL2_mixer library via brew path: ${SDL2_MIXER_LIBRARY_PATH_BREW}")
                list(APPEND SDL2_MIXER_SEARCH_LIBRARIES ${SDL2_MIXER_LIBRARY_PATH_BREW}) # Append the found full path
                mark_as_advanced(SDL2_MIXER_LIBRARY_PATH_BREW)
            else()
                 # If find_library fails even in the brew path, add the library name as a fallback
                message(WARNING "Could not find SDL2_mixer library in brew path (${SDL2_MIXER_BREW_PREFIX}/lib). Using name 'SDL2_mixer'.")
                list(APPEND SDL2_MIXER_SEARCH_LIBRARIES "SDL2_mixer")
            endif()
        else()
            message(WARNING "Could not find SDL2_mixer via standard CMake find or brew on macOS.")
            # As a last resort, ensure the library name is in the list if nothing else was found
            if(NOT SDL2_MIXER_SEARCH_LIBRARIES)
                list(APPEND SDL2_MIXER_SEARCH_LIBRARIES "SDL2_mixer")
            endif()
        endif()
    endif() # End SDL2_mixer search logic block


    # Final check for macOS - if includes/libraries are still not found after all attempts,
    # re-trigger the REQUIRED find_package calls. This will fail the build if they are truly missing.
    if(NOT SDL2_TTF_SEARCH_INCLUDE_DIRS OR NOT SDL2_TTF_SEARCH_LIBRARIES)
        message(FATAL_ERROR "SDL2_ttf is REQUIRED but could not be configured on macOS after all search attempts.")
        # Note: If execution reaches here, it implies previous find_package/brew logic didn't work as expected.
        # The REQUIRED find_package call here is mainly to trigger CMake's standard error message
        # if libraries are still not found in the standard paths it checks after our manual search.
    endif()
     if(NOT SDL2_MIXER_SEARCH_INCLUDE_DIRS OR NOT SDL2_MIXER_SEARCH_LIBRARIES) # ADDED check for mixer
        message(FATAL_ERROR "SDL2_mixer is REQUIRED but could not be configured on macOS after all search attempts.") # ADDED message for mixer
        # Trigger standard CMake error message for mixer if not found
    endif()


    # Assign the final determined paths/libraries to the PLATFORM variables
    set(PLATFORM_SDL2_TTF_INCLUDE_DIRS ${SDL2_TTF_SEARCH_INCLUDE_DIRS})
    set(PLATFORM_SDL2_TTF_LIBRARIES ${SDL2_TTF_SEARCH_LIBRARIES})
    set(PLATFORM_SDL2_MIXER_INCLUDE_DIRS ${SDL2_MIXER_SEARCH_INCLUDE_DIRS}) # ADDED
    set(PLATFORM_SDL2_MIXER_LIBRARIES ${SDL2_MIXER_SEARCH_LIBRARIES})     # ADDED


elseif(UNIX) # Catches Linux, BSD, etc. (but APPLE was handled above)
    message(STATUS "Platform: Unix (Non-Apple)")
    # For Linux, assume standard installation paths like /usr/include and /usr/lib
    message(STATUS "Using standard paths for SDL2_ttf and SDL2_mixer on Unix system.") # ADDED message for both
    set(PLATFORM_SDL2_TTF_INCLUDE_DIRS "/usr/include")
    set(PLATFORM_SDL2_TTF_LIBRARIES "SDL2_ttf")
    set(PLATFORM_SDL2_MIXER_INCLUDE_DIRS "/usr/include") # ADDED for mixer
    set(PLATFORM_SDL2_MIXER_LIBRARIES "SDL2_mixer")     # ADDED for mixer

else()
    # This block should ideally not be reached if APPLE and UNIX cover all platforms you support
    message(FATAL_ERROR "Unsupported platform for SDL configuration.") # ADDED message
endif()

message(STATUS "Final Platform SDL2_TTF Include Dirs = ${PLATFORM_SDL2_TTF_INCLUDE_DIRS}")
message(STATUS "Final Platform SDL2_TTF Libraries = ${PLATFORM_SDL2_TTF_LIBRARIES}")
message(STATUS "Final Platform SDL2_MIXER Include Dirs = ${PLATFORM_SDL2_MIXER_INCLUDE_DIRS}") # ADDED Status message for mixer
message(STATUS "Final Platform SDL2_MIXER Libraries = ${PLATFORM_SDL2_MIXER_LIBRARIES}")     # ADDED Status message for mixer


# Apply SDL, SDL2_ttf, and SDL2_mixer settings to ALL targets that need them
# Use the platform-specific variables determined above
target_include_directories(pscal PRIVATE ${SDL2_INCLUDE_DIRS} ${PLATFORM_SDL2_TTF_INCLUDE_DIRS} ${PLATFORM_SDL2_MIXER_INCLUDE_DIRS}) # ADDED mixer includes
target_include_directories(dscal PRIVATE ${SDL2_INCLUDE_DIRS} ${PLATFORM_SDL2_TTF_INCLUDE_DIRS} ${PLATFORM_SDL2_MIXER_INCLUDE_DIRS}) # ADDED mixer includes
target_include_directories(hscal PRIVATE ${SDL2_INCLUDE_DIRS} ${PLATFORM_SDL2_TTF_INCLUDE_DIRS} ${PLATFORM_SDL2_MIXER_INCLUDE_DIRS}) # ADDED mixer includes

# Link SDL2, SDL2_ttf, and SDL2_mixer using the platform variables
# Link SDL2 using its target if available (preferable), then ttf and mixer libs.
target_link_libraries(pscal PRIVATE SDL2::SDL2 ${PLATFORM_SDL2_TTF_LIBRARIES} ${PLATFORM_SDL2_MIXER_LIBRARIES}) # ADDED mixer libs
target_link_libraries(dscal PRIVATE SDL2::SDL2 ${PLATFORM_SDL2_TTF_LIBRARIES} ${PLATFORM_SDL2_MIXER_LIBRARIES}) # ADDED mixer libs
target_link_libraries(hscal PRIVATE SDL2::SDL2 ${PLATFORM_SDL2_TTF_LIBRARIES} ${PLATFORM_SDL2_MIXER_LIBRARIES}) # ADDED mixer libs
# --------------------------------------------------------------------------

# --- Link m (math) and curl libraries (Applied to all targets) ---
target_link_libraries(pscal PRIVATE m curl)
target_link_libraries(dscal PRIVATE m curl)
target_link_libraries(hscal PRIVATE m curl)
# --- End ---
