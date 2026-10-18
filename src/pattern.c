#include "pattern.h"
#include <ctype.h>

Node *node_new(NodeType t) {
    Node *n = calloc(1, sizeof(Node));
    if (!n) return NULL;
    n->type = t;
    n->repeat = 1;
    n->replicate = 1;
    n->weight = 1;
    n->degrade = -1.0;
    n->fast = 1.0;
    n->slow = 1.0;
    n->src = -1;
    return n;
}
static Node *node_clone(const Node *n) {
    if (!n) return NULL;
    Node *c = node_new(n->type);
    if (!c) return NULL;
    if (n->token) c->token = strdup_safe(n->token);
    c->repeat = n->repeat;
    c->every_n = n->every_n;
    c->replicate = 1;
    c->weight = n->weight;
    c->degrade = n->degrade;
    c->fast = n->fast;
    c->slow = n->slow;
    c->aux = n->aux;
    c->poly_steps = n->poly_steps;
    c->steps_source = n->steps_source;
    c->src = n->src;
    c->src_n = n->src_n;
    if (n->steps_pat) c->steps_pat = node_clone(n->steps_pat);
    for (int i = 0; i < n->nchildren; i++) {
        Node *ch = node_clone(n->children[i]);
        if (ch) node_add(c, ch);
    }
    return c;
}
void node_add(Node *parent, Node *child) {
    if (!parent || !child) return;
    if (parent->nchildren == parent->capchildren) {
        int newcap = parent->capchildren ? parent->capchildren*2 : 8;
        Node **np = realloc(parent->children, newcap*sizeof(Node*));
        if (!np) return;
        parent->children = np;
        parent->capchildren = newcap;
    }
    parent->children[parent->nchildren++] = child;
}
void node_free(Node *n) {
    if (!n) return;
    for (int i = 0; i < n->nchildren; i++) node_free(n->children[i]);
    free(n->children);
    free(n->token);
    node_free(n->steps_pat);
    free(n);
}

void patlist_init(PatEventList *l) { memset(l,0,sizeof(*l)); }
void patlist_free(PatEventList *l) { free(l->items); memset(l,0,sizeof(*l)); }
void patlist_push(PatEventList *l, const char *tok, double start, double dur) {
    if (l->n == l->cap) {
        int newcap = l->cap ? l->cap*2 : 16;
        PatEvent *np = realloc(l->items, newcap*sizeof(PatEvent));
        if (!np) return;
        l->items = np;
        l->cap = newcap;
    }
    PatEvent *e = &l->items[l->n++];
    memset(e,0,sizeof(*e));
    if (tok) strncpy(e->token, tok, 63);
    e->start = start;
    e->dur = dur;
    e->src = -1;
    e->pan = 2.0; /* inherit */
    e->inst = -1;
}

static void push_span(PatEventList *out, const char *tok, double start, double dur,
                      int src, int src_n) {
    patlist_push(out, tok, start, dur);
    if (!out || out->n < 1) return;
    out->items[out->n - 1].src = src;
    out->items[out->n - 1].src_n = src_n;
}

/* ---- lexer: pattern text -> tokens with positions ----
   WORD = runs excluding whitespace and []<>(){} ,*|!@?|/:%
   '/' '!' '@' '?' ':' '%' are Strudel modifiers. '|' inside a pattern
   is choice (the track compiler strips the transform bar first).
   '.' stays inside a word (a.b, 2.5). A lone '.' is a foot. '..' is range.
   '^' starts a token only as the stepwise mark; a^b stays one word.
   '_' stays inside a word (bd_2). A separate _ or _N is weight. */
typedef enum {
    T_WORD, T_NUM,
    T_LBRACK, T_RBRACK, T_LANGLE, T_RANGLE, T_LBRACE, T_RBRACE,
    T_LPAREN, T_RPAREN, T_COMMA, T_STAR, T_SLASH, T_BANG, T_AT, T_QUERY,
    T_BAR, T_COLON, T_PCT, T_DOT, T_DOTDOT, T_CARET, T_EOF
} TokKind;

typedef struct { TokKind kind; char text[64]; int num; double real; int col; } Tok;

static void atom_span(Node *a, const Tok *t) {
    if (!a || !t) return;
    a->src = t->col;
    a->src_n = (int)strlen(t->text);
}

#define PTOK_MAX 1024

typedef struct {
    Tok toks[PTOK_MAX + 1]; /* [ntoks] is the T_EOF sentinel */
    int ntoks;
    int pos;
    int depth;
    char err[128];
} P;

static int tok_is_delim(char c) {
    return c=='['||c==']'||c=='<'||c=='>'||c=='{'||c=='}'||c=='('||c==')'
        ||c==','||c=='*'||c=='/'||c=='!'||c=='@'||c=='?'||c=='|'
        ||c==':'||c=='%';
}

