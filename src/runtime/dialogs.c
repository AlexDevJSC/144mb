/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "dialogs.h"
#include "desktop_resources.h"
#include "task_dialog.h"

#include <stdlib.h>
#include <string.h>

enum { WS_CAPTION = 0xc00000, WS_SYSMENU = 0x80000, COLOR_BTNFACE = 15 };
static const char modal_class[] = "ntnative.dialog";
static bool dialog_class_registered;
static struct {
    char *guest_name, *title, *text;
    uintptr_t window;
} help;

uintptr_t nt_runtime_create_dialog(const char *caption, uintptr_t owner, int width, int height,
                                   nt_window_proc callback, intptr_t data)
{
    if (!dialog_class_registered) {
        const struct nt_window_class definition = {.proc = nt_DefDlgProcA, .window_extra = NT_DLGWINDOWEXTRA,
            .background = COLOR_BTNFACE + 1, .name = modal_class};
        if (nt_RegisterClassA(&definition) == 0)
            return 0;
        dialog_class_registered = true;
    }
    const uintptr_t handle = nt_CreateWindowExA(0, modal_class, caption, WS_CAPTION | WS_SYSMENU,
                                               INT32_MIN, INT32_MIN, width, height, owner, 0, 0, NULL);
    if (handle != 0) {
        nt_SetWindowLongPtrA(handle, NT_DWLP_DLGPROC, (intptr_t)callback);
        nt_SetWindowLongPtrA(handle, NT_DWLP_USER, data);
    }
    return handle;
}

uintptr_t nt_runtime_add_dialog_control(uintptr_t dialog, const char *type, const char *text,
                                       int id, uint32_t style, int x, int y, int width, int height)
{
    return nt_CreateWindowExA(0, type, text, NT_WS_CHILD | NT_WS_VISIBLE | style, x, y, width, height,
                              dialog, (uintptr_t)(intptr_t)id, 0, NULL);
}

int nt_runtime_run_modal(uintptr_t dialog, uintptr_t owner, const int *result)
{
    if (!nt_runtime_dialog_window(dialog))
        return -1;
    const bool restore = owner != 0 && nt_runtime_window_enabled(owner);
    if (owner != 0) {
        nt_runtime_set_modal_owner(dialog, owner);
        if (restore)
            nt_EnableWindow(owner, 0);
    }
    if (nt_runtime_dialog_window(dialog)) {
        const uintptr_t initial = nt_runtime_initial_dialog_focus(dialog);
        nt_SendMessageA(dialog, NT_WM_INITDIALOG, initial, 0);
        if (nt_runtime_dialog_window(dialog))
            nt_ShowWindow(dialog, 5);
    }
    while (*result == 0 && nt_runtime_dialog_window(dialog)) {
        struct nt_message message;
        const int32_t status = nt_GetMessageA(&message, 0, 0, 0);
        if (status <= 0) {
            if (status == 0)
                nt_PostQuitMessage((int32_t)message.wparam);
            break;
        }
        if (!nt_IsDialogMessageA(dialog, &message)) {
            nt_TranslateMessage(&message);
            nt_DispatchMessageA(&message);
        }
    }
    if (nt_runtime_dialog_window(dialog))
        nt_DestroyWindow(dialog);
    if (restore && nt_runtime_window_valid(owner))
        nt_EnableWindow(owner, 1);
    return *result != 0 ? *result : -1;
}

struct message_box {
    int result;
    uint16_t close_id, choices;
};

static bool populate_text_list(uintptr_t list, const char *text, size_t columns);

static intptr_t NT_MS_ABI message_box_proc(uintptr_t dialog, uint32_t message, uintptr_t wparam, intptr_t lparam)
{
    (void)lparam;
    struct message_box *box = (void *)nt_GetWindowLongPtrA(dialog, NT_DWLP_USER);
    if (message == NT_WM_INITDIALOG)
        return 1;
    if (message == NT_WM_CLOSE || (message == NT_WM_COMMAND && (uint16_t)wparam == NT_IDCANCEL)) {
        box->result = box->close_id;
        return 1;
    }
    if (message == NT_WM_COMMAND && wparam < 16 && (box->choices & (1u << wparam)) != 0) {
        box->result = (int)wparam;
        return 1;
    }
    return 0;
}

