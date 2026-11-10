/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "dialogs.h"
#include "desktop_resources.h"

#include <SDL.h>
#include <cairo.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_CONTROLS = 160, MAX_CONTROL_TEXT = 4095, MAX_ITEMS = 256, ROW_HEIGHT = 21,
    WS_DISABLED = 0x08000000, WS_GROUP = 0x20000,
    WS_EX_CLIENTEDGE = 0x200, WS_EX_NOPARENTNOTIFY = 4,
    BS_DEFPUSHBUTTON = 1, BS_CHECKBOX = 2, BS_AUTOCHECKBOX = 3, BS_RADIOBUTTON = 4,
    BS_3STATE = 5, BS_AUTO3STATE = 6, BS_GROUPBOX = 7, BS_AUTORADIOBUTTON = 9,
    BM_GETCHECK = 0xf0, BM_SETCHECK = 0xf1, BM_CLICK = 0xf5,
    WM_SETFONT = 0x30, WM_GETFONT = 0x31, WM_GETDLGCODE = 0x87,
    CB_ADDSTRING = 0x143, CB_GETCURSEL = 0x147, CB_SETCURSEL = 0x14e,
    EM_SETSEL = 0xb1, EM_LIMITTEXT = 0xc5, ES_READONLY = 0x800,
    SB_SETTEXTA = 0x401, SB_GETTEXTA = 0x402, SB_GETTEXTLENGTHA = 0x403,
    STM_SETICON = 0x170, STM_GETICON = 0x171,
    LBN_SELCHANGE = 1, CBN_SELCHANGE = 1, EN_CHANGE = 0x300,
    VK_TAB = 9, VK_RETURN = 13, VK_ESCAPE = 27, VK_SPACE = 32
};

enum control_type { BUTTON, EDIT, LABEL, COMBO, LIST, STATUS };

struct control {
    uintptr_t handle, parent, font, icon;
    uint32_t style;
    int32_t id, x, y, width, height;
    enum control_type type;
    char *text, **items;
    size_t item_count, limit, caret, anchor;
    int selected, top;
    uint32_t check;
    bool focused, pressed, expanded;
};

static struct control controls[MAX_CONTROLS];
static uintptr_t next_handle = 0x2b00000b;
static atomic_bool common_controls;

static struct control *find_control(uintptr_t handle)
{
    if (handle != 0 && nt_runtime_desktop_active()) {
        for (size_t i = 0; i < MAX_CONTROLS; ++i) {
            if (controls[i].handle == handle)
                return &controls[i];
        }
    }
    return NULL;
}

bool nt_runtime_control_valid(uintptr_t handle)
{
    return find_control(handle) != NULL;
}

static struct control *find_item(uintptr_t parent, int32_t id)
{
    for (size_t i = 0; i < MAX_CONTROLS; ++i) {
        if (controls[i].handle != 0 && controls[i].parent == parent && controls[i].id == id)
            return &controls[i];
    }
    return NULL;
}

static void clear_items(struct control *control)
{
    for (size_t i = 0; i < control->item_count; ++i)
        free(control->items[i]);
    free(control->items);
    control->items = NULL;
    control->item_count = 0;
    control->selected = -1;
    control->top = 0;
    control->expanded = false;
}

static void dispose(struct control *control)
{
    clear_items(control);
    free(control->text);
    *control = (struct control){0};
}

bool nt_runtime_destroy_control(uintptr_t handle)
{
    struct control *control = find_control(handle);
    if (control == NULL)
        return false;
    const uintptr_t parent = control->parent;
    dispose(control);
    nt_InvalidateRect(parent, NULL, 1);
    return true;
}

void nt_runtime_remove_controls(uintptr_t parent)
{
    for (size_t i = 0; i < MAX_CONTROLS; ++i) {
        if (controls[i].handle != 0 && controls[i].parent == parent)
            dispose(&controls[i]);
    }
}

void nt_runtime_close_controls(void)
{
    common_controls = false;
}

void NT_MS_ABI nt_InitCommonControls(void)
{
    common_controls = true;
}

static bool control_class(const char *name, enum control_type *type)
{
    static const char *const names[] = {"BUTTON", "EDIT", "STATIC", "COMBOBOX", "LISTBOX",
                                       "msctls_statusbar32"};
    if ((uintptr_t)name <= UINT16_MAX)
        return false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (nt_module_name_equal(name, names[i]) && (i != STATUS || common_controls)) {
            *type = (enum control_type)i;
            return true;
        }
    }
    return false;
}

