#include "convolution.h"
#include "sampler.h"
#include "common.h"
#include "prep_profile.h"
#include "convolution_mix.h"

static void transform(ConvComplex *data, int inverse) {
    for (unsigned i = 1, j = 0; i < CONV_FFT; i++) {
        unsigned bit = CONV_FFT/2;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { ConvComplex value = data[i]; data[i] = data[j]; data[j] = value; }
    }
    for (unsigned length = 2; length <= CONV_FFT; length *= 2) {
        double angle = (inverse ? 2 : -2)*M_PI/length;
        double wr = cos(angle), wi = sin(angle);
        for (unsigned i = 0; i < CONV_FFT; i += length) {
            double re = 1, im = 0;
            for (unsigned j = 0; j < length/2; j++) {
                ConvComplex a = data[i+j], b = data[i+j+length/2];
                double br = b.re*re-b.im*im, bi = b.re*im+b.im*re;
                data[i+j] = (ConvComplex){(float)(a.re+br),(float)(a.im+bi)};
                data[i+j+length/2] = (ConvComplex){(float)(a.re-br),(float)(a.im-bi)};
                double next = re*wr-im*wi; im = re*wi+im*wr; re = next;
            }
        }
    }
    if (inverse) for (unsigned i = 0; i < CONV_FFT; i++) { data[i].re /= CONV_FFT; data[i].im /= CONV_FFT; }
}

void convolution_kernel_free(ConvKernel *kernel) {
    if (kernel) { free(kernel->tail); free(kernel); }
}

static ConvKernel *build_kernel(const float *stereo, int frames, int channels, int normalize, int normal_frames) {
    convolution_simd_init();
    if (!stereo || frames < 1 || frames > CONV_MAX_FRAMES || (channels != 1 && channels != 2)) return NULL;
    ConvKernel *kernel = calloc(1,sizeof(*kernel));
    if (!kernel) return NULL;
    kernel->parts = (unsigned)(frames-1)/CONV_BLOCK;
    size_t bins = (size_t)kernel->parts*2*CONV_FFT;
    if (bins) {
        kernel->tail = calloc(bins,sizeof(*kernel->tail));
        if (!kernel->tail) { free(kernel); return NULL; }
    }
    double sum = 0;
    for (int i = 0; i < frames; i++) for (int ch = 0; ch < channels; ch++) {
        double v = stereo[(size_t)i*2+ch];
        if (!isfinite(v)) { convolution_kernel_free(kernel); return NULL; }
        sum += v*v;
    }
    double scale = normalize ? .00125/fmax(.000125,sqrt(sum/((double)normal_frames*channels))) : 1;
    for (int ch = 0; ch < 2; ch++) {
        int source = channels == 1 ? 0 : ch;
        for (int i = 0; i < frames && i < CONV_BLOCK; i++) kernel->head[ch][i] = (float)(stereo[(size_t)i*2+source]*scale);
        prep_work_begin(PREP_FFT);
        for (unsigned part = 0; part < kernel->parts; part++) {
            ConvComplex *block = kernel->tail+((size_t)ch*kernel->parts+part)*CONV_FFT;
            for (unsigned i = 0; i < CONV_BLOCK; i++) {
                unsigned index = (part+1)*CONV_BLOCK+i;
                if (index < (unsigned)frames) block[i].re = (float)(stereo[(size_t)index*2+source]*scale);
            }
            transform(block,0);
        }
        prep_work_end(PREP_FFT);
    }
    return kernel;
}

ConvKernel *convolution_kernel(const float *stereo, int frames, int channels, int normalize) {
    return build_kernel(stereo,frames,channels,normalize,frames);
}

const ConvKernel *convolution_prepare(int sample, double size, double speed, double begin) {
    enum { CACHE = 16 };
    static struct { int sample; double size, speed, begin; ConvKernel *kernel; int reserved; } cache[CACHE];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (!isfinite(size) || !isfinite(speed) || !isfinite(begin) || size <= 0 || size > 10 ||
        speed == 0 || fabs(speed) > 8 || begin < 0 || begin > 1) return NULL;
    prep_lock(&lock, PREP_LOCK_CONV);
    for (unsigned i = 0; i < CACHE; i++) if (cache[i].kernel && cache[i].sample == sample && cache[i].size == size &&
        cache[i].speed == speed && cache[i].begin == begin) {
        const ConvKernel *result = cache[i].kernel; prep_unlock(&lock, PREP_LOCK_CONV); return result;
    }
    int slot = -1;
    for (unsigned i = 0; i < CACHE; i++) if (!cache[i].kernel && !cache[i].reserved) { slot = (int)i; break; }
    if (slot < 0) { prep_unlock(&lock, PREP_LOCK_CONV); return NULL; }
    cache[slot].reserved = 1;
    prep_unlock(&lock, PREP_LOCK_CONV);
    float *buffer; int length, channels;
    smp_get_stereo(sample,&buffer,&length,&channels);
    int frames = length > 0 ? (int)fmin(length,ceil(size*SR)) : 0;
    if (!buffer || frames < 1 || frames > CONV_MAX_FRAMES) {
        prep_lock(&lock, PREP_LOCK_CONV);
        cache[slot].reserved = 0;
        prep_unlock(&lock, PREP_LOCK_CONV);
        return NULL;
    }
    float *impulse = calloc((size_t)frames*2,sizeof(*impulse));
    if (!impulse) {
        prep_lock(&lock, PREP_LOCK_CONV);
        cache[slot].reserved = 0;
        prep_unlock(&lock, PREP_LOCK_CONV);
        return NULL;
    }
    for (int i = 0; i < frames; i++) {
        double position = fmod(floor(begin*length)+(speed < 0 ? -i : i)*fabs(speed),length);
        if (position < 0) position += length;
        int index = (int)position;
        impulse[(size_t)i*2] = buffer[(size_t)index*2];
        impulse[(size_t)i*2+1] = buffer[(size_t)index*2+1];
    }
    ConvKernel *kernel = build_kernel(impulse,frames,channels,1,length);
    free(impulse);
    prep_lock(&lock, PREP_LOCK_CONV);
    for (unsigned i = 0; i < CACHE; i++) if (cache[i].kernel && cache[i].sample == sample && cache[i].size == size &&
        cache[i].speed == speed && cache[i].begin == begin) {
        const ConvKernel *result = cache[i].kernel;
        cache[slot].reserved = 0;
        prep_unlock(&lock, PREP_LOCK_CONV);
        convolution_kernel_free(kernel);
        return result;
    }
    if (kernel) {
        cache[slot].sample = sample; cache[slot].size = size; cache[slot].speed = speed;
        cache[slot].begin = begin; cache[slot].kernel = kernel; cache[slot].reserved = 0;
    } else cache[slot].reserved = 0;
    prep_unlock(&lock, PREP_LOCK_CONV);
    return kernel;
}