static void plex(P *p, const char *s) {
    p->ntoks = 0; p->pos = 0; p->depth = 0; p->err[0] = 0;
    int len = (int)strlen(s);
    int i = 0;
    while (i < len && p->ntoks < PTOK_MAX) {
        char c = s[i];
        if (isspace((unsigned char)c)) { i++; continue; }
        Tok *t = &p->toks[p->ntoks];
        t->col = i;
        t->num = 0;
        t->real = 0;
        switch (c) {
            case '[': t->kind = T_LBRACK; break;
            case ']': t->kind = T_RBRACK; break;
            case '<': t->kind = T_LANGLE; break;
            case '>': t->kind = T_RANGLE; break;
            case '{': t->kind = T_LBRACE; break;
            case '}': t->kind = T_RBRACE; break;
            case '(': t->kind = T_LPAREN; break;
            case ')': t->kind = T_RPAREN; break;
            case ',': t->kind = T_COMMA; break;
            case '*': t->kind = T_STAR; break;
            case '/': t->kind = T_SLASH; break;
            case '!': t->kind = T_BANG; break;
            case '@': t->kind = T_AT; break;
            case '?': t->kind = T_QUERY; break;
            case '|': t->kind = T_BAR; break;
            case ':': t->kind = T_COLON; break;
            case '%': t->kind = T_PCT; break;
            default: {
                /* lone '.' is a foot; '..' is a range. A leading '^' is the
                   stepwise mark. Otherwise a maximal run. Pure number
                   (optional one dot) -> T_NUM. "bd2" and "a.b" stay words.
                   The run stops before '..' so 0..4 is three tokens. */
                if (c == '.' && i + 1 < len && s[i + 1] == '.') {
                    t->kind = T_DOTDOT;
                    t->text[0] = '.'; t->text[1] = '.'; t->text[2] = 0;
                    i += 2;
                    break;
                }
                if (c == '.' && (i + 1 >= len || !isdigit((unsigned char)s[i + 1]))) {
                    t->kind = T_DOT;
                    break;
                }
                if (c == '^') {
                    t->kind = T_CARET;
                    break;
                }
                int j = i;
                int has_letter = 0, dots = 0, digits = 0;
                while (j < len && !isspace((unsigned char)s[j]) && !tok_is_delim(s[j])) {
                    if (s[j] == '.' && j + 1 < len && s[j + 1] == '.') break;
                    unsigned char cj = (unsigned char)s[j];
                    if (isalpha(cj) || cj >= 128) has_letter = 1;
                    else if (cj == '.') dots++;
                    else if (isdigit(cj)) digits++;
                    j++;
                }
                int numeric = !has_letter && digits > 0 && dots <= 1
                    && (isdigit((unsigned char)c) || c == '.');
                if (!numeric) {
                    int k = 0;
                    while (i < j && k < 63) t->text[k++] = s[i++];
                    t->text[k] = 0;
                    t->kind = T_WORD;
                } else {
                    int k = 0;
                    while (i < j && k < 63) t->text[k++] = s[i++];
                    t->text[k] = 0;
                    t->kind = T_NUM;
                    t->real = strtod(t->text, NULL);
                    if (t->real > 1000000) t->real = 1000000;
                    if (t->real < -1000000) t->real = -1000000;
                    t->num = (int)t->real;
                }
                break;
            }
        }
        if (t->kind == T_DOTDOT) {
            /* text and i already consumed both dots */
        } else if (t->kind != T_WORD && t->kind != T_NUM) {
            t->text[0] = c; t->text[1] = 0;
            i++;
        }
        p->ntoks++;
    }
    p->toks[p->ntoks].kind = T_EOF;
    p->toks[p->ntoks].text[0] = 0;
    p->toks[p->ntoks].col = len;
    p->toks[p->ntoks].real = 0;
    if (i < len && !p->err[0])
        snprintf(p->err, sizeof(p->err), "pattern too long at col %d", i);
}

/* ---- recursive descent over tokens (forgiving: skip + continue) ---- */
static Tok *peek(P *p) { return &p->toks[p->pos]; }
static Tok *next(P *p) {
    Tok *t = peek(p);
    if (t->kind != T_EOF) p->pos++;
    return t;
}
static void perr(P *p, const char *msg) {
    if (p->err[0]) return; /* first error wins */
    snprintf(p->err, sizeof(p->err), "%s at col %d", msg, peek(p)->col);
}

static Node *parse_seq(P *p, int closer);
static Node *parse_item(P *p);
static void elongate_node(Node *n);

/* skip a balanced group after a depth overrun (opener already consumed) */
static void skip_balanced(P *p, TokKind open, TokKind close) {
    int d = 1;
    for (;;) {
        TokKind k = peek(p)->kind;
        if (k == T_EOF || k == T_BAR) return;
        if (k == open) d++;
        else if (k == close) { d--; if (d == 0) { next(p); return; } }
        next(p);
    }
}

static void bjorklund(int k, int n, int rot, int *out);
static int parse_snum(P *p, int *out);
static int expect(P *p, TokKind k, const char *what);

static int whole_num(const Tok *t, int *out) {
    if (!t || t->kind != T_NUM) return 0;
    if (fabs(t->real - round(t->real)) > 1e-6) return 0;
    *out = (int)round(t->real);
    return 1;
}

/* Hits of proto, rests elsewhere. proto is consumed (one hit keeps it). */
static Node *euclid_over(Node *proto, int k, int n, int rot) {
    int pat[64];
    bjorklund(k, n, rot, pat);
    Node *seq = node_new(NODE_SEQ);
    if (!seq) { node_free(proto); return NULL; }
    Node *src = NULL;
    for (int i = 0; i < n; i++) {
        if (pat[i] && (proto || src)) {
            if (!src) { node_add(seq, proto); src = proto; proto = NULL; }
            else {
                Node *c = node_clone(src);
                if (c) node_add(seq, c);
            }
        } else {
            Node *a = node_new(NODE_ATOM);
            if (!a) break;
            a->token = strdup_safe("~");
            node_add(seq, a);
        }
    }
    node_free(proto);
    return seq;
}

/* (old ?? 1) + (n ?? 2) - 1, capped. Bare @ / _ / ! adds one. */
static int bump_count(int old, int has_n, int num) {
    int base = old < 1 ? 1 : old;
    int add = has_n ? num : 2;
    if (add < 1) add = 1;
    if (add > 64) add = 64;
    long w = (long)base + add - 1;
    if (w < 1) w = 1;
    if (w > 64) w = 64;
    return (int)w;
}

/* Separate "_" or "_N" is weight. "bd_2" never reaches here (one word). */
static int weight_word(const Tok *t, int *has, int *num) {
    if (!t || t->kind != T_WORD || t->text[0] != '_') return 0;
    if (t->text[1] == 0) { *has = 0; *num = 0; return 1; }
    char *end = NULL;
    long v = strtol(t->text + 1, &end, 10);
    if (end == t->text + 1 || (end && *end != 0)) return 0;
    if (v < 1) v = 1;
    if (v > 64) v = 64;
    *has = 1;
    *num = (int)v;
    return 1;
}

static int plain_number(const Node *n, double *out) {
    if (!n || n->type != NODE_ATOM || !n->token) return 0;
    if (n->repeat != 1 || n->weight != 1 || n->replicate != 1) return 0;
    if (n->degrade >= 0 || n->fast > 1.0001 || n->slow > 1.0001) return 0;
    if (n->fast > 0 && n->fast < 0.999) return 0;
    char *end = NULL;
    double x = strtod(n->token, &end);
    if (end == n->token || (end && *end != 0) || !isfinite(x)) return 0;
    *out = x;
    return 1;
}

static int plain_int(const Node *n, int *out) {
    double x;
    if (!plain_number(n, &x)) return 0;
    if (fabs(x - round(x)) > 1e-6) return 0;
    *out = (int)round(x);
    return 1;
}

static void apply_number_rate(Node *n, int slash, double r) {
    if (r < 0.01) r = 2;
    if (r > 64) r = 64;
    if (!slash && fabs(r - round(r)) < 1e-6) {
        int ir = (int)round(r);
        if (ir < 1) ir = 2;
        long m = (long)n->repeat * ir;
        if (m > 64) m = 64;
        if (m < 1) m = 1;
        n->repeat = (int)m;
    } else if (slash) {
        n->slow *= r;
        if (n->slow > 64) n->slow = 64;
    } else {
        n->fast *= r;
        if (n->fast > 64) n->fast = 64;
    }
}

