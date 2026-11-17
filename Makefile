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
ICU_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags icu-uc)
ICU_LIBS ?= $(shell $(PKG_CONFIG) --libs icu-uc)
FUSE3_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags fuse3 2>/dev/null)
FUSE3_LIBS ?= $(shell $(PKG_CONFIG) --libs fuse3 2>/dev/null)
ifneq ($(strip $(FUSE3_LIBS)),)
DOS83_FUSE_TARGET = $(BUILD_DIR)/nt-dos83-fuse
else
DOS83_FUSE_TARGET =
endif
RUNTIME_PARTS = guest_boundary exports system global_memory unicode crt crt_stdio guest_files user_folders process virtual_memory section_memory object_broker nt_core_information dynamic_tls environment sync threads fluid crypto winsock nls registry raw_input exceptions exception_callbacks \
                gdi_memory printer menus menu_view messages desktop desktop_resources controls \
                dialogs file_dialogs clipboard guest_thread display_config child_process thread_broker_auth thread_broker_registry thread_broker_ipc shell console
RUNTIME_OBJECTS = $(addprefix $(BUILD_DIR)/runtime-,$(addsuffix .o,$(RUNTIME_PARTS))) $(BUILD_DIR)/runtime-context.o $(BUILD_DIR)/counter.o $(BUILD_DIR)/runtime-thread-supervisor-core.o $(BUILD_DIR)/runtime-supervisor-nt-bridge.o $(BUILD_DIR)/runtime-guest_boundary_import.o
RUNTIME_LIBS = -lm -ldl -lcrypto -pthread $(DESKTOP_LIBS) $(GDI_LIBS) $(FFI_LIBS) $(ICU_LIBS)
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
.PHONY: check-fs-supervisor check-pe-lstrcmp check-audio-required check-pe-preflight check-pe-priority-boost all check check-object-broker check-dos83-mediator check-dos83-fuse check-pe check-pe-import check-pe-image check-pe-external check-pe-native check-pe-kernel-user check-pe-nt-io check-pe-core-objects check-pe-apc check-pe-dll-graph check-pe-native-core check-pe-named-events check-pe-veh check-pe-suspended-thread check-pe-thread-groups check-pe-priority-class check-pe-process-affinity check-pe-teb-tls check-pe-memory check-pe-displayconfig check-pe-console check-pe-graphics check-pe-shaders check-pe-wic check-pe-platform check-pe-seh check-pe-isolated check-desktop benchmark install
all: $(BUILD_DIR)/nt-native-probe $(BUILD_DIR)/pe-import-audit $(BUILD_DIR)/pe-isolated-run $(BUILD_DIR)/nt-vulkan-probe $(BUILD_DIR)/nt-dos83-mediator $(BUILD_DIR)/nt-dos83-landlock $(BUILD_DIR)/nt-object-broker $(DOS83_FUSE_TARGET)

.PHONY: check-thread-supervisor
# Experimental ptrace proof: separate from standard release and RPM targets.
check-thread-supervisor:
	BUILD_DIR="$(BUILD_DIR)" CFLAGS="$(CFLAGS)" CC="$(CC)" bash scripts/check-thread-supervisor.sh

# Controlled *real* PE32+ integration, still opt-in and never shipped.
# Uses the actual validated image loader and guest GS/TEB setup. This is
# a dedicated fixture ABI, not general Windows executable support.
$(BUILD_DIR)/supervisor-pe-fixture.elf.o: tests/pe/supervisor_pe_fixture.c tests/pe/supervisor_pe_shared.h include/ntnative/guest_boundary.h | $(BUILD_DIR)
	$(CC) $(INCLUDES) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) -fno-pie \
	    -fno-toplevel-reorder -fno-reorder-blocks-and-partition -c "$<" -o "$@"

$(BUILD_DIR)/supervisor-pe-fixture.coff.o: $(BUILD_DIR)/supervisor-pe-fixture.elf.o
	$(OBJCOPY) -j .text -O pe-x86-64 "$<" "$@"

$(BUILD_DIR)/supervisor-pe-fixture.exe: $(BUILD_DIR)/supervisor-pe-fixture.coff.o tests/pe/fixture.ld
	$(LD) -m i386pep -T tests/pe/fixture.ld --entry supervisor_pe_entry \
	    --subsystem console --no-insert-timestamp --disable-reloc-section \
	    --strip-all -o "$@" "$<"

