/* Black-box tests of Bones v2 quad geometry and the existing Eye rasterizer.
   Deliberately does not require EYE1/EYE2 source, which was not supplied. */
#include "../include/bones/bones.h"
#include "../src/adapters/eye_adapter.h"
#include "../src/demo/test_house.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static BonesEngine engine;
static BonesEye eye;
static BonesEye other;
static BonesScene scene;
static uint32_t calls;
static bool missing_wood;

static bool recipe_stub(void *ctx, uint32_t id, uint32_t seed,
                        uint32_t *out, uint32_t width, uint32_t height) {
    unsigned x,y;
    (void)ctx;
    ++calls;
    assert(width==64u && height==64u && id<3u);
    if (missing_wood && id==BONES_MAT_WOOD) {
        /* Even a broken generator's partial writes cannot poison Eye cache. */
        out[0]=0u;
        return false;
    }
    for(y=0;y<height;y++) for(x=0;x<width;x++)
        out[y*width+x]=UINT32_C(0xff000000) |
                           (((x*3u + seed + 43u*id)&255u)<<16) |
                           (((y*5u + seed/7u + 29u*id)&255u)<<8) |
                           (((x^y^seed^(id*19u))&255u));
    return true;
}
static BonesVertex vertex(float x,float y,float z,float u,float v) {
    BonesVertex a={{x,y,z},u,v};
    return a;
}
static BonesQuad plane(uint8_t m,float z,float off_u,float off_v) {
    BonesQuad q={0};
    q.vertices[0]=vertex(-1,-1,z,off_u,off_v+1);
    q.vertices[1]=vertex( 1,-1,z,off_u+1,off_v+1);
    q.vertices[2]=vertex( 1, 1,z,off_u+1,off_v);
    q.vertices[3]=vertex(-1, 1,z,off_u,off_v);
    q.material=m;
    return q;
}
static BonesVec3 cross(BonesQuad *q) {
    BonesVec3 a={q->vertices[1].position.x-q->vertices[0].position.x,
                 q->vertices[1].position.y-q->vertices[0].position.y,
                 q->vertices[1].position.z-q->vertices[0].position.z};
    BonesVec3 b={q->vertices[2].position.x-q->vertices[0].position.x,
                 q->vertices[2].position.y-q->vertices[0].position.y,
                 q->vertices[2].position.z-q->vertices[0].position.z};
    BonesVec3 c={a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
    return c;
}
static uint64_t draw(const BonesScene *s) {
    assert(bones_draw_scene(s,&eye.backend));
    return p144_renderer_image_hash(&eye.renderer);
}
static void test_house(void) {
    uint32_t i,stone=0,wood=0,floor_count=0;
    uint64_t with_house,without_house;
    bones_init(&engine,11);
    bones_eye_init(&eye,11);
    bones_make_scene(&engine,&scene);
    without_house=draw(&scene);
    assert(bones_house_append_scene(&scene));
    assert(scene.quad_count==32+BONES_HOUSE_QUADS);
    for(i=32;i<scene.quad_count;i++) {
        const BonesQuad *q=&scene.quads[i];
        if(q->material==BONES_MAT_STONE) stone++;
        if(q->material==BONES_MAT_WOOD) wood++;
        if(q->material==BONES_MAT_FLOOR) floor_count++;
    }
    assert(stone==6 && wood==3 && floor_count==1);
    for(i=38;i<=39;i++) { /* frontons with duplicated final vertex */
        const BonesQuad *q=&scene.quads[i];
        assert(memcmp(&q->vertices[2],&q->vertices[3],sizeof(BonesVertex))==0);
        assert(fabsf(cross((BonesQuad*)q).z)>0.1f);
    }
    assert(cross(&scene.quads[40]).y>0);
    assert(cross(&scene.quads[41]).y>0);
    assert(cross(&scene.quads[32]).z<0); /* front wall faces outward (-z) */
    assert(cross(&scene.quads[37]).z<0); /* door likewise */
    with_house=draw(&scene);
    assert(with_house!=without_house);
    assert(eye.renderer.triangles>20);
    assert(draw(&scene)==with_house);
    /* All-or-nothing append when the quad budget is too small. */
    scene.quad_count=BONES_MAX_QUADS-BONES_HOUSE_QUADS+1;
    assert(!bones_house_append_scene(&scene));
    assert(scene.quad_count==BONES_MAX_QUADS-BONES_HOUSE_QUADS+1);
    puts("PASS: casa 10 quads, 2 gables triangulares, techo/normales y materiales compartidos");
}
static void test_triangle_orientation(void) {
    uint64_t h1,h2,h3;
    BonesVertex a=vertex(-1,-1,3,0,1),b=vertex(1,-1,3,1,1),c=vertex(0,1,3,.5f,0);
    BonesQuad q={0};
    bones_eye_init(&eye,5);
    memset(&scene,0,sizeof(scene));
    q.material=BONES_MAT_STONE;
    q.vertices[0]=a;q.vertices[1]=b;q.vertices[2]=c;q.vertices[3]=c;
    assert(bones_scene_add_quad(&scene,&q));
    h1=draw(&scene);
    assert(eye.renderer.triangles==1);
    assert(eye.renderer.depth[(P144_VIEW_HEIGHT/2)*P144_VIEW_WIDTH+P144_VIEW_WIDTH/2]>0);
    q.vertices[0]=a;q.vertices[1]=a;q.vertices[2]=b;q.vertices[3]=c;
    scene.quads[0]=q;
    h2=draw(&scene);
    assert(h1==h2 && eye.renderer.triangles==1);
    q.vertices[0]=a;q.vertices[1]=c;q.vertices[2]=b;q.vertices[3]=b;
    scene.quads[0]=q;
    h3=draw(&scene);
    assert(h1==h3); /* Current renderer renders BOTH sides, winding independent. */
    puts("PASS: ambos triangulos de quad, UV por vertice, doble cara actual");
}
static void test_uv_and_depth(void) {
    uint64_t hash,periodic_hash;
    uint32_t center,front;
    BonesQuad q;
    bones_eye_init(&eye,73);
    calls=0; missing_wood=false;
    assert(bones_eye_load_textures(&eye,recipe_stub,NULL)==3);
    assert(calls==3);
    memset(&scene,0,sizeof(scene));
    q=plane(BONES_MAT_STONE,3,0,0);
    assert(bones_scene_add_quad(&scene,&q));
    hash=draw(&scene);
    q=plane(BONES_MAT_STONE,3,1,-1);
    scene.quads[0]=q;
    periodic_hash=draw(&scene);
    assert(hash==periodic_hash); /* UV repeat for ±1 wrap */
    q=plane(BONES_MAT_STONE,3,.5f,0);
    scene.quads[0]=q;
    assert(draw(&scene)!=hash); /* independently tunable U */
    q=plane(BONES_MAT_STONE,3,0,.5f);
    scene.quads[0]=q;
    assert(draw(&scene)!=hash); /* independently tunable V */
    assert(calls==3); /* NO regeneration per frame */
    memset(&scene,0,sizeof(scene));
    q=plane(BONES_MAT_FLOOR,3,0,0);
    assert(bones_scene_add_quad(&scene,&q));
    draw(&scene);
    center=(P144_VIEW_HEIGHT/2)*P144_VIEW_WIDTH+P144_VIEW_WIDTH/2;
    front=eye.renderer.pixels[center];
    q=plane(BONES_MAT_STONE,5,0,0);
    assert(bones_scene_add_quad(&scene,&q));
    draw(&scene);
    assert(eye.renderer.pixels[center]==front);
    scene.quads[0]=q;
    scene.quads[1]=plane(BONES_MAT_FLOOR,3,0,0);
    draw(&scene);
    assert(eye.renderer.pixels[center]==front); /* Depth wins, not draw order. */
    puts("PASS: UV independientes, repeticion negativa, oclusion Z y cache por carga");
}
static void test_regeneration_and_fallback(void) {
    uint64_t h1,h2;
    uint32_t old_pixel;
    bones_eye_init(&eye,7);
    bones_eye_init(&other,7);
    calls=0; missing_wood=false;
    assert(bones_eye_load_textures(&eye,recipe_stub,NULL)==3);
    assert(bones_eye_load_textures(&other,recipe_stub,NULL)==3);
    assert(memcmp(eye.renderer.tex,other.renderer.tex,sizeof(eye.renderer.tex))==0);
    old_pixel=eye.renderer.tex[BONES_MAT_WOOD][0];
    missing_wood=true;
    assert(bones_eye_load_textures(&eye,recipe_stub,NULL)==2);
    assert(eye.renderer.tex[BONES_MAT_WOOD][0]==old_pixel);
    assert(!bones_eye_load_textures(NULL,recipe_stub,NULL));
    assert(!bones_eye_load_textures(&eye,NULL,NULL));
    missing_wood=false;
    memset(&scene,0,sizeof(scene));
    assert(bones_scene_add_quad(&scene,&(BonesQuad){
        { {{-1,-1,3},0,0},{{1,-1,3},1,0},{{1,1,3},1,1},{{-1,1,3},0,1} },BONES_MAT_STONE}));
    h1=draw(&scene);
    bones_eye_init(&eye,8);
    assert(bones_eye_load_textures(&eye,recipe_stub,NULL)==3);
    h2=draw(&scene);
    assert(h1!=h2);
    assert(bones_house_append_scene(&scene));
    assert(scene.quad_count==11);
    printf("PASS: material externo, fallback, regeneracion; memoria Eye=%zu, Scene=%zu, quad=%zu\n",
        sizeof(BonesEye),sizeof(BonesScene),sizeof(BonesQuad));
}
int main(void) {
    test_house(); test_triangle_orientation(); test_uv_and_depth();
    test_regeneration_and_fallback();
    puts("PASS: test_geometry completado");
    return 0;
}
