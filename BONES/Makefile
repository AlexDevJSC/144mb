CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CORE = src/bones/core.c src/bones/world.c src/bones/physics.c src/bones/scene.c src/bones/subsystems.c src/foundation/foundation.c
EYE = src/adapters/eye_adapter.c third_party/eye/sw_renderer.c
DEMO = src/demo/test_house.c
MODULE_SRCS = $(wildcard modules/*/module.c)
SRC = $(CORE) $(EYE) $(DEMO) $(MODULE_SRCS) src/generated/module_registry.c
.PHONY: all core test preview clean sanitize registry
registry:
	python3 scripts/gen_registry.py
all: build/modkit_headless
core: build/libbones.a
build/libbones.a: $(CORE) include/bones/bones.h
	mkdir -p build
	$(CC) $(CFLAGS) -c src/bones/core.c -o build/core.o
	$(CC) $(CFLAGS) -c src/bones/world.c -o build/world.o
	$(CC) $(CFLAGS) -c src/bones/physics.c -o build/physics.o
	$(CC) $(CFLAGS) -c src/bones/scene.c -o build/scene.o
	$(CC) $(CFLAGS) -c src/bones/subsystems.c -o build/subsystems.o
	$(CC) $(CFLAGS) -c src/foundation/foundation.c -o build/foundation.o
	ar rcs $@ build/core.o build/world.o build/physics.o build/scene.o build/subsystems.o build/foundation.o
build/bones_headless: registry tools/headless.c $(SRC) include/bones/bones.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/headless.c $(SRC) -lm
build/test_bones: registry tests/test_bones.c $(SRC) include/bones/bones.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_bones.c $(SRC) -lm
build/test_subsystems: registry tests/test_subsystems.c $(SRC) include/bones/bones.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_subsystems.c $(SRC) -lm
build/test_geometry: registry tests/test_geometry.c $(SRC) include/bones/bones.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_geometry.c $(SRC) -lm
build/test_foundation: registry tests/test_foundation.c $(SRC) include/bones/foundation.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_foundation.c $(SRC) -lm
build/test_playtest: registry tests/test_playtest.c $(SRC) include/bones/bones.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_playtest.c $(SRC) -lm
test: build/test_bones build/test_subsystems build/test_geometry build/test_foundation build/test_playtest
	./build/test_bones
	./build/test_subsystems
	./build/test_geometry
	./build/test_foundation
	./build/test_playtest
preview: build/bones_headless
	./build/bones_headless build/bones_preview.ppm
sanitize: registry
	mkdir -p build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -o build/test_sanitize tests/test_bones.c $(SRC) -lm
	./build/test_sanitize
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -o build/test_subsystems_sanitize tests/test_subsystems.c $(SRC) -lm
	./build/test_subsystems_sanitize
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -o build/test_geometry_sanitize tests/test_geometry.c $(SRC) -lm
	./build/test_geometry_sanitize
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -o build/test_foundation_sanitize tests/test_foundation.c $(SRC) -lm
	./build/test_foundation_sanitize
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -o build/test_playtest_sanitize tests/test_playtest.c $(SRC) -lm
	./build/test_playtest_sanitize
clean:
	rm -f build/test_bones build/test_subsystems build/test_geometry build/test_foundation build/test_playtest build/test_foundation_sanitize build/foundation.o build/test_sanitize build/test_subsystems_sanitize build/test_geometry_sanitize build/bones_headless build/bones_preview.ppm build/bones_movimiento.ppm build/bones_tejado.ppm build/libbones.a build/core.o build/world.o build/physics.o build/scene.o build/subsystems.o

# BONES ModKit v0.4 — separate, versioned modules. Do not mix registrations with legacy.
MODKIT = src/modkit/modkit.c
MODKIT_MODULES = $(wildcard modules/*/modkit.c)
MODKIT_REGISTRY = src/generated/modkit_registry.c
MODKIT_LIBS = -ldl
.PHONY: modkit_registry modkit_demo modkit_test dll_test
modkit_registry:
	python3 scripts/gen_modkit_registry.py