static Node *wrap_warp(Node *n, Node *fac, int slash) {
    Node *w = node_new(NODE_WARP);
    if (!w) { node_free(fac); return n; }
    w->aux = slash ? 1 : 0;
    node_add(w, n);
    node_add(w, fac);
    return w;
}

static void join_tail_token(Node *n, const Node *tail) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s:%s", n->token ? n->token : "",
             (tail && tail->token) ? tail->token : "");
    free(n->token);
    n->token = strdup_safe(buf);
    if (n->src >= 0 && tail && tail->src >= 0 && tail->src + tail->src_n > n->src)
        n->src_n = tail->src + tail->src_n - n->src;
}

/* Constant integer ends become a sequence of number atoms. */
static Node *lower_range(Node *lo, Node *hi) {
    int a, b;
    if (!plain_int(lo, &a) || !plain_int(hi, &b)) return NULL;
    int step = (b >= a) ? 1 : -1;
    int count = abs(b - a) + 1;
    if (count > 64) count = 64;
    Node *seq = node_new(NODE_SEQ);
    if (!seq) return NULL;
    for (int i = 0; i < count; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", a + i * step);
        Node *atom = node_new(NODE_ATOM);
        if (!atom) break;
        atom->token = strdup_safe(buf);
        if (lo->src >= 0 && hi->src >= 0 && hi->src + hi->src_n >= lo->src) {
            atom->src = lo->src;
            atom->src_n = hi->src + hi->src_n - lo->src;
        }
        node_add(seq, atom);
    }
    return seq;
}

static Node *parse_suffix(P *p, Node *n) {
    if (!n) return NULL;
    for (;;) {
        TokKind k = peek(p)->kind;
        int whas = 0, wnum = 0;
        if (k == T_STAR || k == T_SLASH) {
            int slash = (k == T_SLASH);
            next(p);
            TokKind fk = peek(p)->kind;
            int grouped = (fk == T_LBRACK || fk == T_LANGLE || fk == T_LBRACE);
            if (fk == T_NUM || grouped) {
                Node *fac = parse_item(p);
                double r;
                if (!fac) break;
                if (plain_number(fac, &r)) {
                    apply_number_rate(n, slash, r);
                    node_free(fac);
                } else {
                    n = wrap_warp(n, fac, slash);
                }
            } else {
                apply_number_rate(n, slash, 2);
            }
        } else if (k == T_BANG || k == T_AT || weight_word(peek(p), &whas, &wnum)) {
            int at = (k == T_AT || k == T_WORD);
            if (k == T_WORD) next(p);
            else {
                next(p);
                whas = 0;
                if (whole_num(peek(p), &wnum)) { whas = 1; next(p); }
            }
            if (wnum < 1) wnum = 1;
            if (wnum > 64) wnum = 64;
            if (at) n->weight = bump_count(n->weight, whas, wnum);
            else n->replicate = bump_count(n->replicate, whas, wnum);
        } else if (k == T_QUERY) {
            next(p);
            double d = 0.5;
            if (peek(p)->kind == T_NUM) { d = peek(p)->real; next(p); }
            if (d < 0) d = 0;
            if (d > 1) d = 1;
            n->degrade = d;
        } else if (k == T_COLON) {
            next(p);
            Node *tail = parse_item(p);
            if (!tail) break;
            if (n->type == NODE_ATOM && tail->type == NODE_ATOM && tail->token &&
                n->repeat == 1 && tail->repeat == 1) {
                join_tail_token(n, tail);
                node_free(tail);
            } else {
                Node *t = node_new(NODE_TAIL);
                if (!t) { node_free(tail); break; }
                node_add(t, n);
                node_add(t, tail);
                n = t;
            }
        } else if (k == T_DOTDOT) {
            next(p);
            Node *hi = parse_item(p);
            if (!hi) break;
            Node *seq = lower_range(n, hi);
            if (seq) {
                node_free(n);
                node_free(hi);
                n = seq;
            } else {
                Node *r = node_new(NODE_RANGE);
                if (!r) { node_free(hi); break; }
                node_add(r, n);
                node_add(r, hi);
                n = r;
            }
        } else if (k == T_LPAREN) {
            next(p);
            if (p->depth >= 32) {
                perr(p, "nesting too deep");
                node_free(n);
                return NULL;
            }
            p->depth++;
            Node *pulse = parse_item(p);
            p->depth--;
            if (!pulse || !expect(p, T_COMMA, "expected ,")) {
                node_free(pulse);
                node_free(n);
                return NULL;
            }
            p->depth++;
            Node *steps = parse_item(p);
            p->depth--;
            if (!steps) { node_free(pulse); node_free(n); return NULL; }
            Node *rot = NULL;
            if (peek(p)->kind == T_COMMA) {
                next(p);
                p->depth++;
                rot = parse_item(p);
                p->depth--;
                if (!rot) {
                    perr(p, "expected rotation");
                    node_free(pulse); node_free(steps); node_free(n);
                    return NULL;
                }
            }
            if (!expect(p, T_RPAREN, "expected )")) {
                node_free(pulse); node_free(steps); node_free(rot); node_free(n);
                return NULL;
            }
            int hits, nsteps, rnum = 0;
            int const_args = plain_int(pulse, &hits) && plain_int(steps, &nsteps)
                && (!rot || plain_int(rot, &rnum));
            if (const_args) {
                if (hits < 1 || nsteps < 1 || hits > nsteps || nsteps > 64) {
                    perr(p, "bad euclid args (need 1<=k<=n<=64)");
                    node_free(pulse); node_free(steps); node_free(rot); node_free(n);
                    return NULL;
                }
                node_free(pulse); node_free(steps); node_free(rot);
                n = euclid_over(n, hits, nsteps, rnum);
                if (!n) return NULL;
            } else {
                Node *e = node_new(NODE_EUCLID);
                if (!e) {
                    node_free(pulse); node_free(steps); node_free(rot); node_free(n);
                    return NULL;
                }
                node_add(e, n);
                node_add(e, pulse);
                node_add(e, steps);
                if (rot) node_add(e, rot);
                n = e;
            }
        } else break;
    }
    return n;
}

/* Strudel bjorklund (packages/core/euclid.mjs), then rotate(pattern, -rot).
   The Bresenham form ((i*k)%n)<k is a different necklace: E(5,8) must be
   x.xx.xx. and E(5,8,-1) must be .xx.xx.x. */
