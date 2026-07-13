# CMake toolchain for the goggle target: aarch64, fully static.
#
# The HUD links no vendor .so's (it reaches the hardware via DRM ioctls, evdev and sockets), so
# unlike libre it builds fully static and needs no device sysroot. The static binary is independent
# of the device glibc.
#
#   cmake -S ml-hud -B ml-hud/build -DCMAKE_TOOLCHAIN_FILE=ml-hud/cmake/aarch64-static.cmake
#   cmake --build ml-hud/build
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED AR_CC)
    set(AR_CC aarch64-linux-gnu-gcc)
endif()
set(CMAKE_C_COMPILER ${AR_CC})

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
