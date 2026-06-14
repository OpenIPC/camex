#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# CMakeLists.txt — NDK build for camex + JNI bridge
#
cmake_minimum_required(VERSION 3.22)
project("camex" C)

# Collect all camex sources (relative to android/app/src/main/jni/)
# 5 levels up = repo root/src
set(CAMEX_SRCS
    ../../../../../src/main.c
    ../../../../../src/camex.c
    ../../../../../src/monocypher.c
    ../../../../../src/util.c
    ../../../../../src/log.c
    ../../../../../src/crypto.c
    ../../../../../src/tun.c
    ../../../../../src/net.c
    ../../../../../src/proto.c
    ../../../../../src/config.c
    ../../../../../src/client.c
    ../../../../../src/server.c
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
    ../../../../../../src   # camex headers
)

target_link_libraries(camex log)
