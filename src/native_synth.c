/* Native ZzFX is adapted from Frank Force's MIT-licensed ZzFX and the
   Strudel/superdough 1.2.6 fork (AGPL-3.0-or-later). Kabelsalat nodes follow
   @kabelsalat/lib 0.4.1, Felix Roos and contributors (AGPL-3.0-or-later).
   See thirdparty/native-synth/README.md for sources and notices. */
#include "native_synth.h"
#include "common.h"
#include "dsp.h"
#include <stdint.h>
#include <stdatomic.h>

#define NS_OPS 256
#define NS_INPUTS 16
#define NS_CACHE 256
#define NS_DELAY_MAX 262144u
#define NS_STATE_BUDGET (64u*1024u*1024u)
#define TAU 6.2831853071795864769

enum { N_CONST,N_TIME,N_GATE,N_AUDIO,N_EXIT,N_POLY,N_OUTPUT,N_SRC,N_THRU,
 N_ADD,N_SUB,N_MUL,N_DIV,N_MOD,N_POW,N_EXP,N_LOG,N_SIN,N_COS,N_TAN,N_ASIN,N_ACOS,N_ATAN,
 N_ABS,N_ROUND,N_FLOOR,N_CEIL,N_SIGN,N_MIN,N_MAX,N_CLAMP,N_CLIP,N_RANGE,N_REMAP,N_MIDI,
 N_GT,N_LT,N_XOR,N_AND,N_OR,N_NOT,N_BOOL,N_IF,
 N_SINE,N_SAW,N_ZAW,N_TRI,N_PULSE,N_NOISE,N_BROWN,N_PINK,N_LCG,N_DUST,N_IMPULSE,N_CLOCK,N_CLOCKDIV,
 N_ADSR,N_DELAY,N_HOLD,N_LAG,N_SLIDE,N_SLEW,N_FILTER,N_BPF,N_FOLD,N_DISTORT,N_SEQ,N_PICK,N_TRIG,
 N_BYTE_TIME,N_NAN,N_PLUS,N_MINUS,N_TIMES,N_DIVIDE,N_REMAINDER,N_POWER,N_BITAND,N_BITOR,N_BITXOR,
 N_SHL,N_SHR,N_USHR,N_LESS,N_LE,N_GREATER,N_GE,N_EQ,N_NE,N_STRICTEQ,N_STRICTNE,N_LOGAND,N_LOGOR,
 N_POS,N_NEG,N_BITNOT,N_LOGNOT,N_SQRT,N_LOG2,N_LOG10,N_TRUNC,N_TYPES };
static const char *const names[]={"n","time","nativeGate","audioin","exit","poly","output","src","thru",
 "add","sub","mul","div","mod","pow","exp","log","sin","cos","tan","asin","acos","atan",
 "abs","round","floor","ceil","sign","min","max","clamp","clip","range","remap","midinote",
 "greater","lower","xor","and","or","not","bool","ifelse",
 "sine","saw","zaw","tri","pulse","noise","brown","pink","lcgnoise","dust","impulse","clock","clockdiv",
 "adsr","delay","hold","lag","slide","slew","filter","bpf","fold","distort","seq","pick","trig",
 "byteTime","nan","+","-","*","/","%","**","&","|","^","<<",">>",">>>","<","<=",">",">=",
 "==","!=","===","!==","&&","||","unary+","unary-","unary~","unary!","sqrt","log2","log10","trunc"};
