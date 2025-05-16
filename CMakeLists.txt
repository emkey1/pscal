# CMakeLists.txt – top of your repo
cmake_minimum_required(VERSION 3.24) # CMake ≥3.24 recommended on macOS
project(pscal C) # pure‑C project

# ---- options you used everywhere ------------------------------------------
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall)

# Put all executables in <repo>/bin (matches your Makefile)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin) # Changed to CMAKE_BINARY_DIR for out-of-source builds
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib) # For static libs if any
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib) # For shared libs if any


# ---- Version Generation Logic ----
string(TIMESTAMP CURRENT_MONTH "%m")
string(TIMESTAMP CURRENT_DAY "%d")
string(TIMESTAMP CURRENT_YEAR "%Y")
string(TIMESTAMP CURRENT_HOUR "%H")
string(TIMESTAMP CURRENT_MINUTE "%M")

set(BASE_VERSION "${CURRENT_YEAR}${CURRENT_MONTH}${CURRENT_DAY}.${CURRENT_HOUR}${CURRENT_MINUTE}")

# Add an option for RELEASE build, default to OFF (meaning DEV)
option(RELEASE_BUILD "Build as a release version (appends _REL)" OFF)

if(RELEASE_BUILD)
    set(VERSION_SUFFIX "_REL")
else()
    set(VERSION_SUFFIX "_DEV")
endif()

set(PROGRAM_VERSION_STRING "${BASE_VERSION}${VERSION_SUFFIX}")
message(STATUS "Building Pscal Version: ${PROGRAM_VERSION_STRING}")

