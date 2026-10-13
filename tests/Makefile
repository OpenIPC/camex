#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# Makefile — unit tests (host only, no root required)
#

CC      ?= gcc
SRC     = ../src
CFLAGS  += -I$(SRC) -std=c99 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
           -Wall -Wextra -Wshadow -Wstrict-prototypes -O1 -g $(EXTRA_CFLAGS)

TESTS = test_net_tcp

.PHONY: all test clean

all: $(TESTS)

test_net_tcp: test_net_tcp.c $(SRC)/net.c $(SRC)/util.c $(SRC)/log.c
	$(CC) $(CFLAGS) -o $@ $^

test: all
	@set -e; for t in $(TESTS); do ./$$t; done

clean:
	rm -f $(TESTS)