$(BUILD_DIR)/supervisor-pe-thread-test: tests/pe/supervisor_pe_thread_test.c tests/pe/supervisor_pe_shared.h src/supervisor/nt_thread_supervisor.c src/supervisor/nt_thread_supervisor.h src/supervisor/nt_supervisor_nt_bridge.c src/supervisor/nt_supervisor_nt_bridge.h src/runtime/compat.h src/runtime/nt_services.h include/ntnative/guest_boundary.h src/pe/image.c src/pe/image.h src/runtime/guest_thread.c src/runtime/guest_boundary.c src/runtime/guest_boundary_import.S include/ntnative/guest_thread.h include/ntnative/guest_boundary_runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/pe -Isrc/runtime -Isrc/supervisor -Itests/pe \
	    $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    tests/pe/supervisor_pe_thread_test.c src/supervisor/nt_thread_supervisor.c \
	    src/supervisor/nt_supervisor_nt_bridge.c src/pe/image.c src/runtime/guest_thread.c src/runtime/guest_boundary.c \
	    src/runtime/guest_boundary_import.S -pthread $(LDLIBS) -o "$@"

.PHONY: check-pe-thread-supervisor
check-pe-thread-supervisor: $(BUILD_DIR)/supervisor-pe-thread-test $(BUILD_DIR)/supervisor-pe-fixture.exe
	timeout 20s $(BUILD_DIR)/supervisor-pe-thread-test $(BUILD_DIR)/supervisor-pe-fixture.exe

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/counter.o: src/ntdll/counter.c include/ntnative/counter.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) -c $< -o "$@"

$(BUILD_DIR)/runtime-guest_boundary_import.o: src/runtime/guest_boundary_import.S | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/runtime-supervisor-nt-bridge.o: src/supervisor/nt_supervisor_nt_bridge.c src/supervisor/nt_supervisor_nt_bridge.h src/supervisor/nt_thread_supervisor.h src/runtime/nt_services.h src/runtime/compat.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime -Isrc/supervisor $(WARN_CFLAGS) $(CFLAGS) -pthread -c "$<" -o "$@"

$(BUILD_DIR)/runtime-thread-supervisor-core.o: src/supervisor/nt_thread_supervisor.c src/supervisor/nt_thread_supervisor.h include/ntnative/guest_boundary.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) -pthread -c "$<" -o "$@"

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
$(BUILD_DIR)/runtime-guest_files.o: override CPPFLAGS += $(ICU_CFLAGS)
$(BUILD_DIR)/runtime-guest_files.o: src/runtime/dos83_alias.inc src/runtime/dos83_wal.inc src/runtime/junctions.inc src/runtime/junction_control.inc
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-guest_files.o $(BUILD_DIR)/runtime-virtual_memory.o $(BUILD_DIR)/runtime-section_memory.o: src/runtime/section_memory.h
$(BUILD_DIR)/runtime-section_memory.o $(BUILD_DIR)/runtime-threads.o $(BUILD_DIR)/runtime-object_broker.o: src/runtime/object_broker.h
$(BUILD_DIR)/runtime-exports.o: src/runtime/dynamic_tls.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-guest_thread.o $(BUILD_DIR)/runtime-threads.o \
    $(BUILD_DIR)/runtime-guest_boundary.o: include/ntnative/guest_boundary_runtime.h
$(BUILD_DIR)/runtime-exports.o: src/runtime/nt_core_information.h
$(BUILD_DIR)/runtime-threads.o: include/ntnative/guest_thread.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-system.o $(BUILD_DIR)/runtime-threads.o \
    $(BUILD_DIR)/runtime-guest_files.o: src/runtime/nt_services.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-guest_thread.o: include/ntnative/guest_thread.h
$(BUILD_DIR)/runtime-dynamic_tls.o: src/runtime/dynamic_tls.h include/ntnative/guest_thread.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-threads.o $(BUILD_DIR)/runtime-child_process.o: src/runtime/child_process.h
$(BUILD_DIR)/runtime-child_process.o: src/runtime/winbase.h src/runtime/winfile.h src/pe/pe_bytes.h
$(BUILD_DIR)/runtime-threads.o $(BUILD_DIR)/runtime-thread_broker_registry.o: src/runtime/thread_broker_registry.h src/runtime/thread_broker_auth.h
$(BUILD_DIR)/runtime-thread_broker_auth.o: src/runtime/thread_broker_auth.h
$(BUILD_DIR)/runtime-thread_broker_ipc.o $(BUILD_DIR)/runtime-threads.o $(BUILD_DIR)/runtime-child_process.o: src/runtime/thread_broker_ipc.h
$(BUILD_DIR)/runtime-thread_broker_ipc.o: src/runtime/thread_broker_registry.h src/supervisor/nt_thread_supervisor.h include/ntnative/guest_boundary.h include/ntnative/guest_boundary_runtime.h
$(BUILD_DIR)/runtime-process.o: src/runtime/thread_broker_ipc.h src/runtime/thread_broker_registry.h src/runtime/thread_broker_auth.h include/ntnative/guest_boundary_runtime.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-shell.o: src/runtime/shell.h
$(BUILD_DIR)/runtime-shell.o: src/runtime/child_process.h src/runtime/dialogs.h src/runtime/winfile.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-guest_files.o $(BUILD_DIR)/runtime-dialogs.o \
    $(BUILD_DIR)/runtime-console.o: src/runtime/console.h
