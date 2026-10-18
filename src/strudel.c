#include "strudel.h"
#include "slider.h"
#include "xform.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *sws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p ? p : "";
}

static int starts(const char *p, const char *w) {
    size_t n = strlen(w);
    return strncmp(p, w, n) == 0 && (p[n] == '(' || p[n] == 0);
}

/* Copy a quoted string. Newlines stay; the pattern lexer treats them
   as spaces. Returns 0 if the quote never closes. */
static int take_string(const char **pp, char *out, int outlen, char *qch) {
    const char *p = *pp;
    char q = *p;
    if (q != '"' && q != '\'' && q != '`') return 0;
    if (qch) *qch = q;
    p++;
    int k = 0;
    while (*p && *p != q) {
        if (*p == '\\' && p[1]) p++;
        if (k + 1 < outlen) out[k++] = *p;
        p++;
    }
    if (*p != q) return 0;
    out[k] = 0;
    *pp = p + 1;
    return 1;
}

/* Numeric arguments are expressions: setcpm(120/4), .lpf(200*4),
   .gain(slider(1,0,2)/2). Quoted mini-notation never reaches this.
   Stops before a closing paren or comma that belongs to the caller. */
static int parse_expr(const char **pp, double *out, int depth);

static int parse_primary(const char **pp, double *out, int depth) {
    const char *p = sws(*pp);
    if (*p == '(') {
        p++;
        double v = 0;
        if (!parse_expr(&p, &v, depth + 1)) return 0;
        p = sws(p);
        if (*p != ')') return 0;
        *out = v;
        *pp = p + 1;
        return 1;
    }
    if (strncmp(p, "slider", 6) == 0) {
        double init, mn, mx, st;
        if (!slider_parse(&p, &init, &mn, &mx, &st)) return 0;
        *out = slider_bind(init, mn, mx, st);
        *pp = p;
        return 1;
    }
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p || !isfinite(v)) return 0;
    *out = v;
    *pp = end;
    return 1;
}

static int parse_unary(const char **pp, double *out, int depth) {
    const char *p = sws(*pp);
    if (*p == '+' || *p == '-') {
        char op = *p++;
        double v = 0;
        if (!parse_unary(&p, &v, depth + 1)) return 0;
        *out = (op == '-') ? -v : v;
        *pp = p;
        return isfinite(*out);
    }
    return parse_primary(pp, out, depth);
}

static int parse_mul(const char **pp, double *out, int depth) {
    double v = 0;
    if (!parse_unary(pp, &v, depth)) return 0;
    for (;;) {
        const char *p = sws(*pp);
        if (*p != '*' && *p != '/' && *p != '%') break;
        char op = *p++;
        double r = 0;
        if (!parse_unary(&p, &r, depth)) return 0;
        if (op == '*') v *= r;
        else if (r == 0.0) return 0;
        else if (op == '/') v /= r;
        else v = fmod(v, r);
        if (!isfinite(v)) return 0;
        *pp = p;
    }
    *out = v;
    return 1;
}

static int parse_expr(const char **pp, double *out, int depth) {
    if (depth > 32) return 0;
    double v = 0;
    if (!parse_mul(pp, &v, depth)) return 0;
    for (;;) {
        const char *p = sws(*pp);
        if (*p != '+' && *p != '-') break;
        char op = *p++;
        double r = 0;
        if (!parse_mul(&p, &r, depth)) return 0;
        v = (op == '+') ? v + r : v - r;
        if (!isfinite(v)) return 0;
        *pp = p;
    }
    *out = v;
    return 1;
}

static int take_number(const char **pp, double *out) {
    return parse_expr(pp, out, 0);
}

static int is_osc(const char *s) {
    return strcmp(s, "sine") == 0 || strcmp(s, "saw") == 0 || strcmp(s, "sawtooth") == 0
        || strcmp(s, "square") == 0 || strcmp(s, "sqr") == 0 || strcmp(s, "tri") == 0
        || strcmp(s, "triangle") == 0 || strcmp(s, "noise") == 0 || strcmp(s, "white") == 0
        || strncmp(s, "wt:", 3) == 0;
}

static void pat_mod(Node **slot, const char *text) {
    if (*slot) node_free(*slot);
    *slot = parse_pattern_text(text);
}

void strudel_line_free(StrudelLine *s) {
    if (!s) return;
    node_free(s->gain_pat);
    node_free(s->pan_pat);
    node_free(s->lpf_pat);
    node_free(s->off_add_pat);
    node_free(s->speed_pat);
    node_free(s->rarely_speed);
    s->gain_pat = s->pan_pat = s->lpf_pat = s->off_add_pat = NULL;
    s->speed_pat = s->rarely_speed = NULL;
    sig_free(s->sig);
    s->sig = NULL;
    for (int i = 0; i < s->hl_n && i < HL_PAT_MAX; i++) node_free(s->hl_pat[i]);
    s->hl_n = 0;
}

static void zero_line(StrudelLine *o) {
    memset(o, 0, sizeof(*o));
    o->gain = 0.8;
    o->gain_slider=o->lpf_slider=-1;
    o->pan = 0;
    o->lpf = 0;
    o->att = o->dec = o->sus = o->rel = -1;
    o->fast_repeat = 1;
    o->degrade = -1;
    o->room = -1;
    o->delay = -1;
    o->sometimes_p = -1;
    o->rarely_p = -1;
    o->rarely_rate = -1;
    o->rarely_send = -1;
    o->lpq = -1;
    o->lpa = -1;
    o->smp_end = -1;
    o->delay_time = -1;
    o->delay_fb = -1;
    o->lpf_sig.slow = o->lpf_sig.fast = 1;
    o->dec_sig.slow = o->dec_sig.fast = 1;
    o->end_sig.slow = o->end_sig.fast = 1;
    o->pattern_at = -1;
    for (int i = 0; i < HL_PAT_MAX; i++) o->hl_at[i] = -1;
}

