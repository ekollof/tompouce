#ifndef RATIONAL_H
#define RATIONAL_H
#include <stdint.h>
#include <quickjs.h>

/* Reduced rational. s is ±1, n is non-negative and d is positive. */
typedef struct Rational {
    int64_t s, n, d;
} Rational;

enum {
    RATIONAL_ADD = 0,
    RATIONAL_SUB,
    RATIONAL_MUL,
    RATIONAL_DIV,
    RATIONAL_COMPARE,
    RATIONAL_EQUALS,
    RATIONAL_MOD,
    RATIONAL_GCD,
    RATIONAL_LCM,
    RATIONAL_FLOOR,
    RATIONAL_CEIL,
    RATIONAL_OPS
};

/* 0 on success. -1 when the denominator is zero or the reduced result
   does not fit in int64; callers then use the original bigint method. */
int rational_add(Rational a, Rational b, Rational *out);
int rational_sub(Rational a, Rational b, Rational *out);
int rational_mul(Rational a, Rational b, Rational *out);
int rational_div(Rational a, Rational b, Rational *out);
int rational_mod(Rational a, Rational b, Rational *out);
int rational_mod_self(Rational a, Rational *out);
int rational_gcd(Rational a, Rational b, Rational *out);
int rational_lcm(Rational a, Rational b, Rational *out);
int rational_floor(Rational a, Rational *out);
int rational_ceil(Rational a, Rational *out);
int rational_compare(Rational a, Rational b, int *out);

typedef struct RationalHost RationalHost;
int rational_bind(JSContext *ctx, RationalHost **host, JSCFunctionMagic *method);
void rational_free(JSContext *ctx, RationalHost *host);
JSValue rational_dispatch(JSContext *ctx, RationalHost *host, int magic,
                          JSValueConst this_val, int argc, JSValueConst *argv);
#endif