uintptr_t nt_runtime_create_control(const char *name, const char *text, uint32_t style,
                                    uint32_t exstyle, int32_t x, int32_t y, int32_t width,
                                    int32_t height, uintptr_t parent, uintptr_t id)
{
    enum control_type type;
    if (!control_class(name, &type)) {
        nt_SetLastError(1407); /* ERROR_CANNOT_FIND_WND_CLASS */
        return 0;
    }
    if ((exstyle & ~(WS_EX_CLIENTEDGE | WS_EX_NOPARENTNOTIFY)) != 0 ||
        width < 0 || width > 4096 || height < 0 || height > 4096 ||
        x < -4096 || x > 4096 || y < -4096 || y > 4096) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (text == NULL)
        text = "";
    if ((uintptr_t)text <= UINT16_MAX || strnlen(text, MAX_CONTROL_TEXT + 1) > MAX_CONTROL_TEXT) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (size_t i = 0; i < MAX_CONTROLS && next_handle <= UINTPTR_MAX - 16; ++i) {
        struct control *control = &controls[i];
        if (control->handle != 0)
            continue;
        char *copy = strdup(text);
        if (copy == NULL)
            break;
        const uintptr_t handle = next_handle;
        next_handle += 16;
        *control = (struct control){.handle = handle, .parent = parent, .style = style,
            .id = (int32_t)id, .type = type, .x = x, .y = y, .width = width, .height = height,
            .text = copy, .limit = MAX_CONTROL_TEXT, .selected = -1};
        control->caret = control->anchor = strlen(copy);
        nt_InvalidateRect(parent, NULL, false);
        return handle;
    }
    nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

static struct nt_rect control_bounds(const struct control *control, int width, int height)
{
    if (control->type == STATUS)
        return (struct nt_rect){0, height > ROW_HEIGHT ? height - ROW_HEIGHT : 0, width, height};
    const int visible_height = control->type == COMBO && control->height > ROW_HEIGHT ?
                               ROW_HEIGHT : control->height;
    return (struct nt_rect){control->x, control->y, control->x + control->width,
                            control->y + visible_height};
}

bool nt_runtime_control_rect(uintptr_t handle, struct nt_rect *rect, bool screen)
{
    const struct control *control = find_control(handle);
    struct nt_rect parent;
    if (control == NULL || rect == NULL || !nt_GetClientRect(control->parent, &parent))
        return false;
    struct nt_rect bounds = control_bounds(control, parent.right, parent.bottom);
    if (!screen)
        bounds = (struct nt_rect){0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top};
    else if (!nt_GetWindowRect(control->parent, &parent) ||
             !nt_OffsetRect(&bounds, parent.left, parent.top))
        return false;
    *rect = bounds;
    return true;
}

int32_t nt_runtime_control_show(uintptr_t handle, bool show)
{
    struct control *control = find_control(handle);
    if (control == NULL)
        return 0;
    const bool previous = (control->style & NT_WS_VISIBLE) != 0;
    if (show)
        control->style |= NT_WS_VISIBLE;
    else {
        control->style &= ~NT_WS_VISIBLE;
        control->focused = control->pressed = control->expanded = false;
    }
    nt_InvalidateRect(control->parent, NULL, !show);
    return previous;
}

int32_t nt_runtime_control_enable(uintptr_t handle, bool enable)
{
    struct control *control = find_control(handle);
    if (control == NULL)
        return 0;
    const bool previous = (control->style & WS_DISABLED) != 0;
    if (enable)
        control->style &= ~WS_DISABLED;
    else {
        control->style |= WS_DISABLED;
        control->focused = control->pressed = control->expanded = false;
    }
    nt_InvalidateRect(control->parent, NULL, false);
    return previous;
}

static bool active(const struct control *control, uintptr_t parent)
{
    return control->handle != 0 && control->parent == parent &&
           (control->style & (NT_WS_VISIBLE | WS_DISABLED)) == NT_WS_VISIBLE;
}

static unsigned int button_style(const struct control *control)
{
    return control->style & 15u;
}

static bool tab_stop(const struct control *control, uintptr_t parent)
{
    return active(control, parent) && (control->style & NT_WS_TABSTOP) != 0;
}

static struct control *focused_control(uintptr_t parent)
{
    for (size_t i = 0; i < MAX_CONTROLS; ++i) {
        if (active(&controls[i], parent) && controls[i].focused)
            return &controls[i];
    }
    return NULL;
}

static void focus(struct control *control)
{
    struct control *previous = focused_control(control->parent);
    if (previous == control)
        return;
    if (previous != NULL)
        previous->focused = previous->expanded = previous->pressed = false;
    control->focused = true;
    nt_InvalidateRect(control->parent, NULL, false);
}

bool nt_runtime_focus_control(uintptr_t parent, uintptr_t handle, bool previous)
{
    if (handle != 0) {
        struct control *target = find_control(handle);
        if (target == NULL || !active(target, parent))
            return false;
        focus(target);
        return true;
    }
    const struct control *current = focused_control(parent);
    struct control *edge = NULL, *next = NULL;
    for (size_t i = 0; i < MAX_CONTROLS; ++i) {
        struct control *candidate = &controls[i];
        if (!tab_stop(candidate, parent))
            continue;
        if (edge == NULL || (candidate->handle < edge->handle) != previous)
            edge = candidate;
        if (current != NULL && (candidate->handle > current->handle) != previous &&
            candidate != current && (next == NULL || (candidate->handle < next->handle) != previous))
            next = candidate;
    }
    if (next == NULL)
        next = edge;
    if (next == NULL)
        return false;
    focus(next);
    return true;
}

uintptr_t nt_runtime_initial_dialog_focus(uintptr_t parent)
{
    struct control *control = focused_control(parent);
    if (control == NULL && nt_runtime_focus_control(parent, 0, false))
        control = focused_control(parent);
    return control != NULL ? control->handle : 0;
}

uintptr_t nt_runtime_dialog_item(uintptr_t parent, int32_t id)
{
    const struct control *control = find_item(parent, id);
    return control != NULL ? control->handle : 0;
}

uint16_t nt_runtime_dialog_default_id(uintptr_t parent)
{
    for (size_t i = 0; i < MAX_CONTROLS; ++i) {
        const struct control *control = &controls[i];
        if (active(control, parent) && control->type == BUTTON && button_style(control) == BS_DEFPUSHBUTTON)
            return (uint16_t)control->id;
    }
    return 0;
}

static void notify(const struct control *control, unsigned int notification)
{
    nt_SendMessageA(control->parent, NT_WM_COMMAND,
                    ((uintptr_t)notification << 16) | (uint16_t)control->id, (intptr_t)control->handle);
}

static void click_button(struct control *control)
{
    const unsigned int style = button_style(control);
    if (style == BS_AUTOCHECKBOX)
        control->check = !control->check;
    else if (style == BS_AUTO3STATE)
        control->check = (control->check + 1) % 3;
    else if (style == BS_AUTORADIOBUTTON) {
        uintptr_t first = 0, last = UINTPTR_MAX;
        for (size_t i = 0; i < MAX_CONTROLS; ++i) {
            const struct control *candidate = &controls[i];
            if (candidate->parent != control->parent || (candidate->style & WS_GROUP) == 0)
                continue;
            if (candidate->handle <= control->handle && candidate->handle > first)
                first = candidate->handle;
            if (candidate->handle > control->handle && candidate->handle < last)
                last = candidate->handle;
        }
        for (size_t i = 0; i < MAX_CONTROLS; ++i) {
            struct control *candidate = &controls[i];
            if (candidate->parent == control->parent && candidate->handle >= first &&
                candidate->handle < last && candidate->type == BUTTON &&
                button_style(candidate) == BS_AUTORADIOBUTTON)
                candidate->check = candidate == control;
        }
    }
    nt_InvalidateRect(control->parent, NULL, false);
    notify(control, 0);
}

static const char *control_text(const struct control *control)
{
    if (control->type == COMBO && control->selected >= 0)
        return control->items[control->selected];
    return control->text;
}

static bool set_text(struct control *control, const char *text)
{
    if (text == NULL)
        text = "";
    const size_t length = strnlen(text, MAX_CONTROL_TEXT + 1);
    if (length > MAX_CONTROL_TEXT)
        return false;
    char *copy = strdup(text);
    if (copy == NULL)
        return false;
    free(control->text);
    control->text = copy;
    control->anchor = control->caret = length;
    nt_InvalidateRect(control->parent, NULL, false);
    return true;
}

static intptr_t copy_text(const char *text, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0)
        return 0;
    size_t length = strlen(text);
    if (length >= capacity)
        length = capacity - 1;
    memcpy(output, text, length);
    output[length] = '\0';
    return (intptr_t)length;
}

