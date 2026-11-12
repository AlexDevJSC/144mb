# SPDX-License-Identifier: MIT
BUILD_DIR ?= build
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
CFLAGS ?= -O2 -g
WARN_CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Werror
INCLUDES = -Iinclude
PE_INCLUDES = $(INCLUDES) -Isrc/pe
MINGW_CC ?= x86_64-w64-mingw32-gcc
MINGW_DLLTOOL ?= x86_64-w64-mingw32-dlltool
PKG_CONFIG ?= pkg-config
DESKTOP_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags sdl2)
DESKTOP_LIBS ?= $(shell $(PKG_CONFIG) --libs sdl2)
GDI_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags cairo)
GDI_LIBS ?= $(shell $(PKG_CONFIG) --libs cairo)
FFI_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libffi)
FFI_LIBS ?= $(shell $(PKG_CONFIG) --libs libffi)
IMAGE_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags SDL2_image 2>/dev/null)
IMAGE_LIBS ?= $(shell $(PKG_CONFIG) --libs SDL2_image 2>/dev/null)
VKD3D_PACKAGES = 'libvkd3d >= 1.2' 'libvkd3d-utils >= 1.2' vulkan
VKD3D_CFLAGS ?= $(patsubst -I%,-isystem %,$(shell $(PKG_CONFIG) --cflags $(VKD3D_PACKAGES) 2>/dev/null))
VKD3D_LIBS ?= $(shell $(PKG_CONFIG) --libs $(VKD3D_PACKAGES) 2>/dev/null)
RUNTIME_PARTS = exports system global_memory unicode crt crt_stdio guest_files user_folders process virtual_memory nt_core_information dynamic_tls environment sync threads fluid crypto winsock nls registry raw_input exceptions \
                gdi_memory printer menus menu_view messages desktop desktop_resources controls \
                dialogs file_dialogs clipboard guest_thread display_config child_process shell console
RUNTIME_OBJECTS = $(addprefix $(BUILD_DIR)/runtime-,$(addsuffix .o,$(RUNTIME_PARTS))) $(BUILD_DIR)/runtime-context.o $(BUILD_DIR)/counter.o
RUNTIME_LIBS = -lm -ldl -lcrypto -pthread $(DESKTOP_LIBS) $(GDI_LIBS) $(FFI_LIBS)
ifneq ($(strip $(IMAGE_LIBS)),)
RUNTIME_PARTS += wic
RUNTIME_LIBS += $(IMAGE_LIBS)
$(BUILD_DIR)/runtime-exports.o: override CPPFLAGS += -DNT_HAVE_WIC
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-wic.o: src/runtime/wic.h
$(BUILD_DIR)/runtime-wic.o: src/runtime/user_folders.h src/runtime/winbase.h
$(BUILD_DIR)/runtime-wic.o: override CPPFLAGS += $(DESKTOP_CFLAGS) $(IMAGE_CFLAGS)
endif
ifneq ($(strip $(VKD3D_LIBS)),)
RUNTIME_PARTS += d3d12_linux dxgi_linux d3dcompiler dxcompiler streamline
RUNTIME_LIBS += $(VKD3D_LIBS)
VKD3D_OBJECTS = $(addprefix $(BUILD_DIR)/runtime-,d3d12_linux.o dxgi_linux.o d3dcompiler.o dxcompiler.o streamline.o)
$(BUILD_DIR)/runtime-exports.o: override CPPFLAGS += -DNT_HAVE_VKD3D
$(BUILD_DIR)/runtime-exports.o $(VKD3D_OBJECTS): src/runtime/graphics.h
$(VKD3D_OBJECTS): src/runtime/graphics_linux.h
$(VKD3D_OBJECTS): override CPPFLAGS += $(VKD3D_CFLAGS)
$(BUILD_DIR)/runtime-d3d12_linux.o: src/runtime/sync.h
$(BUILD_DIR)/runtime-dxgi_linux.o: src/runtime/desktop.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-d3dcompiler.o $(BUILD_DIR)/runtime-dxcompiler.o: src/runtime/shader.h
$(BUILD_DIR)/runtime-d3dcompiler.o $(BUILD_DIR)/runtime-dxcompiler.o: src/runtime/winbase.h
$(BUILD_DIR)/runtime-dxcompiler.o: src/runtime/dxcompiler_path.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-streamline.o: src/runtime/streamline.h
endif
PE_IMPORT_HEADERS = src/pe/fixture_import.h src/pe/pe_bytes.h include/ntnative/runtime.h
PE_IMAGE_HEADERS = src/pe/image.h src/pe/pe_bytes.h include/ntnative/guest_thread.h
PE_FIXTURE_LDFLAGS = -Wl,--entry,import_fixture_entry,--subsystem,console,--no-insert-timestamp \
                     -Wl,--disable-reloc-section,--disable-dynamicbase,--strip-all
