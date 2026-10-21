#include "skin/ui2.h"
#include <stdio.h>
int main(void){
    printf("Skin2Ui=%zu\nSkin2View=%zu\nSkin2Canvas=%zu\nSkin2Theme=%zu\nSkin2Item=%zu\n",
        sizeof(Skin2Ui),sizeof(Skin2View),sizeof(Skin2Canvas),sizeof(Skin2Theme),sizeof(Skin2Item));
    return 0;
}