typedef struct {
    int kind; /* 0 none, 1 num, 2 str, 3 call, 4 sig, 5 fn */
    double num;
    int slider;
    char str[512];
    char call[32];
    double call_num;
    int call_has_num;
    StrudelSig sig;
    double fn_speed;
    char fn_speed_src[64]; /* .speed("2 | -2"): not a single number */
    double fn_delay;
    int fn_fast;
    double fn_slow;
    int fn_rev;
    char fn_inst[32];
    int fn_has_add;
    double fn_add_num;
    char fn_add_pat[512];
    int fn_add_at; /* -1 if the add(note()) string has no source offset */
    char fn_add_q;
    int op_n;
    unsigned char op_k[8]; /* 1 = late, 2 = slow, in source order */
    double op_v[8];
    int str_at; /* byte offset of a quoted string in the statement, -1 if none */
    char quote;
} Arg;

static int parse_ident(const char **pp, char *out, int n) {
    const char *p = sws(*pp);
    if (!isalpha((unsigned char)*p) && *p != '_') return 0;
    int i = 0;
    while (isalnum((unsigned char)*p) || *p == '_') {
        if (i + 1 < n) out[i++] = *p;
        p++;
    }
    out[i] = 0;
    *pp = p;
    return i > 0;
}

static void skip_group(const char **pp) {
    const char *p = sws(*pp);
    if (*p != '(') return;
    int d = 1;
    char q = 0;
    p++;
    while (*p && d) {
        if (q) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == q) q = 0;
            p++;
            continue;
        }
        if (*p == '"' || *p == '\'' || *p == '`') { q = *p; p++; continue; }
        if (*p == '(') d++;
        else if (*p == ')') d--;
        p++;
    }
    *pp = p;
}

static int parse_sig(const char **pp, const char *name, Arg *a) {
    int kind = 0;
    if (strcmp(name, "sine") == 0 || strcmp(name, "cosine") == 0) kind = 1;
    else if (strcmp(name, "perlin") == 0) kind = 2;
    else if (strcmp(name, "rand") == 0) kind = 3;
    else return 0;
    memset(&a->sig, 0, sizeof(a->sig));
    a->kind = 4;
    a->sig.kind = kind;
    a->sig.slow = 1;
    a->sig.fast = 1;
    const char *p = *pp;
    while (*sws(p) == '.') {
        p = sws(p) + 1;
        char m[32];
        if (!parse_ident(&p, m, (int)sizeof(m))) return 0;
        p = sws(p);
        if (*p != '(') return 0;
        p++;
        double n1 = 0, n2 = 0;
        if (!take_number(&p, &n1)) return 0;
        p = sws(p);
        int two = 0;
        if (*p == ',') {
            p++;
            if (!take_number(&p, &n2)) return 0;
            two = 1;
            p = sws(p);
        }
        if (*p != ')') return 0;
        p++;
        if (strcmp(m, "range") == 0 && two) {
            a->sig.has_range = 1;
            a->sig.lo = n1;
            a->sig.hi = n2;
        } else if (strcmp(m, "slow") == 0) {
            if (n1 < 0.01) n1 = 0.01;
            if (n1 > 256) n1 = 256;
            a->sig.slow = n1;
        } else if (strcmp(m, "fast") == 0) {
            if (n1 < 0.01) n1 = 0.01;
            if (n1 > 64) n1 = 64;
            a->sig.fast = n1;
        }
    }
    *pp = p;
    return 1;
}

/* p sits inside a '(', on the first inner character. Leaves p on the matching ')'. */
static void skip_inside(const char **pp) {
    const char *p = *pp;
    int d = 1;
    char q = 0;
    while (*p && d) {
        if (q) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == q) q = 0;
            p++;
            continue;
        }
        if (*p == '"' || *p == '\'' || *p == '`') { q = *p++; continue; }
        if (*p == '(') d++;
        else if (*p == ')') d--;
        if (d) p++;
    }
    *pp = p;
}

/* First argument is already parsed. Consume a comma and the rest through ')'. */
static void skip_rest_args(const char **pp) {
    const char *p = *pp;
    int d = 1;
    char q = 0;
    while (*p && d) {
        if (q) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == q) q = 0;
            p++;
            continue;
        }
        if (*p == '"' || *p == '\'' || *p == '`') { q = *p++; continue; }
        if (*p == '(') d++;
        else if (*p == ')') d--;
        if (d) p++;
    }
    if (*p == ')') p++;
    *pp = p;
}

static void push_op(Arg *a, unsigned char k, double v) {
    if (a->op_n < 0 || a->op_n >= 8) return;
    a->op_k[a->op_n] = k;
    a->op_v[a->op_n] = v;
    a->op_n++;
}

