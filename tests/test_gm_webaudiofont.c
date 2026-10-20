#include "gm_synth.h"
#include "banks.h"
#include "dsp.h"
#include "sampler.h"
#include <cjson/cJSON.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "fixtures/mp3-gapless.h"

/* Focused webaudiofont pack tests: confined base64 vectors, synthetic pack
   parsing, range-based zone selection and detune/loop math. No network, no
   filesystem; the live fetch/materialize path is covered by the gm-epiano1
   compare-audio gate instead. */

static int fails = 0;

static void test_installed_pipeline(void);

static void expect(int ok, const char *name) {
    if (!ok) { fprintf(stderr, "FAIL %s\n", name); fails++; }
}

static void test_b64_vectors(void) {
    unsigned char out[16];
    memset(out, 0, sizeof(out));
    expect(gm_waf_b64_decode("TWFu", 4, out, sizeof(out)) == 3, "b64 Man length");
    expect(!memcmp(out, "Man", 3), "b64 Man bytes");
    expect(gm_waf_b64_decode("TWE=", 4, out, sizeof(out)) == 2, "b64 Ma length");
    expect(!memcmp(out, "Ma", 2), "b64 Ma bytes");
    expect(gm_waf_b64_decode("TQ==", 4, out, sizeof(out)) == 1, "b64 M length");
    expect(out[0] == 'M', "b64 M byte");
    expect(gm_waf_b64_decode("TWFu\n", 5, out, sizeof(out)) == 3, "b64 whitespace skipped");
    expect(!memcmp(out, "Man", 3), "b64 whitespace bytes");
    expect(gm_waf_b64_decode("", 0, out, sizeof(out)) < 0, "b64 empty rejected");
    expect(gm_waf_b64_decode("TWF!", 4, out, sizeof(out)) < 0, "b64 bad alphabet rejected");
    expect(gm_waf_b64_decode("TW=F", 4, out, sizeof(out)) < 0, "b64 mid padding rejected");
    expect(gm_waf_b64_decode("TWF", 3, out, sizeof(out)) < 0, "b64 ragged length rejected");
    expect(gm_waf_b64_decode("TWFu====", 8, out, sizeof(out)) < 0, "b64 excess padding rejected");
    expect(gm_waf_b64_decode("TWFu", 4, out, 2) < 0, "b64 capacity overflow rejected");
    expect(gm_waf_b64_decode("TWFu", 4, NULL, sizeof(out)) < 0, "b64 null out rejected");
    expect(gm_waf_b64_decode(NULL, 4, out, sizeof(out)) < 0, "b64 null text rejected");
}

static const char pack_text[] =
    "console.log('load _tone_0040_JCLive_sf2_file');\n"
    "var _tone_0040_JCLive_sf2_file={\n"
    "\tzones:[\n"
    "\t\t{\n"
    "\t\t\tmidi:4\n"
    "\t\t\t,originalPitch:4400\n"
    "\t\t\t,keyRangeLow:0\n"
    "\t\t\t,keyRangeHigh:40\n"
    "\t\t\t,loopStart:13689\n"
    "\t\t\t,loopEnd:14117\n"
    "\t\t\t,coarseTune:0\n"
    "\t\t\t,fineTune:16\n"
    "\t\t\t,sampleRate:44100\n"
    "\t\t\t,ahdsr:true\n"
    "\t\t\t,file:'TWFu'\n"
    "\t\t\t,anchor:0.00147392\n"
    "\t\t\t//_tone.Soft_EP_E5\n"
    "\t\t}\n"
    "\t\t,{\n"
    "\t\t\tmidi:4\n"
    "\t\t\t,originalPitch:6800\n"
    "\t\t\t,keyRangeLow:58\n"
    "\t\t\t,keyRangeHigh:60\n"
    "\t\t\t,loopStart:100\n"
    "\t\t\t,loopEnd:200\n"
    "\t\t\t,coarseTune:0\n"
    "\t\t\t,fineTune:-60\n"
    "\t\t\t,sampleRate:44100\n"
    "\t\t\t,ahdsr:true\n"
    "\t\t\t,file:'TWE='\n"
    "\t\t}\n"
    "\t\t,{\n"
    "\t\t\tmidi:4\n"
    "\t\t\t,originalPitch:7200\n"
    "\t\t\t,keyRangeLow:63\n"
    "\t\t\t,keyRangeHigh:65\n"
    "\t\t\t,coarseTune:0\n"
    "\t\t\t,fineTune:0\n"
    "\t\t\t,sampleRate:44100\n"
    "\t\t\t,ahdsr:true\n"
    "\t\t\t,file:'TQ=='\n"
    "\t\t}\n"
    "\t]\n"
    "};\n";