static void bjorklund(int k, int n, int rot, int *out) {
    typedef struct { int len; unsigned char b[64]; } Grp;
    Grp on[64], off[64];
    int non, noff, pos;
    if (n < 1) return;
    if (n > 64) n = 64;
    if (k < 0) k = 0;
    if (k > n) k = n;
    non = k;
    noff = n - k;
    for (int i = 0; i < non; i++) { on[i].len = 1; on[i].b[0] = 1; }
    for (int i = 0; i < noff; i++) { off[i].len = 1; off[i].b[0] = 0; }
    while (non > 1 && noff > 1) {
        if (non > noff) {
            int left = non - noff;
            for (int i = 0; i < noff; i++) {
                Grp *a = &on[i];
                for (int j = 0; j < off[i].len && a->len < 64; j++)
                    a->b[a->len++] = off[i].b[j];
            }
            for (int i = 0; i < left; i++) off[i] = on[noff + i];
            non = noff;
            noff = left;
        } else {
            int left = noff - non;
            for (int i = 0; i < non; i++) {
                Grp *a = &on[i];
                for (int j = 0; j < off[i].len && a->len < 64; j++)
                    a->b[a->len++] = off[i].b[j];
            }
            for (int i = 0; i < left; i++) off[i] = off[non + i];
            noff = left;
        }
    }
    pos = 0;
    for (int i = 0; i < non && pos < n; i++)
        for (int j = 0; j < on[i].len && pos < n; j++) out[pos++] = on[i].b[j];
    for (int i = 0; i < noff && pos < n; i++)
        for (int j = 0; j < off[i].len && pos < n; j++) out[pos++] = off[i].b[j];
    while (pos < n) out[pos++] = 0;
    /* rotate(arr, -rot): element i moves to (i - rot) mod n. */
    rot %= n;
    if (rot < 0) rot += n;
    if (rot > 0) {
        int tmp[64];
        for (int i = 0; i < n; i++) tmp[(i + rot) % n] = out[i];
        for (int i = 0; i < n; i++) out[i] = tmp[i];
    }
}

/* signed number: NUMBER or WORD holding -?\d+ (for euclid rot) */
static int parse_snum(P *p, int *out) {
    Tok *t = peek(p);
    if (t->kind == T_NUM) { *out = t->num; next(p); return 1; }
    if (t->kind == T_WORD) {
        const char *s = t->text;
        int neg = 0;
        if (*s == '-') { neg = 1; s++; }
        if (*s < '0' || *s > '9') return 0;
        long v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; if (v > 1000000) break; }
        if (*s != 0) return 0;
        if (neg) v = -v;
        *out = (v > 1000000) ? 1000000 : (v < -1000000 ? -1000000 : (int)v);
        next(p);
        return 1;
    }
    return 0;
}

static int expect(P *p, TokKind k, const char *what) {
    if (peek(p)->kind == k) { next(p); return 1; }
    perr(p, what);
    return 0;
}

/* euclid(k,n,tok[,rot]): static expansion to an n-step SEQ */
static Node *parse_euclid(P *p) {
    next(p); /* WORD euclid */
    next(p); /* LPAREN */
    if (peek(p)->kind != T_NUM) { perr(p, "expected hits"); skip_balanced(p, T_LPAREN, T_RPAREN); return NULL; }
    int k = peek(p)->num; next(p);
    if (!expect(p, T_COMMA, "expected ,")) { skip_balanced(p, T_LPAREN, T_RPAREN); return NULL; }
    if (peek(p)->kind != T_NUM) { perr(p, "expected steps"); skip_balanced(p, T_LPAREN, T_RPAREN); return NULL; }
    int n = peek(p)->num; next(p);
    if (!expect(p, T_COMMA, "expected ,")) { skip_balanced(p, T_LPAREN, T_RPAREN); return NULL; }
    char tok[64] = {0};
    int scol = -1, slen = 0;
    if (peek(p)->kind == T_WORD || peek(p)->kind == T_NUM) {
        snprintf(tok, sizeof(tok), "%s", peek(p)->text);
        scol = peek(p)->col;
        slen = (int)strlen(peek(p)->text);
        next(p);
    } else {
        perr(p, "expected sound");
        skip_balanced(p, T_LPAREN, T_RPAREN);
        return NULL;
    }
    int rot = 0;
    if (peek(p)->kind == T_COMMA) {
        next(p);
        if (!parse_snum(p, &rot)) {
            perr(p, "expected rotation");
            skip_balanced(p, T_LPAREN, T_RPAREN);
            return NULL;
        }
    }
    if (peek(p)->kind != T_RPAREN) {
        perr(p, "expected )");
        skip_balanced(p, T_LPAREN, T_RPAREN);
        return NULL;
    }
    next(p);
    if (k < 1 || n < 1 || k > n || n > 64) {
        perr(p, "bad euclid args (need 1<=k<=n<=64)");
        return NULL;
    }
    Node *seq = node_new(NODE_SEQ);
    if (!seq) return NULL;
    int pat[64];
    bjorklund(k, n, rot, pat);
    for (int i = 0; i < n; i++) {
        Node *a = node_new(NODE_ATOM);
        if (!a) break;
        a->token = strdup_safe(pat[i] ? tok : "~");
        if (pat[i] && scol >= 0) { a->src = scol; a->src_n = slen; }
        node_add(seq, a);
    }
    return seq;
}

/* every(n, X): play X only when cycle_index % n == 0; X is item/group */
static Node *parse_every(P *p) {
    next(p); /* WORD every */
    next(p); /* LPAREN */
    if (peek(p)->kind != T_NUM) { perr(p, "expected period"); skip_balanced(p, T_LPAREN, T_RPAREN); return NULL; }
    int n = peek(p)->num; next(p);
    if (!expect(p, T_COMMA, "expected ,")) { skip_balanced(p, T_LPAREN, T_RPAREN); return NULL; }
    if (n < 1 || n > 64) { perr(p, "bad every period (1-64)"); skip_balanced(p, T_LPAREN, T_RPAREN); return NULL; }
    Node *ev = node_new(NODE_EVERY);
    if (!ev) return NULL;
    ev->every_n = n;
    if (p->depth >= 32) {
        perr(p, "nesting too deep");
        node_free(ev);
        skip_balanced(p, T_LPAREN, T_RPAREN);
        return NULL;
    }
    p->depth++;
    Node *child = parse_item(p);
    p->depth--;
    if (!child) {
        node_free(ev);
        skip_balanced(p, T_LPAREN, T_RPAREN);
        return NULL;
    }
    node_add(ev, child);
    if (peek(p)->kind != T_RPAREN) {
        perr(p, "expected )"); /* return partial node anyway (forgiving) */
    } else {
        next(p);
    }
    return ev;
}

