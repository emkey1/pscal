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

# ---- Address‑Sanitized debug build : dscal -------------------------------
add_executable(dscal  ${SRC_FILES})
target_compile_definitions(dscal PRIVATE DEBUG)
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
find_package(SDL2 REQUIRED)
message(STATUS "SDL2_INCLUDE_DIRS = ${SDL2_INCLUDE_DIRS}")
message(STATUS "SDL2_LIBRARIES = ${SDL2_LIBRARIES}")

# --- Find SDL2_ttf ---
find_package(SDL2_ttf REQUIRED)
message(STATUS "CMake's SDL2_TTF_INCLUDE_DIRS = ${SDL2_TTF_INCLUDE_DIRS}") # Original attempt
message(STATUS "CMake's SDL2_TTF_LIBRARIES = ${SDL2_TTF_LIBRARIES}")     # Original attempt

# Attempt to get SDL2_ttf paths from Homebrew if CMake's find_package is insufficient
set(EFFECTIVE_SDL2_TTF_INCLUDE_DIRS ${SDL2_TTF_INCLUDE_DIRS}) # Start with CMake's version
set(EFFECTIVE_SDL2_TTF_LIBRARIES ${SDL2_TTF_LIBRARIES})   # Start with CMake's version

if(APPLE) # Only run brew specific things on Apple systems
    execute_process(
        COMMAND brew --prefix sdl2_ttf
        OUTPUT_VARIABLE SDL2_TTF_BREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE SDL2_TTF_BREW_RESULT
        ERROR_QUIET # Suppress brew command errors if brew is not found, etc.
    )
    if(SDL2_TTF_BREW_RESULT EQUAL 0 AND SDL2_TTF_BREW_PREFIX AND EXISTS "${SDL2_TTF_BREW_PREFIX}/include/SDL2/SDL_ttf.h")
        message(STATUS "Found SDL2_ttf via brew --prefix: ${SDL2_TTF_BREW_PREFIX}")
        set(EFFECTIVE_SDL2_TTF_INCLUDE_DIRS "${SDL2_TTF_BREW_PREFIX}/include")
        # For linking, we often still rely on SDL2_TTF_LIBRARIES which might be populated
        # correctly by find_package even if include_dirs isn't, or we link directly.
        # If SDL2_TTF_LIBRARIES is empty, explicitly set it too:
        if(NOT EFFECTIVE_SDL2_TTF_LIBRARIES)
            set(EFFECTIVE_SDL2_TTF_LIBRARIES "${SDL2_TTF_BREW_PREFIX}/lib/libSDL2_ttf.dylib") # Or just SDL2_ttf if link_directories is used
            # It's often better to let find_package handle LIBRARIES if possible,
            # or use find_library command if this direct path is too brittle.
            # For now, let's assume SDL2_TTF_LIBRARIES was correctly set by find_package for linking,
            # and only override include dirs if brew gives a better path.
            # If SDL2_TTF_LIBRARIES remains empty, linking will fail later.
        endif()
    else()
        message(WARNING "Could not reliably determine SDL2_ttf path from brew, or header not found. Relying on CMake's find_package variables for SDL2_ttf.")
        # EFFECTIVE_SDL2_TTF_INCLUDE_DIRS and EFFECTIVE_SDL2_TTF_LIBRARIES will retain their values from find_package
    endif()
endif()

message(STATUS "Effective SDL2_TTF_INCLUDE_DIRS = ${EFFECTIVE_SDL2_TTF_INCLUDE_DIRS}")
message(STATUS "Effective SDL2_TTF_LIBRARIES = ${EFFECTIVE_SDL2_TTF_LIBRARIES}")

# Apply SDL settings to ALL targets that need it
target_include_directories(pscal PRIVATE ${SDL2_INCLUDE_DIRS} ${EFFECTIVE_SDL2_TTF_INCLUDE_DIRS})
target_include_directories(dscal PRIVATE ${SDL2_INCLUDE_DIRS} ${EFFECTIVE_SDL2_TTF_INCLUDE_DIRS})
target_include_directories(hscal PRIVATE ${SDL2_INCLUDE_DIRS} ${EFFECTIVE_SDL2_TTF_INCLUDE_DIRS})

target_link_libraries(pscal PRIVATE SDL2::SDL2 ${EFFECTIVE_SDL2_TTF_LIBRARIES})
target_link_libraries(dscal PRIVATE SDL2::SDL2 ${EFFECTIVE_SDL2_TTF_LIBRARIES})
target_link_libraries(hscal PRIVATE SDL2::SDL2 ${EFFECTIVE_SDL2_TTF_LIBRARIES})
# --------------------------------------------------------------------------

# --- Link m and curl (Applied to all targets as they need them) ---
target_link_libraries(pscal PRIVATE m curl)
target_link_libraries(dscal PRIVATE m curl)
target_link_libraries(hscal PRIVATE m curl)
# --- End ---
