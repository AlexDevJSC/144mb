/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "menu_view.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "FAIL line %d %s SDL=%s\n", __LINE__, #test, SDL_GetError()); \
        exit(EXIT_FAILURE); \
    } \
} while (0)
#define BOUND(name) CHECK(nt_runtime_resolve("user32.dll", #name) == (nt_guest_proc)nt_##name)

enum { BLUE = 0xff0000, GREEN = 0x00ff00 };
static unsigned int creates, paints, characters, mouse, timers, destroys, commands;
static struct nt_rect painted;
static uintptr_t enable_on_init;
static unsigned int menu_initializations;
static uintptr_t last_command;

static intptr_t NT_MS_ABI procedure(uintptr_t window, uint32_t message,
                                   uintptr_t wparam, intptr_t lparam)
{
    switch (message) {
    case NT_WM_CREATE: {
        const struct nt_create_window *create = (const struct nt_create_window *)lparam;
        CHECK(create->width == 128 && create->height == 96);
        CHECK(nt_SetWindowLongPtrA(window, -21, (intptr_t)create->parameter) == 0);
        ++creates;
        return 0;
    }
    case NT_WM_PAINT: {
        struct nt_paint paint;
        const uintptr_t dc = nt_BeginPaint(window, &paint);
        CHECK(dc != 0 && paint.dc == dc);
        painted = paint.rect;
        if (paints == 0)
            CHECK(nt_SetPixel(dc, 7, 8, BLUE) == BLUE);
        else
            CHECK(nt_SetPixel(dc, 20, 21, GREEN) == GREEN &&
                  nt_SetPixel(dc, 7, 8, GREEN) == UINT32_MAX);
        CHECK(nt_EndPaint(window, &paint));
        ++paints;
        return 0;
    }
    case NT_WM_CHAR:
        CHECK(wparam == 'A');
        ++characters;
        return 0;
    case NT_WM_LBUTTONDOWN:
        CHECK((wparam & 1) != 0 && ((uint32_t)lparam & 0xffff) == 20);
        ++mouse;
        return 0;
    case NT_WM_TIMER:
        CHECK(wparam == 77 && nt_KillTimer(window, wparam));
        ++timers;
        return 0;
    case NT_WM_COMMAND:
        CHECK((wparam == 501 || wparam == 701) && lparam == 0);
        last_command = wparam;
        ++commands;
        return 0;
    case NT_WM_INITMENUPOPUP:
        ++menu_initializations;
        if (wparam == enable_on_init)
            CHECK(nt_EnableMenuItem(wparam, 701, 0) >= 0);
        return 0;
    case NT_WM_DESTROY:
        ++destroys;
        nt_PostQuitMessage(37);
        return 0;
    default:
        return nt_DefWindowProcA(window, message, wparam, lparam);
    }
}

static void deliver(uintptr_t window, uint32_t type)
{
    struct nt_message message;
    CHECK(nt_GetMessageA(&message, window, type, type) == 1);
    CHECK(message.window == window && message.message == type);
    nt_DispatchMessageA(&message);
}

static void check_clipboard(uintptr_t window)
{
    BOUND(OpenClipboard);
    BOUND(CloseClipboard);
    BOUND(EmptyClipboard);
    BOUND(SetClipboardData);
    const uintptr_t memory = nt_GlobalAlloc(2, 6);
    CHECK(memory != 0);
    char *bytes = nt_GlobalLock(memory);
    CHECK(bytes != NULL);
    memcpy(bytes, "Caf\xe9!", 6);
    CHECK(nt_GlobalUnlock(memory) == 0);
    CHECK(!nt_EmptyClipboard() && nt_OpenClipboard(window) && !nt_OpenClipboard(window));
    CHECK(nt_EmptyClipboard() && nt_SetClipboardData(1, memory) == memory && nt_CloseClipboard());
    char *native = SDL_GetClipboardText();
    CHECK(native != NULL && strcmp(native, "Caf\xc3\xa9!") == 0);
    SDL_free(native);
    CHECK(nt_GlobalFree(memory) == memory);
    CHECK(nt_OpenClipboard(window) && nt_EmptyClipboard() && nt_CloseClipboard());
    CHECK(nt_GlobalLock(memory) == NULL);
    puts("PASS clipboard ownership and ANSI text on the SDL backend");
}

