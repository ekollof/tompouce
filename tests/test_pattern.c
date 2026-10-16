#include <stdio.h>
#include <string.h>
#include "pattern.h"
static int fails = 0;
static void show(const char *p, int cyc, int want_n, const char *want_err) {
    Node *n = parse_pattern_text(p);
    const char *e = pattern_last_error();
    PatEventList l; patlist_init(&l);
    expand_node(n, &l, 0.0, 1.0, cyc);
    int ok = (l.n == want_n) &&
             ((want_err == 0 && e[0] == 0) || (want_err && strstr(e, want_err)));
    printf("%-22s cyc%d -> %d evs err='%s' %s\n", p, cyc, l.n, e, ok ? "OK" : "FAIL");
    if (!ok) fails++;
    if (l.n <= 8) { for (int i = 0; i < l.n; i++) printf("    %s@%.2f", l.items[i].token, l.items[i].start); if (l.n) printf("\n"); }
    patlist_free(&l); node_free(n);
}
int main(void) {
    /* prior behavior, must be identical */
    show("[e2 ~]*8", 0, 8, 0);
    show("bd*8", 0, 8, 0);
    show("bd *8", 0, 8, 0);
    show("hh*4", 0, 4, 0);
    show("[bd bd]*2", 0, 4, 0);
    show("<bd sd>*2", 0, 2, 0);
    show("<bd sd>", 0, 1, 0);
    show("[[a b] c]*2", 0, 6, 0);
    show("[a b]", 0, 2, 0);
    show("c4", 0, 1, 0);
    show("bd*100000", 0, 64, 0);
    show("[[[[x]]]]*3", 0, 3, 0);
    show("euclid(3,8,bd)", 0, 3, 0);
    show("euclid(5,8,hh,2)", 0, 5, 0);
    show("every(2,sd)", 0, 1, 0);
    show("every(2,sd)", 1, 0, 0);
    show("every(2,[bd sd])", 2, 2, 0);
    show("euclid(3,8,bd)*2", 0, 6, 0);
    show("bd sd hh", 0, 3, 0);
    show("~ _ rest", 0, 0, 0);
    show("60", 0, 1, 0);
    show("bd2", 0, 1, 0);
    /* error cases: partial results + positioned errors */
    show("euclid(0,4,x)", 0, 0, "euclid args");
    show("euclid(2", 0, 0, "expected");
    show("every(2,sd", 0, 1, "expected )");
    show("[bd sd", 0, 2, "expected ]");
    show("bd sd]", 0, 2, "unexpected");
    show("every(0,x)", 0, 0, "period");
    show("euclid(9,8,x)", 0, 0, "euclid args");
    show("bd * sd", 0, 3, 0);             /* bare * = x2 on bd, then sd */
    show("- -", 0, 0, 0);                 /* Strudel rest alias */
    show("g3,b3,e4", 0, 3, 0);
    show("[g3,b3,e4]", 0, 3, 0);
    show("every(2,[bd,sd])", 0, 2, 0);
    show("every(2,[bd,sd])", 1, 0, 0);
    {
        Node *n = parse_pattern_text("g3,b3,e4");
        PatEventList l; patlist_init(&l);
        expand_node(n, &l, 0.0, 1.0, 0);
        int ok = l.n == 3;
        for (int i = 0; ok && i < l.n; i++)
            if (l.items[i].start > 1e-9 || l.items[i].dur < 0.99) ok = 0;
        printf("%-22s stacked same span %s\n", "g3,b3,e4", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&l); node_free(n);
    }
    printf(fails ? "FAILURES: %d\n" : "ALL PASS\n", fails);
    return fails != 0;
}
