#include "rational.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>

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
};

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
    return rational_norm((a.n / g) * (ri128)b.n, g * igcd(a.d, b.d), out);
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
    "add", "sub", "mul", "div", "compare", "equals", "mod", "gcd", "lcm", "floor", "ceil"
};
static const uint8_t rational_lengths[RATIONAL_OPS] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1};

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

static int read_operand(JSContext *ctx, const RationalHost *host, int argc, JSValueConst *argv, Rational *out) {
    if (argc <= 0) { out->s = 1; out->n = 0; out->d = 1; return 1; }
    if (argc >= 2) {
        int64_t n, d;
        if (!read_i64(ctx, argv[0], &n) || !read_i64(ctx, argv[1], &d)) return 0;
        return normalize_parts(n, d, out);
    }
    if (read_i64(ctx, argv[0], &out->n)) {
        if (out->n == INT64_MIN) return 0;
        out->s = out->n < 0 ? -1 : 1;
        if (out->n < 0) out->n = -out->n;
        out->d = 1;
        return 1;
    }
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
    if (magic == RATIONAL_FLOOR || magic == RATIONAL_CEIL) {
        if (!places_are_unit(ctx, argc, argv))
            return call_original(ctx, host, magic, this_val, argc, argv);
        int ok = magic == RATIONAL_CEIL ? rational_ceil(left, &result) : rational_floor(left, &result);
        return ok ? make_fraction(ctx, host, result) : call_original(ctx, host, magic, this_val, argc, argv);
    }
    if (magic == RATIONAL_MOD && argc <= 0) {
        return rational_mod_self(left, &result) ? make_fraction(ctx, host, result)
            : call_original(ctx, host, magic, this_val, argc, argv);
    }
    if (!read_operand(ctx, host, argc, argv, &right))
        return call_original(ctx, host, magic, this_val, argc, argv);
    if (magic == RATIONAL_COMPARE || magic == RATIONAL_EQUALS) {
        int order;
        if (rational_compare(left, right, &order))
            return call_original(ctx, host, magic, this_val, argc, argv);
        return magic == RATIONAL_EQUALS ? JS_NewBool(ctx, order == 0) : JS_NewInt32(ctx, order);
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
    return ok ? make_fraction(ctx, host, result) : call_original(ctx, host, magic, this_val, argc, argv);
}

int rational_bind(JSContext *ctx, RationalHost **host_out, JSCFunctionMagic *method) {
    if (host_out) *host_out = NULL;
    RationalHost *host = calloc(1, sizeof(*host));
    if (!host) return 0;
    host->proto = JS_UNDEFINED;
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
    if (!host->atom_s || !host->atom_n || !host->atom_d) {
        rational_free(ctx, host); return 0;
    }
    for (int i = 0; i < RATIONAL_OPS; i++) {
        host->methods[i] = JS_GetPropertyStr(ctx, proto, rational_names[i]);
        if (JS_IsException(host->methods[i]) || !JS_IsFunction(ctx, host->methods[i])) {
            rational_free(ctx, host); return 0;
        }
        /* SetProperty consumes the function, including when installation fails. */
        JSValue native = JS_NewCFunctionMagic(ctx, method, rational_names[i], rational_lengths[i],
                                              JS_CFUNC_generic_magic, i);
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
        if (host->atom_s) JS_FreeAtom(ctx, host->atom_s);
        if (host->atom_n) JS_FreeAtom(ctx, host->atom_n);
        if (host->atom_d) JS_FreeAtom(ctx, host->atom_d);
    }
    free(host);
}