PE_FIXTURE_CFLAGS = -O2 -ffreestanding -nostdlib -fno-ident -fno-stack-protector \
                    -fno-asynchronous-unwind-tables -fno-unwind-tables

.DEFAULT_GOAL := all
.PHONY: all check check-pe check-pe-import check-pe-image check-pe-external check-pe-native check-pe-kernel-user check-pe-native-core check-pe-named-events check-pe-teb-tls check-pe-memory check-pe-displayconfig check-pe-console check-pe-graphics check-pe-shaders check-pe-wic check-pe-platform check-pe-seh check-pe-isolated check-desktop benchmark install
all: $(BUILD_DIR)/nt-native-probe $(BUILD_DIR)/pe-import-audit $(BUILD_DIR)/pe-isolated-run $(BUILD_DIR)/nt-vulkan-probe

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/counter.o: src/ntdll/counter.c include/ntnative/counter.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) -c $< -o "$@"

$(BUILD_DIR)/runtime-%.o: src/runtime/%.c src/runtime/compat.h include/ntnative/runtime.h include/ntnative/counter.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) -pthread -c "$<" -o "$@"

$(BUILD_DIR)/runtime-desktop.o $(BUILD_DIR)/runtime-messages.o $(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-clipboard.o: src/runtime/desktop.h
$(BUILD_DIR)/runtime-desktop.o $(BUILD_DIR)/runtime-clipboard.o: override CPPFLAGS += $(DESKTOP_CFLAGS)
$(BUILD_DIR)/runtime-desktop.o $(BUILD_DIR)/runtime-menus.o $(BUILD_DIR)/runtime-menu_view.o: src/runtime/menu_view.h
$(BUILD_DIR)/runtime-menus.o $(BUILD_DIR)/runtime-menu_view.o: override CPPFLAGS += $(DESKTOP_CFLAGS)
$(BUILD_DIR)/runtime-menu_view.o: override CPPFLAGS += $(GDI_CFLAGS)
$(BUILD_DIR)/runtime-desktop.o $(BUILD_DIR)/runtime-desktop_resources.o $(BUILD_DIR)/runtime-controls.o \
    $(BUILD_DIR)/runtime-dialogs.o $(BUILD_DIR)/runtime-exports.o: src/runtime/desktop_resources.h
$(BUILD_DIR)/runtime-desktop_resources.o: src/pe/pe_bytes.h
$(BUILD_DIR)/runtime-desktop_resources.o $(BUILD_DIR)/runtime-controls.o: override CPPFLAGS += $(DESKTOP_CFLAGS)
$(BUILD_DIR)/runtime-controls.o: override CPPFLAGS += $(GDI_CFLAGS)
$(BUILD_DIR)/runtime-desktop.o $(BUILD_DIR)/runtime-controls.o $(BUILD_DIR)/runtime-dialogs.o \
    $(BUILD_DIR)/runtime-file_dialogs.o $(BUILD_DIR)/runtime-messages.o $(BUILD_DIR)/runtime-exports.o: src/runtime/dialogs.h
$(BUILD_DIR)/runtime-gdi_memory.o: override CPPFLAGS += $(GDI_CFLAGS)
$(BUILD_DIR)/runtime-crt_stdio.o: override CPPFLAGS += $(FFI_CFLAGS)
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-guest_files.o $(BUILD_DIR)/runtime-user_folders.o \
    $(BUILD_DIR)/runtime-file_dialogs.o: src/runtime/user_folders.h

$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-guest_files.o $(BUILD_DIR)/runtime-crt.o \
    $(BUILD_DIR)/runtime-process.o $(BUILD_DIR)/runtime-system.o $(BUILD_DIR)/runtime-fluid.o \
    $(BUILD_DIR)/runtime-threads.o $(BUILD_DIR)/runtime-environment.o $(BUILD_DIR)/runtime-global_memory.o \
    $(BUILD_DIR)/runtime-exceptions.o: src/runtime/winbase.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-threads.o $(BUILD_DIR)/runtime-sync.o \
    $(BUILD_DIR)/runtime-guest_files.o: src/runtime/sync.h
$(BUILD_DIR)/runtime-exports.o: src/runtime/dynamic_tls.h
$(BUILD_DIR)/runtime-exports.o: src/runtime/nt_core_information.h
$(BUILD_DIR)/runtime-threads.o: include/ntnative/guest_thread.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-system.o $(BUILD_DIR)/runtime-threads.o \
    $(BUILD_DIR)/runtime-guest_files.o: src/runtime/nt_services.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-guest_thread.o: include/ntnative/guest_thread.h
$(BUILD_DIR)/runtime-dynamic_tls.o: src/runtime/dynamic_tls.h include/ntnative/guest_thread.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-threads.o $(BUILD_DIR)/runtime-child_process.o: src/runtime/child_process.h
$(BUILD_DIR)/runtime-child_process.o: src/runtime/winbase.h src/runtime/winfile.h src/pe/pe_bytes.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-shell.o: src/runtime/shell.h
$(BUILD_DIR)/runtime-shell.o: src/runtime/child_process.h src/runtime/dialogs.h src/runtime/winfile.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-guest_files.o $(BUILD_DIR)/runtime-dialogs.o \
    $(BUILD_DIR)/runtime-console.o: src/runtime/console.h
$(BUILD_DIR)/runtime-console.o: src/runtime/dialogs.h src/runtime/windesktop.h src/runtime/winbase.h src/runtime/winfile.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-guest_files.o: src/runtime/winfile.h
$(BUILD_DIR)/runtime-process.o: src/runtime/winfile.h include/ntnative/guest_thread.h
$(BUILD_DIR)/runtime-virtual_memory.o: src/runtime/virtual_memory.h
$(BUILD_DIR)/runtime-nt_core_information.o: src/runtime/nt_core_information.h src/runtime/virtual_memory.h include/ntnative/guest_thread.h
$(BUILD_DIR)/runtime-process.o $(BUILD_DIR)/runtime-exports.o: src/runtime/virtual_memory.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-fluid.o: src/runtime/fluid_api.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-crypto.o: src/runtime/crypto.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-winsock.o: src/runtime/winsock.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-nls.o: src/runtime/nls.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-gdi_memory.o: src/runtime/gdi_ext.h
$(BUILD_DIR)/runtime-gdi_memory.o: src/runtime/desktop.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-registry.o: src/runtime/registry.h
$(BUILD_DIR)/runtime-registry.o: src/runtime/winbase.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-desktop.o $(BUILD_DIR)/runtime-raw_input.o: src/runtime/raw_input.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-display_config.o: src/runtime/display_config.h
$(BUILD_DIR)/runtime-display_config.o: override CPPFLAGS += $(DESKTOP_CFLAGS)
$(BUILD_DIR)/runtime-raw_input.o: src/runtime/windesktop.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-dialogs.o: src/runtime/task_dialog.h

$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-desktop.o $(BUILD_DIR)/runtime-desktop_resources.o \
    $(BUILD_DIR)/runtime-clipboard.o $(BUILD_DIR)/runtime-messages.o: src/runtime/windesktop.h

$(BUILD_DIR)/runtime-context.o: src/runtime/context.S | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/pe-import-audit: src/tools/pe_import_audit.c src/pe/imports.c $(RUNTIME_OBJECTS) \
    src/pe/imports.h src/pe/pe_bytes.h include/ntnative/runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PE_INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/nt-vulkan-probe: src/tools/vulkan_probe.c src/runtime/vulkan_probe.c include/ntnative/vulkan_probe.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    src/tools/vulkan_probe.c src/runtime/vulkan_probe.c -ldl $(LDLIBS) -o "$@"

$(BUILD_DIR)/vulkan-probe-test: tests/vulkan_probe_test.c src/runtime/vulkan_probe.c include/ntnative/vulkan_probe.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    tests/vulkan_probe_test.c src/runtime/vulkan_probe.c -ldl $(LDLIBS) -o "$@"

$(BUILD_DIR)/vk-mock/libvulkan.so.1: tests/vulkan_mock_loader.c | $(BUILD_DIR)
	mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(WARN_CFLAGS) $(CFLAGS) -fPIC -shared "$<" $(LDFLAGS) -o "$@"

$(BUILD_DIR)/nt-native-probe: src/tools/counter_probe.c $(BUILD_DIR)/counter.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o "$@"

$(BUILD_DIR)/counter-test: tests/counter_test.c $(BUILD_DIR)/counter.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) $^ \
	    -Wl,--wrap=clock_gettime $(LDLIBS) -o "$@"