_Static_assert(sizeof(names)/sizeof(*names)==N_TYPES,"native opcode table");
typedef struct { int type, count; unsigned in[NS_INPUTS], delay_at, delay_size; double value; } NativeOp;
struct NativeSynth {
    atomic_uint refs;
    int kind, count, result, effect, restart, envelope;
    double tail;
    double offset, z[20]; unsigned delay_samples;
    NativeOp ops[NS_OPS];
};
struct NativeSynthState {
    const NativeSynth *program;
    size_t bytes;
    uint32_t random;
    unsigned frame, crush, repeat, jump;
    unsigned byte_wait;
    int byte_started;
    double reg[NS_OPS], memory[NS_OPS][6], outputs[2], sources[16];
    double z[20], phase, modulation_time, last, start_frequency, start_slide;
    unsigned length;
    float delay[];
};
static atomic_size_t state_bytes;
static atomic_uint seed=1;
static struct { char *key; NativeSynth *program; } cache[NS_CACHE];
static unsigned cache_count;
static pthread_mutex_t cache_lock=PTHREAD_MUTEX_INITIALIZER;
static double bound(double x,double low,double high) { return fmax(low,fmin(high,x)); }
static double num(const cJSON *v,const char *key,double fallback,double low,double high) {
    const cJSON *n=cJSON_GetObjectItemCaseSensitive(v,key);
    return cJSON_IsNumber(n) && isfinite(n->valuedouble)?bound(n->valuedouble,low,high):fallback;
}
static uint32_t uint32_js(double x) {
    if (!isfinite(x) || x==0) return 0;
    double n=fmod(trunc(x),4294967296.0); if (n<0) n+=4294967296.0; return (uint32_t)n;
}
static double int32_js(double x) { uint32_t u=uint32_js(x); return u>=2147483648u?(double)u-4294967296.0:u; }
static double noise(NativeSynthState *s) { s->random=s->random*1664525u+1013904223u; return s->random/2147483648.0-1; }

int native_synth_sound(const char *sound) {
    if (!sound) return 0;
    if (!strcmp(sound,"zzfx") || !strncmp(sound,"z_",2)) return 1;
    return !strncmp(sound,"bytebeat",8) && (!sound[8] || sound[8]==':')?2:0;
}
int native_synth_effect(const NativeSynth *p) { return p && p->effect; }
int native_synth_stereo(const NativeSynth *p) { return p && p->kind!=1; }
int native_synth_enveloped(const NativeSynth *p) { return p && (p->kind==1 || (p->kind==3 && !p->effect && p->envelope)); }
double native_synth_tail(const NativeSynth *p) { return p && p->kind==1?-1:p?p->tail:0; }
void native_synth_retain(const NativeSynth *p) { if (p) atomic_fetch_add(&((NativeSynth *)p)->refs,1); }
void native_synth_release(const NativeSynth *p) { if (p) atomic_fetch_sub(&((NativeSynth *)p)->refs,1); }

static int prepare_zzfx(NativeSynth *p,const cJSON *v,double frequency,double duration,const char *sound) {
    static const char *const controls[]={"zrand","attack","release","curve","slide","deltaSlide","pitchJump","pitchJumpTime",
        "lfo","znoise","zmod","zcrush","zdelay","sustain","decay","tremolo"};
    static const int slots[]={1,3,5,7,8,9,10,11,12,13,14,15,16,17,18,19};
    static const double defaults[]={0,0,.1,1,0,0,0,0,0,0,0,0,0,.8,0,0};
    p->kind=1; p->z[0]=.25; p->z[2]=frequency;
    for (int i=0;i<16;i++) p->z[slots[i]]=num(v,controls[i],defaults[i],-20000,20000);
    p->z[4]=fmax(0,duration-p->z[3]-p->z[18]);
    if (!strcmp(sound,"z_triangle")) p->z[6]=1;
    else if (!strcmp(sound,"z_sawtooth")) p->z[6]=2;
    else if (!strcmp(sound,"z_tan")) p->z[6]=3;
    else if (!strcmp(sound,"z_noise")) p->z[6]=4;
    else if (!strcmp(sound,"z_square")) p->z[7]=0;
    else if (strcmp(sound,"z_sine") && strcmp(sound,"zzfx")) return 0;
    const cJSON *raw=cJSON_GetObjectItemCaseSensitive(v,"zzfx");
    if (raw) {
        if (!cJSON_IsArray(raw) || cJSON_GetArraySize(raw)>20) return 0;
        const double initial[]={1,.05,220,0,0,.1,0,1,0,0,0,0,0,0,0,0,0,1,0,0};
        memcpy(p->z,initial,sizeof(initial));
        for (int i=0;i<cJSON_GetArraySize(raw);i++) {
            const cJSON *x=cJSON_GetArrayItem(raw,i);
            if (cJSON_IsNull(x)) continue;
            if (!cJSON_IsNumber(x) || !isfinite(x->valuedouble) || fabs(x->valuedouble)>20000) return 0;
            p->z[i]=x->valuedouble;
        }
    }
    double seconds=0;
    const int time_slots[]={3,4,5,16,18};
    for (int i=0;i<5;i++) { if (p->z[time_slots[i]]<0) return 0; seconds+=p->z[time_slots[i]]; }
    if (seconds>30 || p->z[16]>2 || p->z[1]<0 || p->z[1]>1 || p->z[6]<0 || p->z[6]>4 ||
        p->z[7]<0 || p->z[7]>8 || p->z[12]<0 || p->z[15]<0 || p->z[15]>1) return 0;
    p->delay_samples=(unsigned)ceil(p->z[16]*SR)+1;
    return 1;
}

