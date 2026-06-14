#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# Kbuild — kernel module build rules for camex.ko
#

# Inject version from git (falls back to the #define in version.h)
CAMEX_GIT_VER := $(shell git -C $(src) describe --tags --always --dirty 2>/dev/null)
ifneq ($(CAMEX_GIT_VER),)
ccflags-y += -DCAMEX_VERSION=\"$(CAMEX_GIT_VER)\"
endif

# Compile camex-k.c as camex.ko (no tun.ko dependency)
obj-m := camex.o
camex-objs := camex-k.o
