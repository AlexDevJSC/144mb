/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "dialogs.h"
#include "desktop_resources.h"

#include <SDL.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "FAIL line %d %s SDL=%s\n", __LINE__, #test, SDL_GetError()); \
        exit(EXIT_FAILURE); \
    } \
} while (0)
#define BOUND(module, name) CHECK(nt_runtime_resolve(module ".dll", #name) == (nt_guest_proc)nt_##name)

enum { ID_EDIT = 91, ID_CHECK = 92, ID_CHOICE = 93 };
static uintptr_t command_control, capture_changed, capture_target;
static uintptr_t command_value;
static unsigned int ordinary_keys;

static uint32_t last_error(void)
{
    typedef uint32_t (NT_MS_ABI *get_error)(void);
    return ((get_error)nt_runtime_resolve("kernel32.dll", "GetLastError"))();
}

static intptr_t NT_MS_ABI window_proc(uintptr_t window, uint32_t message, uintptr_t wparam, intptr_t lparam)
{
    if (message == NT_WM_CAPTURECHANGED) {
        capture_changed = window;
        capture_target = (uintptr_t)lparam;
    }
    if (message == NT_WM_KEYDOWN && (wparam == 0x1b || wparam == 0x0d))
        ++ordinary_keys;
    return nt_DefWindowProcA(window, message, wparam, lparam);
}

static intptr_t NT_MS_ABI dialog_proc(uintptr_t window, uint32_t message, uintptr_t wparam, intptr_t lparam)
{
    if (message == NT_WM_COMMAND) {
        command_value = wparam;
        command_control = (uintptr_t)lparam;
        return 1;
    }
    if (message == 0x8001) {
        nt_SetWindowLongPtrA(window, NT_DWLP_MSGRESULT, 73);
        return 1;
    }
    return 0;
}

static void check_extra_window_memory(void)
{
    struct nt_window_class type = {.proc = window_proc, .name = "WindowExtraFixture",
                                  .window_extra = NT_DLGWINDOWEXTRA + sizeof(intptr_t)};
    CHECK(nt_RegisterClassA(&type) != 0);
    const uintptr_t first = nt_CreateWindowExA(0, type.name, "Extra bytes", 0,
                                               0, 0, 80, 60, 0, 0, 0, NULL);
    const uintptr_t second = nt_CreateWindowExA(0, type.name, "Independent extra bytes", 0,
                                                0, 0, 80, 60, 0, 0, 0, NULL);
    CHECK(first != 0 && second != 0);
    CHECK(!nt_runtime_dialog_window(first));
    const SDL_Keycode keys[] = {SDLK_ESCAPE, SDLK_RETURN};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        SDL_Event event = {.type = SDL_KEYDOWN};
        event.key.windowID = nt_runtime_window_id(first);
        event.key.keysym.sym = keys[i];
        CHECK(SDL_PushEvent(&event) == 1);
        struct nt_message message;
        CHECK(nt_GetMessageA(&message, first, NT_WM_KEYDOWN, NT_WM_KEYDOWN) == 1);
        nt_DispatchMessageA(&message);
    }
    CHECK(ordinary_keys == 2);
    const intptr_t value = (intptr_t)UINT64_C(0x123456789abcdef0);
    nt_SetLastError(91);
    CHECK(nt_GetWindowLongPtrA(first, NT_DLGWINDOWEXTRA) == 0 && last_error() == 91);
    CHECK(nt_SetWindowLongPtrA(first, NT_DLGWINDOWEXTRA, value) == 0 && last_error() == 91);
    CHECK(nt_GetWindowLongPtrA(first, NT_DLGWINDOWEXTRA) == value);
    CHECK(nt_GetWindowLongPtrA(second, NT_DLGWINDOWEXTRA) == 0);
    CHECK(nt_SetWindowLongPtrA(first, NT_DLGWINDOWEXTRA, 73) == value);
    CHECK(nt_SetWindowLongPtrA(first, NT_DLGWINDOWEXTRA + 1, 19) == 0 &&
          last_error() == NT_ERROR_INVALID_INDEX);
    CHECK(nt_GetWindowLongPtrA(first, NT_DLGWINDOWEXTRA) == 73);
    CHECK(nt_DestroyWindow(first) && nt_DestroyWindow(second));
    type.name = "InvalidWindowExtraFixture";
    type.window_extra = -1;
    CHECK(nt_RegisterClassA(&type) == 0 && last_error() == NT_ERROR_INVALID_PARAMETER);
    puts("PASS ordinary window keys, per-window extra bytes, unaligned offsets and boundary errors");
}

