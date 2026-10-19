/* Summarize long JavaScript numeric suffixes before the audio callback.
   No source-language parser or JS context is used here. */
#include "common.h"
#include "duck_onset.h"
#include "js_number.h"
#include <float.h>

_Static_assert(sizeof(double)==sizeof(uint64_t) && DBL_MANT_DIG==53 && DBL_MAX_EXP==1024,
               "Duck numeric conversion requires IEEE binary64");

/* Every finite binary64 rounding boundary has fewer than 800 significant
   decimal digits. Preserve those digits plus a sticky nonzero remainder to
   distinguish exact ties, even for subnormals and enormous zero runs. */
enum { DIGITS=800,TOKENS=8,CACHE=256,RADIX_DIGITS=1024 };
typedef struct {
    uint32_t count,zeros,last;
    char head[DIGITS],significant[DIGITS];
} Digits;
typedef struct { int kind; Digits digits; } Token;
struct DuckOnsetText {
    unsigned count,invalid,radix,radix_overflow,trailing;
    char radix_text[RADIX_DIGITS+4];
    Token tokens[TOKENS];
};
static struct { uint64_t hash; DuckOnsetText text; } cache[CACHE];
static unsigned used;
static pthread_mutex_t cache_lock=PTHREAD_MUTEX_INITIALIZER;
static const DuckOnsetText invalid_text={.invalid=1};

static const DuckOnsetText *intern(const DuckOnsetText *text) {
    uint64_t hash=UINT64_C(14695981039346656037);
    const unsigned char *bytes=(const unsigned char *)text;
    for (size_t i=0;i<sizeof(*text);i++) hash=(hash^bytes[i])*UINT64_C(1099511628211);
    pthread_mutex_lock(&cache_lock);
    for (unsigned i=0;i<used;i++) if (cache[i].hash==hash && !memcmp(&cache[i].text,text,sizeof(*text))) {
        pthread_mutex_unlock(&cache_lock); return &cache[i].text;
    }
    if (used==CACHE) {pthread_mutex_unlock(&cache_lock);return NULL;}
    unsigned at=used++; cache[at].hash=hash; cache[at].text=*text;
    pthread_mutex_unlock(&cache_lock); return &cache[at].text;
}

static int digit_value(unsigned char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return 99;
}

const DuckOnsetText *duck_onset_prepare(const char *text) {
    if (!text) return &invalid_text;
    size_t length=strlen(text);
    if (length>UINT32_MAX) return &invalid_text;
    const char *end=text+length,*p=text;
    DuckOnsetText result={0};
    /* A zero time prefix can turn a suffix into an unsigned radix literal.
       More significant bits than binary64 can hold imply infinity, regardless
       of the subsequently appended attack. Leading zeros cost no storage. */
    if (*p=='x' || *p=='X' || *p=='o' || *p=='O' || *p=='b' || *p=='B') {
        result.radix=(*p=='x'||*p=='X')?16:(*p=='o'||*p=='O')?8:2;
        result.radix_text[0]='0'; result.radix_text[1]=*p++;
        unsigned count=0,any=0,nonzero=0;
        while (p<end && digit_value((unsigned char)*p)<(int)result.radix) {
            unsigned digit=digit_value((unsigned char)*p); any=1;
            if (digit || nonzero) {
                nonzero=1;
                if (count<RADIX_DIGITS) result.radix_text[2+count]=*p;
                count++;
            }
            p++;
        }
        if (js_number_spaces(p,end)!=end) return &invalid_text;
        result.trailing=p!=end;
        unsigned limit=result.radix==16?256:result.radix==8?342:1024;
        result.radix_overflow=count>limit;
        if (!count && any) result.radix_text[2]='0';
        return intern(&result);
    }
    while (p<end) {
        if (result.count==TOKENS) return &invalid_text;
        Token *token=&result.tokens[result.count++];
        if (*p>='0' && *p<='9') {
            token->kind='0'; Digits *digits=&token->digits; digits->last=UINT32_MAX;
            const char *begin=p;
            while (p<end && *p>='0' && *p<='9') {
                uint32_t index=(uint32_t)(p-begin);
                if (*p!='0') digits->last=index;
                if (index==digits->zeros && *p=='0') digits->zeros++;
                if (index<DIGITS) digits->head[index]=*p;
                if (index>=digits->zeros && index-digits->zeros<DIGITS)
                    digits->significant[index-digits->zeros]=*p;
                p++;
            }
            digits->count=(uint32_t)(p-begin);
        } else {
            const char *spaces=js_number_spaces(p,end);
            if (spaces!=p) {token->kind=' ';p=spaces;}
            else if (*p=='.'||*p=='e'||*p=='E'||*p=='+'||*p=='-') token->kind=*p++;
            else return &invalid_text;
        }
    }
    return intern(&result);
}

enum { INTEGER,FRACTION,EXPONENT,EXP_SIGN,EXP_DIGITS,SPACE,BAD };
typedef struct {
    int state,negative,sticky;
    int64_t significant,fraction,exponent;
    unsigned kept;
    char digits[DIGITS];
} Decimal;