const NativeSynth *native_synth_prepare(const cJSON *value,double frequency,double duration) {
    const cJSON *descriptor=cJSON_GetObjectItemCaseSensitive(value,"_nativeProgram");
    const cJSON *sound=cJSON_GetObjectItemCaseSensitive(value,"s");
    const char *s=cJSON_IsString(sound)?sound->valuestring:"";
    NativeSynth temp={0};
    char *key=NULL;
    if (native_synth_sound(s)==1) {
        if (!prepare_zzfx(&temp,value,frequency,duration,s)) return NULL;
        cJSON *params=cJSON_CreateDoubleArray(temp.z,20);
        if (!params) return NULL;
        key=cJSON_PrintUnformatted(params); cJSON_Delete(params);
    } else {
        if (!cJSON_IsObject(descriptor)) return NULL;
        const cJSON *kind=cJSON_GetObjectItemCaseSensitive(descriptor,"kind");
        if (!cJSON_IsString(kind)) return NULL;
        temp.kind=!strcmp(kind->valuestring,"bytebeat")?2:!strcmp(kind->valuestring,"graph")?3:0;
        const cJSON *ops=cJSON_GetObjectItemCaseSensitive(descriptor,"ops");
        if (!temp.kind || !cJSON_IsArray(ops) || !(temp.count=cJSON_GetArraySize(ops)) || temp.count>NS_OPS) return NULL;
        const cJSON *result=cJSON_GetObjectItemCaseSensitive(descriptor,"result");
        if (!cJSON_IsNumber(result) || result->valuedouble!=floor(result->valuedouble) || result->valueint<0 || result->valueint>=temp.count) return NULL;
        temp.result=result->valueint; temp.effect=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(descriptor,"effect"));
        const cJSON *offset=cJSON_GetObjectItemCaseSensitive(descriptor,"offset");
        if (offset) {
            if (!cJSON_IsNumber(offset) || !isfinite(offset->valuedouble) || fabs(offset->valuedouble)>1e12) return NULL;
            temp.restart=1; temp.offset=floor(offset->valuedouble);
        }
        for (int i=0;i<temp.count;i++) {
            NativeOp *op=&temp.ops[i]; const cJSON *entry=cJSON_GetArrayItem(ops,i);
            const cJSON *name=cJSON_GetObjectItemCaseSensitive(entry,"op"), *ins=cJSON_GetObjectItemCaseSensitive(entry,"ins"),
                *v=cJSON_GetObjectItemCaseSensitive(entry,"value");
            if (!cJSON_IsString(name) || !cJSON_IsArray(ins) || (op->count=cJSON_GetArraySize(ins))>NS_INPUTS ||
                !cJSON_IsNumber(v) || !isfinite(v->valuedouble)) return NULL;
            op->type=-1; op->value=v->valuedouble;
            for (int j=0;j<N_TYPES;j++) if (!strcmp(name->valuestring,names[j])) { op->type=j; break; }
            if (op->type<0) return NULL;
            for (int j=0;j<op->count;j++) {
                const cJSON *ref=cJSON_GetArrayItem(ins,j);
                if (!cJSON_IsNumber(ref) || !isfinite(ref->valuedouble) || ref->valuedouble!=floor(ref->valuedouble) ||
                    ref->valuedouble<0 || ref->valuedouble>=temp.count || (ref->valuedouble>=i && !(op->type==N_DELAY && j==0))) return NULL;
                op->in[j]=(unsigned)ref->valueint;
            }
            if (op->type==N_DELAY) {
                double seconds=op->count>1 && temp.ops[op->in[1]].type==N_CONST?temp.ops[op->in[1]].value:2;
                if (seconds<0 || seconds>2) return NULL;
                op->delay_at=temp.delay_samples; op->delay_size=(unsigned)ceil(seconds*SR)+2;
                temp.delay_samples+=op->delay_size;
                if (temp.delay_samples>NS_DELAY_MAX) return NULL;
            }
        }
        for (int i=0;i<temp.count;i++) if (temp.ops[i].type==N_ADSR) {
            const NativeOp *op=&temp.ops[i]; temp.envelope=1;
            double release=op->count>4?
                (temp.ops[op->in[4]].type==N_CONST?temp.ops[op->in[4]].value:30):.1;
            temp.tail=fmax(temp.tail,bound(release,0,30));
        }
        temp.tail+=temp.delay_samples/SR;
        key=cJSON_PrintUnformatted(descriptor);
    }
    if (!key || strlen(key)>65536) { free(key); return NULL; }
    pthread_mutex_lock(&cache_lock);
    for (unsigned i=0;i<cache_count;i++) if (cache[i].program->kind==temp.kind && !strcmp(cache[i].key,key)) {
        const NativeSynth *found=cache[i].program; native_synth_retain(found); pthread_mutex_unlock(&cache_lock); free(key); return found;
    }
    unsigned slot=cache_count;
    if (slot==NS_CACHE) for (unsigned i=0;i<cache_count;i++) if (!atomic_load(&cache[i].program->refs)) { slot=i; break; }
    NativeSynth *program=slot<NS_CACHE?malloc(sizeof(*program)):NULL;
    if (program) {
        *program=temp; atomic_init(&program->refs,1);
        if (slot<cache_count) { free(cache[slot].key); free(cache[slot].program); } else cache_count++;
        cache[slot].key=key; cache[slot].program=program;
    }
    pthread_mutex_unlock(&cache_lock);
    if (!program) free(key);
    return program;
}