static int rows(const struct control *control)
{
    const int available = control->type == COMBO ? control->height - ROW_HEIGHT : control->height;
    const int count = (available - 2) / ROW_HEIGHT;
    return count > 0 ? count : 1;
}

static intptr_t select_item(struct control *control, intptr_t index)
{
    if (index < -1 || index >= (intptr_t)control->item_count)
        return -1;
    control->selected = (int)index;
    if (index >= 0 && control->selected < control->top)
        control->top = control->selected;
    if (index >= control->top + rows(control))
        control->top = control->selected - rows(control) + 1;
    nt_InvalidateRect(control->parent, NULL, false);
    return index;
}

static intptr_t add_item(struct control *control, const char *text)
{
    if (text == NULL || strnlen(text, MAX_CONTROL_TEXT + 1) > MAX_CONTROL_TEXT)
        return -1;
    if (control->item_count == MAX_ITEMS)
        return -2;
    if (control->items == NULL) {
        control->items = calloc(MAX_ITEMS, sizeof(*control->items));
        if (control->items == NULL)
            return -2;
    }
    char *copy = strdup(text);
    if (copy == NULL)
        return -2;
    control->items[control->item_count] = copy;
    nt_InvalidateRect(control->parent, NULL, false);
    return (intptr_t)control->item_count++;
}

