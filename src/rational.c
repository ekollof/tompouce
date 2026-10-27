#include "rational.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__SIZEOF_INT128__)
#error native fractions need a 128-bit integer type
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
typedef __int128 ri128;
typedef unsigned __int128 ru128;
#pragma GCC diagnostic pop

struct RationalHost {
    JSValue proto;
    JSValue methods[RATIONAL_OPS];
    JSAtom atom_s, atom_n, atom_d;
    JSValue span_proto, span_constructor, hap_constructor, span_methods[RATIONAL_SPAN_OPS];
    JSAtom atom_begin, atom_end;
    JSValue hap_proto, hap_methods[2];
    JSAtom hap_fields[5];
    JSAtom atom_span,atom_cycles,atom_whole_cycle;
    JSValue native_cycles,native_whole_cycle;
    JSValue object_proto,guard_owners[7],guard_values[7];
    JSAtom guard_atoms[7];
    int guard_getters[7];
};

static JSValue make_span(JSContext *ctx, RationalHost *host, Rational begin, Rational end);

static ri128 ri128_max(void) {
    return (ri128)(((ru128)1 << 127) - 1);
}

static ri128 ri128_min(void) {
    return (ri128)((ru128)1 << 127);
}

static ri128 igcd(ri128 a, ri128 b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    if (!a) return b;
    if (!b) return a;
    while (1) {
        a %= b;
        if (!a) return b;
        b %= a;
        if (!b) return a;
    }
}

static int rational_norm(ri128 num, ri128 den, Rational *out) {
    if (!den) return -1;
    if (den < 0) {
        if (num == ri128_min() || den == ri128_min()) return -1;
        num = -num;
        den = -den;
    }
    int sign = 1;
    if (num < 0) {
        if (num == ri128_min()) return -1;
        sign = -1;
        num = -num;
    }
    /* Clock products normally remain small. Avoid software int128 division
       in their gcd and reduction, retaining the wide path for large products. */
    if (num<=INT64_MAX && den<=INT64_MAX) {
        uint64_t n=(uint64_t)num,d=(uint64_t)den,a=n,b=d;
        while (b) { uint64_t rem=a%b; a=b; b=rem; }
        out->s=sign; out->n=(int64_t)(n/a); out->d=(int64_t)(d/a);
        return 0;
    }
    ri128 g = igcd(num, den);
    if (!g) return -1;
    num /= g;
    den /= g;
    if (num > INT64_MAX || den > INT64_MAX) return -1;
    out->s = sign;
    out->n = (int64_t)num;
    out->d = (int64_t)den;
    return 0;
}

static int add_overflow(ri128 a, ri128 b, ri128 *out) {
    if ((b > 0 && a > ri128_max() - b) || (b < 0 && a < ri128_min() - b)) return -1;
    *out = a + b;
    return 0;
}

static ri128 term(Rational a, int64_t scale) {
    return (ri128)a.s * a.n * scale;
}

int rational_add(Rational a, Rational b, Rational *out) {
    ri128 num;
    if (add_overflow(term(a, b.d), term(b, a.d), &num)) return -1;
    return rational_norm(num, (ri128)a.d * b.d, out);
}

int rational_sub(Rational a, Rational b, Rational *out) {
    b.s = -b.s;
    return rational_add(a, b, out);
}

int rational_mul(Rational a, Rational b, Rational *out) {
    return rational_norm((ri128)a.s * b.s * a.n * b.n, (ri128)a.d * b.d, out);
}

int rational_div(Rational a, Rational b, Rational *out) {
    if (!b.n) return -1;
    return rational_norm((ri128)a.s * b.s * a.n * b.d, (ri128)a.d * b.n, out);
}

int rational_mod(Rational a, Rational b, Rational *out) {
    if (!b.n || !a.d) return -1;
    ri128 span = (ri128)b.d * a.n;
    ri128 cycle = (ri128)b.n * a.d;
    return rational_norm(((ri128)a.s * span) % cycle, (ri128)b.d * a.d, out);
}

int rational_mod_self(Rational a, Rational *out) {
    if (!a.d) return -1;
    return rational_norm(((ri128)a.s * a.n) % a.d, 1, out);
}

int rational_gcd(Rational a, Rational b, Rational *out) {
    return rational_norm(igcd(a.n, b.n) * igcd(a.d, b.d), (ri128)a.d * b.d, out);
}

int rational_lcm(Rational a, Rational b, Rational *out) {
    if (!a.n && !b.n) {
        out->s = 1; out->n = 0; out->d = 1;
        return 0;
    }
    ri128 g = igcd(a.n, b.n);
    if (!g) return -1;
    return rational_norm((a.n / g) * (ri128)b.n, igcd(a.d, b.d), out);
}

static int whole(Rational a, int ceil, Rational *out) {
    if (!a.d) return -1;
    ri128 q = ((ri128)a.s * a.n) / a.d;
    ri128 rem = a.n % a.d;
    if (rem && (ceil ? a.s >= 0 : a.s < 0)) q += ceil ? 1 : -1;
    return rational_norm(q, 1, out);
}

int rational_floor(Rational a, Rational *out) { return whole(a, 0, out); }
int rational_ceil(Rational a, Rational *out) { return whole(a, 1, out); }

int rational_compare(Rational a, Rational b, int *out) {
    ri128 left = term(a, b.d), right = term(b, a.d);
    *out = (left > right) - (left < right);
    return 0;
}

static const char *const rational_names[RATIONAL_OPS] = {
    "add", "sub", "mul", "div", "compare", "equals", "mod", "gcd", "lcm", "floor", "ceil",
    "sam", "nextSam", "cyclePos", "wholeCycle", "lt", "lte", "gt", "gte", "eq", "ne", "min", "max"
};
static const uint8_t rational_lengths[RATIONAL_OPS] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1,
    0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1};

static void clear_exception(JSContext *ctx) {
    JSValue exception = JS_GetException(ctx);
    JS_FreeValue(ctx, exception);
}