$(BUILD_DIR)/guest-thread-test: tests/guest_thread_test.c src/runtime/guest_thread.c include/ntnative/guest_thread.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    tests/guest_thread_test.c src/runtime/guest_thread.c -pthread $(LDLIBS) -o "$@"

$(BUILD_DIR)/guest-profile-test: tests/guest_profile_test.c src/tools/guest_profile.c src/tools/guest_profile.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc/tools $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    tests/guest_profile_test.c src/tools/guest_profile.c $(LDLIBS) -o "$@"

$(BUILD_DIR)/native-audio-test: tests/native_audio_test.c $(RUNTIME_OBJECTS) src/runtime/compat.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/console-test: tests/console_test.c $(RUNTIME_OBJECTS) src/runtime/console.h src/runtime/winfile.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/runtime-test: tests/runtime_test.c $(RUNTIME_OBJECTS) include/ntnative/runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/compat-batch-test: tests/compat_batch_test.c $(RUNTIME_OBJECTS) src/runtime/compat.h include/ntnative/runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) -Wl,--wrap=clock_gettime $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/mines-services-test: tests/mines_services_test.c $(RUNTIME_OBJECTS) src/runtime/compat.h src/runtime/user_folders.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) -Wl,--wrap=fdatasync $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/desktop-test: tests/desktop_test.c $(RUNTIME_OBJECTS) src/runtime/desktop.h src/runtime/raw_input.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(DESKTOP_CFLAGS) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/dialog-test: tests/dialog_test.c $(RUNTIME_OBJECTS) src/runtime/dialogs.h \
    src/runtime/desktop_resources.h src/runtime/user_folders.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(DESKTOP_CFLAGS) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

