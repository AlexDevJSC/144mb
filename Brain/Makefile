CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
INCLUDES = -Iinclude
SRC = src/brain_clock.c src/brain_random.c src/brain_economy.c \
      src/brain_npc.c src/brain_interaction.c src/brain_world.c src/brain_bones.c src/brain_visual.c
.PHONY: all test demo bones-demo clean sanitize modkit-test playtest-mock
all: build/libbrain.a
build:
	mkdir -p build
build/libbrain.a: $(SRC) include/brain/brain.h include/brain/brain_visual.h | build
	$(CC) $(CFLAGS) $(INCLUDES) -c $(SRC)
	ar rcs $@ brain_clock.o brain_random.o brain_economy.o brain_npc.o brain_interaction.o brain_world.o brain_bones.o brain_visual.o
	rm -f brain_clock.o brain_random.o brain_economy.o brain_npc.o brain_interaction.o brain_world.o brain_bones.o brain_visual.o
build/test_brain: tests/test_brain.c $(SRC) include/brain/brain.h include/brain/brain_visual.h | build
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ tests/test_brain.c $(SRC)
build/bones_stub: demo/bones_stub_integration.c $(SRC) include/brain/brain.h | build
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ demo/bones_stub_integration.c $(SRC)
bones-demo: build/bones_stub
	./build/bones_stub
build/brain_demo: demo/brain_demo.c $(SRC) include/brain/brain.h | build
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ demo/brain_demo.c $(SRC)
build/test_visual_refs: tests/test_visual_refs.c $(SRC) include/brain/brain_visual.h | build
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ tests/test_visual_refs.c $(SRC)
build/test_modkit_bridge: bridge/bones_0_4/tests/test_modkit_bridge.c bridge/bones_0_4/modules/brain/modkit.c bridge/bones_0_4/modules/brain/brain_npc.c bridge/bones_0_4/modules/brain/brain.h bridge/bones_0_4/include/bones/modkit.h | build
	$(CC) $(CFLAGS) -o $@ bridge/bones_0_4/tests/test_modkit_bridge.c bridge/bones_0_4/modules/brain/modkit.c
build/test_brain_interaction: tests/test_brain_interaction.c src/brain_interaction.c include/brain/brain_interaction.h include/brain/brain.h | build
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ tests/test_brain_interaction.c src/brain_interaction.c
build/test_brain_playtest_mock: bridge/bones_0_4/tests/test_brain_playtest_mock.c bridge/bones_0_4/modules/brain/modkit.c bridge/bones_0_4/modules/brain/brain_mod_diagnostics.h | build
	$(CC) $(CFLAGS) -Werror -o $@ bridge/bones_0_4/tests/test_brain_playtest_mock.c bridge/bones_0_4/modules/brain/modkit.c -lm
playtest-mock: build/test_brain_playtest_mock
	./build/test_brain_playtest_mock
modkit-test: build/test_modkit_bridge build/test_brain_playtest_mock
	./build/test_modkit_bridge
	./build/test_brain_playtest_mock
test: build/test_brain build/test_visual_refs build/test_modkit_bridge build/test_brain_playtest_mock build/test_brain_interaction
	./build/test_brain
	./build/test_visual_refs
	./build/test_modkit_bridge
	./build/test_brain_playtest_mock
	./build/test_brain_interaction
demo: build/brain_demo
	./build/brain_demo
clean:
	rm -rf build *.o

sanitize: | build
	$(CC) -O1 -g -std=c11 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer $(INCLUDES) -o build/test_visual_san tests/test_visual_refs.c $(SRC)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build/test_visual_san
