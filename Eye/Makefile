CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wconversion
LIB = src/eye.c src/eye2.c src/eye3.c src/eye_bones_bridge.c src/eye_tone.c
INC = -Iinclude
.PHONY: all test demo integration clean
all: build/eye build/test_eye build/test_eye2 build/test_eye3 build/test_bones_bridge build/test_bones_adapter_example build/test_eye_tone build/test_eye4 build/eye4_cli build/test_eye5 build/eye5_cli
build/eye: tools/eye_cli.c $(LIB) include/eye.h include/eye2.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tools/eye_cli.c $(LIB) -o $@
build/test_eye: tests/test_eye.c $(LIB) include/eye.h include/eye2.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/test_eye.c $(LIB) -o $@
build/test_eye2: tests/test_eye2.c $(LIB) include/eye.h include/eye2.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/test_eye2.c $(LIB) -o $@
build/test_bones_bridge: tests/test_bones_bridge.c $(LIB) include/eye_bones_bridge.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -Itests/mock_bones tests/test_bones_bridge.c $(LIB) -o $@
build/test_bones_adapter_example: tests/test_bones_adapter_example.c examples/bones_legacy_integration.c $(LIB)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -Itests/mock_bones tests/test_bones_adapter_example.c examples/bones_legacy_integration.c $(LIB) -o $@
build/test_eye_tone: tests/test_eye_tone.c $(LIB) include/eye_tone.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/test_eye_tone.c $(LIB) -o $@
test: build/test_eye build/test_eye2 build/test_eye3 build/test_bones_bridge build/test_bones_adapter_example build/test_eye_tone build/test_eye4 build/test_eye5 build/test_eye5_embed
	./build/test_eye
	./build/test_eye2
	./build/test_eye3
	./build/test_bones_bridge
	./build/test_bones_adapter_example
	./build/test_eye_tone
	./build/test_eye4
	./build/test_eye5
	./build/test_eye5_embed
integration: demo
	$(CC) $(CFLAGS) $(INC) -Ibuild tests/consumer_embed.c $(LIB) -o build/consumer_embed
	./build/consumer_embed
demo: build/eye
	./build/eye compile assets/materiales_demo.eye.txt build/materiales_demo_legacy.eyep
	./build/eye compile2 assets/materiales_demo.eye.txt build/materiales_demo.eyep
	./build/eye inspect build/materiales_demo.eyep
	./build/eye atlas build/materiales_demo.eyep build/previews/atlas_demo.ppm
	./build/eye embed build/materiales_demo.eyep build/materiales_embebidos.h
