CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L
INC = -Iinclude -Itests/fixtures
SRC = src/skin.c src/save.c src/ui.c
.PHONY: all test preview sanitize clean
all: build/test_skin
build/test_skin: tests/test_skin.c tests/mock_bones.c $(SRC) include/skin/skin.h
	mkdir -p build
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_skin.c tests/mock_bones.c $(SRC) -lm
build/libskin.a: $(SRC) include/skin/skin.h
	mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c src/skin.c -o build/skin.o
	$(CC) $(CFLAGS) $(INC) -c src/save.c -o build/save.o
	$(CC) $(CFLAGS) $(INC) -c src/ui.c -o build/ui.o
	ar rcs $@ build/skin.o build/save.o build/ui.o
test: build/test_skin
	./build/test_skin
preview: test
sanitize:
	mkdir -p build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -fno-omit-frame-pointer $(INC) -o build/test_skin_asan tests/test_skin.c tests/mock_bones.c $(SRC) -lm
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build/test_skin_asan
clean:
	rm -f build/test_skin build/test_skin_asan build/*.o build/libskin.a build/skin_menu.ppm build/skin_pausa.ppm build/skin_hud.ppm build/skin_test_slot.*