/* add(n) or add(note("mini")). p is just inside add's '('. Consumes the ')'. */
static int parse_fn_add(const char **pp, Arg *a, const char *base) {
    const char *p = sws(*pp);
    const char *save = p;
    char name[32];
    if (parse_ident(&p, name, (int)sizeof(name))) {
        p = sws(p);
        if (*p == '(' && (strcmp(name, "note") == 0 || strcmp(name, "n") == 0)) {
            p++;
            p = sws(p);
            if (*p == '"' || *p == '\'' || *p == '`') {
                char q = *p;
                const char *content = p + 1;
                if (!take_string(&p, a->fn_add_pat, (int)sizeof(a->fn_add_pat), NULL))
                    return 0;
                a->fn_add_q = q;
                if (base && content >= base) a->fn_add_at = (int)(content - base);
                p = sws(p);
                if (*p == ')') p++;
            } else {
                skip_inside(&p);
                if (*p == ')') p++;
            }
        } else {
            p = save;
        }
    }
    if (!a->fn_add_pat[0]) {
        double num = 0;
        p = sws(p);
        if (*p != ')' && take_number(&p, &num)) {
            a->fn_has_add = 1;
            a->fn_add_num = num;
        } else if (*p != ')') {
            skip_inside(&p);
        }
    }
    p = sws(p);
    if (*p == ')') p++;
    *pp = p;
    return 1;
}

/* One method argument that is a number or a short string. Consumes ')'. */
static int parse_fn_simple(const char **pp, const char *m, Arg *a) {
    const char *p = sws(*pp);
    double num = 0;
    int has = 0;
    char text[64];
    text[0] = 0;
    if (*p == '"' || *p == '\'' || *p == '`') {
        if (!take_string(&p, text, (int)sizeof(text), NULL)) return 0;
        char *end = NULL;
        num = strtod(text, &end);
        /* "2 | -2" must stay a pattern. A leading strtod would freeze it at 2. */
        has = (end != text && end && *end == 0 && isfinite(num));
    } else if (*p != ')' && take_number(&p, &num)) {
        has = 1;
    } else if (*p != ')') {
        skip_inside(&p);
    }
    p = sws(p);
    if (*p == ')') p++;
    *pp = p;
    if (has && strcmp(m, "speed") == 0) a->fn_speed = num;
    else if (strcmp(m, "speed") == 0 && text[0] && !has)
        snprintf(a->fn_speed_src, sizeof(a->fn_speed_src), "%s", text);
    else if (has && strcmp(m, "delay") == 0) a->fn_delay = num;
    else if (has && strcmp(m, "fast") == 0) a->fn_fast = (int)(num + 0.5);
    else if (has && strcmp(m, "slow") == 0) {
        a->fn_slow = num;
        push_op(a, 2, num);
    } else if (has && strcmp(m, "late") == 0) {
        push_op(a, 1, num);
    } else if (text[0] && (strcmp(m, "s") == 0 || strcmp(m, "sound") == 0) && is_osc(text)) {
        snprintf(a->fn_inst, sizeof(a->fn_inst), "%.31s", text);
    }
    return 1;
}

/* x => x.rev().s('saw') — the subset jux/off actually use. color() is consumed and ignored. */
static int parse_fn_body(const char **pp, Arg *a, const char *base) {
    const char *p = sws(*pp);
    char id[32];
    if (!parse_ident(&p, id, (int)sizeof(id))) return 0;
    a->kind = 5;
    a->fn_speed = -1;
    a->fn_delay = -1;
    a->fn_add_at = -1;
    while (*sws(p) == '.') {
        p = sws(p) + 1;
        char m[32];
        if (!parse_ident(&p, m, (int)sizeof(m))) return 0;
        p = sws(p);
        if (strcmp(m, "rev") == 0) a->fn_rev = 1;
        if (*p != '(') continue;
        p++;
        if (strcmp(m, "add") == 0) {
            if (!parse_fn_add(&p, a, base)) return 0;
        } else if (!parse_fn_simple(&p, m, a)) {
            return 0;
        }
    }
    *pp = p;
    return 1;
}

/* off(time) is the first late. Arrow lates before slow join it; lates after slow do not. */
static void fold_arrow_time(const Arg *a, double time, double *pre, double *slow, double *post) {
    double p = time;
    double s = 1.0;
    double q = 0.0;
    int saw = 0;
    int n = a->op_n;
    if (n > 8) n = 8;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        double v = a->op_v[i];
        if (!isfinite(v)) continue;
        if (a->op_k[i] == 1) {
            if (!saw) p += v;
            else q += v;
        } else if (a->op_k[i] == 2) {
            if (v < 0.01) v = 0.01;
            if (v > 64.0) v = 64.0;
            s *= v;
            if (s > 64.0) s = 64.0;
            saw = 1;
        }
    }
    if (!isfinite(p)) p = time;
    if (!isfinite(q)) q = 0;
    *pre = p;
    *slow = s;
    *post = q;
}

static int parse_arg(const char **pp, Arg *a, const char *base) {
    memset(a, 0, sizeof(*a));
    a->slider=-1;
    a->fn_speed = -1;
    a->fn_delay = -1;
    a->fn_add_at = -1;
    a->str_at = -1;
    const char *p = sws(*pp);
    if (*p == '"' || *p == '\'' || *p == '`') {
        const char *content = p + 1;
        if (!take_string(&p, a->str, (int)sizeof(a->str), &a->quote)) return 0;
        a->kind = 2;
        if (base) a->str_at = (int)(content - base);
        *pp = p;
        return 1;
    }
    if (strncmp(p, "slider", 6) == 0) {
        int index=slider_next_index();
        const char *direct=p; double init,mn,mx,step;
        int simple=slider_parse(&direct,&init,&mn,&mx,&step);
        if (take_number(&p, &a->num)) {
            if (simple && sws(direct)==sws(p)) a->slider=index;
            a->kind = 1;
            *pp = p;
            return 1;
        }
    }
    if (isalpha((unsigned char)*p) || *p == '_') {
        char name[32];
        if (!parse_ident(&p, name, (int)sizeof(name))) return 0;
        p = sws(p);
        if (p[0] == '=' && p[1] == '>') {
            p += 2;
            if (!parse_fn_body(&p, a, base)) return 0;
            *pp = p;
            return 1;
        }
        if (*p == '(') {
            p++;
            p = sws(p);
            a->kind = 3;
            snprintf(a->call, sizeof(a->call), "%s", name);
            if (*p == '"' || *p == '\'' || *p == '`') {
                if (!take_string(&p, a->str, (int)sizeof(a->str), NULL)) return 0;
                char *end = NULL;
                double v = strtod(a->str, &end);
                if (end != a->str && isfinite(v)) {
                    a->call_num = v;
                    a->call_has_num = 1;
                }
            } else if (*p != ')') {
                if (take_number(&p, &a->call_num)) a->call_has_num = 1;
            }
            p = sws(p);
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
            *pp = p;
            return 1;
        }
        if (*p == '.') {
            if (!parse_sig(&p, name, a)) {
                while (*p && *p != ',' && *p != ')') {
                    if (*p == '(') { skip_group(&p); continue; }
                    p++;
                }
                a->kind = 0;
            }
            *pp = p;
            return 1;
        }
        a->kind = 0;
        *pp = p;
        return 1;
    }
    if (take_number(&p, &a->num)) {
        a->kind = 1;
        *pp = p;
        return 1;
    }
    return 0;
}

