#include "track.h"
#include "websamp.h"
#include "pattern.h"
#include "audio.h"
#include "scheduler.h"
#include "wavetable.h"
#include "sampler.h"
#include "banks.h"
#include "harmony.h"
#include "strudel.h"
#include "slider.h"
#include <ctype.h>
#include <limits.h>

Track tracks[MAX_TRACKS];
int track_count = 0;
pthread_mutex_t tracks_lock = PTHREAD_MUTEX_INITIALIZER;
static char current_code_empty[] = "";
char *current_code = current_code_empty;

int current_code_set(const char *s) {
    if (!s) s = "";
    if (s == current_code) return 1;
    size_t n = strlen(s);
    if (n > (size_t)INT_MAX) return 0;
    char *p = malloc(n + 1);
    if (!p) return 0;
    memcpy(p, s, n + 1);
    if (current_code != current_code_empty) free(current_code);
    current_code = p;
    return 1;
}

static int inst_from_name(const char *name) {
    if (!name) return INST_SINE;
    if (strcmp(name,"sine")==0) return INST_SINE;
    if (strcmp(name,"saw")==0 || strcmp(name,"sawtooth")==0) return INST_SAW;
    if (strcmp(name,"sqr")==0 || strcmp(name,"square")==0) return INST_SQUARE;
    if (strcmp(name,"tri")==0 || strcmp(name,"triangle")==0) return INST_TRI;
    if (strcmp(name,"noise")==0 || strcmp(name,"white")==0 ||
        strcmp(name,"pink")==0 || strcmp(name,"brown")==0) return INST_NOISE;
    return INST_SINE;
}

static const char *default_inst_for_keyword(const char *kw) {
    if (strcmp(kw,"bass")==0) return "saw";
    if (strcmp(kw,"lead")==0) return "square";
    if (strcmp(kw,"melody")==0||strcmp(kw,"notes")==0||strcmp(kw,"note")==0) return "tri";
    if (strcmp(kw,"n")==0||strcmp(kw,"chord")==0) return "tri";
    if (strcmp(kw,"noise")==0) return "noise";
    if (strcmp(kw,"s")==0) return "sine"; /* placeholder; samples bypass synth */
    return "sine";
}

static void free_tracks(void) {
    for (int i=0;i<track_count;i++) {
        javascript_free(tracks[i].javascript);
        node_free(tracks[i].pattern);
        node_free(tracks[i].arp);
        node_free(tracks[i].gain_pat);
        node_free(tracks[i].pan_pat);
        node_free(tracks[i].lpf_pat);
        node_free(tracks[i].struct_pat);
        node_free(tracks[i].mask_pat);
        node_free(tracks[i].off_add_pat);
        node_free(tracks[i].speed_pat);
        node_free(tracks[i].rarely_speed);
        sig_free(tracks[i].sig);
        for (int h = 0; h < tracks[i].hl_n && h < HL_PAT_MAX; h++)
            node_free(tracks[i].hl_pat[h]);
    }
    track_count = 0;
}

static int split_transforms(char *code, char **transform_part) {
    /* returns 0 if no pipe */
    char *p = strchr(code, '|');
    if (!p) return 0;
    *p = 0;
    *transform_part = p+1;
    return 1;
}

static int is_keyword(const char *w) {
    const char *ks[] = {"drum","drums","sound","note","notes","n","bass","lead","melody","noise","chord","s"};
    for (size_t i=0;i<sizeof(ks)/sizeof(ks[0]);i++)
        if (strcmp(w,ks[i])==0) return 1;
    return 0;
}

/* Transform names. A numeric depth must not swallow the next command. */
static int is_pipe_word(const char *w) {
    static const char *ws[] = {
        "rev","fast","slow","gain","inst","s","sound","bank","mute","solo","midi",
        "att","attack","rel","release","dec","decay","sus","sustain","adsr",
        "scale","transpose","add","arp","lfo","vib","pw","lpf","cutoff",
        "shuffle","degrade", NULL
    };
    if (!w || !w[0]) return 0;
    for (int i = 0; ws[i]; i++)
        if (strcmp(w, ws[i]) == 0) return 1;
    return 0;
}

static int is_num_tok(const char *p) {
    if (!p || !*p) return 0;
    if (*p == '+' || *p == '-') p++;
    int dig = 0, dot = 0;
    for (; *p; p++) {
        if (*p == '.') { if (dot) return 0; dot = 1; }
        else if (*p >= '0' && *p <= '9') dig = 1;
        else return 0;
    }
    return dig;
}

