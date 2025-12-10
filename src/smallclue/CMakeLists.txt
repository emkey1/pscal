cmake_minimum_required(VERSION 3.10)

# Project Definition
project(smallclue C)

# Set C Standard (C99 or C11 recommended)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # Enable compiler-specific extensions (useful for POSIX/BSD functions)

# Options for build configuration
option(ENABLE_CURL "Enable curl support (requires libcurl)" ON)
option(ENABLE_IOS_TARGET "Enable iOS specific build flags (PSCAL_TARGET_IOS)" OFF)
option(WITH_EXSH "Include exsh shell integration" OFF)

# Add preprocessor definitions based on system
if(APPLE)
    add_definitions(-D__APPLE__)
    # For compatibility with Apple's availability macros if needed
    add_definitions(-D_DARWIN_C_SOURCE)
elseif(UNIX AND NOT APPLE)
    add_definitions(-D_POSIX_C_SOURCE=200809L)
    add_definitions(-D_XOPEN_SOURCE=700)
    add_definitions(-D_GNU_SOURCE) # For older glibc
endif()

if(ENABLE_IOS_TARGET)
    add_definitions(-DPSCAL_TARGET_IOS)
endif()

# Find dependencies
find_package(Threads REQUIRED)

if(ENABLE_CURL)
    find_package(CURL)
    if(CURL_FOUND)
        add_definitions(-DPSCAL_HAS_LIBCURL)
        include_directories(${CURL_INCLUDE_DIRS})
    else()
        message(WARNING "CURL not found. Building without curl support.")
        set(ENABLE_CURL OFF)
    endif()
endif()

# Include directories
# Assuming directory structure:
#   CMakeLists.txt
#   src/
#   common/ (required by core.c includes)
#   third-party/ (required by openssh_app.c)
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/nextvi
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/ios_system/ssh_keygen
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/ios_system/ssh_keygen/openbsd-compat
)

# Source files
set(NEXTVI_SOURCES third-party/nextvi/vi.c)

file(GLOB_RECURSE OPENSSH_SOURCES CONFIGURE_DEPENDS
    third-party/ios_system/ssh_keygen/*.c
)
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "openbsd-compat/regress/")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/ssh-agent\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/ssh-add\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/auth2-gss\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/gss-genr\\.c$")
# Drop server/auth-only sources; keep client pieces like authfd/authfile.
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/auth2-.*\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/auth-(passwd|rhosts).*\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/monitor.*\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/sshd.*\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/servconf\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/session\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/sandbox-.*\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/ssh-keysign\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/ssh-pkcs11-helper\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/sftp-server\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/sshsig\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/platform-pledge\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/platform-tracing\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/getrrsetbyname.*\\.c$")
list(FILTER OPENSSH_SOURCES EXCLUDE REGEX "/ssh-sk-client\\.c$")

# Re-add client helpers that were filtered out by broad patterns.
list(APPEND OPENSSH_SOURCES
    third-party/ios_system/ssh_keygen/monitor_fdpass.c
    third-party/ios_system/ssh_keygen/sshsig.c
    third-party/ios_system/ssh_keygen/platform-pledge.c
)

set(SOURCES
    src/main.c
    src/core.c
    src/nextvi_app.c
    src/openssh_app.c
    src/runtime_support.c
    src/openssh_stubs.c
    src/openssh_shim.c
    ${NEXTVI_SOURCES}
    ${OPENSSH_SOURCES}
)

set_source_files_properties(${NEXTVI_SOURCES} PROPERTIES COMPILE_DEFINITIONS "main=nextvi_main_entry")
set_source_files_properties(third-party/ios_system/ssh_keygen/ssh.c PROPERTIES COMPILE_DEFINITIONS "ssh_main=pscal_openssh_ssh_main")
set_source_files_properties(third-party/ios_system/ssh_keygen/scp.c PROPERTIES COMPILE_DEFINITIONS "scp_main=pscal_openssh_scp_main")
set_source_files_properties(third-party/ios_system/ssh_keygen/sftp.c PROPERTIES COMPILE_DEFINITIONS "sftp_main=pscal_openssh_sftp_main")
set_source_files_properties(third-party/ios_system/ssh_keygen/ssh-keygen.c PROPERTIES COMPILE_DEFINITIONS "sshkeygen_main=pscal_openssh_ssh_keygen_main")

add_compile_definitions(HAVE_CONFIG_H)

# Optional Integration source
# src/integration.c depends on vm/vm.h and backend_ast/builtin.h
# Only include if building with the full shell environment
if(WITH_EXSH)
    add_definitions(-DSMALLCLUE_WITH_EXSH)
    list(APPEND SOURCES src/integration.c)
    # You might need to add include paths for vm/ and backend_ast/ here
    # include_directories(${CMAKE_CURRENT_SOURCE_DIR}/vm)
endif()

# External Dependency Placeholders
# smallclue references symbols from OpenSSH (pscal_openssh_*) and Nextvi (nextvi_main_entry).
# These sources are NOT in the files you uploaded.
# You must add the source files for these libraries to the target, or link against them.
# Example:
# list(APPEND SOURCES third-party/nextvi/nextvi.c)
# list(APPEND SOURCES third-party/openssh/ssh.c ...)

# Define the Executable
add_executable(smallclue ${SOURCES})

# Link Libraries
target_link_libraries(smallclue PRIVATE Threads::Threads)

if(ENABLE_CURL)
    target_link_libraries(smallclue PRIVATE ${CURL_LIBRARIES})
endif()

if(APPLE)
    target_link_libraries(smallclue PRIVATE resolv)
endif()

# Link OpenSSH dependencies
find_package(ZLIB REQUIRED)
target_link_libraries(smallclue PRIVATE ZLIB::ZLIB)
find_package(OpenSSL REQUIRED)
target_link_libraries(smallclue PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if(APPLE)
    # Link against CoreFoundation or specific frameworks if iOS target functions require them
    # target_link_libraries(smallclue PRIVATE "-framework Foundation")
endif()

# Install instructions (optional)
install(TARGETS smallclue DESTINATION bin)
