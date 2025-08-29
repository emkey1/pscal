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
# ---- End Version Generation Logic ----

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_compile_definitions(PROGRAM_VERSION="${PROGRAM_VERSION_STRING}")

# Optionally enable SDL-based features
option(SDL "Enable SDL-based graphics and audio" OFF)
if(SDL)
    add_compile_definitions(SDL)
endif()

# ---- Extended builtin categories ----
option(ENABLE_EXT_BUILTIN_MATH "Enable extended math builtins" ON)
option(ENABLE_EXT_BUILTIN_STRINGS "Enable extended string builtins" ON)
option(ENABLE_EXT_BUILTIN_SYSTEM "Enable extended system builtins" ON)

if(ENABLE_EXT_BUILTIN_MATH)
    add_compile_definitions(ENABLE_EXT_BUILTIN_MATH)
endif()
if(ENABLE_EXT_BUILTIN_STRINGS)
    add_compile_definitions(ENABLE_EXT_BUILTIN_STRINGS)
endif()
if(ENABLE_EXT_BUILTIN_SYSTEM)
    add_compile_definitions(ENABLE_EXT_BUILTIN_SYSTEM)
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
)

# ---- Dependencies ----
if(SDL)
    find_package(PkgConfig REQUIRED)

    # --- SDL2 Core ---
    find_package(SDL2 REQUIRED)
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
    src/main.c src/globals.c
    src/core/types.c src/core/utils.c src/core/list.c
    src/Pascal/lexer.c src/Pascal/parser.c src/Pascal/ast.c src/Pascal/opt.c
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
        src/backend_ast/audio.c
    )
endif()

# ---- Extended builtins ----
# Allow selectively enabling categories of extended builtins.
add_subdirectory(src/ext_builtins)
list(APPEND PSCAL_SOURCES ${PSCAL_EXT_BUILTIN_SOURCES})


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
    endif()

    # CURL
    if(TARGET CURL::libcurl)
        list(APPEND TARGET_LINK_LIBS CURL::libcurl)
    else()
        list(APPEND TARGET_LINK_LIBS ${CURL_LIBRARIES})
    endif()

    list(APPEND TARGET_LINK_LIBS m) # Math library
    list(APPEND TARGET_LINK_LIBS Threads::Threads)

    # Specific options for dascal (ASan)
    if(${target_name} STREQUAL "dascal")
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
        target_compile_options(${target_name} PRIVATE -fsanitize=address -g)
    elseif(${target_name} STREQUAL "hscal")
        target_compile_definitions(${target_name} PRIVATE DEBUG SUPPRESS_EXIT)
        target_compile_options(${target_name} PRIVATE -g)
    endif()
endfunction()

add_pscal_executable(pascal)
add_pscal_executable(dascal)
# add_pscal_executable(hscal) // Lets not build this normally

# Standalone VM binary
set(PSCAL_VM_SOURCES ${PSCAL_SOURCES})
list(REMOVE_ITEM PSCAL_VM_SOURCES src/main.c)
list(APPEND PSCAL_VM_SOURCES src/vm/vm_main.c)
set(PSCAL_SOURCES_BACKUP ${PSCAL_SOURCES})
set(PSCAL_SOURCES ${PSCAL_VM_SOURCES})
add_pscal_executable(pscalvm)
set(PSCAL_SOURCES ${PSCAL_SOURCES_BACKUP})

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
    src/globals.c
    src/core/utils.c src/core/types.c src/core/list.c src/core/cache.c
    src/compiler/bytecode.c
    src/vm/vm.c
    src/backend_ast/builtin.c src/backend_ast/builtin_network_api.c
    src/symbol/symbol.c
    src/clike/stubs.c
)

# Include user-supplied builtins in clike as well
list(APPEND CLIKE_SOURCES ${PSCAL_EXT_BUILTIN_SOURCES})

# When SDL support is enabled, clike needs the same SDL backend sources
# as the main executables to provide implementations for graphics and
# audio builtins referenced from builtin.c.
if(SDL)
    list(APPEND CLIKE_SOURCES
        src/backend_ast/sdl.c
        src/backend_ast/audio.c
    )
endif()

add_executable(clike ${CLIKE_SOURCES})
if(TARGET CURL::libcurl)
    target_link_libraries(clike PRIVATE CURL::libcurl m Threads::Threads)
else()
    target_link_libraries(clike PRIVATE ${CURL_LIBRARIES} m Threads::Threads)
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

# ---- Examples ----
add_subdirectory(Examples)

# ---- optional install ----
include(GNUInstallDirs)
install(TARGETS pascal dascal pscalvm
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

add_test(NAME pascal_tests COMMAND bash ${CMAKE_SOURCE_DIR}/Tests/run_pascal_tests.sh)
add_test(NAME clike_tests COMMAND bash ${CMAKE_SOURCE_DIR}/Tests/run_clike_tests.sh)
