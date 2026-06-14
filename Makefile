#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# Makefile — build rules for camex userspace app and kernel module
#

CROSS_COMPILE ?=
CC    = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

TARGET  ?= camex
VERSION ?= $(shell git -C $(dir $(abspath $(lastword $(MAKEFILE_LIST)))) \
                describe --tags --always --dirty 2>/dev/null || echo "unknown")

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
# Windows (MinGW): need modern WINVER for socklen_t and Winsock2.
ifneq ($(findstring mingw,$(CC)),)
  WIN32_CFLAGS = -D_WIN32_WINNT=0x0601 -D__USE_MINGW_ANSI_STDIO
else
  WIN32_CFLAGS =
endif
CFLAGS   += -std=$(STD) -D_GNU_SOURCE $(POSIX_SRC) $(DARWIN_CFLAGS) $(WIN32_CFLAGS) \
            -Wall -Wextra -Wshadow -Wstrict-prototypes \
            -Os -ffunction-sections -fdata-sections
LDFLAGS  += -Wl,--gc-sections

SRCS = main.c camex.c monocypher.c util.c log.c crypto.c tun.c net.c proto.c config.c client.c server.c
OBJS = $(SRCS:.c=.o)

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

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)
	$(STRIP) --strip-unneeded $@ 2>/dev/null || true

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

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
	cd android && ./gradlew assembleRelease
	cp android/app/build/outputs/apk/release/app-release.apk camex-$(VERSION).apk
	@echo "Created camex-$(VERSION).apk"

clean:
	rm -f $(TARGET) $(OBJS) $(TARGET).exe
	rm -rf ipkroot camex_*.apk

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