clean:
	rm -f build/eye build/test_eye build/test_eye2 build/test_bones_bridge build/test_bones_adapter_example build/materiales_demo_legacy.eyep build/materiales_demo.eyep build/materiales_embebidos.h build/previews/*.ppm

# Developer preview: PPM originals generated in C (never shipped as textures).
build/eye_tone_preview: tools/eye_tone_preview.c $(LIB) include/eye_tone.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tools/eye_tone_preview.c $(LIB) -o $@
.PHONY: tonepreview
tonepreview: build/eye_tone_preview
	./build/eye_tone_preview build/materiales_demo.eyep build/previews/Eye2_normal.ppm build/previews/Eye2_apagado.ppm

# EYE3 architectural material tests and authoring preview.
build/test_eye3: tests/test_eye3.c $(LIB) include/eye3.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/test_eye3.c $(LIB) -o $@
build/eye_architecture_preview: tools/eye_architecture_preview.c $(LIB) include/eye3.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tools/eye_architecture_preview.c $(LIB) -o $@
.PHONY: architecture
architecture: build/eye build/eye_architecture_preview
	@mkdir -p build/previews
	./build/eye compile3 assets/materiales_arquitectura.eye.txt build/materiales_arquitectura.eyep
	./build/eye atlas build/materiales_arquitectura.eyep build/previews/atlas_arquitectura.ppm
	./build/eye_architecture_preview build/materiales_arquitectura.eyep build/previews/casa_arquitectura.ppm


# EYE4: 3 reconstruction approaches; developer utilities not linked into game.
LIB4 = $(LIB) src/eye4.c
build/test_eye4: tests/test_eye4.c $(LIB4) include/eye4.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/test_eye4.c $(LIB4) -o $@
build/eye4_house_preview: tools/eye4_house_preview.c $(LIB4) include/eye4.h
	@mkdir -p build/previews
	$(CC) $(CFLAGS) $(INC) tools/eye4_house_preview.c $(LIB4) -o $@
build/eye4_cli: tools/eye4_cli.c $(LIB4) include/eye4.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tools/eye4_cli.c $(LIB4) -o $@
.PHONY: eye4 eye4test

eye4test: build/test_eye4
	./build/test_eye4

eye4: build/eye4_house_preview build/eye4_cli
	./build/eye4_house_preview 0 build/previews/eye4_procedural.ppm
	./build/eye4_house_preview 1 build/previews/eye4_estatico.ppm
	./build/eye4_house_preview 2 build/previews/eye4_hibrido.ppm
	./build/eye4_cli build 0 build/materiales_procedural.eyep
	./build/eye4_cli build 1 build/materiales_estatico.eyep
	./build/eye4_cli build 2 build/materiales_hibrido.eyep
	./build/eye4_cli embed build/materiales_hibrido.eyep build/materiales_eye4_embebidos.h
build/eye4_materials_preview: tools/eye4_materials_preview.c $(LIB4) include/eye4.h
	@mkdir -p build/previews
	$(CC) $(CFLAGS) $(INC) tools/eye4_materials_preview.c $(LIB4) -o $@
.PHONY: eye4materials
eye4materials: build/eye4_materials_preview
	./build/eye4_materials_preview build/previews/eye4_muestras.ppm
build/test_eye4_embed: tests/test_eye4_embed.c $(LIB4) build/materiales_eye4_embebidos.h
	$(CC) $(CFLAGS) $(INC) -Ibuild tests/test_eye4_embed.c $(LIB4) -o $@
.PHONY: eye4integration
eye4integration: eye4 build/test_eye4_embed
	./build/test_eye4_embed

# EYE5: compact roof recipes + architectural mesh independent of Bones internals.
LIB5 = $(LIB4) src/eye5.c
build/test_eye5: tests/test_eye5.c $(LIB5) include/eye5.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/test_eye5.c $(LIB5) -o $@
build/eye5_preview: tools/eye5_preview.c $(LIB5) include/eye5.h
	@mkdir -p build/previews
	$(CC) $(CFLAGS) $(INC) tools/eye5_preview.c $(LIB5) -lm -o $@
.PHONY: eye5 eye5test eye5_all_test
eye5test: build/test_eye5
	./build/test_eye5
eye5: build/eye5_preview eye5test
	@mkdir -p build/previews
	./build/eye5_preview build/previews
eye5_all_test: test eye5test

build/eye5_cli: tools/eye5_cli.c $(LIB5) include/eye5.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tools/eye5_cli.c $(LIB5) -o $@
build/recetas_eye5_embebidas.h: build/eye5_preview build/eye5_cli
	@mkdir -p build/previews
	./build/eye5_preview build/previews
	./build/eye5_cli embed build/previews/recetas_cubiertas.eye5 $@
build/test_eye5_embed: tests/test_eye5_embed.c build/recetas_eye5_embebidas.h $(LIB5)
	$(CC) $(CFLAGS) $(INC) -Ibuild tests/test_eye5_embed.c $(LIB5) -o $@
.PHONY: eye5integration
eye5integration: build/test_eye5_embed
	./build/test_eye5_embed

.PHONY: eye5size
eye5size:
	@mkdir -p build
	$(CC) -std=c11 -Os -flto -ffunction-sections -fdata-sections -s -Wl,--gc-sections $(INC) tools/size_probe_eye4.c $(LIB4) -o build/size_eye4
	$(CC) -std=c11 -Os -flto -ffunction-sections -fdata-sections -s -Wl,--gc-sections $(INC) tools/size_probe_eye5.c $(LIB5) -o build/size_eye5
	@wc -c build/size_eye4 build/size_eye5
	@./build/size_eye4 && ./build/size_eye5

.PHONY: clean_eye5
clean_eye5:
	rm -f build/test_eye5 build/test_eye5_embed build/eye5_cli build/eye5_preview build/size_eye4 build/size_eye5 build/recetas_eye5_embebidas.h