static void set_keyword(StrudelLine *o, const char *kind) {
    snprintf(o->keyword, sizeof(o->keyword), "%s", kind);
}

static void apply_call_mod(StrudelLine *o, double p, const Arg *a, int rarely) {
    if (a->kind == 3 && a->call_has_num && strcmp(a->call, "fast") == 0) {
        int n = (int)(a->call_num + 0.5);
        if (n < 1) n = 1;
        if (n > 16) n = 16;
        if (rarely) { /* rare fast is not used by flatrave */ (void)n; }
        else { o->sometimes_p = p; o->sometimes_fast = n; }
    } else if (a->kind == 3 && a->call_has_num && strcmp(a->call, "slow") == 0) {
        double s = a->call_num;
        if (s < 0.01) s = 0.01;
        if (s > 64) s = 64;
        o->sometimes_p = p;
        o->sometimes_slow = s;
    } else if (a->kind == 5) {
        if (rarely) {
            o->rarely_p = p;
            o->rarely_rate = a->fn_speed;
            o->rarely_send = a->fn_delay;
            if (a->fn_speed_src[0]) {
                o->rarely_rate = -1;
                pat_mod(&o->rarely_speed, a->fn_speed_src);
            }
        } else {
            o->sometimes_p = p;
            if (a->fn_fast > 1) o->sometimes_fast = a->fn_fast;
            if (a->fn_slow > 1) o->sometimes_slow = a->fn_slow;
        }
    } else if (!rarely) {
        o->degrade = p;
    } else {
        o->rarely_p = p;
    }
}

/* One .name(arg) or .name(). Stops when the next character is not '.'. */
static void remember_hl(StrudelLine *o, const char *name, const Arg *a) {
    if (!o || !a || a->kind != 2 || !a->str[0] || a->str_at < 0) return;
    int control = name && (strcmp(name, "gain") == 0 || strcmp(name, "pan") == 0
        || strcmp(name, "lpf") == 0 || strcmp(name, "cutoff") == 0
        || strcmp(name, "struct") == 0 || strcmp(name, "mask") == 0);
    int mini = (a->quote == '"' || a->quote == '`');
    if (!control && !mini) return;
    if (a->str_at == o->pattern_at) return;
    if (o->hl_n >= HL_PAT_MAX) return;
    for (int i = 0; i < o->hl_n; i++)
        if (o->hl_at[i] == a->str_at) return;
    Node *n = parse_pattern_text(a->str);
    if (!n) return;
    o->hl_pat[o->hl_n] = n;
    o->hl_at[o->hl_n] = a->str_at;
    o->hl_len[o->hl_n] = (int)strlen(a->str);
    o->hl_n++;
}

