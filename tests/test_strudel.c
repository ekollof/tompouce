/* Patterns taken from the Strudel workshop and mini-notation pages:
   https://strudel.cc/workshop/first-sounds/
   https://strudel.cc/workshop/first-notes/
   https://strudel.cc/workshop/first-effects/
   https://strudel.cc/workshop/recap/
   https://strudel.cc/learn/mini-notation/
   Counts are what this engine schedules for one cycle. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "strudel.h"
#include "pattern.h"
#include "xform.h"
#include "harmony.h"
#include "event.h"

static int fails = 0;

static int events(const char *pat, int cyc) {
    Node *n = parse_pattern_text(pat);
    PatEventList l;
    patlist_init(&l);
    expand_node(n, &l, 0.0, 1.0, cyc);
    int c = l.n;
    patlist_free(&l);
    node_free(n);
    return c;
}

static void line(const char *src, int cyc, int want, const char *what) {
    StrudelLine s;
    int ok = strudel_parse(src, &s);
    int got = -1;
    if (!ok || !s.handled) ok = 0;
    else if (s.bpm_set) {
        ok = fabs(s.bpm - want) < 0.1;
        got = (int)s.bpm;
    } else {
        got = events(s.pattern, cyc);
        if (s.fast_repeat > 1) got *= s.fast_repeat;
        ok = ok && got == want;
    }
    printf("%-44s -> %d (%s) %s\n", src, got, what, ok ? "OK" : "FAIL");
    if (!ok) fails++;
    strudel_line_free(&s);
}

int main(void) {
    /* first sounds */
    line("sound(\"bd sd\")", 0, 2, "two steps");
    line("sound(\"bd sd [~ bd] sd\")", 0, 4, "groups and rests");
    line("sound(\"bd sd*2 cp*3\")", 0, 6, "star speeds a step");
    line("s(\"bd(3,8)\")", 0, 3, "euclidean postfix");
    {
        StrudelLine s;
        int ok = strudel_parse("$: s(\"hh*4\").bank(\"tr909\")", &s);
        ok = ok && s.handled && strcmp(s.keyword, "sound") == 0
            && strcmp(s.pattern, "hh*4") == 0 && strcmp(s.bank, "tr909") == 0;
        printf("%-44s bank %s\n", "$: s(hh*4).bank(tr909)", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        strudel_line_free(&s);
    }
    /* first notes */
    line("note(\"48 52 55 59\")", 0, 4, "midi numbers");
    line("note(\"c e g b\")", 0, 4, "letter notes");
    line("note(\"c@3 e\")", 0, 2, "elongate");
    line("note(\"c!3 e\")", 0, 4, "replicate");
    line("note(\"[c a f e]/2\")", 0, 2, "slow half, cycle 0");
    line("note(\"[c a f e]/2\")", 1, 2, "slow half, cycle 1");
    line("note(\"c <e g>\")", 0, 2, "alternate inside a sequence");
    line("note(\"<[c2 c3]*4 [bb1 bb2]*4 [f2 f3]*4 [eb2 eb3]*4>\")", 0, 8,
         "classy bassline, one cycle of the cat");
    /* recap: n + scale, fast, setcpm */
    line("n(\"6 4 2 0\").scale(\"C4:minor\")", 0, 4, "scale degrees");
    line("sound(\"bd sd [~ bd] sd\").fast(2)", 0, 8, "fast doubles the cycle");
    line("setcpm(45)", 0, 180, "45 cycles/min -> bpm 180");
    line("setcpm(120/4)", 0, 120, "120/4 cycles/min -> bpm 120");
    line("setcpm((100+20)/4)", 0, 120, "arithmetic with parens");
    {
        StrudelLine s;
        int ok = strudel_parse("note(\"c3\").lpf(200*4).gain(1/2)", &s);
        ok = ok && s.handled && s.has_lpf && s.lpf > 799 && s.lpf < 801
            && s.has_gain && s.gain > 0.49 && s.gain < 0.51;
        printf("%-44s expr args %s\n", "lpf(200*4).gain(1/2)", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        strudel_line_free(&s);
    }
    /* first effects */
    line("s(\"hh*16\").gain(\"[.25 1]*4\")", 0, 16, "hats with a gain pattern");
    line("note(\"c3,eb3,g3\").sound(\"sawtooth\")", 0, 3, "stacked chord, saw");
    line("s(\"bd sd, hh*4\")", 0, 6, "comma stack");
    {
        StrudelLine s;
        int ok = strudel_parse("s(\"hh*8\").gain(slider(0.5,0,1,0.05)).lpf(800)", &s);
        ok = ok && s.handled && s.has_gain && s.gain > 0.4 && s.gain < 0.6 && s.has_lpf
            && s.lpf > 799 && s.gain_pat == NULL;
        printf("%-44s slider+lpf %s\n", "gain(slider) .lpf(800)", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        strudel_line_free(&s);
    }
    {
        StrudelLine s;
        strudel_parse("note(\"c e g\").sound(\"sawtooth\").room(0.5).delay(0.25)", &s);
        int ok = s.inst[0] && strcmp(s.inst, "sawtooth") == 0 && s.room > 0.4 && s.delay > 0.2;
        printf("%-44s saw/room/delay %s\n", "note.sound.room.delay", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        strudel_line_free(&s);
    }
    /* flatrave: scale inside the call, quote-first .note(), struct, signals */
    {
        StrudelLine s;
        const char *src = "note(\"<0 2 5 3>\".scale('G1 minor')).struct(\"x(5,8,-1)\")"
            ".s('sawtooth').decay(.1).sustain(0).lpa(.1).lpenv(-4).lpf(800).lpq(8)";
        int ok = strudel_parse(src, &s);
        ok = ok && strcmp(s.keyword, "notes") == 0 && strcmp(s.pattern, "<0 2 5 3>") == 0
            && strcmp(s.scale, "G1 minor") == 0 && strcmp(s.struct_pat, "x(5,8,-1)") == 0
            && strcmp(s.inst, "sawtooth") == 0 && s.dec > 0.09 && s.dec < 0.11
            && s.sus == 0 && s.lpa > 0.09 && s.lpa < 0.11 && s.has_lpenv && s.lpenv == -4
            && s.lpf > 799 && s.lpq > 7.9;
        printf("%-44s bass chain %s\n", "note().scale.struct.lpenv", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        strudel_line_free(&s);
    }
    {
        StrudelLine s;
        const char *src = "\"0 5 3 2\".sometimes(slow(2)).off(1/8,add(5))"
            ".scale('G4 minor').note().decay(.05).sustain(0).delay(.2)"
            ".degradeBy(.5).mask(\"<0 1>/16\")";
        int ok = strudel_parse(src, &s);
        ok = ok && strcmp(s.keyword, "notes") == 0 && strcmp(s.pattern, "0 5 3 2") == 0
            && s.sometimes_p > 0.49 && s.sometimes_slow > 1.9 && s.sometimes_slow < 2.1
            && s.has_off && s.off_degrees && fabs(s.off_time - 0.125) < 0.001
            && fabs(s.off_add - 5) < 0.001 && strcmp(s.scale, "G4 minor") == 0
            && fabs(s.degrade - 0.5) < 0.001 && strcmp(s.mask_pat, "<0 1>/16") == 0
            && fabs(s.delay - 0.2) < 0.001;
        printf("%-44s melody chain %s\n", "quote.off.scale.note", ok ? "OK" : "FAIL");
        if (!ok) {
            fails++;
            printf("    kw=%s pat=%s scale=%s off=%g deg=%d slow=%g mask=%s\n",
                s.keyword, s.pattern, s.scale, s.off_time, s.off_degrees,
                s.sometimes_slow, s.mask_pat);
        }
        strudel_line_free(&s);
    }
    {
        StrudelLine s;
        const char *src = "s(\"hh:1*4\").sometimes(fast(\"2\"))"
            ".rarely(x=>x.speed(\".5\").delay(.5))"
            ".end(perlin.range(0.02,.05).slow(8))"
            ".bank('RolandTR909').room(.5).gain(\"0.4,0.4(5,8,-1)\")";
        int ok = strudel_parse(src, &s);
        ok = ok && strcmp(s.keyword, "sound") == 0 && s.sometimes_fast == 2
            && s.rarely_p > 0.24 && s.rarely_p < 0.26
            && s.rarely_rate > 0.49 && s.rarely_rate < 0.51
            && s.rarely_send > 0.49 && s.end_sig.kind == 2 && s.end_sig.has_range
            && s.end_sig.lo > 0.019 && s.end_sig.hi < 0.051 && s.end_sig.slow > 7
            && strcmp(s.bank, "RolandTR909") == 0 && s.gain_pat != NULL
            && s.room > 0.49;
        printf("%-44s hat chain %s\n", "sometimes.rarely.end", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        strudel_line_free(&s);
    }
    {
        StrudelLine s;
        const char *src = "note(\"<G4 A4 Bb4 A4>,Bb3,D3\").struct(\"~ x*2\").s('square')"
            ".cutoff(sine.range(500,4000).slow(16)).resonance(10)"
            ".decay(sine.slow(15).range(.05,.2)).sustain(0)";
        int ok = strudel_parse(src, &s);
        ok = ok && strcmp(s.inst, "square") == 0 && strcmp(s.struct_pat, "~ x*2") == 0
            && s.lpf_sig.kind == 1 && s.lpf_sig.has_range && s.lpf_sig.lo == 500
            && s.lpf_sig.hi == 4000 && s.lpf_sig.slow == 16 && s.lpq == 10
            && s.dec_sig.kind == 1 && s.dec_sig.slow == 15 && s.dec_sig.lo > 0.04
            && s.dec_sig.hi < 0.21 && s.sus == 0;
        printf("%-44s chord filter %s\n", "cutoff sine.resonance", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        strudel_line_free(&s);
    }
    {
        /* struct retimes; off adds a degree copy an eighth later */
        StrudelLine s;
        strudel_parse("note(\"c d e f\").struct(\"x ~ x ~\")", &s);
        Node *pat = parse_pattern_text(s.pattern);
        PatEventList l;
        patlist_init(&l);
        expand_node(pat, &l, 0.0, 1.0, 0);
        XForm x;
        memset(&x, 0, sizeof(x));
        x.structure = parse_pattern_text(s.struct_pat);
        x.off_time = -1;
        x.sometimes_p = -1;
        x.rarely_p = -1;
        x.degrade = -1;
        xform_events(&x, &l, 0);
        int ok = l.n == 2 && strcmp(l.items[0].token, "c") == 0
            && strcmp(l.items[1].token, "e") == 0;
        printf("%-44s struct x ~ x ~ %s\n", "struct", ok ? "OK" : "FAIL");
        if (!ok) {
            fails++;
            printf("    n=%d\n", l.n);
            for (int i = 0; i < l.n && i < 8; i++)
                printf("    %s @ %.3f\n", l.items[i].token, l.items[i].start);
        }
        patlist_free(&l);
        node_free(pat);
        node_free(x.structure);
        strudel_line_free(&s);
    }
    {
        Node *pat = parse_pattern_text("0 2");
        PatEventList l;
        patlist_init(&l);
        expand_node(pat, &l, 0.0, 1.0, 0);
        XForm x;
        memset(&x, 0, sizeof(x));
        x.has_off = 1;
        x.off_time = 0.125;
        x.off_add = 5;
        x.off_degrees = 1;
        x.sometimes_p = -1;
        x.rarely_p = -1;
        x.degrade = -1;
        xform_events(&x, &l, 0);
        int ok = l.n == 4 && strcmp(l.items[2].token, "5") == 0
            && strcmp(l.items[3].token, "7") == 0
            && fabs(l.items[2].start - 0.125) < 0.02;
        printf("%-44s off 1/8 add 5 %s\n", "off", ok ? "OK" : "FAIL");
        if (!ok) {
            fails++;
            for (int i = 0; i < l.n && i < 8; i++)
                printf("    %s @ %.3f\n", l.items[i].token, l.items[i].start);
        }
        patlist_free(&l);
        node_free(pat);
    }
    {
        double z = xform_signal(1, 1, 1, 0, 0, 1, 0, 0);
        double peak = xform_signal(1, 1, 1, 1, 500, 4000, 0, 0.25);
        int ok = fabs(z - 0.5) < 0.001 && fabs(peak - 4000) < 1;
        printf("%-44s sine range %s\n", "signal", ok ? "OK" : "FAIL");
        if (!ok) fails++;
    }
    {
        StrudelLine q;
        const char *qs = "note(\"c d e f\").struct(\"x ~ x ~\").gain(\"0.2 0.8\").bank('tr909')";
        int qok = strudel_parse(qs, &q);
        int saw_struct = 0, saw_gain = 0, saw_bank = 0;
        for (int i = 0; i < q.hl_n; i++) {
            if (q.hl_at[i] < 0) continue;
            if (strncmp(qs + q.hl_at[i], "x ~ x ~", 7) == 0) saw_struct = 1;
            if (strncmp(qs + q.hl_at[i], "0.2 0.8", 7) == 0) saw_gain = 1;
            if (strncmp(qs + q.hl_at[i], "tr909", 5) == 0) saw_bank = 1;
        }
        qok = qok && saw_struct && saw_gain && !saw_bank && q.hl_n == 2;
        printf("%-44s quoted controls %s\n", "struct.gain.bank", qok ? "OK" : "FAIL");
        if (!qok) {
            fails++;
            printf("    hl_n=%d struct=%d gain=%d bank=%d\n", q.hl_n, saw_struct, saw_gain, saw_bank);
        }
        strudel_line_free(&q);
    }
    {
        /* flatrave pitches: G1 minor degree 0, the square voicing, G4 minor + off. */
        int steps[12], nst = 0, root = 0;
        int ok = harmony_scale("G1 minor", &root, steps, 12, &nst)
            && harmony_degree_midi(0, 0, root, steps, nst) == 31
            && harmony_degree_midi(2, 0, root, steps, nst) == 34
            && harmony_degree_midi(5, 0, root, steps, nst) == 39
            && harmony_degree_midi(3, 0, root, steps, nst) == 36;
        ok = ok && harmony_scale("G4 minor", &root, steps, 12, &nst)
            && harmony_degree_midi(0, 0, root, steps, nst) == 67
            && harmony_degree_midi(5, 0, root, steps, nst) == 75
            && harmony_degree_midi(3, 0, root, steps, nst) == 72
            && harmony_degree_midi(2, 0, root, steps, nst) == 70
            && harmony_degree_midi(10, 0, root, steps, nst) == 84
            && harmony_degree_midi(8, 0, root, steps, nst) == 81
            && harmony_degree_midi(7, 0, root, steps, nst) == 79;
        Node *chord = parse_pattern_text("<G4 A4 Bb4 A4>,Bb3,D3");
        PatEventList cl;
        patlist_init(&cl);
        expand_node(chord, &cl, 0.0, 1.0, 0);
        int sawg = 0, sawbb = 0, sawd = 0;
        for (int i = 0; i < cl.n; i++) {
            if (strcmp(cl.items[i].token, "G4") == 0) sawg = 1;
            if (strcmp(cl.items[i].token, "Bb3") == 0) sawbb = 1;
            if (strcmp(cl.items[i].token, "D3") == 0) sawd = 1;
        }
        ok = ok && cl.n == 3 && sawg && sawbb && sawd;
        Node *st = parse_pattern_text("~ x*2");
        XForm x;
        memset(&x, 0, sizeof(x));
        x.structure = st;
        x.sometimes_p = -1;
        x.rarely_p = -1;
        x.degrade = -1;
        xform_events(&x, &cl, 0);
        int hits = 0;
        for (int i = 0; i < cl.n; i++) {
            if (fabs(cl.items[i].start - 0.5) < 0.02) hits |= 1;
            if (fabs(cl.items[i].start - 0.75) < 0.02) hits |= 2;
        }
        ok = ok && cl.n == 6 && hits == 3;
        printf("%-44s flatrave notes %s\n", "G minor voicing", ok ? "OK" : "FAIL");
        if (!ok) {
            fails++;
            printf("    chord n=%d hits=%d\n", cl.n, hits);
            for (int i = 0; i < cl.n && i < 8; i++)
                printf("    %s @ %.3f\n", cl.items[i].token, cl.items[i].start);
        }
        patlist_free(&cl);
        node_free(chord);
        node_free(st);
    }
    {
        /* juxundtollerei: palindrome, jux stereo, off-arrow triangle copy. */
        StrudelLine s;
        const char *src =
            "note(\"c3 eb3 g3 bb3\").palindrome().s('sawtooth')"
            ".jux(x=>x.rev().color('green').s('sawtooth'))"
            ".off(1/4, x=>x.add(note(\"<7 12>/2\")).slow(2).late(.005).s('triangle'))"
            ".lpf(sine.range(200,2000).slow(8))"
            ".lpa(.2).lpenv(-2).decay(.05).sustain(0).room(.6)"
            ".delay(.5).delaytime(.1).delayfeedback(.4).pianoroll()";
        int ok = strudel_parse(src, &s);
        ok = ok && strcmp(s.keyword, "notes") == 0
            && strcmp(s.pattern, "c3 eb3 g3 bb3") == 0
            && strcmp(s.inst, "sawtooth") == 0
            && s.palindrome == 1 && s.rev == 0 && s.has_off == 0
            && s.jux == 1 && s.jux_rev == 1 && strcmp(s.jux_inst, "sawtooth") == 0
            && s.off_fn == 1 && fabs(s.off_time - 0.25) < 1e-9
            && fabs(s.off_slow - 2.0) < 1e-9 && fabs(s.off_late - 0.005) < 1e-9
            && strcmp(s.off_inst, "triangle") == 0 && s.off_add_pat != NULL
            && s.lpf_sig.kind == 1 && s.lpf_sig.lo == 200 && s.lpf_sig.hi == 2000
            && s.lpf_sig.slow == 8 && s.lpq < 0
            && s.lpa > 0.19 && s.lpa < 0.21 && s.has_lpenv && s.lpenv == -2
            && s.dec > 0.04 && s.dec < 0.06 && s.sus == 0
            && s.room > 0.59 && fabs(s.delay - 0.5) < 0.001
            && fabs(s.delay_time - 0.1) < 0.001
            && fabs(s.delay_fb - 0.4) < 0.001;
        ok = ok && xform_wants_rev(0, 1, 0) == 0
            && xform_wants_rev(0, 1, 1) == 1
            && xform_wants_rev(0, 1, 2) == 0
            && xform_wants_rev(1, 0, 1) == 1
            && xform_wants_rev(1, 1, 1) == 0;
        int add7 = 0, add12 = 0;
        for (int c = 0; c < 4; c++) {
            PatEventList al;
            patlist_init(&al);
            expand_node(s.off_add_pat, &al, 0.0, 1.0, c);
            int hit = 0;
            for (int i = 0; i < al.n; i++) {
                if (c < 2 && strcmp(al.items[i].token, "7") == 0) hit = 1;
                if (c >= 2 && strcmp(al.items[i].token, "12") == 0) hit = 1;
            }
            if (c < 2) add7 += hit;
            else add12 += hit;
            patlist_free(&al);
        }
        ok = ok && add7 == 2 && add12 == 2;
        Node *n = parse_pattern_text("c3 eb3 g3 bb3");
        XForm x;
        memset(&x, 0, sizeof(x));
        x.jux = 1;
        x.jux_rev = 1;
        x.jux_inst = INST_SAW;
        x.off_fn = 1;
        x.off_time = s.off_time;
        x.off_slow = s.off_slow;
        x.off_late = s.off_late;
        x.off_inst = INST_TRI;
        x.off_add_pat = s.off_add_pat;
        x.pat = n;
        x.palindrome = 1;
        x.sometimes_p = -1;
        x.rarely_p = -1;
        x.degrade = -1;
        PatEventList l;
        patlist_init(&l);
        expand_node(n, &l, 0.0, 1.0, 0);
        xform_jux(&l, 1, INST_SAW);
        xform_events(&x, &l, 0);
        xform_off_arrow(&x, &l, 0);
        int left = 0, right = 0, tri = 0, saw_r = 0;
        int left_ok = 1, right_ok = 1;
        const char *lwant[] = {"c3", "eb3", "g3", "bb3"};
        const char *rwant[] = {"bb3", "g3", "eb3", "c3"};
        PatEvent ls[4], rs[4];
        int nl = 0, nr = 0;
        for (int i = 0; i < l.n; i++) {
            if (l.items[i].inst == INST_TRI) {
                tri++;
                continue;
            }
            if (l.items[i].pan < 0.1) {
                left++;
                if (nl < 4) ls[nl++] = l.items[i];
            } else if (l.items[i].pan > 0.9) {
                right++;
                if (l.items[i].inst == INST_SAW) saw_r++;
                if (nr < 4) rs[nr++] = l.items[i];
            }
        }
        for (int i = 0; i < nl; i++) {
            for (int j = i + 1; j < nl; j++) {
                if (ls[j].start < ls[i].start) {
                    PatEvent tmp = ls[i];
                    ls[i] = ls[j];
                    ls[j] = tmp;
                }
            }
        }
        for (int i = 0; i < nr; i++) {
            for (int j = i + 1; j < nr; j++) {
                if (rs[j].start < rs[i].start) {
                    PatEvent tmp = rs[i];
                    rs[i] = rs[j];
                    rs[j] = tmp;
                }
            }
        }
        if (nl != 4) left_ok = 0;
        if (nr != 4) right_ok = 0;
        for (int i = 0; i < nl && i < 4; i++)
            if (strcmp(ls[i].token, lwant[i]) != 0) left_ok = 0;
        for (int i = 0; i < nr && i < 4; i++)
            if (strcmp(rs[i].token, rwant[i]) != 0) right_ok = 0;
        int tri_c3 = 0, tri_bb = 0;
        for (int i = 0; i < l.n; i++) {
            if (l.items[i].inst != INST_TRI) continue;
            if (l.items[i].extra != 7) continue;
            if (fabs(l.items[i].start - 0.505) > 0.01) continue;
            if (fabs(l.items[i].dur - 0.495) > 0.02) continue;
            if (strcmp(l.items[i].token, "c3") == 0 && l.items[i].pan < 0.1) tri_c3 = 1;
            if (strcmp(l.items[i].token, "bb3") == 0 && l.items[i].pan > 0.9) tri_bb = 1;
        }
        ok = ok && l.n == 10 && left == 4 && right == 4 && saw_r == 4
            && tri == 2 && left_ok && right_ok && tri_c3 && tri_bb;
        PatEventList l4;
        patlist_init(&l4);
        expand_node(n, &l4, 0.0, 1.0, 4);
        xform_jux(&l4, 1, INST_SAW);
        xform_off_arrow(&x, &l4, 4);
        int got12 = 0;
        for (int i = 0; i < l4.n; i++) {
            if (l4.items[i].inst == INST_TRI && l4.items[i].extra == 12
                && fabs(l4.items[i].start - 0.505) < 0.01)
                got12 = 1;
        }
        ok = ok && got12;
        printf("%-44s jux+off arrow %s\n", "juxundtollerei chain", ok ? "OK" : "FAIL");
        if (!ok) {
            fails++;
            printf("    jux=%d rev=%d ofn=%d slow=%g late=%g n=%d tri=%d add7=%d add12=%d got12=%d\n",
                s.jux, s.jux_rev, s.off_fn, s.off_slow, s.off_late, l.n, tri, add7, add12, got12);
            for (int i = 0; i < l.n && i < 12; i++)
                printf("    %s @ %.3f d=%.3f pan=%.2f inst=%d extra=%d\n",
                    l.items[i].token, l.items[i].start, l.items[i].dur,
                    l.items[i].pan, l.items[i].inst, l.items[i].extra);
        }
        patlist_free(&l);
        patlist_free(&l4);
        node_free(n);
        strudel_line_free(&s);
    }
    /* Dirt-samples stack: sample indexes, and the sine+saw bass. */
    {
        StrudelLine s;
        const char *amen =
            "n(\"0 1 2 3 4 5 6 7\")"
            ".sometimes(x=>x.ply(2))"
            ".rarely(x=>x.speed(\"2 | -2\"))"
            ".sometimesBy(.4, x=>x.delay(\".5\"))"
            ".s(\"amencutup\").slow(2).room(.5)";
        const char *bass =
            "sine.add(saw.slow(4)).range(0,7).segment(8)"
            ".superimpose(x=>x.add(.1))"
            ".scale('G0 minor').note()"
            ".s(\"sawtooth\").gain(.4).decay(.1).sustain(0)"
            ".lpa(.1).lpenv(-4).lpq(10)"
            ".cutoff(perlin.range(300,3000).slow(8))"
            ".degradeBy(\"0 0.1 .5 .1\")"
            ".rarely(add(note(\"12\")))";
        const char *chord =
            "note(\"Bb3,D4\".superimpose(x=>x.add(.2)))"
            ".s('sawtooth').lpf(1000).struct(\"<~@3 [~ x]>\")"
            ".decay(.05).sustain(.0).delay(.8).delaytime(.125).room(.8)";
        const char *alien =
            "s(\"breath\").room(1).shape(.6).chop(16).rev().mask(\"<x ~@7>\")";
        const char *east =
            "n(\"0 1\").s(\"east\").delay(.5).degradeBy(.8).speed(rand.range(.5,1.5))";
        int ok = strudel_parse(amen, &s);
        int n = ok ? events(s.pattern, 0) : -1;
        ok = ok && strcmp(s.keyword, "n") == 0 && strcmp(s.bank, "amencutup") == 0
            && s.pattern[0] == '[' && n == 4 && s.room == 0.5
            && s.smp_speed == 0 && s.rarely_rate < 0 && s.rarely_speed;
        if (ok) {
            PatEventList sp;
            patlist_init(&sp);
            expand_node(s.rarely_speed, &sp, 0.0, 1.0, 0);
            ok = sp.n == 1 && (!strcmp(sp.items[0].token, "2")
                || !strcmp(sp.items[0].token, "-2"));
            patlist_free(&sp);
        }
        printf("%-44s amen index %s\n", "n().s(amencutup)", ok ? "OK" : "FAIL");
        if (!ok) {
            fails++;
            printf("    kw=%s bank=%s n=%d pat=%s\n", s.keyword, s.bank, n, s.pattern);
        }
        strudel_line_free(&s);

        ok = strudel_parse(bass, &s) && s.sig && s.sig->segment == 8
            && strcmp(s.keyword, "notes") == 0
            && strcmp(s.scale, "G0 minor") == 0
            && strcmp(s.inst, "sawtooth") == 0
            && s.has_gain && fabs(s.gain - 0.4) < 1e-9
            && s.has_env && fabs(s.dec - 0.1) < 1e-9 && s.sus == 0
            && s.has_lpenv && fabs(s.lpenv - -4) < 1e-9
            && fabs(s.lpa - 0.1) < 1e-9 && fabs(s.lpq - 10) < 1e-9
            && s.lpf_sig.kind == 2 && s.lpf_sig.has_range
            && fabs(s.lpf_sig.lo - 300) < 1e-6 && fabs(s.lpf_sig.hi - 3000) < 1e-6
            && fabs(s.lpf_sig.slow - 8) < 1e-9;
        {
            double v0 = s.sig ? sig_eval(s.sig, 0.0) : -1;
            double mid = s.sig ? sig_eval(s.sig, 0.0625) : -1;
            PatEventList ev;
            int root = 0, steps[12], ns = 0, midi = -1, want = 0, i;
            patlist_init(&ev);
            if (s.sig) sig_events(s.sig, &ev, 0);
            for (i = 0; i < 8; i++) {
                double v = s.sig ? sig_eval(s.sig, i / 8.0) : 0;
                want++;
                if (lround(v + 0.1) != lround(v)) want++;
            }
            if (harmony_scale("G0 minor", &root, steps, 12, &ns))
                midi = harmony_degree_midi(4, 0, root, steps, ns);
            ok = ok && fabs(v0 - 3.5) < 1e-9 && lround(v0) == 4
                && lround(mid) == 5
                && ev.n == want && want >= 8
                && ev.items && strcmp(ev.items[0].token, "4") == 0
                && fabs(ev.items[0].start) < 1e-9
                && fabs(ev.items[0].dur - 0.125) < 1e-9
                && root == 19 && midi == 26;
            printf("%-44s bass signal %s\n", "sine+saw segment", ok ? "OK" : "FAIL");
            if (!ok) {
                fails++;
                printf("    v0=%g mid=%g n=%d tok=%s root=%d midi=%d seg=%d\n",
                    v0, mid, ev.n, (ev.items && ev.n > 0) ? ev.items[0].token : "-",
                    root, midi, s.sig ? s.sig->segment : -1);
            }
            patlist_free(&ev);
        }
        strudel_line_free(&s);

        ok = strudel_parse("saw.segment(4).superimpose(x=>x.add(0.6)).note()", &s)
            && s.sig && s.sig->has_extra;
        {
            PatEventList ev;
            patlist_init(&ev);
            if (s.sig) sig_events(s.sig, &ev, 0);
            /* t=0 is 0 and 1; t=0.5 is 1 only. Four slices, two extras. */
            ok = ok && ev.n == 6 && ev.items
                && strcmp(ev.items[0].token, "0") == 0
                && strcmp(ev.items[1].token, "1") == 0;
            printf("%-44s superimpose %s\n", "saw segment extra", ok ? "OK" : "FAIL");
            if (!ok) {
                fails++;
                printf("    n=%d\n", ev.n);
            }
            patlist_free(&ev);
        }
        strudel_line_free(&s);

        ok = strudel_parse(chord, &s) && strcmp(s.pattern, "Bb3,D4") == 0
            && strcmp(s.inst, "sawtooth") == 0 && s.has_lpf && s.lpf == 1000
            && strcmp(s.struct_pat, "<~@3 [~ x]>") == 0
            && fabs(s.delay_time - 0.125) < 1e-9;
        printf("%-44s chord %s\n", "Bb3,D4 struct", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        strudel_line_free(&s);

        ok = strudel_parse(alien, &s) && strcmp(s.keyword, "sound") == 0
            && strcmp(s.pattern, "breath") == 0 && s.rev == 1
            && strcmp(s.mask_pat, "<x ~@7>") == 0 && s.room == 1;
        printf("%-44s breath %s\n", "s(breath) mask", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        strudel_line_free(&s);

        ok = strudel_parse(east, &s) && strcmp(s.keyword, "n") == 0
            && strcmp(s.bank, "east") == 0 && fabs(s.degrade - 0.8) < 1e-9
            && fabs(s.delay - 0.5) < 1e-9 && s.smp_speed == 0;
        printf("%-44s east %s\n", "n().s(east)", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        strudel_line_free(&s);

        ok = !strudel_parse("useRNG('legacy')", &s)
            && !strudel_parse(").reset(\"<x@7 x(5,8,-1)>\")", &s)
            && !strudel_parse("stack(", &s);
        printf("%-44s ignored %s\n", "useRNG reset stack", ok ? "OK" : "FAIL");
        if (!ok) fails++;
        strudel_line_free(&s);
    }
    printf(fails ? "FAILURES: %d\n" : "ALL PASS\n", fails);
    return fails != 0;
}
