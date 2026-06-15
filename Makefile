#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# Makefile — top-level proxy that delegates to src/Makefile
#

TARGET ?= camex
BUILD_DIR ?= .

.PHONY: all clean distclean run help kmod kmod-clean kmod-install apk

# Only pass variables that were explicitly set by caller (not defaults).
# GNU Make v4.0+ supports $(origin …) to detect command-line vs default.
ifeq ($(origin CC),command line)
  MAKE_CC_ARG = CC='$(CC)'
else
  MAKE_CC_ARG =
endif
ifeq ($(origin STRIP),command line)
  MAKE_STRIP_ARG = STRIP='$(STRIP)'
else
  MAKE_STRIP_ARG =
endif
ifeq ($(origin TARGET),command line)
  MAKE_TARGET_ARG = TARGET='$(TARGET)'
else
  MAKE_TARGET_ARG =
endif
ifeq ($(origin BUILD_DIR),command line)
  MAKE_BUILDDIR_ARG = BUILD_DIR='$(BUILD_DIR)'
else
  MAKE_BUILDDIR_ARG =
endif
MAKE_ARGS = $(MAKE_CC_ARG) $(MAKE_STRIP_ARG) $(MAKE_TARGET_ARG) $(MAKE_BUILDDIR_ARG)

all:
	$(MAKE) -C src all $(MAKE_ARGS)
	cp -f src/$(BUILD_DIR)/$(TARGET) $(TARGET) 2>/dev/null || true

clean:
	$(MAKE) -C src clean $(MAKE_ARGS)
	rm -f $(TARGET) $(TARGET).exe

distclean: clean
	$(MAKE) -C src distclean $(MAKE_ARGS)

run: all
	sudo ./$(TARGET)

help:
	$(MAKE) -C src help

kmod:
	$(MAKE) -C src kmod

kmod-clean:
	$(MAKE) -C src kmod-clean

kmod-install:
	$(MAKE) -C src kmod-install

apk:
	$(MAKE) -C src apk $(MAKE_ARGS)
