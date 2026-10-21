/* Headless 3D orthographic preview rasterizer for Eye 0.5; never shipped.
 * The geometry is produced by eye5_build_roof, not hand-drawn polygons. */
#include "eye5.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W 840
#define H 560
#define NT 4096
static uint32_t frame[W*H],roof_pixels[4][NT],wall_pixels[8][NT],timber[NT];
static float depth[W*H];
typedef struct {float x,y,z,u,v;} V;
typedef struct {float x,y,d,u,v;} Projected;
static uint32_t darken(uint32_t c,int off){
    int r=(int)((c>>16)&255u)+off,g=(int)((c>>8)&255u)+off,b=(int)(c&255u)+off;
    if(r<0)r=0;
    if(g<0)g=0;
    if(b<0)b=0;
    if(r>255)r=255;
    if(g>255)g=255;
    if(b>255)b=255;
    return UINT32_C(0xff000000)|((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
}
static V pos(float x,float y,float z,float u,float v){return (V){x,y,z,u,v};}
static Projected project(V p){
    return (Projected){390.f+p.x*87.f+p.z*49.f,485.f-p.y*79.f-p.z*24.f,
                       p.z-p.x*0.81f-p.y*0.025f,p.u,p.v};
}
static float orient(Projected a,Projected b,float x,float y){return (x-a.x)*(b.y-a.y)-(y-a.y)*(b.x-a.x);}
static void triangle(V p0,V p1,V p2,const uint32_t *tex,int shade){
    Projected a=project(p0),b=project(p1),c=project(p2);
    float area=orient(a,b,c.x,c.y);
    if(fabsf(area)<0.00001f)return;
    if(area<0.f){Projected tmp=b;b=c;c=tmp;area=-area;}
    float xx0=fminf(a.x,fminf(b.x,c.x)),xx1=fmaxf(a.x,fmaxf(b.x,c.x));
    float yy0=fminf(a.y,fminf(b.y,c.y)),yy1=fmaxf(a.y,fmaxf(b.y,c.y));
    int x0=(int)floorf(xx0),x1=(int)ceilf(xx1),y0=(int)floorf(yy0),y1=(int)ceilf(yy1);
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>=W)x1=W-1;
    if(y1>=H)y1=H-1;
    for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
        float w0=orient(b,c,(float)x+0.5f,(float)y+0.5f);
        float w1=orient(c,a,(float)x+0.5f,(float)y+0.5f);
        float w2=orient(a,b,(float)x+0.5f,(float)y+0.5f);
        if(w0<0.f||w1<0.f||w2<0.f)continue;
        float z=(w0*a.d+w1*b.d+w2*c.d)/area;
        size_t offset=(size_t)y*W+(size_t)x;
        if(z>=depth[offset])continue;
        float u=(w0*a.u+w1*b.u+w2*c.u)/area;
        float v=(w0*a.v+w1*b.v+w2*c.v)/area;
        int32_t iu=(int32_t)floorf(u*65536.f);
        int32_t iv=(int32_t)floorf(v*65536.f);
        uint32_t color=eye_uv_sample_repeat(tex,iu,iv);
        frame[offset]=darken(color,shade);depth[offset]=z;
    }
}
static void quad(V a,V b,V c,V d,const uint32_t *tex,int shade){
    triangle(a,b,c,tex,shade);triangle(a,c,d,tex,shade);
}
static void background(void){
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){
        size_t i=(size_t)y*W+(size_t)x;
        if(y<436){int v=117+y/18;frame[i]=UINT32_C(0xff000000)|((uint32_t)v<<16)|
            ((uint32_t)(v+3)<<8)|(uint32_t)(v+6);}
        else {int k=((x/9+y/11)*5)%17;
            frame[i]=UINT32_C(0xff55584e)+((uint32_t)k<<16)+((uint32_t)k<<8);}
        depth[i]=1.0e15f;
    }
}
static void wall(float xa,float xb,float ya,float yb,float z,unsigned which,int shade){
    quad(pos(xa,ya,z,0,1),pos(xb,ya,z,1,1),pos(xb,yb,z,1,0),pos(xa,yb,z,0,0),wall_pixels[which],shade);
}
static void side(float za,float zb,float ya,float yb,unsigned which,int shade){
    float x=2.2f;
    quad(pos(x,ya,za,0,1),pos(x,ya,zb,1,1),pos(x,yb,zb,1,0),pos(x,yb,za,0,0),wall_pixels[which],shade);
}
static void house(int kind,int hip){
    Eye5RoofMesh mesh;
    Eye5RoofSpec spec={2.2f,1.5f,3.2f,1.24f,0.18f,(uint8_t)(hip?EYE5_HIP:EYE5_GABLE)};
    const float zf=-1.50f;
    /* Footpath reuses the old EYE4 architectural cobblestone. */
    quad(pos(-3.8f,-0.13f,-2.4f,0,0),pos(3.9f,-0.13f,-2.4f,6,0),
         pos(4.1f,-0.13f,-3.9f,6,2),pos(-3.8f,-0.13f,-3.9f,0,2),wall_pixels[6],-17);
    for(unsigned row=0;row<2u;++row){
        float ya=(float)(float)row*1.6f,yb=ya+1.6f;
        for(unsigned col=0;col<4u;++col){
            float xa=-2.2f+(float)col*1.1f,xb=xa+1.1f;
            static const unsigned mosaic[2][4]={{1,3,2,1},{1,2,2,1}};
            wall(xa,xb,ya,yb,zf,mosaic[row][col],row?-3:0);
        }
    }
    for(unsigned row=0;row<2u;++row){
        float ya=(float)row*1.6f,yb=ya+1.6f;
        for(unsigned col=0;col<2u;++col){
            float z0=-1.5f+(float)col*1.5f,z1=z0+1.5f;
            static const unsigned mosaic[2][2]={{1,2},{2,0}};
            side(z0,z1,ya,yb,mosaic[row][col],-22);
        }
    }
    quad(pos(-2.2f,0.0f,zf-.008f,0,1),pos(2.2f,0.f,zf-.008f,4,1),
         pos(2.2f,0.22f,zf-.008f,4,0),pos(-2.2f,0.22f,zf-.008f,0,0),wall_pixels[5],-15);
    /* Gable wall is a triangle: texture from EYE4, no new triangular bitmap. */
    if(!hip)triangle(pos(-2.2f,3.2f,zf-.01f,0,1),pos(2.2f,3.2f,zf-.01f,2,1),
                     pos(0,4.44f,zf-.01f,1,0),wall_pixels[7],-3);
    if(!eye5_build_roof(&spec,&mesh)){fprintf(stderr,"invalid roof mesh\n");exit(4);}
    for(unsigned i=0;i<mesh.count;++i){
        const Eye5Surface *s=&mesh.surface[i];V v[4];
        for(unsigned j=0;j<4u;++j){Eye5Vertex a=s->vertex[j];v[j]=pos(a.x,a.y,a.z,a.u,a.v);}
        const uint32_t *tile=s->role==EYE5_SURFACE_TRIM?timber:roof_pixels[kind];
        int shade=s->role==EYE5_SURFACE_TRIM?-14:
                  (s->role==EYE5_SURFACE_RIDGE?-12:(i==0?-7:2));
        if(s->vertex_count==3u)triangle(v[0],v[1],v[2],tile,shade);
        else quad(v[0],v[1],v[2],v[3],tile,shade);
    }
}
static int save_ppm(const char *file,const uint32_t *img,unsigned width,unsigned height){
    FILE *f=fopen(file,"wb");if(!f)return 0;
    fprintf(f,"P6\n%u %u\n255\n",width,height);
    for(size_t i=0;i<(size_t)width*height;++i){
        uint8_t rgb[3]={(uint8_t)(img[i]>>16),(uint8_t)(img[i]>>8),(uint8_t)img[i]};
        if(fwrite(rgb,1,3,f)!=3){fclose(f);return 0;}
    }
    return fclose(f)==0;
}
int main(int argc,char **argv){
    Eye5Recipe roofs[4];Eye4Recipe facade[8];Eye5Bank bank;
    uint8_t packet[EYE5_MAX_PACKET];char file[512];
    static uint32_t arena[2*4096u];Eye5Cache cache;
    if(argc!=2){fprintf(stderr,"usage: %s preview_dir\n",argv[0]);return 2;}
    eye5_demo_recipes(roofs);size_t n=eye5_write(roofs,4,packet,sizeof packet);
    if(n!=60u||!eye5_read(packet,n,&bank))return 3;
    snprintf(file,sizeof file,"%s/recetas_cubiertas.eye5",argv[1]);
    FILE *f=fopen(file,"wb");if(!f)return 3;
    if(fwrite(packet,1,n,f)!=n||fclose(f))return 3;
    eye4_demo_recipes(facade,EYE4_HYBRID);
    for(unsigned i=0;i<8u;++i)if(!eye4_generate(&facade[i],wall_pixels[i],4096u))return 3;
    EyeRecipe wood=(EyeRecipe){EYE_WOOD,0,1,0,10,1,9,0,UINT32_C(0x500544)};
    if(!eye3_generate(&wood,timber,4096u))return 3;
    if(!eye5_cache_init(&cache,&bank,arena,8192,2u))return 3;
    for(unsigned i=0;i<4u;++i){
        const uint32_t *p=eye5_cache_get(&cache,i);if(!p)return 4;
        memcpy(roof_pixels[i],p,sizeof roof_pixels[i]);
    }
    const char *names[]={"pizarra","teja","madera","paja"};
    for(unsigned i=0;i<4u;++i){
        background();house((int)i,0);
        snprintf(file,sizeof file,"%s/casa_%s.ppm",argv[1],names[i]);
        if(!save_ppm(file,frame,W,H))return 4;
    }
    background();house(0,1);
    snprintf(file,sizeof file,"%s/casa_cuatro_aguas.ppm",argv[1]);
    if(!save_ppm(file,frame,W,H))return 4;
    static uint32_t atlas[4*256*256];
    for(unsigned q=0;q<4u;++q)for(unsigned y=0;y<256u;++y)for(unsigned x=0;x<256u;++x)
        atlas[((q/2u)*256u+y)*512u+(q%2u)*256u+x]=roof_pixels[q][(y%64u)*64u+(x%64u)];
    snprintf(file,sizeof file,"%s/cubiertas_4_tipos.ppm",argv[1]);
    if(!save_ppm(file,atlas,512u,512u))return 4;
    printf("Eye 0.5: %zu bytes recetas, 4 roof families, 2 roof forms, cache hits=%u misses=%u\n",
           n,cache.hits,cache.misses);
    return 0;
}
