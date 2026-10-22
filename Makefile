# MiYooSDR spike. `make host` builds a PC binary (use --fake --shot x.ppm);
# `./build.sh` cross-compiles build/miyoo/miyoosdr in the Miyoo toolchain image.
CFLAGS := -std=gnu99 -O2 -g -Wall -Wextra -Wno-unused-parameter

host: build/host/miyoosdr
miyoo: build/miyoo/miyoosdr

build/host/miyoosdr: src/main.c src/font.h
	mkdir -p $(dir $@)
	cc $(CFLAGS) -o $@ src/main.c -lm

build/miyoo/miyoosdr: src/main.c src/font.h
	mkdir -p $(dir $@)
	arm-linux-gnueabihf-gcc $(CFLAGS) -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -o $@ src/main.c -lm

clean:
	rm -rf build
.PHONY: host miyoo clean
