cmake_minimum_required(VERSION 3.24)
project(pscal_examples NONE)

# Path to the pscal interpreter built by main project
set(PSCAL_BIN ${CMAKE_BINARY_DIR}/bin/pscal)

# Basic example target
add_custom_target(run_basic_examples
    COMMAND ${CMAKE_COMMAND} -E echo "Running basic examples..."
    COMMAND ${PSCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/AreaCalculation.p
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# SDL examples are only run when SDL option is enabled
if(SDL)
    add_custom_target(run_sdl_examples
        COMMAND ${CMAKE_COMMAND} -E echo "Running SDL examples..."
        COMMAND ${PSCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/SDLSimplePong
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
else()
    add_custom_target(run_sdl_examples
        COMMAND ${CMAKE_COMMAND} -E echo "Skipping SDL examples: SDL option is OFF"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()
