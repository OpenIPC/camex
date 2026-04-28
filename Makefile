# SPDX-License-Identifier: GPL-2.0-or-later
# Top-level Makefile for camex
#
# Targets:
#   all           - build userspace daemon (default)
#   kmod          - build optional kernel module
#   install       - install daemon to $(DESTDIR)$(PREFIX)/sbin
#   clean         - remove build artefacts
#
# Cross-compile example:
#   make ARCH=mips CROSS_COMPILE=mips-linux-gnu-

CC      ?= $(CROSS_COMPILE)gcc
STRIP   ?= $(CROSS_COMPILE)strip

PREFIX  ?= /usr/local
DESTDIR ?=

CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c99
CFLAGS  += -D_GNU_SOURCE

TARGET  := camexd
SRCS    := camexd.c

.PHONY: all kmod install clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

kmod:
	$(MAKE) -C kmod \
	    $(if $(ARCH),ARCH="$(ARCH)",) \
	    $(if $(CROSS_COMPILE),CROSS_COMPILE="$(CROSS_COMPILE)",) \
	    $(if $(KERNELDIR),KERNELDIR="$(KERNELDIR)",) modules

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/sbin/$(TARGET)

clean:
	rm -f $(TARGET)
	$(MAKE) -C kmod clean 2>/dev/null || true