NativeSynthState *native_synth_state_new(const NativeSynth *p) {
    if (!p) return NULL;
    size_t bytes=sizeof(NativeSynthState)+(size_t)p->delay_samples*sizeof(float);
    size_t before=atomic_fetch_add(&state_bytes,bytes);
    if (before>NS_STATE_BUDGET || bytes>NS_STATE_BUDGET-before) { atomic_fetch_sub(&state_bytes,bytes); return NULL; }
    NativeSynthState *s=calloc(1,bytes);
    if (!s) { atomic_fetch_sub(&state_bytes,bytes); return NULL; }
    s->program=p; s->bytes=bytes; s->random=atomic_fetch_add(&seed,1)*2654435761u; s->jump=1;
    native_synth_retain(p);
    if (p->kind==1) {
        memcpy(s->z,p->z,sizeof(s->z));
        s->z[2]*=(1+s->z[1]*noise(s))*TAU/SR;
        s->z[8]*=500*TAU/(SR*SR); s->start_frequency=s->z[2]; s->start_slide=s->z[8];
        s->z[3]=s->z[3]*SR+9;
        const int times[]={4,5,11,12,16,18}; for (int i=0;i<6;i++) s->z[times[i]]*=SR;
        s->z[12]=trunc(s->z[12]); s->z[9]*=500*TAU/(SR*SR*SR); s->z[14]*=TAU/SR; s->z[10]*=TAU/SR;
        s->length=(unsigned)(s->z[3]+s->z[4]+s->z[5]+s->z[16]+s->z[18]);
    }
    return s;
}
void native_synth_state_free(NativeSynthState *s) { if (s) { native_synth_release(s->program); atomic_fetch_sub(&state_bytes,s->bytes); free(s); } }