static int step_weight(const Node *n) {
    int w = (n && n->weight > 1) ? n->weight : 1;
    if (w > 64) w = 64;
    return w;
}

static int seq_units(const Node *n) {
    if (!n || n->type != NODE_SEQ || n->nchildren < 1) return 1;
    int u = 0;
    for (int i = 0; i < n->nchildren; i++) u += step_weight(n->children[i]);
    if (u < 1) u = 1;
    if (u > 64) u = 64;
    return u;
}

static void elongate_node(Node *n) {
    if (!n) return;
    if (n->type == NODE_STACK) {
        for (int i = 0; i < n->nchildren; i++) elongate_node(n->children[i]);
        return;
    }
    if (n->type != NODE_SEQ) return;
    n->slow *= (double)seq_units(n);
    if (n->slow > 64) n->slow = 64;
}

static Node *parse_item(P *p) {
    Tok *t = peek(p);
    if (t->kind == T_WORD) {
        Tok *t2 = (p->pos + 1 <= p->ntoks) ? &p->toks[p->pos + 1] : NULL;
        if (t2 && t2->kind == T_LPAREN && strcmp(t->text, "euclid") == 0) {
            Node *e = parse_euclid(p);
            if (!e) return NULL;
            return parse_suffix(p, e);
        }
        if (t2 && t2->kind == T_LPAREN && strcmp(t->text, "every") == 0) {
            Node *e = parse_every(p);
            if (!e) return NULL;
            return parse_suffix(p, e);
        }
        next(p);
        Node *a = node_new(NODE_ATOM);
        if (!a) return NULL;
        a->token = strdup_safe(t->text);
        atom_span(a, t);
        return parse_suffix(p, a);
    }
    if (t->kind == T_NUM) {
        next(p);
        Node *a = node_new(NODE_ATOM);
        if (!a) return NULL;
        a->token = strdup_safe(t->text);
        atom_span(a, t);
        return parse_suffix(p, a);
    }
    if (t->kind == T_LBRACK) {
        next(p);
        if (p->depth >= 32) {
            perr(p, "nesting too deep");
            skip_balanced(p, T_LBRACK, T_RBRACK);
            Node *e = node_new(NODE_SEQ);
            return parse_suffix(p, e);
        }
        Node *inner = parse_seq(p, T_RBRACK);
        if (!inner) return NULL;
        if (peek(p)->kind == T_RBRACK) next(p);
        else perr(p, "expected ]");
        return parse_suffix(p, inner);
    }
    if (t->kind == T_LANGLE) {
        next(p);
        if (p->depth >= 32) {
            perr(p, "nesting too deep");
            skip_balanced(p, T_LANGLE, T_RANGLE);
            Node *e = node_new(NODE_SEQ);
            return parse_suffix(p, e);
        }
        Node *inner = parse_seq(p, T_RANGLE);
        if (!inner) return NULL;
        /* <a b c> lasts as many cycles as it has steps (same as [a b c]/3).
           A comma stacks the parts, each slowed by its own length. */
        elongate_node(inner);
        if (peek(p)->kind == T_RANGLE) next(p);
        else perr(p, "expected >");
        return parse_suffix(p, inner);
    }
    if (t->kind == T_LBRACE) {
        next(p);
        if (p->depth >= 32) {
            perr(p, "nesting too deep");
            skip_balanced(p, T_LBRACE, T_RBRACE);
            return parse_suffix(p, node_new(NODE_POLY));
        }
        p->depth++;
        Node *poly = node_new(NODE_POLY);
        Node *cur = node_new(NODE_SEQ);
        if (!poly || !cur) { node_free(poly); node_free(cur); p->depth--; return NULL; }
        for (;;) {
            TokKind k = peek(p)->kind;
            if (k == T_EOF || k == T_RBRACE) break;
            if (k == T_COMMA) {
                next(p);
                node_add(poly, cur);
                cur = node_new(NODE_SEQ);
                if (!cur) break;
                continue;
            }
            Node *a = parse_item(p);
            if (!a) {
                if (!p->err[0]) perr(p, "unexpected token");
                next(p);
                continue;
            }
            int reps = a->replicate < 1 ? 1 : a->replicate;
            if (reps > 64) reps = 64;
            a->replicate = 1;
            if (cur) node_add(cur, a);
            for (int i = 1; i < reps && cur; i++) {
                Node *c = node_clone(a);
                if (c) node_add(cur, c);
            }
        }
        if (cur) node_add(poly, cur);
        p->depth--;
        if (peek(p)->kind == T_RBRACE) next(p);
        else perr(p, "expected }");
        /* {a b, c d}%8 — grid size. A non-number is sampled each cycle. */
        if (peek(p)->kind == T_PCT) {
            next(p);
            if (p->depth >= 32) {
                perr(p, "nesting too deep");
            } else {
                p->depth++;
                Node *sp = parse_item(p);
                p->depth--;
                int fixed;
                if (sp && plain_int(sp, &fixed)) {
                    if (fixed < 1) fixed = 1;
                    if (fixed > 64) fixed = 64;
                    poly->poly_steps = fixed;
                    node_free(sp);
                } else if (sp) {
                    poly->steps_pat = sp;
                }
            }
        }
        return parse_suffix(p, poly);
    }
    return NULL;
}

static void seq_add_reps(Node *seq, Node *a) {
    if (!seq || !a) { node_free(a); return; }
    int reps = a->replicate < 1 ? 1 : a->replicate;
    if (reps > 64) reps = 64;
    a->replicate = 1;
    node_add(seq, a);
    for (int i = 1; i < reps; i++) {
        Node *c = node_clone(a);
        if (c) node_add(seq, c);
    }
}

/* Fold comma-layers into one node. Empties nlayers. */
static Node *fold_layers(Node *seq, Node **layers, int *nlayers) {
    int n = *nlayers;
    if (n == 0) return seq;
    if (seq && n < 8) layers[n++] = seq;
    else node_free(seq);
    *nlayers = 0;
    Node *st = node_new(NODE_STACK);
    if (!st) {
        for (int i = 0; i < n; i++) node_free(layers[i]);
        return node_new(NODE_SEQ);
    }
    for (int i = 0; i < n; i++) {
        if (layers[i] && layers[i]->nchildren > 0) node_add(st, layers[i]);
        else node_free(layers[i]);
    }
    if (st->nchildren == 0) { node_free(st); return node_new(NODE_SEQ); }
    return st;
}

/* '.' splits a sequence into feet. Each foot keeps its own weights
   and the feet share the cycle equally: "a@2 b . c" is not "a@2 b c". */
