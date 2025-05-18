# CMakeLists.txt – top of your repo
cmake_minimum_required(VERSION 3.24)
project(pscal C)

# ---- Version Generation Logic ----
# YYYYMMDD.HHMM format
string(TIMESTAMP CURRENT_YEAR "%Y")
string(TIMESTAMP CURRENT_MONTH "%m")
string(TIMESTAMP CURRENT_DAY "%d")
string(TIMESTAMP CURRENT_HOUR "%H")
string(TIMESTAMP CURRENT_MINUTE "%M")

set(BASE_VERSION "${CURRENT_YEAR}${CURRENT_MONTH}${CURRENT_DAY}.${CURRENT_HOUR}${CURRENT_MINUTE}") # YYYYMMDD.HHMM

# Add an option for RELEASE build, default to OFF (meaning DEV)
# This allows "cmake .. -DRELEASE_BUILD=ON"
option(RELEASE_BUILD "Build as a release version (appends _REL)" OFF)

if(RELEASE_BUILD)
    set(VERSION_SUFFIX "_REL")
else()
    set(VERSION_SUFFIX "_DEV")
endif()

set(PROGRAM_VERSION_STRING "${BASE_VERSION}${VERSION_SUFFIX}")
message(STATUS "Building Pscal Version: ${PROGRAM_VERSION_STRING}")
# ---- End Version Generation Logic ----

# ---- options -------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall)

# Put all executables in <repo>/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# ---- Common compile definitions for version ----
# This definition will be used by main.c
add_compile_definitions(PROGRAM_VERSION="${PROGRAM_VERSION_STRING}")