int convolution_state_reserve(ConvState *state, unsigned parts) {
    if (!state) return 0;
    unsigned slots = parts ? parts + 1 : 1;
    if (slots <= state->slots) return 1;
    size_t bins = (size_t)slots*2*CONV_FFT;
    if (bins > SIZE_MAX/sizeof(*state->history)) return 0;
    ConvComplex *next = calloc(bins,sizeof(*next));
    if (!next) return 0;
    free(state->history);
    state->history = next;
    state->slots = slots;
    return 1;
}

void convolution_state_free(ConvState *state) {
    if (!state) return;
    free(state->history);
    state->history = NULL;
    state->slots = 0;
    state->kernel = NULL;
}

int convolution_state_clone(ConvState *dst, const ConvState *src) {
    if (!dst || !src) return 0;
    ConvState copy = *src;
    copy.history = NULL;
    if (src->history && src->slots) {
        size_t bins = (size_t)src->slots*2*CONV_FFT;
        copy.history = malloc(bins*sizeof(*copy.history));
        if (!copy.history) return 0;
        memcpy(copy.history,src->history,bins*sizeof(*copy.history));
    }
    *dst = copy;
    return 1;
}

void convolution_set(ConvState *state, const ConvKernel *kernel) {
    if (state->kernel == kernel) return;
    /* Never touch a history that was not sized for this kernel; a state that
       was not reserved stays silent instead of writing out of bounds. */
    if (kernel && (!state->history || kernel->parts + 1 > state->slots)) kernel = NULL;
    /* History slots become valid only after process_block rewrites them. */
    memset(state->input,0,sizeof(state->input));
    memset(state->wet,0,sizeof(state->wet));
    memset(state->overlap,0,sizeof(state->overlap));
    state->position = state->block = state->filled = state->remaining = 0; state->kernel = kernel;
}

static void process_block(ConvState *state) {
    const ConvKernel *kernel = state->kernel;
    ConvolutionAccumulate accumulate=convolution_accumulator();
    unsigned slots=kernel->parts+1;
    unsigned slot = state->block;
    ConvComplex *base = state->history;
    for (int ch = 0; ch < 2; ch++) {
        ConvComplex *spectrum = base + ((size_t)ch*slots + slot)*CONV_FFT;
        memset(spectrum,0,CONV_FFT*sizeof(*spectrum));
        for (unsigned i = 0; i < CONV_BLOCK; i++) spectrum[i].re = state->input[ch][i];
        transform(spectrum,0);
        ConvComplex sum[CONV_FFT] = {{0}};
        unsigned parts = kernel->parts;
        if (parts > state->filled+1) parts = state->filled+1;
        unsigned read=slot;
        for (unsigned part = 0; part < parts; part++) {
            const ConvComplex *x = base + ((size_t)ch*slots + read)*CONV_FFT;
            const ConvComplex *h = kernel->tail+((size_t)ch*kernel->parts+part)*CONV_FFT;
            accumulate(sum,x,h,CONV_FFT);
            read=read?read-1:slots-1;
        }
        transform(sum,1);
        for (unsigned i = 0; i < CONV_BLOCK; i++) {
            state->wet[ch][i] = sum[i].re+state->overlap[ch][i];
            state->overlap[ch][i] = sum[i+CONV_BLOCK].re;
        }
    }
    state->block = slot+1==slots?0:slot+1;
    if (state->filled < slots) state->filled++;
}

void convolution_process(ConvState *state, double left, double right, double *wet_left, double *wet_right) {
    *wet_left = *wet_right = 0;
    if (!state->kernel) return;
    if (left != 0 || right != 0) state->remaining = (state->kernel->parts+2)*CONV_BLOCK;
    if (!state->remaining) return;
    state->remaining--;
    unsigned position = state->position;
    state->input[0][position] = (float)left; state->input[1][position] = (float)right;
    double wet[2];
    for (int ch = 0; ch < 2; ch++) {
        wet[ch] = state->wet[ch][position];
        for (unsigned i = 0; i < CONV_BLOCK; i++)
            wet[ch] += state->input[ch][(position+CONV_BLOCK-i)%CONV_BLOCK]*state->kernel->head[ch][i];
    }
    *wet_left = wet[0]; *wet_right = wet[1];
    if (++state->position == CONV_BLOCK) {
        if (state->kernel->parts) process_block(state);
        state->position = 0;
    }
}
