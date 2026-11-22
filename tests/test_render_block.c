/* Exactness of the four-frame block routing kernel against its C reference. */
#include "../src/render_block.c"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t random_state=0x243f6a88u;
static double random_value(void) {
    random_state=random_state*1664525u+1013904223u;
    return (double)(random_state>>8)/16777216.0-0.5;
}

#if RENDER_BLOCK_HAVE_AVX2
static void compare(int cross, int dsp, double room) {
    Voice voice={0};
    voice.route_ll=random_value();
    voice.route_rr=random_value();
    voice.route_lr=cross?random_value():0;
    voice.route_rl=cross?random_value():0;
    voice.route_left=random_value()+1;
    voice.route_right=random_value()+1;
    voice.route_dry=random_value();
    voice.route_delay=random_value();
    voice.route_room=room;
    voice.route_room_size=random_value();
    voice.route_dsp=dsp;
    double in_l[RENDER_BLOCK_FRAMES], in_r[RENDER_BLOCK_FRAMES];
    VoiceMixBlock a, b;
    for (unsigned i=0;i<RENDER_BLOCK_FRAMES;i++) {
        in_l[i]=random_value()*2;
        in_r[i]=random_value()*2;
        a.left[i]=b.left[i]=random_value();
        a.right[i]=b.right[i]=random_value();
        a.delay_l[i]=b.delay_l[i]=random_value();
        a.delay_r[i]=b.delay_r[i]=random_value();
        a.room_l[i]=b.room_l[i]=random_value();
        a.room_r[i]=b.room_r[i]=random_value();
        a.room_size[i]=b.room_size[i]=random_value();
    }
    render_block_route_c(&voice,in_l,in_r,&a);
    render_block_route_avx2(&voice,in_l,in_r,&b);
    assert(!memcmp(&a,&b,sizeof(a)));
    double scope_c[RENDER_BLOCK_FRAMES]={0}, scope_avx2[RENDER_BLOCK_FRAMES]={0};
    render_block_route_mask_scope_c(&voice,in_l,in_r,&a,
        (1u<<RENDER_BLOCK_FRAMES)-1,scope_c);
    render_block_route_avx2_scope(&voice,in_l,in_r,&b,scope_avx2);
    assert(!memcmp(scope_c,scope_avx2,sizeof(scope_c)));
    for (unsigned i=0;i<RENDER_BLOCK_FRAMES;i++) {
        double l=in_l[i]*voice.route_ll+(voice.route_lr!=0?in_r[i]*voice.route_lr:0);
        double r=in_r[i]*voice.route_rr+(voice.route_rl!=0?in_l[i]*voice.route_rl:0);
        double want=(l*voice.route_left+r*voice.route_right)*voice.route_dry*.5;
        assert(scope_c[i]==want);
    }
}

int main(void) {
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx2")) {
        puts("render block SIMD skipped (no AVX2)");
        return 0;
    }
    const double rooms[]={0,.1,.7};
    for (int cross=0;cross<2;cross++)
        for (int dsp=0;dsp<2;dsp++)
            for (unsigned r=0;r<sizeof(rooms)/sizeof(*rooms);r++)
                compare(cross,dsp,rooms[r]);
    puts("render block SIMD exactness ok");
    return 0;
}
#else
int main(void) {
    puts("render block SIMD unavailable");
    return 0;
}
#endif
