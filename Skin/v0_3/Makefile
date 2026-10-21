CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
MOD = ../../BONES/modules/skin/modkit.c
.PHONY: all test sanitize clean
all: test
build/test_modkit_ui: tests/test_modkit_ui.c $(MOD)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $^
test: build/test_modkit_ui build/test_eye_skin
	./build/test_modkit_ui
	./build/test_eye_skin
sanitize:
	mkdir -p build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -o build/test_modkit_ui_san tests/test_modkit_ui.c $(MOD)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build/test_modkit_ui_san
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -o build/test_eye_skin_san tests/test_eye_skin_integration.c $(MOD) tests/eye_052_fixture/eye_display.c
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build/test_eye_skin_san
clean:
	rm -rf build

.PHONY: preview
preview: build/preview_phase1
	./build/preview_phase1
build/preview_phase1: tools/preview_phase1.c $(MOD)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: eye_test
build/test_eye_skin: tests/test_eye_skin_integration.c $(MOD) tests/eye_052_fixture/eye_display.c
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $^
eye_test: build/test_eye_skin
	./build/test_eye_skin

