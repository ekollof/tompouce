#include "render_block.h"
#include "convolution_mix.h"
#include <stdatomic.h>
#include <string.h>
#if RENDER_BLOCK_HAVE_AVX2
#include <immintrin.h>
#endif

static void render_block_route_mask_c(const Voice *voice, const double *in_l,
                                      const double *in_r, VoiceMixBlock *mix,
                                      unsigned mask) {
    const double ll=voice->route_ll, lr=voice->route_lr;
    const double rl=voice->route_rl, rr=voice->route_rr;
    const double left=voice->route_left, right=voice->route_right;
    const double dry=voice->route_dry, delay=voice->route_delay;
    const double room=voice->route_room;
    int simple=lr==0 && rl==0;
    for (unsigned i=0;i<RENDER_BLOCK_FRAMES;i++) {
        if (!(mask&(1u<<i))) continue;
        double l, r;
        if (simple) {
            l=in_l[i]*ll; r=in_r[i]*rr;
        } else {
            l=in_l[i]*ll+in_r[i]*lr;
            r=in_l[i]*rl+in_r[i]*rr;
        }
        double base_l=l*left, base_r=r*right;
        mix->left[i]+=base_l*dry;
        mix->right[i]+=base_r*dry;
        mix->delay_l[i]+=base_l*delay;
        mix->delay_r[i]+=base_r*delay;
        if (voice->route_dsp) {
            mix->room_l[i]+=base_l*room;
            mix->room_r[i]+=base_r*room;
            if (room>0) mix->room_size[i]=voice->route_room_size;
        }
    }
}

void render_block_route_c(const Voice *voice, const double *in_l,
                          const double *in_r, VoiceMixBlock *mix) {
    render_block_route_mask_c(voice,in_l,in_r,mix,
        (1u<<RENDER_BLOCK_FRAMES)-1);
}

#if RENDER_BLOCK_HAVE_AVX2
__attribute__((target("avx2,no-fma"),noinline))
void render_block_route_avx2(const Voice *voice, const double *in_l,
                             const double *in_r, VoiceMixBlock *mix) {
    __m256d l4=_mm256_loadu_pd(in_l), r4=_mm256_loadu_pd(in_r);
    __m256d routed_l, routed_r;
    if (voice->route_lr==0 && voice->route_rl==0) {
        routed_l=_mm256_mul_pd(l4,_mm256_set1_pd(voice->route_ll));
        routed_r=_mm256_mul_pd(r4,_mm256_set1_pd(voice->route_rr));
    } else {
        routed_l=_mm256_add_pd(
            _mm256_mul_pd(l4,_mm256_set1_pd(voice->route_ll)),
            _mm256_mul_pd(r4,_mm256_set1_pd(voice->route_lr)));
        routed_r=_mm256_add_pd(
            _mm256_mul_pd(l4,_mm256_set1_pd(voice->route_rl)),
            _mm256_mul_pd(r4,_mm256_set1_pd(voice->route_rr)));
    }
    __m256d base_l=_mm256_mul_pd(routed_l,_mm256_set1_pd(voice->route_left));
    __m256d base_r=_mm256_mul_pd(routed_r,_mm256_set1_pd(voice->route_right));
    __m256d dry=_mm256_set1_pd(voice->route_dry);
    _mm256_storeu_pd(mix->left,_mm256_add_pd(_mm256_loadu_pd(mix->left),
        _mm256_mul_pd(base_l,dry)));
    _mm256_storeu_pd(mix->right,_mm256_add_pd(_mm256_loadu_pd(mix->right),
        _mm256_mul_pd(base_r,dry)));
    __m256d delay=_mm256_set1_pd(voice->route_delay);
    _mm256_storeu_pd(mix->delay_l,_mm256_add_pd(_mm256_loadu_pd(mix->delay_l),
        _mm256_mul_pd(base_l,delay)));
    _mm256_storeu_pd(mix->delay_r,_mm256_add_pd(_mm256_loadu_pd(mix->delay_r),
        _mm256_mul_pd(base_r,delay)));
    if (voice->route_dsp) {
        __m256d room=_mm256_set1_pd(voice->route_room);
        _mm256_storeu_pd(mix->room_l,_mm256_add_pd(_mm256_loadu_pd(mix->room_l),
            _mm256_mul_pd(base_l,room)));
        _mm256_storeu_pd(mix->room_r,_mm256_add_pd(_mm256_loadu_pd(mix->room_r),
            _mm256_mul_pd(base_r,room)));
        if (voice->route_room>0) {
            __m256d size=_mm256_set1_pd(voice->route_room_size);
            _mm256_storeu_pd(mix->room_size,size);
        }
    }
}
#endif

static atomic_int avx2_choice=-1;
static int use_avx2(void) {
    int cached=atomic_load(&avx2_choice);
    if (cached<0) {
        convolution_simd_init();
        cached=strcmp(convolution_simd_name(),"avx2")==0;
        atomic_store(&avx2_choice,cached);
    }
    return cached;
}

void render_block_route(const Voice *voice, const double *in_l,
                        const double *in_r, VoiceMixBlock *mix) {
#if RENDER_BLOCK_HAVE_AVX2
    if (use_avx2()) {
        render_block_route_avx2(voice,in_l,in_r,mix);
        return;
    }
#endif
    render_block_route_c(voice,in_l,in_r,mix);
}

void render_block_route_mask(const Voice *voice, const double *in_l,
                             const double *in_r, VoiceMixBlock *mix,
                             unsigned mask) {
    if (mask==(1u<<RENDER_BLOCK_FRAMES)-1) {
        render_block_route(voice,in_l,in_r,mix);
        return;
    }
    render_block_route_mask_c(voice,in_l,in_r,mix,mask);
}
