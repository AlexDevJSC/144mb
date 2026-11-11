/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_RAW_INPUT_H
#define NTNATIVE_RAW_INPUT_H
#include "windesktop.h"

struct nt_raw_input_device { uint16_t page, usage; uint32_t flags; uintptr_t window; };
int32_t NT_MS_ABI nt_RegisterRawInputDevices(const struct nt_raw_input_device *devices,
                                            uint32_t count, uint32_t size);
uint32_t NT_MS_ABI nt_GetRawInputData(uintptr_t handle, uint32_t command, void *data, uint32_t *size,
                                     uint32_t header_size);
void nt_runtime_close_raw_input(void);
void nt_runtime_raw_window_closed(uintptr_t window);
bool nt_runtime_raw_mouse(uintptr_t window, int32_t dx, int32_t dy, uint16_t flags, uint16_t wheel);
bool nt_runtime_raw_key(uintptr_t window, uint16_t scan, uint16_t key, bool pressed);

struct nt_composition_form { uint32_t style; struct nt_point point; struct nt_rect area; };
struct nt_candidate_form { uint32_t index, style; struct nt_point point; struct nt_rect area; };
uintptr_t NT_MS_ABI nt_ImmGetContext(uintptr_t window);
int32_t NT_MS_ABI nt_ImmReleaseContext(uintptr_t window, uintptr_t context);
int32_t NT_MS_ABI nt_ImmSetCompositionWindow(uintptr_t context, const struct nt_composition_form *form);
int32_t NT_MS_ABI nt_ImmSetCandidateWindow(uintptr_t context, const struct nt_candidate_form *form);
#endif
