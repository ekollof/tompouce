#include "track.h"
#include "pattern.h"
#include "audio.h"
#include "scheduler.h"
#include "wavetable.h"
#include "sampler.h"
#include "banks.h"
#include "harmony.h"
#include <ctype.h>

Track tracks[MAX_TRACKS];
int track_count = 0;
pthread_mutex_t tracks_lock = PTHREAD_MUTEX_INITIALIZER;
char current_code[65536] = {0};

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
        node_free(tracks[i].pattern);
        node_free(tracks[i].arp);
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

static void parse_one_line(char *line, double now) {
    trim(line);
    if (line[0]==0 || line[0]=='#') return;
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
    snprintf(t->keyword, sizeof(t->keyword), "%s", kw);
    t->pattern = pat;
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
                    double g = atof(p);
                    if (!isfinite(g)) g = 0.8;
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
                    double v = atof(p);
                    if (isfinite(v) && v >= 20.0 && v <= 18000.0) t->lpf_hz = v;
                }
            }
            p = strtok_r(NULL, " \t", &sp);
        }
    }
}

void track_load_from_code(const char *code_buf) {
    pthread_mutex_lock(&tracks_lock);
    free_tracks();
    event_queue_clear();
    if (code_buf) { strncpy(current_code, code_buf, 65535); current_code[65535] = 0; } else current_code[0]=0;
    // copy buffer
    char buf[65536];
    if (code_buf) strncpy(buf, code_buf, 65535); else buf[0]=0;
    buf[65535] = 0;

    double now = audio_time_now();
    // ensure now maybe 0; align
    if (now < 0.1) now = 0.1;

    char *sp = NULL;
    char *line = strtok_r(buf, "\n", &sp);
    while (line && track_count < MAX_TRACKS) {
        parse_one_line(line, now);
        line = strtok_r(NULL, "\n", &sp);
    }

    pthread_mutex_unlock(&tracks_lock);
    scheduler_song_sync();
    log_console("Loaded %d track(s), bpm %.0f", track_count, audio_get_bpm());
}