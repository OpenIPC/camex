#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# Makefile — top-level proxy that delegates to src/Makefile
#

TARGET ?= camex

.PHONY: all clean distclean run help kmod kmod-clean kmod-install apk

all:
	$(MAKE) -C src all
	cp -f src/$(TARGET) $(TARGET) 2>/dev/null || true

clean:
	$(MAKE) -C src clean
	rm -f $(TARGET) $(TARGET).exe

distclean: clean
	$(MAKE) -C src distclean

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
	$(MAKE) -C src apk
