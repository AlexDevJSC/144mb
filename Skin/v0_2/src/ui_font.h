#ifndef SKIN_FONT_H
#define SKIN_FONT_H
#include <stdint.h>
/* Shared bitmap glyphs for legacy + UI2. Unmapped characters fall back to ?. */
const uint8_t *skin_font_rows(char c);
#endif