static size_t edit_position(intptr_t value, size_t length)
{
    return value < 0 || (uintptr_t)value > length ? length : (size_t)value;
}

intptr_t nt_runtime_control_message(uintptr_t handle, uint32_t message, uintptr_t wparam, intptr_t lparam)
{
    struct control *control = find_control(handle);
    if (control == NULL)
        return 0;
    switch (message) {
    case NT_WM_SETTEXT: return set_text(control, (const char *)lparam);
    case NT_WM_GETTEXT: return copy_text(control_text(control), (char *)lparam, wparam);
    case NT_WM_GETTEXTLENGTH: return (intptr_t)strlen(control_text(control));
    case WM_SETFONT: control->font = wparam; if (lparam) nt_InvalidateRect(control->parent, NULL, false); return 0;
    case WM_GETFONT: return (intptr_t)control->font;
    case WM_GETDLGCODE:
        return control->type == BUTTON ? 0x2000 | (button_style(control) == BS_DEFPUSHBUTTON ? 0x10 : 0x20) :
               control->type == EDIT ? 0x81 : control->type == COMBO || control->type == LIST ? 1 : 0x100;
    default: break;
    }
    switch (control->type) {
    case BUTTON:
        if (message == BM_GETCHECK)
            return control->check;
        if (message == BM_SETCHECK) {
            const unsigned int style = button_style(control);
            if ((style >= BS_CHECKBOX && style <= BS_AUTO3STATE) || style == BS_AUTORADIOBUTTON) {
                const uint32_t limit = style == BS_3STATE || style == BS_AUTO3STATE ? 2 : 1;
                if (wparam <= limit) {
                    control->check = (uint32_t)wparam;
                    nt_InvalidateRect(control->parent, NULL, false);
                }
            }
            return 0;
        }
        if (message == BM_CLICK) {
            if (active(control, control->parent) && button_style(control) != BS_GROUPBOX)
                click_button(control);
            return 0;
        }
        break;
    case EDIT:
        if (message == EM_LIMITTEXT) {
            control->limit = wparam != 0 && wparam < MAX_CONTROL_TEXT ? wparam : MAX_CONTROL_TEXT;
            return 0;
        }
        if (message == EM_SETSEL) {
            const size_t length = strlen(control->text);
            control->anchor = edit_position((int32_t)wparam, length);
            control->caret = (int32_t)wparam < 0 ? control->anchor : edit_position(lparam, length);
            nt_InvalidateRect(control->parent, NULL, false);
            return 0;
        }
        break;
    case COMBO:
        if (message == CB_ADDSTRING)
            return add_item(control, (const char *)lparam);
        if (message == CB_GETCURSEL)
            return control->selected;
        if (message == CB_SETCURSEL)
            return select_item(control, (int32_t)wparam);
        break;
    case LIST:
        if (message == NT_LB_ADDSTRING)
            return add_item(control, (const char *)lparam);
        if (message == NT_LB_GETCURSEL)
            return control->selected;
        if (message == NT_LB_SETCURSEL)
            return select_item(control, (int32_t)wparam);
        if (message == NT_LB_RESETCONTENT) {
            clear_items(control);
            nt_InvalidateRect(control->parent, NULL, false);
            return 0;
        }
        if (message == NT_LB_GETTEXT) {
            if (wparam >= control->item_count || lparam == 0)
                return -1;
            return copy_text(control->items[wparam], (char *)lparam, MAX_CONTROL_TEXT + 1);
        }
        break;
    case STATUS:
        if (message == SB_SETTEXTA && wparam == 0)
            return set_text(control, (const char *)lparam);
        if (message == SB_GETTEXTA && wparam == 0)
            return copy_text(control->text, (char *)lparam, MAX_CONTROL_TEXT + 1);
        if (message == SB_GETTEXTLENGTHA && wparam == 0)
            return (intptr_t)strlen(control->text);
        break;
    case LABEL:
        if ((control->style & 31u) == 3 && message == STM_SETICON) {
            const uintptr_t previous = control->icon;
            if (wparam == 0 || nt_runtime_icon(wparam) != NULL) {
                control->icon = wparam;
                nt_InvalidateRect(control->parent, NULL, false);
            }
            return (intptr_t)previous;
        }
        if (message == STM_GETICON)
            return (intptr_t)control->icon;
        break;
    }
    nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
    return 0;
}

