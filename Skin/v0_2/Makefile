CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L
INC = -Iinclude -Itests/fixtures
SRC = src/skin.c src/save.c src/ui.c src/ui_font.c src/ui2.c
.PHONY: all test preview sanitize clean bench
all: build/test_skin
build/test_skin: tests/test_skin.c tests/mock_bones.c $(SRC) include/skin/skin.h
	mkdir -p build
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_skin.c tests/mock_bones.c $(SRC) -lm
build/libskin.a: $(SRC) include/skin/skin.h include/skin/ui2.h
	mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c src/skin.c -o build/skin.o
	$(CC) $(CFLAGS) $(INC) -c src/save.c -o build/save.o
	$(CC) $(CFLAGS) $(INC) -c src/ui.c -o build/ui.o
	$(CC) $(CFLAGS) $(INC) -c src/ui_font.c -o build/ui_font.o
	$(CC) $(CFLAGS) $(INC) -c src/ui2.c -o build/ui2.o
	ar rcs $@ build/skin.o build/save.o build/ui.o build/ui_font.o build/ui2.o
build/test_ui2: tests/test_ui2.c src/ui2.c src/ui_font.c include/skin/ui2.h
	mkdir -p build
	$(CC) $(CFLAGS) -Iinclude -o $@ tests/test_ui2.c src/ui2.c src/ui_font.c
build/ui2_preview: tools/ui2_preview.c src/ui2.c src/ui_font.c include/skin/ui2.h
	mkdir -p build
	$(CC) $(CFLAGS) -Iinclude -o $@ tools/ui2_preview.c src/ui2.c src/ui_font.c
build/bench_base: tools/bench_ui2.c
	mkdir -p build
	$(CC) -std=c11 -Os -ffunction-sections -fdata-sections -Wl,--gc-sections -DSKIN_BENCH_NO_UI -o $@ tools/bench_ui2.c
build/bench_ui2: tools/bench_ui2.c src/ui2.c src/ui_font.c include/skin/ui2.h
	mkdir -p build
	$(CC) -std=c11 -Os -ffunction-sections -fdata-sections -Wl,--gc-sections -Iinclude -o $@ tools/bench_ui2.c src/ui2.c src/ui_font.c
bench: build/bench_base build/bench_ui2
	@wc -c build/bench_base build/bench_ui2
preview: build/ui2_preview
	./build/ui2_preview
test: build/test_skin build/test_ui2
	./build/test_skin
	./build/test_ui2
preview: test
sanitize:
	mkdir -p build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -fno-omit-frame-pointer $(INC) -o build/test_skin_asan tests/test_skin.c tests/mock_bones.c $(SRC) -lm
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude -o build/test_ui2_asan tests/test_ui2.c src/ui2.c src/ui_font.c
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build/test_skin_asan
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build/test_ui2_asan
clean:
	rm -f build/test_skin build/test_skin_asan build/*.o build/libskin.a build/skin_menu.ppm build/skin_pausa.ppm build/skin_hud.ppm build/skin_test_slot.* build/test_ui2 build/test_ui2_asan build/ui2_preview build/bench_base build/bench_ui2 build/ui2_*.ppm