$(BUILD_DIR)/runtime-console.o: src/runtime/dialogs.h src/runtime/windesktop.h src/runtime/winbase.h src/runtime/winfile.h
$(BUILD_DIR)/runtime-exports.o $(BUILD_DIR)/runtime-guest_files.o: src/runtime/winfile.h
$(BUILD_DIR)/runtime-process.o: src/runtime/winfile.h include/ntnative/guest_thread.h
$(BUILD_DIR)/runtime-virtual_memory.o: src/runtime/virtual_memory.h
$(BUILD_DIR)/runtime-nt_core_information.o: src/runtime/nt_core_information.h src/runtime/virtual_memory.h include/ntnative/guest_thread.h
$(BUILD_DIR)/runtime-exception_callbacks.o $(BUILD_DIR)/runtime-exceptions.o $(BUILD_DIR)/runtime-exports.o: src/runtime/exception_callbacks.h
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

$(BUILD_DIR)/guest-thread-test: tests/guest_thread_test.c src/runtime/guest_thread.c src/runtime/guest_boundary.c src/runtime/guest_boundary_import.S include/ntnative/guest_thread.h include/ntnative/guest_boundary_runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    tests/guest_thread_test.c src/runtime/guest_thread.c src/runtime/guest_boundary.c \
	    src/runtime/guest_boundary_import.S -pthread $(LDLIBS) -o "$@"

$(BUILD_DIR)/guest-profile-test: tests/guest_profile_test.c src/tools/guest_profile.c src/tools/guest_profile.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc/tools $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    tests/guest_profile_test.c src/tools/guest_profile.c $(LDLIBS) -o "$@"

$(BUILD_DIR)/native-audio-test: tests/native_audio_test.c $(RUNTIME_OBJECTS) src/runtime/compat.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

# Required Fedora runtime audio gate: optional make check remains portable
# but this explicitly fails if libfluidsynth.so.3 cannot be loaded.
check-audio-required: $(BUILD_DIR)/native-audio-test
	NT_REQUIRE_FLUIDSYNTH=1 $(BUILD_DIR)/native-audio-test

$(BUILD_DIR)/console-test: tests/console_test.c $(RUNTIME_OBJECTS) src/runtime/console.h src/runtime/winfile.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/process-thread-test: tests/process_thread_test.c $(RUNTIME_OBJECTS) src/runtime/sync.h src/runtime/nt_services.h src/runtime/child_process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/inherited-file-test: tests/inherited_file_test.c $(RUNTIME_OBJECTS) src/runtime/winfile.h src/runtime/child_process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/thread-affinity-test: tests/thread_affinity_test.c $(RUNTIME_OBJECTS) src/runtime/sync.h src/runtime/nt_services.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/thread-group-affinity-test: tests/thread_group_affinity_test.c $(RUNTIME_OBJECTS) src/runtime/sync.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/dynamic-priority-boost-test: tests/dynamic_priority_boost_test.c $(RUNTIME_OBJECTS) src/runtime/sync.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/process-priority-class-test: tests/process_priority_class_test.c $(RUNTIME_OBJECTS) src/runtime/sync.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/process-priority-multithread-test: tests/process_priority_multithread_test.c $(RUNTIME_OBJECTS) src/runtime/sync.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/priority-boost-boundary-test: tests/priority_boost_boundary_test.c $(RUNTIME_OBJECTS) src/runtime/sync.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/process-affinity-mask-test: tests/process_affinity_mask_test.c $(RUNTIME_OBJECTS) src/runtime/sync.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/process-group-inheritance-test: tests/process_group_inheritance_test.c $(RUNTIME_OBJECTS) src/runtime/sync.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/process-group-synthetic-80cpu-test: tests/process_group_synthetic_80cpu_test.c $(RUNTIME_OBJECTS) src/runtime/sync.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) -Wl,--wrap=sched_getaffinity \
	    -Wl,--wrap=pthread_getaffinity_np -Wl,--wrap=pthread_setaffinity_np \
	    -Wl,--wrap=sched_getcpu $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/external-thread-boundary-test: tests/external_thread_boundary_test.c $(RUNTIME_OBJECTS) src/runtime/sync.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/thread-broker-auth-test: tests/thread_broker_auth_test.c src/runtime/thread_broker_auth.c src/runtime/thread_broker_auth.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c,$^) -o "$@"

$(BUILD_DIR)/thread-broker-registry-test: tests/thread_broker_registry_test.c src/runtime/thread_broker_registry.c src/runtime/thread_broker_auth.c src/runtime/thread_broker_registry.h src/runtime/thread_broker_auth.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c,$^) -pthread -o "$@"