static struct control *require_item(uintptr_t parent, int32_t id)
{
    if (!nt_runtime_window_valid(parent)) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return NULL;
    }
    struct control *control = find_item(parent, id);
    if (control == NULL)
        nt_SetLastError(1421); /* ERROR_CONTROL_ID_NOT_FOUND */
    return control;
}

intptr_t NT_MS_ABI nt_SendDlgItemMessageA(uintptr_t parent, int32_t id, uint32_t message,
                                        uintptr_t wparam, intptr_t lparam)
{
    const struct control *control = require_item(parent, id);
    return control != NULL ? nt_SendMessageA(control->handle, message, wparam, lparam) : 0;
}

uint32_t NT_MS_ABI nt_GetDlgItemTextA(uintptr_t parent, int32_t id, char *text, int32_t capacity)
{
    if (text == NULL || capacity <= 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return (uint32_t)nt_SendDlgItemMessageA(parent, id, NT_WM_GETTEXT, (uintptr_t)capacity, (intptr_t)text);
}

int32_t NT_MS_ABI nt_CheckDlgButton(uintptr_t parent, int32_t id, uint32_t state)
{
    const struct control *control = require_item(parent, id);
    if (control == NULL)
        return 0;
    nt_SendMessageA(control->handle, BM_SETCHECK, state, 0);
    return 1;
}

uint32_t NT_MS_ABI nt_IsDlgButtonChecked(uintptr_t parent, int32_t id)
{
    return (uint32_t)nt_SendDlgItemMessageA(parent, id, BM_GETCHECK, 0, 0);
}

static bool shift_down(void)
{
    unsigned char keys[256];
    return nt_GetKeyboardState(keys) && (keys[0x10] & 0x80) != 0;
}

static void edit_character(struct control *control, unsigned int key)
{
    if ((control->style & ES_READONLY) != 0 || (key != 8 && key != 127 && (key < 32 || key > 255)))
        return;
    const size_t length = strlen(control->text);
    size_t first = control->anchor < control->caret ? control->anchor : control->caret;
    size_t last = control->anchor > control->caret ? control->anchor : control->caret;
    const bool insert = key != 8 && key != 127;
    if (!insert && first == last) {
        if (key == 8 && first > 0)
            --first;
        else if (key == 127 && last < length)
            ++last;
        else
            return;
    }
    if (insert && length - (last - first) >= control->limit)
        return;
    const size_t new_length = length - (last - first) + insert;
    char *changed = malloc(new_length + 1);
    if (changed == NULL)
        return;
    memcpy(changed, control->text, first);
    if (insert)
        changed[first] = (char)key;
    memcpy(changed + first + insert, control->text + last, length - last + 1);
    free(control->text);
    control->text = changed;
    control->anchor = control->caret = first + insert;
    nt_InvalidateRect(control->parent, NULL, false);
    notify(control, EN_CHANGE);
}

static bool edit_key(struct control *control, const struct nt_message *message)
{
    const size_t length = strlen(control->text);
    switch (message->wparam) {
    case 0x23: control->caret = length; break;
    case 0x24: control->caret = 0; break;
    case 0x25: if (control->caret > 0) --control->caret; break;
    case 0x27: if (control->caret < length) ++control->caret; break;
    case 0x2e: edit_character(control, 127); return true;
    default:
        if (message->private_data == 0)
            return false;
        edit_character(control, message->private_data);
        return true;
    }
    if (!shift_down())
        control->anchor = control->caret;
    nt_InvalidateRect(control->parent, NULL, false);
    return true;
}

static void dialog_command(uintptr_t parent, int32_t id)
{
    struct control *control = find_item(parent, id);
    if (control != NULL && active(control, parent) && control->type == BUTTON)
        click_button(control);
    else if (control == NULL)
        nt_SendMessageA(parent, NT_WM_COMMAND, (uint16_t)id, 0);
}

static bool contains_point(struct nt_rect bounds, int x, int y)
{
    return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom;
}

static bool choose_at(struct control *control, int index)
{
    if (index < 0 || index >= (int)control->item_count)
        return false;
    const bool changed = index != control->selected;
    select_item(control, index);
    if (changed)
        notify(control, control->type == LIST ? LBN_SELCHANGE : CBN_SELCHANGE);
    return true;
}

static bool pointer_input(uintptr_t parent, int x, int y, bool down)
{
    struct nt_rect client;
    if (!nt_GetClientRect(parent, &client))
        return false;
    struct control *focused = focused_control(parent);
    if (focused != NULL && focused->expanded) {
        const struct nt_rect box = control_bounds(focused, client.right, client.bottom);
        struct nt_rect list = box;
        list.top = list.bottom;
        list.bottom += rows(focused) * ROW_HEIGHT + 2;
        if (!down) {
            focused->expanded = false;
            nt_InvalidateRect(parent, NULL, false);
            if (contains_point(list, x, y)) {
                choose_at(focused, focused->top + (y - list.top - 1) / ROW_HEIGHT);
                return true;
            }
            if (contains_point(box, x, y)) {
                focused->pressed = false;
                return true;
            }
        } else if (contains_point(list, x, y)) {
            return true;
        }
    }
    bool released = false;
    for (size_t i = 0; i < MAX_CONTROLS; ++i) {
        struct control *control = &controls[i];
        if (!active(control, parent) || control->type == LABEL || control->type == STATUS ||
            (control->type == BUTTON && button_style(control) == BS_GROUPBOX))
            continue;
        const bool pressed = control->pressed;
        control->pressed = false;
        released |= pressed;
        const struct nt_rect bounds = control_bounds(control, client.right, client.bottom);
        if (!contains_point(bounds, x, y))
            continue;
        if (down) {
            focus(control);
            control->pressed = true;
            if (control->type == EDIT)
                control->caret = control->anchor = strlen(control->text);
        } else if (pressed) {
            if (control->type == BUTTON) {
                click_button(control);
                return true;
            }
            if (control->type == COMBO)
                control->expanded = control->item_count != 0;
            if (control->type == LIST) {
                choose_at(control, control->top + (y - bounds.top - 1) / ROW_HEIGHT);
                return true;
            }
        }
        nt_InvalidateRect(parent, NULL, false);
        return true;
    }
    if (released)
        nt_InvalidateRect(parent, NULL, false);
    return released;
}

bool nt_runtime_dialog_input(uintptr_t parent, const struct nt_message *message, bool dialog)
{
    if (!nt_runtime_window_enabled(parent))
        return false;
    if (message->message == NT_WM_LBUTTONDOWN || message->message == NT_WM_LBUTTONUP)
        return pointer_input(parent, (int16_t)message->lparam, (int16_t)((uintptr_t)message->lparam >> 16),
                             message->message == NT_WM_LBUTTONDOWN);
    struct control *control = message->window == parent ? focused_control(parent) : find_control(message->window);
    if (control != NULL && !active(control, parent))
        control = NULL;
    if (message->message == NT_WM_CHAR && control != NULL && control->type == EDIT) {
        edit_character(control, (unsigned int)message->wparam);
        return true;
    }
    if (message->message != NT_WM_KEYDOWN)
        return false;
    const uintptr_t key = message->wparam;
    if (dialog && key == VK_TAB)
        return nt_runtime_focus_control(parent, 0, shift_down());
    if (dialog && key == VK_ESCAPE) {
        if (control != NULL && control->expanded) {
            control->expanded = false;
            nt_InvalidateRect(parent, NULL, false);
        } else {
            dialog_command(parent, NT_IDCANCEL);
        }
        return true;
    }
    if (control != NULL && (control->type == COMBO || control->type == LIST) &&
        (key == 0x26 || key == 0x28 || key == 0x21 || key == 0x22)) {
        const int step = key == 0x21 || key == 0x22 ? rows(control) : 1;
        int next = control->selected + (key == 0x26 || key == 0x21 ? -step : step);
        if (next < 0)
            next = 0;
        if (next >= (int)control->item_count)
            next = (int)control->item_count - 1;
        choose_at(control, next);
        return true;
    }
    if (control != NULL && control->type == COMBO && control->expanded && key == VK_RETURN) {
        control->expanded = false;
        nt_InvalidateRect(parent, NULL, false);
        return true;
    }
    if (control != NULL && control->type == BUTTON && (key == VK_SPACE || (dialog && key == VK_RETURN))) {
        click_button(control);
        return true;
    }
    if (dialog && key == VK_RETURN) {
        const intptr_t result = nt_SendMessageA(parent, 0x400, 0, 0); /* DM_GETDEFID */
        const int32_t id = ((uintptr_t)result >> 16) == 0x534b ? (uint16_t)result : NT_IDOK;
        dialog_command(parent, id);
        return true;
    }
    return control != NULL && control->type == EDIT && edit_key(control, message);
}

int32_t NT_MS_ABI nt_IsDialogMessageA(uintptr_t parent, const struct nt_message *message)
{
    if (!nt_runtime_window_valid(parent) || message == NULL)
        return 0;
    const struct control *control = find_control(message->window);
    if (message->window != parent && (control == NULL || control->parent != parent))
        return 0;
    if (!nt_runtime_dialog_input(parent, message, true)) {
        nt_TranslateMessage(message);
        nt_DispatchMessageA(message);
    }
    return 1;
}

static void background(cairo_t *cr, double gray)
{
    cairo_set_source_rgb(cr, gray, gray, gray);
    cairo_paint(cr);
}

static void frame(cairo_t *cr, int width, int height, bool focused)
{
    cairo_set_source_rgb(cr, focused ? 0.15 : 0.46, focused ? 0.37 : 0.46, focused ? 0.67 : 0.46);
    cairo_set_line_width(cr, focused ? 2 : 1);
    cairo_rectangle(cr, 1, 1, width - 2, height - 2);
    cairo_stroke(cr);
}

static void text_at(cairo_t *cr, const char *text, size_t length, double x, double y, bool enabled)
{
    char *utf8 = nt_runtime_ansi_to_utf8(text, length);
    if (utf8 == NULL)
        return;
    const double gray = enabled ? 0.09 : 0.48;
    cairo_set_source_rgb(cr, gray, gray, gray);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, utf8);
    free(utf8);
}

