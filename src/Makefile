#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# Makefile — build rules for camex userspace app and kernel module
#

CROSS_COMPILE ?=
CC    = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

TARGET  ?= camex
# CAMEX_VERSION of version.h, the one place of the version (#117)
VERSION := $(shell sed -n 's/^\#define CAMEX_VERSION "\(.*\)"/\1/p' \
                $(dir $(abspath $(lastword $(MAKEFILE_LIST))))version.h)

# Out-of-tree build directory: make BUILD_DIR=../build/x86_64
BUILD_DIR ?= .
objdir = $(BUILD_DIR)/obj

CPPFLAGS += -I.
STD      ?= c99
# macOS: -D_POSIX_C_SOURCE suppresses BSD types (u_short, u_char etc.).
# _DARWIN_C_SOURCE exposes them while keeping POSIX conformance.
# Set from CI via STD=gnu99 + POSIX_SRC=.
POSIX_SRC ?= -D_POSIX_C_SOURCE=200809L
# Detect macOS and add _DARWIN_C_SOURCE automatically when POSIX_SRC is in use.
ifneq ($(shell uname -s 2>/dev/null),Darwin)
  DARWIN_CFLAGS =
else
  DARWIN_CFLAGS = -D_DARWIN_C_SOURCE=1
endif
# Windows (MinGW): use gnu99 to avoid __STRICT_ANSI__ issues with
# Windows SDK types (NET_LUID, GetAdaptersInfo, etc.).
ifneq ($(findstring mingw,$(CC)),)
  STD = gnu99
  WIN32_CFLAGS = -D_WIN32_WINNT=0x0601 -DNTDDI_VERSION=0x06010000 -D__USE_MINGW_ANSI_STDIO
else
  WIN32_CFLAGS =
endif
CFLAGS   += -std=$(STD) -D_GNU_SOURCE $(POSIX_SRC) $(DARWIN_CFLAGS) $(WIN32_CFLAGS) \
            -Wall -Wextra -Wshadow -Wstrict-prototypes \
            -Os -ffunction-sections -fdata-sections
ifneq ($(shell uname -s),Darwin)
  LDFLAGS  += -Wl,--gc-sections
else
  LDFLAGS  += -Wl,-dead_strip
endif

SRCS = main.c camex.c netw.c pake.c monocypher.c util.c log.c crypto.c noise.c keys.c session.c tun.c offload.c packet.c net.c proto.c config.c client.c slotidx.c control.c nginxmap.c tcpgate.c
SRCS += wintun.c

# Build profiles (#204), docs/build.md: a set of the flags below; a flag
# given on its own wins over its profile (PROFILE=client CAMEX_MESH=0)
#   full    everything: CAMEX_SERVER=1 CAMEX_L2=1 CAMEX_MESH=1 (the default;
#           "server" is the same)
#   client  cameras, routers, Android, desktops: CAMEX_SERVER=0, with
#           CAMEX_L2=1 CAMEX_MESH=1
PROFILE ?= full
ifneq ($(filter full server,$(PROFILE)),)
  PROFILE_SERVER = 1
else ifeq ($(PROFILE),client)
  PROFILE_SERVER = 0
else
  $(error PROFILE must be full (or server) or client, not "$(PROFILE)")
endif
# TAP and --bridge are Linux only: a profile leaves L2 out on Windows and
# macOS (CAMEX_L2=1 still builds it there)
ifneq ($(findstring mingw,$(CC))$(filter Darwin,$(shell uname -s 2>/dev/null)),)
  PROFILE_L2 = 0
else
  PROFILE_L2 = 1
endif
PROFILE_MESH = 1

# Optional features (#mesh, #204): CAMEX_L2=1 (TAP networks, LAN bridge),
# CAMEX_MESH=1 (peer to peer), CAMEX_SERVER=0 (a client only: no
# --mode server, no server code)
CAMEX_SERVER ?= $(PROFILE_SERVER)
CAMEX_L2 ?= $(PROFILE_L2)
CAMEX_MESH ?= $(PROFILE_MESH)
ifneq ($(words $(filter 0 1,$(CAMEX_SERVER) $(CAMEX_L2) $(CAMEX_MESH))),3)
  $(error CAMEX_SERVER, CAMEX_L2 and CAMEX_MESH are 0 or 1)
endif
ifeq ($(CAMEX_SERVER),1)
  SRCS += server.c cpaceguard.c
else
  CPPFLAGS += -DCAMEX_NO_SERVER
  SRCS += noserver.c
endif
ifeq ($(CAMEX_L2),1)
  CPPFLAGS += -DCAMEX_L2
  SRCS += l2.c bridge.c
endif
ifeq ($(CAMEX_MESH),1)
  CPPFLAGS += -DCAMEX_MESH
  SRCS += mesh.c
