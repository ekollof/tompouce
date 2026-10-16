#include "pattern.h"
#include <ctype.h>

Node *node_new(NodeType t) {
    Node *n = calloc(1, sizeof(Node));
    if (!n) return NULL;
    n->type = t;
    n->repeat = 1;
    return n;
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
    strncpy(e->token, tok, 63);
    e->start = start;
    e->dur = dur;
}

/* ---- lexer: pattern text -> tokens with positions ----
   WORD = runs excluding whitespace and []<>() ,*|
   (',' '(' ')' are always delimiters; '/' stays in words for paths) */
typedef enum {
    T_WORD, T_NUM,
    T_LBRACK, T_RBRACK, T_LANGLE, T_RANGLE,
    T_LPAREN, T_RPAREN, T_COMMA, T_STAR, T_BAR, T_EOF
} TokKind;

typedef struct { TokKind kind; char text[64]; int num; int col; } Tok;

#define PTOK_MAX 256

typedef struct {
    Tok toks[PTOK_MAX + 1]; /* [ntoks] is the T_EOF sentinel */
    int ntoks;
    int pos;
    int depth;
    char err[128];
} P;

static int tok_is_delim(char c) {
    return c=='['||c==']'||c=='<'||c=='>'||c=='('||c==')'||c==','||c=='*'||c=='|';
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
        switch (c) {
            case '[': t->kind = T_LBRACK; break;
            case ']': t->kind = T_RBRACK; break;
            case '<': t->kind = T_LANGLE; break;
            case '>': t->kind = T_RANGLE; break;
            case '(': t->kind = T_LPAREN; break;
            case ')': t->kind = T_RPAREN; break;
            case ',': t->kind = T_COMMA; break;
            case '*': t->kind = T_STAR; break;
            case '|': t->kind = T_BAR; break;
            default: {
                /* maximal run; pure digits -> T_NUM (unless letters follow:
                   "bd2" stays one WORD, like before) */
                int j = i;
                int has_letter = 0;
                while (j < len && !isspace((unsigned char)s[j]) && !tok_is_delim(s[j])) {
                    if (isalpha((unsigned char)s[j])) has_letter = 1;
                    j++;
                }
                if (!isdigit((unsigned char)c) || has_letter) {
                    int k = 0;
                    while (i < j && k < 63) t->text[k++] = s[i++];
                    t->text[k] = 0;
                    t->kind = T_WORD;
                } else {
                    int k = 0, v = 0;
                    while (i < j && k < 63) {
                        t->text[k++] = s[i];
                        if (v < 1000000) v = v * 10 + (s[i] - '0');
                        i++;
                    }
                    t->text[k] = 0;
                    t->kind = T_NUM;
                    t->num = v;
                }
                break;
            }
        }
        if (t->kind != T_WORD && t->kind != T_NUM) {
            t->text[0] = c; t->text[1] = 0;
            i++;
        }
        p->ntoks++;
    }
    p->toks[p->ntoks].kind = T_EOF;
    p->toks[p->ntoks].text[0] = 0;
    p->toks[p->ntoks].col = len;
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

static void parse_suffix(P *p, Node *n) {
    if (!n) return;
    if (peek(p)->kind != T_STAR) return;
    next(p);
    int r = 2;
    if (peek(p)->kind == T_NUM) {
        r = peek(p)->num;
        next(p);
        if (r < 1) r = 2;
        if (r > 64) r = 64;
    }
    n->repeat = r;
}