static void peek_tok(const char *sp, char *out, size_t n) {
    const char *q = sp ? sp : "";
    while (*q == ' ' || *q == '\t') q++;
    size_t k = 0;
    while (q[k] && q[k] != ' ' && q[k] != '\t' && k + 1 < n) {
        out[k] = q[k];
        k++;
    }
    out[k] = 0;
}

static int lfo_shape(const char *p) {
    if (!p) return -1;
    if (strcmp(p, "sine") == 0) return 1;
    if (strcmp(p, "saw") == 0 || strcmp(p, "sawtooth") == 0 || strcmp(p, "ramp") == 0) return 2;
    if (strcmp(p, "square") == 0 || strcmp(p, "sqr") == 0) return 3;
    if (strcmp(p, "triangle") == 0 || strcmp(p, "tri") == 0) return 0;
    return -1;
}

static int lfo_target(const char *p) {
    if (!p) return 0;
    if (strcmp(p, "freq") == 0 || strcmp(p, "pitch") == 0) return 1;
    if (strcmp(p, "gain") == 0) return 2;
    if (strcmp(p, "lpf") == 0 || strcmp(p, "cutoff") == 0) return 3;
    if (strcmp(p, "pw") == 0) return 4;
    return 0;
}

static double default_lfo_depth(int tgt) {
    if (tgt == 1) return 1.0;    /* semitones */
    if (tgt == 2) return 0.5;
    if (tgt == 3) return 400.0;  /* Hz */
    if (tgt == 4) return 0.3;
    return 0.0;
}

static void apply_env(Track *t, double a, double d, double s, double r) {
    if (isfinite(a) && a >= 0.0 && a <= 4.0) t->att = a < 0.001 ? 0.001 : a;
    if (isfinite(d) && d >= 0.0 && d <= 8.0) t->dec = d;
    if (isfinite(s) && s >= 0.0 && s <= 1.0) t->sus = s;
    if (isfinite(r) && r >= 0.005 && r <= 8.0) t->rel = r;
}

/* "A:D:S:R" is one mini-notation word (colon is not a delimiter). */
static int adsr_colon(const char *p, double *a, double *d, double *s, double *r) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", p ? p : "");
    char *sv = NULL;
    char *pa = strtok_r(buf, ":", &sv);
    char *pd = strtok_r(NULL, ":", &sv);
    char *ps = strtok_r(NULL, ":", &sv);
    char *pr = strtok_r(NULL, ":", &sv);
    if (!pa || !pd || !ps || !pr || strtok_r(NULL, ":", &sv)) return 0;
    if (!is_num_tok(pa) || !is_num_tok(pd) || !is_num_tok(ps) || !is_num_tok(pr)) return 0;
    *a = atof(pa); *d = atof(pd); *s = atof(ps); *r = atof(pr);
    return 1;
}

static void set_inst_token(Track *t, const char *p) {
    if (!p) return;
    if (strncmp(p, "wt:", 3) == 0) {
        int w = wt_find(p + 3);
        if (w < 0) w = wt_find("saw");
        t->inst = INST_WT;
        t->wt = w;
    } else if (strcmp(p, "wt") == 0) {
        t->inst = INST_WT;
        t->wt = wt_find("saw");
    } else {
        t->inst = inst_from_name(p);
        t->wt = -1;
    }
}

static int osc_name(const char *s) {
    return s && (strcmp(s, "sine") == 0 || strcmp(s, "saw") == 0 || strcmp(s, "sawtooth") == 0
        || strcmp(s, "square") == 0 || strcmp(s, "sqr") == 0 || strcmp(s, "tri") == 0
        || strcmp(s, "triangle") == 0 || strcmp(s, "noise") == 0 || strcmp(s, "white") == 0
        || strncmp(s, "wt:", 3) == 0);
}

static void stamp_pat_origin(Track *t, const StrudelLine *sl, int line_off) {
    t->pat_off = -1;
    t->pat_bias = sl->pattern_bias;
    t->pat_len = sl->pattern_len;
    if (sl->pattern_at < 0 || line_off < 0) return;
    long off = (long)line_off + sl->pattern_at;
    size_t clen = current_code ? strlen(current_code) : 0;
    if (off < 0 || (size_t)off > clen) return;
    t->pat_off = (int)off;
}