build/modkit_demo: modkit_registry tools/modkit_demo.c $(CORE) $(EYE) $(DEMO) $(MODKIT) $(MODKIT_MODULES) $(MODKIT_REGISTRY)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/modkit_demo.c $(CORE) $(EYE) $(DEMO) $(MODKIT) $(MODKIT_MODULES) $(MODKIT_REGISTRY) -lm $(MODKIT_LIBS)
modkit_demo: build/modkit_demo
	./build/modkit_demo
build/test_modkit: modkit_registry tests/test_modkit.c $(CORE) $(EYE) $(DEMO) $(MODKIT) $(MODKIT_MODULES) $(MODKIT_REGISTRY)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_modkit.c $(CORE) $(EYE) $(DEMO) $(MODKIT) $(MODKIT_MODULES) $(MODKIT_REGISTRY) -lm $(MODKIT_LIBS)
build/clock_mod.so: samples/plugin/clock_mod.c include/bones/modkit.h
	mkdir -p build
	$(CC) -std=c11 -O2 -fPIC -shared -Wall -Wextra -Wpedantic -o $@ $<
build/test_modkit_dll: modkit_registry tests/test_modkit_dll.c $(CORE) $(EYE) $(DEMO) $(MODKIT) $(MODKIT_MODULES) $(MODKIT_REGISTRY)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_modkit_dll.c $(CORE) $(EYE) $(DEMO) $(MODKIT) $(MODKIT_MODULES) $(MODKIT_REGISTRY) -lm $(MODKIT_LIBS)
build/test_host_audio_brain: modkit_registry tests/test_host_audio_brain.c $(CORE) $(EYE) $(DEMO) $(MODKIT) $(MODKIT_MODULES) $(MODKIT_REGISTRY)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_host_audio_brain.c $(CORE) $(EYE) $(DEMO) $(MODKIT) $(MODKIT_MODULES) $(MODKIT_REGISTRY) -lm $(MODKIT_LIBS)
.PHONY: host_audio_brain_test
host_audio_brain_test: build/test_host_audio_brain
	./build/test_host_audio_brain

build/test_host_ui: modkit_registry tests/test_host_ui.c $(CORE) $(EYE) $(DEMO) $(MODKIT) $(MODKIT_MODULES) $(MODKIT_REGISTRY)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_host_ui.c $(CORE) $(EYE) $(DEMO) $(MODKIT) $(MODKIT_MODULES) $(MODKIT_REGISTRY) -lm $(MODKIT_LIBS)
.PHONY: host_ui_test
host_ui_test: build/test_host_ui
	./build/test_host_ui

modkit_test: build/test_modkit build/test_modkit_dll build/clock_mod.so
	./build/test_modkit
	./build/test_modkit_dll ./build/clock_mod.so
dll_test: build/test_modkit_dll build/clock_mod.so
	./build/test_modkit_dll ./build/clock_mod.so
.PHONY: modkit_preview
build/modkit_headless: modkit_registry tools/modkit_headless.c $(CORE) $(EYE) $(DEMO) $(MODKIT) $(MODKIT_MODULES) $(MODKIT_REGISTRY)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/modkit_headless.c $(CORE) $(EYE) $(DEMO) $(MODKIT) $(MODKIT_MODULES) $(MODKIT_REGISTRY) -lm $(MODKIT_LIBS)
modkit_preview: build/modkit_headless
	./build/modkit_headless build/modkit_preview.ppm
.PHONY: modkit_core
build/modkit.o: src/modkit/modkit.c include/bones/modkit.h include/bones/modkit_host.h
	mkdir -p build
	$(CC) $(CFLAGS) -c src/modkit/modkit.c -o $@
build/libbones_modkit.a: build/libbones.a build/modkit.o
	cp build/libbones.a $@
	ar rcs $@ build/modkit.o
modkit_core: build/libbones_modkit.a