static void digits(Decimal *number,const Digits *run) {
    if (number->state==EXPONENT || number->state==EXP_SIGN || number->state==EXP_DIGITS) {
        number->state=EXP_DIGITS;
        uint32_t skip=number->exponent?0:run->zeros;
        if (run->count-skip>13) number->exponent=INT64_C(1000000000000);
        else for (uint32_t i=skip;i<run->count;i++) {
            unsigned digit=skip?run->significant[i-skip]-'0':run->head[i]-'0';
            number->exponent=number->exponent*10+digit;
            if (number->exponent>INT64_C(1000000000000)) number->exponent=INT64_C(1000000000000);
        }
        return;
    }
    if (number->state!=INTEGER && number->state!=FRACTION) {number->state=BAD;return;}
    if (number->state==FRACTION) number->fraction+=run->count;
    uint32_t skip=number->significant?0:run->zeros;
    uint32_t count=run->count-skip;
    unsigned take=count<DIGITS-number->kept?count:DIGITS-number->kept;
    memcpy(number->digits+number->kept,skip?run->significant:run->head,take);
    number->kept+=take; number->significant+=count;
    if (run->last!=UINT32_MAX && run->last>=skip+take) number->sticky=1;
}

static void character(Decimal *number,int c) {
    if (c=='.' && number->state==INTEGER) number->state=FRACTION;
    else if ((c=='e'||c=='E') && (number->state==INTEGER || number->state==FRACTION)) number->state=EXPONENT;
    else if ((c=='+'||c=='-') && number->state==EXPONENT) {number->negative=c=='-';number->state=EXP_SIGN;}
    else if (c==' ' && (number->state==INTEGER || number->state==FRACTION || number->state==EXP_DIGITS || number->state==SPACE)) number->state=SPACE;
    else number->state=BAD;
}

static void short_text(Decimal *number,const char *text) {
    while (*text) {
        if (*text>='0' && *text<='9') {
            /* Time and attack formatter outputs have at most 25 characters. */
            Digits run={.last=UINT32_MAX};
            while (*text>='0' && *text<='9') {
                unsigned i=run.count++;
                if (*text!='0') run.last=i;
                if (i==run.zeros && *text=='0') run.zeros++;
                run.head[i]=*text;
                if (i>=run.zeros) run.significant[i-run.zeros]=*text;
                text++;
            }
            digits(number,&run);
        } else character(number,*text++);
    }
}

/* QuickJS's decimal reader intentionally truncates to about 128 bits and
   does not guarantee correctly rounded subnormals. Use a fixed-size integer
   ratio here; the canonical decimal's order bounds both operands below 4096
   bits. Power-of-two literals still use QuickJS's exact radix reader. */
