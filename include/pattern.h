#ifndef PATTERN_H
#define PATTERN_H
#include "common.h"

typedef enum { NODE_SEQ, NODE_ALT, NODE_ATOM, NODE_EVERY, NODE_STACK } NodeType;

typedef struct Node {
    NodeType type;
    char *token;
    struct Node **children;
    int nchildren;
    int capchildren;
    int repeat;   // *n
    int every_n;  // NODE_EVERY period in cycles
} Node;

Node *node_new(NodeType t);
void node_add(Node *parent, Node *child);
void node_free(Node *n);
Node *parse_pattern_text(const char *s);
/* last parse error ("" when clean); valid until next parse call */
const char *pattern_last_error(void);

/* Expand node into events for one cycle. Each event: token, start fraction [0,1), dur fraction. */
typedef struct { char token[64]; double start; double dur; } PatEvent;
typedef struct { PatEvent *items; int n; int cap; } PatEventList;
void patlist_init(PatEventList *l);
void patlist_free(PatEventList *l);
void patlist_push(PatEventList *l, const char *tok, double start, double dur);
void expand_node(Node *node, PatEventList *out, double start, double dur, int cycle_index);

#endif