static Node *collapse_feet(Node *seq, Node **feet, int *nfeet) {
    int n = *nfeet;
    if (n <= 0) return seq;
    if (n < 8) feet[n++] = seq;
    else node_free(seq);
    *nfeet = 0;
    Node *outer = node_new(NODE_SEQ);
    if (!outer) {
        for (int i = 0; i < n; i++) node_free(feet[i]);
        return node_new(NODE_SEQ);
    }
    for (int i = 0; i < n; i++) {
        if (feet[i] && feet[i]->nchildren > 0) node_add(outer, feet[i]);
        else node_free(feet[i]);
    }
    if (outer->nchildren == 0) { node_free(outer); return node_new(NODE_SEQ); }
    if (outer->nchildren == 1) {
        Node *only = outer->children[0];
        outer->children[0] = NULL;
        outer->nchildren = 0;
        node_free(outer);
        return only;
    }
    return outer;
}

/* Comma stacks layers over the same span (Strudel "c,e,g").
   '|' chooses one branch per cycle ("a | b").
   '.' is feet. A leading '^' marks the sequence stepwise. */
static Node *parse_seq(P *p, int closer) {
    if (p->depth >= 32) {
        perr(p, "nesting too deep");
        return NULL;
    }
    p->depth++;
    Node *layers[8];
    int nlayers = 0;
    int choice = 0;
    Node *branches[8];
    int nb = 0;
    Node *feet[8];
    int nfeet = 0;
    Node *seq = node_new(NODE_SEQ);
    if (!seq) { p->depth--; return NULL; }
    for (;;) {
        TokKind k = peek(p)->kind;
        if (k == T_EOF) break;
        if (closer >= 0 && k == (TokKind)closer) break;
        if (k == T_CARET && seq->nchildren == 0) {
            next(p);
            seq->steps_source = 1;
            continue;
        }
        if (k == T_DOT) {
            next(p);
            if (nfeet < 7) feet[nfeet++] = seq;
            else node_free(seq);
            seq = node_new(NODE_SEQ);
            if (!seq) break;
            continue;
        }
        if (k == T_COMMA || k == T_BAR) {
            if (nfeet > 0) seq = collapse_feet(seq, feet, &nfeet);
        }
        if (k == T_COMMA) {
            next(p);
            if (nlayers < 7) layers[nlayers++] = seq;
            else node_free(seq);
            seq = node_new(NODE_SEQ);
            if (!seq) break;
            continue;
        }
        if (k == T_BAR) {
            next(p);
            choice = 1;
            Node *one = fold_layers(seq, layers, &nlayers);
            if (nb < 8) branches[nb++] = one;
            else node_free(one);
            seq = node_new(NODE_SEQ);
            if (!seq) break;
            continue;
        }
        Node *a = parse_item(p);
        if (!a) {
            if (!p->err[0]) perr(p, "unexpected token");
            next(p); /* skip one, keep grooving */
            continue;
        }
        seq_add_reps(seq, a);
    }
    p->depth--;
    if (nfeet > 0) seq = collapse_feet(seq, feet, &nfeet);
    if (choice) {
        Node *one = fold_layers(seq, layers, &nlayers);
        int empty = one && one->type == NODE_SEQ && one->nchildren == 0 && nlayers == 0;
        if (empty && nb > 0) node_free(one);
        else if (nb < 8) branches[nb++] = one;
        else node_free(one);
        Node *ch = node_new(NODE_CHOICE);
        if (!ch) {
            for (int i = 0; i < nb; i++) node_free(branches[i]);
            return node_new(NODE_SEQ);
        }
        for (int i = 0; i < nb; i++) {
            if (branches[i] && branches[i]->nchildren > 0) node_add(ch, branches[i]);
            else node_free(branches[i]);
        }
        if (ch->nchildren == 0) { node_free(ch); return node_new(NODE_SEQ); }
        return ch;
    }
    if (nlayers == 0) {
        if (!seq || seq->nchildren == 0) { node_free(seq); return node_new(NODE_SEQ); }
        return seq;
    }
    return fold_layers(seq, layers, &nlayers);
}

static char last_pattern_err[128] = {0};

const char *pattern_last_error(void) {
    return last_pattern_err;
}

Node *parse_pattern_text(const char *s) {
    last_pattern_err[0] = 0;
    if (!s) return node_new(NODE_SEQ);
    P p;
    plex(&p, s);
    Node *r = parse_seq(&p, -1);
    if (!r) r = node_new(NODE_SEQ);
    if (p.err[0]) snprintf(last_pattern_err, sizeof(last_pattern_err), "%s", p.err);
    return r;
}

static int node_weight(const Node *n) {
    int w = (n && n->weight > 1) ? n->weight : 1;
    if (w > 64) w = 64;
    return w;
}

