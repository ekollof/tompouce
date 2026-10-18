#include "harmony.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

static int clamp_midi(int m) {
    if (m < 0) return 0;
    if (m > 127) return 127;
    return m;
}

static int letter_semi(char n) {
    switch (n) {
        case 'c': return 0;
        case 'd': return 2;
        case 'e': return 4;
        case 'f': return 5;
        case 'g': return 7;
        case 'a': return 9;
        case 'b': return 11;
        default: return -1;
    }
}

/* Pitch head: letter + optional #/b + optional octave. *rest is the tail. */
static int parse_pitch_head(const char *s, int default_oct, int allow_oct,
    int *midi, const char **rest) {
    if (!s || !s[0]) return 0;
    int semi = letter_semi((char)tolower((unsigned char)s[0]));
    if (semi < 0) return 0;
    const char *p = s + 1;
    if (*p == '#') { semi++; p++; }
    else if (*p == 'b' || *p == 'B') { semi--; p++; }
    int oct = default_oct;
    if (allow_oct && *p >= '0' && *p <= '9') {
        oct = 0;
        while (*p >= '0' && *p <= '9') {
            oct = oct * 10 + (*p - '0');
            p++;
            if (oct > 12) break;
        }
    }
    *midi = clamp_midi((oct + 1) * 12 + semi);
    if (rest) *rest = p;
    return 1;
}

static const int iv_maj[]   = {0, 4, 7};
static const int iv_min[]   = {0, 3, 7};
static const int iv_7[]     = {0, 4, 7, 10};
static const int iv_maj7[]  = {0, 4, 7, 11};
static const int iv_m7[]    = {0, 3, 7, 10};
static const int iv_dim[]   = {0, 3, 6};
static const int iv_dim7[]  = {0, 3, 6, 9};
static const int iv_aug[]   = {0, 4, 8};
static const int iv_sus4[]  = {0, 5, 7};
static const int iv_sus2[]  = {0, 2, 7};
static const int iv_5[]     = {0, 7};
static const int iv_6[]     = {0, 4, 7, 9};
static const int iv_m6[]    = {0, 3, 7, 9};
static const int iv_9[]     = {0, 4, 7, 10, 14};
static const int iv_add9[]  = {0, 4, 7, 14};
static const int iv_m9[]    = {0, 3, 7, 10, 14};
static const int iv_M9[]    = {0, 4, 7, 11, 14};
static const int iv_m7b5[]  = {0, 3, 6, 10};
static const int iv_7sus[]  = {0, 5, 7, 10};
static const int iv_11[]    = {0, 4, 7, 10, 17};
static const int iv_13[]    = {0, 4, 7, 10, 21};

static int same_q(const char *q, const char *a) {
    return strcmp(q, a) == 0;
}