static void apply_strudel(const StrudelLine *sl, int line_off) {
    if (sl->bpm_set) {
        audio_set_bpm(sl->bpm);
        return;
    }
    if ((!sl->pattern[0] && !sl->sig) || track_count >= MAX_TRACKS) {
        node_free(sl->gain_pat);
        node_free(sl->pan_pat);
        node_free(sl->lpf_pat);
        node_free(sl->off_add_pat);
        node_free(sl->speed_pat);
        node_free(sl->rarely_speed);
        sig_free(sl->sig);
        for (int h = 0; h < sl->hl_n && h < HL_PAT_MAX; h++) node_free(sl->hl_pat[h]);
        return;
    }
    Node *pat = NULL;
    if (sl->pattern[0]) {
        pat = parse_pattern_text(sl->pattern);
        const char *perr = pattern_last_error();
        if (perr && *perr) log_console("pattern: %s", perr);
    }
    if (pat && sl->degrade >= 0) pat->degrade = sl->degrade;
    Track *t = &tracks[track_count++];
    memset(t, 0, sizeof(Track));
    t->gain_slider=sl->gain_slider; t->lpf_slider=sl->lpf_slider;
    t->slider_revision=slider_revision();
    snprintf(t->keyword, sizeof(t->keyword), "%s", sl->keyword[0] ? sl->keyword : "notes");
    t->pattern = pat;
    stamp_pat_origin(t, sl, line_off);
    t->hl_n = 0;
    for (int h = 0; h < sl->hl_n && h < HL_PAT_MAX; h++) {
        long abs = (long)line_off + sl->hl_at[h];
        size_t clen = current_code ? strlen(current_code) : 0;
        if (sl->hl_at[h] < 0 || abs < 0 || (size_t)abs > clen) {
            node_free(sl->hl_pat[h]);
            continue;
        }
        int k = t->hl_n++;
        t->hl_pat[k] = sl->hl_pat[h];
        t->hl_at[k] = (int)abs;
        t->hl_len[k] = sl->hl_len[h];
    }
    t->fast_repeat = sl->fast_repeat > 0 ? sl->fast_repeat : 1;
    t->slow_stride = 1;
    t->rev = sl->rev;
    t->palindrome = sl->palindrome;
    t->gain = sl->has_gain ? sl->gain : 0.8;
    t->pan = 0;
    if (sl->has_pan && !sl->pan_pat) {
        double p = sl->pan;
        if (p >= 0 && p <= 1) p = p * 2 - 1;
        if (p < -1) p = -1;
        if (p > 1) p = 1;
        t->pan = p;
    }
    if (sl->inst[0] && osc_name(sl->inst)) set_inst_token(t, sl->inst);
    else t->inst = inst_from_name(default_inst_for_keyword(t->keyword));
    t->wt = (t->inst == INST_WT) ? t->wt : -1;
    t->bankfrag[0] = 0;
    t->bank[0] = 0;
    if (sl->bank[0]) {
        snprintf(t->bankfrag, sizeof(t->bankfrag), "%.63s", sl->bank);
        char dir[1024];
        if (bank_find_dir(sl->bank, dir, sizeof(dir))) {
            snprintf(t->bank, sizeof(t->bank), "%.255s", dir);
            log_console("bank '%s' -> %s", sl->bank, dir);
        } else if (!banks_ready()) {
            log_console("bank '%s' pending index...", sl->bank);
        } else {
            log_console("bank '%s' not found (synth fallback)", sl->bank);
        }
    }
    t->mute = 0;
    t->solo = 0;
    t->midi_chan = -1;
    t->att = sl->has_env ? sl->att : -1;
    t->dec = sl->has_env ? sl->dec : -1;
    t->sus = sl->has_env ? sl->sus : -1;
    t->rel = sl->has_env ? sl->rel : -1;
    t->pw = -1;
    t->lpf_hz = sl->has_lpf && !sl->lpf_pat ? sl->lpf : 0;
    t->degrade = -1;
    t->gain_pat = sl->gain_pat;
    t->pan_pat = sl->pan_pat;
    t->lpf_pat = sl->lpf_pat;
    if (sl->struct_pat[0]) t->struct_pat = parse_pattern_text(sl->struct_pat);
    if (sl->mask_pat[0]) t->mask_pat = parse_pattern_text(sl->mask_pat);
    t->off_time = sl->off_time;
    t->has_off = sl->has_off;
    t->off_add = sl->off_add;
    t->off_degrees = sl->off_degrees;
    t->sometimes_p = sl->sometimes_p;
    t->sometimes_fast = sl->sometimes_fast;
    t->sometimes_slow = sl->sometimes_slow;
    t->rarely_p = sl->rarely_p;
    t->rarely_rate = sl->rarely_rate;
    t->rarely_send = sl->rarely_send;
    /* Drop after struct/mask. A plain .degrade still lives on the pattern node. */
    int shaped = t->struct_pat || t->mask_pat || t->has_off
        || t->sometimes_p >= 0 || t->rarely_p >= 0;
    if (shaped) {
        t->post_degrade = sl->degrade;
        if (pat) pat->degrade = -1;
    } else {
        t->post_degrade = -1;
    }
    t->lpq = sl->lpq;
    t->lpenv = sl->lpenv;
    t->has_lpenv = sl->has_lpenv;
    t->lpa = sl->lpa;
    t->smp_end = sl->smp_end;
    t->smp_speed = sl->smp_speed;
    t->delay_amt = sl->delay;
    t->delay_time = sl->delay_time;
    t->delay_fb = sl->delay_fb;
    t->jux = sl->jux;
    t->jux_rev = sl->jux_rev;
    t->jux_inst = -1;
    if (sl->jux && sl->jux_inst[0]) t->jux_inst = inst_from_name(sl->jux_inst);
    t->off_fn = sl->off_fn;
    t->off_slow = sl->off_slow > 0 ? sl->off_slow : 1;
    t->off_late = sl->off_late;
    t->off_inst = -1;
    if (sl->off_fn && sl->off_inst[0]) t->off_inst = inst_from_name(sl->off_inst);
    t->off_add_pat = sl->off_add_pat;
    t->sig = sl->sig;
    t->speed_pat = sl->speed_pat;
    t->rarely_speed = sl->rarely_speed;
    t->lpf_sig = sl->lpf_sig;
    t->dec_sig = sl->dec_sig;
    t->end_sig = sl->end_sig;
    /* memset left active at 0, so the scheduler skipped every statement line */
    t->active = 1;
    t->next_cycle = audio_time_now();
    t->cycle_index = 0;
    if (sl->scale[0]) {
        int steps[12], n = 0, root = 60;
        if (harmony_scale(sl->scale, &root, steps, 12, &n) && n > 0) {
            t->scale_root = root;
            t->scale_n = n;
            for (int i = 0; i < n && i < 12; i++) t->scale_steps[i] = steps[i];
        }
    }
    if (sl->room >= 0) audio_fx_set("verb", sl->room);
    /* .delay() is a per-event send. The `fx delay` line still sets the master. */
}