static void test_pack_parse(void) {
    GmWafZone zones[GM_WAF_MAX_ZONES];
    int n = gm_waf_parse(pack_text, zones, GM_WAF_MAX_ZONES);
    expect(n == 3, "pack zone count");
    if (n != 3) return;
    expect(zones[0].low == 0 && zones[0].high == 40, "pack zone0 range");
    expect(zones[0].original_pitch == 4400, "pack zone0 pitch");
    expect(zones[0].coarse == 0 && zones[0].fine == 16, "pack zone0 tune");
    expect(zones[0].loop_start == 13689 && zones[0].loop_end == 14117, "pack zone0 loop");
    expect(zones[0].sample_rate == 44100, "pack zone0 rate");
    expect(zones[0].file_len == 4 && !memcmp(zones[0].file, "TWFu", 4), "pack zone0 file slice");
    expect(zones[1].low == 58 && zones[1].high == 60, "pack zone1 range");
    expect(zones[1].fine == -60, "pack zone1 negative fine");
    expect(zones[2].loop_start == 0 && zones[2].loop_end == 0, "pack zone2 missing loop disabled");
    expect(zones[2].file_len == 4 && !memcmp(zones[2].file, "TQ==", 4), "pack zone2 file slice");
    expect(gm_waf_parse("var x={};", zones, GM_WAF_MAX_ZONES) < 0, "pack without zones rejected");
    expect(gm_waf_parse(NULL, zones, GM_WAF_MAX_ZONES) < 0, "pack null rejected");
    expect(gm_waf_parse(pack_text, NULL, GM_WAF_MAX_ZONES) < 0, "pack null zones rejected");
    expect(gm_waf_parse(pack_text, zones, 0) < 0, "pack zero capacity rejected");
}

static void test_pack_bounds(void) {
    /* Over-bound text is rejected without scanning it. */
    size_t big = (size_t)2*1024*1024+16;
    char *text = malloc(big+1);
    if (!text) { expect(0, "pack bounds alloc"); return; }
    memset(text, ' ', big);
    text[big] = 0;
    GmWafZone zones[4];
    expect(gm_waf_parse(text, zones, 4) < 0, "pack over 2MiB rejected");
    free(text);
}

static void test_zone_pick(void) {
    GmWafZone zones[GM_WAF_MAX_ZONES];
    int n = gm_waf_parse(pack_text, zones, GM_WAF_MAX_ZONES);
    expect(n == 3, "pick parse");
    if (n != 3) return;
    expect(gm_waf_pick(zones, n, 0) == 0, "pick low edge inclusive");
    expect(gm_waf_pick(zones, n, 40) == 0, "pick high edge inclusive");
    expect(gm_waf_pick(zones, n, 41) == 0, "pick high+1 slop extends range");
    expect(gm_waf_pick(zones, n, 57) == -1, "pick gap misses (not nearest-root)");
    expect(gm_waf_pick(zones, n, 59) == 1, "pick mid range");
    expect(gm_waf_pick(zones, n, 60) == 1, "pick range end inclusive");
    expect(gm_waf_pick(zones, n, 61) == 1, "pick slop past range end stays");
    expect(gm_waf_pick(zones, n, 60.5) == 1, "pick fractional keeps first range match");
    expect(gm_waf_pick(zones, n, 64) == 2, "pick third zone");
    expect(gm_waf_pick(zones, n, 127) == -1, "pick above all misses");
    expect(gm_waf_pick(zones, n, -5) == -1, "pick below zero misses");
    expect(gm_waf_pick(NULL, n, 60) < 0, "pick null rejected");
    expect(gm_waf_pick(zones, 0, 60) < 0, "pick empty rejected");
}

