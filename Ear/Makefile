CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS = -Iinclude -Icontract
CORE = src/ear_music.c src/ear_synth.c src/ear_bones_bridge.c
SURFACE = src/ear_surface.c src/ear_bones_contacts.c src/surface_materials.c
LDFLAGS = -lm

.PHONY: all test preview measure sanitize clean
all: build/ear_tool build/ear_surface_tool build/test_ear build/test_bones_contract build/test_surface

build/ear_tool: tools/ear_tool.c $(CORE) include/ear.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tools/ear_tool.c $(CORE) $(LDFLAGS)

build/ear_surface_tool: tools/ear_surface_tool.c $(CORE) $(SURFACE)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tools/ear_surface_tool.c $(CORE) $(SURFACE) $(LDFLAGS)

build/test_ear: tests/test_ear.c $(CORE) include/ear.h include/ear_bones_bridge.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_ear.c $(CORE) $(LDFLAGS)

# Legacy BonesModule v1 mock retained to test backwards-compatible Ear bridge.
# This does not prove Bones 0.2.0 source-level or binary integration.
build/test_bones_contract: tests/test_bones_contract.c integration/ear_bones_module.c $(CORE)
	mkdir -p build
	$(CC) $(CPPFLAGS) -Iintegration -Itests/fixtures $(CFLAGS) -o $@ tests/test_bones_contract.c integration/ear_bones_module.c $(CORE) $(LDFLAGS)

build/test_surface: tests/test_surface.c $(CORE) $(SURFACE)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_surface.c $(CORE) $(SURFACE) $(LDFLAGS)

test: build/test_ear build/test_bones_contract build/test_surface
	./build/test_ear
	./build/test_bones_contract
	./build/test_surface

preview: build/ear_surface_tool
	./build/ear_surface_tool footstep stone examples/footstep_stone.wav
	./build/ear_surface_tool footstep wood examples/footstep_wood.wav
	./build/ear_surface_tool footstep metal examples/footstep_metal.wav
	./build/ear_surface_tool impact stone examples/impact_stone.wav
	./build/ear_surface_tool impact wood examples/impact_wood.wav
	./build/ear_surface_tool impact metal examples/impact_metal.wav

measure:
	sh scripts/measure_surface.sh

sanitize:
	mkdir -p build
	$(CC) $(CPPFLAGS) -std=c11 -g -O1 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -o build/test_surface_san tests/test_surface.c $(CORE) $(SURFACE) $(LDFLAGS)
	ASAN_OPTIONS=detect_leaks=1 ./build/test_surface_san

clean:
	rm -f build/* examples/footstep_*.wav examples/impact_*.wav