static int parse_methods(const char **pp, StrudelLine *o, const char *base) {
    const char *lp = *pp;
    while (*sws(lp) == '.') {
        lp = sws(lp) + 1;
        char name[32];
        int ni = 0;
        while (*lp && (isalnum((unsigned char)*lp) || *lp == '_') && ni < 31)
            name[ni++] = *lp++;
        name[ni] = 0;
        lp = sws(lp);
        Arg a;
        memset(&a, 0, sizeof(a));
        int has_arg = 0;
        if (*lp == '(') {
            if (strcmp(name, "off") == 0) {
                const char *open = lp;
                lp++;
                Arg t, add;
                memset(&t, 0, sizeof(t));
                memset(&add, 0, sizeof(add));
                int ok_t = parse_arg(&lp, &t, base);
                lp = sws(lp);
                if (*lp == ',') {
                    lp++;
                    parse_arg(&lp, &add, base);
                }
                lp = sws(lp);
                if (*lp == ')') lp++;
                else { lp = open; skip_group(&lp); }
                /* off(time, add(n)) is the scalar copy. An arrow is a
                   separate copy (slow/late/sound/add(note)), not a unison. */
                int got_add = (add.kind == 1)
                    || (add.kind == 3 && add.call_has_num);
                if (ok_t && t.kind == 1 && got_add) {
                    o->off_time = t.num;
                    o->has_off = 1;
                    o->off_degrees = o->scale[0] ? 0 : 1;
                    if (add.kind == 3) o->off_add = add.call_num;
                    else o->off_add = add.num;
                } else if (ok_t && t.kind == 1 && add.kind == 5) {
                    double pre = 0, slow = 1, post = 0;
                    fold_arrow_time(&add, t.num, &pre, &slow, &post);
                    o->off_fn = 1;
                    o->off_time = pre;
                    o->off_slow = slow;
                    o->off_late = post;
                    if (add.fn_inst[0])
                        snprintf(o->off_inst, sizeof(o->off_inst), "%.31s", add.fn_inst);
                    if (add.fn_has_add) o->off_add = add.fn_add_num;
                    if (add.fn_add_pat[0]) {
                        pat_mod(&o->off_add_pat, add.fn_add_pat);
                        if (add.fn_add_at >= 0
                            && (add.fn_add_q == '"' || add.fn_add_q == '`')
                            && o->hl_n < HL_PAT_MAX) {
                            Node *hn = parse_pattern_text(add.fn_add_pat);
                            if (hn) {
                                o->hl_pat[o->hl_n] = hn;
                                o->hl_at[o->hl_n] = add.fn_add_at;
                                o->hl_len[o->hl_n] = (int)strlen(add.fn_add_pat);
                                o->hl_n++;
                            }
                        }
                    }
                }
                continue;
            }
            lp++;
            lp = sws(lp);
            if (*lp == ')') {
                lp++;
            } else {
                has_arg = 1;
                if (!parse_arg(&lp, &a, base)) {
                    const char *back = lp - 1;
                    while (back > *pp && *back != '(') back--;
                    lp = back;
                    skip_group(&lp);
                    has_arg = 0;
                } else {
                    /* sometimesBy(p, fn) and other extra arguments stay unread. */
                    lp = sws(lp);
                    skip_rest_args(&lp);
                }
            }
        }
        if (strcmp(name, "fast") == 0 && a.kind == 1) {
            int n = (int)(a.num + 0.5);
            if (n < 1) n = 1;
            if (n > 64) n = 64;
            o->fast_repeat = n;
        } else if (strcmp(name, "fast") == 0 && a.kind == 2) {
            double v = strtod(a.str, NULL);
            int n = (int)(v + 0.5);
            if (n < 1) n = 1;
            if (n > 64) n = 64;
            o->fast_repeat = n;
        } else if (strcmp(name, "slow") == 0 && a.kind == 1) {
            double f = a.num;
            if (f < 0.01) f = 0.01;
            if (f > 64) f = 64;
            char num[16];
            int nn = snprintf(num, sizeof(num), "/%.4g", f);
            size_t n = strlen(o->pattern);
            if (nn > 0 && n + (size_t)nn + 3 < sizeof(o->pattern)) {
                if (o->pattern_len <= 0) o->pattern_len = (int)n;
                o->pattern_bias++;
                memmove(o->pattern + 1, o->pattern, n + 1);
                o->pattern[0] = '[';
                o->pattern[n + 1] = ']';
                memcpy(o->pattern + n + 2, num, (size_t)nn + 1);
            }
        } else if (strcmp(name, "rev") == 0) {
            o->rev = 1;
        } else if (strcmp(name, "palindrome") == 0) {
            o->palindrome = 1;
        } else if (strcmp(name, "jux") == 0 && a.kind == 5) {
            o->jux = 1;
            o->jux_rev = a.fn_rev;
            if (a.fn_inst[0])
                snprintf(o->jux_inst, sizeof(o->jux_inst), "%.31s", a.fn_inst);
        } else if (strcmp(name, "gain") == 0 && has_arg) {
            o->gain_slider=a.kind==1?a.slider:-1;
            if (a.kind == 2) {
                pat_mod(&o->gain_pat, a.str);
                o->has_gain = 1;
            } else if (a.kind == 1) {
                double num = a.num;
                if (num < 0) num = 0;
                if (num > 2) num = 2;
                o->gain = num;
                o->has_gain = 1;
            }
        } else if ((strcmp(name, "lpf") == 0 || strcmp(name, "cutoff") == 0) && has_arg) {
            o->lpf_slider=a.kind==1?a.slider:-1;
            if (a.kind == 4) {
                o->lpf_sig = a.sig;
                o->has_lpf = 1;
            } else if (a.kind == 2) {
                pat_mod(&o->lpf_pat, a.str);
                o->has_lpf = 1;
            } else if (a.kind == 1 && a.num >= 20 && a.num <= 18000) {
                o->lpf = a.num;
                o->has_lpf = 1;
            }
        } else if (strcmp(name, "pan") == 0 && has_arg) {
            if (a.kind == 2) {
                pat_mod(&o->pan_pat, a.str);
                o->has_pan = 1;
            } else if (a.kind == 1) {
                o->pan = a.num;
                o->has_pan = 1;
            }
        } else if ((strcmp(name, "attack") == 0 || strcmp(name, "att") == 0) && a.kind == 1) {
            o->att = a.num; o->has_env = 1;
        } else if ((strcmp(name, "decay") == 0 || strcmp(name, "dec") == 0) && a.kind == 4) {
            o->dec_sig = a.sig; o->has_env = 1;
        } else if ((strcmp(name, "decay") == 0 || strcmp(name, "dec") == 0) && a.kind == 1) {
            o->dec = a.num; o->has_env = 1;
        } else if ((strcmp(name, "sustain") == 0 || strcmp(name, "sus") == 0) && a.kind == 1) {
            o->sus = a.num; o->has_env = 1;
        } else if ((strcmp(name, "release") == 0 || strcmp(name, "rel") == 0) && a.kind == 1) {
            o->rel = a.num; o->has_env = 1;
        } else if (strcmp(name, "scale") == 0 && a.kind == 2) {
            snprintf(o->scale, sizeof(o->scale), "%.63s", a.str);
        } else if (strcmp(name, "bank") == 0 && a.kind == 2) {
            snprintf(o->bank, sizeof(o->bank), "%.63s", a.str);
        } else if (strcmp(name, "s") == 0 || strcmp(name, "sound") == 0) {
            if (a.kind == 2) {
                if (is_osc(a.str)) snprintf(o->inst, sizeof(o->inst), "%.31s", a.str);
                else snprintf(o->bank, sizeof(o->bank), "%.63s", a.str);
            }
            if (!o->keyword[0]) set_keyword(o, "sound");
        } else if (strcmp(name, "note") == 0) {
            set_keyword(o, "notes");
        } else if (strcmp(name, "n") == 0) {
            set_keyword(o, "n");
        } else if (strcmp(name, "degrade") == 0) {
            o->degrade = 0.5;
        } else if (strcmp(name, "degradeBy") == 0 && a.kind == 1) {
            double num = a.num;
            if (num < 0) num = 0;
            if (num > 1) num = 1;
            o->degrade = num;
        } else if (strcmp(name, "sometimes") == 0) {
            apply_call_mod(o, 0.5, &a, 0);
        } else if (strcmp(name, "rarely") == 0) {
            apply_call_mod(o, 0.25, &a, 1);
        } else if (strcmp(name, "room") == 0 && a.kind == 1) {
            o->room = a.num;
        } else if (strcmp(name, "delay") == 0 && a.kind == 1) {
            o->delay = a.num;
        } else if (strcmp(name, "delaytime") == 0 && a.kind == 1) {
            /* Seconds, not cycles (Strudel delaysync is the cycle form). */
            double sec = a.num;
            if (sec < 0) sec = 0;
            if (sec > 2) sec = 2;
            o->delay_time = sec;
        } else if ((strcmp(name, "delayfeedback") == 0 || strcmp(name, "delayfb") == 0)
                   && a.kind == 1) {
            double fb = a.num;
            if (fb < 0) fb = 0;
            if (fb > 0.98) fb = 0.98;
            o->delay_fb = fb;
        } else if (strcmp(name, "struct") == 0 && a.kind == 2) {
            snprintf(o->struct_pat, sizeof(o->struct_pat), "%s", a.str);
        } else if (strcmp(name, "mask") == 0 && a.kind == 2) {
            snprintf(o->mask_pat, sizeof(o->mask_pat), "%s", a.str);
        } else if ((strcmp(name, "lpq") == 0 || strcmp(name, "resonance") == 0) && a.kind == 1) {
            double q = a.num;
            if (q < 0) q = 0;
            if (q > 24) q = 24;
            o->lpq = q;
        } else if ((strcmp(name, "lpenv") == 0 || strcmp(name, "lpe") == 0) && a.kind == 1) {
            o->lpenv = a.num;
            o->has_lpenv = 1;
        } else if ((strcmp(name, "lpa") == 0 || strcmp(name, "lpattack") == 0) && a.kind == 1) {
            o->lpa = a.num;
        } else if (strcmp(name, "end") == 0 && a.kind == 4) {
            o->end_sig = a.sig;
        } else if (strcmp(name, "end") == 0 && a.kind == 1) {
            double e = a.num;
            if (e < 0.001) e = 0.001;
            if (e > 1) e = 1;
            o->smp_end = e;
        } else if (strcmp(name, "speed") == 0 && a.kind == 1) {
            if (a.num != 0.0) o->smp_speed = a.num;
        } else if (strcmp(name, "speed") == 0 && a.kind == 2) {
            char *end = NULL;
            double v = strtod(a.str, &end);
            if (end && *end == 0 && isfinite(v) && v != 0.0) o->smp_speed = v;
            else if (a.str[0]) pat_mod(&o->speed_pat, a.str);
        }
        if (has_arg) remember_hl(o, name, &a);
    }
    *pp = lp;
    return 1;
}