static void test_zone_math(void) {
    GmWafZone zones[GM_WAF_MAX_ZONES];
    int n = gm_waf_parse(pack_text, zones, GM_WAF_MAX_ZONES);
    expect(n == 3, "math parse");
    if (n != 3) return;
    /* Web convention: eff = original - coarse*100 - fine.
       Zone0: 4400 - 16 = 4384 -> base 44, bake 2^(-0.16/12).
       Zone1: 6800 + 60 = 6860 -> base 69. */
    expect(gm_waf_base_midi(&zones[0]) == 44, "math base rounds web effective root");
    expect(gm_waf_base_midi(&zones[1]) == 69, "math base rounds negative fine up");
    expect(gm_waf_base_midi(&zones[2]) == 72, "math base exact root");
    /* Correction closes web_rate/native_rate exactly; note cancels out, so
       only the zone's e = orig - 100*coarse - fine and the integer base enter:
       zone0: 2^((4400-4384)/1200). */
    double corr = gm_waf_speed_correction(&zones[0], 44);
    expect(fabs(corr-pow(2.0, 16.0/1200.0)) < 1e-12, "math speed correction matches web rate");
    expect(fabs(gm_waf_speed_correction(&zones[2], 72)-1.0) < 1e-12, "math exact root needs no correction");
    expect(fabs(gm_waf_speed_correction(NULL, 44)-1.0) < 1e-12, "math null correction defaults");
    expect(fabs(gm_waf_speed_correction(&zones[0], 200)-1.0) < 1e-12, "math bad base defaults");
    expect(gm_waf_base_midi(NULL) == 60, "math null base defaults");
    double lo = 0, hi = 0;
    expect(gm_waf_loop_fractions(&zones[0], 30000, 44100, &lo, &hi), "math loop valid");
    expect(fabs(lo-13689.0/30000.0) < 1e-12 && fabs(hi-14117.0/30000.0) < 1e-12,
        "math loop fractions convert through equal rates");
    /* Rate conversion runs through the zone rate: synthetic zones declare
       44100 Hz over a 32000 Hz decode. */
    expect(gm_waf_loop_fractions(&zones[0], 15023, 32000, &lo, &hi), "math loop rate conversion valid");
    expect(fabs(lo-(13689.0/44100)/(15023.0/32000)) < 1e-9 &&
        fabs(hi-(14117.0/44100)/(15023.0/32000)) < 1e-9, "math loop uses zone rate");
    expect(!gm_waf_loop_fractions(&zones[0], 30000, 8, &lo, &hi), "math bad decode rate rejected");
    expect(!gm_waf_loop_fractions(&zones[2], 30000, 44100, &lo, &hi), "math disabled loop rejected");
    expect(gm_waf_loop_fractions(&zones[0], 14000, 44100, &lo, &hi), "math loop past end stays valid");
    expect(fabs(hi-14117.0/14000.0) < 1e-12, "math past-end loop plays to buffer end");
    expect(!gm_waf_loop_fractions(&zones[0], 0, 44100, &lo, &hi), "math empty decode rejected");
    expect(!gm_waf_loop_fractions(NULL, 30000, 44100, &lo, &hi), "math null zone rejected");
    expect(!gm_waf_loop_fractions(&zones[0], 30000, 44100, NULL, &hi), "math null out rejected");
}

int main(void) {
    test_b64_vectors();
    test_pack_parse();
    test_pack_bounds();
    test_zone_pick();
    test_zone_math();
    if (fails) {
        fprintf(stderr, "%d failed\n", fails);
        return 1;
    }
    test_installed_pipeline();
    if (fails) {
        fprintf(stderr, "%d failed\n", fails);
        return 1;
    }
    printf("gm_webaudiofont ok\n");
    return 0;
}

/* Test-only base64 encoder for the fixture MP3. */
static void test_b64_encode(const unsigned char *in, size_t len, char *out) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned a = in[i], b = i+1 < len ? in[i+1] : 0, c = i+2 < len ? in[i+2] : 0;
        out[o++] = alphabet[a>>6]; out[o++] = alphabet[((a&3)<<4)|(b>>4)];
        out[o++] = i+1 < len ? alphabet[((b&15)<<2)|(c>>6)] : '=';
        out[o++] = i+2 < len ? alphabet[c&63] : '=';
    }
    out[o] = 0;
}

/* Offline end-to-end pipeline in a forked child: installed fixture pack,
   confined parse, MP3 decode, staged WAV, pitch-keyed acquire, loop fractions
   and the exact web rate correction. The pack-text fetch must never fire
   (asserted by the absence of gm_*.js in the scratch web cache). */