check-desktop: $(BUILD_DIR)/desktop-test $(BUILD_DIR)/dialog-test $(BUILD_DIR)/pe-desktop-host
	timeout 20s $(BUILD_DIR)/desktop-test
	timeout 20s $(BUILD_DIR)/dialog-test
	timeout 20s $(BUILD_DIR)/pe-desktop-host

$(BUILD_DIR)/import-scan-test: tests/pe/import_scan_test.c src/pe/imports.c \
    src/pe/imports.h src/pe/pe_bytes.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PE_INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c,$^) $(LDLIBS) -o "$@"

check: $(BUILD_DIR)/counter-test $(BUILD_DIR)/nt-native-probe $(BUILD_DIR)/runtime-test \
       $(BUILD_DIR)/compat-batch-test $(BUILD_DIR)/mines-services-test $(BUILD_DIR)/import-scan-test \
       $(BUILD_DIR)/guest-thread-test $(BUILD_DIR)/guest-profile-test $(BUILD_DIR)/native-audio-test \
       $(BUILD_DIR)/console-test $(BUILD_DIR)/vulkan-probe-test $(BUILD_DIR)/vk-mock/libvulkan.so.1
	$(BUILD_DIR)/counter-test
	$(BUILD_DIR)/nt-native-probe
	$(BUILD_DIR)/runtime-test
	$(BUILD_DIR)/compat-batch-test
	$(BUILD_DIR)/mines-services-test
	$(BUILD_DIR)/import-scan-test
	$(BUILD_DIR)/guest-thread-test
	$(BUILD_DIR)/guest-profile-test
	$(BUILD_DIR)/native-audio-test
	timeout 20s $(BUILD_DIR)/console-test
	$(BUILD_DIR)/vulkan-probe-test

	NT_VULKAN_MOCK_CASE=ready NT_VULKAN_EXPECT_STATE=6 LD_LIBRARY_PATH="$(abspath $(BUILD_DIR)/vk-mock)" $(BUILD_DIR)/vulkan-probe-test
	NT_VULKAN_MOCK_CASE=old NT_VULKAN_EXPECT_STATE=5 LD_LIBRARY_PATH="$(abspath $(BUILD_DIR)/vk-mock)" $(BUILD_DIR)/vulkan-probe-test
	NT_VULKAN_MOCK_CASE=noswap NT_VULKAN_EXPECT_STATE=4 LD_LIBRARY_PATH="$(abspath $(BUILD_DIR)/vk-mock)" $(BUILD_DIR)/vulkan-probe-test
	NT_VULKAN_MOCK_CASE=empty NT_VULKAN_EXPECT_STATE=3 LD_LIBRARY_PATH="$(abspath $(BUILD_DIR)/vk-mock)" $(BUILD_DIR)/vulkan-probe-test

