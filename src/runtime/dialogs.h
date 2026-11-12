/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_DIALOGS_H
#define NTNATIVE_DIALOGS_H

#include "desktop.h"

struct SDL_Surface;

enum {
    NT_WS_CHILD = 0x40000000, NT_WS_VISIBLE = 0x10000000, NT_WS_TABSTOP = 0x10000,
    NT_WM_INITDIALOG = 0x110, NT_WM_NEXTDLGCTL = 0x28,
    NT_IDOK = 1, NT_IDCANCEL = 2, NT_DLGWINDOWEXTRA = 30,
    NT_DWLP_MSGRESULT = 0, NT_DWLP_DLGPROC = 8, NT_DWLP_USER = 16,
    NT_LB_ADDSTRING = 0x180, NT_LB_RESETCONTENT = 0x184, NT_LB_SETCURSEL = 0x186,
    NT_LB_GETCURSEL = 0x188, NT_LB_GETTEXT = 0x189
};

uintptr_t nt_runtime_create_control(const char *type, const char *text, uint32_t style,
                                    uint32_t exstyle, int32_t x, int32_t y, int32_t width,
                                    int32_t height, uintptr_t parent, uintptr_t id);
bool nt_runtime_control_valid(uintptr_t handle);
bool nt_runtime_control_rect(uintptr_t handle, struct nt_rect *rect, bool screen);
int32_t nt_runtime_control_show(uintptr_t handle, bool show);
int32_t nt_runtime_control_enable(uintptr_t handle, bool enable);
bool nt_runtime_destroy_control(uintptr_t handle);
void nt_runtime_remove_controls(uintptr_t parent);
void nt_runtime_close_controls(void);
intptr_t nt_runtime_control_message(uintptr_t handle, uint32_t message,
                                     uintptr_t wparam, intptr_t lparam);
bool nt_runtime_draw_controls(struct SDL_Surface *surface, uintptr_t parent, int offset);
bool nt_runtime_dialog_input(uintptr_t parent, const struct nt_message *message, bool dialog);
bool nt_runtime_focus_control(uintptr_t parent, uintptr_t control, bool previous);
uintptr_t nt_runtime_initial_dialog_focus(uintptr_t parent);
uintptr_t nt_runtime_dialog_item(uintptr_t parent, int32_t id);
uint16_t nt_runtime_dialog_default_id(uintptr_t parent);
bool nt_runtime_dialog_window(uintptr_t handle);
bool nt_runtime_window_enabled(uintptr_t handle);
bool nt_runtime_set_modal_owner(uintptr_t handle, uintptr_t owner);

intptr_t NT_MS_ABI nt_DefDlgProcA(uintptr_t window, uint32_t message, uintptr_t wparam, intptr_t lparam);
int32_t NT_MS_ABI nt_IsDialogMessageA(uintptr_t window, const struct nt_message *message);
int32_t NT_MS_ABI nt_CheckDlgButton(uintptr_t window, int32_t id, uint32_t state);
uint32_t NT_MS_ABI nt_IsDlgButtonChecked(uintptr_t window, int32_t id);
uint32_t NT_MS_ABI nt_GetDlgItemTextA(uintptr_t window, int32_t id, char *text, int32_t capacity);
intptr_t NT_MS_ABI nt_SendDlgItemMessageA(uintptr_t window, int32_t id, uint32_t message,
                                        uintptr_t wparam, intptr_t lparam);
void NT_MS_ABI nt_InitCommonControls(void);

uintptr_t nt_runtime_create_dialog(const char *caption, uintptr_t owner, int width, int height,
                                   nt_window_proc callback, intptr_t data);
int nt_runtime_run_modal(uintptr_t window, uintptr_t owner, const int *result);
uintptr_t nt_runtime_add_dialog_control(uintptr_t window, const char *type, const char *text,
                                       int id, uint32_t style, int x, int y, int width, int height);
int32_t NT_MS_ABI nt_MessageBoxA(uintptr_t owner, const char *text, const char *caption, uint32_t type);
int32_t NT_MS_ABI nt_WinHelpA(uintptr_t owner, const char *file, uint32_t command, uintptr_t data);
bool nt_runtime_set_help_document(const char *guest_name, const char *title, const char *text);
void nt_runtime_close_dialogs(void);

struct nt_open_file_name {
    uint32_t size;
    uintptr_t owner, instance;
    const char *filter;
    char *custom_filter;
    uint32_t max_custom_filter, filter_index;
    char *file;
    uint32_t max_file;
    char *file_title;
    uint32_t max_file_title;
    const char *initial_directory, *title;
    uint32_t flags;
    uint16_t file_offset, extension_offset;
    const char *default_extension;
    intptr_t custom_data;
    void *hook;
    const char *template_name;
    void *reserved;
    uint32_t reserved_count, extended_flags;
};

int32_t NT_MS_ABI nt_GetOpenFileNameA(struct nt_open_file_name *request);
int32_t NT_MS_ABI nt_GetSaveFileNameA(struct nt_open_file_name *request);
int32_t NT_MS_ABI nt_GetOpenFileNameW(struct nt_open_file_name *request);
int32_t NT_MS_ABI nt_GetSaveFileNameW(struct nt_open_file_name *request);
uint32_t NT_MS_ABI nt_CommDlgExtendedError(void);
bool nt_runtime_show_folder(const char *path, const char *selection, uintptr_t owner);
void nt_runtime_close_folder_views(void);

#endif
