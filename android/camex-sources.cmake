#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# camex-sources.cmake — the camex sources of the Android builds: included by the app
# (app/src/main/jni/CMakeLists.txt) and by the tunnel library (tunnel/CMakeLists.txt),
# so a new source file is added in one place. Sets CAMEX_SRC_DIR and CAMEX_SRCS.
#
get_filename_component(CAMEX_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)

set(CAMEX_SRCS)
foreach(name main camex monocypher util log crypto noise keys session slotidx tun packet
        net proto config client server control nginxmap)
    list(APPEND CAMEX_SRCS "${CAMEX_SRC_DIR}/${name}.c")
endforeach()
