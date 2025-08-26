# Sources for math extended builtins
set(EXT_BUILTIN_MATH_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/chudnovsky.c
    ${CMAKE_CURRENT_SOURCE_DIR}/factorial.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fibonacci.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mandelbrot.c
    ${CMAKE_CURRENT_SOURCE_DIR}/register.c
)
set(EXT_BUILTIN_MATH_SOURCES ${EXT_BUILTIN_MATH_SOURCES} PARENT_SCOPE)
