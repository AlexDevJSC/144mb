/* Developer-only deterministic architectural preview. NOT Bones world/physics. */
#include "eye4.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W 620
#define H 430
#define Q 65536
static uint32_t img[W*H];
static uint32_t tiles[8][4096];
typedef struct {int x,y; int32_t u,v;} P;
static uint32_t dim(uint32_t col,int d) {
    int r=(int)((col>>16)&255u)+d,g=(int)((col>>8)&255u)+d,b=(int)(col&255u)+d;
    if(r<0)r=0;
    if(g<0)g=0;
    if(b<0)b=0;
    if(r>255)r=255;
    if(g>255)g=255;
    if(b>255)b=255;
    return 0xff000000u|((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
}
static int64_t cross(P a,P b,int x,int y) {
    return (int64_t)(x-a.x)*(b.y-a.y)-(int64_t)(y-a.y)*(b.x-a.x);
}
static P pt(int x,int y,int u,int v) {P p={x,y,u*Q,v*Q};return p;}
static void tri(P a,P b,P c,const uint32_t *tex,int shade) {
    int x0=a.x,x1=a.x,y0=a.y,y1=a.y;int64_t area=cross(a,b,c.x,c.y);
    if(!area)return;
    if(area<0){P t=b;b=c;c=t;area=-area;}
    if(b.x<x0)x0=b.x;
    if(c.x<x0)x0=c.x;
    
    if(b.x>x1)x1=b.x;
    if(c.x>x1)x1=c.x;
    
    if(b.y<y0)y0=b.y;
    if(c.y<y0)y0=c.y;
    
    if(b.y>y1)y1=b.y;
    if(c.y>y1)y1=c.y;
    
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>=W)x1=W-1;
    if(y1>=H)y1=H-1;
    for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
        int64_t w0=cross(b,c,x,y),w1=cross(c,a,x,y),w2=cross(a,b,x,y);
        if(w0<0||w1<0||w2<0)continue;
        int32_t u=(int32_t)((w0*a.u+w1*b.u+w2*c.u)/area);
        int32_t v=(int32_t)((w0*a.v+w1*b.v+w2*c.v)/area);
        img[(size_t)y*W+(size_t)x]=dim(eye_uv_sample_repeat(tex,u,v),shade);
    }
}
static void quad(P a,P b,P c,P d,const uint32_t *tex,int shade) {
    tri(a,b,c,tex,shade);tri(a,c,d,tex,shade);
}
static void line(int x,int y,int xx,int yy,uint32_t c,int w) {
    int dx=abs(xx-x),dy=-abs(yy-y),sx=x<xx?1:-1,sy=y<yy?1:-1,err=dx+dy;
    for(;;){for(int oy=-w;oy<=w;++oy)for(int ox=-w;ox<=w;++ox){
        int px=x+ox,py=y+oy;
        if(px>=0&&px<W&&py>=0&&py<H)img[(size_t)py*W+(size_t)px]=c;
    }if(x==xx&&y==yy)break;
    int d=2*err;if(d>=dy){err+=dy;x+=sx;}if(d<=dx){err+=dx;y+=sy;}}
}
static int ppm(const char *path) {
    FILE *f=fopen(path,"wb");size_t i;
    if(!f)return 0;
    fprintf(f,"P6\n%d %d\n255\n",W,H);
    for(i=0;i<W*H;++i){
        uint8_t rgb[3]={(uint8_t)(img[i]>>16),(uint8_t)(img[i]>>8),(uint8_t)img[i]};
        if(fwrite(rgb,1,3,f)!=3){fclose(f);return 0;}
    }
    return fclose(f)==0;
}
static void background(void) {
    for(int y=0;y<H;++y)for(int x=0;x<W;++x) {
        uint32_t c;
        if(y<318){
            int d=90+y/12;
            c=0xff000000u|((uint32_t)d<<16)|((uint32_t)(d+3)<<8)|(uint32_t)(d+8);
            if(y>305-((x/27*7+x/81)%17) && y<318)c=0xff676a60u;
        }else{
            c=dim(tiles[6][((unsigned)(y*3)&63u)*64u+((unsigned)(x*2)&63u)],-13);
        }
        img[(size_t)y*W+(size_t)x]=c;
    }
}
static void front_panel(int x0,int y0,int x1,int y1,int motif,int shade){
    quad(pt(x0,y0,0,0),pt(x1,y0,1,0),pt(x1,y1,1,1),pt(x0,y1,0,1),tiles[motif],shade);
}
static void house(void) {
    /* Worn stone road, shared across all three constructors. */
    quad(pt(255,342,0,0),pt(386,349,2,0),pt(595,429,6,3),pt(29,429,0,3),tiles[6],-20);
    /* A two-floor timber frame and a side wall, with large encoded windows. */
    int xx[5]={144,211,278,345,412};
    int bottom[3]={343,255,166};
    for(int row=0;row<2;++row)for(int col=0;col<4;++col) {
        int motif;
        if(row==0){int a[4]={1,3,2,1};motif=a[col];}
        else {int a[4]={1,2,2,1};motif=a[col];}
        front_panel(xx[col],bottom[row+1],xx[col+1],bottom[row],motif,0);
    }
    /* The lateral elevation uses two rows, same modular texture vocabulary. */
    quad(pt(412,166,0,0),pt(480,184,1,0),pt(480,262,1,1),pt(412,255,0,1),tiles[2],-18);
    quad(pt(480,184,0,0),pt(548,202,1,0),pt(548,268,1,1),pt(480,262,0,1),tiles[0],-26);
    quad(pt(412,255,0,0),pt(480,262,1,0),pt(480,351,1,1),pt(412,343,0,1),tiles[1],-18);
    quad(pt(480,262,0,0),pt(548,268,1,0),pt(548,359,1,1),pt(480,351,0,1),tiles[2],-26);
    /* Low stone socle and timber middle belt as shared material. */
    quad(pt(143,343,0,0),pt(412,343,4,0),pt(412,362,4,1),pt(143,356,0,1),tiles[5],-15);
    quad(pt(412,343,0,0),pt(548,351,2,0),pt(548,364,2,1),pt(412,362,0,1),tiles[5],-32);
    /* Light-and-shadow linework only (no texture cut-outs). */
    line(144,255,412,255,0xff372e27u,2);
    line(412,166,548,202,0xff342b23u,2);
    /* Gable uses triangle geometry and repeated timber recipe, no triangular asset. */
    tri(pt(143,166,0,1),pt(278,90,1,0),pt(412,166,2,1),tiles[7],-8);
    line(144,166,278,90,0xff45382cu,2);
    line(278,90,411,166,0xff45382cu,2);
    /* Two roof planes slant away from ridge; same 64x64 roof tile. */
    quad(pt(275,81,0,0),pt(437,119,3,0),pt(558,203,3,2),pt(414,164,0,2),tiles[4],-5);
    quad(pt(125,175,0,0),pt(275,81,2,0),pt(437,119,2,2),pt(296,205,0,2),tiles[4],10);
    line(275,81,437,119,0xff625344u,1);
    line(412,166,558,203,0xff443a30u,1);
    line(125,175,296,205,0xff5d4d36u,1);
    /* Small dark wooden entry steps. */
    line(201,345,272,345,0xff4b3e30u,2);
}
int main(int argc,char **argv){
    Eye4Recipe recipes[8];Eye4Bank bank={0};Eye4Cache cache;
    static uint32_t arena[8u*4096u];uint8_t packet[EYE4_MAX_PACKET];size_t len;
    int method;
    if(argc!=3){fprintf(stderr,"usage: %s [0|1|2] output.ppm\n",argv[0]);return 2;}
    method=atoi(argv[1]);if(method<0||method>2)return 2;
    eye4_demo_recipes(recipes,(Eye4Method)method);
    len=eye4_write(recipes,8,packet,sizeof(packet));
    if(!len||!eye4_read(packet,len,&bank))return 3;
    if(!eye4_cache_init(&cache,&bank,arena,8u*4096u,8u))return 4;
    for(unsigned i=0;i<8;++i) {
        const uint32_t *p=eye4_cache_get(&cache,i);
        if(!p)return 5;
        memcpy(tiles[i],p,4096u*sizeof(uint32_t));
    }
    background();house();
    if(!ppm(argv[2]))return 6;
    printf("eye4 preview: method=%d bank=%zu B, generated=%u cache_hits=%u\n",
           method,len,cache.misses,cache.hits);
    return 0;
}
