#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# Makefile — unit tests (host only, no root required)
#

CC      ?= gcc
SRC     = ../src
CFLAGS  += -I$(SRC) -std=c99 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
           -Wall -Wextra -Wshadow -Wstrict-prototypes -O1 -g $(EXTRA_CFLAGS)

TESTS = test_net_tcp test_netw test_l2 test_bridge test_chacha_neon test_proto test_log test_packet test_noise test_keys test_session test_nginxmap test_tcpgate

# Rebuild the tests when any project header changes
HEADERS = $(wildcard $(SRC)/*.h)

.PHONY: all test clean v2tool-build

all: $(TESTS)

test_net_tcp: test_net_tcp.c $(SRC)/net.c $(SRC)/util.c $(SRC)/log.c \
              $(SRC)/keys.c $(SRC)/noise.c $(SRC)/monocypher.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_netw: test_netw.c $(SRC)/netw.c $(SRC)/proto.c $(SRC)/config.c $(SRC)/nginxmap.c $(SRC)/slotidx.c $(SRC)/util.c $(SRC)/log.c \
           $(SRC)/crypto.c $(SRC)/keys.c $(SRC)/monocypher.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_l2: test_l2.c $(SRC)/l2.c $(SRC)/monocypher.c $(HEADERS)
	$(CC) $(CFLAGS) -DCAMEX_L2 -o $@ $(filter %.c,$^)

test_bridge: test_bridge.c $(SRC)/bridge.c $(SRC)/l2.c $(SRC)/util.c $(SRC)/log.c $(SRC)/monocypher.c $(HEADERS)
	$(CC) $(CFLAGS) -DCAMEX_L2 -o $@ $(filter %.c,$^)

# On 32-bit ARM (the musl CI build under QEMU) the NEON code is compared
# with the plain one; elsewhere the test only says there is no NEON
ARM32 := $(if $(filter arm%,$(shell $(CC) -dumpmachine 2>/dev/null)),$(if $(filter aarch64% arm64%,$(shell $(CC) -dumpmachine 2>/dev/null)),,1),)
NEON_TEST_FLAGS := $(if $(ARM32),-DCAMEX_CHACHA_NEON -march=armv7-a -mfpu=neon $(if $(shell $(CC) -dM -E - </dev/null 2>/dev/null | grep __ARM_PCS_VFP),,-mfloat-abi=softfp),)

test_chacha_neon: test_chacha_neon.c $(SRC)/chacha_neon.c $(SRC)/monocypher.c $(HEADERS)
	$(CC) $(CFLAGS) $(NEON_TEST_FLAGS) -o $@ $(filter %.c,$^)

test_proto: test_proto.c $(SRC)/proto.c $(SRC)/netw.c $(SRC)/config.c $(SRC)/nginxmap.c $(SRC)/slotidx.c $(SRC)/util.c $(SRC)/log.c \
            $(SRC)/crypto.c $(SRC)/keys.c $(SRC)/monocypher.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_nginxmap: test_nginxmap.c $(SRC)/nginxmap.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_tcpgate: test_tcpgate.c $(SRC)/tcpgate.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_log: test_log.c $(SRC)/log.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_packet: test_packet.c $(SRC)/packet.c $(SRC)/tun.c $(SRC)/util.c \
             $(SRC)/log.c $(SRC)/monocypher.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_noise: test_noise.c noise_vectors.h $(SRC)/noise.c $(SRC)/monocypher.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_keys: test_keys.c $(SRC)/keys.c $(SRC)/noise.c $(SRC)/monocypher.c \
           $(SRC)/util.c $(SRC)/log.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_session: test_session.c $(SRC)/session.c $(SRC)/noise.c \
              $(SRC)/monocypher.c $(SRC)/util.c $(SRC)/log.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

# e2e helper (not a unit test): protocol v2 handshakes from the tests
v2tool: v2tool.c $(SRC)/keys.c $(SRC)/noise.c $(SRC)/session.c \
        $(SRC)/monocypher.c $(SRC)/util.c $(SRC)/log.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test: all
	@set -e; for t in $(TESTS); do ./$$t; done

clean:
	rm -f $(TESTS) v2tool
