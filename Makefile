# SPDX-License-Identifier: GPL-3.0-only
# make test        the core's checks and the headless render (any Unix with a C compiler)
# make app         build/SHARD XL.app (macOS, Apple Silicon)
# make shots       every page as a picture and 8 s of audio, into build/shots (Linux or Mac)
CC ?= cc
CFLAGS = -std=c11 -O2 -Wall -Wno-unused-function -Wno-unknown-warning-option -Wno-format-truncation -Isrc
VERSION = $(shell sed -n 's/^\#define XL_VERSION "\(.*\)"/\1/p' src/xl.h)
CORE = src/xl_core.c
CORE_DEPS = $(wildcard src/*.c src/*.h)
APP = build/SHARD XL.app

all: test

build/core_test: tests/core_test.c $(CORE_DEPS)
	@mkdir -p build
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -o $@ tests/core_test.c src/plat_posix.c -lm

build/xl_headless: $(CORE_DEPS)
	@mkdir -p build
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -o $@ $(CORE) src/plat_posix.c src/plat_headless.c -lm

test: build/core_test build/xl_headless
	rm -rf build/test_home && mkdir -p build/test_home build/shots
	./build/core_test
	./build/xl_headless build/test_home build/shots 4

shots: build/xl_headless
	rm -rf build/shots_home && mkdir -p build/shots_home build/shots
	./build/xl_headless build/shots_home build/shots 8

build/SHARD-XL: $(CORE_DEPS) src/plat_mac.m
	@mkdir -p build
	clang $(CFLAGS) -arch arm64 -mmacosx-version-min=12.0 -c $(CORE) -o build/xl_core.o
	clang $(CFLAGS) -arch arm64 -mmacosx-version-min=12.0 -fobjc-arc -Wno-deprecated-declarations -c src/plat_mac.m -o build/plat_mac.o
	clang -arch arm64 -mmacosx-version-min=12.0 -o $@ build/xl_core.o build/plat_mac.o \
		-framework Cocoa -framework AudioToolbox -framework CoreAudio -framework CoreMIDI

app: build/SHARD-XL
	rm -rf "$(APP)"
	mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	cp build/SHARD-XL "$(APP)/Contents/MacOS/SHARD-XL"
	sed 's/VERSION/$(VERSION)/g' assets/Info.plist > "$(APP)/Contents/Info.plist"
	iconutil -c icns assets/AppIcon.iconset -o "$(APP)/Contents/Resources/AppIcon.icns"
	codesign --force --deep -s - "$(APP)"

clean:
	rm -rf build

.PHONY: all test shots app clean
