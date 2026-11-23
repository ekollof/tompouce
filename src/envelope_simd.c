#include "envelope_simd.h"
#include "dsp.h"
#include <math.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

void dsp_envelope4_c(const double age[4], const double gate[4],
    const double attack[4], const double decay[4], const double sustain[4],
    const double release[4], double output[4]) {
    for (int i = 0; i < 4; i++)
        output[i] = dsp_envelope(age[i], gate[i], attack[i], decay[i],
            sustain[i], release[i]);
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,no-fma"),always_inline))
static inline __m256d held(__m256d x, __m256d attack, __m256d decay,
                    __m256d sustain) {
    const __m256d one = _mm256_set1_pd(1.0);
    __m256d attack_value = _mm256_div_pd(x, attack);
    __m256d decay_value = _mm256_sub_pd(one,
        _mm256_div_pd(_mm256_mul_pd(_mm256_sub_pd(one, sustain),
            _mm256_sub_pd(x, attack)), decay));
    __m256d first = _mm256_cmp_pd(x, attack, _CMP_LT_OQ);
    __m256d second = _mm256_cmp_pd(x, _mm256_add_pd(attack, decay),
        _CMP_LT_OQ);
    __m256d value = _mm256_blendv_pd(sustain, decay_value, second);
    return _mm256_blendv_pd(value, attack_value, first);
}

__attribute__((target("avx2,no-fma"),noinline))
void dsp_envelope4_avx2(const double age[4], const double gate[4],
    const double attack[4], const double decay[4], const double sustain[4],
    const double release[4], double output[4]) {
    const __m256d zero = _mm256_setzero_pd();
    const __m256d one = _mm256_set1_pd(1.0);
    __m256d ages = _mm256_loadu_pd(age);
    __m256d gates = _mm256_loadu_pd(gate);
    __m256d attacks = _mm256_loadu_pd(attack);
    __m256d decays = _mm256_loadu_pd(decay);
    __m256d sustains = _mm256_loadu_pd(sustain);
    __m256d releases = _mm256_loadu_pd(release);
    __m256d held_age = held(ages, attacks, decays, sustains);
    __m256d held_gate = held(gates, attacks, decays, sustains);
    __m256d release_elapsed = _mm256_div_pd(_mm256_sub_pd(ages, gates),
        releases);
    __m256d release_value = _mm256_mul_pd(held_gate,
        _mm256_sub_pd(one, release_elapsed));
    __m256d active = _mm256_cmp_pd(ages, zero, _CMP_GE_OQ);
    __m256d holding = _mm256_cmp_pd(ages, gates, _CMP_LE_OQ);
    __m256d releasing = _mm256_and_pd(
        _mm256_cmp_pd(releases, zero, _CMP_GT_OQ),
        _mm256_cmp_pd(ages, _mm256_add_pd(gates, releases), _CMP_LT_OQ));
    __m256d value = _mm256_blendv_pd(zero, release_value, releasing);
    value = _mm256_blendv_pd(value, held_age, holding);
    value = _mm256_blendv_pd(zero, value, active);
    _mm256_storeu_pd(output, value);
}
#endif
