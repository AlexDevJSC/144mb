#include "ear.h"
#include <stdint.h>
#include <stdio.h>
static EarEngine audio;
static int16_t buffer[1024];
int main(void) {
    ear_init(&audio, NULL);
    ear_trigger(&audio, EAR_SFX_STEP, 42u);
    ear_render(&audio, buffer, 1024);
    printf("%d\n", buffer[12]);
    return 0;
}
