CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
MOD = ../../BONES/modules/skin/modkit.c
.PHONY: all test sanitize clean
all: test
build/test_modkit_ui: tests/test_modkit_ui.c $(MOD)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $^
test: build/test_modkit_ui
	./build/test_modkit_ui
sanitize:
	mkdir -p build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -o build/test_modkit_ui_san tests/test_modkit_ui.c $(MOD)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build/test_modkit_ui_san
clean:
	rm -rf build