static SigExpr *sig_new(int kind) {
    SigExpr *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->kind = kind;
    e->slow = 1;
    e->fast = 1;
    return e;
}

void sig_free(SigExpr *e) {
    if (!e) return;
    sig_free(e->a);
    sig_free(e->b);
    free(e);
}

double sig_eval(const SigExpr *e, double t) {
    if (!e || !isfinite(t)) return 0;
    double slow = e->slow > 0 ? e->slow : 1;
    double fast = e->fast > 0 ? e->fast : 1;
    double tt = t * fast / slow;
    double v = 0;
    double frac;
    switch (e->kind) {
    case SIG_SINE: v = 0.5 + 0.5 * sin(2.0 * M_PI * tt); break;
    case SIG_COS: v = 0.5 + 0.5 * cos(2.0 * M_PI * tt); break;
    case SIG_SAW: v = tt - floor(tt); break;
    case SIG_SQUARE: v = (tt - floor(tt)) < 0.5 ? 0.0 : 1.0; break;
    case SIG_TRI:
        frac = tt - floor(tt);
        v = frac < 0.5 ? frac * 2.0 : (1.0 - frac) * 2.0;
        break;
    case SIG_PERLIN: v = xform_signal(2, 1, 1, 0, 0, 1, 0, tt); break;
    case SIG_RAND: v = xform_signal(3, 1, 1, 0, 0, 1, 0, tt); break;
    case SIG_NUM: v = e->num; break;
    case SIG_ADD: v = sig_eval(e->a, tt) + sig_eval(e->b, tt); break;
    case SIG_MUL: v = sig_eval(e->a, tt) * sig_eval(e->b, tt); break;
    default: break;
    }
    if (e->has_range && isfinite(e->lo) && isfinite(e->hi))
        v = e->lo + (e->hi - e->lo) * v;
    return isfinite(v) ? v : 0;
}

