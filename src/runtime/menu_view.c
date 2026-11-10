/* SPDX-License-Identifier: MIT */
#include "menu_view.h"

#include <cairo.h>
#include <stdlib.h>
#include <string.h>

uint32_t NT_MS_ABI nt_GetSysColor(int32_t index)
{
    switch (index) {
    case NT_COLOR_MENU: case 15: return 0xf0f0f0; /* menu and button face */
    case NT_COLOR_WINDOW: case NT_COLOR_HIGHLIGHTTEXT: case 20: return 0xffffff;
    case NT_COLOR_MENUTEXT: case 8: case 18: return 0x000000;
    case NT_COLOR_HIGHLIGHT: return 0xb65c20;
    case NT_COLOR_GRAYTEXT: case 16: return 0x707070;
    case 1: return 0x562f21; /* desktop background */
    default:
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
}

static int label_width(const char *source, char *output)
{
    int length = 0;
    while (*source != '\0' && *source != '\t') {
        if (*source == '&') {
            ++source;
            if (*source != '&')
                continue;
        }
        if (output != NULL)
            output[length] = *source;
        ++length;
        ++source;
    }
    if (output != NULL)
        output[length] = '\0';
    return length;
}

static int item_width(const struct nt_menu_view_item *item)
{
    return 18 + label_width(item->label, NULL) * 8;
}

bool nt_runtime_menu_bounds(uintptr_t menu, int popup_x, int width, int height,
                            uint32_t index, struct nt_rect *rect, struct nt_menu_view_item *item)
{
    if (!nt_runtime_menu_item(menu, index, item) || width <= 0 || height < NT_MENU_HEIGHT)
        return false;
    struct nt_menu_view_item entry;
    if (popup_x < 0) {
        int x = 0;
        for (uint32_t i = 0; i < index && nt_runtime_menu_item(menu, i, &entry); ++i)
            x += item_width(&entry);
        const int right = x + item_width(item);
        *rect = (struct nt_rect){x, 0, right < width ? right : width, NT_MENU_HEIGHT};
    } else {
        int popup_width = 48;
        for (uint32_t i = 0; nt_runtime_menu_item(menu, i, &entry); ++i) {
            const int row_width = 16 + item_width(&entry);
            if (row_width > popup_width)
                popup_width = row_width;
        }
        if (popup_width > width)
            popup_width = width;
        const int x = popup_x + popup_width > width ? width - popup_width : popup_x;
        const int y = NT_MENU_HEIGHT + (int)index * NT_MENU_ROW;
        const int bottom = y + NT_MENU_ROW;
        *rect = (struct nt_rect){x, y, x + popup_width, bottom < height ? bottom : height};
    }
    return rect->left < rect->right && rect->top < rect->bottom;
}

static void set_color(cairo_t *draw, uint32_t rgb)
{
    cairo_set_source_rgb(draw, (rgb & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb >> 16) / 255.0);
}

static bool paint_row(cairo_t *draw, struct nt_rect rect, const struct nt_menu_view_item *item,
                      bool popup, bool highlighted)
{
    cairo_save(draw);
    cairo_rectangle(draw, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    cairo_clip(draw);
    if (highlighted) {
        set_color(draw, nt_GetSysColor(NT_COLOR_HIGHLIGHT));
        cairo_paint(draw);
    }
    set_color(draw, nt_GetSysColor((item->flags & NT_MENU_DISABLED) != 0 ? NT_COLOR_GRAYTEXT :
                                  (highlighted ? NT_COLOR_HIGHLIGHTTEXT : NT_COLOR_MENUTEXT)));
    if ((item->flags & NT_MENU_SEPARATOR) != 0) {
        const int middle = (rect.top + rect.bottom) / 2;
        cairo_rectangle(draw, rect.left + 4, middle, rect.right - rect.left - 8, 1);
        cairo_fill(draw);
    } else {
        char label[sizeof(item->label)];
        const int length = label_width(item->label, label);
        char *utf8 = nt_runtime_ansi_to_utf8(label, (size_t)length);
        if (utf8 == NULL) {
            cairo_restore(draw);
            return false;
        }
        if (popup && (item->flags & NT_MENU_CHECKED) != 0) {
            cairo_move_to(draw, rect.left + 4, rect.top + 16);
            cairo_show_text(draw, "*");
        }
        cairo_move_to(draw, rect.left + (popup ? 18 : 8), rect.top + 16);
        cairo_show_text(draw, utf8);
        free(utf8);
    }
    cairo_restore(draw);
    return cairo_status(draw) == CAIRO_STATUS_SUCCESS;
}

static bool draw_part(SDL_Surface *surface, uintptr_t menu, uintptr_t active, int popup_x)
{
    const bool popup = popup_x >= 0;
    struct nt_menu_view_item item;
    struct nt_rect region = {0, 0, surface->w, NT_MENU_HEIGHT};
    if (popup) {
        if (!nt_runtime_menu_bounds(menu, popup_x, surface->w, surface->h, 0, &region, &item))
            return true;
        for (uint32_t i = 1; ; ++i) {
            struct nt_rect row;
            if (!nt_runtime_menu_bounds(menu, popup_x, surface->w, surface->h, i, &row, &item))
                break;
            region.bottom = row.bottom;
        }
    }
    const int width = region.right - region.left, height = region.bottom - region.top;
    if (width <= 0 || height <= 0 || region.bottom > surface->h)
        return false;
    cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    cairo_t *draw = cairo_create(image);
    set_color(draw, nt_GetSysColor(NT_COLOR_MENU));
    cairo_paint(draw);
    cairo_select_font_face(draw, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(draw, 13);
    cairo_translate(draw, -region.left, -region.top);
    bool drawn = true;
    for (uint32_t i = 0; drawn; ++i) {
        struct nt_rect row;
        if (!nt_runtime_menu_bounds(menu, popup_x, surface->w, surface->h, i, &row, &item))
            break;
        drawn = paint_row(draw, row, &item, popup, active != 0 && item.submenu == active);
    }
    if (drawn && cairo_status(draw) == CAIRO_STATUS_SUCCESS) {
        cairo_surface_flush(image);
        unsigned char *target = (unsigned char *)surface->pixels +
                                region.top * surface->pitch + region.left * surface->format->BytesPerPixel;
        drawn = SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_XRGB8888,
                                  cairo_image_surface_get_data(image), cairo_image_surface_get_stride(image),
                                  surface->format->format, target, surface->pitch) == 0;
    } else {
        drawn = false;
    }
    cairo_destroy(draw);
    cairo_surface_destroy(image);
    return drawn;
}

bool nt_runtime_draw_menu(SDL_Surface *surface, uintptr_t menu, uintptr_t popup, int popup_x)
{
    return draw_part(surface, menu, popup, -1) &&
           (popup == 0 || draw_part(surface, popup, 0, popup_x));
}