int32_t NT_MS_ABI nt_MessageBoxA(uintptr_t owner, const char *text, const char *caption, uint32_t type)
{
    enum { IDABORT = 3, IDRETRY = 4, IDIGNORE = 5, IDYES = 6, IDNO = 7, IDTRYAGAIN = 10, IDCONTINUE = 11 };
    static const uint16_t buttons[][3] = {
        {NT_IDOK}, {NT_IDOK, NT_IDCANCEL}, {IDABORT, IDRETRY, IDIGNORE}, {IDYES, IDNO, NT_IDCANCEL},
        {IDYES, IDNO}, {IDRETRY, NT_IDCANCEL}, {NT_IDCANCEL, IDTRYAGAIN, IDCONTINUE}
    };
    static const char *const labels[] = {NULL, "OK", "Cancel", "Abort", "Retry", "Ignore",
                                          "Yes", "No", NULL, NULL, "Try again", "Continue"};
    static const uint16_t icons[] = {0, 32513, 32514, 32515, 32516};
    const uint32_t mode = type & 15u, image = (type >> 4) & 15u;
    const uint32_t preferred = (type >> 8) & 3u;
    if ((type & ~UINT32_C(0x37f)) != 0 || mode >= sizeof(buttons) / sizeof(buttons[0]) ||
        image >= sizeof(icons) / sizeof(icons[0])) {
        nt_SetLastError(NT_ERROR_INVALID_FLAGS);
        return 0;
    }
    if (text == NULL)
        text = "";
    if (caption == NULL)
        caption = "Error";
    if (strnlen(text, 4096) == 4096) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct message_box box = {.close_id = mode == 0 ? NT_IDOK : 0};
    unsigned int count = 0;
    while (count < 3 && buttons[mode][count] != 0) {
        box.choices |= (uint16_t)(1u << buttons[mode][count]);
        if (buttons[mode][count] == NT_IDCANCEL)
            box.close_id = NT_IDCANCEL;
        ++count;
    }
    const int width = strlen(text) > 35 ? 540 : 340;
    const int text_left = image != 0 ? 56 : 16;
    const size_t columns = (size_t)(width - text_left - 16) / 8;
    size_t lines = 1;
    for (const char *next = text; *next != '\0';) {
        const size_t run = strcspn(next, "\n");
        lines += run / (columns - 5);
        next += run;
        if (*next != '\0') {
            ++lines;
            ++next;
        }
    }
    const bool scroll = lines > 23;
    const int height = scroll ? 490 : 92 + (int)lines * 18;
    const uintptr_t dialog = nt_runtime_create_dialog(caption, owner, width, height, message_box_proc, (intptr_t)&box);
    if (dialog == 0)
        return 0;
    const uintptr_t label = nt_runtime_add_dialog_control(dialog, scroll ? "LISTBOX" : "STATIC",
        scroll ? "" : text, -1, scroll ? NT_WS_TABSTOP : 0,
        text_left, 12, width - text_left - 12, height - 58);
    bool ok = label != 0 && (!scroll || populate_text_list(label, text, columns - 1));
    if (ok && image != 0) {
        const uintptr_t icon = nt_LoadIconA(0, (const char *)(uintptr_t)icons[image]);
        const uintptr_t control = nt_runtime_add_dialog_control(dialog, "STATIC", "", -1, 3, 14, 12, 32, 32);
        ok = icon != 0 && control != 0;
        if (ok)
            nt_SendMessageA(control, 0x170, icon, 0); /* STM_SETICON */
    }
    uintptr_t default_button = 0;
    for (unsigned int i = 0; ok && i < count; ++i) {
        const bool is_default = i == (preferred < count ? preferred : 0);
        const uintptr_t button = nt_runtime_add_dialog_control(dialog, "BUTTON", labels[buttons[mode][i]],
            buttons[mode][i], NT_WS_TABSTOP | (uint32_t)is_default,
            (width - 100 * (int)count) / 2 + 100 * (int)i, height - 42, 92, 26);
        ok = button != 0;
        if (is_default)
            default_button = button;
    }
    if (!ok) {
        nt_DestroyWindow(dialog);
        return 0;
    }
    nt_runtime_focus_control(dialog, default_button, false);
    const int result = nt_runtime_run_modal(dialog, owner, &box.result);
    return result < 0 ? 0 : result;
}

static const char *leaf_name(const char *path)
{
    const char *leaf = path;
    for (; *path != '\0'; ++path) {
        if (*path == '/' || *path == '\\')
            leaf = path + 1;
    }
    return leaf;
}

