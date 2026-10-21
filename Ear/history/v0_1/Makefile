CC = cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS = -Iinclude
CORE = src/ear_music.c src/ear_synth.c src/ear_bones_bridge.c
LDFLAGS = -lm

.PHONY: all test clean example
all: build/ear_tool build/test_ear build/test_bones_contract

build/ear_tool: tools/ear_tool.c $(CORE) include/ear.h include/ear_bones_bridge.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tools/ear_tool.c $(CORE) $(LDFLAGS)

build/test_ear: tests/test_ear.c $(CORE) include/ear.h include/ear_bones_bridge.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_ear.c $(CORE) $(LDFLAGS)

build/test_bones_contract: tests/test_bones_contract.c integration/ear_bones_module.c $(CORE) 
	mkdir -p build
	$(CC) $(CPPFLAGS) -Iintegration -Itests/fixtures $(CFLAGS) -o $@ tests/test_bones_contract.c integration/ear_bones_module.c $(CORE) $(LDFLAGS)

test: build/test_ear build/test_bones_contract
	./build/test_ear
	./build/test_bones_contract

example: build/ear_tool
	./build/ear_tool music dungeon 144 examples/dungeon.wav 8
	./build/ear_tool sfx sword examples/sword.wav 144

clean:
	rm -f build/ear_tool build/test_ear build/test_ear_asan build/test_bones_contract examples/*.wav