static void draw_label(cairo_t *cr, const char *text, int width, int height, bool enabled)
{
    const size_t columns = width >= 8 ? (size_t)width / 8 : 1;
    for (int baseline = 15; *text != '\0' && baseline <= height; baseline += 18) {
        size_t length = strcspn(text, "\r\n");
        if (length > columns) {
            length = columns;
            while (length > 0 && text[length] != ' ')
                --length;
            if (length == 0)
                length = columns;
        }
        text_at(cr, text, length, 3, baseline, enabled);
        text += length;
        if (*text == ' ' || *text == '\r')
            ++text;
        if (*text == '\n')
            ++text;
    }
}

static void draw_button(cairo_t *cr, const struct control *control, int width, int height, bool enabled)
{
    const unsigned int style = button_style(control);
    background(cr, control->pressed ? 0.73 : 0.93);
    if (style == 0 || style == BS_DEFPUSHBUTTON || style == BS_GROUPBOX) {
        frame(cr, width, height, control->focused || style == BS_DEFPUSHBUTTON);
        draw_label(cr, control->text, width - 5, height - 2, enabled);
        return;
    }
    const bool radio = style == BS_RADIOBUTTON || style == BS_AUTORADIOBUTTON;
    const double y = height / 2.0;
    if (radio)
        cairo_arc(cr, 10, y, 6, 0, 6.283185307);
    else
        cairo_rectangle(cr, 4, y - 6, 12, 12);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    if (control->check != 0) {
        cairo_set_source_rgb(cr, 0.17, 0.38, 0.67);
        if (radio)
            cairo_arc(cr, 10, y, 3, 0, 6.283185307);
        else
            cairo_rectangle(cr, 6, y - (control->check == 2 ? 1 : 4), 8, control->check == 2 ? 2 : 8);
        cairo_fill(cr);
    }
    text_at(cr, control->text, strlen(control->text), 22, y + 5, enabled);
}