static void parse_one_line(char *line, int line_off, double now) {
    int lead = 0;
    while (line[lead] == ' ' || line[lead] == '\t' || line[lead] == '\r') lead++;
    trim(line);
    line_off += lead;
    if (line[0]==0 || line[0]=='#') return;
    {
        StrudelLine sl;
        if (strudel_parse(line, &sl)) {
            Node *gp = sl.gain_pat, *pp = sl.pan_pat, *lp = sl.lpf_pat;
            Node *ap = sl.off_add_pat;
            Node *sp = sl.speed_pat, *rp = sl.rarely_speed;
            SigExpr *sg = sl.sig;
            sl.gain_pat = sl.pan_pat = sl.lpf_pat = sl.off_add_pat = NULL;
            sl.speed_pat = sl.rarely_speed = NULL;
            sl.sig = NULL;
            StrudelLine applied = sl;
            applied.gain_pat = gp;
            applied.pan_pat = pp;
            applied.lpf_pat = lp;
            applied.off_add_pat = ap;
            applied.speed_pat = sp;
            applied.rarely_speed = rp;
            applied.sig = sg;
            apply_strudel(&applied, line_off);
            /* patterns moved onto the track; don't free them */
            return;
        }
    }
    // bpm
    if (strncmp(line,"bpm ",4)==0 || strncmp(line,"bpm\t",4)==0) {
        audio_set_bpm(atof(line+4));
        return;
    }
    // fx chain: fx <drive|cutoff|delay|verb> <amount>
    if (strncmp(line,"fx ",3)==0 || strncmp(line,"fx\t",3)==0) {
        char fb[MAX_LINE];
        snprintf(fb, sizeof(fb), "%s", line + 3);
        char *sv = NULL;
        char *nm = strtok_r(fb, " \t", &sv);
        char *vl = strtok_r(NULL, " \t", &sv);
        if (nm && vl) {
            double v = atof(vl);
            if (isfinite(v)) audio_fx_set(nm, v);
        }
        return;
    }
    // track?
    char tmp[MAX_LINE];
    snprintf(tmp, sizeof(tmp), "%s", line);
    char *saveptr=NULL;
    char *tok = strtok_r(tmp, " \t", &saveptr);
    if (!tok) return;
    if (!is_keyword(tok)) {
        // maybe command stop
        if (strcmp(tok,"stop")==0) { free_tracks(); return; }
        return;
    }
    if (track_count >= MAX_TRACKS) return;
    // keyword track
    char kw[16];
    snprintf(kw, sizeof(kw), "%s", tok);
    // rest of line is in original line; extract after keyword
    // find second field
    char *rest = line;
    while (*rest && !isspace((unsigned char)*rest)) rest++;
    while (*rest && isspace((unsigned char)*rest)) rest++;
    int pat_at = line_off + (int)(rest - line);
    if (!*rest) return;
    char fullrest[MAX_LINE];
    snprintf(fullrest, sizeof(fullrest), "%s", rest);
    // split transforms
    char *transform = NULL;
    char code_part[MAX_LINE];
    snprintf(code_part, sizeof(code_part), "%s", fullrest);
    split_transforms(code_part, &transform);
    trim(code_part);
    if (code_part[0]==0) return;

    Node *pat = parse_pattern_text(code_part);
    {
        const char *perr = pattern_last_error();
        if (perr && *perr) log_console("pattern: %s", perr);
    }

    Track *t = &tracks[track_count++];
    memset(t,0,sizeof(Track));
    t->gain_slider=t->lpf_slider=-1;
    t->slider_revision=slider_revision();
    snprintf(t->keyword, sizeof(t->keyword), "%s", kw);
    t->pattern = pat;
    t->pat_off = pat_at;
    t->pat_len = (int)strlen(code_part);
    t->pat_bias = 0;
    t->fast_repeat = 1;
    t->slow_stride = 1;
    t->rev = 0;
    t->gain = 0.8;
    t->inst = inst_from_name(default_inst_for_keyword(kw));
    t->wt = -1;
    t->bankfrag[0] = 0;
    t->bank[0] = 0;
    t->mute = 0;
    t->solo = 0;
    t->midi_chan = -1;
    t->att = -1.0;
    t->rel = -1.0;
    t->dec = -1.0;
    t->sus = -1.0;
    t->pw = -1.0;
    t->lpf_hz = 0;
    t->semi = 0;
    t->chord = (strcmp(kw, "chord") == 0);
    t->lfo_tgt = 0;
    t->lfo_wave = 0;
    t->lfo_hz = 0;
    t->lfo_depth = 0;
    t->scale_root = 48;
    t->scale_n = 0;
    t->arp = NULL;
    t->shuffle = 0;
    t->degrade = -1.0;
    t->delay_time = -1;
    t->delay_fb = -1;
    t->next_cycle = now;
    t->cycle_index = 0;
    t->active = 1;

    if (transform) {
        // parse transform tokens
        char tbuf[MAX_LINE];
        snprintf(tbuf, sizeof(tbuf), "%s", transform);
        trim(tbuf);
        char *sp = NULL;
        char *p = strtok_r(tbuf, " \t", &sp);
        while (p) {
            if (strcmp(p,"rev")==0) t->rev = 1;
            else if (strcmp(p,"fast")==0) {
                p = strtok_r(NULL, " \t", &sp); if (p) t->fast_repeat = atoi(p);
                if (t->fast_repeat < 1) t->fast_repeat = 1;
                if (t->fast_repeat > 64) t->fast_repeat = 64;
            } else if (strcmp(p,"slow")==0) {
                p = strtok_r(NULL, " \t", &sp); if (p) t->slow_stride = atoi(p);
                if (t->slow_stride < 1) t->slow_stride = 1;
                if (t->slow_stride > 64) t->slow_stride = 64;
            } else if (strcmp(p,"gain")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) {
                    double g = 0.8;
                    t->gain_slider=!strncmp(p,"slider(",7)?slider_next_index():-1;
                    if (!slider_or_number(p, &g) || !isfinite(g)) g = 0.8;
                    if (g < 0) g = 0;
                    if (g > 2) g = 2;
                    t->gain = g;
                }
            } else if (strcmp(p,"inst")==0 || strcmp(p,"s")==0 || strcmp(p,"sound")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) set_inst_token(t, p);
            } else if (strcmp(p,"bank")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) {
                    snprintf(t->bankfrag, sizeof(t->bankfrag), "%.63s", p);
                    char dir[1024];
                    if (bank_find_dir(p, dir, sizeof(dir))) {
                        snprintf(t->bank, sizeof(t->bank), "%.255s", dir);
                        log_console("bank '%s' -> %s", p, dir);
                    } else if (!banks_ready()) {
                        t->bank[0] = 0;
                        log_console("bank '%s' pending index...", p);
                    } else {
                        t->bank[0] = 0;
                        log_console("bank '%s' not found (synth fallback)", p);
                    }
                }
            } else if (strcmp(p,"mute")==0) {
                t->mute = 1;
            } else if (strcmp(p,"solo")==0) {
                t->solo = 1;
            } else if (strcmp(p,"shuffle")==0) {
                t->shuffle = 1;
            } else if (strcmp(p,"degrade")==0) {
                double d = 0.5;
                /* optional amount: peek at remainder without consuming */
                const char *q = sp ? sp : "";
                while (*q == ' ' || *q == '\t') q++;
                if ((*q >= '0' && *q <= '9') || *q == '.' ||
                    (*q == '-' && q[1] >= '0' && q[1] <= '9')) {
                    p = strtok_r(NULL, " \t", &sp);
                    if (p) {
                        double v = atof(p);
                        if (isfinite(v) && v >= 0.0 && v <= 1.0) d = v;
                    }
                    t->degrade = d;
                    continue; /* amount already consumed */
                }
                t->degrade = d;
            } else if (strcmp(p,"midi")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) {
                    char *end = NULL;
                    long ch = strtol(p, &end, 10);
                    if (end != p && *end == 0 && ch >= 0 && ch <= 15)
                        t->midi_chan = (int)ch;
                }
            } else if (strcmp(p,"att")==0 || strcmp(p,"attack")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) {
                    double v = atof(p);
                    if (isfinite(v) && v >= 0.001 && v <= 4.0) t->att = v;
                }
            } else if (strcmp(p,"dec")==0 || strcmp(p,"decay")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) {
                    double v = atof(p);
                    if (isfinite(v) && v >= 0.0 && v <= 8.0) t->dec = v;
                }
            } else if (strcmp(p,"sus")==0 || strcmp(p,"sustain")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) {
                    double v = atof(p);
                    if (isfinite(v) && v >= 0.0 && v <= 1.0) t->sus = v;
                }
            } else if (strcmp(p,"rel")==0 || strcmp(p,"release")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) {
                    double v = atof(p);
                    if (isfinite(v) && v >= 0.005 && v <= 8.0) t->rel = v;
                }
            } else if (strcmp(p,"adsr")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p && strchr(p, ':')) {
                    double a, d, s, r;
                    if (adsr_colon(p, &a, &d, &s, &r)) apply_env(t, a, d, s, r);
                } else if (p && is_num_tok(p)) {
                    double a = atof(p), d = 0, s = 0, r = 0;
                    char pk[64];
                    int got = 1;
                    for (int k = 0; k < 3; k++) {
                        peek_tok(sp, pk, sizeof(pk));
                        if (!is_num_tok(pk)) { got = 0; break; }
                        p = strtok_r(NULL, " \t", &sp);
                        if (k == 0) d = atof(p);
                        else if (k == 1) s = atof(p);
                        else r = atof(p);
                    }
                    if (got) apply_env(t, a, d, s, r);
                }
            } else if (strcmp(p,"scale")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) {
                    int root = 48, n = 0, steps[12];
                    if (harmony_scale(p, &root, steps, 12, &n) && n > 0 && n <= 12) {
                        t->scale_root = root;
                        t->scale_n = n;
                        for (int i = 0; i < n; i++) t->scale_steps[i] = steps[i];
                    } else {
                        log_console("scale '%s' unknown", p);
                    }
                }
            } else if (strcmp(p,"transpose")==0 || strcmp(p,"add")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) {
                    char *end = NULL;
                    long v = strtol(p, &end, 10);
                    if (end != p && *end == 0 && v >= -48 && v <= 48)
                        t->semi = (int)v;
                }
            } else if (strcmp(p,"arp")==0) {
                char abuf[256];
                size_t used = 0;
                abuf[0] = 0;
                for (;;) {
                    char pk[64];
                    peek_tok(sp, pk, sizeof(pk));
                    if (!pk[0] || is_pipe_word(pk)) break;
                    p = strtok_r(NULL, " \t", &sp);
                    if (!p) break;
                    size_t pl = strlen(p);
                    if (used && used + 1 < sizeof(abuf)) abuf[used++] = ' ';
                    if (used + pl >= sizeof(abuf)) break;
                    memcpy(abuf + used, p, pl);
                    used += pl;
                    abuf[used] = 0;
                }
                if (abuf[0]) {
                    Node *an = parse_pattern_text(abuf);
                    node_free(t->arp);
                    t->arp = an;
                    const char *ae = pattern_last_error();
                    if (ae && *ae) log_console("arp: %s", ae);
                }
            } else if (strcmp(p,"lfo")==0) {
                p = strtok_r(NULL, " \t", &sp);
                int tgt = p ? lfo_target(p) : 0;
                if (tgt) {
                    char pk[64];
                    int wave = 0;
                    peek_tok(sp, pk, sizeof(pk));
                    int sh = lfo_shape(pk);
                    if (sh >= 0) {
                        p = strtok_r(NULL, " \t", &sp);
                        wave = sh;
                        peek_tok(sp, pk, sizeof(pk));
                    }
                    if (is_num_tok(pk)) {
                        p = strtok_r(NULL, " \t", &sp);
                        double rate = atof(p);
                        double depth = default_lfo_depth(tgt);
                        peek_tok(sp, pk, sizeof(pk));
                        if (is_num_tok(pk)) {
                            p = strtok_r(NULL, " \t", &sp);
                            double dv = atof(p);
                            if (isfinite(dv) && dv >= 0.0) depth = dv;
                        }
                        if (isfinite(rate) && rate > 0.0 && rate <= 200.0) {
                            t->lfo_tgt = tgt;
                            t->lfo_wave = wave;
                            t->lfo_hz = rate;
                            t->lfo_depth = depth;
                        }
                    }
                }
            } else if (strcmp(p,"vib")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s", p);
                    char *c = strchr(buf, ':');
                    double depth = 1.0;
                    if (c) { *c = 0; depth = atof(c + 1); }
                    double rate = atof(buf);
                    if (isfinite(rate) && rate > 0.0 && rate <= 200.0) {
                        t->lfo_tgt = 1;
                        t->lfo_wave = 1;
                        t->lfo_hz = rate;
                        if (!isfinite(depth) || depth < 0.0) depth = 1.0;
                        t->lfo_depth = depth;
                    }
                }
            } else if (strcmp(p,"pw")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) {
                    double v = atof(p);
                    if (isfinite(v) && v >= 0.05 && v <= 0.95) t->pw = v;
                }
            } else if (strcmp(p,"lpf")==0 || strcmp(p,"cutoff")==0) {
                p = strtok_r(NULL, " \t", &sp);
                if (p) {
                    double v = 0;
                    t->lpf_slider=!strncmp(p,"slider(",7)?slider_next_index():-1;
                    if (slider_or_number(p, &v) && isfinite(v) && v >= 20.0 && v <= 18000.0)
                        t->lpf_hz = v;
                }
            }
            p = strtok_r(NULL, " \t", &sp);
        }
    }
}