static double zzfx_sample(NativeSynthState *s) {
    double *z=s->z, i=s->frame, value=s->last;
    unsigned crush=(unsigned)(z[15]*100);
    if (!crush || ++s->crush%crush==0) {
        double t=s->phase;
        value=z[6]>3?sin(pow(fmod(t,TAU),3)):z[6]>2?bound(tan(t),-1,1):
            z[6]>1?1-fmod(fmod(2*t/TAU,2)+2,2):z[6]>0?1-4*fabs(floor(t/TAU+.5)-t/TAU):sin(t);
        double env=i<z[3]?i/z[3]:i<z[3]+z[18]?1-(i-z[3])/z[18]*(1-z[17]):
            i<z[3]+z[18]+z[4]?z[17]:i<s->length-z[16]?(s->length-i-z[16])/z[5]*z[17]:0;
        value=(z[12]?1-z[19]+z[19]*sin(TAU*i/z[12]):1)*(value>0?1:-1)*pow(fabs(value),z[7])*z[0]*env;
        if (z[16]) {
            double delayed=0;
            if (z[16]<=i) delayed=s->delay[(unsigned)(i-z[16])%s->program->delay_samples];
            value=value/2+(i<s->length-z[16]?1:(s->length-i)/z[16])*delayed/2;
        }
    }
    double f=(z[2]+=(z[8]+=z[9]))*cos(z[14]*s->modulation_time++);
    s->phase+=f-f*z[13]*(1-fmod((sin(i)+1)*1e9,2));
    if (s->jump && ++s->jump>z[11]) { z[2]+=z[10]; s->start_frequency+=z[10]; s->jump=0; }
    if (z[12] && ++s->repeat%(unsigned)z[12]==0) { z[2]=s->start_frequency; z[8]=s->start_slide; if (!s->jump) s->jump=1; }
    s->last=isfinite(value)?value:0;
    s->delay[s->frame%s->program->delay_samples]=(float)s->last; s->frame++;
    return s->last;
}

static double get(const NativeOp *op,const NativeSynthState *s,int index,double fallback) {
    return index<op->count?s->reg[op->in[index]]:fallback;
}
static double blep(double t,double dt) {
    dt=fabs(dt); if (dt<=0) return 0;
    if (t<dt) { t/=dt; return t+t-t*t-1; }
    if (t>1-dt) { t=(t-1)/dt; return t*t+t+t+1; } return 0;
}