static void key_event(uint32_t window, SDL_Keycode key, uint16_t mods)
{
    SDL_Event event = {.type = SDL_KEYDOWN};
    event.key.windowID = window;
    event.key.keysym.sym = key;
    event.key.keysym.mod = mods;
    CHECK(SDL_PushEvent(&event) == 1);
}

static void pointer_event(uint32_t window, uint32_t type, int x, int y)
{
    SDL_Event event = {.type = type};
    event.button.windowID = window;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.clicks = 1;
    event.button.x = x;
    event.button.y = y;
    CHECK(SDL_PushEvent(&event) == 1);
}

static void deliver_dialog(uintptr_t window, uint32_t message)
{
    struct nt_message received;
    CHECK(nt_GetMessageA(&received, window, message, message) == 1);
    CHECK(received.window == window && nt_IsDialogMessageA(window, &received));
}

static void put16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *bytes, uint32_t value)
{
    put16(bytes, (uint16_t)value);
    put16(bytes + 2, (uint16_t)(value >> 16));
}

static void resource_dir(unsigned char *bytes, uint32_t id, uint32_t offset, bool subdirectory)
{
    put16(bytes + 14, 1);
    put32(bytes + 16, id);
    put32(bytes + 20, offset | (subdirectory ? UINT32_C(0x80000000) : 0));
}

static uintptr_t check_icons(void)
{
    BOUND("user32", LoadIconA);
    BOUND("user32", LoadCursorA);
    enum { RVA = 0x3000, GROUP = 160, DIB = 180, DIB_SIZE = 52 };
    const uintptr_t module = 0x400000;
    unsigned char bytes[DIB + DIB_SIZE] = {0};
    resource_dir(bytes, 3, 32, true);
    put16(bytes + 14, 2);
    put32(bytes + 24, 14);
    put32(bytes + 28, 0x80000038u);
    resource_dir(bytes + 32, 7, 80, true);
    resource_dir(bytes + 56, 200, 104, true);
    resource_dir(bytes + 80, 0x409, 128, false);
    resource_dir(bytes + 104, 0x409, 144, false);
    put32(bytes + 128, RVA + DIB);
    put32(bytes + 132, DIB_SIZE);
    put32(bytes + 144, RVA + GROUP);
    put32(bytes + 148, 20);
    put16(bytes + GROUP + 2, 1);
    put16(bytes + GROUP + 4, 1);
    bytes[GROUP + 6] = 2;
    bytes[GROUP + 7] = 1;
    put16(bytes + GROUP + 10, 1);
    put16(bytes + GROUP + 12, 32);
    put32(bytes + GROUP + 14, DIB_SIZE);
    put16(bytes + GROUP + 18, 7);
    put32(bytes + DIB, 40);
    put32(bytes + DIB + 4, 2);
    put32(bytes + DIB + 8, 2);
    put16(bytes + DIB + 12, 1);
    put16(bytes + DIB + 14, 32);
    memcpy(bytes + DIB + 40, (const unsigned char[]){9, 20, 240, 255, 210, 32, 12, 0}, 8);
    CHECK(nt_runtime_set_icon_resources(module, RVA, bytes, sizeof(bytes)));
    bytes[DIB + 42] = 0;
    const uintptr_t icon = nt_LoadIconA(module, (const char *)200);
    SDL_Surface *surface = nt_runtime_icon(icon);
    CHECK(icon != 0 && surface != NULL && surface->w == 2 && surface->h == 1);
    const unsigned char *pixels = surface->pixels;
    CHECK(pixels[0] == 240 && pixels[1] == 20 && pixels[2] == 9 && pixels[3] == 255 && pixels[7] == 0);
    CHECK(nt_LoadIconA(module, (const char *)200) == icon && nt_LoadIconA(module, (const char *)201) == 0);
    CHECK(nt_runtime_set_icon_resources(module, RVA, bytes, sizeof(bytes) - 1));
    CHECK(nt_LoadIconA(module, (const char *)200) == 0 && nt_runtime_icon(icon) == surface);
    CHECK(nt_LoadIconA(0, (const char *)32512) != 0 && nt_LoadIconA(0, (const char *)32700) == 0);
    puts("PASS stock and PE group icons, native RGBA and truncated resource rejection");
    return icon;
}