# ---- gather sources ------------------------------------------------------
file(GLOB SRC_FILES CONFIGURE_DEPENDS src/*.c src/*.h)

# ---- Release build -------------------------------------------------------
add_executable(pscal ${SRC_FILES})
target_compile_options(pscal PRIVATE -O3)
if(RELEASE_BUILD) # If you use the CMake option RELEASE_BUILD
    target_compile_definitions(pscal PRIVATE RELEASE) # Define RELEASE C macro for pscal target
endif()

# ---- ASan debug build ----------------------------------------------------
#add_executable(dscal ${SRC_FILES})
#target_compile_definitions(dscal PRIVATE DEBUG)
#target_compile_options( dscal PRIVATE -fsanitize=address -g)
#target_link_options( dscal PRIVATE -fsanitize=address)
add_executable(dscal ${SRC_FILES})
target_compile_definitions(dscal PRIVATE DEBUG)
target_compile_options( dscal PRIVATE -fsanitize=address -g)
target_link_options( dscal PRIVATE -fsanitize=address)

# ---- “no-exit” build -----------------------------------------------------
add_executable(hscal ${SRC_FILES})
target_compile_definitions(hscal PRIVATE SUPPRESS_EXIT)
target_compile_options( hscal PRIVATE -g)

# ---- optional install ----------------------------------------------------
include(GNUInstallDirs)
install(TARGETS pscal dscal RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# --------------------------------------------------------------------------
# SDL Support  ──────────────────────────────────────────────────────────────
find_package(SDL2 REQUIRED)
find_package(SDL2_image REQUIRED)
find_package(SDL2_mixer REQUIRED)
find_package(SDL2_ttf REQUIRED)
message(STATUS "SDL2_INCLUDE_DIRS = ${SDL2_INCLUDE_DIRS}") # Core SDL2 includes
# For SDL2::SDL2, SDL2_LIBRARIES might not be the primary way to get link info,
# but FindSDL2.cmake often still populates it.
message(STATUS "SDL2_LIBRARIES    = ${SDL2_LIBRARIES}")

# Initialize PLATFORM_ specific variables for SDL components
foreach(comp TTF MIXER IMAGE)
    set(PLATFORM_SDL2_${comp}_INCLUDE_DIRS "")
    set(PLATFORM_SDL2_${comp}_LIBRARIES "")
endforeach()

# --------------------------------------------------------------------------
if(APPLE)
    message(STATUS "Platform: Apple macOS – hunting SDL2_* components")

    macro(_find_sdl_component COMPONENT brew_name header_name)
        string(TOLOWER "${COMPONENT}" lc_comp) # Use a different variable name like lc_comp
        # Try find_package first which might populate SDL2_${lc_comp}_INCLUDE_DIRS and SDL2_${lc_comp}_LIBRARIES
        find_package(SDL2_${lc_comp} QUIET)

        set(_inc_dirs "${SDL2_${lc_comp}_INCLUDE_DIRS}") # Use the result from find_package
        set(_libs "${SDL2_${lc_comp}_LIBRARIES}")       # Use the result from find_package

        # If find_package didn't find include dirs or the header doesn't exist at that path
        if(NOT _inc_dirs OR NOT EXISTS "${_inc_dirs}/SDL2/${header_name}")
            execute_process(
                COMMAND brew --prefix "${brew_name}"
                OUTPUT_VARIABLE _brew_pref
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _brew_ok
                ERROR_QUIET)
            if(_brew_ok EQUAL 0 AND _brew_pref AND EXISTS "${_brew_pref}/include/SDL2/${header_name}")
                message(STATUS "Found SDL2_${lc_comp} include path via brew: ${_brew_pref}/include")
                list(APPEND _inc_dirs "${_brew_pref}/include") # Append brew path
                find_library(_lib_path NAMES SDL2_${lc_comp}
                                   PATHS "${_brew_pref}/lib" NO_DEFAULT_PATH)
                if(_lib_path)
                    message(STATUS "Found SDL2_${lc_comp} library via brew: ${_lib_path}")
                    list(APPEND _libs "${_lib_path}")
                else()
                    message(WARNING "Could not find SDL2_${lc_comp} library in brew path, using name 'SDL2_${lc_comp}'.")
                    list(APPEND _libs "SDL2_${lc_comp}") # Fallback to name
                endif()
            else()
                message(WARNING "Could not find SDL2_${lc_comp} via brew or standard find_package for includes.")
            endif()
        endif()

        # After attempting find_package and brew, check if we have what we need
        if(NOT _inc_dirs OR NOT _libs) # If still not found, it's an error
             # If find_package provided SDL2_${lc_comp}::SDL2_${lc_comp} target, _libs might be empty but linking works
             # This check might be too strict if modern imported targets are used for extensions too.
             # However, for manual path collection, we need _libs to be populated.
             # Let's assume if find_package(SDL2_${lc_comp}) found something, it's okay.
            if (NOT TARGET SDL2_${lc_comp}::SDL2_${lc_comp} AND (NOT _inc_dirs OR NOT _libs))
                message(FATAL_ERROR "SDL2_${lc_comp} is REQUIRED but could not be properly located on macOS.")
            elseif(TARGET SDL2_${lc_comp}::SDL2_${lc_comp})
                message(STATUS "SDL2_${lc_comp} found as imported target SDL2_${lc_comp}::SDL2_${lc_comp}. Manual paths might be empty if target handles all.")
                # If imported target exists, _libs might be empty, which is fine if we link to the target.
                # We will prioritize linking to the target SDL2_xxx::SDL2_xxx below.
            endif()
        endif()

        # Set the platform-specific variables
        set(PLATFORM_SDL2_${COMPONENT}_INCLUDE_DIRS "${_inc_dirs}")
        set(PLATFORM_SDL2_${COMPONENT}_LIBRARIES "${_libs}")
    endmacro()

    _find_sdl_component(TTF   sdl2_ttf   "SDL_ttf.h")
    _find_sdl_component(MIXER sdl2_mixer "SDL_mixer.h")
    _find_sdl_component(IMAGE sdl2_image "SDL_image.h")

elseif(UNIX)
    message(STATUS "Platform: Generic UNIX – assuming standard system paths")
    # For generic Unix, find_package should work, or we use system defaults.
    # We will primarily rely on the imported targets SDL2_xxx::SDL2_xxx if available.
    # The PLATFORM_ variables can serve as fallbacks if direct target linking fails
    # or if find_package doesn't create imported targets for these extensions.
    foreach(comp TTF MIXER IMAGE)
        string(TOLOWER "${comp}" lc_comp)
        find_package(SDL2_${lc_comp} QUIET) # Try to find it to see if imported target is created
        if(TARGET SDL2_${lc_comp}::SDL2_${lc_comp})
            message(STATUS "Found SDL2_${lc_comp} as imported target SDL2_${lc_comp}::SDL2_${lc_comp} on UNIX.")
            set(PLATFORM_SDL2_${comp}_INCLUDE_DIRS "${SDL2_${lc_comp}_INCLUDE_DIRS}") # Use what find_package found
            set(PLATFORM_SDL2_${comp}_LIBRARIES "${SDL2_${lc_comp}_LIBRARIES}") # Might be empty if target handles it
        else()
            message(STATUS "SDL2_${lc_comp} not found as imported target on UNIX. Assuming standard /usr/include and libSDL2_${lc_comp}.")
            set(PLATFORM_SDL2_${comp}_INCLUDE_DIRS "/usr/include") # Fallback
            set(PLATFORM_SDL2_${comp}_LIBRARIES "SDL2_${lc_comp}") # Fallback
        endif()
    endforeach()
else()
    message(FATAL_ERROR "Unsupported platform for SDL component configuration.")
endif()

# --- Final Status Messages for Platform Variables ---
foreach(comp TTF MIXER IMAGE)
    message(STATUS "Platform SDL2_${comp} Include Dirs = ${PLATFORM_SDL2_${comp}_INCLUDE_DIRS}")
    message(STATUS "Platform SDL2_${comp} Libraries    = ${PLATFORM_SDL2_${comp}_LIBRARIES}")
endforeach()

# --- Consolidate Include Directories ---
set(ALL_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS}) # From find_package(SDL2 REQUIRED)
foreach(comp TTF MIXER IMAGE)
    if(PLATFORM_SDL2_${comp}_INCLUDE_DIRS)
        list(APPEND ALL_SDL_INCLUDE_DIRS ${PLATFORM_SDL2_${comp}_INCLUDE_DIRS})
    endif()
endforeach()
list(REMOVE_DUPLICATES ALL_SDL_INCLUDE_DIRS)
message(STATUS "Consolidated All SDL Include Dirs = ${ALL_SDL_INCLUDE_DIRS}")

# --- Consolidate Libraries ---
# We will try to link against the imported targets if they exist,
# otherwise fall back to the collected library names/paths.
set(ALL_SDL_LIBRARIES SDL2::SDL2) # Core SDL2 target
foreach(comp TTF MIXER IMAGE)
    string(TOLOWER "${comp}" lc_comp)
    if(TARGET SDL2_${lc_comp}::SDL2_${lc_comp})
        list(APPEND ALL_SDL_LIBRARIES SDL2_${lc_comp}::SDL2_${lc_comp})
    elseif(PLATFORM_SDL2_${comp}_LIBRARIES)
        list(APPEND ALL_SDL_LIBRARIES ${PLATFORM_SDL2_${comp}_LIBRARIES})
    else()
        # As a last resort, just add the name if no path or target was found
        list(APPEND ALL_SDL_LIBRARIES "SDL2_${lc_comp}")
        message(WARNING "Could not find imported target or library path for SDL2_${lc_comp}. Linking by name 'SDL2_${lc_comp}'.")
    endif()
endforeach()
list(REMOVE_DUPLICATES ALL_SDL_LIBRARIES)
message(STATUS "Consolidated All SDL Libraries to Link = ${ALL_SDL_LIBRARIES}")

# --- Apply to targets ---
foreach(TARGET_EXE pscal dscal hscal)
    target_include_directories(${TARGET_EXE} PRIVATE ${ALL_SDL_INCLUDE_DIRS})
    target_link_libraries(${TARGET_EXE} PRIVATE ${ALL_SDL_LIBRARIES} m curl) # Added m and curl here
endforeach()
# --------------------------------------------------------------------------

# ---- common libs - m and curl are now linked in the loop above ----
# target_link_libraries(pscal PRIVATE m curl) # These are redundant now
# target_link_libraries(dscal PRIVATE m curl)
# target_link_libraries(hscal PRIVATE m curl)
