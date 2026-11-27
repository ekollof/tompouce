#ifndef VOCODE_H
#define VOCODE_H
/* Shared phase-vocoder core (deployed transient-processor/OLA lineage via
   superdough 1.2.6, originally olvb/phaze): 2048-point FFT, Hann window,
   peak-region shifting with cursor phase correction, overlap-add with a
   128-sample hop. Used both by the offline stretch preparation and by
   the per-voice live vocoder, so the two agree sample for sample on
   static input. All transform state is passed in; no locks or globals
   beyond the shared read-only tables. */
#define VOCODE_FFT 2048
#define VOCODE_HOP 128
typedef struct { double re, im; } VocoderComplex;
void vocoder_tables_init(void);
void vocoder_real_fft(VocoderComplex *out, const float *input);
void vocoder_fft(VocoderComplex *data, int inverse);
void vocoder_transform(const float *input, float *sum, double factor,
    unsigned cursor, VocoderComplex *freq, VocoderComplex *shifted);
/* Map a raw stretch control value to the worklet pitch factor:
   negatives scale by a quarter first, then +1, clamped at zero. */
static inline double vocoder_factor(double raw) {
    double factor = raw < 0 ? raw * 0.25 : raw;
    factor += 1.0;
    return factor < 0 ? 0 : factor;
}

/* Streaming per-voice vocoder. The caller stages 128 input samples,
   the transform runs when a block completes, then the caller consumes
   128 output samples; input and output stay on the absolute 128-sample
   grid (cursor = block % 2048, matching the deployed timeCursor which
   starts at zero and steps per quantum) so the result matches the baked
   offline path sample for sample on identical input. All state is
   prepared up front (trigger time); the per-sample path never allocates.
   A voice starting mid-block passes skip = onset - first_block and the
   pre-onset slots stay zeroed, like the deployed chain whose nodes start
   with cleared histories. The per-block factor is set per sample with
   the last value winning, mirroring the worklet's
   parameters.pitchFactor[last] read. */
typedef struct {
    float in[2][VOCODE_FFT];
    float sum[2][VOCODE_FFT];
    VocoderComplex freq[VOCODE_FFT];
    VocoderComplex shifted[VOCODE_FFT];
    float staged[2][VOCODE_HOP];
    float out[2][VOCODE_HOP];
    int fill;        /* staged input samples in the current block */
    int avail;       /* unconsumed output samples */
    int out_pos;     /* next output sample to consume */
    long block;      /* absolute start sample of the block being staged */
    double factor;   /* mapped pitch factor for the next transform */
    int channels;    /* 1 = mono (ch0 transformed, mirrored to ch1) */
} LiveVocoder;
void live_vocoder_init(LiveVocoder *v, long first_block, int skip,
    double mapped_factor, int channels);
void live_vocoder_set_factor(LiveVocoder *v, double mapped_factor);
/* Stage one stereo input frame; when it completes a block the transform
   runs immediately. Consume the 128 outputs with the out/pop below. */
void live_vocoder_push(LiveVocoder *v, float left, float right);
int live_vocoder_avail(const LiveVocoder *v);
float live_vocoder_out_l(const LiveVocoder *v);
float live_vocoder_out_r(const LiveVocoder *v);
void live_vocoder_pop(LiveVocoder *v);
#endif