$(BUILD_DIR)/thread-broker-ipc-test: tests/thread_broker_ipc_test.c src/runtime/thread_broker_ipc.c src/runtime/thread_broker_registry.c src/runtime/thread_broker_auth.c src/supervisor/nt_thread_supervisor.c src/supervisor/nt_supervisor_nt_bridge.c src/runtime/guest_boundary.c src/runtime/guest_boundary_import.S src/runtime/thread_broker_ipc.h src/runtime/thread_broker_registry.h src/runtime/thread_broker_auth.h src/supervisor/nt_thread_supervisor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.S,$^) -Isrc/supervisor -pthread -o "$@"

$(BUILD_DIR)/guest-boundary-import-test: tests/guest_boundary_import_test.c src/runtime/guest_boundary.c src/runtime/guest_boundary_import.S include/ntnative/guest_boundary_runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    tests/guest_boundary_import_test.c src/runtime/guest_boundary.c \
	    src/runtime/guest_boundary_import.S -pthread -lm -o "$@"

.PHONY: check-guest-boundary
check-guest-boundary: $(BUILD_DIR)/guest-boundary-import-test
	timeout 20s $(BUILD_DIR)/guest-boundary-import-test

$(BUILD_DIR)/supervisor-authenticated-ipc-test: tests/supervisor_authenticated_ipc_test.c src/runtime/thread_broker_ipc.c src/runtime/thread_broker_registry.c src/runtime/thread_broker_auth.c src/supervisor/nt_thread_supervisor.c src/supervisor/nt_supervisor_nt_bridge.c src/runtime/guest_boundary.c src/runtime/guest_boundary_import.S src/runtime/thread_broker_ipc.h src/runtime/thread_broker_registry.h src/runtime/thread_broker_auth.h src/supervisor/nt_thread_supervisor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.S,$^) -Isrc/supervisor -pthread -o "$@"

.PHONY: check-supervisor-authenticated-ipc
check-supervisor-authenticated-ipc: $(BUILD_DIR)/supervisor-authenticated-ipc-test
	timeout 25s $(BUILD_DIR)/supervisor-authenticated-ipc-test

$(BUILD_DIR)/runtime-supervisor-launch-test: tests/runtime_supervisor_launch_test.c $(RUNTIME_OBJECTS) src/runtime/compat.h src/runtime/sync.h src/runtime/thread_broker_ipc.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

.PHONY: check-runtime-supervisor-launch
check-runtime-supervisor-launch: $(BUILD_DIR)/runtime-supervisor-launch-test
	timeout 25s $(BUILD_DIR)/runtime-supervisor-launch-test
	timeout 20s $(BUILD_DIR)/guest-boundary-import-test

$(BUILD_DIR)/runtime-test: tests/runtime_test.c $(RUNTIME_OBJECTS) include/ntnative/runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/nt-object-broker: src/tools/nt_object_broker.c src/runtime/object_broker.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $< $(LDLIBS) -pthread -o "$@"

$(BUILD_DIR)/object-broker-test: tests/object_broker_test.c $(RUNTIME_OBJECTS) src/runtime/object_broker.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

check-object-broker: $(BUILD_DIR)/nt-object-broker $(BUILD_DIR)/object-broker-test
	sh tests/object_broker_smoke.sh "$(BUILD_DIR)/nt-object-broker" "$(BUILD_DIR)/object-broker-test"

$(BUILD_DIR)/section-memory-test: tests/section_memory_test.c $(RUNTIME_OBJECTS) src/runtime/section_memory.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/compat-batch-test: tests/compat_batch_test.c $(RUNTIME_OBJECTS) src/runtime/compat.h include/ntnative/runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) -Wl,--wrap=clock_gettime -Wl,--wrap=pread $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

$(BUILD_DIR)/nt-dos83-landlock: src/tools/dos83_landlock.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o "$@"

ifneq ($(strip $(FUSE3_LIBS)),)
$(BUILD_DIR)/nt-dos83-fuse: src/tools/dos83_fuse.c $(RUNTIME_OBJECTS) src/runtime/compat.h src/runtime/winfile.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(FUSE3_CFLAGS) $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) $(FUSE3_LIBS) -o "$@"

check-dos83-fuse: $(BUILD_DIR)/nt-dos83-fuse $(BUILD_DIR)/nt-dos83-landlock $(BUILD_DIR)/nt-dos83-mediator
	sh tests/dos83_fuse_smoke.sh "$(BUILD_DIR)/nt-dos83-fuse"

check: check-dos83-fuse
endif

check-fs-supervisor:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_fs_supervisor.py'

check: check-fs-supervisor

.PHONY: check-fs-backing-audit check-fs4-acceptance-gates
check-fs-backing-audit:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_fs_backing_audit.py'

check-fs4-acceptance-gates:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_fs4_acceptance_gate.py'

check: check-fs-backing-audit check-fs4-acceptance-gates

$(BUILD_DIR)/fs4-ntfs-probe: src/tools/fs4_ntfs_probe.c $(RUNTIME_OBJECTS) src/runtime/winfile.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

