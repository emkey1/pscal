# CMakeLists.txt – top of your repo
cmake_minimum_required(VERSION 3.24)
project(pscal C)
enable_testing()

# ---- Version Generation Logic ----
string(TIMESTAMP CURRENT_YEAR "%Y")
string(TIMESTAMP CURRENT_MONTH "%m")
string(TIMESTAMP CURRENT_DAY "%d")
string(TIMESTAMP CURRENT_HOUR "%H")
string(TIMESTAMP CURRENT_MINUTE "%M")
set(BASE_VERSION "${CURRENT_YEAR}${CURRENT_MONTH}${CURRENT_DAY}.${CURRENT_HOUR}${CURRENT_MINUTE}")
option(RELEASE_BUILD "Build as a release version (appends _REL)" OFF)
if(RELEASE_BUILD)
    set(VERSION_SUFFIX "_REL")
else()
    set(VERSION_SUFFIX "_DEV")
endif()
set(PROGRAM_VERSION_STRING "${BASE_VERSION}${VERSION_SUFFIX}")
message(STATUS "Building Pscal Version: ${PROGRAM_VERSION_STRING}")

# Discover the most recent annotated tag so `-v` can report it alongside the
# build identifier. Fall back to "untagged" when the repository has no tags or
# when `git` is unavailable (for example in source snapshots).
execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PSCAL_GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_DESCRIBE_RESULT
)
if(NOT GIT_DESCRIBE_RESULT EQUAL 0 OR PSCAL_GIT_TAG STREQUAL "")
    set(PSCAL_GIT_TAG "untagged")
endif()
# ---- End Version Generation Logic ----

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_compile_definitions(
    PROGRAM_VERSION="${PROGRAM_VERSION_STRING}"
    PSCAL_GIT_TAG="${PSCAL_GIT_TAG}"
)

set(PSCAL_INSTALL_ROOT "/usr/local/pscal" CACHE PATH "Default PSCAL runtime asset directory")
string(REGEX REPLACE "/$" "" PSCAL_INSTALL_ROOT "${PSCAL_INSTALL_ROOT}")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
configure_file(src/core/pscal_paths.h.in "${CMAKE_BINARY_DIR}/generated/pscal_paths.h" @ONLY)
include_directories("${CMAKE_BINARY_DIR}/generated")
file(WRITE "${CMAKE_BINARY_DIR}/pscal_install_root.txt" "${PSCAL_INSTALL_ROOT}\n")

# Optionally build additional binaries
option(BUILD_DASCAL "Build the dascal debugging binary" OFF)
option(BUILD_PSCALD "Build the standalone bytecode decompiler" ON)

# Optionally enable SDL-based features
option(SDL "Enable SDL-based graphics and audio" ON)
set(PSCAL_SDL_PLATFORM_LIBS "")

# ---- Extended builtin categories ----
option(ENABLE_EXT_BUILTIN_MATH "Enable extended math builtins" ON)
option(ENABLE_EXT_BUILTIN_STRINGS "Enable extended string builtins" ON)
option(ENABLE_EXT_BUILTIN_SYSTEM "Enable extended system builtins" ON)
option(ENABLE_EXT_BUILTIN_USER "Enable extended user builtins" ON)
option(ENABLE_EXT_BUILTIN_YYJSON "Enable extended yyjson builtins" ON)
option(ENABLE_EXT_BUILTIN_SQLITE "Enable extended sqlite builtins" ON)

# Default the 3D extended builtins to ON whenever SDL support is enabled so
# the bundled 3D demos (which rely on the physics helpers) continue to work
# out of the box.  Respect explicit cache values when users override the
# toggle on the command line.
set(_ENABLE_EXT_BUILTIN_3D_DEFAULT OFF)
if(SDL)
    set(_ENABLE_EXT_BUILTIN_3D_DEFAULT ON)
endif()
option(ENABLE_EXT_BUILTIN_3D "Enable extended 3D builtins" ${_ENABLE_EXT_BUILTIN_3D_DEFAULT})
option(ENABLE_EXT_BUILTIN_GRAPHICS "Enable extended graphics builtins" ON)
option(ENABLE_EXT_BUILTIN_OPENAI "Enable extended OpenAI builtins" ON)

if(ENABLE_EXT_BUILTIN_MATH)
    add_compile_definitions(ENABLE_EXT_BUILTIN_MATH)
endif()
if(ENABLE_EXT_BUILTIN_STRINGS)
    add_compile_definitions(ENABLE_EXT_BUILTIN_STRINGS)
endif()
if(ENABLE_EXT_BUILTIN_SYSTEM)
    add_compile_definitions(ENABLE_EXT_BUILTIN_SYSTEM)
endif()
if(ENABLE_EXT_BUILTIN_YYJSON)
    add_compile_definitions(ENABLE_EXT_BUILTIN_YYJSON)
endif()
if(ENABLE_EXT_BUILTIN_USER)
    add_compile_definitions(ENABLE_EXT_BUILTIN_USER)
endif()
if(ENABLE_EXT_BUILTIN_3D)
    add_compile_definitions(ENABLE_EXT_BUILTIN_3D)
