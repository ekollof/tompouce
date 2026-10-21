#include "convolution_mix.h"
#include "common.h"
#include <stdatomic.h>

/* This translation unit alone uses -O3 and disables FP contraction. The
   baseline remains portable; only targeted functions may emit newer SIMD. */
#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__)) && !defined(TOMPOUCE_DISABLE_SIMD)
#define CONV_HAVE_X86_SIMD 1
#else
#define CONV_HAVE_X86_SIMD 0
#endif

static void accumulate_c(ConvComplex *restrict sum,const ConvComplex *x,
                         const ConvComplex *h,unsigned count) {
    for (unsigned i=0;i<count;i++) {
        sum[i].re+=x[i].re*h[i].re-x[i].im*h[i].im;
        sum[i].im+=x[i].re*h[i].im+x[i].im*h[i].re;
    }
}

#if CONV_HAVE_X86_SIMD
__attribute__((target("sse2,no-sse3,no-ssse3,no-sse4.1,no-sse4.2,no-avx,no-avx2,no-fma"),noinline))
static void accumulate_sse2(ConvComplex *restrict sum,const ConvComplex *x,
                            const ConvComplex *h,unsigned count) {
    for (unsigned i=0;i<count;i++) {
        sum[i].re+=x[i].re*h[i].re-x[i].im*h[i].im;
        sum[i].im+=x[i].re*h[i].im+x[i].im*h[i].re;
    }
}

__attribute__((target("avx,no-avx2,no-fma"),noinline))
static void accumulate_avx(ConvComplex *restrict sum,const ConvComplex *x,
                           const ConvComplex *h,unsigned count) {
    for (unsigned i=0;i<count;i++) {
        sum[i].re+=x[i].re*h[i].re-x[i].im*h[i].im;
        sum[i].im+=x[i].re*h[i].im+x[i].im*h[i].re;
    }
}

__attribute__((target("avx2,no-fma"),noinline))
static void accumulate_avx2(ConvComplex *restrict sum,const ConvComplex *x,
                            const ConvComplex *h,unsigned count) {
    /* Independent frequency bins vectorize without reordering partitions or
       changing the real/imaginary multiply, add and subtract sequence. */
    for (unsigned i=0;i<count;i++) {
        sum[i].re+=x[i].re*h[i].re-x[i].im*h[i].im;
        sum[i].im+=x[i].re*h[i].im+x[i].im*h[i].re;
    }
}
#endif

typedef struct { ConvolutionAccumulate accumulate; const char *name; } MixBackend;
static const MixBackend portable_backend={accumulate_c,"c"};
#if CONV_HAVE_X86_SIMD
static const MixBackend sse2_backend={accumulate_sse2,"sse2"};
static const MixBackend avx_backend={accumulate_avx,"avx"};
static const MixBackend avx2_backend={accumulate_avx2,"avx2"};
#endif
static _Atomic(const MixBackend *) selected_backend=&portable_backend;
static pthread_once_t mix_once=PTHREAD_ONCE_INIT;
static _Atomic unsigned diagnostic_state;
static const char *const policy_names[]={"pending","auto","c","sse2","avx","avx2","invalid"};

enum { MIX_SSE2=1, MIX_AVX=2, MIX_AVX2=4 };
static unsigned usable_features(void) {
#if CONV_HAVE_X86_SIMD
    /* Compiler runtime detection accounts for OS AVX state support as well
       as CPUID. No architecture-specific instructions execute before this. */
    __builtin_cpu_init();
    unsigned features=0;
    if (__builtin_cpu_supports("sse2")) features|=MIX_SSE2;
    if (__builtin_cpu_supports("avx")) features|=MIX_AVX;
    if (__builtin_cpu_supports("avx2")) features|=MIX_AVX2;
    return features;
#else
    return 0;
#endif
}

/* Pure policy also permits testing older CPU/OS feature sets on modern hosts.
   Explicit unsupported requests retain the established safe C fallback. */
static const MixBackend *choose_backend(const char *request,unsigned features) {
#if CONV_HAVE_X86_SIMD
    int automatic=!request || !strcmp(request,"auto");
    if ((features&(MIX_SSE2|MIX_AVX|MIX_AVX2))==(MIX_SSE2|MIX_AVX|MIX_AVX2) &&
        (automatic || !strcmp(request,"avx2"))) return &avx2_backend;
    if ((features&(MIX_SSE2|MIX_AVX))==(MIX_SSE2|MIX_AVX) &&
        (automatic || !strcmp(request,"avx"))) return &avx_backend;
    if ((features&MIX_SSE2) && (automatic || !strcmp(request,"sse2"))) return &sse2_backend;
#else
    (void)request; (void)features;
#endif
    return &portable_backend;
}

static void select_backend(void) {
    const char *requested=getenv("TOMPOUCE_SIMD");
    int automatic=!requested || !strcmp(requested,"auto");
    unsigned features=usable_features(),policy=6;
    for (unsigned i=1;i<6;i++)
        if ((automatic && i==1) || (requested && !strcmp(requested,policy_names[i]))) { policy=i; break; }
    const MixBackend *backend=choose_backend(requested,features);
    int fallback=policy==6 || (policy>=3 && backend==&portable_backend);
    atomic_store_explicit(&diagnostic_state,features|(policy<<3)|(fallback?64u:0u),memory_order_release);
    if (!automatic && strcmp(requested,"c") && strcmp(requested,"sse2") &&
        strcmp(requested,"avx") && strcmp(requested,"avx2")) {
        log_console("SIMD: invalid TOMPOUCE_SIMD (use auto, c, sse2, avx or avx2); using C");
        return;
    }
    if (!automatic && !strcmp(requested,"c")) return;
    atomic_store_explicit(&selected_backend,backend,memory_order_release);
    if (!automatic && backend==&portable_backend)
        log_console("SIMD: requested kernel unavailable; using C");
}

void convolution_simd_init(void) { pthread_once(&mix_once,select_backend); }
const char *convolution_simd_name(void) {
    return atomic_load_explicit(&selected_backend,memory_order_acquire)->name;
}
ConvolutionSimdStats convolution_simd_stats(void) {
    static const char *const names[]={"none","sse2","avx","sse2/avx",
        "avx2","sse2/avx2","avx/avx2","sse2/avx/avx2"};
    unsigned state=atomic_load_explicit(&diagnostic_state,memory_order_acquire);
    return (ConvolutionSimdStats){convolution_simd_name(),policy_names[(state>>3)&7],names[state&7],!!(state&64)};
}
ConvolutionAccumulate convolution_accumulator(void) {
    return atomic_load_explicit(&selected_backend,memory_order_acquire)->accumulate;
}