static void test_installed_pipeline(void) {
    pid_t child = fork();
    if (child < 0) { expect(0, "pipeline fork"); return; }
    if (child) {
        int status = 0;
        while (waitpid(child, &status, 0) < 0) {}
        expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "pipeline child ok");
        return;
    }
    int rc = 1;
    char root[] = "/tmp/tompouce-gm-pack-XXXXXX";
    if (!mkdtemp(root)) { fprintf(stderr, "FAIL pipeline mkdtemp\n"); _exit(1); }
    setenv("HOME", root, 1);
    setenv("XDG_CACHE_HOME", root, 1);
    setenv("XDG_CONFIG_HOME", root, 1);
    setenv("TOMPOUCE_BANKS", root, 1);
    char dir[512], pack[640];
    snprintf(dir, sizeof(dir), "%s/gm-webaudiofont", root);
    if (mkdir(dir, 0700) != 0) { fprintf(stderr, "FAIL pipeline mkdir\n"); _exit(1); }
    snprintf(pack, sizeof(pack), "%s/0040_JCLive_sf2_file.js", dir);
    size_t b64len = (gapless_mono_mp3_len+2)/3*4+1;
    char *b64 = malloc(b64len);
    if (!b64) _exit(1);
    test_b64_encode(gapless_mono_mp3, gapless_mono_mp3_len, b64);
    char text[8192];
    int tlen = snprintf(text, sizeof(text),
        "var _tone_pack={zones:[{midi:4,originalPitch:6000,"
        "keyRangeLow:0,keyRangeHigh:127,loopStart:100,loopEnd:1000,coarseTune:0,"
        "fineTune:18,sampleRate:22050,ahdsr:true,file:'%s'}]};", b64);
    free(b64);
    if (tlen <= 0 || tlen >= (int)sizeof(text)) { fprintf(stderr, "FAIL pipeline fixture\n"); _exit(1); }
    /* gm_epiano1 resolves 0040_JCLive_sf2_file.js from the installed banks. */
    snprintf(pack, sizeof(pack), "%s/0040_JCLive_sf2_file.js", dir);
    FILE *f = fopen(pack, "wb");
    int wrote = f && fwrite(text, 1, (size_t)tlen, f) == (size_t)tlen;
    if (!f || fclose(f) != 0 || !wrote) { fprintf(stderr, "FAIL pipeline fixture\n"); _exit(1); }
    banks_start_index(root);
    for (int i = 0; !banks_ready() && i < 500; i++) usleep(10000);
    if (!banks_ready()) { fprintf(stderr, "FAIL pipeline banks\n"); _exit(1); }
    AudioEvent event;
    memset(&event, 0, sizeof(event));
    snprintf(event.token, sizeof(event.token), "gm_epiano1");
    event.freq = 440.0*pow(2.0, (60-69)/12.0);
    event.smp = -1;
    event.smp_speed = 1.0;
    dsp_params_init(&event.dsp);
    event.dsp.enabled = 1;
    cJSON *value = cJSON_Parse("{}");
    if (!value) { fprintf(stderr, "FAIL pipeline json\n"); _exit(1); }
    if (!gm_synth_prepare_sample(&event, value, 60.0)) { fprintf(stderr, "FAIL pipeline prepare\n"); rc = 1; goto done; }
    /* Piano aliases share gm_piano's pack (same installed file, no refetch). */
    AudioEvent alias;
    memset(&alias, 0, sizeof(alias));
    snprintf(alias.token, sizeof(alias.token), "gm_bright_acoustic_piano");
    alias.freq = 440.0*pow(2.0, (60-69)/12.0);
    alias.smp = -1;
    alias.smp_speed = 1.0;
    dsp_params_init(&alias.dsp);
    alias.dsp.enabled = 1;
    if (event.smp < 0) { fprintf(stderr, "FAIL pipeline smp\n"); goto done; }
    /* smp_gate is assigned by the resolve_sample_source epilogue around this
       call (forced for gm_* there), not by preparation itself. */
    if (event.dsp.p[DSP_LOOP] != 1) { fprintf(stderr, "FAIL pipeline loop flag\n"); goto done; }
    if (!(event.dsp.p[DSP_LOOPBEGIN] > 0 && event.dsp.p[DSP_LOOPEND] > event.dsp.p[DSP_LOOPBEGIN])) {
        fprintf(stderr, "FAIL pipeline loop fractions\n"); goto done;
    }
    /* fine 18 with base 60 closes the web rate exactly through smp_speed. */
    if (fabs(event.smp_speed-pow(2.0, 18.0/1200.0)) > 1e-12) {
        fprintf(stderr, "FAIL pipeline correction %f\n", event.smp_speed); goto done;
    }
    if (!gm_synth_prepare_sample(&alias, value, 60.0) && alias.smp < 0) {
        /* Names outside the pinned web table keep the FluidSynth fallback:
           preparation declines and the resolver falls through. */
    } else { fprintf(stderr, "FAIL pipeline alias\n"); goto done; }
    float *pcm = NULL;
    int frames = 0, channels = 0;
    smp_get_stereo(event.smp, &pcm, &frames, &channels);
    if (!pcm || frames < 1000 || (channels != 1 && channels != 2)) {
        fprintf(stderr, "FAIL pipeline sample\n"); goto done;
    }
    /* No pack-text fetch may have fired: only staged WAVs sit in cache. */
    {
        char web[512];
        snprintf(web, sizeof(web), "%s/.cache/tompouce/web", root);
        DIR *dp = opendir(web);
        int js = 0, wav = 0;
        if (dp) {
            struct dirent *entry;
            while ((entry = readdir(dp)) != NULL) {
                size_t n = strlen(entry->d_name);
                if (n > 3 && !strcmp(entry->d_name+n-3, ".js")) js++;
                if (n > 4 && !strcmp(entry->d_name+n-4, ".wav")) wav++;
            }
            closedir(dp);
        }
        if (js != 0 || wav != 1) { fprintf(stderr, "FAIL pipeline cache js=%d wav=%d\n", js, wav); goto done; }
    }
    rc = 0;
done:
    cJSON_Delete(value);
    _exit(rc);
}