# These explicit flags keep the fixture freestanding, position independent
# within .text, and free of host sanitizer and library dependencies.
$(BUILD_DIR)/fixture.elf.o: tests/pe/fixture.c tests/pe/fixture_abi.h include/ntnative/counter.h include/ntnative/runtime.h | $(BUILD_DIR)
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

$(BUILD_DIR)/pe-host: tests/pe/host.c $(BUILD_DIR)/fixture-embedded.o $(BUILD_DIR)/counter.o \
    tests/pe/fixture_abi.h src/pe/pe_bytes.h include/ntnative/runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PE_INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) -o "$@"

check-pe: $(BUILD_DIR)/pe-host
	$(OBJDUMP) -f $(BUILD_DIR)/nt-counter.exe
	$(BUILD_DIR)/pe-host

$(BUILD_DIR)/pe-image-test: tests/pe/image_test.c src/pe/image.c $(PE_IMAGE_HEADERS) include/ntnative/runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PE_INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    tests/pe/image_test.c src/pe/image.c $(LDLIBS) -o "$@"

check-pe-image: $(BUILD_DIR)/pe-image-test
	$(BUILD_DIR)/pe-image-test

$(BUILD_DIR)/pe-external-host: tests/pe/external_host.c src/pe/image.c $(RUNTIME_OBJECTS) \
    $(PE_IMAGE_HEADERS) src/runtime/winbase.h src/runtime/sync.h include/ntnative/guest_thread.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PE_INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    tests/pe/external_host.c src/pe/image.c $(RUNTIME_OBJECTS) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

check-pe-external: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/win32-desktop.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/win32-desktop.exe

$(BUILD_DIR)/console-fixture.exe: tests/pe/console_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) \
	    $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -lkernel32

check-pe-console: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/console-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 25s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/console-fixture.exe

$(BUILD_DIR)/displayconfig-fixture.exe: tests/pe/displayconfig_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) \
	    $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -luser32