bool nt_runtime_set_help_document(const char *guest_name, const char *title, const char *text)
{
    if (!nt_runtime_desktop_active() ||
        (guest_name != NULL && (title == NULL || text == NULL || *guest_name == '\0' ||
         strnlen(guest_name, 256) >= 256 || strnlen(title, 256) >= 256 ||
         strnlen(text, 12001) > 12000 || leaf_name(guest_name) != guest_name)))
        return false;
    char *name_copy = NULL, *title_copy = NULL, *text_copy = NULL;
    if (guest_name != NULL) {
        name_copy = strdup(guest_name);
        title_copy = strdup(title);
        text_copy = strdup(text);
        if (name_copy == NULL || title_copy == NULL || text_copy == NULL) {
            free(name_copy);
            free(title_copy);
            free(text_copy);
            return false;
        }
    }
    if (help.window != 0)
        nt_DestroyWindow(help.window);
    free(help.guest_name);
    free(help.title);
    free(help.text);
    help.guest_name = name_copy;
    help.title = title_copy;
    help.text = text_copy;
    return true;
}

void nt_runtime_close_dialogs(void)
{
    /* The desktop has already released its windows without dispatching callbacks. */
    help.window = 0;
    nt_runtime_set_help_document(NULL, NULL, NULL);
    dialog_class_registered = false;
}

static intptr_t NT_MS_ABI help_proc(uintptr_t dialog, uint32_t message, uintptr_t wparam, intptr_t lparam)
{
    (void)lparam;
    if (message == NT_WM_CLOSE || (message == NT_WM_COMMAND && (uint16_t)wparam == NT_IDCANCEL)) {
        nt_DestroyWindow(dialog);
        return 1;
    }
    if (message == NT_WM_NCDESTROY && help.window == dialog)
        help.window = 0;
    return 0;
}

static bool populate_text_list(uintptr_t list, const char *text, size_t columns)
{
    char line[76];
    while (*text != '\0') {
        size_t length = strcspn(text, "\r\n");
        if (length > columns) {
            length = columns;
            while (length > 0 && text[length] != ' ')
                --length;
            if (length == 0)
                length = columns;
        }
        memcpy(line, text, length);
        line[length] = '\0';
        if (nt_SendMessageA(list, NT_LB_ADDSTRING, 0, (intptr_t)line) < 0)
            return false;
        text += length;
        if (*text == '\r' || *text == ' ')
            ++text;
        if (*text == '\n')
            ++text;
    }
    return true;
}