endif()
if(ENABLE_EXT_BUILTIN_GRAPHICS)
    add_compile_definitions(ENABLE_EXT_BUILTIN_GRAPHICS)
endif()
if(ENABLE_EXT_BUILTIN_OPENAI)
    add_compile_definitions(ENABLE_EXT_BUILTIN_OPENAI)
endif()

set(PSCAL_SQLITE_LIBS "")
set(PSCAL_SQLITE_INCLUDE_DIRS "")
if(ENABLE_EXT_BUILTIN_SQLITE)
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        add_compile_definitions(ENABLE_EXT_BUILTIN_SQLITE)
        if(SQLite3_LIBRARIES)
            set(PSCAL_SQLITE_LIBS ${SQLite3_LIBRARIES})
        endif()
        if(SQLite3_INCLUDE_DIRS)
            set(PSCAL_SQLITE_INCLUDE_DIRS ${SQLite3_INCLUDE_DIRS})
        endif()
    else()
        message(WARNING "SQLite3 development files not found; disabling sqlite extended builtins. Set -DENABLE_EXT_BUILTIN_SQLITE=OFF to silence this warning.")
        set(ENABLE_EXT_BUILTIN_SQLITE OFF CACHE BOOL "Enable extended sqlite builtins" FORCE)
    endif()
endif()

# ---- macOS SDK auto-detect (fixes stale MacOSX.sdk paths) ----
if(APPLE AND NOT DEFINED CMAKE_OSX_SYSROOT)
    execute_process(
        COMMAND xcrun --sdk macosx --show-sdk-path
        OUTPUT_VARIABLE _MACOSX_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_MACOSX_SDK_PATH AND EXISTS "${_MACOSX_SDK_PATH}/usr/include")
        set(CMAKE_OSX_SYSROOT "${_MACOSX_SDK_PATH}" CACHE PATH "Selected macOS SDK" FORCE)
        message(STATUS "Using macOS SDK: ${CMAKE_OSX_SYSROOT}")
    endif()
endif()

# ---- Include Directories (Project Structure) ----
include_directories(
    src
    src/core
    src/Pascal
    src/clike
    src/symbol
    src/backend_ast
    src/compiler
    src/vm
    src/third_party/yyjson
)
if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_INCLUDE_DIRS)
    include_directories(${PSCAL_SQLITE_INCLUDE_DIRS})
endif()

# ---- Dependencies ----
set(PSCAL_OPENGL_TARGET "")

if(SDL)
    find_package(PkgConfig REQUIRED)

    set(_PSCAL_SDL2_FOUND FALSE)
    set(_PSCAL_SDL2_PROVIDER "")

    # --- SDL2 Core (CMake config first, pkg-config fallback) ---
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        set(_PSCAL_SDL2_FOUND TRUE)
        set(_PSCAL_SDL2_PROVIDER "CMake package")
    else()
        pkg_check_modules(PC_SDL2 QUIET sdl2)
        if(PC_SDL2_FOUND)
            set(_PSCAL_SDL2_FOUND TRUE)
            set(_PSCAL_SDL2_PROVIDER "pkg-config")
            set(SDL2_INCLUDE_DIRS ${PC_SDL2_INCLUDE_DIRS})
            set(SDL2_LIBRARIES ${PC_SDL2_LIBRARIES})
        endif()
    endif()

    if(NOT _PSCAL_SDL2_FOUND)
        message(WARNING "SDL2 development files not found; disabling SDL support. Re-run with -DSDL=OFF to silence this warning.")
        set(SDL OFF)
        set(SDL OFF CACHE BOOL "Enable SDL-based graphics and audio" FORCE)
    else()
        add_compile_definitions(SDL)
        message(STATUS "SDL2 detected via ${_PSCAL_SDL2_PROVIDER}")
        message(STATUS "SDL2 Core Include Dirs: ${SDL2_INCLUDE_DIRS}")
        message(STATUS "SDL2 Core Libraries Variable: ${SDL2_LIBRARIES}") # e.g., /path/to/libSDL2.dylib or SDL2::SDL2

        # --- SDL2_image ---
        pkg_check_modules(PC_SDL2_IMAGE REQUIRED SDL2_image)
        message(STATUS "SDL2_image (pkg-config) Include Dirs: ${PC_SDL2_IMAGE_INCLUDE_DIRS}")
        message(STATUS "SDL2_image (pkg-config) Libs: ${PC_SDL2_IMAGE_LIBRARIES}")
        message(STATUS "SDL2_image (pkg-config) Lib Dirs: ${PC_SDL2_IMAGE_LIBRARY_DIRS}")
        message(STATUS "SDL2_image (pkg-config) Other LDFLAGS: ${PC_SDL2_IMAGE_LDFLAGS_OTHER}")

        # --- SDL2_mixer ---
        pkg_check_modules(PC_SDL2_MIXER REQUIRED SDL2_mixer)
        message(STATUS "SDL2_mixer (pkg-config) Include Dirs: ${PC_SDL2_MIXER_INCLUDE_DIRS}")
        message(STATUS "SDL2_mixer (pkg-config) Libs: ${PC_SDL2_MIXER_LIBRARIES}")
        message(STATUS "SDL2_mixer (pkg-config) Lib Dirs: ${PC_SDL2_MIXER_LIBRARY_DIRS}")
        message(STATUS "SDL2_mixer (pkg-config) Other LDFLAGS: ${PC_SDL2_MIXER_LDFLAGS_OTHER}")

        # --- SDL2_ttf ---
        pkg_check_modules(PC_SDL2_TTF REQUIRED SDL2_ttf)
        message(STATUS "SDL2_ttf (pkg-config) Include Dirs: ${PC_SDL2_TTF_INCLUDE_DIRS}")
        message(STATUS "SDL2_ttf (pkg-config) Libs: ${PC_SDL2_TTF_LIBRARIES}")
        message(STATUS "SDL2_ttf (pkg-config) Lib Dirs: ${PC_SDL2_TTF_LIBRARY_DIRS}")
        message(STATUS "SDL2_ttf (pkg-config) Other LDFLAGS: ${PC_SDL2_TTF_LDFLAGS_OTHER}")

        if(UNIX AND NOT APPLE)
            find_package(X11)
            if(X11_FOUND)
                if(TARGET X11::X11)
                    list(APPEND PSCAL_SDL_PLATFORM_LIBS X11::X11)
                else()
                    list(APPEND PSCAL_SDL_PLATFORM_LIBS ${X11_LIBRARIES})
                endif()
                message(STATUS "Found X11 for SDL backend: ${PSCAL_SDL_PLATFORM_LIBS}")
            endif()
        endif()

        find_package(OpenGL REQUIRED)
        if(TARGET OpenGL::GL)
            set(PSCAL_OPENGL_TARGET OpenGL::GL)
        elseif(TARGET OpenGL::OpenGL)
            set(PSCAL_OPENGL_TARGET OpenGL::OpenGL)
        elseif(OPENGL_LIBRARIES)
            set(PSCAL_OPENGL_TARGET ${OPENGL_LIBRARIES})
        endif()
    endif()