/* Bjorklund E(k,n) rotated right by rot */
static void bjorklund(int k, int n, int rot, int *out) {
    for (int i = 0; i < n; i++)
        out[i] = (((i * k) % n) < k) ? 1 : 0;
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
    if (peek(p)->kind == T_WORD || peek(p)->kind == T_NUM) {
        snprintf(tok, sizeof(tok), "%s", peek(p)->text);
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

static Node *parse_item(P *p) {
    Tok *t = peek(p);
    if (t->kind == T_WORD) {
        Tok *t2 = (p->pos + 1 <= p->ntoks) ? &p->toks[p->pos + 1] : NULL;
        if (t2 && t2->kind == T_LPAREN && strcmp(t->text, "euclid") == 0) {
            Node *e = parse_euclid(p);
            if (!e) return NULL;
            parse_suffix(p, e);
            return e;
        }
        if (t2 && t2->kind == T_LPAREN && strcmp(t->text, "every") == 0) {
            Node *e = parse_every(p);
            if (!e) return NULL;
            parse_suffix(p, e);
            return e;
        }
        next(p);
        Node *a = node_new(NODE_ATOM);
        if (!a) return NULL;
        a->token = strdup_safe(t->text);
        parse_suffix(p, a);
        return a;
    }
    if (t->kind == T_NUM) {
        next(p);
        Node *a = node_new(NODE_ATOM);
        if (!a) return NULL;
        a->token = strdup_safe(t->text);
        parse_suffix(p, a);
        return a;
    }
    if (t->kind == T_LBRACK) {
        next(p);
        if (p->depth >= 32) {
            perr(p, "nesting too deep");
            skip_balanced(p, T_LBRACK, T_RBRACK);
            Node *e = node_new(NODE_SEQ);
            parse_suffix(p, e);
            return e;
        }
        Node *inner = parse_seq(p, T_RBRACK);
        if (!inner) return NULL;
        if (peek(p)->kind == T_RBRACK) next(p);
        else perr(p, "expected ]");
        parse_suffix(p, inner);
        return inner;
    }
    if (t->kind == T_LANGLE) {
        next(p);
        if (p->depth >= 32) {
            perr(p, "nesting too deep");
            skip_balanced(p, T_LANGLE, T_RANGLE);
            Node *e = node_new(NODE_SEQ);
            parse_suffix(p, e);
            return e;
        }
        Node *inner = parse_seq(p, T_RANGLE);
        if (!inner) return NULL;
        /* seq children become the alternation set */
        Node *alt = node_new(NODE_ALT);
        if (!alt) { node_free(inner); return NULL; }
        for (int i = 0; i < inner->nchildren; i++)
            node_add(alt, inner->children[i]);
        free(inner->children);
        inner->children = NULL;
        inner->nchildren = 0;
        node_free(inner);
        if (peek(p)->kind == T_RANGLE) next(p);
        else perr(p, "expected >");
        parse_suffix(p, alt);
        return alt;
    }
    return NULL;
}

/* Comma stacks layers over the same span (Strudel "c,e,g" / "[c,e,g]"). */
static Node *parse_seq(P *p, int closer) {
    if (p->depth >= 32) {
        perr(p, "nesting too deep");
        return NULL;
    }
    p->depth++;
    Node *layers[8];
    int nlayers = 0;
    Node *seq = node_new(NODE_SEQ);
    if (!seq) { p->depth--; return NULL; }
    for (;;) {
        TokKind k = peek(p)->kind;
        if (k == T_EOF || k == T_BAR) break;
        if (closer >= 0 && k == (TokKind)closer) break;
        if (k == T_COMMA) {
            next(p);
            if (nlayers < 7) layers[nlayers++] = seq;
            else node_free(seq);
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
        if (seq) node_add(seq, a);
    }
    p->depth--;
    if (nlayers == 0) {
        if (!seq || seq->nchildren == 0) { node_free(seq); return node_new(NODE_SEQ); }
        return seq;
    }
    if (seq && nlayers < 8) layers[nlayers++] = seq;
    else node_free(seq);
    Node *st = node_new(NODE_STACK);
    if (!st) {
        for (int i = 0; i < nlayers; i++) node_free(layers[i]);
        return node_new(NODE_SEQ);
    }
    for (int i = 0; i < nlayers; i++) {
        if (layers[i] && layers[i]->nchildren > 0) node_add(st, layers[i]);
        else node_free(layers[i]);
    }
    if (st->nchildren == 0) { node_free(st); return node_new(NODE_SEQ); }
    return st;
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

void expand_node(Node *node, PatEventList *out, double start, double dur, int cycle_index) {
    if (!node) return;
    if (node->type == NODE_SEQ) {
        int r = node->repeat < 1 ? 1 : node->repeat;
        if (r > 64) r = 64;
        double sub = dur / r;
        for (int k = 0; k < r; k++) {
            int n = node->nchildren;
            if (n == 0) return;
            double slot = sub / n;
            for (int i = 0; i < n; i++)
                expand_node(node->children[i], out, start + k*sub + i*slot, slot, cycle_index);
        }
        return;
    }
    if (node->type == NODE_ALT) {
        int n = node->nchildren;
        if (n == 0) return;
        int idx = cycle_index % n;
        if (idx < 0) idx = 0;
        int r = node->repeat < 1 ? 1 : node->repeat;
        if (r > 64) r = 64;
        double sub = dur / r;
        for (int k = 0; k < r; k++)
            expand_node(node->children[idx], out, start + k*sub, sub, cycle_index);
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
            patlist_push(out, node->token, s, subdur);
        }
    }
}