# CMakeLists.txt – top of your repo
cmake_minimum_required(VERSION 3.24)    # CMake ≥3.24 recommended on macOS
project(pscal C)                        # pure‑C project

# ---- options you used everywhere ------------------------------------------
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall)

# Put all executables in <repo>/bin  (matches your Makefile)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# ---- gather sources -------------------------------------------------------
file(GLOB SRC_FILES CONFIGURE_DEPENDS src/*.c src/*.h)

# ---- Release‑style build : pscal ------------------------------------------
add_executable(pscal  ${SRC_FILES})
target_compile_options(pscal PRIVATE -O3)      

# ---- Address‑Sanitized debug build : dscal -------------------------------
add_executable(dscal  ${SRC_FILES})
target_compile_definitions(dscal PRIVATE )
#target_compile_definitions(dscal PRIVATE DEBUG)
target_compile_options( dscal PRIVATE -fsanitize=address -g)
target_link_options(    dscal PRIVATE -fsanitize=address)

# ---- “no‑exit” build : hscal (formerly nscal) -----------------------------
add_executable(hscal  ${SRC_FILES})
target_compile_definitions(hscal PRIVATE SUPPRESS_EXIT)
target_compile_options( hscal PRIVATE -g)

# ---- optional install rule (matches `make install`) -----------------------
include(GNUInstallDirs)
install(TARGETS pscal dscal RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# --------------------------------------------------------------------------
# SDL Support

# --- Find SDL2 (Core) ---
# Assuming SDL2 is found correctly by find_package on both platforms
find_package(SDL2 REQUIRED)
message(STATUS "SDL2_INCLUDE_DIRS = ${SDL2_INCLUDE_DIRS}")
message(STATUS "SDL2_LIBRARIES = ${SDL2_LIBRARIES}")

# --- Platform-Specific SDL2_ttf Configuration ---
set(PLATFORM_SDL2_TTF_INCLUDE_DIRS "") # Initialize common variable
set(PLATFORM_SDL2_TTF_LIBRARIES "")    # Initialize common variable
if(APPLE)
    message(STATUS "Platform: Apple macOS - Finding SDL2_ttf...")
    # Try CMake's find_package first on macOS
    find_package(SDL2_ttf QUIET) # Use QUIET initially, check results below

    set(PLATFORM_SDL2_TTF_INCLUDE_DIRS ${SDL2_TTF_INCLUDE_DIRS})
    set(PLATFORM_SDL2_TTF_LIBRARIES ${SDL2_TTF_LIBRARIES})

    # Fallback using Homebrew if CMake's find_package didn't populate variables well
    if(NOT SDL2_TTF_FOUND OR NOT PLATFORM_SDL2_TTF_INCLUDE_DIRS)
        message(STATUS "CMake find_package for SDL2_ttf yielded no include dirs or package not found, trying brew --prefix...")
        execute_process(
            COMMAND brew --prefix sdl2_ttf
            OUTPUT_VARIABLE SDL2_TTF_BREW_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE SDL2_TTF_BREW_RESULT
            ERROR_QUIET # Suppress brew errors if not found
        )
        if(SDL2_TTF_BREW_RESULT EQUAL 0 AND SDL2_TTF_BREW_PREFIX AND EXISTS "${SDL2_TTF_BREW_PREFIX}/include/SDL2/SDL_ttf.h")
            message(STATUS "Using SDL2_ttf include path from brew --prefix: ${SDL2_TTF_BREW_PREFIX}/include")
            set(PLATFORM_SDL2_TTF_INCLUDE_DIRS "${SDL2_TTF_BREW_PREFIX}/include")

            # ---- START CHANGE ----
            # Explicitly find the library within the brew prefix using find_library
            find_library(SDL2_TTF_LIBRARY_PATH_BREW NAMES SDL2_ttf PATHS "${SDL2_TTF_BREW_PREFIX}/lib" NO_DEFAULT_PATH)
            if(SDL2_TTF_LIBRARY_PATH_BREW)
                message(STATUS "Found SDL2_ttf library via brew path: ${SDL2_TTF_LIBRARY_PATH_BREW}")
                set(PLATFORM_SDL2_TTF_LIBRARIES ${SDL2_TTF_LIBRARY_PATH_BREW}) # Use the full path found
                mark_as_advanced(SDL2_TTF_LIBRARY_PATH_BREW) # Hide this intermediate var from basic cmake-gui
            else()
                # If find_library fails even in brew path, warn and fallback to name (might still fail link)
                message(WARNING "Could not find SDL2_ttf library in brew path (${SDL2_TTF_BREW_PREFIX}/lib). Using name 'SDL2_ttf'.")
                set(PLATFORM_SDL2_TTF_LIBRARIES "SDL2_ttf")
            endif()
            # ---- END CHANGE ----

        else()
            message(WARNING "Could not find SDL2_ttf via CMake or brew on macOS.")
            # If we got here, both find_package and brew failed. Set library to name as last resort.
            if(NOT PLATFORM_SDL2_TTF_LIBRARIES)
                set(PLATFORM_SDL2_TTF_LIBRARIES "SDL2_ttf")
            endif()
        endif()
    endif()

    # Final check for macOS - ensure we have something if REQUIRED
    if(NOT PLATFORM_SDL2_TTF_INCLUDE_DIRS OR NOT PLATFORM_SDL2_TTF_LIBRARIES)
         # If find_package was truly REQUIRED, this will trigger the failure
         find_package(SDL2_ttf REQUIRED)
         # If execution reaches here, find_package somehow succeeded without populating vars - raise error
         message(FATAL_ERROR "SDL2_ttf is REQUIRED but could not be configured on macOS.")
    endif()
elseif(UNIX) # Catches Linux, BSD, etc. (but APPLE was handled above)
    message(STATUS "Platform: Unix (Non-Apple)")
    # For Linux, use the manually determined paths due to the previous issues
    message(STATUS "Using manual paths for SDL2_ttf on Unix system.")
    set(PLATFORM_SDL2_TTF_INCLUDE_DIRS "/usr/include")
    set(PLATFORM_SDL2_TTF_LIBRARIES "SDL2_ttf")
else()
    message(FATAL_ERROR "Unsupported platform for SDL2_ttf configuration.")
endif()

message(STATUS "Final Platform SDL2_TTF Include Dirs = ${PLATFORM_SDL2_TTF_INCLUDE_DIRS}")
message(STATUS "Final Platform SDL2_TTF Libraries = ${PLATFORM_SDL2_TTF_LIBRARIES}")

# Apply SDL settings to ALL targets that need it
# Use the platform-specific variables determined above
target_include_directories(pscal PRIVATE ${SDL2_INCLUDE_DIRS} ${PLATFORM_SDL2_TTF_INCLUDE_DIRS})
target_include_directories(dscal PRIVATE ${SDL2_INCLUDE_DIRS} ${PLATFORM_SDL2_TTF_INCLUDE_DIRS})
target_include_directories(hscal PRIVATE ${SDL2_INCLUDE_DIRS} ${PLATFORM_SDL2_TTF_INCLUDE_DIRS})

# Link SDL2 using its target (if found), link SDL2_ttf using the platform variable
target_link_libraries(pscal PRIVATE SDL2::SDL2 ${PLATFORM_SDL2_TTF_LIBRARIES})
target_link_libraries(dscal PRIVATE SDL2::SDL2 ${PLATFORM_SDL2_TTF_LIBRARIES})
target_link_libraries(hscal PRIVATE SDL2::SDL2 ${PLATFORM_SDL2_TTF_LIBRARIES})
# --------------------------------------------------------------------------

# --- Link m and curl (Applied to all targets as they need them) ---
target_link_libraries(pscal PRIVATE m curl)
target_link_libraries(dscal PRIVATE m curl)
target_link_libraries(hscal PRIVATE m curl)
# --- End ---
