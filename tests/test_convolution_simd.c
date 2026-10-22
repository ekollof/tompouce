/* Production dispatch, arithmetic and streaming convolution, no devices. */
#include "../src/convolution_mix.c"
#include "../src/convolution.c"
#include <assert.h>
#include <stdint.h>

static uint32_t random_state=0x195ab7c2;
static float random_value(void) {
    random_state=random_state*1664525u+1013904223u;
    return ((int32_t)(random_state>>8)-8388608)*0x1p-23f;
}

static void kernel_equivalence(ConvolutionAccumulate accumulate) {
    /* Offset one complex value from malloc alignment and cover vector tails.
       Exact allocations let ASan catch reads/writes past the final bin. */
    const unsigned sizes[]={0,1,2,3,4,5,7,8,9,15,16,17,255,256,257};
    for (unsigned at=0;at<sizeof(sizes)/sizeof(*sizes);at++) {
        unsigned n=sizes[at];
        ConvComplex *a=malloc((n+1)*sizeof(*a)),*b=malloc((n+1)*sizeof(*b));
        ConvComplex *x=malloc((n+1)*sizeof(*x)),*h=malloc((n+1)*sizeof(*h));
        assert(a && b && x && h);
        a[0]=b[0]=(ConvComplex){123,-456};
        for (unsigned i=1;i<=n;i++) {
            a[i]=b[i]=(ConvComplex){random_value(),random_value()};
            x[i]=(ConvComplex){random_value(),random_value()};
            h[i]=(ConvComplex){random_value(),random_value()};
        }
        for (int partition=0;partition<1088;partition++) {
            accumulate_c(a+1,x+1,h+1,n);
            accumulate(b+1,x+1,h+1,n);
        }
        assert(!memcmp(a,b,(n+1)*sizeof(*a)));
        free(a); free(b); free(x); free(h);
    }
}

static void streaming_equivalence(ConvolutionAccumulate accumulate) {
    enum { frames=8193 };
    float *pcm=malloc(frames*2*sizeof(*pcm)); assert(pcm);
    for (int i=0;i<frames*2;i++) pcm[i]=random_value()*.02f;
    ConvKernel *k=convolution_kernel(pcm,frames,2,0); assert(k); free(pcm);
    ConvState *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b)); assert(a && b);
    assert(convolution_state_reserve(a,k->parts) && convolution_state_reserve(b,k->parts));
    convolution_set(a,k); convolution_set(b,k);
    MixBackend backend={accumulate,"test"};
    /* The override is private to this single-threaded test, never product API. */
    for (int i=0;i<160000;i++) {
        double l=i<145000?random_value()*.1:0;
        double r=i<145000?random_value()*.1:0;
        double al,ar,bl,br;
        atomic_store(&selected_backend,&portable_backend);
        convolution_process(a,l,r,&al,&ar);
        atomic_store(&selected_backend,&backend);
        convolution_process(b,l,r,&bl,&br);
        assert(al==bl && ar==br);
        if (i==13001) { convolution_set(a,NULL); convolution_set(b,NULL); }
        if (i==13002) { convolution_set(a,k); convolution_set(b,k); }
    }
    assert(!a->remaining && !b->remaining);
    atomic_store(&selected_backend,&portable_backend);
    convolution_kernel_free(k); convolution_state_free(a); convolution_state_free(b); free(a); free(b);
}

/* Demand-sized histories: a state that was never reserved must stay silent
   rather than write past a smaller buffer, and reserve must size exactly to
   the attached kernel so unused CONV_MAX_FRAMES slots are never allocated. */