.PHONY: check-fs4-ntfs-diff
check-fs4-ntfs-diff: $(BUILD_DIR)/fs4-ntfs-probe
	PYTHONDONTWRITEBYTECODE=1 python3 tests/fs4_ntfs_probe_smoke.py "$(BUILD_DIR)/fs4-ntfs-probe"
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_fs4_ntfs_diff.py'

check: check-fs4-ntfs-diff

$(BUILD_DIR)/nt-dos83-mediator: src/tools/dos83_mediator.c $(RUNTIME_OBJECTS) src/runtime/compat.h src/runtime/winfile.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

check-dos83-mediator: $(BUILD_DIR)/nt-dos83-mediator
	sh tests/dos83_mediator_smoke.sh "$(BUILD_DIR)/nt-dos83-mediator"

.PHONY: check-dos83-wal
check-dos83-wal: $(BUILD_DIR)/nt-dos83-mediator
	PYTHONDONTWRITEBYTECODE=1 python3 tests/dos83_wal_recovery.py "$(BUILD_DIR)/nt-dos83-mediator"

check: check-dos83-wal

# Compile crash hooks ONLY into this standalone test mediator. Production
# runtime/PE objects do not receive the injection macro, nor a trigger
# controllable by an untrusted guest program.
$(BUILD_DIR)/runtime-guest_files-crash.o: src/runtime/guest_files.c src/runtime/dos83_alias.inc src/runtime/dos83_wal.inc src/runtime/compat.h src/runtime/winfile.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(ICU_CFLAGS) $(WARN_CFLAGS) $(CFLAGS) \
	    -DNTNATIVE_TEST_DOS83_CRASH_HOOKS -pthread -c "$<" -o "$@"

$(BUILD_DIR)/nt-dos83-mediator-crash: src/tools/dos83_mediator.c $(filter-out $(BUILD_DIR)/runtime-guest_files.o,$(RUNTIME_OBJECTS)) $(BUILD_DIR)/runtime-guest_files-crash.o src/runtime/winfile.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    $(filter %.c %.o,$^) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

.PHONY: check-dos83-sigkill
check-dos83-sigkill: $(BUILD_DIR)/nt-dos83-mediator $(BUILD_DIR)/nt-dos83-mediator-crash
	PYTHONDONTWRITEBYTECODE=1 python3 tests/dos83_wal_sigkill.py \
	    "$(BUILD_DIR)/nt-dos83-mediator" "$(BUILD_DIR)/nt-dos83-mediator-crash"

check: check-dos83-sigkill

.PHONY: check-dos83-all-grants
check-dos83-all-grants: $(BUILD_DIR)/nt-dos83-mediator
	PYTHONDONTWRITEBYTECODE=1 python3 tests/dos83_wal_all_grants.py \
	    "$(BUILD_DIR)/nt-dos83-mediator"

check: check-dos83-all-grants

.PHONY: check-dos83-bad-source
check-dos83-bad-source: $(BUILD_DIR)/nt-dos83-mediator
	PYTHONDONTWRITEBYTECODE=1 python3 tests/dos83_wal_bad_source.py "$(BUILD_DIR)/nt-dos83-mediator"

check: check-dos83-bad-source

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

