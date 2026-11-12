/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_TASK_DIALOG_H
#define NTNATIVE_TASK_DIALOG_H
#include "compat.h"

typedef int32_t (NT_MS_ABI *nt_task_callback)(uintptr_t, uint32_t, uintptr_t, intptr_t, intptr_t);
struct __attribute__((packed)) nt_task_button { int32_t id; const uint16_t *label; };
struct __attribute__((packed)) nt_task_config {
    uint32_t size;
    uintptr_t parent, instance;
    uint32_t flags, common_buttons;
    const uint16_t *title;
    uintptr_t icon;
    const uint16_t *instruction, *content;
    uint32_t button_count;
    const struct nt_task_button *buttons;
    int32_t default_button;
    uint32_t radio_count;
    const struct nt_task_button *radios;
    int32_t default_radio;
    const uint16_t *verification, *expanded, *expand_label, *collapse_label;
    uintptr_t footer_icon;
    const uint16_t *footer;
    nt_task_callback callback;
    intptr_t context;
    uint32_t width;
};

int32_t NT_MS_ABI nt_TaskDialogIndirect(const struct nt_task_config *config, int32_t *button,
                                        int32_t *radio, int32_t *checked);
#endif
