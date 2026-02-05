# CMakeLists.txt – top of your repo
cmake_minimum_required(VERSION 3.18)
project(pscal C)
enable_testing()

option(PSCAL_FORCE_IOS "Force iOS mode (PSCAL_TARGET_IOS + vproc shims) on non-iOS hosts." OFF)
option(VPROC_ENABLE_STUBS_FOR_TESTS "Build vproc with test stubs (host-only helpers)." OFF)

set(PSCAL_REAL_IOS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PSCAL_REAL_IOS ON)
elseif(APPLE AND CMAKE_OSX_SYSROOT)
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" _pscal_sysroot_lower)
    if(_pscal_sysroot_lower MATCHES "iphoneos" OR _pscal_sysroot_lower MATCHES "iphonesimulator")
        set(PSCAL_REAL_IOS ON)
    endif()
endif()

set(PSCAL_PLATFORM_IOS ${PSCAL_REAL_IOS})
if(PSCAL_FORCE_IOS)
    set(PSCAL_PLATFORM_IOS ON)
endif()

set(PSCAL_MACCATALYST OFF)
if(APPLE AND CMAKE_OSX_SYSROOT)
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" _pscal_sysroot_lower)
    if(_pscal_sysroot_lower MATCHES "macosx")
        if(CMAKE_C_FLAGS MATCHES "macabi" OR CMAKE_CXX_FLAGS MATCHES "macabi")
            set(PSCAL_MACCATALYST ON)
        endif()
    endif()
endif()
# Fall back to directory-name heuristic when toolchains don’t inject macabi flags.
if(NOT PSCAL_MACCATALYST AND APPLE)
    string(TOLOWER "${CMAKE_BINARY_DIR}" _pscal_binary_dir_lower)
    if(_pscal_binary_dir_lower MATCHES "maccatalyst")
        set(PSCAL_MACCATALYST ON)
    endif()
endif()
if(PSCAL_MACCATALYST)
    # Mac Catalyst runs under iOS-style sandboxing; enable iOS shims and defs.
    set(PSCAL_PLATFORM_IOS ON)
endif()

if(NOT DEFINED PSCAL_USE_HOST_OPENSSL)
    set(PSCAL_USE_HOST_OPENSSL OFF)
    if(PSCAL_FORCE_IOS AND NOT PSCAL_REAL_IOS AND NOT PSCAL_MACCATALYST)
        set(PSCAL_USE_HOST_OPENSSL ON)
    endif()
    if(PSCAL_MACCATALYST)
        set(PSCAL_USE_HOST_OPENSSL ON)
    endif()
endif()

if(NOT PSCAL_PLATFORM_IOS OR PSCAL_USE_HOST_OPENSSL)
    find_package(OpenSSL REQUIRED)
    # CMake's FindOpenSSL should provide imported targets, but some environments
    # (notably newer Homebrew/OpenSSL combos) occasionally omit them. Fall back
    # to raw library paths when the targets are missing so host builds keep working.
    if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        set(PSCAL_OPENSSL_SSL_TARGET OpenSSL::SSL)
        set(PSCAL_OPENSSL_CRYPTO_TARGET OpenSSL::Crypto)
    else()
        set(PSCAL_OPENSSL_SSL_TARGET ${OPENSSL_SSL_LIBRARY})
        set(PSCAL_OPENSSL_CRYPTO_TARGET ${OPENSSL_CRYPTO_LIBRARY})
    endif()
endif()
if(PSCAL_PLATFORM_IOS)
    add_compile_definitions(PSCAL_TARGET_IOS __progname=pscal_progname __progname_full=pscal_progname_full)
    if(VPROC_ENABLE_STUBS_FOR_TESTS)
        add_compile_definitions(VPROC_ENABLE_STUBS_FOR_TESTS)
    endif()
    add_compile_options(-include ${CMAKE_SOURCE_DIR}/src/ios/vproc_shim.h)
    # vproc implementation compiles without the global shim overrides to avoid macro pollution.
    set_property(SOURCE src/ios/vproc.c APPEND PROPERTY COMPILE_DEFINITIONS VPROC_SHIM_DISABLED=1;VPROC_SHIM_HARD_DISABLED=1)
    # path virtualization helpers must call the real syscalls to avoid recursion with vproc shim macros.
    set_property(SOURCE src/common/path_virtualization.c APPEND PROPERTY COMPILE_DEFINITIONS VPROC_SHIM_DISABLED=1;VPROC_SHIM_HARD_DISABLED=1)
    # path truncation helpers should bypass vproc shims to avoid recursive syscalls.
    set_property(SOURCE src/common/path_truncate.c APPEND PROPERTY COMPILE_DEFINITIONS VPROC_SHIM_DISABLED=1;VPROC_SHIM_HARD_DISABLED=1)
    # low-level tty/pty helpers must bypass vproc shim macros to avoid recursive interpose calls.
    set_property(SOURCE src/ios/tty/pscal_fd.c APPEND PROPERTY COMPILE_DEFINITIONS VPROC_SHIM_DISABLED=1;VPROC_SHIM_HARD_DISABLED=1)
    set_property(SOURCE src/ios/tty/pscal_pty.c APPEND PROPERTY COMPILE_DEFINITIONS VPROC_SHIM_DISABLED=1;VPROC_SHIM_HARD_DISABLED=1)
elseif(VPROC_ENABLE_STUBS_FOR_TESTS)
    add_compile_definitions(VPROC_ENABLE_STUBS_FOR_TESTS)
endif()

if(PSCAL_PLATFORM_IOS AND NOT TARGET pscal_common_frontend_shim)
    add_library(pscal_common_frontend_shim STATIC src/ios/common_frontend_shim.c)
endif()

# ---- Version Generation Logic ----
string(TIMESTAMP CURRENT_YEAR "%Y")
string(TIMESTAMP CURRENT_MONTH "%m")
string(TIMESTAMP CURRENT_DAY "%d")
string(TIMESTAMP CURRENT_HOUR "%H")
string(TIMESTAMP CURRENT_MINUTE "%M")
set(BASE_VERSION "${CURRENT_YEAR}${CURRENT_MONTH}${CURRENT_DAY}.${CURRENT_HOUR}${CURRENT_MINUTE}")
option(RELEASE_BUILD "Build as a release version (appends _REL)" OFF)
option(release "Enable the PSCAL release profile (all builtins, sans dascal)" OFF)
if(release)
    message(STATUS "Configuring PSCAL release profile: enabling all builtins and excluding dascal")
    set(RELEASE_BUILD ON CACHE BOOL "Build as a release version (appends _REL)" FORCE)
endif()
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

# Derive the default install root from the selected CMake install prefix so a
# customised `-DCMAKE_INSTALL_PREFIX` keeps runtime assets colocated with the
# binaries unless users explicitly override `-DPSCAL_INSTALL_ROOT`.
set(_pscal_install_prefix "${CMAKE_INSTALL_PREFIX}")
if(NOT _pscal_install_prefix)
    set(_pscal_install_prefix "/usr/local")
endif()
string(REGEX REPLACE "/$" "" _pscal_install_prefix "${_pscal_install_prefix}")
if(_pscal_install_prefix MATCHES "/pscal$")
    set(_pscal_install_root_default "${_pscal_install_prefix}")
else()
    set(_pscal_install_root_default "${_pscal_install_prefix}/pscal")
endif()
set(PSCAL_INSTALL_ROOT "${_pscal_install_root_default}" CACHE PATH "Default PSCAL runtime asset directory")
string(REGEX REPLACE "/$" "" PSCAL_INSTALL_ROOT "${PSCAL_INSTALL_ROOT}")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
configure_file(src/core/pscal_paths.h.in "${CMAKE_BINARY_DIR}/generated/pscal_paths.h" @ONLY)
include_directories("${CMAKE_BINARY_DIR}/generated")

