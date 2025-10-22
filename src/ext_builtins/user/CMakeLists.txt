# Sources for user-defined extended builtins
set(EXT_BUILTIN_USER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/register.c
    ${CMAKE_CURRENT_SOURCE_DIR}/landscape.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sierpinski_threads.c
)
set(EXT_BUILTIN_USER_SOURCES ${EXT_BUILTIN_USER_SOURCES} PARENT_SCOPE)
