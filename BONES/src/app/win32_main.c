#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "../../include/bones/modkit_host.h"
#include "../adapters/eye_adapter.h"
#include "../demo/test_house.h"

static BonesFoundation foundation;
#define game (foundation.engine)
static BonesModHost *mod_host;
/* Eye owns the framebuffer, even when invoked through the ModKit ABI. */
extern const uint32_t *bones_eye_mod_pixels(void);
extern uint32_t bones_eye_mod_triangles(void);
static int held[256];
static int captured;
static void capture_mouse(HWND hwnd, int on) {
    if (on && !captured) {
        RECT rc; POINT c[2];
        GetClientRect(hwnd,&rc);
        c[0].x=rc.left; c[0].y=rc.top; c[1].x=rc.right; c[1].y=rc.bottom;
        ClientToScreen(hwnd,&c[0]); ClientToScreen(hwnd,&c[1]);
        rc.left=c[0].x; rc.top=c[0].y; rc.right=c[1].x; rc.bottom=c[1].y;
        if (rc.right<=rc.left || rc.bottom<=rc.top) return;
        ClipCursor(&rc); SetCapture(hwnd);
        SetCursorPos((rc.left+rc.right)/2,(rc.top+rc.bottom)/2);
        ShowCursor(FALSE); captured=1;
    } else if (!on && captured) {
        captured=0; ClipCursor(NULL); ReleaseCapture(); ShowCursor(TRUE);
    }
}
static LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    switch(msg) {
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: capture_mouse(hwnd,0); PostQuitMessage(0); return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if(w<256) held[w]=1;
        if(w==VK_ESCAPE) capture_mouse(hwnd,0);
        if(msg==WM_SYSKEYDOWN) return DefWindowProcA(hwnd,msg,w,l);
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        if(w<256) held[w]=0;
        if(msg==WM_SYSKEYUP) return DefWindowProcA(hwnd,msg,w,l);
        return 0;
    case WM_LBUTTONDOWN: SetFocus(hwnd); capture_mouse(hwnd,1); return 0;
    case WM_ENTERSIZEMOVE: capture_mouse(hwnd,0); return 0;
    case WM_ACTIVATEAPP:
        if(!w) { capture_mouse(hwnd,0); ZeroMemory(held,sizeof(held)); }
        return 0;
    case WM_CAPTURECHANGED:
        if(captured && (HWND)l != hwnd) {
            captured=0; ClipCursor(NULL); ShowCursor(TRUE);
        }
        return 0;
    case WM_PAINT: { PAINTSTRUCT p; BeginPaint(hwnd,&p); EndPaint(hwnd,&p); return 0; }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcA(hwnd,msg,w,l);
}
static void get_input(HWND hwnd,BonesInput *out) {
    ZeroMemory(out,sizeof(*out));
    out->forward=(uint8_t)held['W']; out->backward=(uint8_t)held['S'];
    out->left=(uint8_t)held['A']; out->right=(uint8_t)held['D'];
    out->sprint=(uint8_t)held[VK_SHIFT]; out->jump=(uint8_t)held[VK_SPACE];
    out->yaw_left=(uint8_t)held[VK_LEFT]; out->yaw_right=(uint8_t)held[VK_RIGHT];
    out->pitch_up=(uint8_t)held[VK_UP]; out->pitch_down=(uint8_t)held[VK_DOWN];
    if(captured) {
        RECT rc; POINT center,current;
        GetClientRect(hwnd,&rc);
        center.x=(rc.left+rc.right)/2; center.y=(rc.top+rc.bottom)/2;
        ClientToScreen(hwnd,&center); GetCursorPos(&current);
        out->mouse_dx=(float)(current.x-center.x);
        out->mouse_dy=(float)(current.y-center.y);
        SetCursorPos(center.x,center.y);
    }
}
static void present(HWND hwnd) {
    RECT r; BITMAPINFO bi; HDC dc;
    const uint32_t *pixels=bones_eye_mod_pixels();
    int w,h,s,dw,dh;
    if(!pixels) return;
    GetClientRect(hwnd,&r);
    w=r.right-r.left; h=r.bottom-r.top;
    if(w<=0||h<=0) return;
    dw=P144_VIEW_WIDTH; dh=P144_VIEW_HEIGHT;
    s=w/dw<h/dh?w/dw:h/dh;
    if(s>=1) {dw*=s; dh*=s;}
    else if((int64_t)w*P144_VIEW_HEIGHT <= (int64_t)h*P144_VIEW_WIDTH)
        {dw=w; dh=(int)((int64_t)w*P144_VIEW_HEIGHT/P144_VIEW_WIDTH);}
    else {dh=h; dw=(int)((int64_t)h*P144_VIEW_WIDTH/P144_VIEW_HEIGHT);}
    ZeroMemory(&bi,sizeof(bi));
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=P144_VIEW_WIDTH;
    bi.bmiHeader.biHeight=-P144_VIEW_HEIGHT;
    bi.bmiHeader.biPlanes=1;
    bi.bmiHeader.biBitCount=32;
    bi.bmiHeader.biCompression=BI_RGB;
    dc=GetDC(hwnd); if(!dc) return;
    PatBlt(dc,0,0,w,h,BLACKNESS);
    SetStretchBltMode(dc,COLORONCOLOR);
    StretchDIBits(dc,(w-dw)/2,(h-dh)/2,dw,dh,0,0,P144_VIEW_WIDTH,P144_VIEW_HEIGHT,
        pixels,&bi,DIB_RGB_COLORS,SRCCOPY);
    ReleaseDC(hwnd,dc);
}
int WINAPI WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command,int show) {
    WNDCLASSEXA wc; HWND hwnd; MSG msg;
    LARGE_INTEGER frequency,previous_time,now;
    double accumulator=0.0,report_clock=0.0;
    int running=1, frames=0;
    (void)previous; (void)command;
    ZeroMemory(&wc,sizeof(wc)); wc.cbSize=sizeof(wc); wc.hInstance=inst;
    wc.lpfnWndProc=proc; wc.lpszClassName="BonesP144Window";
    wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClassExA(&wc)) return 1;
    hwnd=CreateWindowExA(0,wc.lpszClassName,
        "Proyecto 1.44 - Bones + Eye | WASD Space Shift, click para raton",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,960,640,
        NULL,NULL,inst,NULL);
    if(!hwnd) return 2;
    if(!bones_foundation_init(&foundation,UINT64_C(0x1440001),UINT64_C(0x01442026),1)) return 6;
    {
        BonesModule m=bones_test_house_module();
        if(!bones_register_module(&game,&m)) return 5;
    }
    mod_host=bones_mod_host_create(&foundation);
    if(!mod_host || bones_mod_register_builtins(mod_host)!=BONES_MOD_OK ||
       bones_mod_host_start(mod_host)!=BONES_MOD_OK)return 4;
    ShowWindow(hwnd,show); UpdateWindow(hwnd);
    if(!QueryPerformanceFrequency(&frequency)||!QueryPerformanceCounter(&previous_time)) return 3;
    while(running) {
        while(PeekMessageA(&msg,NULL,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT) {running=0; break;}
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        if(!running) break;
        QueryPerformanceCounter(&now);
        {
            double elapsed=(double)(now.QuadPart-previous_time.QuadPart)/(double)frequency.QuadPart;
            int steps=0;
            previous_time=now;
            if(elapsed<0.0) elapsed=0.0;
            if(elapsed>.25) elapsed=.25;
            accumulator+=elapsed; report_clock+=elapsed;
            while(accumulator >= (double)1.0/60.0 && steps<5) {
                BonesInput input;
                get_input(hwnd,&input);
                if(bones_mod_host_step(mod_host,&input)!=BONES_MOD_OK){running=0;break;}
                accumulator-=1.0/60.0;
                steps++;
            }
            if(steps==5 && accumulator>=1.0/60.0) accumulator=0.0;
        }
        if(bones_mod_host_frame(mod_host)!=BONES_MOD_OK)break;
        present(hwnd);
        frames++;
        if(report_clock>=1.0) {
            char title[180];
            (void)snprintf(title,sizeof(title),
                "PROYECTO 1.44 | Bones + Eye | FPS %d | Tri %u | Tick %llu",
                frames,(unsigned)bones_eye_mod_triangles(),(unsigned long long)game.tick);
            SetWindowTextA(hwnd,title); frames=0; report_clock=0.0;
        }
        Sleep(1);
    }
    capture_mouse(hwnd,0);
    bones_mod_host_destroy(mod_host);
    return 0;
}