if(PSCAL_PLATFORM_IOS AND NOT PSCAL_USE_HOST_OPENSSL)
    set(PSCAL_OPENSSL_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third-party/openssl-3.6.0")
    if(NOT EXISTS "${PSCAL_OPENSSL_SOURCE_DIR}/Configure")
        message(FATAL_ERROR "third-party/openssl-3.6.0 not found; please add the OpenSSL sources")
    endif()
    find_program(PSCAL_PERL_EXECUTABLE perl REQUIRED)
    find_program(PSCAL_MAKE_EXECUTABLE make REQUIRED)

    if(CMAKE_OSX_ARCHITECTURES)
        string(REPLACE ";" " " _pscal_openssl_arch "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(_pscal_openssl_arch "arm64")
    endif()
    if(PSCAL_MACCATALYST)
        set(_pscal_openssl_platform "Mac Catalyst")
        if(_pscal_openssl_arch MATCHES "x86_64")
            set(_pscal_openssl_target "darwin64-x86_64-cc")
        else()
            set(_pscal_openssl_target "darwin64-arm64-cc")
        endif()
        set(_pscal_openssl_version_flag "")
        set(_pscal_openssl_bitcode_flag "")
    else()
        set(_pscal_openssl_platform "iOS")
        if(CMAKE_OSX_SYSROOT MATCHES "Simulator")
            set(_pscal_openssl_target "iossimulator-xcrun")
        else()
            set(_pscal_openssl_target "ios64-cross")
        endif()
        get_filename_component(_pscal_sdk_dir "${CMAKE_OSX_SYSROOT}" DIRECTORY)
        get_filename_component(_pscal_cross_top "${_pscal_sdk_dir}" DIRECTORY)
        get_filename_component(_pscal_cross_sdk "${CMAKE_OSX_SYSROOT}" NAME)
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            set(_pscal_openssl_min_version "${CMAKE_OSX_DEPLOYMENT_TARGET}")
        else()
            set(_pscal_openssl_min_version "15.0")
        endif()
        if(PSCALI_IOS_PLATFORM STREQUAL "SIMULATOR")
            set(_pscal_openssl_version_flag "-mios-simulator-version-min=${_pscal_openssl_min_version}")
        else()
            set(_pscal_openssl_version_flag "-miphoneos-version-min=${_pscal_openssl_min_version}")
        endif()
        set(_pscal_openssl_bitcode_flag "-fembed-bitcode")
    endif()

    set(PSCAL_OPENSSL_BUILD_DIR "${CMAKE_BINARY_DIR}/third-party/openssl/build")
    set(PSCAL_OPENSSL_INSTALL_DIR "${CMAKE_BINARY_DIR}/third-party/openssl/install")
    set(_pscal_openssl_stamp_file "${PSCAL_OPENSSL_INSTALL_DIR}/.pscal_openssl_target")
    set(_pscal_openssl_stamp_value "${_pscal_openssl_platform};${_pscal_openssl_target};${_pscal_openssl_version_flag};${_pscal_openssl_arch}")
    if(PSCAL_MACCATALYST)
        string(APPEND _pscal_openssl_stamp_value ";${CMAKE_C_FLAGS}")
    endif()
    if(EXISTS "${_pscal_openssl_stamp_file}")
        file(READ "${_pscal_openssl_stamp_file}" _pscal_openssl_stamp_content)
        string(STRIP "${_pscal_openssl_stamp_content}" _pscal_openssl_stamp_content)
        if(NOT _pscal_openssl_stamp_content STREQUAL _pscal_openssl_stamp_value)
            message(STATUS "OpenSSL deployment target changed; resetting cached build/install directories")
            file(REMOVE_RECURSE "${PSCAL_OPENSSL_BUILD_DIR}" "${PSCAL_OPENSSL_INSTALL_DIR}")
        endif()
    endif()
    file(MAKE_DIRECTORY "${PSCAL_OPENSSL_BUILD_DIR}")
    file(MAKE_DIRECTORY("${PSCAL_OPENSSL_INSTALL_DIR}"))
    file(MAKE_DIRECTORY("${PSCAL_OPENSSL_INSTALL_DIR}/include"))
    file(WRITE "${_pscal_openssl_stamp_file}" "${_pscal_openssl_stamp_value}\n")

    set(_pscal_openssl_configure
        ${PSCAL_PERL_EXECUTABLE} "${PSCAL_OPENSSL_SOURCE_DIR}/Configure"
        ${_pscal_openssl_target} no-shared no-dso no-tests
        --prefix=${PSCAL_OPENSSL_INSTALL_DIR} --openssldir=${PSCAL_OPENSSL_INSTALL_DIR}/ssl
    )

    set(_pscal_openssl_sysroot "${CMAKE_OSX_SYSROOT}")
    if(NOT IS_ABSOLUTE "${_pscal_openssl_sysroot}")
        execute_process(
            COMMAND xcrun --sdk ${CMAKE_OSX_SYSROOT} --show-sdk-path
            OUTPUT_VARIABLE _pscal_openssl_sysroot_resolved
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _pscal_openssl_sysroot_status
        )
        if(_pscal_openssl_sysroot_status EQUAL 0 AND _pscal_openssl_sysroot_resolved)
            set(_pscal_openssl_sysroot "${_pscal_openssl_sysroot_resolved}")
        endif()
    endif()

    set(_pscal_openssl_extra_cflags "")
    if(NOT _pscal_openssl_version_flag STREQUAL "")
        list(APPEND _pscal_openssl_extra_cflags "${_pscal_openssl_version_flag}")
    endif()
    if(NOT _pscal_openssl_bitcode_flag STREQUAL "")
        list(APPEND _pscal_openssl_extra_cflags "${_pscal_openssl_bitcode_flag}")
    endif()
    string(REPLACE ";" " " _pscal_openssl_extra_cflags "${_pscal_openssl_extra_cflags}")

    set(_pscal_openssl_env
        "CC=${CMAKE_C_COMPILER}"
        "CFLAGS=${CMAKE_C_FLAGS} -arch ${_pscal_openssl_arch} -isysroot ${_pscal_openssl_sysroot} ${_pscal_openssl_extra_cflags}"
        "CXXFLAGS=${CMAKE_CXX_FLAGS} -arch ${_pscal_openssl_arch} -isysroot ${_pscal_openssl_sysroot} ${_pscal_openssl_extra_cflags}"
        "LDFLAGS=${CMAKE_EXE_LINKER_FLAGS}"
    )
    if(DEFINED _pscal_cross_top AND NOT _pscal_cross_top STREQUAL "")
        list(APPEND _pscal_openssl_env "CROSS_TOP=${_pscal_cross_top}")
    endif()
    if(DEFINED _pscal_cross_sdk AND NOT _pscal_cross_sdk STREQUAL "")
        list(APPEND _pscal_openssl_env "CROSS_SDK=${_pscal_cross_sdk}")
    endif()

    add_custom_command(
        OUTPUT "${PSCAL_OPENSSL_INSTALL_DIR}/lib/libssl.a" "${PSCAL_OPENSSL_INSTALL_DIR}/lib/libcrypto.a"
        COMMAND ${CMAKE_COMMAND} -E env ${_pscal_openssl_env} ${_pscal_openssl_configure}
        COMMAND ${CMAKE_COMMAND} -E env ${_pscal_openssl_env} ${PSCAL_MAKE_EXECUTABLE} build_libs
        COMMAND ${CMAKE_COMMAND} -E env ${_pscal_openssl_env} ${PSCAL_MAKE_EXECUTABLE} install_dev
        WORKING_DIRECTORY "${PSCAL_OPENSSL_BUILD_DIR}"
        DEPENDS "${PSCAL_OPENSSL_SOURCE_DIR}/Configure"
        COMMENT "Building OpenSSL for ${_pscal_openssl_platform}"
        VERBATIM
    )

    add_custom_target(pscal_openssl ALL
        DEPENDS "${PSCAL_OPENSSL_INSTALL_DIR}/lib/libssl.a" "${PSCAL_OPENSSL_INSTALL_DIR}/lib/libcrypto.a")

    add_library(pscal_openssl_crypto STATIC IMPORTED)
    set_target_properties(pscal_openssl_crypto PROPERTIES
        IMPORTED_LOCATION "${PSCAL_OPENSSL_INSTALL_DIR}/lib/libcrypto.a"
        INTERFACE_INCLUDE_DIRECTORIES "${PSCAL_OPENSSL_INSTALL_DIR}/include")
    add_dependencies(pscal_openssl_crypto pscal_openssl)

    add_library(pscal_openssl_ssl STATIC IMPORTED)
    set_target_properties(pscal_openssl_ssl PROPERTIES
        IMPORTED_LOCATION "${PSCAL_OPENSSL_INSTALL_DIR}/lib/libssl.a"
        INTERFACE_INCLUDE_DIRECTORIES "${PSCAL_OPENSSL_INSTALL_DIR}/include"
        INTERFACE_LINK_LIBRARIES pscal_openssl_crypto)
    add_dependencies(pscal_openssl_ssl pscal_openssl)
    set(PSCAL_OPENSSL_SSL_TARGET pscal_openssl_ssl)
    set(PSCAL_OPENSSL_CRYPTO_TARGET pscal_openssl_crypto)
    if(NOT TARGET OpenSSL::Crypto)
        add_library(OpenSSL::Crypto ALIAS pscal_openssl_crypto)
    endif()
    if(NOT TARGET OpenSSL::SSL)
        add_library(OpenSSL::SSL ALIAS pscal_openssl_ssl)
    endif()
endif()
file(WRITE "${CMAKE_BINARY_DIR}/pscal_install_root.txt" "${PSCAL_INSTALL_ROOT}\n")

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/openssh")
configure_file(third-party/openssh-10.2p1/config_pscal_ios.h
    "${CMAKE_BINARY_DIR}/generated/openssh/config.h" COPYONLY)

# Resolve the final runtime destination so install rules can write directly to
# either an absolute override or the default path under the chosen prefix.
set(PSCAL_INSTALL_ROOT_RESOLVED "${PSCAL_INSTALL_ROOT}")
if(NOT IS_ABSOLUTE "${PSCAL_INSTALL_ROOT_RESOLVED}")
    set(PSCAL_INSTALL_ROOT_RESOLVED "${CMAKE_INSTALL_PREFIX}/${PSCAL_INSTALL_ROOT_RESOLVED}")
endif()
string(REGEX REPLACE "/$" "" PSCAL_INSTALL_ROOT_RESOLVED "${PSCAL_INSTALL_ROOT_RESOLVED}")

set(_pscal_runtime_dest "${PSCAL_INSTALL_ROOT}")
if(IS_ABSOLUTE "${_pscal_runtime_dest}")
    file(RELATIVE_PATH _pscal_install_root_relative "${CMAKE_INSTALL_PREFIX}" "${PSCAL_INSTALL_ROOT_RESOLVED}")
    string(SUBSTRING "${_pscal_install_root_relative}" 0 2 _pscal_relative_prefix)
    if(_pscal_install_root_relative STREQUAL "")
        set(_pscal_runtime_dest ".")
    elseif(_pscal_relative_prefix STREQUAL "..")
        set(_pscal_runtime_dest "${PSCAL_INSTALL_ROOT_RESOLVED}")
    else()
        set(_pscal_runtime_dest "${_pscal_install_root_relative}")
    endif()
elseif(_pscal_runtime_dest STREQUAL "")
    set(_pscal_runtime_dest ".")
endif()

set(PSCAL_INSTALL_ROOT_DESTINATION "${_pscal_runtime_dest}")

# Optionally build additional binaries
option(BUILD_DASCAL "Build the dascal debugging binary" OFF)
# The dascal debugger is no longer shipped on any platform.
set(BUILD_DASCAL OFF CACHE BOOL "Build the dascal debugging binary" FORCE)
option(BUILD_PSCALD "Build the standalone bytecode decompiler" ON)
if(release)
    set(BUILD_PSCALD ON CACHE BOOL "Build the standalone bytecode decompiler" FORCE)
endif()

if(BUILD_DASCAL)
    add_compile_definitions(BUILD_DASCAL)
endif()
if(BUILD_PSCALD)
    add_compile_definitions(BUILD_PSCALD)
endif()

# Optionally emit static libraries (default when targeting iOS/Mac Catalyst).
set(_PSCAL_BUILD_STATIC_LIBS_DEFAULT OFF)
if(PSCAL_PLATFORM_IOS)
    set(_PSCAL_BUILD_STATIC_LIBS_DEFAULT ON)
endif()
option(PSCAL_BUILD_STATIC_LIBS "Build PSCAL static libraries for embedding runners" ${_PSCAL_BUILD_STATIC_LIBS_DEFAULT})

# Optionally enable SDL-based features
set(_PSCAL_SDL_DEFAULT ON)
if(PSCAL_PLATFORM_IOS)
    set(_PSCAL_SDL_DEFAULT OFF)
endif()
option(SDL "Enable SDL-based graphics and audio" ${_PSCAL_SDL_DEFAULT})
if(release)
    set(SDL ON CACHE BOOL "Enable SDL-based graphics and audio" FORCE)
endif()
option(PSCAL_USE_SDL3 "Prefer SDL3 over SDL2 when SDL support is enabled (falls back automatically)" OFF)
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
if(release)
    set(ENABLE_EXT_BUILTIN_MATH ON CACHE BOOL "Enable extended math builtins" FORCE)
    set(ENABLE_EXT_BUILTIN_STRINGS ON CACHE BOOL "Enable extended string builtins" FORCE)
    set(ENABLE_EXT_BUILTIN_SYSTEM ON CACHE BOOL "Enable extended system builtins" FORCE)
    set(ENABLE_EXT_BUILTIN_USER ON CACHE BOOL "Enable extended user builtins" FORCE)
    set(ENABLE_EXT_BUILTIN_YYJSON ON CACHE BOOL "Enable extended yyjson builtins" FORCE)
    set(ENABLE_EXT_BUILTIN_SQLITE ON CACHE BOOL "Enable extended sqlite builtins" FORCE)
    set(ENABLE_EXT_BUILTIN_3D ON CACHE BOOL "Enable extended 3D builtins" FORCE)
    set(ENABLE_EXT_BUILTIN_GRAPHICS ON CACHE BOOL "Enable extended graphics builtins" FORCE)
    set(ENABLE_EXT_BUILTIN_OPENAI ON CACHE BOOL "Enable extended OpenAI builtins" FORCE)
endif()

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
set(PSCAL_SQLITE_LIBS "")
set(PSCAL_SQLITE_INCLUDE_DIRS "")
if(ENABLE_EXT_BUILTIN_SQLITE)
    if(DEFINED SQLite3_INCLUDE_DIR AND NOT EXISTS "${SQLite3_INCLUDE_DIR}/sqlite3.h")
        unset(SQLite3_INCLUDE_DIR CACHE)
    endif()
    if(DEFINED SQLite3_LIBRARY AND NOT EXISTS "${SQLite3_LIBRARY}")
        unset(SQLite3_LIBRARY CACHE)
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(_PSCAL_SQLITE_SYSROOT "${CMAKE_OSX_SYSROOT}")
        if(_PSCAL_SQLITE_SYSROOT STREQUAL "iphoneos" OR _PSCAL_SQLITE_SYSROOT STREQUAL "iphonesimulator")
            execute_process(
                COMMAND xcrun --sdk ${_PSCAL_SQLITE_SYSROOT} --show-sdk-path
                OUTPUT_VARIABLE _PSCAL_SQLITE_SYSROOT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
        endif()
        if(_PSCAL_SQLITE_SYSROOT AND EXISTS "${_PSCAL_SQLITE_SYSROOT}/usr/include/sqlite3.h")
            set(PSCAL_SQLITE_INCLUDE_DIRS "${_PSCAL_SQLITE_SYSROOT}/usr/include")
        endif()
        if(_PSCAL_SQLITE_SYSROOT AND EXISTS "${_PSCAL_SQLITE_SYSROOT}/usr/lib/libsqlite3.tbd")
            set(PSCAL_SQLITE_LIBS "${_PSCAL_SQLITE_SYSROOT}/usr/lib/libsqlite3.tbd")
        endif()
        unset(_PSCAL_SQLITE_SYSROOT)
    endif()

    if(PSCAL_SQLITE_LIBS AND PSCAL_SQLITE_INCLUDE_DIRS)
        add_compile_definitions(ENABLE_EXT_BUILTIN_SQLITE)
    else()
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
            if(release)
                message(FATAL_ERROR "-Drelease requires SQLite3 development files so extended sqlite builtins can be enabled.")
            else()
                message(WARNING "SQLite3 development files not found; disabling sqlite extended builtins. Set -DENABLE_EXT_BUILTIN_SQLITE=OFF to silence this warning.")
                set(ENABLE_EXT_BUILTIN_SQLITE OFF CACHE BOOL "Enable extended sqlite builtins" FORCE)
            endif()
        endif()
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
    lib
)
if(PSCAL_PLATFORM_IOS)
    include_directories(
        src/smallclue/src
        src/smallclue
    )
endif()
if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_INCLUDE_DIRS)
    include_directories(${PSCAL_SQLITE_INCLUDE_DIRS})
endif()

# ---- Dependencies ----
set(PSCAL_OPENGL_TARGET "")

if(SDL)
    set(PSCAL_SDL_INCLUDE_DIRS "")
    set(PSCAL_SDL_LIBRARIES "")
    set(PSCAL_SDL_CORE_LINK_LIBS "")
    set(_PSCAL_SDL_PROVIDER "")
    set(_PSCAL_SDL_SELECTED_MAJOR "")

    if(PSCAL_PLATFORM_IOS)
        set(_PSCAL_SDL_SELECTED_MAJOR 2)
        set(_PSCAL_SDL_PROVIDER "vendored subprojects")
        set(_PSCAL_VENDORED_SDL2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third-party/SDL2")
        set(_PSCAL_VENDORED_SDL2_SRC "${_PSCAL_VENDORED_SDL2_ROOT}/SDL2-2.32.10")
        set(_PSCAL_VENDORED_SDL2_IMAGE_SRC "${_PSCAL_VENDORED_SDL2_ROOT}/SDL2_image-2.8.8")
        set(_PSCAL_VENDORED_SDL2_MIXER_SRC "${_PSCAL_VENDORED_SDL2_ROOT}/SDL2_mixer-2.8.0")
        set(_PSCAL_VENDORED_SDL2_TTF_SRC "${_PSCAL_VENDORED_SDL2_ROOT}/SDL2_ttf-2.24.0")
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

        function(_pscal_check_vendored_component label path)
            if(NOT EXISTS "${path}/CMakeLists.txt")
                message(FATAL_ERROR "Missing vendored ${label} source directory at ${path}. Run the vendor sync or update the checkout.")
            endif()
        endfunction()

        function(_pscal_add_vendored_component label path)
            _pscal_check_vendored_component(${label} "${path}")
            add_subdirectory("${path}" "${CMAKE_BINARY_DIR}/third-party/${label}")
        endfunction()

        set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build vendored SDL components as static libraries" FORCE)
        set(SDL_SHARED OFF CACHE BOOL "Disable SDL shared library for vendored build" FORCE)
        set(SDL_STATIC ON CACHE BOOL "Enable SDL static library for vendored build" FORCE)
        set(SDL_TEST OFF CACHE BOOL "Disable SDL test library for vendored build" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "Disable SDL test programs for vendored build" FORCE)
        set(SDL_INSTALL_TESTS OFF CACHE BOOL "Disable SDL test install rules" FORCE)
        set(SDL2_DISABLE_INSTALL ON CACHE BOOL "Disable SDL2 install targets for vendored build" FORCE)

        set(SDL2IMAGE_INSTALL OFF CACHE BOOL "Disable SDL2_image installation" FORCE)
        set(SDL2IMAGE_SAMPLES OFF CACHE BOOL "Disable SDL2_image samples" FORCE)
        set(SDL2IMAGE_SAMPLES_INSTALL OFF CACHE BOOL "Disable SDL2_image sample install" FORCE)
        set(SDL2IMAGE_TESTS OFF CACHE BOOL "Disable SDL2_image tests" FORCE)
        set(SDL2IMAGE_TESTS_INSTALL OFF CACHE BOOL "Disable SDL2_image test install" FORCE)
        set(SDL2IMAGE_VENDORED ON CACHE BOOL "Use vendored dependencies for SDL2_image" FORCE)
        set(SDL2IMAGE_BUILD_SHARED_LIBS OFF CACHE BOOL "Build SDL2_image as a static library" FORCE)
        set(SDL2IMAGE_AVIF OFF CACHE BOOL "Disable AVIF loader to avoid missing vendored deps" FORCE)
        set(SDL2IMAGE_JXL OFF CACHE BOOL "Disable JXL loader to avoid missing vendored deps" FORCE)
        set(SDL2IMAGE_TIF OFF CACHE BOOL "Disable TIFF loader to avoid missing vendored deps" FORCE)
        set(SDL2IMAGE_WEBP OFF CACHE BOOL "Disable WEBP loader to avoid missing vendored deps" FORCE)

        set(SDL2MIXER_INSTALL OFF CACHE BOOL "Disable SDL2_mixer installation" FORCE)
        set(SDL2MIXER_SAMPLES OFF CACHE BOOL "Disable SDL2_mixer sample programs" FORCE)
        set(SDL2MIXER_SAMPLES_INSTALL OFF CACHE BOOL "Disable SDL2_mixer sample install" FORCE)
        set(SDL2MIXER_FLAC OFF CACHE BOOL "Disable FLAC support in SDL2_mixer for iOS build" FORCE)
        set(SDL2MIXER_GME OFF CACHE BOOL "Disable GME support in SDL2_mixer for iOS build" FORCE)
        set(SDL2MIXER_MOD OFF CACHE BOOL "Disable MOD support in SDL2_mixer for iOS build" FORCE)
        set(SDL2MIXER_MIDI OFF CACHE BOOL "Disable MIDI support in SDL2_mixer for iOS build" FORCE)
        set(SDL2MIXER_OPUS OFF CACHE BOOL "Disable Opus support in SDL2_mixer for iOS build" FORCE)
        set(SDL2MIXER_WAVPACK OFF CACHE BOOL "Disable WavPack support in SDL2_mixer for iOS build" FORCE)
        set(SDL2MIXER_BUILD_SHARED_LIBS OFF CACHE BOOL "Build SDL2_mixer as a static library" FORCE)

        set(SDL2TTF_INSTALL OFF CACHE BOOL "Disable SDL2_ttf installation" FORCE)
        set(SDL2TTF_SAMPLES OFF CACHE BOOL "Disable SDL2_ttf samples" FORCE)
        set(SDL2TTF_VENDORED ON CACHE BOOL "Use vendored dependencies for SDL2_ttf" FORCE)
        set(SDL2TTF_BUILD_SHARED_LIBS OFF CACHE BOOL "Build SDL2_ttf as a static library" FORCE)

        _pscal_add_vendored_component("SDL2" "${_PSCAL_VENDORED_SDL2_SRC}")
        _pscal_add_vendored_component("SDL2_image" "${_PSCAL_VENDORED_SDL2_IMAGE_SRC}")
        _pscal_add_vendored_component("SDL2_mixer" "${_PSCAL_VENDORED_SDL2_MIXER_SRC}")
        _pscal_add_vendored_component("SDL2_ttf" "${_PSCAL_VENDORED_SDL2_TTF_SRC}")

        function(_pscal_select_target out_target static_target fallback_target)
            if(TARGET ${static_target})
                set(${out_target} ${static_target} PARENT_SCOPE)
            elseif(TARGET ${fallback_target})
                set(${out_target} ${fallback_target} PARENT_SCOPE)
            else()
                message(FATAL_ERROR "Expected target ${fallback_target} (or ${static_target}) was not defined while configuring vendored SDL2.")
            endif()
        endfunction()

        _pscal_select_target(_PSCAL_VENDORED_SDL2_TARGET "SDL2::SDL2-static" "SDL2::SDL2")
        _pscal_select_target(_PSCAL_VENDORED_SDL2_IMAGE_TARGET "SDL2_image::SDL2_image-static" "SDL2_image::SDL2_image")
        _pscal_select_target(_PSCAL_VENDORED_SDL2_MIXER_TARGET "SDL2_mixer::SDL2_mixer-static" "SDL2_mixer::SDL2_mixer")
        _pscal_select_target(_PSCAL_VENDORED_SDL2_TTF_TARGET "SDL2_ttf::SDL2_ttf-static" "SDL2_ttf::SDL2_ttf")

        set(PSCAL_SDL_CORE_LINK_LIBS ${_PSCAL_VENDORED_SDL2_TARGET})
        set(PSCAL_SDL_INCLUDE_DIRS "${_PSCAL_VENDORED_SDL2_SRC}/include")

        set(PC_SDL_IMAGE_INCLUDE_DIRS "${_PSCAL_VENDORED_SDL2_IMAGE_SRC}/include")
        set(PC_SDL_IMAGE_LIBRARIES ${_PSCAL_VENDORED_SDL2_IMAGE_TARGET})
        set(PC_SDL_IMAGE_LIBRARY_DIRS "")
        set(PC_SDL_IMAGE_LDFLAGS_OTHER "")

        set(PC_SDL_MIXER_INCLUDE_DIRS "${_PSCAL_VENDORED_SDL2_MIXER_SRC}/include")
        set(PC_SDL_MIXER_LIBRARIES ${_PSCAL_VENDORED_SDL2_MIXER_TARGET})
        set(PC_SDL_MIXER_LIBRARY_DIRS "")
        set(PC_SDL_MIXER_LDFLAGS_OTHER "")

        set(PC_SDL_TTF_INCLUDE_DIRS "${_PSCAL_VENDORED_SDL2_TTF_SRC}")
        set(PC_SDL_TTF_LIBRARIES ${_PSCAL_VENDORED_SDL2_TTF_TARGET})
        set(PC_SDL_TTF_LIBRARY_DIRS "")
        set(PC_SDL_TTF_LDFLAGS_OTHER "")

        list(APPEND PSCAL_SDL_PLATFORM_LIBS "-framework ImageIO" "-framework MobileCoreServices")
    else()
        find_package(PkgConfig REQUIRED)
        set(_PSCAL_SDL_CANDIDATES "2")
        if(PSCAL_USE_SDL3)
            set(_PSCAL_SDL_CANDIDATES "3;2")
        endif()

        foreach(_PSCAL_SDL_MAJOR IN LISTS _PSCAL_SDL_CANDIDATES)
            set(_PSCAL_SDL_FOUND FALSE)
            set(_PSCAL_SDL_PROVIDER "")

            if(_PSCAL_SDL_MAJOR EQUAL 3)
                find_package(SDL3 QUIET CONFIG)
                if(TARGET SDL3::SDL3)
                    set(_PSCAL_SDL_FOUND TRUE)
                    set(_PSCAL_SDL_PROVIDER "CMake package")
                    set(PSCAL_SDL_CORE_LINK_LIBS SDL3::SDL3)
                    if(DEFINED SDL3_INCLUDE_DIRS)
                        set(PSCAL_SDL_INCLUDE_DIRS ${SDL3_INCLUDE_DIRS})
                    endif()
                endif()
                if(NOT _PSCAL_SDL_FOUND)
                    pkg_check_modules(PC_SDL_CORE QUIET sdl3)
                    if(PC_SDL_CORE_FOUND)
                        set(_PSCAL_SDL_FOUND TRUE)
                        set(_PSCAL_SDL_PROVIDER "pkg-config")
                        set(PSCAL_SDL_INCLUDE_DIRS ${PC_SDL_CORE_INCLUDE_DIRS})
                        set(PSCAL_SDL_CORE_LINK_LIBS ${PC_SDL_CORE_LIBRARIES})
                    endif()
                endif()
                if(_PSCAL_SDL_FOUND)
                    pkg_check_modules(PC_SDL_IMAGE QUIET SDL3_image)
                    pkg_check_modules(PC_SDL_MIXER QUIET SDL3_mixer)
                    pkg_check_modules(PC_SDL_TTF QUIET SDL3_ttf)
                    if(NOT (PC_SDL_IMAGE_FOUND AND PC_SDL_MIXER_FOUND AND PC_SDL_TTF_FOUND))
                        set(_PSCAL_SDL_FOUND FALSE)
                    endif()
                endif()
            else()
                find_package(SDL2 QUIET)
                if(SDL2_FOUND)
                    set(_PSCAL_SDL_FOUND TRUE)
                    set(_PSCAL_SDL_PROVIDER "CMake package")
                    if(TARGET SDL2::SDL2)
                        set(PSCAL_SDL_CORE_LINK_LIBS SDL2::SDL2)
                    endif()
                    if(DEFINED SDL2_INCLUDE_DIRS)
                        set(PSCAL_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
                    endif()
                endif()
                if(NOT _PSCAL_SDL_FOUND)
                    pkg_check_modules(PC_SDL_CORE QUIET sdl2)
                    if(PC_SDL_CORE_FOUND)
                        set(_PSCAL_SDL_FOUND TRUE)
                        set(_PSCAL_SDL_PROVIDER "pkg-config")
                        set(PSCAL_SDL_INCLUDE_DIRS ${PC_SDL_CORE_INCLUDE_DIRS})
                        set(PSCAL_SDL_CORE_LINK_LIBS ${PC_SDL_CORE_LIBRARIES})
                    endif()
                endif()
                if(_PSCAL_SDL_FOUND)
                    pkg_check_modules(PC_SDL_IMAGE REQUIRED SDL2_image)
                    pkg_check_modules(PC_SDL_MIXER REQUIRED SDL2_mixer)
                    pkg_check_modules(PC_SDL_TTF REQUIRED SDL2_ttf)
                endif()
            endif()

            if(_PSCAL_SDL_FOUND)
                set(_PSCAL_SDL_SELECTED_MAJOR ${_PSCAL_SDL_MAJOR})
                break()
            elseif(_PSCAL_SDL_MAJOR EQUAL 3)
                message(WARNING "SDL3 development headers not found; falling back to SDL2.")
            endif()
        endforeach()
    endif()

    if(NOT _PSCAL_SDL_SELECTED_MAJOR)
        if(release)
            message(FATAL_ERROR "-Drelease requires SDL development files so SDL-dependent builtins can be enabled.")
        else()
            message(WARNING "SDL development files not found; disabling SDL support. Re-run with -DSDL=OFF to silence this warning.")
            set(SDL OFF)
            set(SDL OFF CACHE BOOL "Enable SDL-based graphics and audio" FORCE)
        endif()
    else()
        add_compile_definitions(SDL)
        if(_PSCAL_SDL_SELECTED_MAJOR EQUAL 3)
            add_compile_definitions(PSCALI_SDL3)
        endif()
        message(STATUS "SDL${_PSCAL_SDL_SELECTED_MAJOR} detected via ${_PSCAL_SDL_PROVIDER}")
        if(PSCAL_SDL_INCLUDE_DIRS)
            message(STATUS "SDL Include Dirs: ${PSCAL_SDL_INCLUDE_DIRS}")
        endif()

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

        if(NOT PSCAL_PLATFORM_IOS)
            find_package(OpenGL REQUIRED)
            if(TARGET OpenGL::GL)
                set(PSCAL_OPENGL_TARGET OpenGL::GL)
            elseif(TARGET OpenGL::OpenGL)
                set(PSCAL_OPENGL_TARGET OpenGL::OpenGL)
            elseif(OPENGL_LIBRARIES)
                set(PSCAL_OPENGL_TARGET ${OPENGL_LIBRARIES})
            endif()
        else()
            set(PSCAL_OPENGL_TARGET "")
        endif()

        if(PSCAL_SDL_INCLUDE_DIRS)
            include_directories(BEFORE ${PSCAL_SDL_INCLUDE_DIRS})
        endif()
        if(PC_SDL_IMAGE_INCLUDE_DIRS)
            include_directories(BEFORE ${PC_SDL_IMAGE_INCLUDE_DIRS})
        endif()
        if(PC_SDL_MIXER_INCLUDE_DIRS)
            include_directories(BEFORE ${PC_SDL_MIXER_INCLUDE_DIRS})
        endif()
        if(PC_SDL_TTF_INCLUDE_DIRS)
            include_directories(BEFORE ${PC_SDL_TTF_INCLUDE_DIRS})
        endif()

        # Provide backward-compatible variable names used elsewhere in the build.
    endif()
endif()

# ---- iOS SDK cache cleanup (Zlib) ----
if(PSCAL_REAL_IOS)
    if(DEFINED ZLIB_INCLUDE_DIR AND NOT EXISTS "${ZLIB_INCLUDE_DIR}/zlib.h")
        unset(ZLIB_INCLUDE_DIR CACHE)
    endif()
    if(DEFINED ZLIB_LIBRARY AND NOT EXISTS "${ZLIB_LIBRARY}")
        unset(ZLIB_LIBRARY CACHE)
    endif()
    if(DEFINED ZLIB_LIBRARY_RELEASE AND NOT EXISTS "${ZLIB_LIBRARY_RELEASE}")
        unset(ZLIB_LIBRARY_RELEASE CACHE)
    endif()
    if(DEFINED ZLIB_LIBRARY_DEBUG AND NOT EXISTS "${ZLIB_LIBRARY_DEBUG}")
        unset(ZLIB_LIBRARY_DEBUG CACHE)
    endif()

    set(_PSCAL_ZLIB_SYSROOT "${CMAKE_OSX_SYSROOT}")
    if(_PSCAL_ZLIB_SYSROOT STREQUAL "iphoneos" OR _PSCAL_ZLIB_SYSROOT STREQUAL "iphonesimulator")
        execute_process(
            COMMAND xcrun --sdk ${_PSCAL_ZLIB_SYSROOT} --show-sdk-path
            OUTPUT_VARIABLE _PSCAL_ZLIB_SYSROOT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
    if(_PSCAL_ZLIB_SYSROOT AND EXISTS "${_PSCAL_ZLIB_SYSROOT}/usr/include/zlib.h")
        if(NOT ZLIB_INCLUDE_DIR)
            set(ZLIB_INCLUDE_DIR "${_PSCAL_ZLIB_SYSROOT}/usr/include" CACHE PATH "Zlib include directory")
        endif()
    endif()
    if(_PSCAL_ZLIB_SYSROOT AND EXISTS "${_PSCAL_ZLIB_SYSROOT}/usr/lib/libz.tbd")
        if(NOT ZLIB_LIBRARY_RELEASE)
            set(ZLIB_LIBRARY_RELEASE "${_PSCAL_ZLIB_SYSROOT}/usr/lib/libz.tbd" CACHE FILEPATH "Zlib release library")
        endif()
        if(NOT ZLIB_LIBRARY_DEBUG)
            set(ZLIB_LIBRARY_DEBUG "${_PSCAL_ZLIB_SYSROOT}/usr/lib/libz.tbd" CACHE FILEPATH "Zlib debug library")
        endif()
        if(NOT ZLIB_LIBRARY)
            set(ZLIB_LIBRARY "${_PSCAL_ZLIB_SYSROOT}/usr/lib/libz.tbd" CACHE FILEPATH "Zlib library")
        endif()
    endif()
    unset(_PSCAL_ZLIB_SYSROOT)
endif()

# --- CURL (system vs bundled) ---
# Option A: build against a bundled libcurl (default on iOS where the SDK does not provide one).
# Option B: rely on the system SDK via FindCURL (with optional Homebrew hints on macOS).
set(_PSCAL_DEFAULT_BUNDLED_CURL OFF)
if(PSCAL_REAL_IOS)
    set(_PSCAL_DEFAULT_BUNDLED_CURL ON)
endif()
option(PSCAL_USE_BUNDLED_CURL "Build libcurl from third-party/curl-8.17.0" ${_PSCAL_DEFAULT_BUNDLED_CURL})

if(PSCAL_USE_BUNDLED_CURL)
    message(STATUS "CURL: building bundled libcurl from third-party/curl-8.17.0")
    # Build a static-only libcurl without the command line tool, docs, or tests.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "Build static libraries" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "Build curl executable" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "Build libcurl tests" FORCE)
    set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "Build libcurl documentation" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "Build libcurl examples" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "Disable LDAP support in libcurl" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "Disable LDAPS support in libcurl" FORCE)
    set(CURL_DISABLE_TELNET ON CACHE BOOL "Disable TELNET support in libcurl" FORCE)
    set(CURL_DISABLE_TFTP ON CACHE BOOL "Disable TFTP support in libcurl" FORCE)
    set(CURL_DISABLE_SMTP ON CACHE BOOL "Disable SMTP support in libcurl" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "Disable libpsl dependency for bundled libcurl" FORCE)
    if(PSCAL_REAL_IOS)
        # Prefer OpenSSL with Apple SecTrust so we can rely on the system
        # certificate store instead of shipping our own CA bundle.
        set(CURL_USE_OPENSSL ON CACHE BOOL "Use OpenSSL for bundled libcurl" FORCE)
        set(USE_APPLE_SECTRUST ON CACHE BOOL "Enable Apple SecTrust validation on Apple platforms" FORCE)
        set(CURL_BROTLI OFF CACHE BOOL "Disable brotli to avoid extra dependencies on iOS" FORCE)
        set(CURL_ZSTD OFF CACHE BOOL "Disable zstd to avoid extra dependencies on iOS" FORCE)
        set(USE_NGHTTP2 OFF CACHE BOOL "Disable nghttp2 on iOS builds" FORCE)
        set(HTTP_ONLY ON CACHE BOOL "Limit libcurl to HTTP/HTTPS on iOS builds" FORCE)
    else()
        set(CURL_USE_OPENSSL ON CACHE BOOL "Use OpenSSL for bundled libcurl" FORCE)
    endif()
    if(PSCAL_REAL_IOS)
        if(NOT DEFINED PSCAL_OPENSSL_INSTALL_DIR)
            message(FATAL_ERROR "Bundled libcurl on iOS requires PSCAL_OPENSSL_INSTALL_DIR (ensure PSCAL_PLATFORM_IOS OpenSSL logic runs first).")
        endif()
        set(_PSCAL_CURL_OPENSSL_ROOT "${PSCAL_OPENSSL_INSTALL_DIR}")
        set(OPENSSL_ROOT_DIR "${_PSCAL_CURL_OPENSSL_ROOT}" CACHE PATH "OpenSSL root used by bundled libcurl" FORCE)
        set(OPENSSL_INCLUDE_DIR "${_PSCAL_CURL_OPENSSL_ROOT}/include" CACHE PATH "OpenSSL include dir for bundled libcurl" FORCE)
        set(OPENSSL_CRYPTO_LIBRARY "${_PSCAL_CURL_OPENSSL_ROOT}/lib/libcrypto.a" CACHE FILEPATH "OpenSSL libcrypto for bundled libcurl" FORCE)
        set(OPENSSL_SSL_LIBRARY "${_PSCAL_CURL_OPENSSL_ROOT}/lib/libssl.a" CACHE FILEPATH "OpenSSL libssl for bundled libcurl" FORCE)
        set(OpenSSL_DIR "${_PSCAL_CURL_OPENSSL_ROOT}/lib/cmake/OpenSSL" CACHE PATH "OpenSSL config path for bundled libcurl" FORCE)
        set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "Link bundled libcurl against static OpenSSL" FORCE)
        set(CURL_USE_SECTRANSPORT OFF CACHE BOOL "Disable SecureTransport when using bundled libcurl" FORCE)
        set(CURL_USE_SCHANNEL OFF CACHE BOOL "Disable Schannel when using bundled libcurl" FORCE)
        set(CURL_USE_MBEDTLS OFF CACHE BOOL "Disable mbedTLS when using bundled libcurl" FORCE)
        set(CURL_USE_WOLFSSL OFF CACHE BOOL "Disable wolfSSL when using bundled libcurl" FORCE)
        set(CURL_USE_GNUTLS OFF CACHE BOOL "Disable GnuTLS when using bundled libcurl" FORCE)
        set(CURL_USE_RUSTLS OFF CACHE BOOL "Disable Rustls when using bundled libcurl" FORCE)
    endif()
    add_subdirectory(third-party/curl-8.17.0 EXCLUDE_FROM_ALL)
    if(TARGET libcurl_static)
        set(_PSCAL_LIBCURL_TARGET libcurl_static)
    elseif(TARGET libcurl)
        set(_PSCAL_LIBCURL_TARGET libcurl)
    else()
        set(_PSCAL_LIBCURL_TARGET "")
    endif()
    if(_PSCAL_LIBCURL_TARGET STREQUAL "")
        message(FATAL_ERROR "Bundled libcurl target not found after configuring third-party/curl-8.17.0.")
    endif()
    if(PSCAL_REAL_IOS AND TARGET pscal_openssl)
        add_dependencies(${_PSCAL_LIBCURL_TARGET} pscal_openssl)
    endif()
    if(NOT TARGET CURL::libcurl)
        add_library(CURL::libcurl ALIAS ${_PSCAL_LIBCURL_TARGET})
    endif()
    get_target_property(_PSCAL_CURL_INTERFACE_DIRS ${_PSCAL_LIBCURL_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
    if(_PSCAL_CURL_INTERFACE_DIRS)
        set(CURL_INCLUDE_DIRS ${_PSCAL_CURL_INTERFACE_DIRS})
    endif()
    if(NOT CURL_INCLUDE_DIRS)
        set(CURL_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/curl-8.17.0/include")
    endif()
    set(CURL_INCLUDE_DIRS "${CURL_INCLUDE_DIRS}" CACHE STRING "libcurl include directories" FORCE)
    set(CURL_LIBRARIES CURL::libcurl)
    set(CURL_FOUND ON CACHE BOOL "Bundled libcurl configured" FORCE)
else()
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

    set(_PSCAL_REQUIRE_CURL ON)
    if(PSCAL_PLATFORM_IOS)
        set(_PSCAL_REQUIRE_CURL OFF)
    endif()

    find_package(CURL QUIET)
    if(NOT CURL_FOUND)
        if(_PSCAL_REQUIRE_CURL)
            message(FATAL_ERROR "libcurl is required for this configuration. Install libcurl dev headers or set PSCAL_USE_BUNDLED_CURL=ON.")
        else()
            message(WARNING "libcurl not found; network builtins will be disabled for this build.")
        endif()
    else()
        if(TARGET CURL::libcurl)
            message(STATUS "CURL: using imported target CURL::libcurl")
        else()
            message(STATUS "CURL Include Dirs: ${CURL_INCLUDE_DIRS}")
            message(STATUS "CURL Libraries Variable: ${CURL_LIBRARIES}")
        endif()
    endif()
endif()

set(PSCAL_HAS_LIBCURL ${CURL_FOUND})
if(PSCAL_HAS_LIBCURL)
    add_compile_definitions(PSCAL_HAS_LIBCURL)
endif()
if(NOT PSCAL_HAS_LIBCURL AND ENABLE_EXT_BUILTIN_OPENAI)
    message(WARNING "Disabling OpenAI extended builtins: libcurl not available.")
    set(ENABLE_EXT_BUILTIN_OPENAI OFF CACHE BOOL "Enable extended OpenAI builtins" FORCE)
endif()

if(ENABLE_EXT_BUILTIN_OPENAI)
    add_compile_definitions(ENABLE_EXT_BUILTIN_OPENAI)
endif()

find_package(Threads REQUIRED)

if(PSCAL_HAS_LIBCURL)
    set(PSCAL_NETWORK_IMPL_SOURCE src/backend_ast/builtin_network_api.c)
else()
    set(PSCAL_NETWORK_IMPL_SOURCE src/backend_ast/builtin_network_api_stub.c)
endif()

# ---- Source Files ----
set(PSCAL_CORE_SOURCES
    src/Pascal/globals.c
    src/Pascal/type_registry.c
    src/core/types.c src/core/utils.c src/core/list.c src/core/preproc.c src/core/version.c
    src/ast/ast.c src/ast/closure_registry.c
    src/symbol/symbol.c
    src/backend_ast/builtin.c
    ${PSCAL_NETWORK_IMPL_SOURCE}
    src/backend_ast/shell_runtime_stub.c
    src/backend_ast/thread_wrappers.c
    src/compiler/bytecode.c src/compiler/compiler.c
    src/core/cache.c
    src/common/runtime_params.c
    src/common/runtime_tty.c
    src/common/runtime_clipboard.c
    src/common/pscal_hosts.c
    src/common/path_truncate.c
    src/common/path_virtualization.c
    src/common/pscal_terminal_host.c
    src/common/frontend_kind.c
    src/common/runtime_paths.c
    src/common/pascal_state.c
    src/vm/vm.c
    lib/noise/noise.c
    src/runtime/terrain/terrain_generator.c
    src/runtime/shaders/shader_common.c
    src/runtime/shaders/terrain/terrain_shader.c
    src/runtime/shaders/sky/sky_dome.c
    src/runtime/shaders/sky/cloud_layer.c
)

set(SMALLCLUE_SOURCES
    src/smallclue/src/core.c
    src/smallclue/src/integration.c
    src/smallclue/src/nextvi_app.c
    src/smallclue/src/openssh_app.c
    src/smallclue/src/vproc_test_app.c
)

if(PSCAL_PLATFORM_IOS)
    list(APPEND PSCAL_CORE_SOURCES ${SMALLCLUE_SOURCES})
endif()

set(PSCAL_OPENSSH_ROOT third-party/openssh-10.2p1)
set(PSCAL_OPENSSH_COMMON_SOURCES
        ${PSCAL_OPENSSH_ROOT}/pscal_ios_shim.c
        ${PSCAL_OPENSSH_ROOT}/addr.c
        ${PSCAL_OPENSSH_ROOT}/addrmatch.c
        ${PSCAL_OPENSSH_ROOT}/atomicio.c
        ${PSCAL_OPENSSH_ROOT}/authfd.c
        ${PSCAL_OPENSSH_ROOT}/authfile.c
        ${PSCAL_OPENSSH_ROOT}/bitmap.c
        ${PSCAL_OPENSSH_ROOT}/canohost.c
        ${PSCAL_OPENSSH_ROOT}/channels.c
        ${PSCAL_OPENSSH_ROOT}/chacha.c
        ${PSCAL_OPENSSH_ROOT}/cipher.c
        ${PSCAL_OPENSSH_ROOT}/cipher-aes.c
        ${PSCAL_OPENSSH_ROOT}/cipher-aesctr.c
        ${PSCAL_OPENSSH_ROOT}/cipher-chachapoly.c
        ${PSCAL_OPENSSH_ROOT}/cipher-chachapoly-libcrypto.c
        ${PSCAL_OPENSSH_ROOT}/compat.c
        ${PSCAL_OPENSSH_ROOT}/digest-libc.c
        ${PSCAL_OPENSSH_ROOT}/digest-openssl.c
        ${PSCAL_OPENSSH_ROOT}/dispatch.c
        ${PSCAL_OPENSSH_ROOT}/dns.c
        ${PSCAL_OPENSSH_ROOT}/dh.c
        ${PSCAL_OPENSSH_ROOT}/ed25519.c
        ${PSCAL_OPENSSH_ROOT}/entropy.c
        ${PSCAL_OPENSSH_ROOT}/fatal.c
        ${PSCAL_OPENSSH_ROOT}/hash.c
        ${PSCAL_OPENSSH_ROOT}/hmac.c
        ${PSCAL_OPENSSH_ROOT}/kex.c
        ${PSCAL_OPENSSH_ROOT}/kex-names.c
        ${PSCAL_OPENSSH_ROOT}/kexc25519.c
        ${PSCAL_OPENSSH_ROOT}/kexdh.c
        ${PSCAL_OPENSSH_ROOT}/kexecdh.c
        ${PSCAL_OPENSSH_ROOT}/kexgen.c
        ${PSCAL_OPENSSH_ROOT}/kexgex.c
        ${PSCAL_OPENSSH_ROOT}/kexgexc.c
        ${PSCAL_OPENSSH_ROOT}/kexgexs.c
        ${PSCAL_OPENSSH_ROOT}/kexmlkem768x25519.c
        ${PSCAL_OPENSSH_ROOT}/kexsntrup761x25519.c
        ${PSCAL_OPENSSH_ROOT}/krl.c
        ${PSCAL_OPENSSH_ROOT}/log.c
        ${PSCAL_OPENSSH_ROOT}/mac.c
        ${PSCAL_OPENSSH_ROOT}/match.c
        ${PSCAL_OPENSSH_ROOT}/misc.c
        ${PSCAL_OPENSSH_ROOT}/misc-agent.c
        ${PSCAL_OPENSSH_ROOT}/moduli.c
        ${PSCAL_OPENSSH_ROOT}/hostfile.c
        ${PSCAL_OPENSSH_ROOT}/monitor_fdpass.c
        ${PSCAL_OPENSSH_ROOT}/msg.c
        ${PSCAL_OPENSSH_ROOT}/nchan.c
        ${PSCAL_OPENSSH_ROOT}/packet.c
        ${PSCAL_OPENSSH_ROOT}/platform-misc.c
        ${PSCAL_OPENSSH_ROOT}/platform-pledge.c
        ${PSCAL_OPENSSH_ROOT}/platform-tracing.c
        ${PSCAL_OPENSSH_ROOT}/poly1305.c
        ${PSCAL_OPENSSH_ROOT}/progressmeter.c
        ${PSCAL_OPENSSH_ROOT}/readpass.c
        ${PSCAL_OPENSSH_ROOT}/rijndael.c
        ${PSCAL_OPENSSH_ROOT}/sftp-realpath.c
        ${PSCAL_OPENSSH_ROOT}/smult_curve25519_ref.c
        ${PSCAL_OPENSSH_ROOT}/sntrup761.c
        ${PSCAL_OPENSSH_ROOT}/ssh_api.c
        ${PSCAL_OPENSSH_ROOT}/ssh-ed25519.c
        ${PSCAL_OPENSSH_ROOT}/ssh-ecdsa.c
        ${PSCAL_OPENSSH_ROOT}/ssh-sk.c
        ${PSCAL_OPENSSH_ROOT}/ssh-rsa.c
        ${PSCAL_OPENSSH_ROOT}/sshbuf.c
        ${PSCAL_OPENSSH_ROOT}/sshbuf-getput-basic.c
        ${PSCAL_OPENSSH_ROOT}/sshbuf-getput-crypto.c
        ${PSCAL_OPENSSH_ROOT}/sshbuf-io.c
        ${PSCAL_OPENSSH_ROOT}/sshbuf-misc.c
        ${PSCAL_OPENSSH_ROOT}/ssherr.c
        ${PSCAL_OPENSSH_ROOT}/sshkey.c
        ${PSCAL_OPENSSH_ROOT}/sshsig.c
        ${PSCAL_OPENSSH_ROOT}/ssh-pkcs11-client.c
        ${PSCAL_OPENSSH_ROOT}/ssh-sk-client.c
        ${PSCAL_OPENSSH_ROOT}/ttymodes.c
        ${PSCAL_OPENSSH_ROOT}/umac.c
        ${PSCAL_OPENSSH_ROOT}/umac128.c
        ${PSCAL_OPENSSH_ROOT}/utf8.c
        ${PSCAL_OPENSSH_ROOT}/xmalloc.c
        ${PSCAL_OPENSSH_ROOT}/pscal_runtime_hooks.c
    )
    set(PSCAL_OPENSSH_CLIENT_SOURCES
        ${PSCAL_OPENSSH_ROOT}/clientloop.c
        ${PSCAL_OPENSSH_ROOT}/mux.c
        ${PSCAL_OPENSSH_ROOT}/readconf.c
        ${PSCAL_OPENSSH_ROOT}/ssh.c
        ${PSCAL_OPENSSH_ROOT}/sshconnect.c
        ${PSCAL_OPENSSH_ROOT}/sshconnect2.c
        ${PSCAL_OPENSSH_ROOT}/sshtty.c
    )
    set(PSCAL_OPENSSH_SCP_SOURCES ${PSCAL_OPENSSH_ROOT}/scp.c)
    set(PSCAL_OPENSSH_SFTP_SOURCES
        ${PSCAL_OPENSSH_ROOT}/sftp.c
        ${PSCAL_OPENSSH_ROOT}/sftp-client.c
        ${PSCAL_OPENSSH_ROOT}/sftp-common.c
        ${PSCAL_OPENSSH_ROOT}/sftp-glob.c
        ${PSCAL_OPENSSH_ROOT}/sftp-usergroup.c
    )
    set(PSCAL_OPENSSH_OPENBSD_COMPAT_SOURCES
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/arc4random.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/arc4random_uniform.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/base64.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/basename.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bcrypt_pbkdf.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bindresvport.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/blowfish.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/daemon.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/dirname.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/explicit_bzero.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/fmt_scaled.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/fnmatch.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/freezero.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/getcwd.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/getgrouplist.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/getopt_long.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/glob.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/inet_aton.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/inet_ntoa.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/inet_ntop.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/md5.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/memmem.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/mktemp.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/pwcache.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/readpassphrase.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/reallocarray.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/recallocarray.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/rresvport.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/setenv.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/setproctitle.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/sha1.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/sha2.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/sigact.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/strcasestr.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/strlcat.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/strlcpy.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/strmode.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/strndup.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/strnlen.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/strptime.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/strsep.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/strtoll.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/strtonum.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/strtoul.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/strtoull.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/timingsafe_bcmp.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/vis.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-asprintf.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-closefrom.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-cygwin_util.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-err.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-flock.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-getentropy.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-getline.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-getpagesize.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-getpeereid.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-malloc.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-misc.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-nextstep.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-openpty.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-poll.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-pselect.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-setres_id.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-signal.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-snprintf.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-statvfs.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-timegm.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/bsd-waitpid.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/fake-rfc2553.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/kludge-fd_set.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/libressl-api-compat.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/openssl-compat.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/port-aix.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/port-irix.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/port-linux.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/port-net.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/port-prngd.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/port-solaris.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/port-uw.c
        ${PSCAL_OPENSSH_ROOT}/openbsd-compat/xcrypt.c
    )
    if(PSCAL_PLATFORM_IOS)
        list(APPEND PSCAL_OPENSSH_OPENBSD_COMPAT_SOURCES
            ${PSCAL_OPENSSH_ROOT}/pscal_getrrset_stub.c)
    else()
        list(APPEND PSCAL_OPENSSH_OPENBSD_COMPAT_SOURCES
            ${PSCAL_OPENSSH_ROOT}/openbsd-compat/getrrsetbyname.c)
    endif()

set(PSCAL_OPENSSH_SOURCES
    ${PSCAL_OPENSSH_COMMON_SOURCES}
    ${PSCAL_OPENSSH_CLIENT_SOURCES}
    ${PSCAL_OPENSSH_SCP_SOURCES}
    ${PSCAL_OPENSSH_SFTP_SOURCES}
    ${PSCAL_OPENSSH_ROOT}/ssh-keygen.c
    ${PSCAL_OPENSSH_OPENBSD_COMPAT_SOURCES}
)

list(APPEND PSCAL_CORE_SOURCES ${PSCAL_OPENSSH_SOURCES})
set_property(SOURCE ${PSCAL_OPENSSH_SOURCES}
    APPEND PROPERTY INCLUDE_DIRECTORIES
    "${CMAKE_BINARY_DIR}/generated/openssh"
    "${CMAKE_SOURCE_DIR}/third-party/openssh-10.2p1"
    "${CMAKE_SOURCE_DIR}/third-party/openssh-10.2p1/openbsd-compat}")
set_property(SOURCE ${PSCAL_OPENSSH_SOURCES}
    APPEND PROPERTY COMPILE_OPTIONS
    -Wno-deprecated-declarations
    -Wno-pointer-sign
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-string-compare)
if(PSCAL_PLATFORM_IOS)
    set_property(SOURCE ${PSCAL_OPENSSH_SOURCES}
        APPEND PROPERTY COMPILE_DEFINITIONS HAVE_CONFIG_H;PSCAL_TARGET_IOS;__progname=pscal_progname;VPROC_SHIM_DISABLED=1;VPROC_SHIM_HARD_DISABLED=1)
else()
    set_property(SOURCE ${PSCAL_OPENSSH_SOURCES}
        APPEND PROPERTY COMPILE_DEFINITIONS HAVE_CONFIG_H)
endif()
set_property(SOURCE ${PSCAL_OPENSSH_ROOT}/ssh.c
    APPEND PROPERTY COMPILE_DEFINITIONS main=pscal_openssh_ssh_main)
set_property(SOURCE ${PSCAL_OPENSSH_ROOT}/scp.c
    APPEND PROPERTY COMPILE_DEFINITIONS main=pscal_openssh_scp_main)
set_property(SOURCE ${PSCAL_OPENSSH_ROOT}/sftp.c
    APPEND PROPERTY COMPILE_DEFINITIONS main=pscal_openssh_sftp_main)
set_property(SOURCE ${PSCAL_OPENSSH_ROOT}/ssh-keygen.c
    APPEND PROPERTY COMPILE_DEFINITIONS main=pscal_openssh_ssh_keygen_main)
# --- Conditionally add SDL source files ---
if(SDL)
    list(APPEND PSCAL_CORE_SOURCES
        src/backend_ast/sdl.c
        src/backend_ast/sdl3d.c
        src/backend_ast/gl.c
        src/backend_ast/audio.c
    )
    if(PSCAL_PLATFORM_IOS)
        list(APPEND PSCAL_CORE_SOURCES
            src/backend_ast/graphics_3d_backend_ios.c
        )
    else()
        list(APPEND PSCAL_CORE_SOURCES
            src/backend_ast/graphics_3d_backend_gl.c
        )
    endif()
endif()

set(NEXTVI_CORE_SOURCES third-party/nextvi/vi.c)
list(APPEND PSCAL_CORE_SOURCES ${NEXTVI_CORE_SOURCES})
set_property(SOURCE third-party/nextvi/vi.c APPEND PROPERTY COMPILE_DEFINITIONS "main=nextvi_main_entry")
set_property(SOURCE third-party/nextvi/vi.c APPEND PROPERTY COMPILE_OPTIONS -Wno-gnu-folding-constant)

if(PSCAL_PLATFORM_IOS)
    list(APPEND PSCAL_CORE_SOURCES
        src/ios/progname_stub.c
        src/ios/runtime_session_stub.c
    )
endif()

# The OpenSSH integration on real iOS requires interposing getaddrinfo so the
# container-scoped hosts file is honored. Forced-iOS host parity builds should
# not override the system resolver, so this stays real-iOS only.
if(PSCAL_REAL_IOS)
    list(APPEND PSCAL_CORE_SOURCES
        src/ios/hosts_override.c)
    # When using the custom resolver/interposer, suppress the portable wrapper
    # definitions in pscal_hosts.c to avoid duplicate symbols.
    set_property(SOURCE src/common/pscal_hosts.c APPEND PROPERTY COMPILE_DEFINITIONS PSCAL_HOSTS_CUSTOM_IMPL)
endif()

# ---- Extended builtins ----
# Allow selectively enabling categories of extended builtins.
add_subdirectory(src/ext_builtins)
list(APPEND PSCAL_CORE_SOURCES ${PSCAL_EXT_BUILTIN_SOURCES})
list(APPEND PSCAL_CORE_SOURCES src/third_party/yyjson/yyjson.c)
if(PSCAL_PLATFORM_IOS)
    list(APPEND PSCAL_CORE_SOURCES
        src/common/frontend_symbol_wrappers.c
        src/ios/vproc.c
        src/ios/vproc_tree.c
        src/ios/tty/ish_compat.c
        src/ios/tty/pscal_fd.c
        src/ios/tty/pscal_tty.c
        src/ios/tty/pscal_pty.c
        src/ios/tty/pscal_tty_host.c
    )
endif()

set(PASCAL_FRONTEND_SOURCES
    src/Pascal/main.c
    src/Pascal/lexer.c
    src/Pascal/parser.c
    src/Pascal/semantic.c
    src/Pascal/opt.c
)

# ---- Toolchain feature detection ----
# Detect whether AddressSanitizer is usable before wiring it into dascal
set(PSCAL_CAN_ENABLE_ASAN OFF)
if(BUILD_DASCAL)
    if(PSCAL_PLATFORM_IOS)
        message(STATUS "AddressSanitizer disabled for iOS builds (runtime is unavailable on device/simulator targets).")
    else()
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
endif()


# ---- Executable Targets Function ----
function(pscal_apply_common_compilation_settings target_name)
    if(SDL)
        target_include_directories(${target_name} PRIVATE
            ${PSCAL_SDL_INCLUDE_DIRS}
            ${PC_SDL_IMAGE_INCLUDE_DIRS}
            ${PC_SDL_MIXER_INCLUDE_DIRS}
            ${PC_SDL_TTF_INCLUDE_DIRS}
        )
        target_compile_definitions(${target_name} PRIVATE SDL)
    endif()
    if(PSCAL_HAS_LIBCURL AND CURL_INCLUDE_DIRS)
        target_include_directories(${target_name} PRIVATE ${CURL_INCLUDE_DIRS})
    endif()
endfunction()

add_library(pscal_core_static STATIC ${PSCAL_CORE_SOURCES})
pscal_apply_common_compilation_settings(pscal_core_static)
if(PSCAL_HAS_LIBCURL AND CURL_INCLUDE_DIRS)
    target_include_directories(pscal_core_static PRIVATE ${CURL_INCLUDE_DIRS})
endif()
if(PSCAL_HAS_LIBCURL)
    if(TARGET CURL::libcurl)
        target_link_libraries(pscal_core_static PUBLIC CURL::libcurl)
    elseif(CURL_LIBRARIES)
        target_link_libraries(pscal_core_static PUBLIC ${CURL_LIBRARIES})
    endif()
endif()
target_compile_definitions(pscal_core_static PRIVATE PSCAL_NO_CLI_ENTRYPOINTS)
if(NOT PSCAL_PLATFORM_IOS OR PSCAL_USE_HOST_OPENSSL)
    if(OPENSSL_INCLUDE_DIR)
        target_include_directories(pscal_core_static PRIVATE ${OPENSSL_INCLUDE_DIR})
    endif()
endif()
target_link_libraries(pscal_core_static PUBLIC
    ${PSCAL_OPENSSL_SSL_TARGET}
    ${PSCAL_OPENSSL_CRYPTO_TARGET}
    z resolv)
if(PSCAL_PLATFORM_IOS AND NOT PSCAL_USE_HOST_OPENSSL)
    add_dependencies(pscal_core_static pscal_openssl)
    target_include_directories(pscal_core_static PUBLIC
        "${PSCAL_OPENSSL_INSTALL_DIR}/include")
endif()
if(PSCAL_REAL_IOS)
    target_compile_definitions(pscal_core_static PRIVATE __progname=pscal_progname __progname_full=pscal_progname_full)
    set(_pscal_core_embed_archives
        "$<TARGET_FILE:pscal_core_static>"
        "${PSCAL_OPENSSL_INSTALL_DIR}/lib/libssl.a"
        "${PSCAL_OPENSSL_INSTALL_DIR}/lib/libcrypto.a"
    )
    if(PSCAL_HAS_LIBCURL)
        if(TARGET CURL::libcurl)
            list(APPEND _pscal_core_embed_archives "$<TARGET_FILE:CURL::libcurl>")
        elseif(CURL_LIBRARIES)
            list(APPEND _pscal_core_embed_archives ${CURL_LIBRARIES})
        endif()
    endif()
    add_custom_command(TARGET pscal_core_static POST_BUILD
        COMMAND /usr/bin/libtool -static
            -o "$<TARGET_FILE:pscal_core_static>.merged"
            ${_pscal_core_embed_archives}
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:pscal_core_static>.merged"
            "$<TARGET_FILE:pscal_core_static>"
        COMMAND ${CMAKE_COMMAND} -E remove
            "$<TARGET_FILE:pscal_core_static>.merged"
            COMMENT "Embedding OpenSSL archives into libpscal_core_static.a for iOS")
endif()
target_include_directories(pscal_core_static BEFORE PRIVATE
    third-party/nextvi
)
if(PSCAL_PLATFORM_IOS)
    target_link_libraries(pscal_core_static PUBLIC pscal_common_frontend_shim)
endif()


function(add_pscal_executable target_name)
    set(exec_sources ${ARGN})
    add_executable(${target_name} ${exec_sources})
    pscal_apply_common_compilation_settings(${target_name})

    if(SDL)
        # Add library search paths from pkg-config
        target_link_directories(${target_name} PRIVATE
            ${PC_SDL_IMAGE_LIBRARY_DIRS}
            ${PC_SDL_MIXER_LIBRARY_DIRS}
            ${PC_SDL_TTF_LIBRARY_DIRS}
        )
    endif()

    # Construct the list of libraries and flags to link
    set(TARGET_LINK_LIBS "")

    if(SDL)
        # Core SDL
        list(APPEND TARGET_LINK_LIBS ${PSCAL_SDL_CORE_LINK_LIBS})

        # SDL Extensions - using the library names from pkg-config
        list(APPEND TARGET_LINK_LIBS ${PC_SDL_IMAGE_LIBRARIES})
        list(APPEND TARGET_LINK_LIBS ${PC_SDL_MIXER_LIBRARIES})
        list(APPEND TARGET_LINK_LIBS ${PC_SDL_TTF_LIBRARIES})

        # Add other linker flags from pkg-config (these might include rpath, etc.)
        list(APPEND TARGET_LINK_LIBS ${PC_SDL_IMAGE_LDFLAGS_OTHER})
        list(APPEND TARGET_LINK_LIBS ${PC_SDL_MIXER_LDFLAGS_OTHER})
        list(APPEND TARGET_LINK_LIBS ${PC_SDL_TTF_LDFLAGS_OTHER})
        if(PSCAL_OPENGL_TARGET)
            list(APPEND TARGET_LINK_LIBS ${PSCAL_OPENGL_TARGET})
        elseif(OPENGL_LIBRARIES)
            list(APPEND TARGET_LINK_LIBS ${OPENGL_LIBRARIES})
        endif()
        list(APPEND TARGET_LINK_LIBS ${PSCAL_SDL_PLATFORM_LIBS})
    endif()

    # CURL
    if(PSCAL_HAS_LIBCURL)
        if(TARGET CURL::libcurl)
            list(APPEND TARGET_LINK_LIBS CURL::libcurl)
        else()
            list(APPEND TARGET_LINK_LIBS ${CURL_LIBRARIES})
        endif()
    endif()

    if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
        list(APPEND TARGET_LINK_LIBS ${PSCAL_SQLITE_LIBS})
    endif()

    list(APPEND TARGET_LINK_LIBS pscal_core_static)
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

function(pscal_add_static_library target_name)
    set(options)
    set(oneValueArgs FRONTEND KIND)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(PSLIB "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(NOT PSLIB_SOURCES)
        message(FATAL_ERROR "pscal_add_static_library(${target_name}) requires SOURCES")
    endif()

    add_library(${target_name} STATIC ${PSLIB_SOURCES})
    pscal_apply_common_compilation_settings(${target_name})
    target_compile_definitions(${target_name} PRIVATE PSCAL_NO_CLI_ENTRYPOINTS)

    if(PSLIB_FRONTEND)
        target_compile_definitions(${target_name} PRIVATE ${PSLIB_FRONTEND})
    endif()

    if(PSLIB_KIND STREQUAL "pascal")
        target_compile_options(${target_name} PRIVATE -O3)
        if(RELEASE_BUILD)
            target_compile_definitions(${target_name} PRIVATE RELEASE)
        else()
            target_compile_definitions(${target_name} PRIVATE DEBUGNOT)
        endif()
        if(PSCAL_PLATFORM_IOS)
            target_compile_definitions(${target_name} PRIVATE SUPPRESS_EXIT)
        endif()
    elseif(PSLIB_KIND STREQUAL "dascal")
        target_compile_definitions(${target_name} PRIVATE DEBUG)
        if(PSCAL_CAN_ENABLE_ASAN)
            target_compile_options(${target_name} PRIVATE -fsanitize=address -g)
        else()
            target_compile_options(${target_name} PRIVATE -g)
        endif()
    elseif(PSLIB_KIND STREQUAL "hscal")
        target_compile_definitions(${target_name} PRIVATE DEBUG SUPPRESS_EXIT)
        target_compile_options(${target_name} PRIVATE -g)
    endif()
endfunction()

if(NOT PSCAL_PLATFORM_IOS)
    add_pscal_executable(pascal ${PASCAL_FRONTEND_SOURCES})
    target_compile_definitions(pascal PRIVATE FRONTEND_PASCAL)
endif()
pscal_add_static_library(pscal_pascal_static
    FRONTEND FRONTEND_PASCAL
    KIND pascal
    SOURCES ${PASCAL_FRONTEND_SOURCES})
target_link_libraries(pscal_pascal_static PUBLIC pscal_core_static)
if(NOT PSCAL_PLATFORM_IOS AND BUILD_DASCAL)
    add_pscal_executable(dascal ${PASCAL_FRONTEND_SOURCES})
endif()
if(PSCAL_BUILD_STATIC_LIBS AND BUILD_DASCAL)
    add_library(pscal_dascal_static STATIC src/dascal/shim.c)
    pscal_apply_common_compilation_settings(pscal_dascal_static)
    target_link_libraries(pscal_dascal_static PUBLIC pscal_pascal_static)
endif()
# add_pscal_executable(hscal) // Lets not build this normally

# Standalone VM binary
set(PSCAL_VM_SOURCES src/vm/vm_main.c)
if(NOT PSCAL_PLATFORM_IOS)
    add_pscal_executable(pscalvm ${PSCAL_VM_SOURCES})
endif()
if(PSCAL_BUILD_STATIC_LIBS)
    pscal_add_static_library(pscal_vm_static
        SOURCES ${PSCAL_VM_SOURCES})
    target_link_libraries(pscal_vm_static PUBLIC pscal_core_static)
endif()

if(BUILD_PSCALD)
    set(DISASSEMBLER_SOURCES src/disassembler/main.c)
    if(NOT PSCAL_PLATFORM_IOS)
        add_pscal_executable(pscald ${DISASSEMBLER_SOURCES})
    endif()
    if(PSCAL_BUILD_STATIC_LIBS)
        pscal_add_static_library(pscal_pscald_static
            SOURCES ${DISASSEMBLER_SOURCES})
        target_link_libraries(pscal_pscald_static PUBLIC pscal_core_static)
    endif()
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
    src/backend_ast/shell.c
)

list(APPEND SHELL_SOURCES
    src/common/ast_shared.c
    src/common/builtin_shared.c
)

list(APPEND SHELL_SOURCES ${EXSH_EXT_BUILTIN_SOURCES})

# On real iOS builds we do not ship CLI entrypoints, but for host-based iOS
# parity testing (PSCAL_FORCE_IOS=ON) we still build exsh so vproc/job-control
# behavior can be exercised on macOS. Skip the executable for Mac Catalyst.
if(NOT PSCAL_PLATFORM_IOS OR (PSCAL_FORCE_IOS AND NOT PSCAL_MACCATALYST))
    add_executable(exsh ${SHELL_SOURCES})
    target_link_libraries(exsh PRIVATE pscal_core_static)
    target_compile_definitions(exsh PRIVATE FRONTEND_SHELL)
    if(PSCAL_HAS_LIBCURL)
        if(TARGET CURL::libcurl)
            target_link_libraries(exsh PRIVATE CURL::libcurl)
        else()
            target_link_libraries(exsh PRIVATE ${CURL_LIBRARIES})
        endif()
    endif()
    target_link_libraries(exsh PRIVATE m Threads::Threads)
    if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
        target_link_libraries(exsh PRIVATE ${PSCAL_SQLITE_LIBS})
    endif()

    if(SDL)
        target_include_directories(exsh PRIVATE
            ${PSCAL_SDL_INCLUDE_DIRS}
            ${PC_SDL_IMAGE_INCLUDE_DIRS}
            ${PC_SDL_MIXER_INCLUDE_DIRS}
            ${PC_SDL_TTF_INCLUDE_DIRS}
        )

        target_link_directories(exsh PRIVATE
            ${PC_SDL_IMAGE_LIBRARY_DIRS}
            ${PC_SDL_MIXER_LIBRARY_DIRS}
            ${PC_SDL_TTF_LIBRARY_DIRS}
        )

        set(EXSH_SDL_LIBS "")
        list(APPEND EXSH_SDL_LIBS ${PSCAL_SDL_CORE_LINK_LIBS})
        list(APPEND EXSH_SDL_LIBS
            ${PC_SDL_IMAGE_LIBRARIES}
            ${PC_SDL_MIXER_LIBRARIES}
            ${PC_SDL_TTF_LIBRARIES}
            ${PC_SDL_IMAGE_LDFLAGS_OTHER}
            ${PC_SDL_MIXER_LDFLAGS_OTHER}
            ${PC_SDL_TTF_LDFLAGS_OTHER}
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
endif()

if(PSCAL_BUILD_STATIC_LIBS)
    pscal_add_static_library(pscal_exsh_static
        FRONTEND FRONTEND_SHELL
        SOURCES ${SHELL_SOURCES})
    target_link_libraries(pscal_exsh_static PUBLIC pscal_core_static)
endif()
if(SMALLCLUE_WITH_EXSH AND NOT TARGET pscal_exsh_static)
    pscal_add_static_library(pscal_exsh_static
        FRONTEND FRONTEND_SHELL
        SOURCES ${SHELL_SOURCES})
    target_link_libraries(pscal_exsh_static PUBLIC pscal_core_static)
endif()

# Tiny C front-end executable
set(CLIKE_SOURCES
    src/clike/main.c
    src/clike/lexer.c
    src/clike/parser.c
    src/clike/ast.c
    src/clike/builtins.c
    src/clike/builtins/thread.c
    src/clike/semantics.c
    src/clike/codegen.c
    src/clike/opt.c
    src/clike/preproc.c
    src/clike/state.c
)

set(CLIKE_REPL_SOURCES ${CLIKE_SOURCES})
list(REMOVE_ITEM CLIKE_REPL_SOURCES src/clike/main.c)
list(APPEND CLIKE_REPL_SOURCES src/clike/repl.c)

if(NOT PSCAL_PLATFORM_IOS)
    add_executable(clike ${CLIKE_SOURCES})
    target_link_libraries(clike PRIVATE pscal_core_static)
    if(PSCAL_HAS_LIBCURL)
        if(TARGET CURL::libcurl)
            target_link_libraries(clike PRIVATE CURL::libcurl)
        else()
            target_link_libraries(clike PRIVATE ${CURL_LIBRARIES})
        endif()
    endif()
    target_link_libraries(clike PRIVATE m Threads::Threads)
    if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
        target_link_libraries(clike PRIVATE ${PSCAL_SQLITE_LIBS})
    endif()

    add_executable(clike-repl ${CLIKE_REPL_SOURCES})
    target_link_libraries(clike-repl PRIVATE pscal_core_static)
    if(PSCAL_HAS_LIBCURL)
        if(TARGET CURL::libcurl)
            target_link_libraries(clike-repl PRIVATE CURL::libcurl)
        else()
            target_link_libraries(clike-repl PRIVATE ${CURL_LIBRARIES})
        endif()
    endif()
    target_link_libraries(clike-repl PRIVATE m Threads::Threads)
    if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
        target_link_libraries(clike-repl PRIVATE ${PSCAL_SQLITE_LIBS})
    endif()

    # When SDL support is enabled, clike needs the same SDL include paths,
    # library directories, and libraries as the other executables to compile
    # headers that reference SDL types.
    if(SDL)
        target_include_directories(clike PRIVATE
            ${PSCAL_SDL_INCLUDE_DIRS}
            ${PC_SDL_IMAGE_INCLUDE_DIRS}
            ${PC_SDL_MIXER_INCLUDE_DIRS}
            ${PC_SDL_TTF_INCLUDE_DIRS}
        )

        target_link_directories(clike PRIVATE
            ${PC_SDL_IMAGE_LIBRARY_DIRS}
            ${PC_SDL_MIXER_LIBRARY_DIRS}
            ${PC_SDL_TTF_LIBRARY_DIRS}
        )

        set(CLIKE_SDL_LIBS "")
        list(APPEND CLIKE_SDL_LIBS ${PSCAL_SDL_CORE_LINK_LIBS})
        list(APPEND CLIKE_SDL_LIBS
            ${PC_SDL_IMAGE_LIBRARIES}
            ${PC_SDL_MIXER_LIBRARIES}
            ${PC_SDL_TTF_LIBRARIES}
            ${PC_SDL_IMAGE_LDFLAGS_OTHER}
            ${PC_SDL_MIXER_LDFLAGS_OTHER}
            ${PC_SDL_TTF_LDFLAGS_OTHER}
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
            ${PSCAL_SDL_INCLUDE_DIRS}
            ${PC_SDL_IMAGE_INCLUDE_DIRS}
            ${PC_SDL_MIXER_INCLUDE_DIRS}
            ${PC_SDL_TTF_INCLUDE_DIRS}
        )

        target_link_directories(clike-repl PRIVATE
            ${PC_SDL_IMAGE_LIBRARY_DIRS}
            ${PC_SDL_MIXER_LIBRARY_DIRS}
            ${PC_SDL_TTF_LIBRARY_DIRS}
        )

        target_link_libraries(clike-repl PRIVATE ${CLIKE_SDL_LIBS})
        target_compile_definitions(clike-repl PRIVATE SDL)

    endif()
endif()

if(PSCAL_BUILD_STATIC_LIBS)
    pscal_add_static_library(pscal_clike_static
        SOURCES ${CLIKE_SOURCES})
    target_link_libraries(pscal_clike_static PUBLIC pscal_core_static)
endif()
if(PSCAL_BUILD_STATIC_LIBS)
    pscal_add_static_library(pscal_clike_repl_static
        FRONTEND FRONTEND_CLIKE
        SOURCES ${CLIKE_REPL_SOURCES})
    target_link_libraries(pscal_clike_repl_static PUBLIC pscal_core_static)
endif()

# Rea front-end executable (placeholder)
set(REA_SOURCES
    src/rea/main.c
    src/rea/lexer.c
    src/rea/parser.c
    src/rea/semantic.c
    src/rea/state.c
    src/rea/builtins/thread.c
    src/rea/type_stubs.c
)

if(NOT PSCAL_PLATFORM_IOS)
    add_executable(rea ${REA_SOURCES})
    target_compile_definitions(rea PRIVATE FRONTEND_REA)
    target_link_libraries(rea PRIVATE pscal_pascal_static)
    if(PSCAL_HAS_LIBCURL)
        if(TARGET CURL::libcurl)
            target_link_libraries(rea PRIVATE CURL::libcurl)
        else()
            target_link_libraries(rea PRIVATE ${CURL_LIBRARIES})
        endif()
    endif()
    target_link_libraries(rea PRIVATE m Threads::Threads)
    if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
        target_link_libraries(rea PRIVATE ${PSCAL_SQLITE_LIBS})
    endif()

    if(SDL)
        target_include_directories(rea PRIVATE
            ${PSCAL_SDL_INCLUDE_DIRS}
            ${PC_SDL_IMAGE_INCLUDE_DIRS}
            ${PC_SDL_MIXER_INCLUDE_DIRS}
            ${PC_SDL_TTF_INCLUDE_DIRS}
        )

        target_link_directories(rea PRIVATE
            ${PC_SDL_IMAGE_LIBRARY_DIRS}
            ${PC_SDL_MIXER_LIBRARY_DIRS}
            ${PC_SDL_TTF_LIBRARY_DIRS}
        )

        set(REA_SDL_LIBS "")
        list(APPEND REA_SDL_LIBS ${PSCAL_SDL_CORE_LINK_LIBS})
        list(APPEND REA_SDL_LIBS
            ${PC_SDL_IMAGE_LIBRARIES}
            ${PC_SDL_MIXER_LIBRARIES}
            ${PC_SDL_TTF_LIBRARIES}
            ${PC_SDL_IMAGE_LDFLAGS_OTHER}
            ${PC_SDL_MIXER_LDFLAGS_OTHER}
            ${PC_SDL_TTF_LDFLAGS_OTHER}
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
endif()

if(PSCAL_BUILD_STATIC_LIBS)
    pscal_add_static_library(pscal_rea_static
        FRONTEND FRONTEND_REA
        SOURCES ${REA_SOURCES})
    target_link_libraries(pscal_rea_static PUBLIC pscal_core_static)
endif()

# ---- Examples ----
if(NOT PSCAL_PLATFORM_IOS)
    add_subdirectory(Examples)

    file(GLOB_RECURSE PSCAL_ALL_EXAMPLE_FILES
        LIST_DIRECTORIES false
        RELATIVE "${CMAKE_SOURCE_DIR}"
        "Examples/*")

    set(PSCAL_CONFIGURED_EXAMPLES)
    foreach(example IN LISTS PSCAL_ALL_EXAMPLE_FILES)
        unset(PSCAL_CONFIG_MARKER)
        file(STRINGS "${CMAKE_SOURCE_DIR}/${example}" PSCAL_CONFIG_MARKER
            REGEX "@PSCAL_INSTALL_ROOT")
        if(PSCAL_CONFIG_MARKER)
            list(APPEND PSCAL_CONFIGURED_EXAMPLES "${example}")
        endif()
    endforeach()

    list(REMOVE_DUPLICATES PSCAL_CONFIGURED_EXAMPLES)
    list(SORT PSCAL_CONFIGURED_EXAMPLES)
    set(PSCAL_CONFIGURED_EXAMPLES_DIR "${CMAKE_BINARY_DIR}/configured_examples")
    set(PSCAL_CONFIGURE_EXAMPLE_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/ConfigureExampleFile.cmake")
    set(PSCAL_CONFIGURED_EXAMPLE_OUTPUTS)
    foreach(example IN LISTS PSCAL_CONFIGURED_EXAMPLES)
        set(src "${CMAKE_SOURCE_DIR}/${example}")
        set(dst "${PSCAL_CONFIGURED_EXAMPLES_DIR}/${example}")
        get_filename_component(dst_dir "${dst}" DIRECTORY)
        add_custom_command(
            OUTPUT "${dst}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${dst_dir}"
            COMMAND ${CMAKE_COMMAND}
                -DINPUT_FILE=${src}
                -DOUTPUT_FILE=${dst}
                -DPSCAL_INSTALL_ROOT_RESOLVED=${PSCAL_INSTALL_ROOT_RESOLVED}
                -P ${PSCAL_CONFIGURE_EXAMPLE_SCRIPT}
            DEPENDS "${src}" "${PSCAL_CONFIGURE_EXAMPLE_SCRIPT}"
            COMMENT "Configuring example ${example}"
            VERBATIM
        )
        list(APPEND PSCAL_CONFIGURED_EXAMPLE_OUTPUTS "${dst}")
    endforeach()
    add_custom_target(configured_examples ALL
        DEPENDS ${PSCAL_CONFIGURED_EXAMPLE_OUTPUTS})
endif()

if(NOT PSCAL_PLATFORM_IOS)
    # ---- optional install ----
    include(GNUInstallDirs)
    set(_pscal_runtime_root "${PSCAL_INSTALL_ROOT_DESTINATION}")

    install(TARGETS pascal pscalvm clike clike-repl rea
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    if(BUILD_DASCAL)
        install(TARGETS dascal
                RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
    if(BUILD_PSCALD)
        install(TARGETS pscald
                RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()

    # Ensure we can replace a running exsh binary (for example when users have it as
    # their login shell) by renaming the existing executable before installing the
    # new build. The backup is overwritten on subsequent installs.
    install(CODE [[
        set(exsh_path "$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}/exsh")
        if(EXISTS "${exsh_path}" OR IS_SYMLINK "${exsh_path}")
            set(exsh_backup "${exsh_path}.previous")
            if(EXISTS "${exsh_backup}" OR IS_SYMLINK "${exsh_backup}")
                file(REMOVE "${exsh_backup}")
            endif()
            message(STATUS "Backing up existing exsh to ${exsh_backup}")
            file(RENAME "${exsh_path}" "${exsh_backup}")
        endif()
    ]])
    install(TARGETS exsh
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

    install(DIRECTORY lib/
            DESTINATION "${_pscal_runtime_root}/lib"
            USE_SOURCE_PERMISSIONS)
    install(DIRECTORY fonts/
            DESTINATION "${_pscal_runtime_root}/fonts"
            USE_SOURCE_PERMISSIONS)
    install(DIRECTORY etc/
            DESTINATION "${_pscal_runtime_root}/etc"
            USE_SOURCE_PERMISSIONS)
    install(DIRECTORY Examples/
            DESTINATION "${_pscal_runtime_root}/Examples"
            USE_SOURCE_PERMISSIONS)
    if(PSCAL_CONFIGURED_EXAMPLES)
        install(CODE [[
            set(_pscal_build_cmd "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target configured_examples)
            if(CMAKE_CONFIGURATION_TYPES)
                list(APPEND _pscal_build_cmd --config "${CMAKE_INSTALL_CONFIG_NAME}")
            endif()
            execute_process(COMMAND ${_pscal_build_cmd}
                RESULT_VARIABLE _pscal_configured_examples_result)
            if(NOT _pscal_configured_examples_result EQUAL 0)
                message(FATAL_ERROR "Failed to generate configured example files (configured_examples target)")
            endif()
        ]])
    endif()
    foreach(example IN LISTS PSCAL_CONFIGURED_EXAMPLES)
        string(REGEX REPLACE "^Examples/" "" example_relative "${example}")
        get_filename_component(example_dir "${example_relative}" DIRECTORY)
        install(FILES "${PSCAL_CONFIGURED_EXAMPLES_DIR}/${example}"
                DESTINATION "${_pscal_runtime_root}/Examples/${example_dir}"
                PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                           GROUP_READ GROUP_EXECUTE
                           WORLD_READ WORLD_EXECUTE)
    endforeach()
    install(DIRECTORY Docs/
            DESTINATION "${_pscal_runtime_root}/docs")
    # Tiny compiler runtime assets for iOS/iPadOS: install script plus minimal headers it loads.
    install(PROGRAMS bin/tiny
            DESTINATION "${_pscal_runtime_root}/bin"
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                        GROUP_READ GROUP_EXECUTE
                        WORLD_READ WORLD_EXECUTE)
    install(FILES src/compiler/bytecode.h
            DESTINATION "${_pscal_runtime_root}/src/compiler")
    install(FILES src/core/version.h
            DESTINATION "${_pscal_runtime_root}/src/core")
    install(DIRECTORY Tests/libs/
            DESTINATION "${_pscal_runtime_root}/etc/tests"
            USE_SOURCE_PERMISSIONS)
    install(DIRECTORY lib/rea/
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/rea"
            USE_SOURCE_PERMISSIONS)

    install(CODE [[
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
    ]])
endif()

if(NOT PSCAL_PLATFORM_IOS)
    add_test(NAME pascal_tests COMMAND bash ${CMAKE_SOURCE_DIR}/Tests/run_pascal_tests.sh)
    add_test(NAME clike_tests COMMAND bash ${CMAKE_SOURCE_DIR}/Tests/run_clike_tests.sh)
    add_test(NAME rea_tests COMMAND bash ${CMAKE_SOURCE_DIR}/Tests/run_rea_tests.sh)
    set_tests_properties(rea_tests PROPERTIES ENVIRONMENT "REA_SKIP_TESTS=constructor_init field_access_assign field_access_read method_this_assign new_alloc new_assign_ptr")
    add_test(NAME json2bc_tests COMMAND bash ${CMAKE_SOURCE_DIR}/Tests/run_json2bc_tests.sh)
endif()

# ---- AST JSON to Bytecode tool ----
set(JSON2BC_SOURCES
    src/tools/json2bc.c
    src/tools/ast_json_loader.c
    src/rea/type_stubs.c
)
if(NOT PSCAL_PLATFORM_IOS)
    add_executable(pscaljson2bc ${JSON2BC_SOURCES})
    target_link_libraries(pscaljson2bc PRIVATE pscal_core_static)
    if(PSCAL_HAS_LIBCURL)
        if(TARGET CURL::libcurl)
            target_link_libraries(pscaljson2bc PRIVATE CURL::libcurl)
        else()
            target_link_libraries(pscaljson2bc PRIVATE ${CURL_LIBRARIES})
        endif()
    endif()
    target_link_libraries(pscaljson2bc PRIVATE m Threads::Threads)
    if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
        target_link_libraries(pscaljson2bc PRIVATE ${PSCAL_SQLITE_LIBS})
    endif()
    if(SDL)
        target_include_directories(pscaljson2bc PRIVATE
            ${PSCAL_SDL_INCLUDE_DIRS}
            ${PC_SDL_IMAGE_INCLUDE_DIRS}
            ${PC_SDL_MIXER_INCLUDE_DIRS}
            ${PC_SDL_TTF_INCLUDE_DIRS}
        )
        target_link_directories(pscaljson2bc PRIVATE
            ${PC_SDL_IMAGE_LIBRARY_DIRS}
            ${PC_SDL_MIXER_LIBRARY_DIRS}
            ${PC_SDL_TTF_LIBRARY_DIRS}
        )
        set(JSON2BC_SDL_LIBS "")
        list(APPEND JSON2BC_SDL_LIBS ${PSCAL_SDL_CORE_LINK_LIBS})
        list(APPEND JSON2BC_SDL_LIBS
            ${PC_SDL_IMAGE_LIBRARIES}
            ${PC_SDL_MIXER_LIBRARIES}
            ${PC_SDL_TTF_LIBRARIES}
            ${PC_SDL_IMAGE_LDFLAGS_OTHER}
            ${PC_SDL_MIXER_LDFLAGS_OTHER}
            ${PC_SDL_TTF_LDFLAGS_OTHER}
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
endif()

if(PSCAL_BUILD_STATIC_LIBS)
    pscal_add_static_library(pscal_json2bc_static
        SOURCES ${JSON2BC_SOURCES})
    target_link_libraries(pscal_json2bc_static PUBLIC pscal_core_static)
endif()

# Host-based iOS parity builds link tool entrypoints directly into exsh so
# `pascal`, `clike`, `rea`, etc. can run without spawning external processes.
if(TARGET exsh AND PSCAL_FORCE_IOS)
    if(TARGET pscal_pascal_static)
        target_link_libraries(exsh PRIVATE pscal_pascal_static)
    endif()
    if(TARGET pscal_vm_static)
        target_link_libraries(exsh PRIVATE pscal_vm_static)
    endif()
    if(TARGET pscal_pscald_static)
        target_link_libraries(exsh PRIVATE pscal_pscald_static)
    endif()
    if(TARGET pscal_json2bc_static)
        target_link_libraries(exsh PRIVATE pscal_json2bc_static)
    endif()
    if(TARGET pscal_clike_static)
        target_link_libraries(exsh PRIVATE pscal_clike_static)
    endif()
    if(TARGET pscal_rea_static)
        target_link_libraries(exsh PRIVATE pscal_rea_static)
    endif()
endif()

# Optional smallclue extras
option(SMALLCLUE_WITH_EXSH "Enable the exsh front end as the smallclue 'sh' applet" OFF)
if(NOT PSCAL_PLATFORM_IOS AND SMALLCLUE_WITH_EXSH)
    message(WARNING "SMALLCLUE_WITH_EXSH is ignored on non-iOS builds; disabling for host toolchains.")
    set(SMALLCLUE_WITH_EXSH OFF CACHE BOOL "Enable the exsh front end as the smallclue 'sh' applet" FORCE)
endif()
if(PSCAL_PLATFORM_IOS AND SMALLCLUE_WITH_EXSH)
    target_compile_definitions(pscal_core_static PUBLIC SMALLCLUE_WITH_EXSH)
endif()

if(PSCAL_PLATFORM_IOS)
    add_executable(smallclue
        src/smallclue/src/main.c
    )
    set(_SMALLCLUE_LINK_LIBS m Threads::Threads)
    if(PSCAL_HAS_LIBCURL AND TARGET CURL::libcurl)
        list(APPEND _SMALLCLUE_LINK_LIBS CURL::libcurl)
    endif()
    if(SMALLCLUE_WITH_EXSH AND TARGET pscal_exsh_static)
        list(APPEND _SMALLCLUE_LINK_LIBS pscal_exsh_static)
        target_compile_definitions(smallclue PRIVATE SMALLCLUE_WITH_EXSH)
    elseif(SMALLCLUE_WITH_EXSH AND NOT TARGET pscal_exsh_static)
        message(FATAL_ERROR "SMALLCLUE_WITH_EXSH=ON but pscal_exsh_static is not available. Enable PSCAL_BUILD_STATIC_LIBS or ensure exsh static target is built.")
    else()
        list(APPEND _SMALLCLUE_LINK_LIBS pscal_core_static)
    endif()
    if(ENABLE_EXT_BUILTIN_SQLITE AND PSCAL_SQLITE_LIBS)
        list(APPEND _SMALLCLUE_LINK_LIBS ${PSCAL_SQLITE_LIBS})
    endif()
    if(SDL)
        target_link_directories(smallclue PRIVATE
            ${PC_SDL_IMAGE_LIBRARY_DIRS}
            ${PC_SDL_MIXER_LIBRARY_DIRS}
            ${PC_SDL_TTF_LIBRARY_DIRS}
        )
        list(APPEND _SMALLCLUE_LINK_LIBS ${PSCAL_SDL_CORE_LINK_LIBS})
        list(APPEND _SMALLCLUE_LINK_LIBS
            ${PC_SDL_IMAGE_LIBRARIES}
            ${PC_SDL_MIXER_LIBRARIES}
            ${PC_SDL_TTF_LIBRARIES}
            ${PC_SDL_IMAGE_LDFLAGS_OTHER}
            ${PC_SDL_MIXER_LDFLAGS_OTHER}
            ${PC_SDL_TTF_LDFLAGS_OTHER}
            ${PSCAL_SDL_PLATFORM_LIBS}
        )
        if(PSCAL_OPENGL_TARGET)
            list(APPEND _SMALLCLUE_LINK_LIBS ${PSCAL_OPENGL_TARGET})
        elseif(OPENGL_LIBRARIES)
            list(APPEND _SMALLCLUE_LINK_LIBS ${OPENGL_LIBRARIES})
        endif()
    endif()
    list(REMOVE_DUPLICATES _SMALLCLUE_LINK_LIBS)
    target_link_libraries(smallclue PRIVATE ${_SMALLCLUE_LINK_LIBS})
endif()

if(PSCAL_PLATFORM_IOS)
    add_executable(pscal_tool_runner src/ios/tool_runner_main.c)
    set(_tool_runner_libs
        pscal_clike_static
        pscal_rea_static
        pscal_pascal_static
        pscal_vm_static
        pscal_json2bc_static
        pscal_core_static
        pscal_common_frontend_shim
    )
    if(TARGET pscal_dascal_static)
        list(APPEND _tool_runner_libs pscal_dascal_static)
    endif()
    if(TARGET pscal_pscald_static)
        list(APPEND _tool_runner_libs pscal_pscald_static)
    endif()
    list(REMOVE_DUPLICATES _tool_runner_libs)
    set(_tool_runner_sdl_libs ${PSCAL_SDL_CORE_LINK_LIBS})
    list(APPEND _tool_runner_sdl_libs
        ${PC_SDL_IMAGE_LIBRARIES}
        ${PC_SDL_MIXER_LIBRARIES}
        ${PC_SDL_TTF_LIBRARIES}
        ${PC_SDL_IMAGE_LDFLAGS_OTHER}
        ${PC_SDL_MIXER_LDFLAGS_OTHER}
        ${PC_SDL_TTF_LDFLAGS_OTHER}
        ${PSCAL_SDL_PLATFORM_LIBS}
    )
    if(PSCAL_OPENGL_TARGET)
        list(APPEND _tool_runner_sdl_libs ${PSCAL_OPENGL_TARGET})
    elseif(OPENGL_LIBRARIES)
        list(APPEND _tool_runner_sdl_libs ${OPENGL_LIBRARIES})
    endif()
    list(REMOVE_DUPLICATES _tool_runner_sdl_libs)
    target_link_libraries(pscal_tool_runner PRIVATE ${_tool_runner_libs} ${_tool_runner_sdl_libs} "-lsqlite3")
    target_link_options(pscal_tool_runner PRIVATE "-Wl,-no_warn_duplicate_libraries")
endif()

# Install the JSON→Bytecode tool alongside other binaries
if(NOT PSCAL_PLATFORM_IOS)
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
endif()

if(PSCAL_PLATFORM_IOS)
endif()