static void draw_list(cairo_t *cr, const struct control *control, int width, int height, bool enabled)
{
    background(cr, 1);
    frame(cr, width, height, control->focused);
    for (int i = 0; i < rows(control) && i + control->top < (int)control->item_count; ++i) {
        const int index = i + control->top, y = i * ROW_HEIGHT + 2;
        if (index == control->selected) {
            cairo_set_source_rgb(cr, 0.67, 0.80, 0.94);
            cairo_rectangle(cr, 2, y, width - 4, ROW_HEIGHT);
            cairo_fill(cr);
        }
        const char *text = control->items[index];
        text_at(cr, text, strlen(text), 6, y + 15, enabled);
    }
}

static void draw_field(cairo_t *cr, const struct control *control, int width, int height, bool enabled)
{
    background(cr, control->type == STATUS ? 0.90 : 1);
    frame(cr, width, height, control->focused);
    const char *text = control_text(control);
    const size_t columns = width > 12 ? (size_t)(width - 12) / 8 : 1;
    const size_t start = control->type == EDIT && control->caret >= columns ?
                         control->caret - columns + 1 : 0;
    if (control->type == EDIT && control->focused) {
        const size_t first = control->anchor < control->caret ? control->anchor : control->caret;
        const size_t last = control->anchor > control->caret ? control->anchor : control->caret;
        if (last > first && last > start) {
            const size_t visible = first > start ? first - start : 0;
            cairo_set_source_rgb(cr, 0.67, 0.80, 0.94);
            cairo_rectangle(cr, 5 + visible * 8, 3, (last - start - visible) * 8, height - 6);
            cairo_fill(cr);
        }
    }
    text_at(cr, text + start, strlen(text + start), 5, height / 2.0 + 5, enabled);
    if (control->type == EDIT && control->focused && control->anchor == control->caret) {
        cairo_set_source_rgb(cr, 0.13, 0.26, 0.40);
        cairo_rectangle(cr, 5 + (control->caret - start) * 8, 4, 1, height - 8);
        cairo_fill(cr);
    }
    if (control->type == COMBO) {
        cairo_set_source_rgb(cr, 0.21, 0.25, 0.28);
        cairo_move_to(cr, width - 16, height / 2.0 - 2);
        cairo_line_to(cr, width - 6, height / 2.0 - 2);
        cairo_line_to(cr, width - 11, height / 2.0 + 3);
        cairo_fill(cr);
    }
}

