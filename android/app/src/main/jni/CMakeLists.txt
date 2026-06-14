#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# CMakeLists.txt — NDK build for camex + JNI bridge
#
cmake_minimum_required(VERSION 3.22)
project("camex" C)

# Collect all camex sources (relative to android/app/src/main/jni/)
# 5 levels up = repo root
set(CAMEX_SRCS
    ../../../../../main.c
    ../../../../../camex.c
    ../../../../../monocypher.c
    ../../../../../util.c
    ../../../../../log.c
    ../../../../../crypto.c
    ../../../../../tun.c
    ../../../../../net.c
    ../../../../../proto.c
    ../../../../../config.c
    ../../../../../client.c
    ../../../../../server.c
)

# Build the shared library
add_library(camex SHARED
    camex-jni.c
    ${CAMEX_SRCS}
)

target_compile_definitions(camex PRIVATE
    -D_GNU_SOURCE
    -D_POSIX_C_SOURCE=200809L
)

target_include_directories(camex PRIVATE
    ../../../../../../   # camex headers
)

target_link_libraries(camex log)
