# Run from repository root with: make -C Mouth test / make -C Mouth loopback
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
B = ../BONES
CORE = $(B)/src/bones/core.c $(B)/src/bones/world.c \
       $(B)/src/bones/physics.c $(B)/src/bones/scene.c \
       $(B)/src/bones/subsystems.c $(B)/src/foundation/foundation.c
HOST = $(B)/src/modkit/modkit.c
DROPIN = $(B)/modules/mouth/modkit.c $(B)/modules/mouth/mouth_lan.c $(B)/modules/mouth/mouth_lan.h
INC = -I$(B)/include -Iinclude
LIBS = -lm
.PHONY: all test loopback sanitize clean
all: build/mouth_lan_demo
build/mouth_lan_demo: examples/lan_demo.c include/mouth_lan.h $(CORE) $(HOST) $(DROPIN)
	mkdir -p build
	$(CC) $(CFLAGS) -DBONES_MODKIT_NO_DYNLIB $(INC) \
		examples/lan_demo.c $(B)/modules/mouth/modkit.c $(CORE) $(HOST) -o $@ $(LIBS)
build/test_codec: tests/test_codec.c src/mouth_lan.c include/mouth_lan.h
	mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/test_codec.c src/mouth_lan.c -o $@ $(LIBS)
test: build/test_codec
	./build/test_codec
loopback: all test
	python3 tests/test_loopback.py ./build/mouth_lan_demo
sanitize:
	mkdir -p build
	$(CC) -std=c11 -g -O1 -Wall -Wextra -Wpedantic -fsanitize=address,undefined \
		-fno-omit-frame-pointer $(INC) tests/test_codec.c src/mouth_lan.c \
		-o build/test_codec_sanitize $(LIBS)
	./build/test_codec_sanitize
clean:
	rm -f build/test_codec build/test_codec_sanitize build/mouth_lan_demo
