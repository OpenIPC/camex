#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# CMakeLists.txt — libcamex.so of the tunnel library (#170): camex itself, as the app builds
# it, and the library's JNI (src/main/cpp/camex-jni.c)
#
cmake_minimum_required(VERSION 3.22)
project(camex C)

include(${CMAKE_CURRENT_SOURCE_DIR}/../camex-sources.cmake)

add_library(camex SHARED src/main/cpp/camex-jni.c ${CAMEX_SRCS})
target_compile_definitions(camex PRIVATE _GNU_SOURCE _POSIX_C_SOURCE=200809L)
target_include_directories(camex PRIVATE ${CAMEX_SRC_DIR})
target_link_libraries(camex log)
# 16 KB pages (Android 15 devices)
target_link_options(camex PRIVATE -Wl,-z,max-page-size=16384)

# The app stays inside its own VPN (it reaches the other clients through it): every socket()
# call of camex goes through __wrap_socket (src/main/cpp/camex-jni.c), which protects the
# socket from the VPN
target_link_options(camex PRIVATE -Wl,--wrap=socket)
