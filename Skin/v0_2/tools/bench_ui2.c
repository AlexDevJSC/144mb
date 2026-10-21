#include <stdio.h>
#ifndef SKIN_BENCH_NO_UI
#include "skin/ui2.h"
static uint32_t pixels[320*200];
#endif
int main(void) {
    unsigned status=0;
#ifndef SKIN_BENCH_NO_UI
    Skin2Ui ui;
    Skin2View view={0};
    Skin2Canvas canvas;
    skin2_init(&ui);
    if(!skin2_canvas_init(&canvas,pixels,320,200,320))return 1;
    skin2_draw(&ui,&view,&canvas);
    status=pixels[100*320+160];
#endif
    (void)printf("Skin UI2 benchmark host: %u\n",status);
    return 0;
}
