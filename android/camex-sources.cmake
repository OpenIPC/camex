#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# camex-sources.cmake — the camex sources of the Android build: included by the tunnel
# library (tunnel/CMakeLists.txt), which the app uses too (#172), and by apps that build
# their own JNI over camex. Sets CAMEX_SRC_DIR and CAMEX_SRCS.
#
get_filename_component(CAMEX_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)

set(CAMEX_SRCS)
foreach(name main camex monocypher util log crypto noise keys session slotidx tun packet
        net proto config client server control nginxmap tcpgate netw pake offload)
    list(APPEND CAMEX_SRCS "${CAMEX_SRC_DIR}/${name}.c")
endforeach()