static int clamp_deg(double v) {
    long d;
    if (!isfinite(v)) return 0;
    d = lround(v);
    if (d > 64) d = 64;
    if (d < -64) d = -64;
    return (int)d;
}

void sig_events(const SigExpr *e, PatEventList *list, int cycle) {
    int n, i;
    if (!e || !list) return;
    n = e->segment;
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    for (i = 0; i < n; i++) {
        double start = (double)i / (double)n;
        double dur = 1.0 / (double)n;
        double v = sig_eval(e, (double)cycle + start);
        char tok[16];
        int d = clamp_deg(v);
        snprintf(tok, sizeof(tok), "%d", d);
        patlist_push(list, tok, start, dur);
        if (e->has_extra && isfinite(e->extra)) {
            int d2 = clamp_deg(v + e->extra);
            if (d2 != d) {
                snprintf(tok, sizeof(tok), "%d", d2);
                patlist_push(list, tok, start, dur);
            }
        }
    }
}

static int sig_leaf_kind(const char *name) {
    if (strcmp(name, "sine") == 0) return SIG_SINE;
    if (strcmp(name, "saw") == 0) return SIG_SAW;
    if (strcmp(name, "cosine") == 0 || strcmp(name, "cos") == 0) return SIG_COS;
    if (strcmp(name, "square") == 0) return SIG_SQUARE;
    if (strcmp(name, "tri") == 0 || strcmp(name, "triangle") == 0) return SIG_TRI;
    if (strcmp(name, "perlin") == 0) return SIG_PERLIN;
    if (strcmp(name, "rand") == 0) return SIG_RAND;
    return 0;
}

static int is_sig_method(const char *m) {
    return strcmp(m, "add") == 0 || strcmp(m, "mul") == 0
        || strcmp(m, "slow") == 0 || strcmp(m, "fast") == 0
        || strcmp(m, "range") == 0 || strcmp(m, "segment") == 0
        || strcmp(m, "seg") == 0 || strcmp(m, "superimpose") == 0;
}

/* x => x.add(number). Consumes the closing ')'. Returns 0 with *pp unchanged. */
static int parse_super_add(const char **pp, SigExpr *cur) {
    const char *p = sws(*pp);
    char id[32], m[32];
    double extra = 0;
    if (!parse_ident(&p, id, (int)sizeof(id))) return 0;
    p = sws(p);
    if (!(p[0] == '=' && p[1] == '>')) return 0;
    p += 2;
    if (!parse_ident(&p, id, (int)sizeof(id))) return 0;
    p = sws(p);
    if (*p != '.') return 0;
    p++;
    if (!parse_ident(&p, m, (int)sizeof(m)) || strcmp(m, "add") != 0) return 0;
    p = sws(p);
    if (*p != '(') return 0;
    p++;
    if (!take_number(&p, &extra) || !isfinite(extra)) return 0;
    p = sws(p);
    if (*p != ')') return 0;
    p++;
    p = sws(p);
    if (*p != ')') return 0;
    p++;
    cur->has_extra = 1;
    cur->extra = extra;
    *pp = p;
    return 1;
}

static int parse_sig_expr(const char **pp, SigExpr **out, int depth);

static int parse_sig_operand(const char **pp, SigExpr **out, int depth) {
    const char *p = sws(*pp);
    double n = 0;
    SigExpr *e;
    if (isalpha((unsigned char)*p) || *p == '_')
        return parse_sig_expr(pp, out, depth);
    if (!take_number(&p, &n) || !isfinite(n)) return 0;
    e = sig_new(SIG_NUM);
    if (!e) return 0;
    e->num = n;
    *out = e;
    *pp = p;
    return 1;
}

static int take_sig_call(const char **pp, double *n1, double *n2, int *two) {
    const char *p = sws(*pp);
    if (*p != '(') return 0;
    p++;
    if (!take_number(&p, n1)) return 0;
    p = sws(p);
    *two = 0;
    if (*p == ',') {
        p++;
        if (!take_number(&p, n2)) return 0;
        *two = 1;
        p = sws(p);
    }
    if (*p != ')') return 0;
    *pp = p + 1;
    return 1;
}

