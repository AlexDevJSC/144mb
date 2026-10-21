/* Windows UI smoke test. Compiled against tests/mock_bones.c, NOT actual Bones.
   A real Bones+Eye host calls skin_ui_draw AFTER Eye has rendered its scene. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "skin/skin.h"
static BonesEngine game;
static Skin skin;
static SkinUi ui;
static uint32_t fb[320*200];
static uint8_t keys[256];
static int active=1;
static const char *savebase="skin_demo";
static const SkinWorldId world_id={UINT64_C(0x1440001),UINT64_C(0x5a7e)};
static void reset_game(void) {
    bones_init(&game,UINT64_C(0x1442026));
    (void)skin_init(&skin,&game,world_id,1);
    (void)skin_attach(&skin,&game);
}
static void run_action(SkinAction a) {
    switch(a) {
    case SKIN_ACTION_START: reset_game();skin_ui_notice(&ui,"NUEVA PARTIDA",false);break;
    case SKIN_ACTION_SAVE: {
        bool ok=skin_save(&skin,&game,savebase);
        skin_ui_notice(&ui,ok?"PARTIDA GUARDADA":"ERROR AL GUARDAR",!ok);
        break;
    }
    case SKIN_ACTION_LOAD:
        if(skin_load(&skin,&game,savebase)) {
            ui.screen=SKIN_MENU_GAME;
            skin_ui_notice(&ui,"PARTIDA CARGADA",false);
        } else skin_ui_notice(&ui,"SIN GUARDADO VALIDO",true);
        break;
    case SKIN_ACTION_QUIT:active=0;PostQuitMessage(0);break;
    default:break;
    }
}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l) {
    SkinAction a=SKIN_ACTION_NONE;
    switch(m){
    case WM_CLOSE:DestroyWindow(h);return 0;
    case WM_DESTROY:active=0;PostQuitMessage(0);return 0;
    case WM_KEYDOWN:case WM_SYSKEYDOWN:
        if(w<256){
            /* Auto-repeat is ignored for menu actions; movement uses held state. */
            if(!(l&(1L<<30))) {
                if(w==VK_UP)a=skin_ui_key(&ui,SKIN_KEY_UP);
                else if(w==VK_DOWN)a=skin_ui_key(&ui,SKIN_KEY_DOWN);
                else if(w==VK_RETURN)a=skin_ui_key(&ui,SKIN_KEY_ACCEPT);
                else if(w==VK_ESCAPE)a=skin_ui_key(&ui,SKIN_KEY_BACK);
            }
            keys[w]=1;
        }
        if(a!=SKIN_ACTION_NONE)run_action(a);
        if(m==WM_SYSKEYDOWN)return DefWindowProcA(h,m,w,l);
        return 0;
    case WM_KEYUP:case WM_SYSKEYUP:
        if(w<256)keys[w]=0;
        if(m==WM_SYSKEYUP)return DefWindowProcA(h,m,w,l);
        return 0;
    case WM_ACTIVATEAPP:
        if(!w)ZeroMemory(keys,sizeof(keys));
        return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT ps;BeginPaint(h,&ps);EndPaint(h,&ps);return 0;}
    }
    return DefWindowProcA(h,m,w,l);
}
static void show_frame(HWND hwnd) {
    RECT r;BITMAPINFO bi;HDC dc;int w,h,scale,dw,dh;
    skin_ui_placeholder_background(fb,320,200,(uint32_t)game.tick);
    skin_ui_draw(&ui,&game,fb,320,200);
    GetClientRect(hwnd,&r);w=r.right-r.left;h=r.bottom-r.top;
    if(w<=0||h<=0)return;
    scale=w/320<h/200?w/320:h/200;
    if(scale>=1){dw=320*scale;dh=200*scale;}
    else if((int64_t)w*200<=(int64_t)h*320){dw=w;dh=(int)((int64_t)w*200/320);}
    else {dh=h;dw=(int)((int64_t)h*320/200);}
    ZeroMemory(&bi,sizeof(bi));bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=320;bi.bmiHeader.biHeight=-200;
    bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    dc=GetDC(hwnd);if(!dc)return;
    PatBlt(dc,0,0,w,h,BLACKNESS);SetStretchBltMode(dc,COLORONCOLOR);
    StretchDIBits(dc,(w-dw)/2,(h-dh)/2,dw,dh,0,0,320,200,fb,&bi,DIB_RGB_COLORS,SRCCOPY);
    ReleaseDC(hwnd,dc);
}
int WINAPI WinMain(HINSTANCE inst,HINSTANCE prev,LPSTR args,int show) {
    WNDCLASSEXA c;HWND hwnd;MSG msg;LARGE_INTEGER freq,old,now;double lag=0;
    (void)prev;(void)args;
    ZeroMemory(&c,sizeof(c));c.cbSize=sizeof(c);c.hInstance=inst;
    c.lpfnWndProc=proc;c.lpszClassName="Skin144SmokeDemo";
    c.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClassExA(&c))return 1;
    hwnd=CreateWindowExA(0,c.lpszClassName,"Proyecto 1.44 - Skin UI / Guardado (demo independiente)",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,960,640,NULL,NULL,inst,NULL);
    if(!hwnd)return 2;
    reset_game();skin_ui_init(&ui);
    ShowWindow(hwnd,show);UpdateWindow(hwnd);
    if(!QueryPerformanceFrequency(&freq)||!QueryPerformanceCounter(&old))return 3;
    while(active){
        while(PeekMessageA(&msg,NULL,0,0,PM_REMOVE)){
          if(msg.message==WM_QUIT){active=0;break;}
          TranslateMessage(&msg);DispatchMessageA(&msg);
        }
        if(!active)break;
        QueryPerformanceCounter(&now);
        {double elapsed=(double)(now.QuadPart-old.QuadPart)/(double)freq.QuadPart;int steps=0;
          old=now;if(elapsed<0)elapsed=0;if(elapsed>.25)elapsed=.25;lag+=elapsed;
          while(lag>=1.0/60.0&&steps<5) {
            if(ui.screen==SKIN_MENU_GAME) {
               BonesInput in={0};
               in.forward=keys['W'];in.backward=keys['S'];
               in.left=keys['A'];in.right=keys['D'];in.jump=keys[VK_SPACE];
               (void)bones_step(&game,&in,BONES_TICK_DT);
            }
            lag-=1.0/60.0;steps++;
          }
          if(steps==5&&lag>=1.0/60.0)lag=0;
        }
        show_frame(hwnd);Sleep(1);
    }
    return 0;
}