static uintptr_t check_menu(uintptr_t window, uint32_t native_id)
{
    BOUND(SetMenu);
    BOUND(GetMenu);
    BOUND(DrawMenuBar);
    BOUND(GetMenuItemRect);
    BOUND(AdjustWindowRectEx);
    BOUND(GetSysColor);
    const uintptr_t menu = nt_CreateMenu(), popup = nt_CreateMenu();
    CHECK(menu != 0 && popup != 0);
    CHECK(nt_AppendMenuA(popup, 0, 501, "&Restart") && nt_AppendMenuA(menu, 0x10, popup, "&Game"));
    struct nt_rect client, bounds, item;
    CHECK(nt_GetClientRect(window, &client));
    bounds = client;
    CHECK(nt_AdjustWindowRectEx(&bounds, UINT32_C(0x80000000), 1, 0) && bounds.top == -24);
    CHECK(nt_SetMenu(window, menu) && nt_GetMenu(window) == menu && !nt_DestroyMenu(menu));
    CHECK(nt_GetClientRect(window, &bounds) && bounds.bottom == client.bottom - 24);
    CHECK(nt_DrawMenuBar(window) && nt_GetMenuItemRect(window, menu, 0, &item) &&
          item.right > item.left && item.bottom - item.top == 24);
    uint32_t color;
    CHECK(nt_runtime_window_pixel(window, 120, 4, &color) && color == nt_GetSysColor(4));

    SDL_Event click = {.type = SDL_MOUSEBUTTONUP};
    click.button.windowID = native_id;
    click.button.button = SDL_BUTTON_LEFT;
    click.button.x = 8;
    click.button.y = 12;
    CHECK(SDL_PushEvent(&click) == 1);
    struct nt_message message;
    CHECK(nt_runtime_take_window_message(&message, window, NT_WM_COMMAND, NT_WM_COMMAND) == 0 &&
          nt_GetMenuItemRect(window, popup, 0, &item) && item.bottom - item.top == 22);
    click.button.y = 33;
    CHECK(SDL_PushEvent(&click) == 1);
    deliver(window, NT_WM_COMMAND);
    CHECK(commands == 1 && !nt_GetMenuItemRect(window, popup, 0, &item));
    puts("PASS attached menu, rendered bar, popup hit testing and WM_COMMAND");
    return menu;
}

static void click_menu_item(uintptr_t window, uint32_t native_id, uintptr_t menu, uint32_t index)
{
    struct nt_rect bounds;
    CHECK(nt_GetMenuItemRect(window, menu, index, &bounds));
    int x, y;
    SDL_GetWindowPosition(SDL_GetWindowFromID(native_id), &x, &y);
    SDL_Event event = {.type = SDL_MOUSEBUTTONUP};
    event.button.windowID = native_id;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = (bounds.left + bounds.right) / 2 - x;
    event.button.y = (bounds.top + bounds.bottom) / 2 - y;
    CHECK(SDL_PushEvent(&event) == 1);
}

