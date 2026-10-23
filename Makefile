# MiYooSDR. `make host` builds a PC binary (use --fake --shot x.ppm);
# `./build.sh` cross-compiles build/miyoo/miyoosdr in the Miyoo toolchain image.
# The device build defines MIYOO, which enables the SigmaStar MI_AO audio path.
CFLAGS := -std=gnu99 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation

host: build/host/miyoosdr
miyoo: build/miyoo/miyoosdr

build/host/miyoosdr: src/main.c src/font.h
	mkdir -p $(dir $@)
	cc $(CFLAGS) -o $@ src/main.c -lm

build/miyoo/miyoosdr: src/main.c src/font.h
	mkdir -p $(dir $@)
	arm-linux-gnueabihf-gcc $(CFLAGS) -DMIYOO -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -o $@ src/main.c -lm -lpthread -ldl

clean:
	rm -rf build
.PHONY: host miyoo clean
