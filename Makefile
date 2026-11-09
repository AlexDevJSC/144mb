# SPDX-License-Identifier: MIT
BUILD_DIR ?= build
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
CFLAGS ?= -O2 -g
WARN_CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Werror
INCLUDES = -Iinclude

.PHONY: all check check-pe benchmark install
all: $(BUILD_DIR)/nt-native-probe

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/counter.o: src/ntdll/counter.c include/ntnative/counter.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) -c $< -o "$@"

$(BUILD_DIR)/nt-native-probe: src/tools/counter_probe.c $(BUILD_DIR)/counter.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o "$@"

$(BUILD_DIR)/counter-test: tests/counter_test.c $(BUILD_DIR)/counter.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) $^ \
	    -Wl,--wrap=clock_gettime $(LDLIBS) -o "$@"

check: $(BUILD_DIR)/counter-test $(BUILD_DIR)/nt-native-probe
	$(BUILD_DIR)/counter-test
	$(BUILD_DIR)/nt-native-probe

# These explicit flags keep the fixture freestanding, position independent
# within .text, and free of host sanitizer and library dependencies.
$(BUILD_DIR)/fixture.elf.o: tests/pe/fixture.c tests/pe/fixture_abi.h include/ntnative/counter.h | $(BUILD_DIR)
	$(CC) $(INCLUDES) $(WARN_CFLAGS) -O2 -ffreestanding -fno-pie \
	    -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	    -fno-toplevel-reorder -fno-reorder-blocks-and-partition -c $< -o "$@"

$(BUILD_DIR)/fixture.coff.o: $(BUILD_DIR)/fixture.elf.o
	$(OBJCOPY) -j .text -O pe-x86-64 "$<" "$@"

OBJCOPY ?= objcopy
OBJDUMP ?= objdump

$(BUILD_DIR)/nt-counter.exe: $(BUILD_DIR)/fixture.coff.o tests/pe/fixture.ld
	$(LD) -m i386pep -T tests/pe/fixture.ld --entry fixture_entry --subsystem console \
	    --no-insert-timestamp --disable-reloc-section --strip-all -o "$@" "$<"

$(BUILD_DIR)/fixture-embedded.o: tests/pe/embed.S $(BUILD_DIR)/nt-counter.exe
	$(CC) -DNT_PE_FIXTURE_PATH='"$(BUILD_DIR)/nt-counter.exe"' -c "$<" -o "$@"

$(BUILD_DIR)/pe-host: tests/pe/host.c $(BUILD_DIR)/fixture-embedded.o $(BUILD_DIR)/counter.o tests/pe/fixture_abi.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) -o "$@"

check-pe: $(BUILD_DIR)/pe-host
	$(OBJDUMP) -f $(BUILD_DIR)/nt-counter.exe
	$(BUILD_DIR)/pe-host

$(BUILD_DIR)/counter-bench: tests/bench/counter_bench.c $(BUILD_DIR)/counter.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o "$@"

benchmark: $(BUILD_DIR)/counter-bench
	$(BUILD_DIR)/counter-bench

install: $(BUILD_DIR)/nt-native-probe
	install -D -m 0755 "$<" "$(DESTDIR)$(BINDIR)/nt-native-probe"