check-pe-displayconfig: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/displayconfig-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 25s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/displayconfig-fixture.exe

ifneq ($(strip $(VKD3D_LIBS)),)
$(BUILD_DIR)/fixture-streamline-graphics.a: tests/pe/streamline-graphics.def | $(BUILD_DIR)
	$(MINGW_DLLTOOL) -d "$<" -l "$@"

$(BUILD_DIR)/graphics-fixture.exe: tests/pe/graphics_fixture.c $(BUILD_DIR)/fixture-streamline-graphics.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" $^ -lkernel32

check-pe-graphics: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/graphics-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 30s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/graphics-fixture.exe

$(BUILD_DIR)/fixture-dxcompiler.a: tests/pe/dxcompiler.def | $(BUILD_DIR)
	$(MINGW_DLLTOOL) -d "$<" -l "$@"

$(BUILD_DIR)/shader-fixture.exe: tests/pe/shader_fixture.c $(BUILD_DIR)/fixture-dxcompiler.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" $^ -ld3dcompiler_47 -lkernel32

check-pe-shaders: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/shader-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 30s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/shader-fixture.exe
else
check-pe-graphics check-pe-shaders:
	@echo 'This check requires libvkd3d, libvkd3d-utils and Vulkan development files.' >&2
	@exit 1
endif

ifneq ($(strip $(IMAGE_LIBS)),)
$(BUILD_DIR)/wic-fixture.exe: tests/pe/wic_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" $< -lole32 -lkernel32

check-pe-wic: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/wic-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/wic-fixture.exe
else
check-pe-wic:
	@echo 'check-pe-wic requires SDL2_image development files.' >&2
	@exit 1
endif

check-pe-native: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/native-services.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 25s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/native-services.exe

$(BUILD_DIR)/fixture-kernel_sync.a $(BUILD_DIR)/fixture-kernel_base.a \
    $(BUILD_DIR)/fixture-user_rect.a $(BUILD_DIR)/fixture-user_native.a: $(BUILD_DIR)/fixture-%.a: tests/pe/%.def | $(BUILD_DIR)
	$(MINGW_DLLTOOL) -d "$<" -l "$@"

$(BUILD_DIR)/kernel-services.exe: tests/pe/kernel_services_fixture.c $(BUILD_DIR)/fixture-kernel_sync.a $(BUILD_DIR)/fixture-kernel_base.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" $^ -lntdll -lkernel32

$(BUILD_DIR)/user-services.exe: tests/pe/user_services_fixture.c $(BUILD_DIR)/fixture-user_rect.a $(BUILD_DIR)/fixture-user_native.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" $^ -luser32 -lkernel32

check-pe-kernel-user: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/kernel-services.exe $(BUILD_DIR)/user-services.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/kernel-services.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/user-services.exe

$(BUILD_DIR)/teb-tls-fixture.exe: tests/pe/teb_tls_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -lkernel32

check-pe-teb-tls: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/teb-tls-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/teb-tls-fixture.exe

$(BUILD_DIR)/fixture-nt_core.a: tests/pe/nt_core.def | $(BUILD_DIR)
	$(MINGW_DLLTOOL) -d "$<" -l "$@"

$(BUILD_DIR)/named-event-fixture.exe: tests/pe/named_event_fixture.c $(BUILD_DIR)/fixture-nt_core.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" $(BUILD_DIR)/fixture-nt_core.a -lkernel32

check-pe-named-events: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/named-event-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/named-event-fixture.exe

$(BUILD_DIR)/nt-core-fixture.exe: tests/pe/nt_core_fixture.c $(BUILD_DIR)/fixture-nt_core.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" $(BUILD_DIR)/fixture-nt_core.a -lkernel32

check-pe-native-core: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/nt-core-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/nt-core-fixture.exe

$(BUILD_DIR)/virtual-memory-fixture.exe: tests/pe/virtual_memory_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -lkernel32