static int read_i64(JSContext *ctx, JSValueConst value, int64_t *out) {
    int tag = JS_VALUE_GET_TAG(value);
    if (tag == JS_TAG_INT) { *out = JS_VALUE_GET_INT(value); return 1; }
    if (tag == JS_TAG_SHORT_BIG_INT) { *out = JS_VALUE_GET_SHORT_BIG_INT(value); return 1; }
    if (tag == JS_TAG_BIG_INT) {
        int64_t exact;
        if (JS_ToBigInt64(ctx, &exact, value)) { clear_exception(ctx); return 0; }
        JSValue back = JS_NewBigInt64(ctx, exact);
        if (JS_IsException(back)) { clear_exception(ctx); return 0; }
        int same = JS_StrictEq(ctx, back, value);
        JS_FreeValue(ctx, back);
        if (!same) return 0;
        *out = exact;
        return 1;
    }
    if (!JS_IsNumber(value)) return 0;
    double number;
    if (JS_ToFloat64(ctx, &number, value)) { clear_exception(ctx); return 0; }
    if (!isfinite(number) || number < (double)INT64_MIN || number >= 9223372036854775808.0) return 0;
    int64_t exact = (int64_t)number;
    if ((double)exact != number) return 0;
    *out = exact;
    return 1;
}

static int abs_i64(int64_t value, int64_t *out) {
    if (value == INT64_MIN) return 0;
    *out = value < 0 ? -value : value;
    return 1;
}

static int normalize_parts(int64_t num, int64_t den, Rational *out) {
    if (!den || num == INT64_MIN || den == INT64_MIN) return 0;
    int sign = (num < 0) != (den < 0) ? -1 : 1;
    if (!abs_i64(num, &out->n) || !abs_i64(den, &out->d) || !out->d) return 0;
    out->s = sign;
    return 1;
}

static void release(JSContext *ctx, JSValue a, JSValue b, JSValue c) {
    JS_FreeValue(ctx, a); JS_FreeValue(ctx, b); JS_FreeValue(ctx, c);
}

static int read_object(JSContext *ctx, const RationalHost *host, JSValueConst value, Rational *out) {
    JSValue num = JS_GetProperty(ctx, value, host->atom_n);
    JSValue den = JS_GetProperty(ctx, value, host->atom_d);
    if (JS_IsException(num) || JS_IsException(den)) {
        release(ctx, num, den, JS_UNDEFINED); clear_exception(ctx); return 0;
    }
    if (JS_IsUndefined(num) || JS_IsUndefined(den)) {
        release(ctx, num, den, JS_UNDEFINED);
        int array = JS_IsArray(ctx, value);
        if (array < 0) { clear_exception(ctx); return 0; }
        if (!array) return 0;
        num = JS_GetPropertyUint32(ctx, value, 0);
        den = JS_GetPropertyUint32(ctx, value, 1);
        if (JS_IsException(num) || JS_IsException(den) || JS_IsUndefined(den)) {
            release(ctx, num, den, JS_UNDEFINED); clear_exception(ctx); return 0;
        }
        int64_t n, d;
        int ok = read_i64(ctx, num, &n) && read_i64(ctx, den, &d) && normalize_parts(n, d, out);
        release(ctx, num, den, JS_UNDEFINED);
        return ok;
    }
    JSValue sign = JS_GetProperty(ctx, value, host->atom_s);
    int64_t n, d, s = 1;
    if (JS_IsException(sign)) { release(ctx, num, den, sign); clear_exception(ctx); return 0; }
    int ok = read_i64(ctx, num, &n) && read_i64(ctx, den, &d);
    if (ok && !JS_IsUndefined(sign)) ok = read_i64(ctx, sign, &s);
    else if (JS_IsUndefined(sign)) s = 1;
    release(ctx, num, den, sign);
    if (!ok) return 0;
    ri128 folded = (ri128)n * s;
    if (folded > INT64_MAX || folded < INT64_MIN) return 0;
    return normalize_parts((int64_t)folded, d, out);
}

static int read_fraction(JSContext *ctx, const RationalHost *host, JSValueConst value, Rational *out) {
    if (!JS_IsObject(value)) return 0;
    return read_object(ctx, host, value, out);
}

static int read_number(JSContext *ctx, JSValueConst value, Rational *out) {
    int64_t integer;
    return read_i64(ctx,value,&integer) && normalize_parts(integer,1,out);
}

static int read_operand(JSContext *ctx, const RationalHost *host, int argc, JSValueConst *argv, Rational *out) {
    if (argc <= 0) { out->s = 1; out->n = 0; out->d = 1; return 1; }
    if (argc >= 2) {
        int64_t n, d;
        if (!read_i64(ctx, argv[0], &n) || !read_i64(ctx, argv[1], &d)) return 0;
        return normalize_parts(n, d, out);
    }
    if (read_number(ctx,argv[0],out)) return 1;
    if (JS_IsObject(argv[0])) return read_object(ctx, host, argv[0], out);
    return 0;
}

static int places_are_unit(JSContext *ctx, int argc, JSValueConst *argv) {
    if (argc <= 0 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) return 1;
    int64_t places;
    return read_i64(ctx, argv[0], &places) && !places;
}

static JSValue call_original(JSContext *ctx, RationalHost *host, int magic,
                             JSValueConst this_val, int argc, JSValueConst *argv) {
    if (magic < 0 || magic >= RATIONAL_OPS) return JS_ThrowTypeError(ctx, "unknown fraction operation");
    return JS_Call(ctx, host->methods[magic], this_val, argc, argv);
}

