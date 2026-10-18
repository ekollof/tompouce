#include <stdio.h>
#include <string.h>
#include <math.h>
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
    show("c!3 e", 0, 4, 0);          /* replicate splices steps */
    show("c@3 e", 0, 2, 0);          /* elongate: two events, unequal time */
    show("bd(3,8)", 0, 3, 0);
    show("bd(3,8,2)", 0, 3, 0);
    {
        /* Cuban cinquillo and its Strudel rotation, not the Bresenham necklace. */
        struct { const char *pat; double at[5]; int n; } cs[] = {
            { "bd(3,8)", { 0, 0.375, 0.75 }, 3 },
            { "bd(5,8)", { 0, 0.25, 0.375, 0.625, 0.75 }, 5 },
            { "x(5,8,-1)", { 0.125, 0.25, 0.5, 0.625, 0.875 }, 5 },
        };
        for (int c = 0; c < 3; c++) {
            Node *n = parse_pattern_text(cs[c].pat);
            PatEventList l; patlist_init(&l);
            expand_node(n, &l, 0.0, 1.0, 0);
            int ok = l.n == cs[c].n;
            for (int i = 0; ok && i < l.n; i++)
                if (fabs(l.items[i].start - cs[c].at[i]) > 0.02) ok = 0;
            printf("%-22s bjorklund %s\n", cs[c].pat, ok ? "OK" : "FAIL");
            if (!ok) {
                fails++;
                for (int i = 0; i < l.n; i++) printf("    @%.3f\n", l.items[i].start);
            }
            patlist_free(&l); node_free(n);
        }
    }
    show("[c a f e]/2", 0, 2, 0);    /* slow stretches across cycles */
    show("[c a f e]/2", 1, 2, 0);
    show("bd sd*2", 0, 3, 0);
    show("{bd sd hh, cp lt}", 0, 6, 0);
    show("a | b", 0, 1, 0);
    {
        Node *n = parse_pattern_text("c@3 e");
        PatEventList l; patlist_init(&l);
        expand_node(n, &l, 0.0, 1.0, 0);
        int ok = l.n == 2 && l.items[0].dur > 0.7 && l.items[1].dur < 0.3;
        printf("%-22s elongate %.2f/%.2f %s\n", "c@3 e",
            l.n > 0 ? l.items[0].dur : 0, l.n > 1 ? l.items[1].dur : 0, ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&l); node_free(n);
    }
    {
        /* ? is seeded: the same cycle always makes the same cut */
        Node *n = parse_pattern_text("bd sd hh cp?");
        PatEventList a, b; patlist_init(&a); patlist_init(&b);
        expand_node(n, &a, 0.0, 1.0, 3);
        expand_node(n, &b, 0.0, 1.0, 3);
        int ok = a.n == b.n;
        for (int i = 0; ok && i < a.n; i++)
            if (strcmp(a.items[i].token, b.items[i].token) != 0) ok = 0;
        printf("%-22s degrade stable %s\n", "bd sd hh cp?", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&a); patlist_free(&b); node_free(n);
    }
    show("c!! e", 0, 4, 0);          /* !! adds one each: 3 copies, not 4 */
    show("c!!! e", 0, 5, 0);         /* !!! == !4 */
    show("hh:1", 0, 1, 0);
    show("0..4", 0, 5, 0);
    show("4..2", 0, 3, 0);
    show("bd*[2 4]", 0, 6, 0);
    show("bd(<3 5>,8)", 0, 3, 0);
    show("bd(<3 5>,8)", 1, 5, 0);
    show("{bd sd, cp}%4", 0, 8, 0);
    show("a@2 b . c", 0, 3, 0);
    show("^bd sd", 0, 2, 0);
    show("f#4", 0, 1, 0);
    show("a.b", 0, 1, 0);
    show("bd _2 sd", 0, 2, 0);
    show("<bd sd>", 1, 1, 0);
    {
        Node *n = parse_pattern_text("hh:1");
        PatEventList l; patlist_init(&l);
        expand_node(n, &l, 0.0, 1.0, 0);
        int ok = l.n == 1 && strcmp(l.items[0].token, "hh:1") == 0;
        printf("%-22s sample tail %s\n", "hh:1", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&l); node_free(n);
    }
    {
        Node *n = parse_pattern_text("0..4");
        PatEventList l; patlist_init(&l);
        expand_node(n, &l, 0.0, 1.0, 0);
        int ok = l.n == 5;
        const char *want[] = {"0", "1", "2", "3", "4"};
        for (int i = 0; ok && i < 5; i++)
            if (strcmp(l.items[i].token, want[i]) != 0) ok = 0;
        printf("%-22s range %s\n", "0..4", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&l); node_free(n);
    }
    {
        Node *n = parse_pattern_text("<bd sd>*2");
        PatEventList l; patlist_init(&l);
        expand_node(n, &l, 0.0, 1.0, 0);
        int ok = l.n == 2 && strcmp(l.items[0].token, "bd") == 0
            && strcmp(l.items[1].token, "sd") == 0;
        printf("%-22s angle*2 %s\n", "<bd sd>*2", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&l); node_free(n);
    }
    {
        Node *n = parse_pattern_text("<bd sd>");
        PatEventList l; patlist_init(&l);
        expand_node(n, &l, 0.0, 1.0, 1);
        int ok = l.n == 1 && strcmp(l.items[0].token, "sd") == 0;
        printf("%-22s angle cyc1 %s\n", "<bd sd>", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&l); node_free(n);
    }
    {
        Node *n = parse_pattern_text("c@@ e");
        PatEventList l; patlist_init(&l);
        expand_node(n, &l, 0.0, 1.0, 0);
        int ok = l.n == 2 && l.items[0].dur > 0.7;
        printf("%-22s accum @ %.2f %s\n", "c@@ e",
            l.n > 0 ? l.items[0].dur : 0, ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&l); node_free(n);
    }
    {
        Node *n = parse_pattern_text("bd _2 sd");
        PatEventList l; patlist_init(&l);
        expand_node(n, &l, 0.0, 1.0, 0);
        int ok = l.n == 2 && l.items[0].dur > 0.6 && strcmp(l.items[0].token, "bd") == 0;
        printf("%-22s _ weight %.2f %s\n", "bd _2 sd",
            l.n > 0 ? l.items[0].dur : 0, ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&l); node_free(n);
    }
    {
        Node *n = parse_pattern_text("a@2 b . c");
        PatEventList l; patlist_init(&l);
        expand_node(n, &l, 0.0, 1.0, 0);
        int ok = l.n == 3 && l.items[2].dur > 0.45 && strcmp(l.items[2].token, "c") == 0;
        printf("%-22s feet %.2f %s\n", "a@2 b . c",
            l.n > 2 ? l.items[2].dur : 0, ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&l); node_free(n);
    }
    {
        Node *n = parse_pattern_text("bd sd");
        PatEventList l; patlist_init(&l);
        expand_node(n, &l, 0.0, 1.0, 0);
        int ok = l.n == 2 && l.items[0].src == 0 && l.items[0].src_n == 2
            && l.items[1].src == 3 && l.items[1].src_n == 2;
        printf("%-22s src %d,%d %s\n", "bd sd",
            l.n > 1 ? l.items[1].src : -1, l.n > 1 ? l.items[1].src_n : 0,
            ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&l); node_free(n);
    }
    {
        Node *n = parse_pattern_text("<bd sd>");
        PatEventList a, b; patlist_init(&a); patlist_init(&b);
        expand_node(n, &a, 0.0, 1.0, 0);
        expand_node(n, &b, 0.0, 1.0, 1);
        int ok = a.n == 1 && b.n == 1 && a.items[0].src == 1 && a.items[0].src_n == 2
            && b.items[0].src == 4 && b.items[0].src_n == 2;
        printf("%-22s angle src %s\n", "<bd sd>", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&a); patlist_free(&b); node_free(n);
    }
    {
        Node *n = parse_pattern_text("hh*4");
        PatEventList l; patlist_init(&l);
        expand_node(n, &l, 0.0, 1.0, 0);
        int ok = l.n == 4;
        for (int i = 0; ok && i < l.n; i++)
            ok = l.items[i].src == 0 && l.items[i].src_n == 2;
        printf("%-22s repeat src %s\n", "hh*4", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        patlist_free(&l); node_free(n);
    }
    printf(fails ? "FAILURES: %d\n" : "ALL PASS\n", fails);
    return fails != 0;
}
