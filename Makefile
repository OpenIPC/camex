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
CFLAGS   += -std=c99 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
            -Wall -Wextra -Wshadow -Wstrict-prototypes \
            -Os -ffunction-sections -fdata-sections
LDFLAGS  += -Wl,--gc-sections

SRCS = main.c camex.c monocypher.c util.c log.c crypto.c tun.c net.c proto.c config.c client.c server.c
OBJS = $(SRCS:.c=.o)

KDIR ?= /lib/modules/$(shell uname -r)/build

# Platform-specific libraries
ifneq ($(findstring mingw,$(CC)),)
LDLIBS += -lws2_32 -lbcrypt
TARGET := camex.exe
endif
ifneq ($(findstring android,$(CC)),)
LDLIBS += -static
endif

.PHONY: all clean distclean run help kmod kmod-clean kmod-install ipk

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

# Android .ipk package
ipk: $(TARGET)
	@mkdir -p ipkroot/DEBIAN ipkroot/usr/sbin
	cp $(TARGET) ipkroot/usr/sbin/camex
	echo "Package: camex" > ipkroot/DEBIAN/control
	echo "Version: $(VERSION)" >> ipkroot/DEBIAN/control
	echo "Architecture: aarch64" >> ipkroot/DEBIAN/control
	echo "Maintainer: OpenIPC" >> ipkroot/DEBIAN/control
	echo "Section: net" >> ipkroot/DEBIAN/control
	echo "Priority: optional" >> ipkroot/DEBIAN/control
	echo "Description: Minimal dependency-free UDP/TCP tunnel for embedded Linux" >> ipkroot/DEBIAN/control
	@chmod 755 ipkroot/DEBIAN
	@chmod 755 ipkroot/usr/sbin/camex
	@tar -czf control.tar.gz -C ipkroot/DEBIAN .
	@tar -czf data.tar.gz -C ipkroot ./usr
	@echo 2.0 > debian-binary
	@tar -czf camex_$(VERSION)_aarch64.ipk debian-binary control.tar.gz data.tar.gz
	@rm -f control.tar.gz data.tar.gz debian-binary
	@rm -rf ipkroot
	@echo "Created camex_$(VERSION)_aarch64.ipk"

clean:
	rm -f $(TARGET) $(OBJS) $(TARGET).exe
	rm -rf ipkroot
	rm -f camex_*.ipk control.tar.gz data.tar.gz debian-binary

distclean: clean kmod-clean

run: $(TARGET)
	sudo ./$(TARGET)

help:
	@echo "make all                         - build camex userspace app"
	@echo "make CROSS_COMPILE=mipsel-linux- - cross-compile for MIPS"
	@echo "make CC=clang                    - build with clang (macOS)"
	@echo "make CC=x86_64-w64-mingw32-gcc   - cross-compile for Windows"
	@echo "make CC=aarch64-linux-android21-clang - cross-compile for Android"
	@echo "make ipk                         - build Android .ipk package"
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
