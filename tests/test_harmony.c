#include <stdio.h>
#include <string.h>
#include "harmony.h"

static int fails = 0;

static void expect_n(const char *name, int got, int want) {
    int ok = got == want;
    printf("%-28s %d (want %d) %s\n", name, got, want, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

static void expect_chord(const char *tok, int as_chord, int nwant, const int *midis) {
    int out[8];
    int n = harmony_chord(tok, as_chord, out, 8);
    int ok = n == nwant;
    for (int i = 0; ok && i < n; i++) if (out[i] != midis[i]) ok = 0;
    printf("%-28s n=%d %s\n", tok, n, ok ? "OK" : "FAIL");
    if (!ok) {
        fails++;
        printf("    got");
        for (int i = 0; i < n; i++) printf(" %d", out[i]);
        printf("\n");
    }
}

int main(void) {
    const int cm[] = {48, 51, 55};
    const int cmaj7[] = {48, 52, 55, 59};
    const int am7[] = {57, 60, 64, 67};
    const int bbm7[] = {58, 61, 65, 68};
    const int cdom[] = {48, 52, 55, 58};
    const int c4[] = {60, 64, 67};
    const int cm7[] = {48, 51, 55, 58};
    expect_chord("Cm", 1, 3, cm);
    expect_chord("C^7", 1, 4, cmaj7);
    expect_chord("Am7", 1, 4, am7);
    expect_chord("Bb-7", 1, 4, bbm7);
    expect_n("c7 note", harmony_chord("c7", 0, (int[8]){0}, 8), 0);
    expect_chord("cm7", 0, 4, cm7);
    expect_chord("C", 1, 3, (const int[]){48, 52, 55});
    expect_n("C not a note-track chord", harmony_chord("C", 0, (int[8]){0}, 8), 0);
    expect_chord("C4", 1, 3, c4);
    expect_chord("C7", 1, 4, cdom);

    int steps[12], n = 0, root = 0;
    expect_n("C:minor", harmony_scale("C:minor", &root, steps, 12, &n), 1);
    expect_n("minor root", root, 48);
    expect_n("deg0", harmony_degree_midi(0, 0, root, steps, n), 48);
    expect_n("deg2", harmony_degree_midi(2, 0, root, steps, n), 51);
    expect_n("deg7", harmony_degree_midi(7, 0, root, steps, n), 60);

    expect_n("G1 minor", harmony_scale("G1 minor", &root, steps, 12, &n), 1);
    expect_n("G1 root", root, 31);
    expect_n("G1 deg0", harmony_degree_midi(0, 0, root, steps, n), 31);
    expect_n("G1 deg2", harmony_degree_midi(2, 0, root, steps, n), 34);
    expect_n("G1 deg5", harmony_degree_midi(5, 0, root, steps, n), 39);

    expect_n("G4 minor", harmony_scale("G4 minor", &root, steps, 12, &n), 1);
    expect_n("G4 root", root, 67);
    expect_n("G4 deg0", harmony_degree_midi(0, 0, root, steps, n), 67);
    expect_n("G4 deg5", harmony_degree_midi(5, 0, root, steps, n), 75);

    expect_n("C4:major", harmony_scale("C4:major", &root, steps, 12, &n), 1);
    expect_n("maj root", root, 60);
    expect_n("deg4", harmony_degree_midi(4, 0, root, steps, n), 67);
    expect_n("deg-1", harmony_degree_midi(-1, 0, root, steps, n), 59);

    int deg = 0, acc = 0;
    expect_n("7b degree", harmony_degree("7b", &deg, &acc), 1);
    expect_n("7b acc", acc, -1);
    printf(fails ? "FAILURES: %d\n" : "ALL PASS\n", fails);
    return fails != 0;
}