enum { WORDS=128 };
typedef struct { uint32_t words[WORDS]; unsigned used; } Integer;
static void normalize(Integer *n) {while (n->used && !n->words[n->used-1]) n->used--;}
static int multiply(Integer *n,uint32_t factor,uint32_t add) {
    uint64_t carry=add;
    for (unsigned i=0;i<n->used;i++) {
        uint64_t next=(uint64_t)n->words[i]*factor+carry;
        n->words[i]=(uint32_t)next; carry=next>>32;
    }
    if (carry) {if(n->used==WORDS)return 0;n->words[n->used++]=(uint32_t)carry;}
    return 1;
}
static int shift(Integer *n,unsigned bits) {
    if (!n->used) return 1;
    unsigned whole=bits/32,part=bits%32;
    if (n->used+whole+(part!=0)>WORDS) return 0;
    memmove(n->words+whole,n->words,n->used*sizeof(n->words[0]));
    memset(n->words,0,whole*sizeof(n->words[0])); n->used+=whole;
    uint32_t carry=0;
    if (part) for (unsigned i=whole;i<n->used;i++) {
        uint32_t next=n->words[i]>>(32-part);
        n->words[i]=(n->words[i]<<part)|carry; carry=next;
    }
    if (carry) n->words[n->used++]=carry;
    return 1;
}
static unsigned bits(const Integer *n) {
    if (!n->used) return 0;
    unsigned count=(n->used-1)*32; uint32_t top=n->words[n->used-1];
    while (top) {count++;top>>=1;} return count;
}
static int compare(const Integer *a,const Integer *b) {
    if (a->used!=b->used) return a->used>b->used?1:-1;
    for (unsigned i=a->used;i;i--) if (a->words[i-1]!=b->words[i-1]) return a->words[i-1]>b->words[i-1]?1:-1;
    return 0;
}
static void subtract(Integer *a,const Integer *b) {
    uint64_t borrow=0;
    for (unsigned i=0;i<a->used;i++) {
        uint64_t sub=(i<b->used?b->words[i]:0)+borrow,previous=a->words[i];
        a->words[i]=(uint32_t)(previous-sub); borrow=previous<sub;
    }
    normalize(a);
}
static void halve(Integer *n) {
    uint32_t carry=0;
    for (unsigned i=n->used;i;i--) {
        uint32_t next=n->words[i-1]&1;
        n->words[i-1]=(n->words[i-1]>>1)|(carry<<31); carry=next;
    }
    normalize(n);
}
static double decimal_binary(const char *text,unsigned length,int64_t exponent) {
    int64_t order=exponent+length;
    if (order>309) return INFINITY;
    if (order< -323) return 0;
    Integer numerator={0},denominator={.words={1},.used=1};
    for (unsigned at=0;at<length;) {
        unsigned take=length-at>9?9:length-at;
        uint32_t factor=1,add=0;
        for (unsigned i=0;i<take;i++) {factor*=10;add=add*10+(unsigned)(text[at++]-'0');}
        if (!multiply(&numerator,factor,add)) return NAN;
    }
    Integer *scaled=exponent>=0?&numerator:&denominator;
    unsigned power=(unsigned)(exponent>=0?exponent:-exponent);
    while (power) {
        unsigned take=power>9?9:power; uint32_t factor=1;
        for (unsigned i=0;i<take;i++) factor*=10;
        if (!multiply(scaled,factor,0)) return NAN;
        power-=take;
    }
    int binary=(int)bits(&numerator)-(int)bits(&denominator);
    Integer boundary=binary>=0?denominator:numerator;
    if (!shift(&boundary,(unsigned)(binary>=0?binary:-binary))) return NAN;
    if ((binary>=0?compare(&numerator,&boundary):compare(&boundary,&denominator))<0) binary--;
    if (binary>1023) return INFINITY;
    int quantum=binary-52; if (quantum< -1074) quantum=-1074;
    if (!shift(quantum>=0?&denominator:&numerator,(unsigned)(quantum>=0?quantum:-quantum))) return NAN;
    int difference=(int)bits(&numerator)-(int)bits(&denominator);
    uint64_t mantissa=0;
    if (difference>53) return NAN;
    if (difference>=0) {
        boundary=denominator;
        if (!shift(&boundary,(unsigned)difference)) return NAN;
        for (int bit=difference;bit>=0;bit--) {
            if (compare(&numerator,&boundary)>=0) {subtract(&numerator,&boundary);mantissa|=UINT64_C(1)<<bit;}
            halve(&boundary);
        }
    }
    if (!shift(&numerator,1)) return NAN;
    int rounding=compare(&numerator,&denominator);
    if (rounding>0 || (rounding==0 && (mantissa&1))) mantissa++;
    /* Build bits directly: audio dependencies can enable denormal flushing
       in the calling thread. Numeric control conversion must preserve them. */
    uint64_t encoded=mantissa;
    if (quantum!= -1074) {
        if (mantissa==UINT64_C(1)<<53) {mantissa>>=1;quantum++;}
        encoded=((uint64_t)(quantum+1075)<<52)|(mantissa&((UINT64_C(1)<<52)-1));
    }
    double result; memcpy(&result,&encoded,sizeof(result)); return result;
}

static double value(const Decimal *number) {
    if (number->state==BAD || number->state==EXPONENT || number->state==EXP_SIGN) return NAN;
    if (!number->significant) return 0;
    char text[DIGITS+1]; memcpy(text,number->digits,number->kept);
    unsigned length=number->kept;
    if (number->sticky) text[length++]='1';
    else while (length>1 && text[length-1]=='0') length--;
    int64_t exponent=(number->negative?-number->exponent:number->exponent)+
        number->significant-(int64_t)length-number->fraction;
    return decimal_binary(text,length,exponent);
}

void duck_onset_times(const DuckOnsetText *text,double base,double attack,double *minimum,double *end) {
    *minimum=*end=NAN;
    if (!text || text->invalid || !isfinite(base) || base<0) return;
    char first[32],last[32]; JSDTOATempMem temporary;
    js_dtoa(first,base,10,0,JS_DTOA_FORMAT_FREE,&temporary);
    js_dtoa(last,attack,10,0,JS_DTOA_FORMAT_FREE,&temporary);
    if (text->radix) {
        if (base!=0) return;
        if (text->radix_overflow) {
            *minimum=INFINITY;
            if (text->trailing) return;
            for (const char *p=last;*p;p++) if (digit_value((unsigned char)*p)>=(int)text->radix) return;
            *end=INFINITY; return;
        }
        char joined[RADIX_DIGITS+40]; size_t length=strlen(text->radix_text);
        memcpy(joined,text->radix_text,length+1); *minimum=js_number_parse(joined);
        if (text->trailing) return;
        size_t suffix=strlen(last); memcpy(joined+length,last,suffix+1);
        *end=js_number_parse(joined); return;
    }
    Decimal number={0}; short_text(&number,first);
    for (unsigned i=0;i<text->count;i++) {
        if (text->tokens[i].kind=='0') digits(&number,&text->tokens[i].digits);
        else character(&number,text->tokens[i].kind);
    }
    *minimum=value(&number); short_text(&number,last); *end=value(&number);
}