check: $(BUILD_DIR)/counter-test $(BUILD_DIR)/nt-native-probe $(BUILD_DIR)/runtime-test $(BUILD_DIR)/process-thread-test $(BUILD_DIR)/thread-affinity-test $(BUILD_DIR)/thread-group-affinity-test $(BUILD_DIR)/process-priority-class-test $(BUILD_DIR)/dynamic-priority-boost-test $(BUILD_DIR)/process-priority-multithread-test $(BUILD_DIR)/priority-boost-boundary-test $(BUILD_DIR)/process-affinity-mask-test $(BUILD_DIR)/process-group-inheritance-test $(BUILD_DIR)/process-group-synthetic-80cpu-test $(BUILD_DIR)/external-thread-boundary-test $(BUILD_DIR)/thread-broker-auth-test $(BUILD_DIR)/thread-broker-registry-test $(BUILD_DIR)/thread-broker-ipc-test $(BUILD_DIR)/supervisor-authenticated-ipc-test $(BUILD_DIR)/runtime-supervisor-launch-test $(BUILD_DIR)/guest-boundary-import-test $(BUILD_DIR)/inherited-file-test \
       $(BUILD_DIR)/compat-batch-test $(BUILD_DIR)/section-memory-test $(BUILD_DIR)/nt-object-broker $(BUILD_DIR)/object-broker-test $(BUILD_DIR)/mines-services-test $(BUILD_DIR)/import-scan-test \
       $(BUILD_DIR)/guest-thread-test $(BUILD_DIR)/guest-profile-test $(BUILD_DIR)/native-audio-test \
       $(BUILD_DIR)/console-test $(BUILD_DIR)/vulkan-probe-test $(BUILD_DIR)/vk-mock/libvulkan.so.1 $(BUILD_DIR)/nt-dos83-mediator
	$(BUILD_DIR)/counter-test
	$(BUILD_DIR)/nt-native-probe
	$(BUILD_DIR)/runtime-test
	timeout 20s $(BUILD_DIR)/process-thread-test
	timeout 20s $(BUILD_DIR)/thread-affinity-test
	timeout 20s $(BUILD_DIR)/thread-group-affinity-test
	timeout 20s $(BUILD_DIR)/process-priority-class-test
	timeout 20s $(BUILD_DIR)/dynamic-priority-boost-test
	timeout 20s $(BUILD_DIR)/process-priority-multithread-test
	timeout 20s $(BUILD_DIR)/priority-boost-boundary-test
	timeout 20s $(BUILD_DIR)/process-affinity-mask-test
	timeout 20s $(BUILD_DIR)/process-group-inheritance-test
	timeout 20s $(BUILD_DIR)/process-group-synthetic-80cpu-test
	timeout 20s $(BUILD_DIR)/external-thread-boundary-test
	timeout 20s $(BUILD_DIR)/thread-broker-auth-test
	timeout 20s $(BUILD_DIR)/thread-broker-registry-test
	timeout 20s $(BUILD_DIR)/thread-broker-ipc-test
	timeout 25s $(BUILD_DIR)/supervisor-authenticated-ipc-test
	timeout 25s $(BUILD_DIR)/runtime-supervisor-launch-test
	timeout 25s $(BUILD_DIR)/supervisor-authenticated-ipc-test
	timeout 20s $(BUILD_DIR)/inherited-file-test
	timeout 30s $(BUILD_DIR)/compat-batch-test
	timeout 30s $(BUILD_DIR)/section-memory-test
	sh tests/object_broker_smoke.sh "$(BUILD_DIR)/nt-object-broker" "$(BUILD_DIR)/object-broker-test"
	sh tests/dos83_mediator_smoke.sh "$(BUILD_DIR)/nt-dos83-mediator"
	timeout 30s $(BUILD_DIR)/mines-services-test
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

$(BUILD_DIR)/pe-external-host: tests/pe/external_host.c src/pe/image.c src/pe/dll_graph.c $(RUNTIME_OBJECTS) \
    $(PE_IMAGE_HEADERS) src/pe/dll_graph.h src/runtime/winbase.h src/runtime/sync.h include/ntnative/guest_thread.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PE_INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    tests/pe/external_host.c src/pe/image.c src/pe/dll_graph.c $(RUNTIME_OBJECTS) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"

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

$(BUILD_DIR)/fixture-nt_file_io.a: tests/pe/nt_file_io.def | $(BUILD_DIR)
	$(MINGW_DLLTOOL) -d "$<" -l "$@"

$(BUILD_DIR)/nt-file-io-fixture.exe: tests/pe/nt_file_io_fixture.c $(BUILD_DIR)/fixture-nt_file_io.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" $(BUILD_DIR)/fixture-nt_file_io.a -lkernel32

check-pe-nt-io: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/nt-file-io-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/nt-file-io-fixture.exe

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

$(BUILD_DIR)/fixture-nt_apc.a: tests/pe/nt_apc.def | $(BUILD_DIR)
	$(MINGW_DLLTOOL) -d "$<" -l "$@"

$(BUILD_DIR)/apc-fixture.exe: tests/pe/apc_fixture.c $(BUILD_DIR)/fixture-nt_apc.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" $(BUILD_DIR)/fixture-nt_apc.a -lkernel32

check-pe-apc: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/apc-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/apc-fixture.exe

$(BUILD_DIR)/guest_companion.dll: tests/pe/guest_companion_dll.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) -shared -nostdlib \
	    -Wl,--entry,DllMain,--subsystem,windows,--out-implib,$(BUILD_DIR)/fixture-guest_companion.a \
	    -o "$@" "$<" -lkernel32

$(BUILD_DIR)/guest_tls.dll: tests/pe/guest_tls_dll.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) -shared -nostdlib \
	    -Wl,--entry,DllMain,--subsystem,windows,--out-implib,$(BUILD_DIR)/fixture-guest_tls.a \
	    -o "$@" "$<" -lkernel32

$(BUILD_DIR)/guest-tls-fixture.exe: tests/pe/guest_tls_fixture.c $(BUILD_DIR)/guest_tls.dll | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" \
	    $(BUILD_DIR)/fixture-guest_tls.a -lkernel32

$(BUILD_DIR)/guest-loader-dynamic.exe: tests/pe/guest_loader_dynamic_fixture.c $(BUILD_DIR)/guest_companion.dll | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -lkernel32

$(BUILD_DIR)/guest-loader-static.exe: tests/pe/guest_loader_static_fixture.c $(BUILD_DIR)/guest_companion.dll | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" $(BUILD_DIR)/fixture-guest_companion.a -lkernel32

check-pe-dll-graph: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/guest-loader-dynamic.exe $(BUILD_DIR)/guest-loader-static.exe $(BUILD_DIR)/guest-tls-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/guest-loader-dynamic.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/guest-loader-static.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/guest-tls-fixture.exe

