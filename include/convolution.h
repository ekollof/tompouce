#ifndef CONVOLUTION_H
#define CONVOLUTION_H
#define CONV_BLOCK 128
#define CONV_FFT 256
#define CONV_PARTS 512
typedef struct { float re, im; } ConvComplex;
typedef struct ConvKernel {
    float head[2][CONV_BLOCK];
    unsigned parts;
    ConvComplex *tail;
} ConvKernel;
typedef struct {
    const ConvKernel *kernel;
    float input[2][CONV_BLOCK], wet[2][CONV_BLOCK], overlap[2][CONV_BLOCK];
    ConvComplex history[2][CONV_PARTS][CONV_FFT];
    unsigned position, block, filled, remaining;
} ConvState;
/* Generator thread only. Immutable cached kernels live for the process. */
const ConvKernel *convolution_prepare(int sample, double size, double speed, double begin);
/* Test/offline builder; normalization follows WebAudio when normalize != 0. */
ConvKernel *convolution_kernel(const float *stereo, int frames, int channels, int normalize);
void convolution_kernel_free(ConvKernel *kernel);
/* Callback safe: no allocation, I/O, or locks. */
void convolution_set(ConvState *state, const ConvKernel *kernel);
void convolution_process(ConvState *state, double left, double right, double *wet_left, double *wet_right);
#endif