static JSValue make_fraction(JSContext *ctx, RationalHost *host, Rational value) {
    JSValue obj = JS_NewObjectProto(ctx, host->proto);
    if (JS_IsException(obj)) return obj;
    if (JS_SetProperty(ctx, obj, host->atom_s, JS_NewBigInt64(ctx, value.s)) < 0 ||
        JS_SetProperty(ctx, obj, host->atom_n, JS_NewBigInt64(ctx, value.n)) < 0 ||
        JS_SetProperty(ctx, obj, host->atom_d, JS_NewBigInt64(ctx, value.d)) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

JSValue rational_dispatch(JSContext *ctx, RationalHost *host, int magic,
                          JSValueConst this_val, int argc, JSValueConst *argv) {
    Rational left, right, result;
    if (!host) return JS_ThrowTypeError(ctx, "fraction runtime is unavailable");
    if (!read_fraction(ctx, host, this_val, &left))
        return call_original(ctx, host, magic, this_val, argc, argv);
    if (magic==RATIONAL_WHOLE_CYCLE) {
        Rational end;
        if (!rational_floor(left,&result) && !rational_add(result,(Rational){1,1,1},&end))
            return make_span(ctx,host,result,end);
        return call_original(ctx,host,magic,this_val,argc,argv);
    }
    if (magic==RATIONAL_SAM || magic==RATIONAL_NEXT_SAM || magic==RATIONAL_CYCLE_POS) {
        int status=rational_floor(left,&result);
        if (!status && magic==RATIONAL_NEXT_SAM)
            status=rational_add(result,(Rational){1,1,1},&result);
        if (!status && magic==RATIONAL_CYCLE_POS) status=rational_sub(left,result,&result);
        return !status?make_fraction(ctx,host,result):call_original(ctx,host,magic,this_val,argc,argv);
    }
    if (magic == RATIONAL_FLOOR || magic == RATIONAL_CEIL) {
        if (!places_are_unit(ctx, argc, argv))
            return call_original(ctx, host, magic, this_val, argc, argv);
        int ok = magic == RATIONAL_CEIL ? rational_ceil(left, &result) : rational_floor(left, &result);
        return ok == 0 ? make_fraction(ctx, host, result) : call_original(ctx, host, magic, this_val, argc, argv);
    }
    if (magic == RATIONAL_MOD && argc <= 0) {
        return rational_mod_self(left, &result) == 0 ? make_fraction(ctx, host, result)
            : call_original(ctx, host, magic, this_val, argc, argv);
    }
    int operand_argc=magic>=RATIONAL_LT && argc>1?1:argc;
    if (!read_operand(ctx, host, operand_argc, argv, &right))
        return call_original(ctx, host, magic, this_val, argc, argv);
    if (magic == RATIONAL_COMPARE || magic == RATIONAL_EQUALS || magic>=RATIONAL_LT) {
        int order;
        if (rational_compare(left, right, &order))
            return call_original(ctx, host, magic, this_val, argc, argv);
        switch (magic) {
        case RATIONAL_COMPARE: return JS_NewInt32(ctx,order);
        case RATIONAL_EQUALS: case RATIONAL_EQ: return JS_NewBool(ctx,order==0);
        case RATIONAL_NE: return JS_NewBool(ctx,order!=0);
        case RATIONAL_LT: return JS_NewBool(ctx,order<0);
        case RATIONAL_LTE: return JS_NewBool(ctx,order<=0);
        case RATIONAL_GT: return JS_NewBool(ctx,order>0);
        case RATIONAL_GTE: return JS_NewBool(ctx,order>=0);
        case RATIONAL_MIN: case RATIONAL_MAX:
            if ((magic==RATIONAL_MIN && order<0) || (magic==RATIONAL_MAX && order>0))
                return JS_DupValue(ctx,this_val);
            return argc>0?JS_DupValue(ctx,argv[0]):JS_UNDEFINED;
        default: return call_original(ctx,host,magic,this_val,argc,argv);
        }
    }
    int ok = -1;
    switch (magic) {
    case RATIONAL_ADD: ok = rational_add(left, right, &result); break;
    case RATIONAL_SUB: ok = rational_sub(left, right, &result); break;
    case RATIONAL_MUL: ok = rational_mul(left, right, &result); break;
    case RATIONAL_DIV: ok = rational_div(left, right, &result); break;
    case RATIONAL_MOD: ok = rational_mod(left, right, &result); break;
    case RATIONAL_GCD: ok = rational_gcd(left, right, &result); break;
    case RATIONAL_LCM: ok = rational_lcm(left, right, &result); break;
    default: return call_original(ctx, host, magic, this_val, argc, argv);
    }
    return ok == 0 ? make_fraction(ctx, host, result) : call_original(ctx, host, magic, this_val, argc, argv);
}

/* Only ordinary pinned clock objects take span shortcuts. Accessors, proxies,
   subclasses and out-of-range fractions retain the original JS operations. */
static int pinned_object(JSContext *ctx, JSValueConst obj, JSValueConst expected) {
    if (!JS_IsObject(obj) || JS_GetClassID(obj)!=JS_GetClassID(expected)) return 0;
    JSValue proto=JS_GetPrototype(ctx,obj);
    int same=JS_StrictEq(ctx,proto,expected);
    JS_FreeValue(ctx,proto);
    return same;
}

static int own_data(JSContext *ctx, JSValueConst obj, JSAtom atom, JSValue *out) {
    JSPropertyDescriptor desc;
    if (JS_GetOwnProperty(ctx,&desc,obj,atom)!=1) return 0;
    int data=!(desc.flags&JS_PROP_GETSET);
    *out=data?desc.value:JS_UNDEFINED;
    if (!data) JS_FreeValue(ctx,desc.value);
    JS_FreeValue(ctx,desc.getter); JS_FreeValue(ctx,desc.setter);
    return data;
}

static int clock_fraction(JSContext *ctx, RationalHost *host, JSValueConst value, Rational *out) {
    if (!pinned_object(ctx,value,host->proto)) return 0;
    JSValue n=JS_UNDEFINED,d=JS_UNDEFINED,s=JS_UNDEFINED;
    int64_t num,den,sign;
    int ok=own_data(ctx,value,host->atom_n,&n) && own_data(ctx,value,host->atom_d,&d) &&
        own_data(ctx,value,host->atom_s,&s) && read_i64(ctx,n,&num) && read_i64(ctx,d,&den) && read_i64(ctx,s,&sign);
    release(ctx,n,d,s);
    if (!ok) return 0;
    ri128 folded=(ri128)num*sign;
    return folded>=INT64_MIN && folded<=INT64_MAX && normalize_parts((int64_t)folded,den,out);
}

static int clock_span(JSContext *ctx, RationalHost *host, JSValueConst value, Rational *begin, Rational *end) {
    if (!pinned_object(ctx,value,host->span_proto)) return 0;
    JSValue b=JS_UNDEFINED,e=JS_UNDEFINED;
    int ok=own_data(ctx,value,host->atom_begin,&b) && own_data(ctx,value,host->atom_end,&e) &&
        clock_fraction(ctx,host,b,begin) && clock_fraction(ctx,host,e,end);
    JS_FreeValue(ctx,b); JS_FreeValue(ctx,e);
    return ok;
}

JSValue rational_create(JSContext *ctx, RationalHost *host, JSValueConst value) {
    if (!host) return JS_NULL; /* Bundle initialization still uses the parser. */
    Rational result;
    if (JS_IsObject(value)) {
        if (!clock_fraction(ctx,host,value,&result)) return JS_HasException(ctx)?JS_EXCEPTION:JS_NULL;
    } else {
        if (!read_number(ctx,value,&result)) return JS_NULL;
    }
    if (rational_norm((ri128)result.s*result.n,result.d,&result)) return JS_NULL;
    return make_fraction(ctx,host,result);
}

static JSValue make_span(JSContext *ctx, RationalHost *host, Rational begin, Rational end) {
    Rational b,e;
    if (rational_norm((ri128)begin.s*begin.n,begin.d,&b) || rational_norm((ri128)end.s*end.n,end.d,&e))
        return JS_ThrowRangeError(ctx,"clock span out of range");
    JSValue obj=JS_NewObjectProto(ctx,host->span_proto);
    if (JS_IsException(obj)) return obj;
    JSValue bv=make_fraction(ctx,host,b);
    if (JS_IsException(bv)) { JS_FreeValue(ctx,obj); return bv; }
    if (JS_SetProperty(ctx,obj,host->atom_begin,bv)<0) {
        JS_FreeValue(ctx,obj); return JS_EXCEPTION;
    }
    JSValue ev=make_fraction(ctx,host,e);
    if (JS_IsException(ev)) { JS_FreeValue(ctx,obj); return ev; }
    if (JS_SetProperty(ctx,obj,host->atom_end,ev)<0) {
        JS_FreeValue(ctx,obj); return JS_EXCEPTION;
    }
    return obj;
}

static int clock_order(Rational a, Rational b) {
    int order; rational_compare(a,b,&order); return order;
}

JSValue rational_hap_dispatch(JSContext *ctx, RationalHost *host, int magic,
                             JSValueConst self, int argc, JSValueConst *argv) {
    if (!host || magic<0 || magic>1) return JS_ThrowTypeError(ctx,"native hap unavailable");
    if (!argc || !JS_IsFunction(ctx,argv[0]) || !pinned_object(ctx,self,host->hap_proto))
        return JS_Call(ctx,host->hap_methods[magic],self,argc,argv);
    JSValue fields[5];
    /* Check data descriptors before invoking callbacks. Subsequent reads retain
       upstream ordering, including mutations made by the callback. */
    for (int i=0;i<4;i++) {
        JSValue value;
        if (!own_data(ctx,self,host->hap_fields[i],&value)) {
            if (JS_HasException(ctx)) return JS_EXCEPTION;
            return JS_Call(ctx,host->hap_methods[magic],self,argc,argv);
        }
        JS_FreeValue(ctx,value);
    }
    fields[0]=JS_GetProperty(ctx,self,host->hap_fields[0]);
    if (magic==0) {
        fields[1]=JS_GetProperty(ctx,self,host->hap_fields[1]);
        JSValue value=JS_GetProperty(ctx,self,host->hap_fields[2]);
        fields[2]=JS_Call(ctx,argv[0],JS_UNDEFINED,1,&value);
        JS_FreeValue(ctx,value);
    } else {
        if (JS_ToBool(ctx,fields[0])) {
            JSValue whole=JS_Call(ctx,argv[0],JS_UNDEFINED,1,&fields[0]);
            JS_FreeValue(ctx,fields[0]); fields[0]=whole;
        } else { JS_FreeValue(ctx,fields[0]); fields[0]=JS_UNDEFINED; }
        fields[1]=JS_UNDEFINED; fields[2]=JS_UNDEFINED;
        if (!JS_IsException(fields[0])) {
            JSValue part=JS_GetProperty(ctx,self,host->hap_fields[1]);
            fields[1]=JS_IsException(part)?JS_EXCEPTION:JS_Call(ctx,argv[0],JS_UNDEFINED,1,&part);
            JS_FreeValue(ctx,part);
            if (!JS_IsException(fields[1])) fields[2]=JS_GetProperty(ctx,self,host->hap_fields[2]);
        }
    }
    fields[3]=JS_UNDEFINED; fields[4]=JS_FALSE;
    JSValue out=JS_EXCEPTION;
    if (!JS_HasException(ctx)) {
        fields[3]=JS_GetProperty(ctx,self,host->hap_fields[3]);
        if (JS_IsUndefined(fields[3])) { JS_FreeValue(ctx,fields[3]); fields[3]=JS_NewObject(ctx); }
        if (!JS_IsException(fields[3])) {
            out=JS_NewObjectProto(ctx,host->hap_proto);
            if (!JS_IsException(out)) for (int i=0;i<5;i++) {
                if (JS_SetProperty(ctx,out,host->hap_fields[i],JS_DupValue(ctx,fields[i]))<0) {
                    JS_FreeValue(ctx,out); out=JS_EXCEPTION; break;
                }
            }
        }
    }
    for (int i=0;i<5;i++) JS_FreeValue(ctx,fields[i]);
    return out;
}

JSValue rational_common_keys(JSContext *ctx,RationalHost *host,int argc,JSValueConst *argv) {
    if (!host || argc!=2 || !pinned_object(ctx,argv[0],host->object_proto) ||
        !pinned_object(ctx,argv[1],host->object_proto)) return JS_NULL;
    for (int i=0;i<7;i++) {
        JSPropertyDescriptor desc;
        if (JS_GetOwnProperty(ctx,&desc,host->guard_owners[i],host->guard_atoms[i])!=1)
            return JS_HasException(ctx)?JS_EXCEPTION:JS_NULL;
        int getter=!!(desc.flags&JS_PROP_GETSET);
        int same=getter==host->guard_getters[i] &&
            JS_StrictEq(ctx,getter?desc.getter:desc.value,host->guard_values[i]);
        JS_FreeValue(ctx,desc.value);JS_FreeValue(ctx,desc.getter);JS_FreeValue(ctx,desc.setter);
        if (!same) return JS_NULL;
    }
    JSPropertyEnum *keys[2]={NULL,NULL}; uint32_t counts[2]={0,0};
    JSValue out=JS_EXCEPTION;
    for (int i=0;i<2;i++) if (JS_GetOwnPropertyNames(ctx,&keys[i],&counts[i],argv[i],JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY)<0) goto done;
    if (counts[0]>4096 || counts[1]>4096 || (uint64_t)counts[0]*counts[1]>65536) { out=JS_NULL;goto done; }
    out=JS_NewArray(ctx);
    if (JS_IsException(out)) goto done;
    uint32_t at=0;
    for (uint32_t a=0;a<counts[0];a++) for (uint32_t b=0;b<counts[1];b++) if (keys[0][a].atom==keys[1][b].atom) {
        JSValue key=JS_AtomToString(ctx,keys[0][a].atom);
        if (JS_IsException(key) || JS_DefinePropertyValueUint32(ctx,out,at++,key,JS_PROP_C_W_E)<0) {
            JS_FreeValue(ctx,out);out=JS_EXCEPTION;goto done;
        }
        break;
    }
done:
    for (int i=0;i<2;i++) { for (uint32_t k=0;k<counts[i];k++) JS_FreeAtom(ctx,keys[i][k].atom);js_free(ctx,keys[i]); }
    return out;
}

JSValue rational_pure(JSContext *ctx,RationalHost *host,int argc,JSValueConst *argv) {
    if (!host || argc!=2 || !JS_IsObject(argv[0]) ||
        JS_GetClassID(argv[0])!=JS_GetClassID(host->hap_proto) ||
        !pinned_object(ctx,host->hap_proto,host->object_proto) ||
        !pinned_object(ctx,host->span_proto,host->object_proto) ||
        !pinned_object(ctx,host->proto,host->object_proto)) return JS_NULL;
    JSValue span=JS_UNDEFINED,method=JS_UNDEFINED;
    Rational begin,end;
    if (!own_data(ctx,argv[0],host->atom_span,&span) || !clock_span(ctx,host,span,&begin,&end)) goto unsupported;
    JSPropertyDescriptor desc;
    int found=JS_GetOwnProperty(ctx,&desc,span,host->atom_cycles);
    if (found==1) { JS_FreeValue(ctx,desc.value);JS_FreeValue(ctx,desc.getter);JS_FreeValue(ctx,desc.setter); }
    if (found!=0) goto unsupported;
    found=JS_GetOwnProperty(ctx,&desc,host->span_proto,host->atom_cycles);
    if (found!=1) goto unsupported;
    int standard=(desc.flags&JS_PROP_GETSET) && JS_StrictEq(ctx,desc.getter,host->native_cycles);
    JS_FreeValue(ctx,desc.value);JS_FreeValue(ctx,desc.getter);JS_FreeValue(ctx,desc.setter);
    if (!standard || !own_data(ctx,host->proto,host->atom_whole_cycle,&method) ||
        !JS_StrictEq(ctx,method,host->native_whole_cycle)) goto unsupported;
    for (int i=3;i<=5;i++) {
        JSPropertyDescriptor guard;
        if (JS_GetOwnProperty(ctx,&guard,host->guard_owners[i],host->guard_atoms[i])!=1) goto unsupported;
        int getter=!!(guard.flags&JS_PROP_GETSET);
        int same=getter==host->guard_getters[i] &&
            JS_StrictEq(ctx,getter?guard.getter:guard.value,host->guard_values[i]);
        JS_FreeValue(ctx,guard.value);JS_FreeValue(ctx,guard.getter);JS_FreeValue(ctx,guard.setter);
        if (!same) goto unsupported;
    }
    for (int i=0;i<5;i++) if (JS_HasProperty(ctx,host->hap_proto,host->hap_fields[i])!=0) goto unsupported;
    if (JS_HasProperty(ctx,host->span_proto,host->atom_begin)!=0 ||
        JS_HasProperty(ctx,host->span_proto,host->atom_end)!=0) goto unsupported;
    JSAtom fraction_fields[]={host->atom_n,host->atom_d,host->atom_s};
    for (int i=0;i<3;i++) {
        JSValue initial;
        if (!own_data(ctx,host->proto,fraction_fields[i],&initial)) goto unsupported;
        JS_FreeValue(ctx,initial);
    }
    Rational first,last;
    if (rational_floor(begin,&first) || rational_floor(end,&last) ||
        (ri128)last.s*last.n-(ri128)first.s*first.n>8190) goto unsupported;
    JS_FreeValue(ctx,span);JS_FreeValue(ctx,method);
    JSValue out=JS_NewArray(ctx);
    if (JS_IsException(out)) return out;
    unsigned count=0;
    while (clock_order(begin,end)<=0) {
        Rational cycle,next;
        if (rational_floor(begin,&cycle) || rational_add(cycle,(Rational){1,1,1},&next)) {
            JS_FreeValue(ctx,out);return JS_NULL;
        }
        Rational finish=clock_order(next,end)>0?end:next;
        if (!clock_order(begin,end)) finish=end;
        JSValue whole=make_span(ctx,host,cycle,next),part=make_span(ctx,host,begin,finish);
        JSValue context=JS_NewObject(ctx),hap=JS_EXCEPTION;
        if (!JS_IsException(whole) && !JS_IsException(part) && !JS_IsException(context)) {
            hap=JS_NewObjectProto(ctx,host->hap_proto);
            if (!JS_IsException(hap)) {
                JSValue fields[]={whole,part,argv[1],context,JS_FALSE};
                for (int i=0;i<5;i++) if (JS_SetProperty(ctx,hap,host->hap_fields[i],JS_DupValue(ctx,fields[i]))<0) {
                    JS_FreeValue(ctx,hap);hap=JS_EXCEPTION;break;
                }
            }
        }
        JS_FreeValue(ctx,whole);JS_FreeValue(ctx,part);JS_FreeValue(ctx,context);
        if (JS_IsException(hap) || JS_DefinePropertyValueUint32(ctx,out,count++,hap,JS_PROP_C_W_E)<0) {
            JS_FreeValue(ctx,out);return JS_EXCEPTION;
        }
        if (!clock_order(finish,end)) return out;
        begin=finish;
    }
    return out;
unsupported:
    JS_FreeValue(ctx,span);JS_FreeValue(ctx,method);
    return JS_HasException(ctx)?JS_EXCEPTION:JS_NULL;
}

JSValue rational_triangle(JSContext *ctx, RationalHost *host, int argc, JSValueConst *argv) {
    Rational begin,end,rate,phase,two={1,2,1};
    int32_t a,b;
    if (!host || argc!=6 || !clock_span(ctx,host,argv[0],&begin,&end) ||
        !clock_fraction(ctx,host,argv[1],&rate) || !clock_fraction(ctx,host,argv[2],&phase) ||
        rate.s<0 || !rate.n || !JS_IsNumber(argv[3]) || !JS_IsNumber(argv[4]) ||
        JS_ToInt32(ctx,&a,argv[3]) || JS_ToInt32(ctx,&b,argv[4]) || a<0 || a>1 || b<0 || b>1)
        return JS_HasException(ctx)?JS_EXCEPTION:JS_NULL;
    /* Operations originate in the adapter, but remain bounded at this boundary. */
    int array=JS_IsArray(ctx,argv[5]);
    if (array!=1) return array<0?JS_EXCEPTION:JS_NULL;
    JSValue length=JS_GetPropertyStr(ctx,argv[5],"length");
    uint32_t count;
    int valid=JS_ToUint32(ctx,&count,length)==0;
    JS_FreeValue(ctx,length);
    if (!valid || count>256) return JS_HasException(ctx)?JS_EXCEPTION:JS_NULL;
    struct { int kind; double k; } ops[256];
    for (uint32_t i=0;i<count;i++) {
        JSValue op=JS_GetPropertyUint32(ctx,argv[5],i);
        if (JS_IsException(op)) return op;
        JSValue type=JS_GetPropertyStr(ctx,op,"t");
        if (JS_IsException(type)) { JS_FreeValue(ctx,op); return type; }
        const char *name=JS_IsString(type)?JS_ToCString(ctx,type):NULL;
        int kind=name?(!strcmp(name,"mul")?0:!strcmp(name,"add")?1:
            !strcmp(name,"bipolar")?2:!strcmp(name,"unipolar")?3:-1):-1;
        JS_FreeCString(ctx,name); JS_FreeValue(ctx,type);
        double k=0;
        if (kind==0 || kind==1) {
            JSValue v=JS_GetPropertyStr(ctx,op,"k");
            if (!JS_IsNumber(v) || JS_ToFloat64(ctx,&k,v) || !isfinite(k)) kind=-1;
            JS_FreeValue(ctx,v);
        }
        JS_FreeValue(ctx,op);
        if (kind<0) return JS_HasException(ctx)?JS_EXCEPTION:JS_NULL;
        ops[i].kind=kind; ops[i].k=k;
    }
    if (rational_mul(begin,rate,&begin) || rational_add(begin,phase,&begin) || rational_mul(begin,two,&begin) ||
        rational_mul(end,rate,&end) || rational_add(end,phase,&end) || rational_mul(end,two,&end) ||
        clock_order(begin,end)>0) return JS_NULL;
    JSValue out=JS_NewArray(ctx);
    if (JS_IsException(out)) return out;
    for (unsigned i=0;i<8192;i++) {
        Rational index,next,half,offset,time,ub,ue;
        if (rational_floor(begin,&index)) goto unsupported;
        int64_t signed_index=index.s*index.n;
        if (signed_index>9007199254740991LL || signed_index< -9007199254740991LL) goto unsupported;
        next=end;
        if (clock_order(begin,end) && (rational_add(index,(Rational){1,1,1},&next))) goto unsupported;
        if (clock_order(next,end)>0) next=end;
        if (rational_div(begin,two,&half) || rational_floor(half,&half) ||
            rational_sub(index,half,&offset) || rational_sub(begin,offset,&time)) goto unsupported;
        double value=fmod((double)time.s*(double)time.n/(double)time.d,1.0);
        int kind=signed_index%2==0?a:b;
        if (kind) value=1-value;
        for (uint32_t j=0;j<count;j++) {
            switch (ops[j].kind) {
            case 0: value*=ops[j].k; break;
            case 1: value+=ops[j].k; break;
            case 2: value=value*2-1; break;
            default: value=(value+1)/2; break;
            }
        }
        if (rational_div(begin,two,&ub) || rational_sub(ub,phase,&ub) || rational_div(ub,rate,&ub) ||
            rational_div(next,two,&ue) || rational_sub(ue,phase,&ue) || rational_div(ue,rate,&ue)) goto unsupported;
        JSValue span=make_span(ctx,host,ub,ue);
        if (JS_IsException(span)) goto failed;
        JSValue context=JS_NewObject(ctx), locations=JS_NewArray(ctx);
        if (JS_IsException(context) || JS_IsException(locations)) {
            JS_FreeValue(ctx,span); JS_FreeValue(ctx,context); JS_FreeValue(ctx,locations); goto failed;
        }
        if (JS_SetPropertyStr(ctx,context,"locations",locations)<0) {
            JS_FreeValue(ctx,span); JS_FreeValue(ctx,context); goto failed;
        }
        JSValue args[]={JS_UNDEFINED,span,JS_NewFloat64(ctx,value),context};
        JSValue hap=JS_CallConstructor(ctx,host->hap_constructor,4,args);
        JS_FreeValue(ctx,span); JS_FreeValue(ctx,context);
        if (JS_IsException(hap)) goto failed;
        if (JS_SetPropertyUint32(ctx,out,i,hap)<0) goto failed;
        if (!clock_order(next,end)) return out;
        begin=next;
    }
unsupported:
    JS_FreeValue(ctx,out); return JS_NULL;
failed:
    JS_FreeValue(ctx,out); return JS_EXCEPTION;
}

JSValue rational_span_dispatch(JSContext *ctx, RationalHost *host, int magic,
                               JSValueConst self, int argc, JSValueConst *argv) {
    if (!host || magic<0 || magic>=RATIONAL_SPAN_OPS) return JS_ThrowTypeError(ctx,"clock span unavailable");
    if (magic==RATIONAL_SPAN_WITH_TIME && argc>0 && JS_IsFunction(ctx,argv[0]) && pinned_object(ctx,self,host->span_proto)) {
        JSValue begin=JS_GetProperty(ctx,self,host->atom_begin);
        if (JS_IsException(begin)) return begin;
        JSValue b=JS_Call(ctx,argv[0],JS_UNDEFINED,1,&begin); JS_FreeValue(ctx,begin);
        if (JS_IsException(b)) return b;
        /* The first callback can change self.end: read it only afterward. */
        JSValue end=JS_GetProperty(ctx,self,host->atom_end);
        if (JS_IsException(end)) { JS_FreeValue(ctx,b); return end; }
        JSValue e=JS_Call(ctx,argv[0],JS_UNDEFINED,1,&end); JS_FreeValue(ctx,end);
        if (JS_IsException(e)) { JS_FreeValue(ctx,b); return e; }
        Rational rb,re;
        JSValue result;
        if (clock_fraction(ctx,host,b,&rb) && clock_fraction(ctx,host,e,&re)) result=make_span(ctx,host,rb,re);
        else if (JS_HasException(ctx)) result=JS_EXCEPTION;
        else { JSValue args[2]={b,e}; result=JS_CallConstructor(ctx,host->span_constructor,2,args); }
        JS_FreeValue(ctx,b); JS_FreeValue(ctx,e); return result;
    }
    Rational begin,end,other_begin,other_end,result;
    if (!clock_span(ctx,host,self,&begin,&end)) goto fallback;
    switch (magic) {
    case RATIONAL_SPAN_INTERSECTION:
        if (!argc || !clock_span(ctx,host,argv[0],&other_begin,&other_end)) goto fallback;
        {
            Rational b=clock_order(begin,other_begin)>0?begin:other_begin;
            Rational e=clock_order(end,other_end)<0?end:other_end;
            int order=clock_order(b,e);
            if (order>0 || (!order && ((!clock_order(b,end) && clock_order(begin,end)<0) ||
                (!clock_order(b,other_end) && clock_order(other_begin,other_end)<0)))) return JS_UNDEFINED;
            return make_span(ctx,host,b,e);
        }
    case RATIONAL_SPAN_CYCLES:
        {
            Rational first,last;
            if (rational_floor(begin,&first) || rational_floor(end,&last)) goto fallback;
            ri128 width=(ri128)last.s*last.n-(ri128)first.s*first.n;
            if (width>8190) goto fallback;
            JSValue spans=JS_NewArray(ctx);
            if (JS_IsException(spans)) return spans;
            unsigned count=0;
            int order=clock_order(begin,end);
            while (order<=0) {
                Rational next;
                if (rational_floor(begin,&next) || rational_add(next,(Rational){1,1,1},&next)) {
                    JS_FreeValue(ctx,spans); goto fallback;
                }
                if (!order || clock_order(next,end)>=0) next=end;
                JSValue span=make_span(ctx,host,begin,next);
                if (JS_IsException(span)) { JS_FreeValue(ctx,spans); return span; }
                if (JS_SetPropertyUint32(ctx,spans,count++,span)<0) { JS_FreeValue(ctx,spans); return JS_EXCEPTION; }
                if (!clock_order(next,end)) break;
                begin=next; order=clock_order(begin,end);
            }
            return spans;
        }
    case RATIONAL_SPAN_DURATION:
        if (rational_sub(end,begin,&result)) goto fallback;
        return make_fraction(ctx,host,result);
    case RATIONAL_SPAN_MIDPOINT:
        if (rational_sub(end,begin,&result) || rational_div(result,(Rational){1,2,1},&result) ||
            rational_add(begin,result,&result)) goto fallback;
        return make_fraction(ctx,host,result);
    case RATIONAL_SPAN_EQUALS:
        if (!argc || !clock_span(ctx,host,argv[0],&other_begin,&other_end)) goto fallback;
        return JS_NewBool(ctx,!clock_order(begin,other_begin) && !clock_order(end,other_end));
    case RATIONAL_SPAN_CYCLE_ARC:
        if (rational_floor(begin,&result) || rational_sub(end,result,&end) || rational_sub(begin,result,&begin)) goto fallback;
        return make_span(ctx,host,begin,end);
    default: break;
    }
fallback:
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    return JS_Call(ctx,host->span_methods[magic],self,argc,argv);
}

int rational_bind(JSContext *ctx, RationalHost **host_out, JSCFunctionMagic *method, JSCFunctionMagic *span_method, JSCFunctionMagic *hap_method) {
    if (host_out) *host_out = NULL;
    RationalHost *host = calloc(1, sizeof(*host));
    if (!host) return 0;
    host->proto = JS_UNDEFINED;
    host->span_proto = JS_UNDEFINED;
    host->span_constructor=JS_UNDEFINED;
    host->hap_constructor=JS_UNDEFINED;
    host->hap_proto=JS_UNDEFINED;
    host->native_cycles=JS_UNDEFINED;host->native_whole_cycle=JS_UNDEFINED;
    host->object_proto=JS_UNDEFINED;
    for (int i=0;i<7;i++) { host->guard_owners[i]=JS_UNDEFINED;host->guard_values[i]=JS_UNDEFINED; }
    for (int i=0;i<2;i++) host->hap_methods[i]=JS_UNDEFINED;
    for (int i=0;i<RATIONAL_SPAN_OPS;i++) host->span_methods[i]=JS_UNDEFINED;
    for (int i = 0; i < RATIONAL_OPS; i++) host->methods[i] = JS_UNDEFINED;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue wrapper = JS_GetPropertyStr(ctx, global, "Fraction");
    JSValue real = JS_IsException(wrapper) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, wrapper, "_original");
    JSValue proto = JS_IsException(real) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, real, "prototype");
    JS_FreeValue(ctx, global); JS_FreeValue(ctx, wrapper); JS_FreeValue(ctx, real);
    if (JS_IsException(proto) || !JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto); free(host); return 0;
    }
    host->proto = proto;
    host->atom_s = JS_NewAtom(ctx, "s");
    host->atom_n = JS_NewAtom(ctx, "n");
    host->atom_d = JS_NewAtom(ctx, "d");
    host->atom_begin=JS_NewAtom(ctx,"begin"); host->atom_end=JS_NewAtom(ctx,"end");
    host->atom_span=JS_NewAtom(ctx,"span");host->atom_cycles=JS_NewAtom(ctx,"spanCycles");
    host->atom_whole_cycle=JS_NewAtom(ctx,"wholeCycle");
    if (!host->atom_s || !host->atom_n || !host->atom_d || !host->atom_begin || !host->atom_end ||
        !host->atom_span || !host->atom_cycles || !host->atom_whole_cycle) {
        rational_free(ctx, host); return 0;
    }
    global=JS_GetGlobalObject(ctx);
    host->hap_constructor=JS_GetPropertyStr(ctx,global,"Hap");
    JSValue span=JS_GetPropertyStr(ctx,global,"TimeSpan");
    host->span_constructor=span;
    host->span_proto=JS_GetPropertyStr(ctx,span,"prototype");
    JS_FreeValue(ctx,global);
    if (!JS_IsObject(host->span_proto) || !JS_IsFunction(ctx,host->hap_constructor)) { rational_free(ctx,host); return 0; }
    host->hap_proto=JS_GetPropertyStr(ctx,host->hap_constructor,"prototype");
    host->object_proto=JS_GetPrototype(ctx,host->hap_proto);
    global=JS_GetGlobalObject(ctx);
    JSValue object=JS_GetPropertyStr(ctx,global,"Object"),array=JS_GetPropertyStr(ctx,global,"Array");
    JSValue array_proto=JS_GetPropertyStr(ctx,array,"prototype"),symbol=JS_GetPropertyStr(ctx,global,"Symbol");
    JSValue species=JS_GetPropertyStr(ctx,symbol,"species");
    const char *guard_names[]={"keys","filter","includes","constructor",NULL,"map","Object"};
    for (int i=0;i<7;i++) {
        host->guard_owners[i]=JS_DupValue(ctx,i==0?object:i==4?array:i==6?global:array_proto);
        host->guard_atoms[i]=i==4?JS_ValueToAtom(ctx,species):JS_NewAtom(ctx,guard_names[i]);
        JSPropertyDescriptor desc;
        if (!host->guard_atoms[i] || JS_GetOwnProperty(ctx,&desc,host->guard_owners[i],host->guard_atoms[i])!=1) {
            JS_FreeValue(ctx,global);JS_FreeValue(ctx,object);JS_FreeValue(ctx,array);JS_FreeValue(ctx,array_proto);
            JS_FreeValue(ctx,symbol);JS_FreeValue(ctx,species);rational_free(ctx,host);return 0;
        }
        host->guard_getters[i]=!!(desc.flags&JS_PROP_GETSET);
        host->guard_values[i]=JS_DupValue(ctx,host->guard_getters[i]?desc.getter:desc.value);
        JS_FreeValue(ctx,desc.value);JS_FreeValue(ctx,desc.getter);JS_FreeValue(ctx,desc.setter);
    }
    JS_FreeValue(ctx,global);JS_FreeValue(ctx,object);JS_FreeValue(ctx,array);JS_FreeValue(ctx,array_proto);
    JS_FreeValue(ctx,symbol);JS_FreeValue(ctx,species);
    static const char *const fields[]={"whole","part","value","context","stateful"};
    for (int i=0;i<5;i++) {
        host->hap_fields[i]=JS_NewAtom(ctx,fields[i]);
        if (!host->hap_fields[i]) { rational_free(ctx,host); return 0; }
    }
    static const char *const hap_names[]={"withValue","withSpan"};
    for (int i=0;i<2;i++) {
        host->hap_methods[i]=JS_GetPropertyStr(ctx,host->hap_proto,hap_names[i]);
        JSValue native=JS_NewCFunctionMagic(ctx,hap_method,hap_names[i],1,JS_CFUNC_generic_magic,i);
        if (!JS_IsFunction(ctx,host->hap_methods[i]) || JS_IsException(native)) {
            JS_FreeValue(ctx,native); rational_free(ctx,host); return 0;
        }
        if (JS_SetPropertyStr(ctx,host->hap_proto,hap_names[i],native)<0) { rational_free(ctx,host); return 0; }
    }
    static const char *const names[]={"intersection","spanCycles","duration","midpoint","equals","cycleArc","withTime"};
    for (int i=0;i<RATIONAL_SPAN_OPS;i++) {
        int getter=i==RATIONAL_SPAN_CYCLES || i==RATIONAL_SPAN_DURATION;
        JSAtom atom=JS_NewAtom(ctx,names[i]);
        if (!atom) { rational_free(ctx,host); return 0; }
        if (getter) {
            JSPropertyDescriptor desc;
            int found=JS_GetOwnProperty(ctx,&desc,host->span_proto,atom);
            if (found!=1) { JS_FreeAtom(ctx,atom); rational_free(ctx,host); return 0; }
            host->span_methods[i]=desc.getter;
            JS_FreeValue(ctx,desc.value); JS_FreeValue(ctx,desc.setter);
        } else host->span_methods[i]=JS_GetProperty(ctx,host->span_proto,atom);
        if (!JS_IsFunction(ctx,host->span_methods[i])) {
            JS_FreeAtom(ctx,atom); rational_free(ctx,host); return 0;
        }
        JSValue native=JS_NewCFunctionMagic(ctx,span_method,names[i],getter?0:1,JS_CFUNC_generic_magic,i);
        if (i==RATIONAL_SPAN_CYCLES) host->native_cycles=JS_DupValue(ctx,native);
        int ok=!JS_IsException(native);
        if (ok) ok=getter?JS_DefinePropertyGetSet(ctx,host->span_proto,atom,native,JS_UNDEFINED,JS_PROP_CONFIGURABLE)>=0:
            JS_SetProperty(ctx,host->span_proto,atom,native)>=0;
        JS_FreeAtom(ctx,atom);
        if (!ok) { rational_free(ctx,host); return 0; }
    }
    for (int i = 0; i < RATIONAL_OPS; i++) {
        host->methods[i] = JS_GetPropertyStr(ctx, proto, rational_names[i]);
        if (JS_IsException(host->methods[i]) || !JS_IsFunction(ctx, host->methods[i])) {
            rational_free(ctx, host); return 0;
        }
        /* SetProperty consumes the function, including when installation fails. */
        JSValue native = JS_NewCFunctionMagic(ctx, method, rational_names[i], rational_lengths[i],
                                              JS_CFUNC_generic_magic, i);
        if (i==RATIONAL_WHOLE_CYCLE) host->native_whole_cycle=JS_DupValue(ctx,native);
        if (JS_IsException(native) || JS_SetPropertyStr(ctx, proto, rational_names[i], native) < 0) {
            rational_free(ctx, host); return 0;
        }
    }
    if (host_out) *host_out = host;
    else rational_free(ctx, host);
    return 1;
}