endif()

# --- CURL (two-path strategy) ---
# Option A (default): use system SDK curl via FindCURL (imported target CURL::libcurl)
# Option B (opt-in): prefer Homebrew curl for OpenSSL, etc.
option(PSCAL_USE_BREW_CURL "Prefer Homebrew-provided curl over the macOS SDK one" OFF)
if(APPLE AND PSCAL_USE_BREW_CURL)
    # Hint FindCURL using Homebrew prefix and pkg-config
    execute_process(
        COMMAND brew --prefix curl
        OUTPUT_VARIABLE _BREW_CURL_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_BREW_CURL_PREFIX AND EXISTS "${_BREW_CURL_PREFIX}")
        list(PREPEND CMAKE_PREFIX_PATH "${_BREW_CURL_PREFIX}")
        # Propagate pkg-config hint for libcurl.pc
        if(DEFINED ENV{PKG_CONFIG_PATH})
            set(ENV{PKG_CONFIG_PATH} "${_BREW_CURL_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
        else()
            set(ENV{PKG_CONFIG_PATH} "${_BREW_CURL_PREFIX}/lib/pkgconfig")
        endif()
        message(STATUS "Hinting CURL with Homebrew prefix: ${_BREW_CURL_PREFIX}")
    else()
        message(WARNING "PSCAL_USE_BREW_CURL=ON but Homebrew curl not found; falling back to system CURL.")
    endif()
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
if(TARGET CURL::libcurl)
    message(STATUS "CURL: using imported target CURL::libcurl")
else()
    message(STATUS "CURL Include Dirs: ${CURL_INCLUDE_DIRS}")
    message(STATUS "CURL Libraries Variable: ${CURL_LIBRARIES}")
endif()

# ---- Source Files ----
set(PSCAL_SOURCES
    src/Pascal/main.c src/Pascal/globals.c
    src/core/types.c src/core/utils.c src/core/list.c src/core/preproc.c src/core/version.c
    src/Pascal/lexer.c src/Pascal/parser.c src/ast/ast.c src/Pascal/opt.c
    src/symbol/symbol.c
    src/backend_ast/builtin.c
    src/backend_ast/builtin_network_api.c
    src/compiler/bytecode.c src/compiler/compiler.c
    src/core/cache.c
    src/vm/vm.c
)

# --- Conditionally add SDL source files ---
if(SDL)
    list(APPEND PSCAL_SOURCES
        src/backend_ast/sdl.c
        src/backend_ast/sdl3d.c
        src/backend_ast/gl.c
        src/backend_ast/audio.c
    )
endif()

# ---- Extended builtins ----
# Allow selectively enabling categories of extended builtins.
add_subdirectory(src/ext_builtins)
list(APPEND PSCAL_SOURCES ${PSCAL_EXT_BUILTIN_SOURCES})
list(APPEND PSCAL_SOURCES src/third_party/yyjson/yyjson.c)

# ---- Toolchain feature detection ----
# Detect whether AddressSanitizer is usable before wiring it into dascal
set(PSCAL_CAN_ENABLE_ASAN OFF)
if(BUILD_DASCAL)
    include(CheckCSourceCompiles)
    set(_PSCAL_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
    if(_PSCAL_SAVED_REQUIRED_FLAGS)
        set(CMAKE_REQUIRED_FLAGS "${_PSCAL_SAVED_REQUIRED_FLAGS} -fsanitize=address")
    else()
        set(CMAKE_REQUIRED_FLAGS "-fsanitize=address")
    endif()
    check_c_source_compiles("#include <stdio.h>\nint main(void){return 0;}\n" PSCAL_HAS_ADDRESS_SANITIZER)
    set(CMAKE_REQUIRED_FLAGS "${_PSCAL_SAVED_REQUIRED_FLAGS}")
    if(PSCAL_HAS_ADDRESS_SANITIZER)
        set(PSCAL_CAN_ENABLE_ASAN ON)
    else()
        message(WARNING "AddressSanitizer (-fsanitize=address) is not available; building dascal without it.")
    endif()
endif()


# ---- Executable Targets Function ----
function(add_pscal_executable target_name)
    add_executable(${target_name} ${PSCAL_SOURCES})
    
    if(SDL)
        target_include_directories(${target_name} PRIVATE
            ${SDL2_INCLUDE_DIRS}
            ${PC_SDL2_IMAGE_INCLUDE_DIRS}
            ${PC_SDL2_MIXER_INCLUDE_DIRS}
            ${PC_SDL2_TTF_INCLUDE_DIRS}
        )
    endif()
    if(NOT TARGET CURL::libcurl AND CURL_INCLUDE_DIRS)
        target_include_directories(${target_name} PRIVATE ${CURL_INCLUDE_DIRS})
    endif()

    if(SDL)
        # Add library search paths from pkg-config
        target_link_directories(${target_name} PRIVATE
            ${PC_SDL2_IMAGE_LIBRARY_DIRS}
            ${PC_SDL2_MIXER_LIBRARY_DIRS}
            ${PC_SDL2_TTF_LIBRARY_DIRS}
        )
    endif()

    # Construct the list of libraries and flags to link
    set(TARGET_LINK_LIBS "")

    if(SDL)
        # Core SDL2
        if(TARGET SDL2::SDL2)
            list(APPEND TARGET_LINK_LIBS SDL2::SDL2)
        else()
            list(APPEND TARGET_LINK_LIBS ${SDL2_LIBRARIES})
        endif()

        # SDL Extensions - using the library names from pkg-config
        list(APPEND TARGET_LINK_LIBS ${PC_SDL2_IMAGE_LIBRARIES})
        list(APPEND TARGET_LINK_LIBS ${PC_SDL2_MIXER_LIBRARIES})
        list(APPEND TARGET_LINK_LIBS ${PC_SDL2_TTF_LIBRARIES})

        # Add other linker flags from pkg-config (these might include rpath, etc.)
        list(APPEND TARGET_LINK_LIBS ${PC_SDL2_IMAGE_LDFLAGS_OTHER})
        list(APPEND TARGET_LINK_LIBS ${PC_SDL2_MIXER_LDFLAGS_OTHER})
        list(APPEND TARGET_LINK_LIBS ${PC_SDL2_TTF_LDFLAGS_OTHER})
        if(PSCAL_OPENGL_TARGET)
            list(APPEND TARGET_LINK_LIBS ${PSCAL_OPENGL_TARGET})
        elseif(OPENGL_LIBRARIES)
            list(APPEND TARGET_LINK_LIBS ${OPENGL_LIBRARIES})
        endif()
        list(APPEND TARGET_LINK_LIBS ${PSCAL_SDL_PLATFORM_LIBS})
    endif()

    # CURL
    if(TARGET CURL::libcurl)
        list(APPEND TARGET_LINK_LIBS CURL::libcurl)
    else()
        list(APPEND TARGET_LINK_LIBS ${CURL_LIBRARIES})
    endif()

    if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
        list(APPEND TARGET_LINK_LIBS ${PSCAL_SQLITE_LIBS})
    endif()

    list(APPEND TARGET_LINK_LIBS m) # Math library
    list(APPEND TARGET_LINK_LIBS Threads::Threads)

    # Specific options for dascal (ASan)
    if(${target_name} STREQUAL "dascal" AND PSCAL_CAN_ENABLE_ASAN)
        list(APPEND TARGET_LINK_LIBS "-fsanitize=address")
    endif()

    # Optional: Remove duplicate library names from the list before linking
    if(TARGET_LINK_LIBS)
        list(REMOVE_DUPLICATES TARGET_LINK_LIBS)
    endif()

    message(STATUS "Linking ${target_name} with: ${TARGET_LINK_LIBS}")
    target_link_libraries(${target_name} PRIVATE ${TARGET_LINK_LIBS})

    if(SDL)
        target_compile_definitions(${target_name} PRIVATE SDL)
    endif()

    # Specific compile definitions and options
    if(${target_name} STREQUAL "pascal")
        target_compile_options(${target_name} PRIVATE -O3)
        if(RELEASE_BUILD)
            target_compile_definitions(${target_name} PRIVATE RELEASE)
        else()
            target_compile_definitions(${target_name} PRIVATE DEBUGNOT)
        endif()
    elseif(${target_name} STREQUAL "dascal")
        target_compile_definitions(${target_name} PRIVATE DEBUG)
        if(PSCAL_CAN_ENABLE_ASAN)
            target_compile_options(${target_name} PRIVATE -fsanitize=address -g)
        else()
            target_compile_options(${target_name} PRIVATE -g)
        endif()
    elseif(${target_name} STREQUAL "hscal")
        target_compile_definitions(${target_name} PRIVATE DEBUG SUPPRESS_EXIT)
        target_compile_options(${target_name} PRIVATE -g)
    endif()
endfunction()

add_pscal_executable(pascal)
target_compile_definitions(pascal PRIVATE FRONTEND_PASCAL)
if(BUILD_DASCAL)
    add_pscal_executable(dascal)
endif()
# add_pscal_executable(hscal) // Lets not build this normally

# Standalone VM binary
set(PSCAL_VM_SOURCES ${PSCAL_SOURCES})
list(REMOVE_ITEM PSCAL_VM_SOURCES src/Pascal/main.c)
list(APPEND PSCAL_VM_SOURCES src/vm/vm_main.c)
set(PSCAL_SOURCES_BACKUP ${PSCAL_SOURCES})
set(PSCAL_SOURCES ${PSCAL_VM_SOURCES})
add_pscal_executable(pscalvm)
set(PSCAL_SOURCES ${PSCAL_SOURCES_BACKUP})

if(BUILD_PSCALD)
    set(DISASSEMBLER_SOURCES ${PSCAL_VM_SOURCES})
    list(REMOVE_ITEM DISASSEMBLER_SOURCES src/vm/vm_main.c)
    list(APPEND DISASSEMBLER_SOURCES src/disassembler/main.c)
    set(PSCAL_SOURCES_BACKUP ${PSCAL_SOURCES})
    set(PSCAL_SOURCES ${DISASSEMBLER_SOURCES})
    add_pscal_executable(pscald)
    set(PSCAL_SOURCES ${PSCAL_SOURCES_BACKUP})
endif()

# exsh front-end executable
set(SHELL_SOURCES
    src/shell/main.c
    src/shell/lexer.c
    src/shell/parser.c
    src/shell/ast.c
    src/shell/semantics.c
    src/shell/codegen.c
    src/shell/builtins.c
    src/shell/runner.c
    src/shell/opt.c
    src/shell/stubs.c
    src/backend_ast/builtin.c
    src/backend_ast/shell.c
    src/backend_ast/builtin_network_api.c
    src/Pascal/globals.c
    src/core/utils.c src/core/types.c src/core/list.c src/core/preproc.c src/core/version.c src/core/cache.c
    src/compiler/bytecode.c
    src/vm/vm.c
    src/symbol/symbol.c
)

list(APPEND SHELL_SOURCES ${PSCAL_EXT_BUILTIN_SOURCES})
list(APPEND SHELL_SOURCES ${EXSH_EXT_BUILTIN_SOURCES})
list(APPEND SHELL_SOURCES src/third_party/yyjson/yyjson.c)

if(SDL)
    list(APPEND SHELL_SOURCES
        src/backend_ast/sdl.c
        src/backend_ast/sdl3d.c
        src/backend_ast/gl.c
        src/backend_ast/audio.c
    )
endif()

add_executable(exsh ${SHELL_SOURCES})
target_compile_definitions(exsh PRIVATE FRONTEND_SHELL)
if(TARGET CURL::libcurl)
    target_link_libraries(exsh PRIVATE CURL::libcurl m Threads::Threads)
else()
    target_link_libraries(exsh PRIVATE ${CURL_LIBRARIES} m Threads::Threads)
endif()
if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
    target_link_libraries(exsh PRIVATE ${PSCAL_SQLITE_LIBS})
endif()

if(SDL)
    target_include_directories(exsh PRIVATE
        ${SDL2_INCLUDE_DIRS}
        ${PC_SDL2_IMAGE_INCLUDE_DIRS}
        ${PC_SDL2_MIXER_INCLUDE_DIRS}
        ${PC_SDL2_TTF_INCLUDE_DIRS}
    )

    target_link_directories(exsh PRIVATE
        ${PC_SDL2_IMAGE_LIBRARY_DIRS}
        ${PC_SDL2_MIXER_LIBRARY_DIRS}
        ${PC_SDL2_TTF_LIBRARY_DIRS}
    )

    set(EXSH_SDL_LIBS "")
    if(TARGET SDL2::SDL2)
        list(APPEND EXSH_SDL_LIBS SDL2::SDL2)
    else()
        list(APPEND EXSH_SDL_LIBS ${SDL2_LIBRARIES})
    endif()
    list(APPEND EXSH_SDL_LIBS
        ${PC_SDL2_IMAGE_LIBRARIES}
        ${PC_SDL2_MIXER_LIBRARIES}
        ${PC_SDL2_TTF_LIBRARIES}
        ${PC_SDL2_IMAGE_LDFLAGS_OTHER}
        ${PC_SDL2_MIXER_LDFLAGS_OTHER}
        ${PC_SDL2_TTF_LDFLAGS_OTHER}
    )
    if(PSCAL_OPENGL_TARGET)
        list(APPEND EXSH_SDL_LIBS ${PSCAL_OPENGL_TARGET})
    elseif(OPENGL_LIBRARIES)
        list(APPEND EXSH_SDL_LIBS ${OPENGL_LIBRARIES})
    endif()
    list(APPEND EXSH_SDL_LIBS ${PSCAL_SDL_PLATFORM_LIBS})
    list(REMOVE_DUPLICATES EXSH_SDL_LIBS)
    target_link_libraries(exsh PRIVATE ${EXSH_SDL_LIBS})
    target_compile_definitions(exsh PRIVATE SDL)
endif()

# Tiny C front-end executable
set(CLIKE_SOURCES
    src/clike/main.c
    src/clike/lexer.c
    src/clike/parser.c
    src/clike/ast.c
    src/clike/builtins.c
    src/clike/semantics.c
    src/clike/codegen.c
    src/clike/opt.c
    src/clike/preproc.c
    src/Pascal/globals.c
    src/core/utils.c src/core/types.c src/core/list.c src/core/preproc.c src/core/version.c src/core/cache.c
    src/compiler/bytecode.c
    src/vm/vm.c
    src/backend_ast/builtin.c src/backend_ast/builtin_network_api.c
    src/symbol/symbol.c
    src/clike/stubs.c
)

# Include user-supplied builtins in clike as well
list(APPEND CLIKE_SOURCES ${PSCAL_EXT_BUILTIN_SOURCES})
list(APPEND CLIKE_SOURCES src/third_party/yyjson/yyjson.c)

# When SDL support is enabled, clike needs the same SDL backend sources
# as the main executables to provide implementations for graphics and
# audio builtins referenced from builtin.c.
if(SDL)
    list(APPEND CLIKE_SOURCES
        src/backend_ast/sdl.c
        src/backend_ast/sdl3d.c
        src/backend_ast/gl.c
        src/backend_ast/audio.c
    )
endif()

add_executable(clike ${CLIKE_SOURCES})
target_compile_definitions(clike PRIVATE FRONTEND_CLIKE)
if(TARGET CURL::libcurl)
    target_link_libraries(clike PRIVATE CURL::libcurl m Threads::Threads)
else()
    target_link_libraries(clike PRIVATE ${CURL_LIBRARIES} m Threads::Threads)
endif()
if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
    target_link_libraries(clike PRIVATE ${PSCAL_SQLITE_LIBS})
endif()

set(CLIKE_REPL_SOURCES ${CLIKE_SOURCES})
list(REMOVE_ITEM CLIKE_REPL_SOURCES src/clike/main.c)
list(APPEND CLIKE_REPL_SOURCES src/clike/repl.c)
add_executable(clike-repl ${CLIKE_REPL_SOURCES})
if(TARGET CURL::libcurl)
    target_link_libraries(clike-repl PRIVATE CURL::libcurl m Threads::Threads)
else()
    target_link_libraries(clike-repl PRIVATE ${CURL_LIBRARIES} m Threads::Threads)
endif()
if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
    target_link_libraries(clike-repl PRIVATE ${PSCAL_SQLITE_LIBS})
endif()

# When SDL support is enabled, clike needs the same SDL include paths,
# library directories, and libraries as the other executables to compile
# headers that reference SDL types.
if(SDL)
    target_include_directories(clike PRIVATE
        ${SDL2_INCLUDE_DIRS}
        ${PC_SDL2_IMAGE_INCLUDE_DIRS}
        ${PC_SDL2_MIXER_INCLUDE_DIRS}
        ${PC_SDL2_TTF_INCLUDE_DIRS}
    )

    target_link_directories(clike PRIVATE
        ${PC_SDL2_IMAGE_LIBRARY_DIRS}
        ${PC_SDL2_MIXER_LIBRARY_DIRS}
        ${PC_SDL2_TTF_LIBRARY_DIRS}
    )

    set(CLIKE_SDL_LIBS "")
    if(TARGET SDL2::SDL2)
        list(APPEND CLIKE_SDL_LIBS SDL2::SDL2)
    else()
        list(APPEND CLIKE_SDL_LIBS ${SDL2_LIBRARIES})
    endif()
    list(APPEND CLIKE_SDL_LIBS
        ${PC_SDL2_IMAGE_LIBRARIES}
        ${PC_SDL2_MIXER_LIBRARIES}
        ${PC_SDL2_TTF_LIBRARIES}
        ${PC_SDL2_IMAGE_LDFLAGS_OTHER}
        ${PC_SDL2_MIXER_LDFLAGS_OTHER}
        ${PC_SDL2_TTF_LDFLAGS_OTHER}
    )
    if(PSCAL_OPENGL_TARGET)
        list(APPEND CLIKE_SDL_LIBS ${PSCAL_OPENGL_TARGET})
    elseif(OPENGL_LIBRARIES)
        list(APPEND CLIKE_SDL_LIBS ${OPENGL_LIBRARIES})
    endif()
    list(APPEND CLIKE_SDL_LIBS ${PSCAL_SDL_PLATFORM_LIBS})
    list(REMOVE_DUPLICATES CLIKE_SDL_LIBS)
    target_link_libraries(clike PRIVATE ${CLIKE_SDL_LIBS})
    target_compile_definitions(clike PRIVATE SDL)

    target_include_directories(clike-repl PRIVATE
        ${SDL2_INCLUDE_DIRS}
        ${PC_SDL2_IMAGE_INCLUDE_DIRS}
        ${PC_SDL2_MIXER_INCLUDE_DIRS}
        ${PC_SDL2_TTF_INCLUDE_DIRS}
    )

    target_link_directories(clike-repl PRIVATE
        ${PC_SDL2_IMAGE_LIBRARY_DIRS}
        ${PC_SDL2_MIXER_LIBRARY_DIRS}
        ${PC_SDL2_TTF_LIBRARY_DIRS}
    )

    target_link_libraries(clike-repl PRIVATE ${CLIKE_SDL_LIBS})
    target_compile_definitions(clike-repl PRIVATE SDL)

endif()

# Rea front-end executable (placeholder)
set(REA_SOURCES
    src/rea/main.c
    src/rea/lexer.c
    src/rea/parser.c
    src/rea/semantic.c
    src/Pascal/lexer.c
    src/Pascal/parser.c
    src/ast/ast.c
    src/Pascal/globals.c
    src/core/utils.c src/core/types.c src/core/list.c src/core/preproc.c src/core/version.c src/core/cache.c
    src/compiler/bytecode.c
    src/compiler/compiler.c
    src/vm/vm.c
    src/backend_ast/builtin.c src/backend_ast/builtin_network_api.c
    src/symbol/symbol.c
)

list(APPEND REA_SOURCES ${PSCAL_EXT_BUILTIN_SOURCES})
list(APPEND REA_SOURCES src/third_party/yyjson/yyjson.c)

if(SDL)
    list(APPEND REA_SOURCES
        src/backend_ast/sdl.c
        src/backend_ast/sdl3d.c
        src/backend_ast/gl.c
        src/backend_ast/audio.c
    )
endif()

add_executable(rea ${REA_SOURCES})
target_compile_definitions(rea PRIVATE FRONTEND_REA)
if(TARGET CURL::libcurl)
    target_link_libraries(rea PRIVATE CURL::libcurl m Threads::Threads)
else()
    target_link_libraries(rea PRIVATE ${CURL_LIBRARIES} m Threads::Threads)
endif()
if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
    target_link_libraries(rea PRIVATE ${PSCAL_SQLITE_LIBS})
endif()

if(SDL)
    target_include_directories(rea PRIVATE
        ${SDL2_INCLUDE_DIRS}
        ${PC_SDL2_IMAGE_INCLUDE_DIRS}
        ${PC_SDL2_MIXER_INCLUDE_DIRS}
        ${PC_SDL2_TTF_INCLUDE_DIRS}
    )

    target_link_directories(rea PRIVATE
        ${PC_SDL2_IMAGE_LIBRARY_DIRS}
        ${PC_SDL2_MIXER_LIBRARY_DIRS}
        ${PC_SDL2_TTF_LIBRARY_DIRS}
    )

    set(REA_SDL_LIBS "")
    if(TARGET SDL2::SDL2)
        list(APPEND REA_SDL_LIBS SDL2::SDL2)
    else()
        list(APPEND REA_SDL_LIBS ${SDL2_LIBRARIES})
    endif()
    list(APPEND REA_SDL_LIBS
        ${PC_SDL2_IMAGE_LIBRARIES}
        ${PC_SDL2_MIXER_LIBRARIES}
        ${PC_SDL2_TTF_LIBRARIES}
        ${PC_SDL2_IMAGE_LDFLAGS_OTHER}
        ${PC_SDL2_MIXER_LDFLAGS_OTHER}
        ${PC_SDL2_TTF_LDFLAGS_OTHER}
    )
    if(PSCAL_OPENGL_TARGET)
        list(APPEND REA_SDL_LIBS ${PSCAL_OPENGL_TARGET})
    elseif(OPENGL_LIBRARIES)
        list(APPEND REA_SDL_LIBS ${OPENGL_LIBRARIES})
    endif()
    list(APPEND REA_SDL_LIBS ${PSCAL_SDL_PLATFORM_LIBS})
    list(REMOVE_DUPLICATES REA_SDL_LIBS)
    target_link_libraries(rea PRIVATE ${REA_SDL_LIBS})
    target_compile_definitions(rea PRIVATE SDL)
endif()

# ---- Examples ----
add_subdirectory(Examples)

# ---- optional install ----
include(GNUInstallDirs)
install(TARGETS pascal pscalvm
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(BUILD_DASCAL)
    install(TARGETS dascal
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
if(BUILD_PSCALD)
    install(TARGETS pscald
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

add_test(NAME pascal_tests COMMAND bash ${CMAKE_SOURCE_DIR}/Tests/run_pascal_tests.sh)
add_test(NAME clike_tests COMMAND bash ${CMAKE_SOURCE_DIR}/Tests/run_clike_tests.sh)
add_test(NAME rea_tests COMMAND bash ${CMAKE_SOURCE_DIR}/Tests/run_rea_tests.sh)
set_tests_properties(rea_tests PROPERTIES ENVIRONMENT "REA_SKIP_TESTS=constructor_init field_access_assign field_access_read method_this_assign new_alloc new_assign_ptr")
add_test(NAME json2bc_tests COMMAND bash ${CMAKE_SOURCE_DIR}/Tests/run_json2bc_tests.sh)

# ---- AST JSON to Bytecode tool ----
set(JSON2BC_SOURCES
    src/tools/json2bc.c
    src/tools/ast_json_loader.c
    src/rea/type_stubs.c
    src/ast/ast.c
    src/core/utils.c src/core/types.c src/core/list.c src/core/version.c src/core/cache.c
    src/compiler/bytecode.c src/compiler/compiler.c
    src/backend_ast/builtin.c src/backend_ast/builtin_network_api.c
    src/backend_ast/sdl.c src/backend_ast/sdl3d.c src/backend_ast/gl.c src/backend_ast/audio.c
    src/symbol/symbol.c
    src/vm/vm.c
    src/Pascal/globals.c
)
list(APPEND JSON2BC_SOURCES ${PSCAL_EXT_BUILTIN_SOURCES})
list(APPEND JSON2BC_SOURCES src/third_party/yyjson/yyjson.c)
add_executable(pscaljson2bc ${JSON2BC_SOURCES})
if(TARGET CURL::libcurl)
    target_link_libraries(pscaljson2bc PRIVATE CURL::libcurl m Threads::Threads)
else()
    target_link_libraries(pscaljson2bc PRIVATE ${CURL_LIBRARIES} m Threads::Threads)
endif()
if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
    target_link_libraries(pscaljson2bc PRIVATE ${PSCAL_SQLITE_LIBS})
endif()
if(SDL)
    target_include_directories(pscaljson2bc PRIVATE
        ${SDL2_INCLUDE_DIRS}
        ${PC_SDL2_IMAGE_INCLUDE_DIRS}
        ${PC_SDL2_MIXER_INCLUDE_DIRS}
        ${PC_SDL2_TTF_INCLUDE_DIRS}
    )
    target_link_directories(pscaljson2bc PRIVATE
        ${PC_SDL2_IMAGE_LIBRARY_DIRS}
        ${PC_SDL2_MIXER_LIBRARY_DIRS}
        ${PC_SDL2_TTF_LIBRARY_DIRS}
    )
    set(JSON2BC_SDL_LIBS "")
    if(TARGET SDL2::SDL2)
        list(APPEND JSON2BC_SDL_LIBS SDL2::SDL2)
    else()
        list(APPEND JSON2BC_SDL_LIBS ${SDL2_LIBRARIES})
    endif()
    list(APPEND JSON2BC_SDL_LIBS
        ${PC_SDL2_IMAGE_LIBRARIES}
        ${PC_SDL2_MIXER_LIBRARIES}
        ${PC_SDL2_TTF_LIBRARIES}
        ${PC_SDL2_IMAGE_LDFLAGS_OTHER}
        ${PC_SDL2_MIXER_LDFLAGS_OTHER}
        ${PC_SDL2_TTF_LDFLAGS_OTHER}
    )
    if(PSCAL_OPENGL_TARGET)
        list(APPEND JSON2BC_SDL_LIBS ${PSCAL_OPENGL_TARGET})
    elseif(OPENGL_LIBRARIES)
        list(APPEND JSON2BC_SDL_LIBS ${OPENGL_LIBRARIES})
    endif()
    list(APPEND JSON2BC_SDL_LIBS ${PSCAL_SDL_PLATFORM_LIBS})
    list(REMOVE_DUPLICATES JSON2BC_SDL_LIBS)
    target_link_libraries(pscaljson2bc PRIVATE ${JSON2BC_SDL_LIBS})
    target_compile_definitions(pscaljson2bc PRIVATE SDL)
endif()

# Install the JSON→Bytecode tool alongside other binaries
install(TARGETS pscaljson2bc RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# ---- Optional shell completions ----
option(PSCAL_INSTALL_COMPLETIONS "Install shell completion scripts for pscaljson2bc" ON)
if(PSCAL_INSTALL_COMPLETIONS)
    # Bash completion (standard location). Rename to the program name without extension
    install(FILES tools/completions/pscaljson2bc.bash
            DESTINATION "${CMAKE_INSTALL_DATADIR}/bash-completion/completions"
            RENAME pscaljson2bc)
    # Zsh completion (site-functions). Ensure the filename begins with an underscore
    install(FILES tools/completions/_pscaljson2bc
            DESTINATION "${CMAKE_INSTALL_DATADIR}/zsh/site-functions")
endif()