static void check_controls(uintptr_t owner)
{
    BOUND("user32", DefDlgProcA);
    BOUND("user32", CheckDlgButton);
    BOUND("user32", IsDlgButtonChecked);
    BOUND("user32", GetDlgItemTextA);
    BOUND("user32", IsDialogMessageA);
    BOUND("user32", SendDlgItemMessageA);
    const uintptr_t dialog = nt_runtime_create_dialog("Control fixture", owner, 220, 154, dialog_proc, 17);
    CHECK(dialog != 0 && nt_GetWindowLongPtrA(dialog, NT_DWLP_USER) == 17);
    CHECK(nt_SendMessageA(dialog, 0x8001, 0, 0) == 73);
    const uintptr_t edit = nt_runtime_add_dialog_control(dialog, "EDIT", "initial", ID_EDIT, NT_WS_TABSTOP,
                                                         10, 10, 190, 22);
    const uintptr_t check = nt_runtime_add_dialog_control(dialog, "BUTTON", "Tri-state", ID_CHECK,
                                                          NT_WS_TABSTOP | 6, 10, 42, 190, 22);
    const uintptr_t combo = nt_runtime_add_dialog_control(dialog, "COMBOBOX", "", ID_CHOICE,
                                                          NT_WS_TABSTOP | 3, 10, 76, 150, 74);
    CHECK(edit != 0 && check != 0 && combo != 0);
    const nt_guest_proc init = nt_runtime_resolve_ordinal("comctl32.dll", 17);
    CHECK(init != NULL && init == (nt_guest_proc)nt_InitCommonControls);
    const uintptr_t module = nt_LoadLibraryA("COMCTL32");
    CHECK(module != 0 && nt_GetProcAddress(module, (const char *)17) == init &&
          nt_GetProcAddress(module, (const char *)18) == NULL);
    CHECK(nt_runtime_add_dialog_control(dialog, "msctls_statusbar32", "", 99, 0, 0, 0, 0, 0) == 0);
    ((void (NT_MS_ABI *)(void))init)();
    const uintptr_t status = nt_runtime_add_dialog_control(dialog, "msctls_statusbar32", "", 99, 0, 0, 0, 0, 0);
    char status_text[64];
    CHECK(status != 0 && nt_SendMessageA(status, 0x401, 0, (intptr_t)"Ready") == 1 &&
          nt_SendMessageA(status, 0x402, 0, (intptr_t)status_text) == 5 && strcmp(status_text, "Ready") == 0);
    CHECK(nt_FreeLibrary(module));
    CHECK(nt_SendDlgItemMessageA(dialog, ID_CHOICE, 0x143, 0, (intptr_t)"Small") == 0 &&
          nt_SendDlgItemMessageA(dialog, ID_CHOICE, 0x143, 0, (intptr_t)"Large") == 1 &&
          nt_SendDlgItemMessageA(dialog, ID_CHOICE, 0x14e, 0, 0) == 0);
    CHECK(nt_CheckDlgButton(dialog, ID_CHECK, 2) && nt_IsDlgButtonChecked(dialog, ID_CHECK) == 2);
    CHECK(!nt_CheckDlgButton(dialog, 1000, 1) && last_error() == 1421);
    char text[12] = "unchanged";
    CHECK(nt_GetDlgItemTextA(dialog, ID_EDIT, text, 4) == 3 && memcmp(text, "ini\0", 4) == 0 &&
          text[4] == 'a');
    nt_ShowWindow(dialog, 5);
    deliver_dialog(dialog, NT_WM_PAINT);
    uint32_t color;
    CHECK(nt_runtime_window_pixel(dialog, 12, 49, &color) && color != 0);
    const uint32_t id = nt_runtime_window_id(dialog);
    const struct nt_message other = {.window = owner, .message = NT_WM_KEYDOWN, .wparam = 9};
    CHECK(!nt_IsDialogMessageA(dialog, &other));
    key_event(id, SDLK_TAB, 0);
    deliver_dialog(dialog, NT_WM_KEYDOWN);
    nt_SendMessageA(edit, 0xb1, 0, -1);
    key_event(id, SDLK_m, KMOD_SHIFT);
    deliver_dialog(dialog, NT_WM_KEYDOWN);
    CHECK(nt_GetDlgItemTextA(dialog, ID_EDIT, text, sizeof(text)) == 1 && strcmp(text, "M") == 0 &&
          command_value == (0x300u << 16 | ID_EDIT) && command_control == edit);
    key_event(id, SDLK_TAB, 0);
    deliver_dialog(dialog, NT_WM_KEYDOWN);
    key_event(id, SDLK_SPACE, 0);
    deliver_dialog(dialog, NT_WM_KEYDOWN);
    CHECK(nt_IsDlgButtonChecked(dialog, ID_CHECK) == 0 && command_value == ID_CHECK && command_control == check);
    pointer_event(id, SDL_MOUSEBUTTONDOWN, 19, 51);
    pointer_event(id, SDL_MOUSEBUTTONUP, 19, 51);
    deliver_dialog(dialog, NT_WM_LBUTTONDOWN);
    deliver_dialog(dialog, NT_WM_LBUTTONUP);
    CHECK(nt_IsDlgButtonChecked(dialog, ID_CHECK) == 1);
    key_event(id, SDLK_TAB, 0);
    deliver_dialog(dialog, NT_WM_KEYDOWN);
    key_event(id, SDLK_DOWN, 0);
    deliver_dialog(dialog, NT_WM_KEYDOWN);
    CHECK(nt_SendDlgItemMessageA(dialog, ID_CHOICE, 0x147, 0, 0) == 1 &&
          command_value == (1u << 16 | ID_CHOICE) && command_control == combo);
    CHECK(nt_DestroyWindow(dialog) && !nt_runtime_window_valid(edit));
    puts("PASS dynamic dialog control IDs, status bar, edit, check states, combo, SDL key/click and lifetime");
}

