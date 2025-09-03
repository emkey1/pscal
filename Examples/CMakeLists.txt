cmake_minimum_required(VERSION 3.24)
project(pscal_examples NONE)

# Path to the pascal interpreter built by main project
set(PASCAL_BIN ${CMAKE_BINARY_DIR}/bin/pascal)

# Basic example target
add_custom_target(run_basic_examples
    COMMAND ${CMAKE_COMMAND} -E echo "Running basic examples..."
    COMMAND ${PASCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/AreaCalculation.p
    COMMAND ${PASCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/DosExample.p
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# Procedure pointers + threads demo
add_custom_target(run_threads_procptr_demo
    COMMAND ${CMAKE_COMMAND} -E echo "Running ThreadsProcPtrDemo..."
    COMMAND ${PASCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/Pascal/ThreadsProcPtrDemo
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# HTTP fetch demo (sync)
add_custom_target(run_http_fetch_demo
    COMMAND ${CMAKE_COMMAND} -E echo "Running HttpFetchDemo..."
    COMMAND ${PASCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/Pascal/HttpFetchDemo
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# SDL examples are only run when SDL option is enabled
if(SDL)
    add_custom_target(run_sdl_examples
        COMMAND ${CMAKE_COMMAND} -E echo "Running SDL examples..."
        COMMAND ${PASCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/SDLSimplePong
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
else()
    add_custom_target(run_sdl_examples
        COMMAND ${CMAKE_COMMAND} -E echo "Skipping SDL examples: SDL option is OFF"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()
