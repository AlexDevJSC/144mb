/* Independent UX smoke demo. No Eye or Bones code. Compositor target owned by app. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "skin/ui2.h"

static Skin2Ui ui;
static Skin2View view;
static uint32_t frame[SKIN_UI2_W*SKIN_UI2_H];
static int running=1;
static void background(void){
    int x,y;
    for(y=0;y<SKIN_UI2_H;y++)for(x=0;x<SKIN_UI2_W;x++){
        int line=(y%16==0||((x+(y/16%2)*11)%32)==0);
        unsigned v= (unsigned)(line?45:83+((x*17+y*7)%8));
        frame[y*SKIN_UI2_W+x]=0xff000000u|(v<<16)|((v-5u)<<8)|(v-12u);
    }
}
static LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    Skin2Action action=SKIN2_NO_ACTION;
    switch(msg){
    case WM_CLOSE:DestroyWindow(hwnd);return 0;
    case WM_DESTROY:running=0;PostQuitMessage(0);return 0;
    case WM_KEYDOWN:case WM_SYSKEYDOWN:
        if(!(lp&(1L<<30))){
            if(wp==VK_UP) action=skin2_input(&ui,SKIN2_UP);
            if(wp==VK_DOWN) action=skin2_input(&ui,SKIN2_DOWN);
            if(wp==VK_LEFT) action=skin2_input(&ui,SKIN2_LEFT);
            if(wp==VK_RIGHT) action=skin2_input(&ui,SKIN2_RIGHT);
            if(wp==VK_RETURN) action=skin2_input(&ui,SKIN2_ACCEPT);
            if(wp==VK_ESCAPE) action=skin2_input(&ui,SKIN2_BACK);
            if(wp=='I') action=skin2_input(&ui,SKIN2_OPEN_INV);
            if(action==SKIN2_START)skin2_notice(&ui,"ENTORNO DE PRUEBA");
            if(action==SKIN2_LOAD||action==SKIN2_SAVE)skin2_notice(&ui,"ACCION DEMO: NO EJECUTADA");
            if(action==SKIN2_MAIN_REQUEST){ui.screen=SKIN2_MAIN;ui.selection=0;}
            if(action==SKIN2_QUIT){DestroyWindow(hwnd);return 0;}
        }
        InvalidateRect(hwnd,NULL,FALSE);
        if(msg==WM_SYSKEYDOWN)return DefWindowProcA(hwnd,msg,wp,lp);
        return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps;RECT r;BITMAPINFO bmi;Skin2Canvas c;
        int w,h,dw,dh,scale;
        HDC dc=BeginPaint(hwnd,&ps);
        background();
        if(skin2_canvas_init(&c,frame,SKIN_UI2_W,SKIN_UI2_H,SKIN_UI2_W))skin2_draw(&ui,&view,&c);
        GetClientRect(hwnd,&r);w=r.right-r.left;h=r.bottom-r.top;
        if(w>0&&h>0){
            scale=w/SKIN_UI2_W<h/SKIN_UI2_H?w/SKIN_UI2_W:h/SKIN_UI2_H;
            if(scale>=1){dw=SKIN_UI2_W*scale;dh=SKIN_UI2_H*scale;}
            else if((int64_t)w*SKIN_UI2_H<=(int64_t)h*SKIN_UI2_W){dw=w;dh=(int)((int64_t)w*SKIN_UI2_H/SKIN_UI2_W);}
            else{dh=h;dw=(int)((int64_t)h*SKIN_UI2_W/SKIN_UI2_H);}
            ZeroMemory(&bmi,sizeof(bmi));
            bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth=SKIN_UI2_W;
            bmi.bmiHeader.biHeight=-SKIN_UI2_H;
            bmi.bmiHeader.biPlanes=1;
            bmi.bmiHeader.biBitCount=32;
            bmi.bmiHeader.biCompression=BI_RGB;
            PatBlt(dc,0,0,w,h,BLACKNESS);SetStretchBltMode(dc,COLORONCOLOR);
            StretchDIBits(dc,(w-dw)/2,(h-dh)/2,dw,dh,0,0,SKIN_UI2_W,SKIN_UI2_H,
                frame,&bmi,DIB_RGB_COLORS,SRCCOPY);
        }
        EndPaint(hwnd,&ps);
        return 0;
    }
    }
    return DefWindowProcA(hwnd,msg,wp,lp);
}
int WINAPI WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR args,int show){
    WNDCLASSEXA wc;HWND hwnd;MSG message;int i;
    (void)previous;(void)args;
    skin2_init(&ui);view.hp=80;view.hp_max=100;view.mp=30;view.mp_max=50;view.gold=94;view.level=2;
    view.item_count=3;
    for(i=0;i<3;i++){view.items[i].count=(uint16_t)(i+1);view.items[i].icon=(uint16_t)i;}
    (void)snprintf(view.items[0].name,SKIN_UI2_ITEM_NAME,"ESPADA");
    (void)snprintf(view.items[1].name,SKIN_UI2_ITEM_NAME,"POCION");
    (void)snprintf(view.items[2].name,SKIN_UI2_ITEM_NAME,"LLAVE");
    ZeroMemory(&wc,sizeof(wc));wc.cbSize=sizeof(wc);wc.hInstance=inst;
    wc.lpfnWndProc=proc;wc.lpszClassName="Skin144Ui2Demo";
    wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClassExA(&wc))return 1;
    hwnd=CreateWindowExA(0,wc.lpszClassName,"SKIN UI2 - test de menus e inventario",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,960,640,NULL,NULL,inst,NULL);
    if(!hwnd)return 2;
    ShowWindow(hwnd,show);UpdateWindow(hwnd);
    while(running&&GetMessageA(&message,NULL,0,0)>0){TranslateMessage(&message);DispatchMessageA(&message);}
    return 0;
}
