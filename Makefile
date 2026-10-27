# MiYooSDR. `make host` builds PC binaries (try build/host/miyoosdr --fake --shot x.png);
# `./build.sh` cross-compiles build/miyoo/* in the Miyoo toolchain image and packages a zip.
# The device build defines MIYOO, which enables the SigmaStar MI_AO audio path.
# -ffp-contract=off: no fused multiply-adds, so the PC and the Miyoo draw identical frames.
VERSION := $(shell cat VERSION)
CFLAGS := -std=gnu99 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -ffp-contract=off
APP_CFLAGS := $(CFLAGS) -DMIYOOSDR_VERSION='"$(VERSION)"'
CROSS_CC := arm-linux-gnueabihf-gcc
ARM_CFLAGS := -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
APP_SRC := src/main.c src/pcap.c
APP_DEPS := $(APP_SRC) src/font.h src/pcap.h VERSION

host: build/host/miyoosdr build/host/miyoo-pcap
miyoo: build/miyoo/miyoosdr build/miyoo/miyoo-pcap

build/host/miyoo-pcap: tools/miyoo-pcap.c
	mkdir -p $(dir $@)
	cc $(CFLAGS) -o $@ $<

build/miyoo/miyoo-pcap: tools/miyoo-pcap.c
	mkdir -p $(dir $@)
	$(CROSS_CC) $(CFLAGS) $(ARM_CFLAGS) -o $@ $<

build/host/miyoosdr: $(APP_DEPS)
	mkdir -p $(dir $@)
	cc $(APP_CFLAGS) -o $@ $(APP_SRC) -lm

build/miyoo/miyoosdr: $(APP_DEPS)
	mkdir -p $(dir $@)
	$(CROSS_CC) $(APP_CFLAGS) -DMIYOO $(ARM_CFLAGS) -o $@ $(APP_SRC) -lm -lpthread -ldl

# Leaves build/research alone: it holds local research data (captures, the stock module).
clean:
	rm -rf build/host build/miyoo

test: build/host/miyoosdr
	python3 tests/test_pcap.py
	python3 tests/test_cli.py
	python3 tests/test_replay.py
	python3 tests/test_app.py
	python3 tests/test_scripts.py

.PHONY: host miyoo clean test
