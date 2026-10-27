#include "rational.h"
#include <assert.h>
#include <stdio.h>

static Rational R(int64_t s, int64_t n, int64_t d) {
    Rational r = {s < 0 ? -1 : 1, n < 0 ? -n : n, d < 0 ? -d : d};
    return r;
}

static void same(Rational r, int64_t s, int64_t n, int64_t d) {
    assert(r.s == s && r.n == n && r.d == d);
}

int main(void) {
    Rational out;
    assert(!rational_add(R(1, 1, 6), R(1, 1, 3), &out)); same(out, 1, 1, 2);
    assert(!rational_sub(R(1, 1, 2), R(1, 1, 3), &out)); same(out, 1, 1, 6);
    assert(!rational_sub(R(1, 1, 3), R(1, 1, 2), &out)); same(out, -1, 1, 6);
    assert(!rational_mul(R(1, 2, 3), R(1, 3, 4), &out)); same(out, 1, 1, 2);
    assert(!rational_div(R(1, 2, 3), R(1, 3, 4), &out)); same(out, 1, 8, 9);
    assert(!rational_div(R(-1, 2, 3), R(1, 3, 4), &out)); same(out, -1, 8, 9);
    assert(rational_div(R(1, 1, 2), R(1, 0, 1), &out));
    assert(!rational_floor(R(1, 5, 2), &out)); same(out, 1, 2, 1);
    assert(!rational_floor(R(-1, 5, 2), &out)); same(out, -1, 3, 1);
    assert(!rational_ceil(R(1, 5, 2), &out)); same(out, 1, 3, 1);
    assert(!rational_ceil(R(-1, 5, 2), &out)); same(out, -1, 2, 1);
    assert(!rational_floor(R(-1, 1, 1), &out)); same(out, -1, 1, 1);
    assert(!rational_mod(R(1, 7, 3), R(1, 1, 1), &out)); same(out, 1, 1, 3);
    assert(!rational_mod_self(R(-1, 5, 3), &out)); same(out, -1, 2, 1);
    assert(rational_mod(R(1, 1, 2), R(1, 0, 1), &out));
    assert(!rational_gcd(R(1, 1, 6), R(1, 1, 4), &out)); same(out, 1, 1, 12);
    assert(!rational_lcm(R(1, 5, 8), R(1, 3, 7), &out)); same(out, 1, 15, 1);
    assert(!rational_lcm(R(1, 0, 1), R(1, 0, 1), &out)); same(out, 1, 0, 1);
    assert(!rational_add(R(1, 9999999, 10000000), R(1, 1, 10000000), &out)); same(out, 1, 1, 1);
    assert(rational_mul(R(1, 1, 1LL << 40), R(1, 1, 1LL << 40), &out));
    int order = 0;
    assert(!rational_compare(R(1, 1, 3), R(1, 1, 2), &order) && order < 0);
    assert(!rational_compare(R(1, 2, 4), R(1, 1, 2), &order) && order == 0);
    assert(!rational_compare(R(-1, 1, 2), R(1, 1, 3), &order) && order < 0);
    puts("native rational identities ok");
    return 0;
}