check-pe-memory: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/virtual-memory-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/virtual-memory-fixture.exe

$(BUILD_DIR)/fixture-ws2_32.a: tests/pe/ws2_32.def | $(BUILD_DIR)
	$(MINGW_DLLTOOL) -d "$<" -l "$@"

$(BUILD_DIR)/native-services.exe: tests/pe/native_services_fixture.c $(BUILD_DIR)/fixture-ws2_32.a $(BUILD_DIR)/fixture-comctl32.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) \
	    $(PE_FIXTURE_LDFLAGS) -o "$@" $^ -lbcrypt -lshell32 -lole32 -luser32 -lgdi32 -lmsimg32 -ladvapi32 -limm32 -lkernel32

$(BUILD_DIR)/platform-fixture.exe: tests/pe/platform_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" $< -lshell32 -lole32 -luser32 -lkernel32

check-pe-platform: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/platform-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/platform-fixture.exe

$(BUILD_DIR)/fixture-seh.a: tests/pe/seh.def | $(BUILD_DIR)
	$(MINGW_DLLTOOL) -d "$<" -l "$@"

$(BUILD_DIR)/seh-fixture.exe: tests/pe/seh_fixture.S $(BUILD_DIR)/fixture-seh.a | $(BUILD_DIR)
	$(MINGW_CC) -nostdlib $(PE_FIXTURE_LDFLAGS) -o "$@" $^ -lkernel32

check-pe-seh: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/seh-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/seh-fixture.exe

$(BUILD_DIR)/child-parent-fixture.exe: tests/pe/child_process_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" $< -lkernel32

$(BUILD_DIR)/child-fixture.exe: tests/pe/child_process_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) -DNT_CHILD_FIXTURE $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" $< -lkernel32

$(BUILD_DIR)/command-fixture.exe: tests/pe/command_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" $< -lshell32 -lkernel32

check-pe-isolated: $(BUILD_DIR)/pe-isolated-run $(BUILD_DIR)/win32-desktop.exe $(BUILD_DIR)/profile-fixture.exe \
    $(BUILD_DIR)/folder-fixture.exe $(BUILD_DIR)/child-parent-fixture.exe $(BUILD_DIR)/child-fixture.exe \
    $(BUILD_DIR)/command-fixture.exe
	sh tests/pe/run_external_fixture.sh $(BUILD_DIR)/pe-isolated-run $(BUILD_DIR)/win32-desktop.exe \
	    $(BUILD_DIR)/profile-fixture.exe $(BUILD_DIR)/folder-fixture.exe \
	    $(BUILD_DIR)/child-parent-fixture.exe $(BUILD_DIR)/child-fixture.exe $(BUILD_DIR)/command-fixture.exe

$(BUILD_DIR)/pe-isolated-run: src/tools/pe_isolated_run.c src/tools/guest_profile.c src/pe/image.c $(RUNTIME_OBJECTS) \
    $(PE_IMAGE_HEADERS) src/tools/guest_profile.h src/runtime/compat.h src/runtime/desktop.h src/runtime/desktop_resources.h \
    src/runtime/user_folders.h src/runtime/winbase.h src/runtime/winfile.h src/runtime/sync.h \
    src/runtime/child_process.h src/runtime/dxcompiler_path.h include/ntnative/guest_thread.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PE_INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    src/tools/pe_isolated_run.c src/tools/guest_profile.c src/pe/image.c $(RUNTIME_OBJECTS) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"


$(BUILD_DIR)/nt-counter-import.exe: tests/pe/import_fixture.c include/ntnative/counter.h | $(BUILD_DIR)
	$(MINGW_CC) $(INCLUDES) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) \
	    $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -lntdll

$(BUILD_DIR)/win32-counter-import.exe: tests/pe/win32_counter_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) \
	    $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -lkernel32

$(BUILD_DIR)/profile-fixture.exe: tests/pe/profile_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) \
	    $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -lucrt

$(BUILD_DIR)/folder-fixture.exe: tests/pe/folder_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) \
	    $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -lshell32 -lole32 -lucrt