static void demand_sized_history(void) {
    enum { frames=3000 };
    float *pcm=calloc((size_t)frames*2,sizeof(*pcm));
    assert(pcm);
    for (int i=0;i<frames*2;i++) pcm[i]=(float)(sin(i*.011)*.05);
    ConvKernel *k=convolution_kernel(pcm,frames,1,0); assert(k); free(pcm);
    ConvState silent={0};
    double l=1, r=1;
    convolution_set(&silent,k);
    assert(!silent.kernel); /* unreserved: refused, not truncated */
    convolution_process(&silent,1,1,&l,&r);
    assert(l==0 && r==0);
    assert(convolution_state_reserve(&silent,k->parts));
    assert(silent.slots==k->parts+1);
    convolution_set(&silent,k);
    for (int i=0;i<CONV_BLOCK;i++) convolution_process(&silent,i==0,i==0,&l,&r);
    ConvState retained={0}; assert(convolution_state_clone(&retained,&silent));
    assert(convolution_state_reserve(&silent,k->parts+10));
    for (int i=0;i<frames;i++) {
        double left,right;
        convolution_process(&silent,0,0,&l,&r);
        convolution_process(&retained,0,0,&left,&right);
        assert(l==left && r==right);
    }
    convolution_state_free(&retained);
    convolution_set(&silent,NULL); convolution_set(&silent,k);
    double energy=0;
    for (int i=0;i<frames+4*CONV_BLOCK;i++) {
        convolution_process(&silent,i==0?1:0,i==0?1:0,&l,&r);
        energy+=l*l+r*r;
    }
    assert(energy>0 && !silent.remaining);
    /* A clone must own its history: advancing it cannot disturb the source. */
    ConvState clone={0};
    assert(convolution_state_clone(&clone,&silent));
    assert(clone.history && clone.history!=silent.history && clone.slots==silent.slots);
    double sl, sr, cl, cr;
    convolution_set(&silent,k); convolution_set(&clone,k);
    for (int i=0;i<256;i++) {
        convolution_process(&silent,i==0?1:0,0,&sl,&sr);
        convolution_process(&clone,0,0,&cl,&cr);
        assert(cl==0 && cr==0);
    }
    /* A smaller kernel keeps the larger allocation but still matches. */
    float delta[2]={1,1};
    ConvKernel *small=convolution_kernel(delta,1,1,1); assert(small);
    unsigned slots=silent.slots;
    assert(convolution_state_reserve(&silent,small->parts));
    assert(silent.slots==slots);
    convolution_set(&silent,small);
    convolution_process(&silent,1,-2,&l,&r);
    assert(fabs(l-.00125)<1e-9 && fabs(r+.0025)<1e-9);
    convolution_kernel_free(small); convolution_kernel_free(k);
    convolution_state_free(&clone); convolution_state_free(&silent);
    puts("demand-sized convolution histories and clones passed");
}

static void *initialize_thread(void *unused) {
    (void)unused; convolution_simd_init(); return NULL;
}

static void selection_policy(void) {
    for (unsigned features=0;features<8;features++) {
        const char *expected="c";
#if CONV_HAVE_X86_SIMD
        if (features&MIX_SSE2) expected="sse2";
        if ((features&3)==3) expected="avx";
        if (features==7) expected="avx2";
#endif
        assert(!strcmp(choose_backend(NULL,features)->name,expected));
        assert(!strcmp(choose_backend("auto",features)->name,expected));
        assert(choose_backend("c",features)==&portable_backend);
        assert(choose_backend("invalid",features)==&portable_backend);
        const char *modes[]={"sse2","avx","avx2"};
        for (unsigned m=0;m<3;m++) {
            const char *forced="c";
#if CONV_HAVE_X86_SIMD
            unsigned required=(1u<<(m+1))-1;
            if ((features&required)==required) forced=modes[m];
#endif
            assert(!strcmp(choose_backend(modes[m],features)->name,forced));
        }
    }
}

int main(void) {
    log_console_stderr(0);
    selection_policy();
    /* Callback accessor has a safe fallback without running initialization. */
    assert(!strcmp(convolution_simd_name(),"c"));
    assert(convolution_accumulator()==accumulate_c);
    ConvolutionSimdStats before=convolution_simd_stats();
    assert(!strcmp(before.policy,"pending") && !strcmp(before.available,"none") && !before.fallback);
    pthread_t threads[8];
    for (unsigned i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,initialize_thread,NULL));
    for (unsigned i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
    const char *request=getenv("TOMPOUCE_SIMD");
    ConvolutionSimdStats stats=convolution_simd_stats();
    const char *policy=!request?"auto":request;
    if (strcmp(policy,"auto") && strcmp(policy,"c") && strcmp(policy,"sse2") &&
        strcmp(policy,"avx") && strcmp(policy,"avx2")) policy="invalid";
    assert(!strcmp(stats.policy,policy));
    assert(!strcmp(stats.kernel,convolution_simd_name()));
    assert((strstr(stats.available,"sse2")!=NULL)==!!(usable_features()&MIX_SSE2));
    assert((strstr(stats.available,"avx2")!=NULL)==!!(usable_features()&MIX_AVX2));
    assert(stats.fallback==(!strcmp(policy,"invalid") ||
        (strcmp(policy,"auto") && strcmp(policy,"c") && !strcmp(stats.kernel,"c"))));
    assert(!strcmp(convolution_simd_name(),choose_backend(request,usable_features())->name));
    ConvolutionAccumulate chosen=convolution_accumulator();
    convolution_simd_init(); assert(chosen==convolution_accumulator());
    printf("Convolution backend: %s\n",convolution_simd_name());
    kernel_equivalence(chosen); streaming_equivalence(chosen);
    puts("Exact kernel/streaming PCM parity, ring wrap, tails, replacement and vector bounds passed");
    demand_sized_history();
    return 0;
}
