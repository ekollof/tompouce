/* Independent Fourier-table builder following WebAudio's 4096-point,
   three-bands-per-octave periodic-wave definition. Prepared off the callback.
   Strudel coefficient/phase conventions: packages/superdough/synth.mjs. */
#include "dsp.h"
#include "common.h"

enum { WAVE_SIZE=4096, WAVE_RANGES=36, WAVE_CACHE=256, WAVE_BUDGET=64*1024*1024, NOISE_FRAMES=88200 };
struct DspPeriodic {
    int count,kind,numeric;
    double weights[32],phases[32];
    float *table[WAVE_RANGES],*storage;
    size_t bytes;
};
static DspPeriodic *waves[WAVE_CACHE];
static unsigned wave_count;
static size_t wave_bytes;
static pthread_mutex_t wave_lock=PTHREAD_MUTEX_INITIALIZER;
typedef struct { double re,im; } Complex;
typedef struct { Complex fft[WAVE_SIZE]; float re[WAVE_SIZE/2],im[WAVE_SIZE/2]; } WaveWork;

static void inverse(Complex *data) {
    for (unsigned i=1,j=0;i<WAVE_SIZE;i++) {
        unsigned bit=WAVE_SIZE/2;
        for (;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if (i<j) { Complex swap=data[i];data[i]=data[j];data[j]=swap; }
    }
    for (unsigned length=2;length<=WAVE_SIZE;length*=2) {
        double angle=2*M_PI/length,wr=cos(angle),wi=sin(angle);
        for (unsigned i=0;i<WAVE_SIZE;i+=length) {
            double re=1,im=0;
            for (unsigned j=0;j<length/2;j++) {
                Complex a=data[i+j],b=data[i+j+length/2];
                double br=b.re*re-b.im*im,bi=b.re*im+b.im*re;
                data[i+j]=(Complex){a.re+br,a.im+bi};
                data[i+j+length/2]=(Complex){a.re-br,a.im-bi};
                double next=re*wr-im*wi;im=re*wi+im*wr;re=next;
            }
        }
    }
}

static int matches(const DspPeriodic *wave,const DspParams *p) {
    return wave->count==p->n_partials && wave->kind==p->periodic_kind &&
        wave->numeric==p->periodic_numeric &&
        !memcmp(wave->weights,p->partials,sizeof(wave->weights)) &&
        !memcmp(wave->phases,p->phases,sizeof(wave->phases));
}

int dsp_prepare_periodic(DspParams *p) {
    if (!p->n_partials) return 1;
    if (p->n_partials<0 || p->n_partials>2047 || p->periodic_kind<1 || p->periodic_kind>4 ||
        (!p->periodic_numeric && p->n_partials>32)) return 0;
    for (int i=0;i<32;i++) if (!isfinite(p->partials[i]) || !isfinite(p->phases[i])) return 0;
    pthread_mutex_lock(&wave_lock);
    for (unsigned i=0;i<wave_count;i++) if (matches(waves[i],p)) {
        p->periodic=waves[i]; pthread_mutex_unlock(&wave_lock); return 1;
    }
    int full=wave_count==WAVE_CACHE || wave_bytes>=WAVE_BUDGET;
    pthread_mutex_unlock(&wave_lock);
    if (full) return 0;
    DspPeriodic *wave=calloc(1,sizeof(*wave)); WaveWork *work=calloc(1,sizeof(*work));
    if (!wave || !work) {free(wave);free(work);return 0;}
    unsigned limits[WAVE_RANGES],tables=0;
    for (unsigned range=0;range<WAVE_RANGES;range++) {
        unsigned limit=(unsigned)((float)pow(2,-(double)range/3)*(WAVE_SIZE/2));
        limits[range]=limit>(unsigned)p->n_partials?(unsigned)p->n_partials:limit;
        if (!range || limits[range]!=limits[range-1]) tables++;
    }
    wave->bytes=sizeof(*wave)+(size_t)tables*WAVE_SIZE*sizeof(float);
    wave->storage=calloc((size_t)tables*WAVE_SIZE,sizeof(float));
    if (!wave->storage) {free(wave);free(work);return 0;}
    wave->count=p->n_partials;wave->kind=p->periodic_kind;wave->numeric=p->periodic_numeric;
    memcpy(wave->weights,p->partials,sizeof(wave->weights));memcpy(wave->phases,p->phases,sizeof(wave->phases));
    for (int k=1;k<=p->n_partials;k++) {
        double weight=p->periodic_numeric?1:p->partials[k-1];
        double re=0,im=0,phase=k<=32?2*M_PI*p->phases[k-1]:0;
        if (p->periodic_kind==1) im=-weight/k;
        else if (p->periodic_kind==2) im=k%2?weight/k:0;
        else if (p->periodic_kind==3) re=k%2?weight/(k*k):0;
        else im=weight;
        work->re[k]=(float)(cos(phase)*re-sin(phase)*im);
        work->im[k]=(float)(sin(phase)*re+cos(phase)*im);
    }
    double scale=1;
    unsigned table=0;
    for (unsigned range=0;range<WAVE_RANGES;range++) {
        if (range && limits[range]==limits[range-1]) {wave->table[range]=wave->table[range-1];continue;}
        wave->table[range]=wave->storage+(size_t)table++*WAVE_SIZE;
        unsigned limit=limits[range];
        memset(work->fft,0,sizeof(work->fft));
        for (unsigned k=1;k<=limit;k++) {
            work->fft[k]=(Complex){work->re[k]*.5,-work->im[k]*.5};
            work->fft[WAVE_SIZE-k]=(Complex){work->re[k]*.5,work->im[k]*.5};
        }
        inverse(work->fft);
        if (!range) {
            double peak=0;
            for (unsigned i=0;i<WAVE_SIZE;i++) peak=fmax(peak,fabs(work->fft[i].re));
            if (peak>0) scale=1/peak;
        }
        for (unsigned i=0;i<WAVE_SIZE;i++) wave->table[range][i]=(float)(work->fft[i].re*scale);
    }
    free(work);
    pthread_mutex_lock(&wave_lock);
    for (unsigned i=0;i<wave_count;i++) if (matches(waves[i],p)) {
        p->periodic=waves[i];pthread_mutex_unlock(&wave_lock);free(wave->storage);free(wave);return 1;
    }
    if (wave_count==WAVE_CACHE || wave->bytes>WAVE_BUDGET-wave_bytes) {
        pthread_mutex_unlock(&wave_lock);free(wave->storage);free(wave);return 0;
    }
    wave_bytes+=wave->bytes;
    waves[wave_count++]=wave;p->periodic=wave;
    pthread_mutex_unlock(&wave_lock);return 1;
}

double dsp_additive(DspState *s,double phase,double frequency) {
    const DspPeriodic *wave=s->control.periodic;
    if (!wave) return 0;
    float freq=(float)fabs(frequency);
    if (freq!=s->periodic_frequency) {
        float range=freq>0?1+3*log2f(freq/(44100.f/WAVE_SIZE)):0;
        range=fmaxf(0,fminf(WAVE_RANGES-1,range));
        s->periodic_range=(unsigned)range;s->periodic_blend=range-s->periodic_range;
        s->periodic_frequency=freq;
    }
    double pos=phase/(2*M_PI);pos=(pos-floor(pos))*WAVE_SIZE;
    unsigned a=(unsigned)pos,b=(a+1)%WAVE_SIZE,r=s->periodic_range;
    unsigned next=r+1<WAVE_RANGES?r+1:r;double f=pos-a;
    double high=wave->table[r][a]*(1-f)+wave->table[r][b]*f;
    double low=wave->table[next][a]*(1-f)+wave->table[next][b]*f;
    return high+(low-high)*s->periodic_blend;
}

static float *noise_buffers[3];
static pthread_mutex_t noise_lock=PTHREAD_MUTEX_INITIALIZER;
const float *dsp_prepare_noise(int color) {
    if (color<0 || color>2) return NULL;
    pthread_mutex_lock(&noise_lock);
    if (!noise_buffers[color]) {
        float *pcm=calloc(NOISE_FRAMES,sizeof(*pcm));
        if (pcm) {
            unsigned seed=0xc0ffeeu;double b[7]={0},last=0;
            for (int i=0;i<NOISE_FRAMES;i++) {
                seed=seed*1664525u+1013904223u;double white=seed/2147483648.0-1;
                if (!color) pcm[i]=(float)white;
                else if (color==2) {pcm[i]=(float)((last+.02*white)/1.02);last=pcm[i];}
                else {
                    b[0]=.99886*b[0]+white*.0555179;b[1]=.99332*b[1]+white*.0750759;
                    b[2]=.969*b[2]+white*.153852;b[3]=.8665*b[3]+white*.3104856;
                    b[4]=.55*b[4]+white*.5329522;b[5]=-.7616*b[5]-white*.016898;
                    pcm[i]=(float)(b[0]+b[1]+b[2]+b[3]+b[4]+b[5]+b[6]+white*.5362);
                    pcm[i]=(float)(pcm[i]*.11);b[6]=white*.115926;
                }
            }
            noise_buffers[color]=pcm;
        }
    }
    const float *result=noise_buffers[color];pthread_mutex_unlock(&noise_lock);return result;
}

double dsp_source_noise(DspState *s) {
    if (!s->control.source_noise) return 0;
    unsigned a=(unsigned)s->source_noise_position,b=(a+1)%NOISE_FRAMES;
    double fraction=s->source_noise_position-a;
    double left=s->control.source_noise[a],right=s->control.source_noise[b];
    double result=left+(right-left)*fraction;
    s->source_noise_position++;
    if (s->source_noise_position>=NOISE_FRAMES) s->source_noise_position-=NOISE_FRAMES;
    return result;
}
