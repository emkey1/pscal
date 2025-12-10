cmake_minimum_required(VERSION 3.16)
project(smallclue_standalone C)

# Path to the PSCAL repository root. Adjust when using this CMake outside the
# tree (e.g. -DPSCALSRC=/path/to/PBuild).
set(PSCALSRC "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "Path to PSCAL source tree")
# Path to a build tree that already produced libpscal_core_static.a (and
# optionally libpscal_exsh_static.a if you want sh support).
set(PSCAL_BUILD_ROOT "${PSCALSRC}/build" CACHE PATH "Path to PSCAL build products")

option(SMALLCLUE_WITH_EXSH "Enable the exsh front end as the smallclue 'sh' applet" OFF)

# Locate the prebuilt core library.
find_library(PSC_CORE_LIB NAMES pscal_core_static PATHS "${PSCAL_BUILD_ROOT}" NO_DEFAULT_PATH)
if(NOT PSC_CORE_LIB)
    message(FATAL_ERROR "libpscal_core_static.a not found under ${PSCAL_BUILD_ROOT}")
endif()

# Optional exsh static lib (needed for SMALLCLUE_WITH_EXSH).
if(SMALLCLUE_WITH_EXSH)
    find_library(PSC_EXSH_LIB NAMES pscal_exsh_static PATHS "${PSCAL_BUILD_ROOT}" NO_DEFAULT_PATH)
    if(NOT PSC_EXSH_LIB)
        message(FATAL_ERROR "SMALLCLUE_WITH_EXSH=ON but libpscal_exsh_static.a not found under ${PSCAL_BUILD_ROOT}")
    endif()
endif()

# smallclue main source
add_executable(smallclue_standalone
    "${PSCALSRC}/src/smallclue/src/main.c"
)

target_include_directories(smallclue_standalone PRIVATE
    "${PSCALSRC}/src"
    "${PSCALSRC}/src/smallclue/src"
    "${PSCAL_BUILD_ROOT}/generated"
)

target_link_libraries(smallclue_standalone PRIVATE "${PSC_CORE_LIB}")

if(SMALLCLUE_WITH_EXSH)
    target_compile_definitions(smallclue_standalone PRIVATE SMALLCLUE_WITH_EXSH)
    target_link_libraries(smallclue_standalone PRIVATE "${PSC_EXSH_LIB}")
    message(WARNING "Ensure libpscal_core_static.a was built with SMALLCLUE_WITH_EXSH=ON so the 'sh' applet is included.")
endif()

find_package(Threads REQUIRED)
target_link_libraries(smallclue_standalone PRIVATE Threads::Threads m)

# Optionally pick up libcurl if it is not embedded in the static libs.
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(smallclue_standalone PRIVATE CURL::libcurl)
endif()

message(STATUS "Building smallclue_standalone with SMALLCLUE_WITH_EXSH=${SMALLCLUE_WITH_EXSH}")