# ---- gather sources -------------------------------------------------------
file(GLOB SRC_FILES CONFIGURE_DEPENDS src/*.c src/*.h)

# ---- Common compile definitions for version ----
# This will be applied to all targets unless overridden
add_compile_definitions(PROGRAM_VERSION="${PROGRAM_VERSION_STRING}")

# ---- Release‑style build : pscal ------------------------------------------
add_executable(pscal ${SRC_FILES})
target_compile_options(pscal PRIVATE -O3)
# If RELEASE_BUILD is ON, also define RELEASE for pscal target's C code
if(RELEASE_BUILD)
    target_compile_definitions(pscal PRIVATE RELEASE)
endif()

# ---- Address‑Sanitized debug build : dscal -------------------------------
add_executable(dscal ${SRC_FILES})
target_compile_definitions(dscal PRIVATE DEBUG) # DEBUG is already added
target_compile_options( dscal PRIVATE -fsanitize=address -g)
target_link_options( dscal PRIVATE -fsanitize=address)

# ---- “no‑exit” build : hscal (formerly nscal) -----------------------------
add_executable(hscal ${SRC_FILES})
target_compile_definitions(hscal PRIVATE SUPPRESS_EXIT)
# If you want hscal to also be a "debug" build in terms of features (not sanitizers)
# you could add DEBUG definition here too, or make it inherit default DEBUG if defined globally.
# For now, let's assume it's a "normal" build with just SUPPRESS_EXIT.
target_compile_options( hscal PRIVATE -g)


# ---- optional install rule (matches `make install`) -----------------------
include(GNUInstallDirs)
install(TARGETS pscal dscal RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}") # Removed hscal for now, add if needed

# --------------------------------------------------------------------------
# SDL Support (Your existing SDL configuration logic follows)
# No changes needed to this part for the version string, assuming it's correct.

# --- Find SDL2 (Core) ---
find_package(SDL2 REQUIRED) 
message(STATUS "SDL2_INCLUDE_DIRS = ${SDL2_INCLUDE_DIRS}")
message(STATUS "SDL2_LIBRARIES = ${SDL2_LIBRARIES}")

# --- Platform-Specific SDL2_ttf and SDL2_mixer Configuration ---
set(PLATFORM_SDL2_TTF_INCLUDE_DIRS "")
set(PLATFORM_SDL2_TTF_LIBRARIES "")
set(PLATFORM_SDL2_MIXER_INCLUDE_DIRS "")
set(PLATFORM_SDL2_MIXER_LIBRARIES "")

if(APPLE)
    message(STATUS "Platform: Apple macOS - Finding SDL2_ttf and SDL2_mixer...")
    find_package(SDL2_ttf QUIET)
    find_package(SDL2_mixer QUIET) 

    set(SDL2_TTF_SEARCH_INCLUDE_DIRS ${SDL2_TTF_INCLUDE_DIRS})
    set(SDL2_TTF_SEARCH_LIBRARIES ${SDL2_TTF_LIBRARIES})

    if(NOT SDL2_TTF_FOUND OR NOT SDL2_TTF_SEARCH_INCLUDE_DIRS) 
        message(STATUS "Standard find_package for SDL2_ttf inconclusive, trying brew --prefix...")
        execute_process(
            COMMAND brew --prefix sdl2_ttf
            OUTPUT_VARIABLE SDL2_TTF_BREW_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE SDL2_TTF_BREW_RESULT
            ERROR_QUIET 
        )
        if(SDL2_TTF_BREW_RESULT EQUAL 0 AND SDL2_TTF_BREW_PREFIX AND EXISTS "${SDL2_TTF_BREW_PREFIX}/include/SDL2/SDL_ttf.h")
            message(STATUS "Found SDL2_ttf include path via brew --prefix: ${SDL2_TTF_BREW_PREFIX}/include")
            list(APPEND SDL2_TTF_SEARCH_INCLUDE_DIRS "${SDL2_TTF_BREW_PREFIX}/include")
            find_library(SDL2_TTF_LIBRARY_PATH_BREW NAMES SDL2_ttf PATHS "${SDL2_TTF_BREW_PREFIX}/lib" NO_DEFAULT_PATH)
            if(SDL2_TTF_LIBRARY_PATH_BREW)
                message(STATUS "Found SDL2_ttf library via brew path: ${SDL2_TTF_LIBRARY_PATH_BREW}")
                list(APPEND SDL2_TTF_SEARCH_LIBRARIES ${SDL2_TTF_LIBRARY_PATH_BREW}) 
                mark_as_advanced(SDL2_TTF_LIBRARY_PATH_BREW) 
            else()
                message(WARNING "Could not find SDL2_ttf library in brew path (${SDL2_TTF_BREW_PREFIX}/lib). Using name 'SDL2_ttf'.")
                list(APPEND SDL2_TTF_SEARCH_LIBRARIES "SDL2_ttf")
            endif()
        else()
            message(WARNING "Could not find SDL2_ttf via standard CMake find or brew on macOS.")
            if(NOT SDL2_TTF_SEARCH_LIBRARIES)
                list(APPEND SDL2_TTF_SEARCH_LIBRARIES "SDL2_ttf")
            endif()
        endif()
    endif() 

    set(SDL2_MIXER_SEARCH_INCLUDE_DIRS ${SDL2_mixer_INCLUDE_DIRS})
    set(SDL2_MIXER_SEARCH_LIBRARIES ${SDL2_mixer_LIBRARIES})

    if(NOT SDL2_mixer_FOUND OR NOT SDL2_MIXER_SEARCH_INCLUDE_DIRS) 
        message(STATUS "Standard find_package for SDL2_mixer inconclusive, trying brew --prefix...")
        execute_process(
            COMMAND brew --prefix sdl2_mixer 
            OUTPUT_VARIABLE SDL2_MIXER_BREW_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE SDL2_MIXER_BREW_RESULT
            ERROR_QUIET
        )
        if(SDL2_MIXER_BREW_RESULT EQUAL 0 AND SDL2_MIXER_BREW_PREFIX AND EXISTS "${SDL2_MIXER_BREW_PREFIX}/include/SDL2/SDL_mixer.h")
            message(STATUS "Found SDL2_mixer include path via brew --prefix: ${SDL2_MIXER_BREW_PREFIX}/include")
            list(APPEND SDL2_MIXER_SEARCH_INCLUDE_DIRS "${SDL2_MIXER_BREW_PREFIX}/include")
            find_library(SDL2_MIXER_LIBRARY_PATH_BREW NAMES SDL2_mixer PATHS "${SDL2_MIXER_BREW_PREFIX}/lib" NO_DEFAULT_PATH)
            if(SDL2_MIXER_LIBRARY_PATH_BREW)
                message(STATUS "Found SDL2_mixer library via brew path: ${SDL2_MIXER_LIBRARY_PATH_BREW}")
                list(APPEND SDL2_MIXER_SEARCH_LIBRARIES ${SDL2_MIXER_LIBRARY_PATH_BREW}) 
                mark_as_advanced(SDL2_MIXER_LIBRARY_PATH_BREW)
            else()
                message(WARNING "Could not find SDL2_mixer library in brew path (${SDL2_MIXER_BREW_PREFIX}/lib). Using name 'SDL2_mixer'.")
                list(APPEND SDL2_MIXER_SEARCH_LIBRARIES "SDL2_mixer")
            endif()
        else()
            message(WARNING "Could not find SDL2_mixer via standard CMake find or brew on macOS.")
            if(NOT SDL2_MIXER_SEARCH_LIBRARIES)
                list(APPEND SDL2_MIXER_SEARCH_LIBRARIES "SDL2_mixer")
            endif()
        endif()
    endif() 

    if(NOT SDL2_TTF_SEARCH_INCLUDE_DIRS OR NOT SDL2_TTF_SEARCH_LIBRARIES)
        message(FATAL_ERROR "SDL2_ttf is REQUIRED but could not be configured on macOS after all search attempts.")
    endif()
    if(NOT SDL2_MIXER_SEARCH_INCLUDE_DIRS OR NOT SDL2_MIXER_SEARCH_LIBRARIES) 
        message(FATAL_ERROR "SDL2_mixer is REQUIRED but could not be configured on macOS after all search attempts.") 
    endif()

    set(PLATFORM_SDL2_TTF_INCLUDE_DIRS ${SDL2_TTF_SEARCH_INCLUDE_DIRS})
    set(PLATFORM_SDL2_TTF_LIBRARIES ${SDL2_TTF_SEARCH_LIBRARIES})
    set(PLATFORM_SDL2_MIXER_INCLUDE_DIRS ${SDL2_MIXER_SEARCH_INCLUDE_DIRS}) 
    set(PLATFORM_SDL2_MIXER_LIBRARIES ${SDL2_MIXER_SEARCH_LIBRARIES})    

elseif(UNIX) 
    message(STATUS "Platform: Unix (Non-Apple)")
    message(STATUS "Using standard paths for SDL2_ttf and SDL2_mixer on Unix system.") 
    set(PLATFORM_SDL2_TTF_INCLUDE_DIRS "/usr/include")
    set(PLATFORM_SDL2_TTF_LIBRARIES "SDL2_ttf")
    set(PLATFORM_SDL2_MIXER_INCLUDE_DIRS "/usr/include") 
    set(PLATFORM_SDL2_MIXER_LIBRARIES "SDL2_mixer")   

else()
    message(FATAL_ERROR "Unsupported platform for SDL configuration.") 
endif()

message(STATUS "Final Platform SDL2_TTF Include Dirs = ${PLATFORM_SDL2_TTF_INCLUDE_DIRS}")
message(STATUS "Final Platform SDL2_TTF Libraries = ${PLATFORM_SDL2_TTF_LIBRARIES}")
message(STATUS "Final Platform SDL2_MIXER Include Dirs = ${PLATFORM_SDL2_MIXER_INCLUDE_DIRS}")
message(STATUS "Final Platform SDL2_MIXER Libraries = ${PLATFORM_SDL2_MIXER_LIBRARIES}")   

target_include_directories(pscal PRIVATE ${SDL2_INCLUDE_DIRS} ${PLATFORM_SDL2_TTF_INCLUDE_DIRS} ${PLATFORM_SDL2_MIXER_INCLUDE_DIRS}) 
target_include_directories(dscal PRIVATE ${SDL2_INCLUDE_DIRS} ${PLATFORM_SDL2_TTF_INCLUDE_DIRS} ${PLATFORM_SDL2_MIXER_INCLUDE_DIRS}) 
target_include_directories(hscal PRIVATE ${SDL2_INCLUDE_DIRS} ${PLATFORM_SDL2_TTF_INCLUDE_DIRS} ${PLATFORM_SDL2_MIXER_INCLUDE_DIRS}) 

target_link_libraries(pscal PRIVATE SDL2::SDL2 ${PLATFORM_SDL2_TTF_LIBRARIES} ${PLATFORM_SDL2_MIXER_LIBRARIES}) 
target_link_libraries(dscal PRIVATE SDL2::SDL2 ${PLATFORM_SDL2_TTF_LIBRARIES} ${PLATFORM_SDL2_MIXER_LIBRARIES}) 
target_link_libraries(hscal PRIVATE SDL2::SDL2 ${PLATFORM_SDL2_TTF_LIBRARIES} ${PLATFORM_SDL2_MIXER_LIBRARIES}) 
# --------------------------------------------------------------------------

# --- Link m (math) and curl libraries (Applied to all targets) ---
target_link_libraries(pscal PRIVATE m curl)
target_link_libraries(dscal PRIVATE m curl)
target_link_libraries(hscal PRIVATE m curl)
# --- End ---