static uintptr_t check_menu_navigation(uintptr_t window, uint32_t native_id, uintptr_t previous)
{
    const uintptr_t bar = nt_CreateMenu(), popup = nt_CreateMenu();
    const uintptr_t child = nt_CreateMenu(), deep = nt_CreateMenu();
    CHECK(bar != 0 && popup != 0 && child != 0 && deep != 0);
    CHECK(nt_AppendMenuA(popup, 1, 500, "Disabled action"));
    CHECK(nt_AppendMenuA(popup, 0x800, 0, NULL));
    for (uint32_t i = 0; i < 10; ++i)
        CHECK(nt_AppendMenuA(popup, 0, 600 + i, "Another action"));
    CHECK(nt_AppendMenuA(deep, 1, 701, "Dynamic action"));
    CHECK(nt_AppendMenuA(child, 0x10, deep, "Second level"));
    CHECK(nt_AppendMenuA(popup, 0x10, child, "First level"));
    CHECK(nt_AppendMenuA(bar, 0x10, popup, "Actions"));
    CHECK(nt_SetMenu(window, bar) && nt_DestroyMenu(previous));
    enable_on_init = deep;
    const unsigned int before = menu_initializations;
    struct nt_message message;
    struct nt_rect bounds;
    click_menu_item(window, native_id, bar, 0);
    CHECK(nt_runtime_take_window_message(&message, window, NT_WM_COMMAND, NT_WM_COMMAND) == 0);
    CHECK(menu_initializations == before + 1);
    click_menu_item(window, native_id, popup, 0);
    CHECK(nt_runtime_take_window_message(&message, window, NT_WM_COMMAND, NT_WM_COMMAND) == 0);
    CHECK(nt_GetMenuItemRect(window, popup, 0, &bounds));
    CHECK(!nt_GetMenuItemRect(window, popup, 12, &bounds));
    SDL_Event wheel = {.type = SDL_MOUSEWHEEL};
    wheel.wheel.windowID = native_id;
    wheel.wheel.y = -1000;
    CHECK(SDL_PushEvent(&wheel) == 1);
    CHECK(nt_runtime_take_window_message(&message, window, NT_WM_COMMAND, NT_WM_COMMAND) == 0);
    CHECK(!nt_GetMenuItemRect(window, popup, 0, &bounds));
    click_menu_item(window, native_id, popup, 12);
    CHECK(nt_runtime_take_window_message(&message, window, NT_WM_COMMAND, NT_WM_COMMAND) == 0);
    click_menu_item(window, native_id, child, 0);
    CHECK(nt_runtime_take_window_message(&message, window, NT_WM_COMMAND, NT_WM_COMMAND) == 0);
    CHECK(menu_initializations == before + 3);
    SDL_Event escape = {.type = SDL_KEYDOWN};
    escape.key.windowID = native_id;
    escape.key.keysym.sym = SDLK_ESCAPE;
    CHECK(SDL_PushEvent(&escape) == 1);
    CHECK(nt_runtime_take_window_message(&message, window, NT_WM_COMMAND, NT_WM_COMMAND) == 0);
    CHECK(!nt_GetMenuItemRect(window, deep, 0, &bounds));
    click_menu_item(window, native_id, child, 0);
    CHECK(nt_runtime_take_window_message(&message, window, NT_WM_COMMAND, NT_WM_COMMAND) == 0);
    click_menu_item(window, native_id, deep, 0);
    deliver(window, NT_WM_COMMAND);
    CHECK(commands == 2 && last_command == 701 && !nt_GetMenuItemRect(window, popup, 12, &bounds));
    puts("PASS long menu scrolling, nested submenus, disabled items, initialization and Escape");
    return bar;
}

static intptr_t NT_MS_ABI destroy_during_menu_init(uintptr_t window, uint32_t message,
                                                 uintptr_t wparam, intptr_t lparam)
{
    if (message == NT_WM_INITMENUPOPUP) {
        CHECK(nt_DestroyWindow(window));
        return 0;
    }
    return nt_DefWindowProcA(window, message, wparam, lparam);
}

static void check_menu_lifetime(void)
{
    const struct nt_window_class type = {.proc = destroy_during_menu_init,
                                        .name = "MenuLifetimeFixture"};
    CHECK(nt_RegisterClassA(&type) != 0);
    const uintptr_t window = nt_CreateWindowExA(0, type.name, "Menu lifetime", 0,
                                                0, 0, 100, 100, 0, 0, 0, NULL);
    const uintptr_t bar = nt_CreateMenu(), popup = nt_CreateMenu();
    CHECK(window != 0 && bar != 0 && popup != 0);
    CHECK(nt_AppendMenuA(popup, 0, 701, "An action"));
    CHECK(nt_AppendMenuA(bar, 0x10, popup, "Actions") && nt_SetMenu(window, bar));
    click_menu_item(window, nt_runtime_window_id(window), bar, 0);
    struct nt_message message;
    CHECK(nt_runtime_take_window_message(&message, window, NT_WM_COMMAND, NT_WM_COMMAND) == 0);
    struct nt_rect rect;
    CHECK(!nt_GetClientRect(window, &rect) && nt_GetMenuItemCount(bar) == -1);
    puts("PASS menu initialization tolerates destruction of the owner and menu tree");
}

