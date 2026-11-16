#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# CMakeLists.txt — NDK build for camex + JNI bridge
#
cmake_minimum_required(VERSION 3.22)
project("camex" C)

# The camex sources, shared with the tunnel library (#170)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../../camex-sources.cmake)

# Build the shared library
add_library(camex SHARED
    camex-jni.c
    ${CAMEX_SRCS}
)

target_compile_definitions(camex PRIVATE
    -D_GNU_SOURCE
    -D_POSIX_C_SOURCE=200809L
)

target_include_directories(camex PRIVATE ${CAMEX_SRC_DIR})

target_link_libraries(camex log)
