# Cross toolchain for jcmvbkbc's ESP32-S3 FDPIC Linux userspace.
# Pass -DXTENSA_TOOLCHAIN_ROOT=/absolute/path/to/the/crosstool-NG/prefix and
# export XTENSA_GNU_CONFIG=/absolute/path/to/xtensa-dynconfig/esp32s3.so.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR xtensa)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XTENSA_TOOLCHAIN_ROOT)

if(NOT DEFINED XTENSA_TOOLCHAIN_ROOT)
  message(FATAL_ERROR "XTENSA_TOOLCHAIN_ROOT is required")
endif()
get_filename_component(XTENSA_TOOLCHAIN_ROOT "${XTENSA_TOOLCHAIN_ROOT}" ABSOLUTE)

set(_xtensa_triplet xtensa-esp32s3-linux-uclibcfdpic)
set(_xtensa_bin "${XTENSA_TOOLCHAIN_ROOT}/bin/${_xtensa_triplet}")
set(_xtensa_sysroot
    "${XTENSA_TOOLCHAIN_ROOT}/${_xtensa_triplet}/sysroot")

set(CMAKE_C_COMPILER "${_xtensa_bin}-gcc")
set(CMAKE_AR "${_xtensa_bin}-ar")
set(CMAKE_RANLIB "${_xtensa_bin}-ranlib")
set(CMAKE_STRIP "${_xtensa_bin}-strip")
set(CMAKE_SYSROOT "${_xtensa_sysroot}")
set(CMAKE_FIND_ROOT_PATH "${_xtensa_sysroot}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