/* Signal methods rewrite *cur. A later .scale/.note/.s stops on the dot. */
static int parse_sig_expr(const char **pp, SigExpr **out, int depth) {
    const char *p;
    char name[32];
    SigExpr *cur;
    int kind;
    if (depth > 32) return 0;
    p = sws(*pp);
    if (!parse_ident(&p, name, (int)sizeof(name))) return 0;
    kind = sig_leaf_kind(name);
    if (!kind) return 0;
    cur = sig_new(kind);
    if (!cur) return 0;
    while (*sws(p) == '.') {
        const char *dot = sws(p);
        const char *q = dot + 1;
        char m[32];
        if (!parse_ident(&q, m, (int)sizeof(m))) { sig_free(cur); return 0; }
        if (!is_sig_method(m)) { p = dot; break; }
        if (strcmp(m, "superimpose") == 0) {
            q = sws(q);
            if (*q != '(') { sig_free(cur); return 0; }
            q++;
            if (!parse_super_add(&q, cur)) {
                skip_inside(&q);
                if (*q == ')') q++;
            }
        } else if (strcmp(m, "add") == 0 || strcmp(m, "mul") == 0) {
            SigExpr *rhs = NULL;
            SigExpr *op;
            q = sws(q);
            if (*q != '(') { sig_free(cur); return 0; }
            q++;
            if (!parse_sig_operand(&q, &rhs, depth + 1)) { sig_free(cur); return 0; }
            q = sws(q);
            if (*q != ')') { sig_free(rhs); sig_free(cur); return 0; }
            q++;
            op = sig_new(strcmp(m, "add") == 0 ? SIG_ADD : SIG_MUL);
            if (!op) { sig_free(rhs); sig_free(cur); return 0; }
            op->a = cur;
            op->b = rhs;
            cur = op;
        } else {
            double n1 = 0, n2 = 0;
            int two = 0;
            if (!take_sig_call(&q, &n1, &n2, &two)) { sig_free(cur); return 0; }
            if (strcmp(m, "slow") == 0 || strcmp(m, "fast") == 0) {
                double n = n1;
                if (!(n > 0)) n = 1;
                if (n < 0.01) n = 0.01;
                if (n > 256) n = 256;
                if (strcmp(m, "slow") == 0) {
                    cur->slow *= n;
                    if (cur->slow > 256) cur->slow = 256;
                } else {
                    if (n > 64) n = 64;
                    cur->fast *= n;
                    if (cur->fast > 64) cur->fast = 64;
                }
            } else if (strcmp(m, "range") == 0 && two) {
                cur->has_range = 1;
                cur->lo = n1;
                cur->hi = n2;
            } else if (strcmp(m, "segment") == 0 || strcmp(m, "seg") == 0) {
                int k = (int)lround(n1);
                if (k < 1) k = 1;
                if (k > 64) k = 64;
                cur->segment = k;
            } else {
                sig_free(cur);
                return 0;
            }
        }
        p = q;
    }
    *pp = p;
    *out = cur;
    return 1;
}

int strudel_parse(const char *line, StrudelLine *out) {
    if (!out) return 0;
    zero_line(out);
    if (!line) return 0;
    char buf[8192];
    snprintf(buf, sizeof(buf), "%s", line);
    /* strip // comments that are outside quotes */
    {
        int q = 0;
        for (char *p = buf; *p; p++) {
            if (*p == '"' || *p == '\'' || *p == '`') q = !q;
            if (!q && p[0] == '/' && p[1] == '/') { *p = 0; break; }
        }
    }
    const char *p = sws(buf);
    if (p[0] == '$' && p[1] == ':') p = sws(p + 2);
    if (strncmp(p, "setcpm", 6) == 0 || strncmp(p, "setcps", 6) == 0) {
        int cps = (p[3] == 'c' && p[4] == 'p' && p[5] == 's');
        const char *lp = strchr(p, '(');
        if (!lp) return 0;
        lp++;
        double v = 0;
        if (!take_number(&lp, &v)) return 0;
        if (*sws(lp) != ')') return 0;
        /* setcpm: cycles per minute. Our cycle is 4 beats, so BPM = cpm*4.
           setcps: cycles per second → BPM = cps*240. */
        double bpm = cps ? v * 240.0 : v * 4.0;
        out->handled = 1;
        out->bpm_set = 1;
        out->bpm = bpm;
        return 1;
    }
    /* "0 5 3 2".scale('G4 minor').note() — the keyword is a later method. */
    if (*p == '"' || *p == '\'' || *p == '`') {
        const char *content = p + 1;
        if (!take_string(&p, out->pattern, (int)sizeof(out->pattern), NULL)) return 0;
        out->pattern_at = (int)(content - buf);
        out->pattern_len = (int)strlen(out->pattern);
        if (!parse_methods(&p, out, buf)) {
            strudel_line_free(out);
            return 0;
        }
        if (!out->keyword[0] || !out->pattern[0]) {
            strudel_line_free(out);
            out->handled = 0;
            return 0;
        }
        out->handled = 1;
        return 1;
    }
    /* sine.add(saw.slow(4)).range(0,7).segment(8).scale('G0 minor').note() */
    {
        const char *np = p;
        char sname[32];
        if (parse_ident(&np, sname, (int)sizeof(sname)) && sig_leaf_kind(sname)
            && *sws(np) == '.') {
            SigExpr *sig = NULL;
            if (!parse_sig_expr(&p, &sig, 0) || !sig) {
                sig_free(sig);
                return 0;
            }
            if (!parse_methods(&p, out, buf)) {
                sig_free(sig);
                strudel_line_free(out);
                return 0;
            }
            if (!out->keyword[0]) {
                sig_free(sig);
                strudel_line_free(out);
                out->handled = 0;
                return 0;
            }
            out->sig = sig;
            out->handled = 1;
            return 1;
        }
    }
    const char *kind = NULL;
    if (starts(p, "note")) kind = "notes";
    else if (starts(p, "n")) kind = "n";
    else if (starts(p, "sound")) kind = "sound";
    else if (starts(p, "s")) kind = "sound";
    else return 0;
    const char *lp = strchr(p, '(');
    if (!lp) return 0;
    lp++;
    lp = sws(lp);
    if (*lp != '"' && *lp != '\'' && *lp != '`') return 0;
    {
        const char *content = lp + 1;
        if (!take_string(&lp, out->pattern, (int)sizeof(out->pattern), NULL)) return 0;
        out->pattern_at = (int)(content - buf);
        out->pattern_len = (int)strlen(out->pattern);
    }
    /* note("<0 2>".scale('G1 minor')) — methods before the call's ')'. */
    if (!parse_methods(&lp, out, buf)) {
        strudel_line_free(out);
        return 0;
    }
    lp = sws(lp);
    if (*lp != ')') {
        strudel_line_free(out);
        return 0;
    }
    lp++;
    set_keyword(out, kind);
    out->handled = 1;
    if (!parse_methods(&lp, out, buf)) {
        strudel_line_free(out);
        out->handled = 0;
        return 0;
    }
    return 1;
}