endif
# 32-bit ARM: ChaCha20 with NEON (chacha_neon.c), taken at run time only
# when the CPU has NEON, so one binary serves ARMv5 to ARMv7 cameras.
# CAMEX_NEON=0 leaves it out.
CAMEX_NEON ?= 1
TARGET_MACHINE := $(shell $(CC) -dumpmachine 2>/dev/null)
ifeq ($(CAMEX_NEON),1)
ifneq ($(filter arm%,$(TARGET_MACHINE)),)
ifeq ($(filter aarch64% arm64%,$(TARGET_MACHINE)),)
  CPPFLAGS += -DCAMEX_CHACHA_NEON
  SRCS += chacha_neon.c
  # softfp links with soft-float objects; a hard-float toolchain keeps its own
  NEON_FLOAT := $(if $(shell $(CC) -dM -E - </dev/null 2>/dev/null | grep __ARM_PCS_VFP),,-mfloat-abi=softfp)
  NEON_CFLAGS := -march=armv7-a -mfpu=neon $(NEON_FLOAT)
endif
endif
endif
OBJS = $(addprefix $(objdir)/, $(SRCS:.c=.o))

# Ensure object directory exists
$(shell mkdir -p $(objdir))

# The flags the objects were built with (#204): another profile or flag in
# the same BUILD_DIR rebuilds them all instead of mixing the two
FLAGS_STAMP = $(objdir)/.flags
FLAGS_NOW = $(CC) $(CPPFLAGS) $(CFLAGS) $(NEON_CFLAGS)
ifneq ($(shell cat $(FLAGS_STAMP) 2>/dev/null),$(strip $(FLAGS_NOW)))
  $(shell echo '$(strip $(FLAGS_NOW))' > $(FLAGS_STAMP))
endif

KDIR ?= /lib/modules/$(shell uname -r)/build

# Platform-specific libraries
ifneq ($(findstring mingw,$(CC)),)
LDLIBS += -lws2_32 -lbcrypt -liphlpapi
TARGET := camex.exe
endif
ifneq ($(findstring android,$(CC)),)
LDLIBS += -static
endif

.PHONY: all clean distclean run help kmod kmod-clean kmod-install apk

all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(OBJS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)
	$(STRIP) --strip-unneeded $@ 2>/dev/null || true

$(objdir)/%.o: %.c $(FLAGS_STAMP)
	mkdir -p $(objdir)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(objdir)/chacha_neon.o: chacha_neon.c $(FLAGS_STAMP)
	mkdir -p $(objdir)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(NEON_CFLAGS) -MMD -MP -c $< -o $@

# Header dependencies: an edited header rebuilds every object using it
-include $(OBJS:.o=.d)

# Kernel module (Linux only)
kmod:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

kmod-install: kmod
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

kmod-clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

# Android .apk package (requires Android SDK + NDK)
apk:
	cd ../android && ./gradlew assembleRelease
	APK=$$(ls ../android/app/build/outputs/apk/release/app-release.apk ../android/app/build/outputs/apk/release/app-release-unsigned.apk 2>/dev/null | head -1); \
	if [ -z "$$APK" ]; then echo "ERROR: APK not found" >&2; exit 1; fi; \
	cp "$$APK" camex-$(VERSION).apk
	@echo "Created camex-$(VERSION).apk"

clean:
	rm -f $(BUILD_DIR)/$(TARGET) $(OBJS) $(BUILD_DIR)/$(TARGET).exe
	rm -rf camex_*.apk
	rm -rf $(objdir)

distclean: clean kmod-clean

run: $(TARGET)
	sudo ./$(TARGET)

help:
	@echo "make all                         - build camex userspace app"
	@echo "make CROSS_COMPILE=mipsel-linux- - cross-compile for MIPS"
	@echo "make CC=clang                    - build with clang (macOS)"
	@echo "make CC=x86_64-w64-mingw32-gcc   - cross-compile for Windows"
	@echo "make CC=aarch64-linux-android21-clang - cross-compile for Android"
	@echo "make apk                         - build Android .apk package (requires SDK)"
	@echo "make CAMEX_L2=1 CAMEX_MESH=1     - the full camex: TAP networks, bridge, mesh"
	@echo "make CPPFLAGS=-DTUN_PACKET_MAX=1600 - build with smaller packet buffer"
	@echo "make run                         - run camex as root"
	@echo "make clean                       - remove userspace build outputs"
	@echo "make kmod                        - build camex.ko kernel module"
	@echo "make kmod-install                - install camex.ko into kernel tree"
	@echo "make kmod-clean                  - remove kernel module build artifacts"
	@echo "make distclean                   - clean all (app + kmod)"
	@echo ""
	@echo "Variables:"
	@echo "  KDIR=<path>   Kernel build directory (default: /lib/modules/\$$(uname -r)/build)"
	@echo "  CC=<compiler> Compiler (auto-detects MinGW, Android)"