int32_t NT_MS_ABI nt_WinHelpA(uintptr_t owner, const char *file, uint32_t command, uintptr_t data)
{
    enum { HELP_QUIT = 2, HELP_CONTENTS = 3, HELP_FINDER = 11 };
    if (!nt_runtime_desktop_active()) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (command == HELP_QUIT) {
        if (help.window != 0)
            nt_DestroyWindow(help.window);
        return 1;
    }
    if ((command != HELP_CONTENTS && command != HELP_FINDER) || data != 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (file == NULL || strnlen(file, 4096) == 4096 || help.guest_name == NULL ||
        !nt_module_name_equal(help.guest_name, leaf_name(file))) {
        nt_SetLastError(NT_ERROR_FILE_NOT_FOUND);
        return 0;
    }
    if (help.window != 0) {
        nt_ShowWindow(help.window, 5);
        nt_SetForegroundWindow(help.window);
        return 1;
    }
    const uintptr_t window = nt_runtime_create_dialog(help.title, owner, 624, 436, help_proc, 0);
    if (window == 0)
        return 0;
    const uintptr_t list = nt_runtime_add_dialog_control(window, "LISTBOX", "", 100, NT_WS_TABSTOP,
                                                         14, 14, 596, 364);
    const uintptr_t close_button = nt_runtime_add_dialog_control(window, "BUTTON", "Close", NT_IDCANCEL,
        NT_WS_TABSTOP | 1u, 510, 390, 100, 28);
    if (list == 0 || close_button == 0 || !populate_text_list(list, help.text, 75)) {
        nt_DestroyWindow(window);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    nt_runtime_focus_control(window, list, false);
    help.window = window;
    nt_ShowWindow(window, 5);
    return 1;
}

_Static_assert(sizeof(struct nt_task_config) == 160, "TASKDIALOGCONFIG packed AMD64 layout");
_Static_assert(sizeof(struct nt_task_button) == 12, "TASKDIALOG_BUTTON packed AMD64 layout");

enum { MAX_TASK_BUTTONS = 8, TASK_ALLOW_CANCEL = 8, TASK_USE_ICON = 2 };
struct task_dialog {
    const struct nt_task_config *config;
    int result;
    uint16_t ids[MAX_TASK_BUTTONS], close_id;
    size_t count;
};

static int32_t task_notify(const struct task_dialog *task, uintptr_t dialog, uint32_t message, uintptr_t value)
{
    return task->config->callback != NULL ?
           task->config->callback(dialog, message, value, 0, task->config->context) : 0;
}

static intptr_t NT_MS_ABI task_dialog_proc(uintptr_t dialog, uint32_t message,
                                           uintptr_t wparam, intptr_t lparam)
{
    (void)lparam;
    struct task_dialog *task = (void *)nt_GetWindowLongPtrA(dialog, NT_DWLP_USER);
    if (message == NT_WM_INITDIALOG) {
        task_notify(task, dialog, 7, 0); /* TDN_DIALOG_CONSTRUCTED */
        if (nt_runtime_window_valid(dialog))
            task_notify(task, dialog, 0, 0); /* TDN_CREATED */
        return 1;
    }
    if (message != NT_WM_CLOSE && message != NT_WM_COMMAND && message != 0x466)
        return 0;
    const uint16_t pressed = (uint16_t)wparam;
    if (message == NT_WM_CLOSE || pressed == NT_IDCANCEL) {
        if (task->close_id != 0 && task_notify(task, dialog, 2, task->close_id) != 1)
            task->result = task->close_id;
        return 1;
    }
    for (size_t i = 0; i < task->count; ++i) {
        if (task->ids[i] == pressed) {
            if (task_notify(task, dialog, 2, pressed) != 1)
                task->result = pressed;
            return 1;
        }
    }
    return 0;
}

static bool task_text(const uint16_t *text, char *result, int32_t capacity)
{
    if (text == NULL) {
        result[0] = '\0';
        return true;
    }
    return (uintptr_t)text > UINT16_MAX && nt_runtime_wide_length(text, 4096) < 4096 &&
           nt_WideCharToMultiByte(1252, 0, text, -1, result, capacity, NULL, NULL) != 0;
}

int32_t NT_MS_ABI nt_TaskDialogIndirect(const struct nt_task_config *config, int32_t *button,
                                        int32_t *radio, int32_t *checked)
{
    const int32_t invalid_argument = (int32_t)UINT32_C(0x80070057);
    const int32_t not_supported = (int32_t)UINT32_C(0x80004001);
    const int32_t failure = (int32_t)UINT32_C(0x80004005);
    if (config == NULL || config->size != sizeof(*config) || config->button_count > MAX_TASK_BUTTONS ||
        (config->button_count != 0 && config->buttons == NULL))
        return invalid_argument;
    if ((config->flags & ~(TASK_ALLOW_CANCEL | TASK_USE_ICON | 0x01000000u)) != 0 ||
        (config->common_buttons & ~63u) != 0 || config->radio_count != 0 ||
        config->verification != NULL || config->expanded != NULL || config->footer_icon != 0)
        return not_supported;

    struct task_dialog task = {.config = config};
    char title[512], instruction[1024], content[3072], footer[1024];
    if (!task_text(config->title, title, sizeof(title)) ||
        !task_text(config->instruction, instruction, sizeof(instruction)) ||
        !task_text(config->content, content, sizeof(content)) ||
        !task_text(config->footer, footer, sizeof(footer)))
        return invalid_argument;

    static const uint16_t common_ids[] = {1, 6, 7, 2, 4, 8};
    static const char *const common_labels[] = {"OK", "Yes", "No", "Cancel", "Retry", "Close"};
    char custom[MAX_TASK_BUTTONS][192];
    const char *labels[MAX_TASK_BUTTONS];
    for (size_t i = 0; i < config->button_count; ++i) {
        const struct nt_task_button *source = &config->buttons[i];
        if (source->id <= 0 || source->id > UINT16_MAX || source->label == NULL ||
            !task_text(source->label, custom[i], sizeof(custom[i])))
            return invalid_argument;
        for (size_t j = 0; j < task.count; ++j) {
            if (task.ids[j] == source->id)
                return invalid_argument;
        }
        task.ids[task.count] = (uint16_t)source->id;
        labels[task.count++] = custom[i];
    }
    uint32_t common = config->common_buttons;
    if (task.count == 0 && common == 0)
        common = 1;
    for (size_t i = 0; i < sizeof(common_ids) / sizeof(common_ids[0]); ++i) {
        if ((common & (1u << i)) == 0)
            continue;
        if (task.count == MAX_TASK_BUTTONS)
            return not_supported;
        for (size_t j = 0; j < task.count; ++j) {
            if (task.ids[j] == common_ids[i])
                return invalid_argument;
        }
        task.ids[task.count] = common_ids[i];
        labels[task.count++] = common_labels[i];
    }
    if ((config->flags & TASK_ALLOW_CANCEL) != 0 || (common & 8) != 0)
        task.close_id = NT_IDCANCEL;

    uintptr_t icon = 0;
    if ((config->flags & TASK_USE_ICON) != 0)
        icon = config->icon;
    else if (config->icon != 0) {
        const unsigned int stock = config->icon == 65535 ? 32515 : config->icon == 65534 ? 32513 :
                                   config->icon == 65533 ? 32516 : 0;
        if (stock == 0)
            return not_supported;
        icon = nt_LoadIconA(0, (const char *)(uintptr_t)stock);
    }
    if (icon != 0 && nt_runtime_icon(icon) == NULL)
        return invalid_argument;
    const int width = config->width != 0 && config->width <= 360 ? 240 + (int)config->width : 540;
    const int columns = width / 108, rows = ((int)task.count + columns - 1) / columns;
    const int text_left = icon != 0 ? 54 : 14;
    const int text_height = strlen(content) < 96 ? 88 : strlen(content) < 300 ? 148 : 235;
    const int footer_height = *footer != '\0' ? 28 : 0;
    const int height = 44 + text_height + footer_height + rows * 38;
    const uintptr_t dialog = nt_runtime_create_dialog(*title != '\0' ? title : "Task dialog",
        config->parent, width, height, task_dialog_proc, (intptr_t)&task);
    if (dialog == 0)
        return failure;
    bool ok = nt_runtime_add_dialog_control(dialog, "STATIC", instruction, -1, 0, text_left, 12,
                                            width - text_left - 14, 28) != 0;
    const bool scroll = strlen(content) >= 300;
    const uintptr_t text = ok ? nt_runtime_add_dialog_control(dialog, scroll ? "LISTBOX" : "STATIC",
        scroll ? "" : content, -1, scroll ? NT_WS_TABSTOP : 0,
        text_left, 44, width - text_left - 14, text_height - 8) : 0;
    ok = text != 0 && (!scroll || populate_text_list(text, content, (size_t)(width - text_left - 14) / 8));
    if (ok && footer_height != 0)
        ok = nt_runtime_add_dialog_control(dialog, "STATIC", footer, -1, 0, text_left,
                                            44 + text_height, width - text_left - 14, footer_height) != 0;
    if (ok && icon != 0) {
        const uintptr_t control = nt_runtime_add_dialog_control(dialog, "STATIC", "", -1, 3, 12, 12, 32, 32);
        ok = control != 0;
        if (ok)
            nt_SendMessageA(control, 0x170, icon, 0);
    }
    size_t preferred = 0;
    for (size_t i = 0; i < task.count; ++i) {
        if (task.ids[i] == config->default_button)
            preferred = i;
    }
    uintptr_t focus = 0;
    for (size_t i = 0; ok && i < task.count; ++i) {
        const uintptr_t control = nt_runtime_add_dialog_control(dialog, "BUTTON", labels[i], task.ids[i],
            NT_WS_TABSTOP | (i == preferred ? 1u : 0u), 8 + (int)(i % (size_t)columns) * 108,
            44 + text_height + footer_height + (int)(i / (size_t)columns) * 38, 100, 28);
        ok = control != 0;
        if (i == preferred)
            focus = control;
    }
    if (!ok) {
        nt_DestroyWindow(dialog);
        return failure;
    }
    nt_runtime_focus_control(dialog, focus, false);
    const int result = nt_runtime_run_modal(dialog, config->parent, &task.result);
    task_notify(&task, dialog, 5, 0); /* TDN_DESTROYED */
    if (result < 0)
        return failure;
    if (button != NULL)
        *button = result;
    if (radio != NULL)
        *radio = 0;
    if (checked != NULL)
        *checked = 0;
    return 0;
}
