cmake_minimum_required(VERSION 3.18)
project(pscal_examples NONE)

# Path to the pascal interpreter built by main project
set(PASCAL_BIN ${CMAKE_BINARY_DIR}/bin/pascal)
# Path to the Rea front end built by main project
set(REA_BIN ${CMAKE_BINARY_DIR}/bin/rea)

# Basic example target
add_custom_target(run_basic_examples
    COMMAND ${CMAKE_COMMAND} -E echo "Running basic examples..."
    COMMAND ${PASCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/pascal/base/AreaCalculation
    COMMAND ${PASCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/pascal/base/DosExample
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# Procedure pointers + threads demo
add_custom_target(run_threads_procptr_demo
    COMMAND ${CMAKE_COMMAND} -E echo "Running ThreadsProcPtrDemo..."
    COMMAND ${PASCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/pascal/base/ThreadsProcPtrDemo
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# HTTP fetch demo (sync)
add_custom_target(run_http_fetch_demo
    COMMAND ${CMAKE_COMMAND} -E echo "Running HttpFetchDemo..."
    COMMAND ${PASCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/pascal/base/HttpFetchDemo
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# Rea example target
add_custom_target(run_rea_examples
    COMMAND ${CMAKE_COMMAND} -E echo "Running Rea examples..."
    COMMAND ${REA_BIN} --dump-bytecode-only ${CMAKE_CURRENT_SOURCE_DIR}/rea/base/hello
    COMMAND ${REA_BIN} --dump-bytecode-only ${CMAKE_CURRENT_SOURCE_DIR}/rea/base/showcase
    COMMAND ${REA_BIN} --dump-bytecode-only ${CMAKE_CURRENT_SOURCE_DIR}/rea/base/method_demo
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# Executing CRT demo (requires unit linking via #import)
add_custom_target(run_rea_crt_demo
    COMMAND ${CMAKE_COMMAND} -E echo "Running Rea CRT demo (executing)..."
    COMMAND ${REA_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/rea/base/crt_demo
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# SDL examples are only run when SDL option is enabled
if(SDL)
    add_custom_target(run_sdl_examples
        COMMAND ${CMAKE_COMMAND} -E echo "Running SDL examples..."
        COMMAND ${PASCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/pascal/sdl/RuntimeCameraDemo
        COMMAND ${PASCAL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/pascal/sdl/SoundSimplePong
        COMMAND ${REA_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/rea/sdl/multibouncingballs
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
else()
    add_custom_target(run_sdl_examples
        COMMAND ${CMAKE_COMMAND} -E echo "Skipping SDL examples: SDL option is OFF"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()
