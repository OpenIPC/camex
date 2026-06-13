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

CPPFLAGS += -I. -DCAMEX_VERSION=\"$(VERSION)\"
CFLAGS   += -std=c99 -D_POSIX_C_SOURCE=200809L \
            -Wall -Wextra -Wshadow -Wstrict-prototypes \
            -Os -ffunction-sections -fdata-sections
LDFLAGS  += -Wl,--gc-sections

SRCS = main.c camex.c monocypher.c util.c log.c crypto.c tun.c net.c proto.c config.c client.c server.c
OBJS = $(SRCS:.c=.o)

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean distclean run help kmod kmod-clean kmod-install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)
	$(STRIP) --strip-unneeded $@ 2>/dev/null || true

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

kmod:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

kmod-install: kmod
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

kmod-clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

clean:
	rm -f $(TARGET) $(OBJS)

distclean: clean kmod-clean

run: $(TARGET)
	sudo ./$(TARGET)

help:
	@echo "make all                         - build camex userspace app"
	@echo "make CROSS_COMPILE=mipsel-linux- - cross-compile for MIPS"
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
