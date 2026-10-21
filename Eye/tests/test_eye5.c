#include "eye5.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static uint32_t a[4096],b[4096],arena[8192];
static uint8_t packet[EYE5_MAX_PACKET],damaged[EYE5_MAX_PACKET];
int main(void){
    Eye5Recipe r[4];Eye5Bank bank={0};Eye5Cache c;Eye5RoofMesh m;
    Eye5RoofSpec s={2.2f,1.5f,3.f,1.25f,0.16f,EYE5_GABLE};
    eye5_demo_recipes(r);
    for(unsigned i=0;i<4u;++i){
        assert(eye5_valid(&r[i]));
        assert(eye5_generate(&r[i],a,4096u));
        assert(eye5_generate(&r[i],b,4096u));
        assert(memcmp(a,b,sizeof(a))==0);
        /* UV wrapping must be periodic, including outside 0..1. */
        for(int u=-2;u<=2;++u)for(int v=-2;v<=2;++v)
            assert(eye_uv_sample_repeat(a,u*65536+157, v*65536+231)==a[0]);
        assert(!eye5_generate(&r[i],b,4095u));
    }
    size_t n=eye5_write(r,4u,packet,sizeof packet);
    assert(n==60u);
    assert(eye5_read(packet,n,&bank));
    assert(bank.count==4u && bank.recipe[3].seed==r[3].seed);
    assert(!eye5_read(packet,n-1u,&bank));
    memcpy(damaged,packet,n);damaged[3]='4';assert(!eye5_read(damaged,n,&bank));
    memcpy(damaged,packet,n);damaged[27]^=1u;assert(!eye5_read(damaged,n,&bank));
    memcpy(damaged,packet,n);damaged[7]=1u;assert(!eye5_read(damaged,n,&bank));
    assert(!eye5_cache_init(&c,&bank,arena,4096u,2u));
    assert(eye5_cache_init(&c,&bank,arena,8192u,2u));
    assert(eye5_cache_get(&c,0u));memcpy(a,eye5_cache_get(&c,0u),sizeof(a));
    assert(c.hits==1u);
    assert(eye5_cache_get(&c,1u));assert(eye5_cache_get(&c,2u));
    assert(eye5_cache_get(&c,0u)); /* evict and regenerate */
    assert(memcmp(a,eye5_cache_get(&c,0u),sizeof(a))==0);
    assert(c.misses==4u);
    assert(!eye5_cache_get(&c,99u));
    assert(eye5_build_roof(&s,&m));assert(m.count==9u);
    assert(m.surface[0].vertex_count==4u&&m.surface[1].vertex_count==4u);
    assert(m.surface[0].vertex[0].x<0&&m.surface[1].vertex[1].x>0);
    /* Roof courses must run parallel to eaves: U spans length,
     * V is consistent on each equal-height ridge/eave. */
    assert(m.surface[0].vertex[0].v>m.surface[0].vertex[1].v);
    assert(m.surface[0].vertex[1].v==m.surface[0].vertex[2].v);
    assert(m.surface[0].vertex[0].v==m.surface[0].vertex[3].v);
    assert(m.surface[1].vertex[0].v<m.surface[1].vertex[1].v);
    assert(m.surface[1].vertex[0].v==m.surface[1].vertex[3].v);
    assert(m.surface[1].vertex[1].v==m.surface[1].vertex[2].v);
    assert(m.surface[2].role==EYE5_SURFACE_RIDGE);
    for(unsigned i=0;i<m.count;++i){
        assert(m.surface[i].vertex_count==4u);
        for(unsigned j=0;j<4u;++j){
            Eye5Vertex v=m.surface[i].vertex[j];
            assert(isfinite(v.x)&&isfinite(v.y)&&isfinite(v.z));
        }
    }
    s.form=EYE5_HIP;assert(eye5_build_roof(&s,&m));assert(m.count==13u);
    assert(m.surface[0].vertex[0].v>m.surface[0].vertex[1].v);
    assert(m.surface[1].vertex[1].v>m.surface[1].vertex[0].v);
    assert(m.surface[2].vertex[0].v==m.surface[2].vertex[1].v);
    assert(m.surface[2].vertex[2].v<m.surface[2].vertex[1].v);
    assert(m.surface[3].vertex[1].v>m.surface[3].vertex[0].v);
    assert(m.surface[3].vertex[2].v==m.surface[3].vertex[1].v);
    for(unsigned i=9u;i<13u;++i)
        assert(m.surface[i].role==EYE5_SURFACE_RIDGE);
    assert(m.surface[2].vertex_count==3u&&m.surface[3].vertex_count==3u);
    Eye5RoofMesh before=m;
    s.roof_rise=0.f;assert(!eye5_build_roof(&s,&m));assert(memcmp(&before,&m,sizeof(m))==0);
    s.roof_rise=1.25f;s.half_width=NAN;assert(!eye5_build_roof(&s,&m));
    r[0].width=7u;assert(!eye5_valid(&r[0]));
    printf("PASS EYE5: 4 tile families, 60 B bank, deterministic, EYE5 codec, eviction, two roof forms + corrected UV and hip flashings\n");
    return 0;
}