static void check_pointer(uintptr_t first, uintptr_t second)
{
    BOUND("user32", SetCapture);
    BOUND("user32", ReleaseCapture);
    BOUND("user32", SetForegroundWindow);
    CHECK(!nt_SetForegroundWindow(first));
    nt_ShowWindow(first, 5);
    nt_ShowWindow(second, 5);
    CHECK(nt_SetCapture(first) == 0);
    pointer_event(nt_runtime_window_id(second), SDL_MOUSEBUTTONUP, 10, 12);
    struct nt_message message;
    CHECK(nt_GetMessageA(&message, 0, NT_WM_LBUTTONUP, NT_WM_LBUTTONUP) == 1 && message.window == first);
    CHECK(nt_SetCapture(second) == first && capture_changed == first && capture_target == second);
    CHECK(nt_ReleaseCapture() && capture_changed == second && capture_target == 0);
    pointer_event(nt_runtime_window_id(first), SDL_MOUSEBUTTONUP, 10, 12);
    CHECK(nt_GetMessageA(&message, 0, NT_WM_LBUTTONUP, NT_WM_LBUTTONUP) == 1 && message.window == first);
    CHECK(!nt_SetForegroundWindow(UINTPTR_MAX) && last_error() == NT_ERROR_INVALID_HANDLE);
    const bool raised = nt_SetForegroundWindow(second) != 0;
    CHECK(raised || last_error() == NT_ERROR_ACCESS_DENIED);
    if (strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0)
        CHECK(raised);
    puts(raised ? "PASS pointer capture, release and native focus" :
                 "PASS pointer capture, release and the backend's foreground denial");
}

static SDL_Window *named_window(const char *name)
{
    for (uint32_t id = 1; id <= 512; ++id) {
        SDL_Window *window = SDL_GetWindowFromID(id);
        if (window != NULL && (SDL_GetWindowFlags(window) & SDL_WINDOW_SHOWN) != 0 &&
            strcmp(SDL_GetWindowTitle(window), name) == 0)
            return window;
    }
    return NULL;
}

static void check_help(uintptr_t owner)
{
    BOUND("user32", WinHelpA);
    CHECK(!nt_WinHelpA(owner, "puzzles.hlp", 3, 0) && last_error() == NT_ERROR_FILE_NOT_FOUND);
    CHECK(nt_runtime_set_help_document("puzzles.hlp", "Puzzle help fixture", "Minefield\nMark the mines."));
    CHECK(nt_WinHelpA(owner, "C:\\docs\\PUZZLES.HLP", 11, 0) &&
          named_window("Puzzle help fixture") != NULL);
    CHECK(nt_WinHelpA(owner, "puzzles.hlp", 3, 0) && nt_WinHelpA(owner, NULL, 2, 0) &&
          named_window("Puzzle help fixture") == NULL);
    puts("PASS WinHelp opens provisioned help, reuses the viewer and closes it");
}

