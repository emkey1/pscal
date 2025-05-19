# CMakeLists.txt – top of your repo
cmake_minimum_required(VERSION 3.24)
project(pscal C)

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

# ---- Include Directories (Project Structure) ----
include_directories(
    src
    src/core
    src/frontend
    src/symbol
    src/backend_ast
    # src/compiler
    # src/vm
)

# ---- Dependencies ----
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

# --- CURL ---
find_package(CURL REQUIRED)
message(STATUS "CURL Include Dirs: ${CURL_INCLUDE_DIRS}")
message(STATUS "CURL Libraries Variable: ${CURL_LIBRARIES}")


# ---- Source Files ----
set(PSCAL_SOURCES
    src/main.c src/globals.c
    src/core/types.c src/core/utils.c src/core/list.c
    src/frontend/lexer.c src/frontend/parser.c src/frontend/ast.c
    src/symbol/symbol.c
    src/backend_ast/interpreter.c src/backend_ast/builtin.c
    src/backend_ast/builtin_network_api.c src/backend_ast/sdl.c src/backend_ast/audio.c

    # --- Add new files for bytecode compiler and VM here ---
    src/compiler/bytecode.c src/compiler/compiler.c   

    # src/vm/vm.c             # Add when you create it
)

# ---- Executable Targets Function ----
function(add_pscal_executable target_name)
    add_executable(${target_name} ${PSCAL_SOURCES})

    target_include_directories(${target_name} PRIVATE
        ${SDL2_INCLUDE_DIRS}
        ${PC_SDL2_IMAGE_INCLUDE_DIRS}
        ${PC_SDL2_MIXER_INCLUDE_DIRS}
        ${PC_SDL2_TTF_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
    )

    # Add library search paths from pkg-config
    target_link_directories(${target_name} PRIVATE
        ${PC_SDL2_IMAGE_LIBRARY_DIRS}
        ${PC_SDL2_MIXER_LIBRARY_DIRS}
        ${PC_SDL2_TTF_LIBRARY_DIRS}
    )

    # Construct the list of libraries and flags to link
    set(TARGET_LINK_LIBS "")

    # Core SDL2
    if(TARGET SDL2::SDL2)
        list(APPEND TARGET_LINK_LIBS SDL2::SDL2)
    else()
        list(APPEND TARGET_LINK_LIBS ${SDL2_LIBRARIES})
    endif()

    # SDL Extensions - using the library names from pkg-config
    # These variables (e.g. PC_SDL2_IMAGE_LIBRARIES) often contain a list like "SDL2_image;SDL2"
    # CMake handles linking these by name (e.g. -lSDL2_image -lSDL2)
    # The target_link_directories above tells the linker where to find them.
    list(APPEND TARGET_LINK_LIBS ${PC_SDL2_IMAGE_LIBRARIES})
    list(APPEND TARGET_LINK_LIBS ${PC_SDL2_MIXER_LIBRARIES})
    list(APPEND TARGET_LINK_LIBS ${PC_SDL2_TTF_LIBRARIES})

    # Add other linker flags from pkg-config (these might include rpath, etc.)
    list(APPEND TARGET_LINK_LIBS ${PC_SDL2_IMAGE_LDFLAGS_OTHER})
    list(APPEND TARGET_LINK_LIBS ${PC_SDL2_MIXER_LDFLAGS_OTHER})
    list(APPEND TARGET_LINK_LIBS ${PC_SDL2_TTF_LDFLAGS_OTHER})

    # CURL
    if(TARGET CURL::libcurl)
        list(APPEND TARGET_LINK_LIBS CURL::libcurl)
    else()
        list(APPEND TARGET_LINK_LIBS ${CURL_LIBRARIES})
    endif()
    
    list(APPEND TARGET_LINK_LIBS m) # Math library

    # Specific options for dscal (ASan)
    if(${target_name} STREQUAL "dscal")
        list(APPEND TARGET_LINK_LIBS "-fsanitize=address")
    endif()
    
    # Optional: Remove duplicate library names from the list before linking
    # This helps with the ld warning but might not be strictly necessary if the linker handles it.
    if(TARGET_LINK_LIBS)
        list(REMOVE_DUPLICATES TARGET_LINK_LIBS)
    endif()
    
    message(STATUS "Linking ${target_name} with: ${TARGET_LINK_LIBS}")
    target_link_libraries(${target_name} PRIVATE ${TARGET_LINK_LIBS})

    # Specific compile definitions and options
    if(${target_name} STREQUAL "pscal")
        target_compile_options(${target_name} PRIVATE -O3)
        if(RELEASE_BUILD)
            target_compile_definitions(${target_name} PRIVATE RELEASE)
        else()
            target_compile_definitions(${target_name} PRIVATE DEBUGNOT)
        endif()
    elseif(${target_name} STREQUAL "dscal")
        target_compile_definitions(${target_name} PRIVATE DEBUG)
        target_compile_options(${target_name} PRIVATE -fsanitize=address -g)
        # target_link_options(${target_name} PRIVATE INTERFACE -fsanitize=address) # Covered by adding to TARGET_LINK_LIBS
    elseif(${target_name} STREQUAL "hscal")
        target_compile_definitions(${target_name} PRIVATE DEBUG SUPPRESS_EXIT)
        target_compile_options(${target_name} PRIVATE -g)
    endif()
endfunction()

add_pscal_executable(pscal)
add_pscal_executable(dscal)
# add_pscal_executable(hscal) // Lets not build this normally

# ---- optional install ----
include(GNUInstallDirs)
install(TARGETS pscal dscal
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