void rational_free(JSContext *ctx, RationalHost *host) {
    if (!host) return;
    if (ctx) {
        for (int i = 0; i < RATIONAL_OPS; i++) JS_FreeValue(ctx, host->methods[i]);
        JS_FreeValue(ctx, host->proto);
        JS_FreeValue(ctx,host->span_proto);
        JS_FreeValue(ctx,host->span_constructor);
        JS_FreeValue(ctx,host->hap_constructor);
        JS_FreeValue(ctx,host->hap_proto);
        JS_FreeValue(ctx,host->native_cycles);JS_FreeValue(ctx,host->native_whole_cycle);
        JS_FreeValue(ctx,host->object_proto);
        for (int i=0;i<7;i++) {
            JS_FreeValue(ctx,host->guard_owners[i]);JS_FreeValue(ctx,host->guard_values[i]);
            if (host->guard_atoms[i]) JS_FreeAtom(ctx,host->guard_atoms[i]);
        }
        if (host->atom_span) JS_FreeAtom(ctx,host->atom_span);
        if (host->atom_cycles) JS_FreeAtom(ctx,host->atom_cycles);
        if (host->atom_whole_cycle) JS_FreeAtom(ctx,host->atom_whole_cycle);
        for (int i=0;i<2;i++) JS_FreeValue(ctx,host->hap_methods[i]);
        for (int i=0;i<5;i++) if (host->hap_fields[i]) JS_FreeAtom(ctx,host->hap_fields[i]);
        for (int i=0;i<RATIONAL_SPAN_OPS;i++) JS_FreeValue(ctx,host->span_methods[i]);
        if (host->atom_s) JS_FreeAtom(ctx, host->atom_s);
        if (host->atom_n) JS_FreeAtom(ctx, host->atom_n);
        if (host->atom_d) JS_FreeAtom(ctx, host->atom_d);
        if (host->atom_begin) JS_FreeAtom(ctx,host->atom_begin);
        if (host->atom_end) JS_FreeAtom(ctx,host->atom_end);
    }
    free(host);
}
