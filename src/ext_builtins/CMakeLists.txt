# CMake configuration for extended builtins

# Options to control which categories of extended builtins are compiled.
option(ENABLE_EXT_BUILTIN_MATH "Enable extended math builtins" ON)
option(ENABLE_EXT_BUILTIN_STRINGS "Enable extended string builtins" ON)
option(ENABLE_EXT_BUILTIN_SYSTEM "Enable extended system builtins" ON)
option(ENABLE_EXT_BUILTIN_USER "Enable user-provided builtins" ON)
option(ENABLE_EXT_BUILTIN_YYJSON "Enable extended yyjson builtins" ON)
option(ENABLE_EXT_BUILTIN_SQLITE "Enable extended sqlite builtins" ON)
option(ENABLE_EXT_BUILTIN_3D "Enable extended 3D builtins" ON)
option(ENABLE_EXT_BUILTIN_GRAPHICS "Enable extended graphics builtins" ON)
option(ENABLE_EXT_BUILTIN_OPENAI "Enable extended OpenAI builtins" ON)

# Base sources that are always built
set(PSCAL_EXT_BUILTIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/register.c
    ${CMAKE_CURRENT_SOURCE_DIR}/registry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/query_builtin.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dump.c
)

# Conditionally add builtin categories
if(ENABLE_EXT_BUILTIN_MATH)
    add_subdirectory(math)
    list(APPEND PSCAL_EXT_BUILTIN_SOURCES ${EXT_BUILTIN_MATH_SOURCES})
endif()

if(ENABLE_EXT_BUILTIN_STRINGS)
    add_subdirectory(strings)
    list(APPEND PSCAL_EXT_BUILTIN_SOURCES ${EXT_BUILTIN_STRINGS_SOURCES})
endif()

if(ENABLE_EXT_BUILTIN_SYSTEM)
    add_subdirectory(system)
    list(APPEND PSCAL_EXT_BUILTIN_SOURCES ${EXT_BUILTIN_SYSTEM_SOURCES})
endif()

if(ENABLE_EXT_BUILTIN_YYJSON)
    add_subdirectory(yyjson)
    list(APPEND PSCAL_EXT_BUILTIN_SOURCES ${EXT_BUILTIN_YYJSON_SOURCES})
endif()

if(ENABLE_EXT_BUILTIN_SQLITE)
    add_subdirectory(sqlite)
    list(APPEND PSCAL_EXT_BUILTIN_SOURCES ${EXT_BUILTIN_SQLITE_SOURCES})
endif()

if(ENABLE_EXT_BUILTIN_USER)
    add_subdirectory(user)
    list(APPEND PSCAL_EXT_BUILTIN_SOURCES ${EXT_BUILTIN_USER_SOURCES})
endif()

if(ENABLE_EXT_BUILTIN_3D)
    add_subdirectory(threed)
    list(APPEND PSCAL_EXT_BUILTIN_SOURCES ${EXT_BUILTIN_THREED_SOURCES})
endif()

if(ENABLE_EXT_BUILTIN_GRAPHICS)
    add_subdirectory(graphics)
    list(APPEND PSCAL_EXT_BUILTIN_SOURCES ${EXT_BUILTIN_GRAPHICS_SOURCES})
endif()

if(ENABLE_EXT_BUILTIN_OPENAI)
    add_subdirectory(openai)
    list(APPEND PSCAL_EXT_BUILTIN_SOURCES ${EXT_BUILTIN_OPENAI_SOURCES})
endif()

# Expose sources to parent scope
set(PSCAL_EXT_BUILTIN_SOURCES ${PSCAL_EXT_BUILTIN_SOURCES} PARENT_SCOPE)
