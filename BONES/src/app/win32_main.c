#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
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
static int window_active=1;
/* Relative mouse movement, accumulated from WM_INPUT between fixed ticks. */
static LONG mouse_relative_x,mouse_relative_y;
/* Complete off-screen frame is composed before a single window BitBlt. */
static HDC back_dc;
static HBITMAP back_bitmap,back_previous;
static int back_width,back_height,back_ready;
static FILE *trace_file;
static uint32_t trace_rows;
static FILE *frame_trace;
static uint32_t frame_trace_rows;
/* Diagnostic only: sample Eye's unmodified framebuffer before GDI. */
static void trace_framebuffer(const uint32_t *pixels) {
    uint32_t i,dark=0;
    uint64_t hash=UINT64_C(1469598103934665603);
    if(!frame_trace || frame_trace_rows>=600 || !pixels) return;
    for(i=0;i<P144_VIEW_WIDTH*P144_VIEW_HEIGHT;i++) {
        uint32_t px=pixels[i];
        hash^=(uint64_t)px;hash*=UINT64_C(1099511628211);
        if((px&0x00ffffffu)==0u)dark++;
    }
    fprintf(frame_trace,"%u,%016llx,%u\n",frame_trace_rows,
            (unsigned long long)hash,(unsigned)dark);
    frame_trace_rows++;
    if(frame_trace_rows%60==0)fflush(frame_trace);
}
static void reset_mouse_deltas(void) { mouse_relative_x=0; mouse_relative_y=0; }
static LONG bounded_sum(LONG a,LONG b) {
    const LONG limit=10000;
    if(b>limit)b=limit;
    if(b < -limit)b=-limit;
    if(b>0 && a>limit-b)return limit;
    if(b<0 && a< -limit-b)return -limit;
    return a+b;
}
static void release_backbuffer(void) {
    if(back_dc) {
        if(back_previous) SelectObject(back_dc,back_previous);
        if(back_bitmap) DeleteObject(back_bitmap);
        DeleteDC(back_dc);
    }
    back_dc=NULL; back_bitmap=NULL; back_previous=NULL;
    back_width=back_height=back_ready=0;
}
static int create_backbuffer(HDC window_dc,int w,int h) {
    BITMAPINFO bi;
    void *data=NULL;
    if(back_dc && w==back_width && h==back_height) return 1;
    release_backbuffer();
    if(w<=0 || h<=0 || w>8192 || h>8192) return 0;
    ZeroMemory(&bi,sizeof(bi));
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=-h;
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32;
    bi.bmiHeader.biCompression=BI_RGB;
    back_dc=CreateCompatibleDC(window_dc);
    if(!back_dc) return 0;
    back_bitmap=CreateDIBSection(window_dc,&bi,DIB_RGB_COLORS,&data,NULL,0);
    if(!back_bitmap || !data) {release_backbuffer();return 0;}
    back_previous=(HBITMAP)SelectObject(back_dc,back_bitmap);
    back_width=w;back_height=h;
    return 1;
}
static void copy_backbuffer(HDC dc,const RECT *r) {
    if(back_ready && back_dc)
        BitBlt(dc,0,0,r->right-r->left,r->bottom-r->top,back_dc,0,0,SRCCOPY);
    else FillRect(dc,r,(HBRUSH)GetStockObject(BLACK_BRUSH));
}
static void record_input(const BonesInput *input,const BonesPlayer *b,
                         const BonesPlayer *a,uint64_t tick) {
    if(!trace_file || trace_rows>=600) return;
    fprintf(trace_file,
        "%llu,%d,%d,%ld,%ld,%.2f,%.2f,%u,%u,%u,%u,%u,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
        (unsigned long long)tick,captured,window_active,
        (long)input->mouse_dx,(long)input->mouse_dy,
        input->mouse_dx,input->mouse_dy,
        (unsigned)input->forward,(unsigned)input->backward,
        (unsigned)input->left,(unsigned)input->right,(unsigned)input->jump,
        b->feet.x,b->feet.y,b->feet.z,b->velocity.x,b->velocity.y,b->velocity.z,b->yaw,b->pitch,
        a->feet.x,a->feet.y,a->feet.z,a->velocity.x,a->velocity.y,a->velocity.z,a->yaw,a->pitch);
    trace_rows++;
    if(trace_rows%60==0)fflush(trace_file);
}
static void capture_mouse(HWND hwnd, int on) {
    if (on && !captured && window_active) {
        RECT rc; POINT c[2];
        GetClientRect(hwnd,&rc);
        c[0].x=rc.left; c[0].y=rc.top; c[1].x=rc.right; c[1].y=rc.bottom;
        ClientToScreen(hwnd,&c[0]); ClientToScreen(hwnd,&c[1]);
        rc.left=c[0].x; rc.top=c[0].y; rc.right=c[1].x; rc.bottom=c[1].y;
        if (rc.right<=rc.left || rc.bottom<=rc.top) return;
        reset_mouse_deltas();
        ClipCursor(&rc);SetCapture(hwnd);
        ShowCursor(FALSE); captured=1;
    } else if (!on && captured) {
        captured=0; reset_mouse_deltas(); ClipCursor(NULL);
        if(GetCapture()==hwnd)ReleaseCapture();
        ShowCursor(TRUE);
    }
}
static LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    switch(msg) {
    case WM_INPUT: {
        RAWINPUT raw;UINT n=sizeof(raw);
        if(GetRawInputData((HRAWINPUT)l,RID_INPUT,&raw,&n,
                           sizeof(RAWINPUTHEADER))==n && n<=sizeof(raw) &&
           raw.header.dwType==RIM_TYPEMOUSE &&
           !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) &&
           captured && window_active) {
            mouse_relative_x=bounded_sum(mouse_relative_x,raw.data.mouse.lLastX);
            mouse_relative_y=bounded_sum(mouse_relative_y,raw.data.mouse.lLastY);
        }
        /* DefWindowProc releases foreground raw input resources. */
        return DefWindowProcA(hwnd,msg,w,l);
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: capture_mouse(hwnd,0); release_backbuffer(); PostQuitMessage(0); return 0;
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
        window_active=w!=0;
        if(!window_active) { capture_mouse(hwnd,0); ZeroMemory(held,sizeof(held)); }
        reset_mouse_deltas();return 0;
    case WM_KILLFOCUS:
        window_active=0;capture_mouse(hwnd,0);
        ZeroMemory(held,sizeof(held));reset_mouse_deltas();return 0;
    case WM_SETFOCUS: window_active=1;reset_mouse_deltas();return 0;
    case WM_CAPTURECHANGED:
        if(captured && (HWND)l != hwnd) {
            captured=0; reset_mouse_deltas();ClipCursor(NULL); ShowCursor(TRUE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT p;RECT r;HDC dc=BeginPaint(hwnd,&p);
        GetClientRect(hwnd,&r);copy_backbuffer(dc,&r);
        EndPaint(hwnd,&p); return 0;
    }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcA(hwnd,msg,w,l);
}
static void get_input(BonesInput *out) {
    ZeroMemory(out,sizeof(*out));
    out->forward=(uint8_t)held['W']; out->backward=(uint8_t)held['S'];
    out->left=(uint8_t)held['A']; out->right=(uint8_t)held['D'];
    out->sprint=(uint8_t)held[VK_SHIFT]; out->jump=(uint8_t)held[VK_SPACE];
    out->yaw_left=(uint8_t)held[VK_LEFT]; out->yaw_right=(uint8_t)held[VK_RIGHT];
    out->pitch_up=(uint8_t)held[VK_UP]; out->pitch_down=(uint8_t)held[VK_DOWN];
    if(captured && window_active) {
        out->mouse_dx=(float)mouse_relative_x;
        out->mouse_dy=(float)mouse_relative_y;
    }
    reset_mouse_deltas();
}
static void present(HWND hwnd) {
    RECT r; BITMAPINFO bi; HDC dc;
    const uint32_t *pixels=bones_eye_mod_pixels();
    int w,h,s,dw,dh;
    if(!pixels) return;
    trace_framebuffer(pixels);
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
    dc=GetDC(hwnd);if(!dc)return;
    if(create_backbuffer(dc,w,h)) {
        /* BLACKNESS is applied ONLY OFFSCREEN; the visible window receives
           one complete BitBlt after composition. Keep Eye's Z-buffer intact. */
        PatBlt(back_dc,0,0,w,h,BLACKNESS);
        SetStretchBltMode(back_dc,COLORONCOLOR);
        if(StretchDIBits(back_dc,(w-dw)/2,(h-dh)/2,dw,dh,
               0,0,P144_VIEW_WIDTH,P144_VIEW_HEIGHT,
               pixels,&bi,DIB_RGB_COLORS,SRCCOPY)!=GDI_ERROR) {
            back_ready=1;
            copy_backbuffer(dc,&r);
        }
    }
    ReleaseDC(hwnd,dc);
}
int WINAPI WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command,int show) {
    WNDCLASSEXA wc; HWND hwnd; MSG msg;
    LARGE_INTEGER frequency,previous_time,now;
    double accumulator=0.0,report_clock=0.0;
    int running=1, frames=0;
    (void)previous; (void)command;
    /* Diagnostic CSV is opt-in and capped at 600 simulation ticks. */
    {
        char enabled[4]={0};
        if(GetEnvironmentVariableA("BONES_INPUT_TRACE",enabled,sizeof(enabled))>0 &&
           enabled[0]=='1') {
            trace_file=fopen("BONES_playtest_input.csv","w");
            if(trace_file) fputs("tick,captured,active,raw_dx,raw_dy,mouse_dx,mouse_dy,"
                "W,S,A,D,space,before_x,before_y,before_z,before_vx,before_vy,before_vz,"
                "before_yaw,before_pitch,after_x,after_y,after_z,after_vx,after_vy,after_vz,"
                "after_yaw,after_pitch\n",trace_file);
        }
        if(GetEnvironmentVariableA("BONES_FRAME_TRACE",enabled,sizeof(enabled))>0 &&
           enabled[0]=='1') {
            frame_trace=fopen("BONES_playtest_frames.csv","w");
            if(frame_trace) fputs("frame,fnv64_pixels,black_pixels\n",frame_trace);
        }
    }
    ZeroMemory(&wc,sizeof(wc)); wc.cbSize=sizeof(wc); wc.hInstance=inst;
    wc.lpfnWndProc=proc; wc.lpszClassName="BonesP144Window";
    wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClassExA(&wc)) return 1;
    hwnd=CreateWindowExA(0,wc.lpszClassName,
        "Proyecto 1.44 - Bones + Eye | WASD Space Shift, click para raton",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,960,640,
        NULL,NULL,inst,NULL);
    if(!hwnd) return 2;
    { RAWINPUTDEVICE mouse={0};
      mouse.usUsagePage=0x01; mouse.usUsage=0x02;
      mouse.dwFlags=0;mouse.hwndTarget=hwnd;
      if(!RegisterRawInputDevices(&mouse,1,sizeof(mouse))) {
          MessageBoxA(hwnd,"No se pudo registrar Raw Input para el raton.",
                      "Bones: entrada",MB_OK|MB_ICONERROR);
          DestroyWindow(hwnd);return 7;
      }
    }
    if(!bones_foundation_init(&foundation,UINT64_C(0x1440001),UINT64_C(0x01442026),1)) return 6;
    if(!bones_house_register_colliders(&game.world)) return 8;
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
                BonesPlayer before=game.player;
                get_input(&input);
                if(bones_mod_host_step(mod_host,&input)!=BONES_MOD_OK){running=0;break;}
                record_input(&input,&before,&game.player,game.tick);
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
    release_backbuffer();
    if(trace_file)fclose(trace_file);
    if(frame_trace)fclose(frame_trace);
    return 0;
}