enum action { ENTER, ESCAPE, FIRST_FILE, YES };
struct script_step { const char *title; enum action action; };
static const struct script_step *script;
static size_t script_count, script_position;
static uintptr_t modal_owner;

static void NT_MS_ABI modal_input(uintptr_t window, uint32_t message, uintptr_t timer, uint32_t time)
{
    (void)window;
    (void)message;
    (void)time;
    const struct script_step *step = &script[script_position];
    SDL_Window *native = named_window(step->title);
    if (native == NULL)
        return;
    CHECK(modal_owner == 0 || !nt_runtime_window_enabled(modal_owner));
    CHECK(SDL_GetWindowSurface(native) != NULL);
    const uint32_t id = SDL_GetWindowID(native);
    if (step->action == FIRST_FILE) {
        pointer_event(id, SDL_MOUSEBUTTONDOWN, 18, 49);
        pointer_event(id, SDL_MOUSEBUTTONUP, 18, 49);
    } else if (step->action == YES) {
        key_event(id, SDLK_TAB, 0);
    }
    key_event(id, step->action == ESCAPE ? SDLK_ESCAPE : SDLK_RETURN, 0);
    if (++script_position == script_count)
        CHECK(nt_KillTimer(0, timer));
}

static void run_script(uintptr_t owner, const struct script_step *steps, size_t count)
{
    modal_owner = owner;
    script = steps;
    script_count = count;
    script_position = 0;
    CHECK(nt_SetTimer(0, 0, 10, modal_input) != 0);
}

static void check_message_box(uintptr_t owner)
{
    BOUND("user32", MessageBoxA);
    const struct script_step ok[] = {{"Fixture information", ENTER}};
    run_script(owner, ok, 1);
    CHECK(nt_MessageBoxA(owner, "The Linux message box drew this text.", ok[0].title, 0x40) == NT_IDOK &&
          script_position == 1 && nt_runtime_window_enabled(owner) && named_window(ok[0].title) == NULL);
    const struct script_step no[] = {{"Fixture choice", ENTER}};
    run_script(owner, no, 1);
    CHECK(nt_MessageBoxA(owner, "Use the second button.", no[0].title, 0x124) == 7 && script_position == 1);
    const struct script_step cancel[] = {{"Fixture cancel", ESCAPE}};
    run_script(owner, cancel, 1);
    CHECK(nt_MessageBoxA(owner, "Cancel the dialog.", cancel[0].title, 1) == NT_IDCANCEL &&
          script_position == 1 && nt_runtime_window_enabled(owner));
    puts("PASS Linux modal message boxes, icon, default button, Escape and owner restoration");
}

static void make_file(const char *path)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && write(fd, "fixture", 7) == 7 && close(fd) == 0);
}

