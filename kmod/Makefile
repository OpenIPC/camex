# SPDX-License-Identifier: GPL-2.0-or-later
# Kernel module Makefile for camex_kmod
#
# Build out-of-tree:
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
#
# Cross-compile example (OpenWrt / Buildroot):
#   make ARCH=mips CROSS_COMPILE=mips-linux-gnu- \
#        KERNELDIR=/path/to/linux modules

obj-m := camex_kmod.o

# Allow the caller to override the kernel source tree location.
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: modules modules_install clean

modules:
	$(MAKE) -C $(KERNELDIR) \
	    $(if $(ARCH),ARCH="$(ARCH)",) \
	    $(if $(CROSS_COMPILE),CROSS_COMPILE="$(CROSS_COMPILE)",) \
	    M=$(CURDIR) modules

modules_install:
	$(MAKE) -C $(KERNELDIR) \
	    $(if $(ARCH),ARCH="$(ARCH)",) \
	    $(if $(CROSS_COMPILE),CROSS_COMPILE="$(CROSS_COMPILE)",) \
	    M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) clean