static int blank_or_comment(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    if (*s == 0 || *s == '#') return 1;
    if (s[0] == '/' && s[1] == '/') return 1;
    return 0;
}

static int starts_with_dot(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    return *s == '.';
}

int track_load_from_code(const char *code_buf) {
    if (!code_buf) code_buf = "";
    if (strlen(code_buf) > (size_t)INT_MAX) {
        log_console("code exceeds the editor offset limit");
        return 0;
    }
    JsProgram *program = NULL;
    cJSON *metadata = NULL;
    if (javascript_is_code(code_buf)) {
        char error[512];
        program = javascript_compile(code_buf,audio_get_bpm(),&metadata,error,sizeof(error));
        pthread_mutex_lock(&tracks_lock);
        JsProgram *previous=track_count==1?tracks[0].javascript:NULL;
        javascript_retain(previous);
        pthread_mutex_unlock(&tracks_lock);
        if (program && !javascript_prepare_csound_from(program,metadata,previous,error,sizeof(error))) {
            javascript_free(program); program = NULL;
            cJSON_Delete(metadata); metadata = NULL;
        }
        javascript_free(previous);
        if (!program) {
            pthread_mutex_lock(&tracks_lock);
            int stored = current_code_set(code_buf);
            pthread_mutex_unlock(&tracks_lock);
            if (!stored) log_console("out of memory");
            else log_console("JavaScript: %s (keeping the last playable pattern)",error);
            return 0;
        }
        char *samples = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(metadata,"samples"));
        websamp_load_arguments(samples);
        free(samples);
    } else {
    /* Fetch sample-map JSON before taking tracks_lock. Audio files
       themselves download later, on the first note that uses them. */
        websamp_load_code(code_buf);
    }
    /* The legacy parser writes NULs into its copy as it splits lines. */
    char *buf = NULL;
    if (!program) {
        size_t clen = strlen(code_buf);
        buf = malloc(clen + 1);
        if (!buf) {
            log_console("out of memory");
            return 0;
        }
        memcpy(buf, code_buf, clen + 1);
    }
    pthread_mutex_lock(&tracks_lock);
    if (!current_code_set(code_buf)) {
        pthread_mutex_unlock(&tracks_lock);
        free(buf);
        if (program) javascript_free(program);
        cJSON_Delete(metadata);
        log_console("out of memory");
        return 0;
    }
    double now=audio_time_now(), music_cursor=0, tick=now;
    double boundary=round((now+.01)*10000)/10000+.1;
    int replace_javascript=program && track_count==1 && tracks[0].javascript;
    int continuous=replace_javascript && audio_is_playing() && tracks[0].js_seconds>0;
    if (continuous) {
        music_cursor=tracks[0].js_cursor;
        boundary=tracks[0].js_phase+.1;
        tick=tracks[0].js_tick;
        if (boundary<now+.05) {
            music_cursor+=(now+.1-boundary)/tracks[0].js_seconds;
            boundary=now+.1; tick=now;
        }
    }
    free_tracks();
    /* Web output has already accepted these notes. Replacing a pattern, even
       after Stop, invalidates producers without withdrawing committed audio. */
    if (replace_javascript) event_queue_new_program(HUGE_VAL);
    else event_queue_clear();
    audio_set_csound(javascript_csound(program));
    audio_set_reference_mode(program != NULL);
    if (program) {
        javascript_activate_sliders(program);
        Track *t = &tracks[0];
        memset(t,0,sizeof(*t));
        t->gain_slider=t->lpf_slider=-1;
        t->slider_revision=slider_revision();
        t->javascript = program;
        const cJSON *visuals = cJSON_GetObjectItemCaseSensitive(metadata,"visuals");
        t->visual_requests = cJSON_IsNumber(visuals) ? visuals->valueint & 3 : 0;
        t->compile_generation = event_queue_generation();
        t->active = t->fast_repeat = t->slow_stride = 1;
        snprintf(t->keyword,sizeof(t->keyword),"javascript");
        track_count = 1;
        const cJSON *tempo = cJSON_GetObjectItemCaseSensitive(metadata,"bpm");
        if (cJSON_IsNumber(tempo) && isfinite(tempo->valuedouble)) audio_set_bpm(tempo->valuedouble);
        t->js_seconds=audio_sec_per_cycle();
        t->js_cursor=music_cursor; t->js_origin=isfinite(t->js_seconds)?boundary-music_cursor*t->js_seconds:boundary; t->js_tick=tick;
        t->js_anchor_cycle=music_cursor;
        t->js_anchor_time=boundary; t->js_phase=boundary-.1;
        t->cycle_index=music_cursor>=INT_MAX?INT_MAX:music_cursor<=0?0:(int)floor(music_cursor);
        t->next_cycle=isfinite(t->js_seconds)?t->js_origin+t->cycle_index*t->js_seconds:boundary;
        cJSON_Delete(metadata);
        pthread_mutex_unlock(&tracks_lock);
        if (!continuous) scheduler_song_sync();
        log_console("Loaded JavaScript/Strudel pattern, bpm %.0f",audio_get_bpm());
        return 1;
    }
    now = audio_time_now();
    // ensure now maybe 0; align
    if (now < 0.1) now = 0.1;

    slider_begin();
    /* Join a statement while a quote is open, and while the next code
       line starts with '.'. Comments are skipped, so an apostrophe in
       a // comment does not glue the following lines. */
    char *cursor = buf;
    while (*cursor && track_count < MAX_TRACKS) {
        char *start = cursor;
        int q = 0;
        while (*cursor) {
            if (!q && cursor[0] == '/' && cursor[1] == '/') {
                while (*cursor && *cursor != '\n') cursor++;
                continue;
            }
            if (!q && (*cursor == '"' || *cursor == '\'' || *cursor == '`')) {
                q = *cursor;
                cursor++;
                continue;
            }
            if (q && *cursor == '\\' && cursor[1]) { cursor += 2; continue; }
            if (q && *cursor == q) { q = 0; cursor++; continue; }
            if (*cursor == '\n' && !q) {
                char *look = cursor + 1;
                int join = 0;
                while (*look) {
                    char *ls = look;
                    while (*look && *look != '\n') look++;
                    char save = *look;
                    *look = 0;
                    int blank = blank_or_comment(ls);
                    int dot = starts_with_dot(ls);
                    *look = save;
                    if (blank) {
                        if (*look == '\n') look++;
                        else break;
                        continue;
                    }
                    join = dot;
                    break;
                }
                if (join) { *cursor = ' '; cursor++; continue; }
                break;
            }
            cursor++;
        }
        if (*cursor == '\n') { *cursor = 0; cursor++; }
        parse_one_line(start, (int)(start - buf), now);
    }

    pthread_mutex_unlock(&tracks_lock);
    free(buf);
    scheduler_song_sync();
    log_console("Loaded %d track(s), bpm %.0f", track_count, audio_get_bpm());
    return 1;
}
