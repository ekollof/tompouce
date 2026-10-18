#ifndef PATTERN_H
#define PATTERN_H
#include "common.h"

typedef enum {
    NODE_SEQ, NODE_ALT, NODE_ATOM, NODE_EVERY, NODE_STACK,
    NODE_POLY,   /* {a b, c d}%n polymeter: grid from % or the first layer */
    NODE_CHOICE, /* a | b  one branch per cycle, stable for that cycle */
    NODE_EUCLID, /* pat(p,s[,r]) with patterned args. 0 proto, 1 p, 2 s, 3 r */
    NODE_WARP,   /* pat * factor or / factor. 0 source, 1 factor. aux 0 fast, 1 slow */
    NODE_TAIL,   /* pat:tail. 0 source, 1 tail. atom:atom is joined into the token */
    NODE_RANGE   /* a..b integer range, squeezed across the slot */
} NodeType;

typedef struct Node {
    NodeType type;
    char *token;
    struct Node **children;
    int nchildren;
    int capchildren;
    int repeat;    /* *n integer fast, default 1 */
    int every_n;   /* NODE_EVERY period in cycles */
    int replicate; /* !n, spliced into the parent as copies, default 1 */
    int weight;    /* @n / _n temporal weight among siblings, default 1 */
    double degrade;/* <0 off; else probability of dropping each event */
    double fast;   /* fractional * factor, default 1 */
    double slow;   /* /n, stretch across this many cycles, default 1 */
    int aux;       /* NODE_WARP: 0 fast, 1 slow */
    int poly_steps;/* NODE_POLY: >0 fixed % grid; 0 = first layer's weight */
    int steps_source; /* leading ^ : stepwise marker, rhythm unchanged */
    struct Node *steps_pat; /* NODE_POLY %pattern, sampled each cycle */
    int src;       /* byte offset of this atom in the pattern text, -1 if none */
    int src_n;     /* byte length of that atom */
} Node;

Node *node_new(NodeType t);
void node_add(Node *parent, Node *child);
void node_free(Node *n);
Node *parse_pattern_text(const char *s);
/* last parse error ("" when clean); valid until next parse call */
const char *pattern_last_error(void);

/* Expand node into events for one cycle. Each event: token, start fraction [0,1), dur fraction. */
typedef struct {
    char token[64];
    double start;
    double dur;
    int extra;    /* semitones added after the token is parsed */
    float rate;   /* 0 = engine default playback speed */
    float send;   /* 0 = no per-event delay send */
    int src;      /* byte offset in the pattern text, -1 if none */
    int src_n;
    double pan;   /* Strudel 0..1; >1 means inherit the track pan */
    int inst;     /* <0 inherit the track oscillator */
} PatEvent;
typedef struct { PatEvent *items; int n; int cap; } PatEventList;
void patlist_init(PatEventList *l);
void patlist_free(PatEventList *l);
void patlist_push(PatEventList *l, const char *tok, double start, double dur);
void expand_node(Node *node, PatEventList *out, double start, double dur, int cycle_index);

#endif