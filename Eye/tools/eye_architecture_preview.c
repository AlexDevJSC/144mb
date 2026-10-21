/* Offline preview only, not a replacement for Bones rendering or physics.
 * Draws a stylized building with shared EYE3 materials on triangles/quads. */
#include "eye3.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 760
#define H 510
#define FP 65536

typedef struct { int x,y; int32_t u,v; } V;
static uint32_t frame[W*H];
static uint32_t tex[7][EYE_SIZE*EYE_SIZE];
static uint32_t tint(uint32_t c,unsigned lum) {
    unsigned r=((c>>16)&255u)*lum/256u;
    unsigned g=((c>>8)&255u)*lum/256u;
    unsigned b=(c&255u)*lum/256u;
    return UINT32_C(0xff000000)|(r<<16)|(g<<8)|b;
}
static int64_t cross(V a,V b,int px,int py) {
    return (int64_t)(px-a.x)*(int64_t)(b.y-a.y)-
           (int64_t)(py-a.y)*(int64_t)(b.x-a.x);
}
static void tri(V a,V b,V c,const uint32_t *texture,unsigned lum) {
    int xmin=a.x, xmax=a.x, ymin=a.y, ymax=a.y;
    int x,y;
    int64_t area=cross(a,b,c.x,c.y);
    if(area==0) return;
    if(area<0) { V tmp=b;b=c;c=tmp;area=-area; }
    if(b.x<xmin) xmin=b.x;
    if(c.x<xmin) xmin=c.x;
    if(b.x>xmax) xmax=b.x;
    if(c.x>xmax) xmax=c.x;
    if(b.y<ymin) ymin=b.y;
    if(c.y<ymin) ymin=c.y;
    if(b.y>ymax) ymax=b.y;
    if(c.y>ymax) ymax=c.y;
    if(xmin<0) xmin=0;
    if(ymin<0) ymin=0;
    if(xmax>=W) xmax=W-1;
    if(ymax>=H) ymax=H-1;
    for(y=ymin;y<=ymax;++y) for(x=xmin;x<=xmax;++x) {
        int64_t w0=cross(b,c,x,y),w1=cross(c,a,x,y),w2=cross(a,b,x,y);
        if(w0<0||w1<0||w2<0) continue;
        int64_t u=(w0*a.u+w1*b.u+w2*c.u)/area;
        int64_t v=(w0*a.v+w1*b.v+w2*c.v)/area;
        uint32_t color=eye_uv_sample_repeat(texture,(int32_t)u,(int32_t)v);
        frame[(size_t)y*W+(size_t)x]=tint(color,lum);
    }
}
static V vertex(int x,int y,int u,int v) {
    V p={x,y,u*FP,v*FP};return p;
}
static void quad(V a,V b,V c,V d,const uint32_t *t,unsigned shade) {
    tri(a,b,c,t,shade);tri(a,c,d,t,shade);
}
static void rect(int x0,int y0,int x1,int y1,uint32_t color) {
    int x,y;
    if(x0<0)x0=0;
    if(x1>W)x1=W;
    if(y0<0)y0=0;
    if(y1>H)y1=H;
    for(y=y0;y<y1;++y) for(x=x0;x<x1;++x) frame[(size_t)y*W+(size_t)x]=color;
}
static void line(int x0,int y0,int x1,int y1,uint32_t color) {
    int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1;
    int err=dx+dy;
    for(;;) {
        if(x0>=0&&x0<W&&y0>=0&&y0<H)frame[(size_t)y0*W+(size_t)x0]=color;
        if(x0==x1&&y0==y1)break;
        int e=2*err;
        if(e>=dy){err+=dy;x0+=sx;}
        if(e<=dx){err+=dx;y0+=sy;}
    }
}
static int writeppm(const char *name) {
    FILE *f=fopen(name,"wb");unsigned char rgb[3];size_t i;
    if(!f)return 0;
    fprintf(f,"P6\n%d %d\n255\n",W,H);
    for(i=0;i<W*H;++i) {
        rgb[0]=(unsigned char)(frame[i]>>16);
        rgb[1]=(unsigned char)(frame[i]>>8);
        rgb[2]=(unsigned char)frame[i];
        if(fwrite(rgb,1,3,f)!=3u){fclose(f);return 0;}
    }
    return fclose(f)==0;
}
int main(int argc,char **argv) {
    static uint8_t bytes[EYE_PACK_MAX_BYTES];
    static EyeTextureBank bank;
    size_t length;
    FILE *f;
    unsigned i;
    const unsigned which[7]={0,3,5,8,11,13,15}; /* stone, wood, roof, path, trim, iron, grass */
    if(argc!=3) {fputs("uso: eye_architecture_preview PACK out.ppm\n",stderr);return 2;}
    f=fopen(argv[1],"rb");
    if(!f){perror(argv[1]);return 3;}
    length=fread(bytes,1,sizeof bytes,f);
    if(fgetc(f)!=EOF){fclose(f);return 4;}
    fclose(f);
    if(!eye_bank_open(&bank,bytes,length)||bank.version!=3u)return 5;
    for(i=0;i<7u;++i) if(!eye_bank_render(&bank,which[i],tex[i],4096u))return 6;
    /* Sky and floor intentionally muted, with periodical foliage. */
    for(int y=0;y<H;++y) for(int x=0;x<W;++x) {
        if(y<335) {
            int sky=80+(y*33/335);
            frame[(size_t)y*W+(size_t)x]=UINT32_C(0xff000000)|
                (uint32_t)(sky<<16)|(uint32_t)((sky+4)<<8)|(uint32_t)(sky+7);
        } else {
            frame[(size_t)y*W+(size_t)x]=tint(eye_uv_sample_repeat(tex[6],x*205,y*194),205u);
        }
    }
    /* Path behind house; cobble material shared with all paved floors. */
    quad(vertex(290,399,0,0),vertex(414,401,2,0),vertex(570,509,4,5),vertex(52,509,0,5),tex[3],212);
    /* Exterior walls: stone is shared by both faces and the triangular gable. */
    quad(vertex(155,244,0,0),vertex(427,234,5,0),vertex(427,425,5,3),vertex(155,425,0,3),tex[0],238);
    quad(vertex(427,234,0,0),vertex(665,307,4,0),vertex(665,444,4,3),vertex(427,425,0,3),tex[0],190);
    tri(vertex(155,244,0,3),vertex(299,112,3,0),vertex(427,234,6,3),tex[0],235);
    /* Roof: two slopes, shared slate texture, no triangular image assets. */
    quad(vertex(299,112,0,0),vertex(536,184,4,0),vertex(407,318,4,3),vertex(129,257,0,3),tex[2],206);
    quad(vertex(299,112,0,0),vertex(536,184,4,0),vertex(683,311,4,3),vertex(427,234,0,3),tex[2],178);
    /* Corners/gutters: simple lines are geometry, not stored textures. */
    line(299,112,536,184,UINT32_C(0xff77776e));
    line(427,234,683,311,UINT32_C(0xff3a403f));
    line(129,257,407,318,UINT32_C(0xff363936));
    /* Door: reuse wood; trim uses the carving family. */
    quad(vertex(271,311,0,0),vertex(337,311,2,0),vertex(337,425,2,4),vertex(271,425,0,4),tex[1],180);
    quad(vertex(265,307,0,0),vertex(343,307,3,0),vertex(343,314,3,1),vertex(265,314,0,1),tex[4],205);
    quad(vertex(265,307,0,0),vertex(272,307,1,0),vertex(272,425,1,4),vertex(265,425,0,4),tex[4],198);
    quad(vertex(337,307,0,0),vertex(344,307,1,0),vertex(344,425,1,4),vertex(337,425,0,4),tex[4],198);
    rect(324,368,331,374,UINT32_C(0xff8f8065));
    /* Window surfaces: metal frame, wood shutters, opaque dark glass. */
    quad(vertex(187,296,0,0),vertex(242,296,2,0),vertex(242,348,2,2),vertex(187,348,0,2),tex[1],170);
    rect(199,303,230,339,UINT32_C(0xff30383b));
    line(215,303,215,339,UINT32_C(0xff69665b));
    line(199,320,230,320,UINT32_C(0xff69665b));
    quad(vertex(509,329,0,0),vertex(581,348,2,0),vertex(581,396,2,2),vertex(509,381,0,2),tex[1],168);
    quad(vertex(520,341,0,0),vertex(571,354,2,0),vertex(571,384,2,2),vertex(520,371,0,2),tex[5],115);
    line(545,347,545,377,UINT32_C(0xff908a7c));
    line(520,357,571,371,UINT32_C(0xff908a7c));
    /* A small stone foundation, using the same wall recipe. */
    quad(vertex(149,421,0,0),vertex(431,420,5,0),vertex(431,439,5,1),vertex(149,439,0,1),tex[0],171);
    quad(vertex(431,420,0,0),vertex(666,442,4,0),vertex(666,455,4,1),vertex(431,439,0,1),tex[0],154);
    /* Restore bottom of door over foundation. */
    quad(vertex(271,416,0,0),vertex(337,416,2,0),vertex(337,438,2,1),vertex(271,438,0,1),tex[1],170);
    if(!writeppm(argv[2]))return 7;
    printf("Preview real: 1 house, 7 shared materials (of 16 recipes), triangle/quads, no new shape texture\n");
    return 0;
}
