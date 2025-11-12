# PSCAL iOS toolchain
#
# This file configures CMake to target iOS (device or simulator) using the
# Apple clang toolchain shipped with Xcode. It intentionally keeps the logic
# lightweight so it can be used by multiple presets.

if(NOT DEFINED PSCALI_IOS_PLATFORM)
    set(PSCALI_IOS_PLATFORM "SIMULATOR")
endif()

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_VERSION 17)
set(CMAKE_IOS_INSTALL_COMBINED YES)

if(PSCALI_IOS_PLATFORM STREQUAL "DEVICE")
    set(CMAKE_OSX_SYSROOT "iphoneos")
    set(CMAKE_OSX_ARCHITECTURES "arm64")
    set(CMAKE_SYSTEM_PROCESSOR "arm64")
else()
    set(CMAKE_OSX_SYSROOT "iphonesimulator")
    set(CMAKE_OSX_ARCHITECTURES "arm64")
    set(CMAKE_SYSTEM_PROCESSOR "arm64")
endif()

set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "" FORCE)

message(STATUS "Configuring PSCAL for iOS (${PSCALI_IOS_PLATFORM}) using sysroot=${CMAKE_OSX_SYSROOT}")
