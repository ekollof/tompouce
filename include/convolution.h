#ifndef CONVOLUTION_H
#define CONVOLUTION_H
#define CONV_BLOCK 128
#define CONV_FFT 256
/* Web roomsize maximum 10 s plus its 50% IR tail at 44.1 kHz.
   Histories are bounded here; only kernel-length ring slots are touched. */
#define CONV_MAX_ROOM_SIZE 10
#define CONV_MAX_FRAMES 661500
#define CONV_PARTS ((CONV_MAX_FRAMES+CONV_BLOCK-1)/CONV_BLOCK)
typedef struct { float re, im; } ConvComplex;
typedef struct ConvKernel {
    float head[2][CONV_BLOCK];
    unsigned parts;
    unsigned channels;
    ConvComplex *tail;
} ConvKernel;
typedef struct {
    const ConvKernel *kernel;
    float input[2][CONV_BLOCK], wet[2][CONV_BLOCK], overlap[2][CONV_BLOCK];
    /* [2][slots][CONV_FFT], sized to the attached kernel by
       convolution_state_reserve (preparation only; never the callback).
       Only kernel->parts+1 slots are ever touched, so allocating the
       10-second maximum per orbit would reserve ~20 MiB of unused memory. */
    ConvComplex *history;
    unsigned slots;
    unsigned position, block, filled, remaining;
    unsigned right_position, right_block, right_filled, right_remaining;
} ConvState;
/* Generator/worker only: size the history for a kernel with `parts` tail
   blocks, retaining its existing history. The caller must exclusively own
   the state: never resize a live rendering state. Returns 0 on allocation
   failure, leaving the state unchanged. */
int convolution_state_reserve(ConvState *state, unsigned parts);
/* Release a state's history. Not for callback use. */
void convolution_state_free(ConvState *state);
/* Deep-copy a state, including its history, into dst. dst must be zeroed or
   previously freed; a plain struct assignment would alias the history. */
int convolution_state_clone(ConvState *dst, const ConvState *src);
/* Generator/worker only. Identical concurrent preparations share one build;
   waiters may sleep. Never call these from the audio callback. Immutable
   cached kernels live for the process. */
const ConvKernel *convolution_prepare(int sample, double size, double speed, double begin);
const ConvKernel *convolution_prepare_reverb(double size,double fade,double lp,double dim);
/* Optional static failure description; generator/worker only. */
const ConvKernel *convolution_prepare_reverb_ex(double size,double fade,double lp,double dim,const char **error);
/* Reset the generated-room random stream to its startup seed, with preparation
   idle. Does not clear cached kernels. Tests/offline renderers only. */
void convolution_reverb_reset_stream(void);
/* Test/offline builder; normalization follows WebAudio when normalize != 0. */
ConvKernel *convolution_kernel(const float *stereo, int frames, int channels, int normalize);
void convolution_kernel_free(ConvKernel *kernel);
/* Callback safe: no allocation, I/O, or locks. */
void convolution_set(ConvState *state, const ConvKernel *kernel);
void convolution_process(ConvState *state, double left, double right, double *wet_left, double *wet_right);
/* Mono input + mono IR processes only the left history and duplicates output.
   The right history/clock is paused until stereo input reconnects. Stereo IRs
   still process both histories, using the mono input on both sides. */
void convolution_process_input(ConvState *state,double left,double right,int stereo_input,double *wet_left,double *wet_right);
#endif