static void draw_icon(cairo_t *cr, const SDL_Surface *icon)
{
    cairo_surface_t *canvas = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, icon->w, icon->h);
    if (cairo_surface_status(canvas) == CAIRO_STATUS_SUCCESS) {
        unsigned char *pixels = cairo_image_surface_get_data(canvas);
        const size_t stride = (size_t)cairo_image_surface_get_stride(canvas);
        for (int y = 0; y < icon->h; ++y) {
            uint32_t *row = (uint32_t *)(pixels + (size_t)y * stride);
            const unsigned char *source = (const unsigned char *)icon->pixels + (size_t)y * icon->pitch;
            for (int x = 0; x < icon->w; ++x, source += 4) {
                const uint32_t alpha = source[3];
                row[x] = alpha << 24 | (source[0] * alpha / 255) << 16 |
                         (source[1] * alpha / 255) << 8 | source[2] * alpha / 255;
            }
        }
        cairo_surface_mark_dirty(canvas);
        cairo_set_source_surface(cr, canvas, 0, 0);
        cairo_paint(cr);
    }
    cairo_surface_destroy(canvas);
}

static bool paint_control(SDL_Surface *surface, const struct control *control, struct nt_rect bounds,
                          int offset, bool popup)
{
    const struct nt_rect visible = {0, 0, surface->w, surface->h - offset};
    struct nt_rect clipped;
    if (!nt_IntersectRect(&clipped, &bounds, &visible))
        return true;
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const int clipped_width = clipped.right - clipped.left, clipped_height = clipped.bottom - clipped.top;
    cairo_surface_t *canvas = cairo_image_surface_create(CAIRO_FORMAT_RGB24, clipped_width, clipped_height);
    cairo_t *cr = cairo_create(canvas);
    cairo_translate(cr, bounds.left - clipped.left, bounds.top - clipped.top);
    if (control->font == 0 || !nt_runtime_apply_font(control->font, cr)) {
        cairo_select_font_face(cr, control->type == EDIT ? "monospace" : "sans",
                               CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 13);
    }
    const bool enabled = (control->style & WS_DISABLED) == 0;
    if (popup || control->type == LIST)
        draw_list(cr, control, width, height, enabled);
    else if (control->type == BUTTON)
        draw_button(cr, control, width, height, enabled);
    else if (control->type == LABEL) {
        background(cr, 0.93);
        const SDL_Surface *icon = nt_runtime_icon(control->icon);
        if (icon != NULL)
            draw_icon(cr, icon);
        else
            draw_label(cr, control->text, width, height, enabled);
    } else {
        draw_field(cr, control, width, height, enabled);
    }
    const bool drawn = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
    cairo_destroy(cr);
    cairo_surface_flush(canvas);
    unsigned char *destination = (unsigned char *)surface->pixels +
        (size_t)(clipped.top + offset) * surface->pitch + (size_t)clipped.left * surface->format->BytesPerPixel;
    const bool converted = drawn && SDL_ConvertPixels(clipped_width, clipped_height, SDL_PIXELFORMAT_XRGB8888,
        cairo_image_surface_get_data(canvas), cairo_image_surface_get_stride(canvas),
        surface->format->format, destination, surface->pitch) == 0;
    cairo_surface_destroy(canvas);
    return converted;
}

bool nt_runtime_draw_controls(SDL_Surface *surface, uintptr_t parent, int offset)
{
    const struct control *popup = NULL;
    for (size_t i = 0; i < MAX_CONTROLS; ++i) {
        const struct control *control = &controls[i];
        if (control->handle == 0 || control->parent != parent || (control->style & NT_WS_VISIBLE) == 0)
            continue;
        const struct nt_rect bounds = control_bounds(control, surface->w, surface->h - offset);
        if (!paint_control(surface, control, bounds, offset, false))
            return false;
        if (control->expanded)
            popup = control;
    }
    if (popup == NULL)
        return true;
    struct nt_rect bounds = control_bounds(popup, surface->w, surface->h - offset);
    bounds.top = bounds.bottom;
    bounds.bottom += rows(popup) * ROW_HEIGHT + 2;
    return paint_control(surface, popup, bounds, offset, true);
}