$(BUILD_DIR)/core-objects-fixture.exe: tests/pe/core_objects_fixture.c $(BUILD_DIR)/fixture-nt_core.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" $(BUILD_DIR)/fixture-nt_core.a -lkernel32

check-pe-core-objects: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/core-objects-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/core-objects-fixture.exe

$(BUILD_DIR)/suspended-thread-fixture.exe: tests/pe/suspended_thread_fixture.c $(BUILD_DIR)/fixture-nt_core.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" $(BUILD_DIR)/fixture-nt_core.a -lkernel32

check-pe-suspended-thread: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/suspended-thread-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/suspended-thread-fixture.exe

$(BUILD_DIR)/thread-group-affinity-fixture.exe: tests/pe/thread_group_affinity_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -lkernel32

check-pe-thread-groups: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/thread-group-affinity-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/thread-group-affinity-fixture.exe

$(BUILD_DIR)/priority-class-fixture.exe: tests/pe/priority_class_fixture.c $(BUILD_DIR)/fixture-nt_core.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" $(BUILD_DIR)/fixture-nt_core.a -lkernel32

check-pe-priority-class: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/priority-class-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/priority-class-fixture.exe

$(BUILD_DIR)/priority-boost-boundary-fixture.exe: tests/pe/priority_boost_boundary_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -lkernel32

check-pe-priority-boost: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/priority-boost-boundary-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/priority-boost-boundary-fixture.exe

$(BUILD_DIR)/process-affinity-mask-fixture.exe: tests/pe/process_affinity_mask_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -lkernel32

check-pe-process-affinity: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/process-affinity-mask-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/process-affinity-mask-fixture.exe

$(BUILD_DIR)/veh-fixture.exe: tests/pe/vectored_exception_fixture.S | $(BUILD_DIR)
	$(MINGW_CC) -nostdlib $(PE_FIXTURE_LDFLAGS) -o "$@" "$<" -lkernel32

check-pe-veh: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/veh-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/veh-fixture.exe

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

$(BUILD_DIR)/child-parent-fixture.exe: tests/pe/child_process_fixture.c $(BUILD_DIR)/fixture-nt_core.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" $< $(BUILD_DIR)/fixture-nt_core.a -lkernel32

$(BUILD_DIR)/child-fixture.exe: tests/pe/child_process_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) -DNT_CHILD_FIXTURE $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" $< -lkernel32

$(BUILD_DIR)/command-fixture.exe: tests/pe/command_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) -o "$@" $< -lshell32 -lkernel32

$(BUILD_DIR)/lstrcmp-fixture.exe: tests/pe/lstrcmp_fixture.c | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) \
	    -o "$@" "$<" -lkernel32

# Verify genuine MS x64 PE import resolution and execution on Linux.
check-pe-lstrcmp: $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/pe-import-audit $(BUILD_DIR)/lstrcmp-fixture.exe
	$(BUILD_DIR)/pe-import-audit --verify $(BUILD_DIR)/lstrcmp-fixture.exe
	SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 timeout 20s \
	    $(BUILD_DIR)/pe-external-host $(BUILD_DIR)/lstrcmp-fixture.exe

PE_ISOLATED_FIXTURES = $(BUILD_DIR)/win32-desktop.exe $(BUILD_DIR)/profile-fixture.exe \
    $(BUILD_DIR)/folder-fixture.exe $(BUILD_DIR)/child-parent-fixture.exe \
    $(BUILD_DIR)/child-fixture.exe $(BUILD_DIR)/command-fixture.exe

$(BUILD_DIR)/fixture-missing-import.a: tests/pe/missing_import_fixture.def | $(BUILD_DIR)
	$(MINGW_DLLTOOL) -d "$<" -l "$@"

$(BUILD_DIR)/missing-import-fixture.exe: tests/pe/missing_import_fixture.c $(BUILD_DIR)/fixture-missing-import.a | $(BUILD_DIR)
	$(MINGW_CC) $(WARN_CFLAGS) $(PE_FIXTURE_CFLAGS) $(PE_FIXTURE_LDFLAGS) \
	    -o "$@" $^

# Preflight runs even on hosts where Bubblewrap cannot create namespaces.
# Direct imports are checked without executing untrusted PE code; this does
# not claim coverage of delay imports, LoadLibrary or dependent DLL semantics.
check-pe-preflight: $(BUILD_DIR)/pe-import-audit $(PE_ISOLATED_FIXTURES) $(BUILD_DIR)/lstrcmp-fixture.exe $(BUILD_DIR)/missing-import-fixture.exe
	$(BUILD_DIR)/pe-import-audit --verify $(PE_ISOLATED_FIXTURES) $(BUILD_DIR)/lstrcmp-fixture.exe
	@status=0; $(BUILD_DIR)/pe-import-audit --verify $(BUILD_DIR)/missing-import-fixture.exe \
	    > $(BUILD_DIR)/missing-import-audit.log 2>&1 || status=$?; \
	    if [ "$status" -ne 1 ] || ! grep -q 'MissingNtNativeApi' $(BUILD_DIR)/missing-import-audit.log; then \
	        cat $(BUILD_DIR)/missing-import-audit.log; \
	        echo "FAIL: auditor did not reject unsupported KERNEL32 import" >&2; exit 1; \
	    fi; \
	    echo "PASS PE import preflight rejects a genuinely missing runtime API"