int main(void)
{
    BOUND(RegisterClassA);
    BOUND(CreateWindowExA);
    BOUND(DefWindowProcA);
    BOUND(DestroyWindow);
    BOUND(ShowWindow);
    BOUND(GetClientRect);
    BOUND(GetWindowRect);
    BOUND(InvalidateRect);
    BOUND(BeginPaint);
    BOUND(EndPaint);
    BOUND(GetDC);
    BOUND(ReleaseDC);
    BOUND(EnableWindow);
    BOUND(SetWindowTextA);
    BOUND(SendMessageA);
    BOUND(GetWindowLongPtrA);
    BOUND(SetWindowLongPtrA);
    BOUND(SetWindowPos);
    BOUND(GetKeyboardState);
    BOUND(TranslateMessage);
    BOUND(SystemParametersInfoA);
    CHECK(nt_runtime_open_desktop());
    const struct nt_window_class type = {.style = 8, .proc = procedure,
        .background = nt_GetStockObject(0), .name = "CompatFixture", .instance = 7};
    const uint16_t atom = nt_RegisterClassA(&type);
    CHECK(atom != 0 && nt_RegisterClassA(&type) == 0);
    CHECK(nt_CreateWindowExA(0, "Missing", "unused", 0, 0, 0, 128, 96, 0, 0, 7, NULL) == 0);
    int state = 42;
    const uintptr_t window = nt_CreateWindowExA(0, (const char *)(uintptr_t)atom, "Mines fixture",
        0, INT32_MIN, INT32_MIN, 128, 96, 0, 0, 7, &state);
    CHECK(window != 0 && creates == 1 && nt_GetWindowLongPtrA(window, -21) == (intptr_t)&state);
    CHECK(nt_SetWindowLongPtrA(window, -21, 17) == (intptr_t)&state &&
          nt_GetWindowLongPtrA(window, -21) == 17);
    CHECK(nt_GetWindowLongPtrA(window, -4) == (intptr_t)procedure);
    CHECK(nt_ShowWindow(window, 5) == 0);
    deliver(window, NT_WM_PAINT);
    CHECK(paints == 1 && painted.left == 0 && painted.right == 128);
    uint32_t color;
    CHECK(nt_runtime_window_pixel(window, 7, 8, &color) && color == BLUE);

    CHECK(nt_SetWindowTextA(window, "Prueba gr\xe1" "fica"));
    char title[32];
    CHECK(nt_SendMessageA(window, NT_WM_GETTEXT, sizeof(title), (intptr_t)title) == 14 &&
          strcmp(title, "Prueba gr\xe1" "fica") == 0);
    CHECK(strcmp(SDL_GetWindowTitle(SDL_GetWindowFromID(nt_runtime_window_id(window))),
                 "Prueba gr\xc3\xa1" "fica") == 0);
    struct nt_rect client, outer, work;
    CHECK(nt_GetClientRect(window, &client) && client.right == 128 && client.bottom == 96);
    CHECK(nt_GetWindowRect(window, &outer) && outer.right - outer.left == 128);
    CHECK(nt_SystemParametersInfoA(0x30, 0, &work, 0) && work.right > work.left);
    CHECK(nt_EnableWindow(window, 0) == 0 && nt_EnableWindow(window, 1) == 1);
    CHECK(nt_SetWindowPos(window, 0, 0, 0, 132, 100, 2 | 4 | 16));
    CHECK(nt_GetClientRect(window, &client) && client.right == 132 && client.bottom == 100);
    /* Clear the resize damage before checking a strictly smaller update rectangle. */
    struct nt_paint resize;
    CHECK(nt_BeginPaint(window, &resize) != 0 && nt_SetPixel(resize.dc, 7, 8, BLUE) == BLUE &&
          nt_EndPaint(window, &resize));
    SDL_PumpEvents();
    SDL_FlushEvent(SDL_WINDOWEVENT);
    const struct nt_rect damage = {19, 20, 22, 23};
    CHECK(nt_InvalidateRect(window, &damage, 0));
    deliver(window, NT_WM_PAINT);
    CHECK(paints == 2 && memcmp(&damage, &painted, sizeof(damage)) == 0);
    CHECK(nt_runtime_window_pixel(window, 20, 21, &color) && color == GREEN);
    CHECK(nt_runtime_window_pixel(window, 7, 8, &color) && color == BLUE);
    const uintptr_t dc = nt_GetDC(window);
    CHECK(dc != 0 && nt_SetPixel(dc, 9, 8, GREEN) == GREEN && nt_ReleaseDC(window, dc));
    CHECK(nt_runtime_window_pixel(window, 9, 8, &color) && color == GREEN);

    const uint32_t native_id = nt_runtime_window_id(window);
    SDL_Event event = {.type = SDL_KEYDOWN};
    event.key.windowID = native_id;
    event.key.keysym.sym = SDLK_a;
    event.key.keysym.scancode = SDL_SCANCODE_A;
    event.key.keysym.mod = KMOD_LSHIFT;
    CHECK(SDL_PushEvent(&event) == 1);
    struct nt_message message;
    unsigned char keys[256];
    CHECK(nt_GetMessageA(&message, window, NT_WM_KEYDOWN, NT_WM_KEYDOWN) == 1 &&
          message.wparam == 'A' && nt_GetKeyboardState(keys) && (keys['A'] & 0x80) != 0);
    CHECK(nt_TranslateMessage(&message));
    deliver(window, NT_WM_CHAR);
    CHECK(characters == 1);
    event.type = SDL_KEYUP;
    CHECK(SDL_PushEvent(&event) == 1);
    deliver(window, NT_WM_KEYUP);
    CHECK(nt_GetKeyboardState(keys) && (keys['A'] & 0x80) == 0);
    event = (SDL_Event){.type = SDL_MOUSEBUTTONDOWN};
    event.button.windowID = native_id;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 20;
    event.button.y = 21;
    CHECK(SDL_PushEvent(&event) == 1);
    deliver(window, NT_WM_LBUTTONDOWN);
    CHECK(mouse == 1);
    CHECK(nt_SetTimer(window, 77, 10, NULL) == 77);
    deliver(window, NT_WM_TIMER);
    CHECK(timers == 1);
    if (strcmp(SDL_GetCurrentVideoDriver(), "offscreen") == 0) {
        check_clipboard(window);
        BOUND(MessageBeep);
        CHECK(SDL_setenv("SDL_AUDIODRIVER", "dummy", 1) == 0);
        CHECK(nt_MessageBeep(0x30) && !nt_MessageBeep(123));
        puts("PASS SDL queues an asynchronous alert tone");
    }
    const uintptr_t menu = check_menu_navigation(window, native_id, check_menu(window, native_id));
    check_menu_lifetime();

    event = (SDL_Event){.type = SDL_WINDOWEVENT};
    event.window.windowID = native_id;
    event.window.event = SDL_WINDOWEVENT_CLOSE;
    CHECK(SDL_PushEvent(&event) == 1);
    deliver(window, NT_WM_CLOSE);
    CHECK(destroys == 1 && !nt_GetClientRect(window, &client) && !nt_DestroyWindow(window));
    CHECK(nt_GetMenuItemCount(menu) == -1);
    CHECK(nt_GetMessageA(&message, 0, 0, 0) == 0 && message.wparam == 37);
    printf("PASS SDL %s window, painted pixels, damage clipping, input and close\n",
           SDL_GetCurrentVideoDriver());
    nt_runtime_close_desktop();
    return EXIT_SUCCESS;
}