/* ireal-style qualities. M vs m is case-sensitive; word names are not. */
static int quality_iv(const char *q, const int **iv, int *n) {
    if (!q) return 0;
    if (q[0] == 0 || same_q(q, "M") || same_q(q, "^") ||
        strcasecmp(q, "maj") == 0 || strcasecmp(q, "major") == 0) {
        *iv = iv_maj; *n = 3; return 1;
    }
    if (same_q(q, "m") || same_q(q, "-") ||
        strcasecmp(q, "min") == 0 || strcasecmp(q, "minor") == 0) {
        *iv = iv_min; *n = 3; return 1;
    }
    if (same_q(q, "7")) { *iv = iv_7; *n = 4; return 1; }
    if (same_q(q, "M7") || same_q(q, "^7") || strcasecmp(q, "maj7") == 0) {
        *iv = iv_maj7; *n = 4; return 1;
    }
    if (same_q(q, "m7") || same_q(q, "-7") || strcasecmp(q, "min7") == 0) {
        *iv = iv_m7; *n = 4; return 1;
    }
    if (same_q(q, "o") || strcasecmp(q, "dim") == 0) { *iv = iv_dim; *n = 3; return 1; }
    if (same_q(q, "o7") || strcasecmp(q, "dim7") == 0) { *iv = iv_dim7; *n = 4; return 1; }
    if (same_q(q, "+") || strcasecmp(q, "aug") == 0) { *iv = iv_aug; *n = 3; return 1; }
    if (strcasecmp(q, "sus") == 0 || strcasecmp(q, "sus4") == 0) { *iv = iv_sus4; *n = 3; return 1; }
    if (same_q(q, "2") || strcasecmp(q, "sus2") == 0) { *iv = iv_sus2; *n = 3; return 1; }
    if (same_q(q, "5")) { *iv = iv_5; *n = 2; return 1; }
    if (same_q(q, "6")) { *iv = iv_6; *n = 4; return 1; }
    if (same_q(q, "m6") || same_q(q, "-6")) { *iv = iv_m6; *n = 4; return 1; }
    if (same_q(q, "9")) { *iv = iv_9; *n = 5; return 1; }
    if (strcasecmp(q, "add9") == 0) { *iv = iv_add9; *n = 4; return 1; }
    if (same_q(q, "m9") || same_q(q, "-9")) { *iv = iv_m9; *n = 5; return 1; }
    if (same_q(q, "M9") || same_q(q, "^9") || strcasecmp(q, "maj9") == 0) {
        *iv = iv_M9; *n = 5; return 1;
    }
    if (same_q(q, "m7b5") || same_q(q, "-7b5") || same_q(q, "h") || same_q(q, "h7") ||
        strcasecmp(q, "min7b5") == 0) {
        *iv = iv_m7b5; *n = 4; return 1;
    }
    if (same_q(q, "7sus") || same_q(q, "7sus4")) { *iv = iv_7sus; *n = 4; return 1; }
    if (same_q(q, "11")) { *iv = iv_11; *n = 5; return 1; }
    if (same_q(q, "13")) { *iv = iv_13; *n = 5; return 1; }
    return 0;
}