int native_synth_process(NativeSynthState *s,double age,double gate,double frequency,double origin,
                         double input_l,double input_r,double *left,double *right) {
    *left=*right=0; if (!s) return 0;
    const NativeSynth *p=s->program;
    if (p->kind==1) { if (s->frame>=s->length) return 0; *left=*right=zzfx_sample(s); return 1; }
    if (p->kind==2) {
        if (!s->byte_started) {
            /* Match the reference worklet's currentTime <= begin guard and
               128-frame quantum. bbst resets phase, not its block boundary. */
            s->byte_wait=128-(uint32_js(floor(origin*SR+.5))&127u); s->byte_started=1;
        }
        if (s->frame<s->byte_wait) { s->frame++; return 1; }
    }
    s->outputs[0]=s->outputs[1]=0;
    for (int i=0;i<p->count;i++) {
        const NativeOp *op=&p->ops[i];
        double a=get(op,s,0,0), b=get(op,s,1,0), c=get(op,s,2,0), out=0, *m=s->memory[i];
        uint32_t ua=uint32_js(a), ub=uint32_js(b); unsigned shift=ub&31;
        switch (op->type) {
        case N_CONST: out=op->value; break;
        case N_TIME: out=age; break;
        case N_GATE: out=age<gate?1:0; break;
        case N_AUDIO: out=input_l; break;
        case N_BYTE_TIME:
            /* The worklet reads float32 AudioParam frequency and divides its
               frame counter before multiplying. Bitwise programs amplify
               even tiny differences in this arithmetic. */
            out=((p->restart?0:origin*SR)+s->frame-s->byte_wait)/(SR/256)*(double)(float)frequency+p->offset;
            break;
        case N_NAN: out=NAN; break;
        case N_PLUS: case N_ADD: out=a+b; break;
        case N_MINUS: case N_SUB: out=a-b; break;
        case N_TIMES: case N_MUL: out=a*b; break;
        case N_DIVIDE: case N_DIV: out=a/b; break;
        case N_REMAINDER: case N_MOD: out=fmod(a,b); break;
        case N_POWER: case N_POW: out=pow(a,b); break;
        case N_BITAND: out=int32_js(ua&ub); break;
        case N_BITOR: out=int32_js(ua|ub); break;
        case N_BITXOR: out=int32_js(ua^ub); break;
        case N_SHL: out=int32_js(ua<<shift); break;
        case N_SHR: { uint32_t x=ua>>shift; if (shift && (ua&0x80000000u)) x|=UINT32_MAX<<(32-shift); out=int32_js(x); break; }
        case N_USHR: out=ua>>shift; break;
        case N_LESS: case N_LT: out=a<b; break; case N_LE: out=a<=b; break;
        case N_GREATER: case N_GT: out=a>b; break; case N_GE: out=a>=b; break;
        case N_EQ: case N_STRICTEQ: out=a==b; break; case N_NE: case N_STRICTNE: out=a!=b; break;
        case N_LOGAND: out=a && !isnan(a)?b:a; break; case N_LOGOR: out=a && !isnan(a)?a:b; break;
        case N_POS: out=a; break; case N_NEG: out=-a; break; case N_BITNOT: out=int32_js(~ua); break;
        case N_LOGNOT: case N_NOT: out=!a || isnan(a); break;
        case N_EXP: out=exp(a); break; case N_LOG: out=log(a); break; case N_LOG2: out=log2(a); break; case N_LOG10: out=log10(a); break;
        case N_SIN: out=sin(a); break; case N_COS: out=cos(a); break; case N_TAN: out=tan(a); break;
        case N_ASIN: out=asin(a); break; case N_ACOS: out=acos(a); break; case N_ATAN: out=atan(a); break;
        case N_ABS: out=fabs(a); break; case N_ROUND: out=floor(a+.5); break;
        case N_FLOOR: out=floor(a); break; case N_CEIL: out=ceil(a); break; case N_TRUNC: out=trunc(a); break;
        case N_SIGN: out=isnan(a)?NAN:a>0?1:a<0?-1:0; break; case N_SQRT: out=sqrt(a); break;
        case N_MIN: case N_MAX:
            out=a; for (int j=1;j<op->count;j++) {
                double operand=get(op,s,j,0);
                out=isnan(out) || isnan(operand)?NAN:op->type==N_MIN?fmin(out,operand):fmax(out,operand);
            } break;
        case N_CLAMP: case N_CLIP: out=bound(a,get(op,s,1,-1),get(op,s,2,1)); break;
        case N_RANGE: out=b+(a+1)*.5*(c-b); break;
        case N_REMAP: out=get(op,s,3,0)+(a-b)/(c-b)*(get(op,s,4,1)-get(op,s,3,0)); break;
        case N_MIDI: out=440*pow(2,(a-69)/12); break;
        case N_BOOL: out=a!=0; break; case N_AND: out=a!=0 && b!=0; break; case N_OR: out=a!=0 || b!=0; break; case N_XOR: out=(a!=0)!=(b!=0); break;
        case N_IF: out=a && !isnan(a)?b:c; break;
        case N_THRU: case N_POLY: case N_EXIT: out=a; break;
        case N_OUTPUT: if (b>=0 && b<=1 && b==floor(b)) { s->outputs[(int)b]+=a; s->sources[(int)b]=s->outputs[(int)b]; } out=a; break;
        case N_SRC: out=a>=0 && a<16?s->sources[(int)a]:0; break;
        case N_SINE:
            if (b>0 && m[1]<=0) m[0]=0;
            out=sin(TAU*fmod(m[0]+c,1)); m[0]=fmod(m[0]+bound(a,-SR*.49,SR*.49)/SR,1); m[1]=b; break;
        case N_SAW: case N_ZAW: case N_TRI: case N_PULSE: {
            double dt=bound(a,-SR*.49,SR*.49)/SR, phase=m[0]; if (phase<0) phase+=1;
            if (op->type==N_TRI) { phase=fmod(phase+dt+1,1); out=1-4*fabs(phase-.5); }
            else if (op->type==N_PULSE) { double pw=bound(get(op,s,1,.5),0,1); out=phase<pw?1:-1; out+=blep(phase,dt)-blep(fmod(phase-pw+1,1),dt); }
            else out=phase*2-1-(op->type==N_SAW?blep(phase,dt):0);
            m[0]=op->type==N_TRI?phase:fmod(phase+dt+1,1); break;
        }
        case N_NOISE: case N_LCG: if (get(op,s,0,1)!=0) m[0]=noise(s); out=m[0]; break;
        case N_BROWN: m[0]=(m[0]+.02*noise(s))/1.02; out=m[0]*3.5; break;
        case N_PINK: m[0]=.98*m[0]+.02*noise(s); out=m[0]*5; break;
        case N_DUST: out=noise(s)>1-2*bound(a,0,SR)/SR?1:0; break;
        case N_IMPULSE: out=s->frame==0 || m[0]+a/SR>=1?1:0; m[0]=fmod(m[0]+a/SR,1); break;
        case N_CLOCK: m[0]=fmod(m[0]+a*.4/SR,1); out=m[0]<.5?1:-1; break;
        case N_CLOCKDIV: if ((a>0)!=(m[0]>0) && ++m[1]>=fmax(1,b)) { m[1]=0; m[2]=!m[2]; } m[0]=a; out=m[2]?-1:1; break;
        case N_ADSR: {
            double att=get(op,s,1,.02), dec=get(op,s,2,.1), sus=get(op,s,3,.2), rel=get(op,s,4,.1);
            if (a>0 && m[0]<=0) { m[1]=age; m[2]=m[4]; }
            if (a<=0 && m[0]>0) { m[3]=age; m[2]=m[4]; }
            if (a>0) { double t=age-m[1]; out=t<att?m[2]+(1-m[2])*t/fmax(1e-9,att):
                t<att+dec?1-(1-sus)*(t-att)/fmax(1e-9,dec):sus; }
            else out=m[2]*fmax(0,1-(age-m[3])/fmax(1e-9,rel));
            m[0]=a; m[4]=out; break;
        }
        case N_DELAY: {
            double frames=bound(b*SR,1,op->delay_size-1), position=fmod((double)s->frame-frames,op->delay_size);
            if (position<0) position+=op->delay_size;
            unsigned at=(unsigned)position; double f=position-at;
            out=s->delay[op->delay_at+at]*(1-f)+s->delay[op->delay_at+(at+1)%op->delay_size]*f; break;
        }
        case N_HOLD: if (b>0 && m[1]<=0) m[0]=a; m[1]=b; out=m[0]; break;
        case N_LAG: case N_SLIDE: m[0]+=(a-m[0])/fmax(1,b*(op->type==N_LAG?4410:1000)); out=m[0]; break;
        case N_SLEW: m[0]+=bound(a-m[0],-get(op,s,2,1)/SR,get(op,s,1,1)/SR); out=m[0]; break;
        case N_FILTER: case N_BPF: {
            double f=pow(.5,(1-fmin(b,1))/.125), r=pow(.5,(fmax(c,0)+.125)/.125), damping=1-r*f;
            m[0]=damping*m[0]-f*m[1]+f*a; m[1]=damping*m[1]+f*m[0];
            out=op->type==N_FILTER?m[1]:m[0]; break;
        }
        case N_FOLD: { double x=a*(fmax(0,b)+1)*.25+.25; out=4*(fabs(x-floor(x+.5))-.25); break; }
        case N_DISTORT: { double k=2*bound(b,0,.999)/(1-bound(b,0,.999)); out=(1+k)*a/(1+k*fabs(a)); break; }
        case N_SEQ: if (a>0 && m[0]<=0 && s->frame) m[1]++; m[0]=a; out=op->count>1?get(op,s,1+(int)fmod(m[1],op->count-1),0):0; break;
        case N_PICK: out=op->count>1?get(op,s,1+(int)bound(floor(a),0,op->count-2),0):0; break;
        case N_TRIG: out=a>0 && m[0]<=0?1:0; m[0]=a; break;
        default: break;
        }
        s->reg[i]=p->kind==2?out:isfinite(out)?bound(out,-1e6,1e6):0;
    }
    for (int i=0;i<p->count;i++) if (p->ops[i].type==N_DELAY) {
        const NativeOp *op=&p->ops[i];
        s->delay[op->delay_at+s->frame%op->delay_size]=(float)get(op,s,0,0);
    }
    if (p->kind==2) *left=*right=((uint32_js(s->reg[p->result])&255)/127.5-1)*.2;
    else { *left=s->outputs[0]; *right=s->outputs[1]; }
    (void)input_r;
    s->frame++; return 1;
}
