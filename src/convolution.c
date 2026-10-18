#include "convolution.h"
#include "sampler.h"
#include "common.h"

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
    if (!stereo || frames < 1 || frames > CONV_PARTS*CONV_BLOCK || (channels != 1 && channels != 2)) return NULL;
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
        for (unsigned part = 0; part < kernel->parts; part++) {
            ConvComplex *block = kernel->tail+((size_t)ch*kernel->parts+part)*CONV_FFT;
            for (unsigned i = 0; i < CONV_BLOCK; i++) {
                unsigned index = (part+1)*CONV_BLOCK+i;
                if (index < (unsigned)frames) block[i].re = (float)(stereo[(size_t)index*2+source]*scale);
            }
            transform(block,0);
        }
    }
    return kernel;
}

ConvKernel *convolution_kernel(const float *stereo, int frames, int channels, int normalize) {
    return build_kernel(stereo,frames,channels,normalize,frames);
}

const ConvKernel *convolution_prepare(int sample, double size, double speed, double begin) {
    enum { CACHE = 16 };
    static struct { int sample; double size, speed, begin; ConvKernel *kernel; } cache[CACHE];
    static unsigned used;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (!isfinite(size) || !isfinite(speed) || !isfinite(begin) || size <= 0 || size > 10 ||
        speed == 0 || fabs(speed) > 8 || begin < 0 || begin > 1) return NULL;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < used; i++) if (cache[i].sample == sample && cache[i].size == size &&
        cache[i].speed == speed && cache[i].begin == begin) {
        const ConvKernel *result = cache[i].kernel; pthread_mutex_unlock(&lock); return result;
    }
    if (used == CACHE) { pthread_mutex_unlock(&lock); return NULL; }
    float *buffer; int length, channels;
    smp_get_stereo(sample,&buffer,&length,&channels);
    int frames = length > 0 ? (int)fmin(length,ceil(size*SR)) : 0;
    if (!buffer || frames < 1 || frames > CONV_PARTS*CONV_BLOCK) { pthread_mutex_unlock(&lock); return NULL; }
    float *impulse = calloc((size_t)frames*2,sizeof(*impulse));
    if (!impulse) { pthread_mutex_unlock(&lock); return NULL; }
    for (int i = 0; i < frames; i++) {
        double position = fmod(floor(begin*length)+(speed < 0 ? -i : i)*fabs(speed),length);
        if (position < 0) position += length;
        int index = (int)position;
        impulse[(size_t)i*2] = buffer[(size_t)index*2];
        impulse[(size_t)i*2+1] = buffer[(size_t)index*2+1];
    }
    ConvKernel *kernel = build_kernel(impulse,frames,channels,1,length);
    free(impulse);
    if (kernel) {
        cache[used].sample = sample; cache[used].size = size; cache[used].speed = speed;
        cache[used].begin = begin; cache[used++].kernel = kernel;
    }
    pthread_mutex_unlock(&lock); return kernel;
}

void convolution_set(ConvState *state, const ConvKernel *kernel) {
    if (state->kernel == kernel) return;
    /* History slots become valid only after process_block rewrites them. */
    memset(state->input,0,sizeof(state->input));
    memset(state->wet,0,sizeof(state->wet));
    memset(state->overlap,0,sizeof(state->overlap));
    state->position = state->block = state->filled = state->remaining = 0; state->kernel = kernel;
}

static void process_block(ConvState *state) {
    const ConvKernel *kernel = state->kernel;
    unsigned slot = state->block%CONV_PARTS;
    for (int ch = 0; ch < 2; ch++) {
        ConvComplex *spectrum = state->history[ch][slot];
        memset(spectrum,0,CONV_FFT*sizeof(*spectrum));
        for (unsigned i = 0; i < CONV_BLOCK; i++) spectrum[i].re = state->input[ch][i];
        transform(spectrum,0);
        ConvComplex sum[CONV_FFT] = {{0}};
        unsigned parts = kernel->parts;
        if (parts > state->filled+1) parts = state->filled+1;
        for (unsigned part = 0; part < parts; part++) {
            const ConvComplex *x = state->history[ch][(slot+CONV_PARTS-part)%CONV_PARTS];
            const ConvComplex *h = kernel->tail+((size_t)ch*kernel->parts+part)*CONV_FFT;
            for (unsigned i = 0; i < CONV_FFT; i++) {
                sum[i].re += x[i].re*h[i].re-x[i].im*h[i].im;
                sum[i].im += x[i].re*h[i].im+x[i].im*h[i].re;
            }
        }
        transform(sum,1);
        for (unsigned i = 0; i < CONV_BLOCK; i++) {
            state->wet[ch][i] = sum[i].re+state->overlap[ch][i];
            state->overlap[ch][i] = sum[i+CONV_BLOCK].re;
        }
    }
    state->block = (state->block+1)%CONV_PARTS;
    if (state->filled < CONV_PARTS) state->filled++;
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