check-pe-isolated: check-pe-preflight $(BUILD_DIR)/pe-isolated-run
	sh tests/pe/run_external_fixture.sh $(BUILD_DIR)/pe-isolated-run $(BUILD_DIR)/win32-desktop.exe \
	    $(BUILD_DIR)/profile-fixture.exe $(BUILD_DIR)/folder-fixture.exe \
	    $(BUILD_DIR)/child-parent-fixture.exe $(BUILD_DIR)/child-fixture.exe $(BUILD_DIR)/command-fixture.exe

$(BUILD_DIR)/pe-isolated-run: src/tools/pe_isolated_run.c src/tools/guest_profile.c src/pe/image.c src/pe/dll_graph.c $(RUNTIME_OBJECTS) \
    $(PE_IMAGE_HEADERS) src/tools/guest_profile.h src/runtime/compat.h src/runtime/desktop.h src/runtime/desktop_resources.h \
    src/runtime/user_folders.h src/runtime/winbase.h src/runtime/winfile.h src/runtime/sync.h \
    src/runtime/child_process.h src/runtime/dxcompiler_path.h src/pe/dll_graph.h include/ntnative/guest_thread.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PE_INCLUDES) -Isrc/runtime $(WARN_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    src/tools/pe_isolated_run.c src/tools/guest_profile.c src/pe/image.c src/pe/dll_graph.c $(RUNTIME_OBJECTS) $(LDLIBS) $(RUNTIME_LIBS) -o "$@"


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

$(BUILD_DIR)/stage4-quantum-host: tests/bench/scheduler_quantum_observation.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(WARN_CFLAGS) $(CFLAGS) -std=c11 -pthread -o "$@" "$<"

.PHONY: check-stage4-quantum
check-stage4-quantum: $(BUILD_DIR)/stage4-quantum-host
	timeout 20s $(BUILD_DIR)/stage4-quantum-host > $(BUILD_DIR)/stage4-linux-quantum.txt
	grep -Fx 'SCHEMA=ntnative.stage4.quantum-observation.v1' $(BUILD_DIR)/stage4-linux-quantum.txt
	grep -Fx 'RESULT=PASS' $(BUILD_DIR)/stage4-linux-quantum.txt
	grep -Fx 'NT_QUANTUM_VERIFIED=NO' $(BUILD_DIR)/stage4-linux-quantum.txt
	cat $(BUILD_DIR)/stage4-linux-quantum.txt

benchmark: $(BUILD_DIR)/counter-bench
	$(BUILD_DIR)/counter-bench

install: $(BUILD_DIR)/nt-native-probe $(BUILD_DIR)/pe-isolated-run $(BUILD_DIR)/nt-vulkan-probe $(BUILD_DIR)/pe-import-audit $(BUILD_DIR)/nt-dos83-mediator $(BUILD_DIR)/nt-dos83-landlock $(BUILD_DIR)/nt-object-broker $(DOS83_FUSE_TARGET)
	install -D -m 0755 "$(BUILD_DIR)/nt-native-probe" "$(DESTDIR)$(BINDIR)/nt-native-probe"
	install -D -m 0755 "$(BUILD_DIR)/pe-isolated-run" "$(DESTDIR)$(BINDIR)/pe-isolated-run"
	install -D -m 0755 "$(BUILD_DIR)/nt-vulkan-probe" "$(DESTDIR)$(BINDIR)/nt-vulkan-probe"
	install -D -m 0755 "$(BUILD_DIR)/pe-import-audit" "$(DESTDIR)$(BINDIR)/pe-import-audit"
	install -D -m 0755 "$(BUILD_DIR)/nt-dos83-mediator" "$(DESTDIR)$(BINDIR)/nt-dos83-mediator"
	install -D -m 0755 "$(BUILD_DIR)/nt-dos83-landlock" "$(DESTDIR)$(BINDIR)/nt-dos83-landlock"
	install -D -m 0755 scripts/nt-fs-supervisor.py "$(DESTDIR)$(BINDIR)/nt-fs-supervisor"
	install -D -m 0755 "$(BUILD_DIR)/nt-object-broker" "$(DESTDIR)$(BINDIR)/nt-object-broker"
ifneq ($(strip $(FUSE3_LIBS)),)
	install -D -m 0755 "$(BUILD_DIR)/nt-dos83-fuse" "$(DESTDIR)$(BINDIR)/nt-dos83-fuse"
endif