static void check_files(uintptr_t owner)
{
    BOUND("comdlg32", GetOpenFileNameA);
    BOUND("comdlg32", GetSaveFileNameA);
    BOUND("comdlg32", CommDlgExtendedError);
    char root[] = "/tmp/ntnative-dialogs.XXXXXX";
    CHECK(mkdtemp(root) != NULL && nt_runtime_set_file_root(root));
    char directory[128], source[128], ignored[128], symlink_path[128];
    snprintf(directory, sizeof(directory), "%s/games", root);
    snprintf(source, sizeof(source), "%s/games/inside.min", root);
    snprintf(ignored, sizeof(ignored), "%s/games/aaa.txt", root);
    snprintf(symlink_path, sizeof(symlink_path), "%s/games/escape.min", root);
    CHECK(mkdir(directory, 0700) == 0);
    make_file(source);
    make_file(ignored);
    CHECK(symlink("/etc/passwd", symlink_path) == 0);

    char file[260] = {0}, title[32] = {0};
    struct nt_open_file_name request = {.size = 136, .owner = owner, .file = file, .max_file = sizeof(file),
        .file_title = title, .max_file_title = sizeof(title), .initial_directory = "C:\\games",
        .filter = "Mines\0*.min;*.sav\0All files\0*.*\0", .filter_index = 1,
        .title = "Open fixture", .flags = 0x1000, .default_extension = "min"};
    const struct script_step open_first[] = {{"Open fixture", FIRST_FILE}};
    run_script(owner, open_first, 1);
    CHECK(nt_GetOpenFileNameA(&request) && nt_CommDlgExtendedError() == 0 && script_position == 1);
    CHECK(strcmp(file, "C:\\games\\inside.min") == 0 && strcmp(title, "inside.min") == 0 &&
          request.file_offset == 9 && request.extension_offset == 16 && request.filter_index == 1);

    strcpy(file, "round");
    request.title = "Save fixture";
    request.size = sizeof(request);
    request.flags = 0x802;
    const struct script_step save[] = {{"Save fixture", ENTER}};
    run_script(owner, save, 1);
    CHECK(nt_GetSaveFileNameA(&request) && strcmp(file, "C:\\games\\round.min") == 0);
    char saved[128];
    snprintf(saved, sizeof(saved), "%s/games/round.min", root);
    CHECK(access(saved, F_OK) != 0); /* Selection alone does not create a file. */

    strcpy(file, "inside.min");
    const struct script_step overwrite[] = {{"Save fixture", ENTER}, {"Confirm save", YES}};
    run_script(owner, overwrite, 2);
    CHECK(nt_GetSaveFileNameA(&request) && script_position == 2 && strcmp(file, "C:\\games\\inside.min") == 0);

    strcpy(file, "escape.min");
    request.title = "Open fixture";
    request.flags = 0x1000;
    const struct script_step rejected[] = {{"Open fixture", ENTER}, {"Open fixture", ESCAPE}};
    run_script(owner, rejected, 2);
    CHECK(!nt_GetOpenFileNameA(&request) && script_position == 2 && strcmp(file, "escape.min") == 0 &&
          nt_CommDlgExtendedError() == 0);
    char small[3] = {0, 0, 91};
    request.file = small;
    request.max_file = 2;
    run_script(owner, open_first, 1);
    CHECK(!nt_GetOpenFileNameA(&request) && nt_CommDlgExtendedError() == 0x3003 &&
          (unsigned char)small[0] == strlen("C:\\games\\inside.min") + 1 && small[1] == 0 && small[2] == 91);
    request.size = 12;
    CHECK(!nt_GetSaveFileNameA(&request) && nt_CommDlgExtendedError() == 1);
    CHECK(unlink(source) == 0 && unlink(ignored) == 0 && unlink(symlink_path) == 0 &&
          rmdir(directory) == 0 && rmdir(root) == 0 && nt_runtime_set_file_root(NULL));
    puts("PASS file open/save controls, filters, overwrite, offsets, cancellation and guest-root confinement");
}

int main(void)
{
    CHECK(nt_runtime_open_desktop());
    check_extra_window_memory();
    const uintptr_t icon = check_icons();
    SDL_Cursor *probe = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    const uintptr_t cursor = nt_LoadCursorA(0, (const char *)32512);
    CHECK((cursor != 0) == (probe != NULL));
    if (probe != NULL) {
        SDL_FreeCursor(probe);
        CHECK(nt_runtime_cursor(cursor) != NULL && nt_LoadCursorA(0, (const char *)32512) == cursor);
        puts("PASS native SDL system cursor and shared handle");
    } else {
        CHECK(last_error() == NT_ERROR_GEN_FAILURE);
        puts("PASS cursor reports the SDL backend's unavailable pointer device");
    }
    const struct nt_window_class definition = {.proc = window_proc, .name = "dialog_test_host",
                                                .icon = icon, .cursor = cursor, .background = 16};
    CHECK(nt_RegisterClassA(&definition) != 0);
    const uintptr_t first = nt_CreateWindowExA(0, definition.name, "First fixture", 0xc00000,
                                               30, 30, 180, 144, 0, 0, 0, NULL);
    const uintptr_t second = nt_CreateWindowExA(0, definition.name, "Second fixture", 0xc00000,
                                                44, 44, 180, 144, 0, 0, 0, NULL);
    CHECK(first != 0 && second != 0);
    check_pointer(first, second);
    check_controls(first);
    check_help(first);
    check_message_box(first);
    check_files(first);
    CHECK(nt_DestroyWindow(first) && nt_DestroyWindow(second));
    nt_runtime_close_desktop();
    return EXIT_SUCCESS;
}