static int digits_only(const char *q) {
    if (!q || !q[0]) return 1;
    for (const char *p = q; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

int harmony_chord(const char *token, int as_chord, int *out, int max) {
    if (!token || !out || max < 1) return 0;
    int semi = letter_semi((char)tolower((unsigned char)token[0]));
    if (semi < 0) return 0;
    const char *p = token + 1;
    if (*p == '#') { semi++; p++; }
    else if (*p == 'b' || *p == 'B') { semi--; p++; }
    const char *q = p;
    int oct = 3;
    /* C4 = major at octave 4. C7/C9/C2/C5/C6 stay qualities, not octaves. */
    if (as_chord && q[0] >= '0' && q[0] <= '8' && q[1] == 0) {
        int d = q[0] - '0';
        if (d != 2 && d != 5 && d != 6 && d != 7) {
            oct = d;
            q = "";
        }
    }
    if (!q[0]) {
        if (!as_chord) return 0;
    } else if (digits_only(q) && !as_chord) {
        return 0; /* "c7" is a note on note tracks */
    }
    const int *iv = NULL;
    int niv = 0;
    if (!quality_iv(q, &iv, &niv)) return 0;
    int root = (oct + 1) * 12 + semi;
    int c = niv < max ? niv : max;
    for (int i = 0; i < c; i++) out[i] = clamp_midi(root + iv[i]);
    return c;
}

static int scale_fill(const char *type, int *steps, int max, int *nsteps) {
    char name[48];
    size_t j = 0;
    for (size_t i = 0; type[i] && j + 1 < sizeof(name); i++) {
        char c = type[i] == ':' ? ' ' : (char)tolower((unsigned char)type[i]);
        if (c == ' ' && (j == 0 || name[j - 1] == ' ')) continue;
        name[j++] = c;
    }
    while (j > 0 && name[j - 1] == ' ') j--;
    name[j] = 0;
    const int *s = NULL;
    int n = 0;
    static const int major[] = {0, 2, 4, 5, 7, 9, 11};
    static const int minor[] = {0, 2, 3, 5, 7, 8, 10};
    static const int dorian[] = {0, 2, 3, 5, 7, 9, 10};
    static const int phrygian[] = {0, 1, 3, 5, 7, 8, 10};
    static const int lydian[] = {0, 2, 4, 6, 7, 9, 11};
    static const int mixolydian[] = {0, 2, 4, 5, 7, 9, 10};
    static const int locrian[] = {0, 1, 3, 5, 6, 8, 10};
    static const int pent[] = {0, 2, 4, 7, 9};
    static const int mpent[] = {0, 3, 5, 7, 10};
    static const int blues[] = {0, 3, 5, 6, 7, 10};
    static const int chrom[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    static const int whole[] = {0, 2, 4, 6, 8, 10};
    static const int harm[] = {0, 2, 3, 5, 7, 8, 11};
    static const int melodic[] = {0, 2, 3, 5, 7, 9, 11};
    if (strcmp(name, "major") == 0 || strcmp(name, "ionian") == 0) { s = major; n = 7; }
    else if (strcmp(name, "minor") == 0 || strcmp(name, "aeolian") == 0) { s = minor; n = 7; }
    else if (strcmp(name, "dorian") == 0) { s = dorian; n = 7; }
    else if (strcmp(name, "phrygian") == 0) { s = phrygian; n = 7; }
    else if (strcmp(name, "lydian") == 0) { s = lydian; n = 7; }
    else if (strcmp(name, "mixolydian") == 0) { s = mixolydian; n = 7; }
    else if (strcmp(name, "locrian") == 0) { s = locrian; n = 7; }
    else if (strcmp(name, "pentatonic") == 0 || strcmp(name, "major pentatonic") == 0) { s = pent; n = 5; }
    else if (strcmp(name, "minor pentatonic") == 0) { s = mpent; n = 5; }
    else if (strcmp(name, "blues") == 0) { s = blues; n = 6; }
    else if (strcmp(name, "chromatic") == 0) { s = chrom; n = 12; }
    else if (strcmp(name, "whole tone") == 0 || strcmp(name, "wholetone") == 0) { s = whole; n = 6; }
    else if (strcmp(name, "harmonic minor") == 0 || strcmp(name, "harmonic") == 0) { s = harm; n = 7; }
    else if (strcmp(name, "melodic minor") == 0 || strcmp(name, "melodic") == 0) { s = melodic; n = 7; }
    else return 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) steps[i] = s[i];
    *nsteps = n;
    return 1;
}

int harmony_scale(const char *spec, int *root_out, int *steps, int max, int *nsteps) {
    if (!spec || !spec[0] || !root_out || !steps || max < 1 || !nsteps) return 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *colon = strchr(buf, ':');
    const char *type = buf;
    int root = 48; /* C3, matching Strudel's default octave */
    if (colon) {
        *colon = 0;
        type = colon + 1;
        if (!buf[0] || !type[0]) return 0;
        if (!parse_pitch_head(buf, 3, 1, &root, NULL)) return 0;
    } else {
        /* "G1 minor" / "G4 minor": pitch head, then a mode name. */
        const char *rest = NULL;
        int pitched = 0;
        if (parse_pitch_head(buf, 3, 1, &root, &rest) && rest && *rest) {
            while (*rest == ' ' || *rest == '\t') rest++;
            if (*rest) {
                type = rest;
                pitched = 1;
            }
        }
        if (!pitched) root = 48;
    }
    if (!scale_fill(type, steps, max, nsteps)) return 0;
    *root_out = root;
    return 1;
}

int harmony_degree(const char *token, int *degree, int *acc) {
    if (!token || !token[0] || !degree || !acc) return 0;
    const char *p = token;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return 0;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        if (v > 64) return 0;
    }
    int a = 0;
    while (*p == '#' || *p == 'b' || *p == 'B') {
        a += (*p == '#') ? 1 : -1;
        p++;
        if (a > 3 || a < -3) break;
    }
    if (*p != 0) return 0;
    if (neg) v = -v;
    *degree = v;
    *acc = a;
    return 1;
}

int harmony_degree_midi(int degree, int acc, int root, const int *steps, int n) {
    if (!steps || n < 1) return clamp_midi(root);
    if (acc > 3) acc = 3;
    if (acc < -3) acc = -3;
    int neg = degree < 0;
    int ad = neg ? -degree : degree;
    int oct = ad / n;
    int idx = ad % n;
    int semi;
    if (!neg) semi = oct * 12 + steps[idx];
    else if (idx == 0) semi = -(oct * 12);
    else semi = -(oct * 12) - (12 - steps[n - idx]);
    return clamp_midi(root + semi + acc);
}