$(BUILD_DIR)/fixture-comctl32.a: tests/pe/comctl32.def | $(BUILD_DIR)
	$(MINGW_DLLTOOL) -d "$<" -l "$@"

$(BUILD_DIR)/win32-desktop.exe: tests/pe/desktop_fixture.c $(BUILD_DIR)/fixture-comctl32.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) \
	    $(PE_FIXTURE_LDFLAGS) -o "$@" $^ -luser32 -lgdi32 -lcomdlg32

$(BUILD_DIR)/import-embedded.o: tests/pe/embed.S $(BUILD_DIR)/nt-counter-import.exe
	$(CC) -DNT_PE_FIXTURE_PATH='"$(BUILD_DIR)/nt-counter-import.exe"' -c "$<" -o "$@"

$(BUILD_DIR)/win32-import-embedded.o: tests/pe/embed.S $(BUILD_DIR)/win32-counter-import.exe
	$(CC) -DNT_PE_FIXTURE_PATH='"$(BUILD_DIR)/win32-counter-import.exe"' -c "$<" -o "$@"

$(BUILD_DIR)/desktop-embedded.o: tests/pe/embed.S $(BUILD_DIR)/win32-desktop.exe
	$(CC) -DNT_PE_FIXTURE_PATH='"$(BUILD_DIR)/win32-desktop.exe"' -c "$<" -o "$@"

$(BUILD_DIR)/pe-import-host: tests/pe/import_host.c src/pe/fixture_import.c \
    $(BUILD_DIR)/import-embedded.o $(RUNTIME_OBJECTS) $(PE_IMPORT_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PE_INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/pe-win32-import-host: tests/pe/import_host.c src/pe/fixture_import.c \
    $(BUILD_DIR)/win32-import-embedded.o $(RUNTIME_OBJECTS) $(PE_IMPORT_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PE_INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) -DTEST_WIN32_COUNTER \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/pe-desktop-host: tests/pe/import_host.c src/pe/fixture_import.c \
    $(BUILD_DIR)/desktop-embedded.o $(RUNTIME_OBJECTS) $(PE_IMPORT_HEADERS) src/runtime/desktop.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PE_INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) -DTEST_DESKTOP \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

check-pe-import: $(BUILD_DIR)/pe-import-host $(BUILD_DIR)/pe-win32-import-host $(BUILD_DIR)/pe-import-audit
	$(OBJDUMP) -h -p $(BUILD_DIR)/nt-counter-import.exe $(BUILD_DIR)/win32-counter-import.exe
	$(BUILD_DIR)/pe-import-host
	$(BUILD_DIR)/pe-win32-import-host
	$(BUILD_DIR)/pe-import-audit $(BUILD_DIR)/nt-counter-import.exe
	$(BUILD_DIR)/pe-import-audit $(BUILD_DIR)/win32-counter-import.exe

$(BUILD_DIR)/counter-bench: tests/bench/counter_bench.c $(BUILD_DIR)/counter.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o "$@"

benchmark: $(BUILD_DIR)/counter-bench
	$(BUILD_DIR)/counter-bench

install: $(BUILD_DIR)/nt-native-probe $(BUILD_DIR)/pe-isolated-run $(BUILD_DIR)/nt-vulkan-probe $(BUILD_DIR)/pe-import-audit
	install -D -m 0755 "$(BUILD_DIR)/nt-native-probe" "$(DESTDIR)$(BINDIR)/nt-native-probe"
	install -D -m 0755 "$(BUILD_DIR)/pe-isolated-run" "$(DESTDIR)$(BINDIR)/pe-isolated-run"
	install -D -m 0755 "$(BUILD_DIR)/nt-vulkan-probe" "$(DESTDIR)$(BINDIR)/nt-vulkan-probe"
	install -D -m 0755 "$(BUILD_DIR)/pe-import-audit" "$(DESTDIR)$(BINDIR)/pe-import-audit"