static unsigned mix32(unsigned x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* Stable per cycle so the 2s lookahead hears the same events it scheduled. */
static int degrade_drop(int cycle, double start, const char *tok, double p) {
    if (p <= 0) return 0;
    if (p >= 1) return 1;
    unsigned h = mix32((unsigned)(cycle < 0 ? 0 : cycle) * 0x9e3779b9u);
    h = mix32(h ^ (unsigned)(start * 100000.0 + 0.5));
    if (tok && tok[0]) h = mix32(h ^ (unsigned char)tok[0]);
    double u = (h & 0xFFFFFFu) / (double)0x1000000u;
    return u < p;
}

static int choice_index(int cycle, int n) {
    if (n <= 1) return 0;
    unsigned h = mix32((unsigned)(cycle < 0 ? 0 : cycle) * 0x85ebca6bu);
    return (int)(h % (unsigned)n);
}

static void expand_plain(Node *node, PatEventList *out, double start, double dur, int cycle_index);

static int token_number(const char *s, double *out) {
    if (!s || !s[0]) return 0;
    char *end = NULL;
    double x = strtod(s, &end);
    if (end == s || (end && *end != 0) || !isfinite(x)) return 0;
    *out = x;
    return 1;
}

/* Value sounding at the start of this cycle, else the first number. */
static int sample_int(Node *n, int cycle, int *out) {
    int v;
    if (plain_int(n, &v)) { *out = v; return 1; }
    if (!n) return 0;
    PatEventList l;
    patlist_init(&l);
    expand_node(n, &l, 0.0, 1.0, cycle);
    int found = 0;
    int fallback = 0;
    int have_fb = 0;
    for (int i = 0; i < l.n; i++) {
        double x;
        if (!token_number(l.items[i].token, &x)) continue;
        if (fabs(x - round(x)) > 1e-6) continue;
        if (!have_fb) { fallback = (int)round(x); have_fb = 1; }
        if (l.items[i].start <= 1e-6) { found = (int)round(x); have_fb = 2; break; }
    }
    patlist_free(&l);
    if (have_fb == 2) { *out = found; return 1; }
    if (have_fb) { *out = fallback; return 1; }
    return 0;
}

static void expand_with_rate(Node *src, PatEventList *out, double start, double dur,
                             int cycle, double f, int slow) {
    if (!src) return;
    if (f < 0.01) f = 0.01;
    if (f > 64) f = 64;
    int sr = src->repeat;
    double sf = src->fast, ss = src->slow;
    if (slow) {
        if (f < 1) f = 1;
        src->slow *= f;
        if (src->slow > 64) src->slow = 64;
    } else if (fabs(f - round(f)) < 1e-6) {
        int ir = (int)round(f);
        if (ir < 1) ir = 1;
        long m = (long)sr * ir;
        if (m > 64) m = 64;
        if (m < 1) m = 1;
        src->repeat = (int)m;
    } else {
        src->fast *= f;
        if (src->fast > 64) src->fast = 64;
    }
    expand_node(src, out, start, dur, cycle);
    src->repeat = sr;
    src->fast = sf;
    src->slow = ss;
}

static void expand_warp(Node *node, PatEventList *out, double start, double dur, int cycle) {
    Node *src = node->nchildren > 0 ? node->children[0] : NULL;
    Node *fac = node->nchildren > 1 ? node->children[1] : NULL;
    int rep = node->repeat < 1 ? 1 : node->repeat;
    if (rep > 64) rep = 64;
    if (!src) return;
    double sub = dur / rep;
    for (int k = 0; k < rep; k++) {
        double ss = start + k * sub;
        if (!fac) { expand_node(src, out, ss, sub, cycle); continue; }
        PatEventList fl;
        patlist_init(&fl);
        expand_node(fac, &fl, ss, sub, cycle);
        int any = 0;
        for (int i = 0; i < fl.n; i++) {
            double f;
            if (!token_number(fl.items[i].token, &f)) continue;
            expand_with_rate(src, out, fl.items[i].start, fl.items[i].dur, cycle, f, node->aux == 1);
            any = 1;
        }
        if (!any) expand_node(src, out, ss, sub, cycle);
        patlist_free(&fl);
    }
}

static void expand_euclid_node(Node *node, PatEventList *out, double start, double dur, int cycle) {
    if (node->nchildren < 3) return;
    int rep = node->repeat < 1 ? 1 : node->repeat;
    if (rep > 64) rep = 64;
    int hits = 1, steps = 1, rot = 0;
    if (!sample_int(node->children[1], cycle, &hits)) hits = 1;
    if (!sample_int(node->children[2], cycle, &steps)) steps = 1;
    if (node->nchildren > 3) sample_int(node->children[3], cycle, &rot);
    if (steps < 1) steps = 1;
    if (steps > 64) steps = 64;
    if (hits < 1) hits = 1;
    if (hits > steps) hits = steps;
    int pat[64];
    bjorklund(hits, steps, rot, pat);
    double sub = dur / rep;
    for (int r = 0; r < rep; r++) {
        double step = sub / steps;
        for (int i = 0; i < steps; i++) {
            if (!pat[i]) continue;
            expand_node(node->children[0], out, start + r * sub + i * step, step, cycle);
        }
    }
}

static void expand_range(Node *node, PatEventList *out, double start, double dur, int cycle) {
    if (node->nchildren < 2) return;
    int a, b;
    if (!sample_int(node->children[0], cycle, &a)) return;
    if (!sample_int(node->children[1], cycle, &b)) return;
    int step = (b >= a) ? 1 : -1;
    int count = abs(b - a) + 1;
    if (count < 1) return;
    if (count > 64) count = 64;
    double w = dur / count;
    int src = -1, src_n = 0;
    Node *lo = node->children[0], *hi = node->children[1];
    if (lo && hi && lo->src >= 0 && hi->src >= 0 && hi->src + hi->src_n >= lo->src) {
        src = lo->src;
        src_n = hi->src + hi->src_n - lo->src;
    }
    for (int i = 0; i < count; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", a + i * step);
        push_span(out, buf, start + i * w, w, src, src_n);
    }
}

static void expand_tail(Node *node, PatEventList *out, double start, double dur, int cycle) {
    if (node->nchildren < 2) return;
    PatEventList src, tl;
    patlist_init(&src);
    patlist_init(&tl);
    expand_node(node->children[0], &src, start, dur, cycle);
    expand_node(node->children[1], &tl, start, dur, cycle);
    for (int i = 0; i < src.n; i++) {
        double a0 = src.items[i].start;
        double a1 = a0 + src.items[i].dur;
        int any = 0;
        for (int j = 0; j < tl.n; j++) {
            double b0 = tl.items[j].start;
            double b1 = b0 + tl.items[j].dur;
            double c0 = a0 > b0 ? a0 : b0;
            double c1 = a1 < b1 ? a1 : b1;
            if (c1 - c0 <= 1e-9) continue;
            char buf[64];
            snprintf(buf, sizeof(buf), "%.31s:%.31s", src.items[i].token, tl.items[j].token);
            push_span(out, buf, c0, c1 - c0, src.items[i].src, src.items[i].src_n);
            any = 1;
        }
        if (!any) push_span(out, src.items[i].token, a0, src.items[i].dur,
                            src.items[i].src, src.items[i].src_n);
    }
    patlist_free(&src);
    patlist_free(&tl);
}

static void expand_plain(Node *node, PatEventList *out, double start, double dur, int cycle_index) {
    if (!node) return;
    if (node->type == NODE_SEQ) {
        int r = node->repeat < 1 ? 1 : node->repeat;
        if (r > 64) r = 64;
        int units = 0;
        for (int i = 0; i < node->nchildren; i++) units += node_weight(node->children[i]);
        if (units < 1) return;
        double sub = dur / r;
        for (int k = 0; k < r; k++) {
            double pos = 0;
            for (int i = 0; i < node->nchildren; i++) {
                int w = node_weight(node->children[i]);
                double slot = sub * (double)w / (double)units;
                expand_node(node->children[i], out, start + k * sub + pos, slot, cycle_index);
                pos += slot;
            }
        }
        return;
    }
    if (node->type == NODE_ALT || node->type == NODE_CHOICE) {
        int n = node->nchildren;
        if (n == 0) return;
        int r = node->repeat < 1 ? 1 : node->repeat;
        if (r > 64) r = 64;
        double sub = dur / r;
        int total = 0;
        for (int i = 0; i < n; i++) total += node_weight(node->children[i]);
        if (total < 1) return;
        for (int k = 0; k < r; k++) {
            int pos;
            if (node->type == NODE_CHOICE) pos = choice_index(cycle_index, n);
            else pos = (cycle_index < 0 ? 0 : cycle_index) % total;
            if (node->type == NODE_CHOICE) {
                expand_node(node->children[pos], out, start + k * sub, sub, cycle_index);
                continue;
            }
            int acc = 0;
            for (int i = 0; i < n; i++) {
                int w = node_weight(node->children[i]);
                if (pos < acc + w) {
                    if (pos == acc)
                        expand_node(node->children[i], out, start + k * sub, sub * (double)w, cycle_index);
                    break;
                }
                acc += w;
            }
        }
        return;
    }
    if (node->type == NODE_POLY) {
        if (node->nchildren < 1) return;
        int steps = node->poly_steps;
        if (node->steps_pat) {
            int sampled = 0;
            if (sample_int(node->steps_pat, cycle_index, &sampled) && sampled > 0)
                steps = sampled;
        }
        if (steps < 1) steps = seq_units(node->children[0]);
        if (steps < 1) steps = 1;
        if (steps > 64) steps = 64;
        for (int li = 0; li < node->nchildren; li++) {
            Node *layer = node->children[li];
            if (!layer) continue;
            int w = seq_units(layer);
            if (layer->type != NODE_SEQ) w = step_weight(layer);
            if (w < 1) w = 1;
            double rate = (double)steps / (double)w;
            expand_with_rate(layer, out, start, dur, cycle_index, rate, 0);
        }
        return;
    }
    if (node->type == NODE_WARP) {
        expand_warp(node, out, start, dur, cycle_index);
        return;
    }
    if (node->type == NODE_EUCLID) {
        expand_euclid_node(node, out, start, dur, cycle_index);
        return;
    }
    if (node->type == NODE_TAIL) {
        expand_tail(node, out, start, dur, cycle_index);
        return;
    }
    if (node->type == NODE_RANGE) {
        expand_range(node, out, start, dur, cycle_index);
        return;
    }
    if (node->type == NODE_EVERY) {
        int n = node->every_n < 1 ? 2 : node->every_n;
        if (cycle_index % n != 0) return;
        if (node->nchildren == 0) return;
        int r = node->repeat < 1 ? 1 : node->repeat;
        if (r > 64) r = 64;
        double sub = dur / r;
        for (int k = 0; k < r; k++)
            expand_node(node->children[0], out, start + k*sub, sub, cycle_index);
        return;
    }
    if (node->type == NODE_STACK) {
        int r = node->repeat < 1 ? 1 : node->repeat;
        if (r > 64) r = 64;
        double sub = dur / r;
        for (int k = 0; k < r; k++) {
            for (int i = 0; i < node->nchildren; i++)
                expand_node(node->children[i], out, start + k * sub, sub, cycle_index);
        }
        return;
    }
    if (node->type == NODE_ATOM) {
        if (!node->token || strcmp(node->token,"~")==0 || strcmp(node->token,"_")==0 ||
            strcmp(node->token,"-")==0 || strcmp(node->token,"rest")==0)
            return;
        int r = node->repeat < 1 ? 1 : node->repeat;
        double subdur = dur / r;
        for (int i = 0; i < r; i++) {
            double s = start + i*subdur;
            push_span(out, node->token, s, subdur, node->src, node->src_n);
        }
    }
}

void expand_node(Node *node, PatEventList *out, double start, double dur, int cycle_index) {
    if (!node) return;
    int special = (node->degrade >= 0.0) || (node->slow > 1.0001)
        || (node->fast > 1.0001) || (node->fast > 0.0 && node->fast < 0.999);
    if (!special) {
        expand_plain(node, out, start, dur, cycle_index);
        return;
    }
    double deg = node->degrade, slow = node->slow, fast = node->fast;
    node->degrade = -1.0;
    node->slow = 1.0;
    node->fast = 1.0;
    PatEventList body;
    patlist_init(&body);
    expand_plain(node, &body, 0.0, 1.0, cycle_index);
    node->degrade = deg;
    node->slow = slow;
    node->fast = fast;

    PatEventList tiled;
    patlist_init(&tiled);
    double rate = fast;
    if (rate < 0.01) rate = 1;
    if (rate > 64) rate = 64;
    if (fabs(rate - 1.0) < 1e-6) {
        for (int i = 0; i < body.n; i++)
            push_span(&tiled, body.items[i].token, body.items[i].start, body.items[i].dur,
                      body.items[i].src, body.items[i].src_n);
    } else {
        int ncopy = (int)ceil(rate - 1e-9);
        if (ncopy < 1) ncopy = 1;
        if (ncopy > 64) ncopy = 64;
        for (int k = 0; k < ncopy; k++) {
            double local = (k + 1 > rate) ? (rate - k) : 1.0;
            if (local <= 0) break;
            double origin = (double)k / rate;
            double win = 1.0 / rate;
            for (int i = 0; i < body.n; i++) {
                if (body.items[i].start >= local) continue;
                double d = body.items[i].dur;
                if (body.items[i].start + d > local) d = local - body.items[i].start;
                if (d <= 1e-9) continue;
                push_span(&tiled, body.items[i].token, origin + body.items[i].start * win, d * win,
                          body.items[i].src, body.items[i].src_n);
            }
        }
    }
    patlist_free(&body);

    double s = slow < 1 ? 1 : slow;
    if (s > 64) s = 64;
    double phase = 0;
    if (s > 1.0001) {
        double cyc = cycle_index < 0 ? 0 : (double)cycle_index;
        phase = fmod(cyc, s);
        if (phase < 0) phase += s;
    }
    double a = (s > 1.0001) ? phase / s : 0;
    double b = (s > 1.0001) ? (phase + 1.0) / s : 1;
    if (b > 1) b = 1;
    double span = (s > 1.0001) ? (1.0 / s) : 1.0;
    for (int i = 0; i < tiled.n; i++) {
        double e0 = tiled.items[i].start;
        double e1 = e0 + tiled.items[i].dur;
        if (e1 <= a + 1e-12 || e0 >= b - 1e-12) continue;
        if (e0 < a) e0 = a;
        if (e1 > b) e1 = b;
        double ns = (e0 - a) / span;
        double nd = (e1 - e0) / span;
        if (ns >= 1.0 || nd <= 1e-9) continue;
        if (degrade_drop(cycle_index, ns, tiled.items[i].token, deg)) continue;
        push_span(out, tiled.items[i].token, start + ns * dur, nd * dur,
                  tiled.items[i].src, tiled.items[i].src_n);
    }
    patlist_free(&tiled);
}