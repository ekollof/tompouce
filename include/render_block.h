#ifndef RENDER_BLOCK_H
#define RENDER_BLOCK_H
#include "audio.h"

#define RENDER_BLOCK_FRAMES 4

/* Per-orbit accumulators for one four-frame block. The arrays are SoA so the
   block kernel can operate on four frames at once. */
typedef struct {
    double left[RENDER_BLOCK_FRAMES], right[RENDER_BLOCK_FRAMES];
    double delay_l[RENDER_BLOCK_FRAMES], delay_r[RENDER_BLOCK_FRAMES];
    double room_l[RENDER_BLOCK_FRAMES], room_r[RENDER_BLOCK_FRAMES];
    double room_size[RENDER_BLOCK_FRAMES];
} VoiceMixBlock;

#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__)) && !defined(TOMPOUCE_DISABLE_SIMD)
#define RENDER_BLOCK_HAVE_AVX2 1
#else
#define RENDER_BLOCK_HAVE_AVX2 0
#endif

/* Add one voice's four routed frames to a block. The caller supplies source
   samples after graph fade and uses only voices without scope/named targets. */
void render_block_route(const Voice *voice, const double *in_l,
    const double *in_r, VoiceMixBlock *mix);
/* Only frames whose bit is set in mask are routed. */
void render_block_route_mask(const Voice *voice, const double *in_l,
    const double *in_r, VoiceMixBlock *mix, unsigned mask);
void render_block_route_c(const Voice *voice, const double *in_l,
    const double *in_r, VoiceMixBlock *mix);
#if RENDER_BLOCK_HAVE_AVX2
void render_block_route_avx2(const Voice *voice, const double *in_l,
    const double *in_r, VoiceMixBlock *mix);
#endif

#endif
