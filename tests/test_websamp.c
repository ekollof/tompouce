#include "websamp.h"
#include "sampler.h"
#include "sound_store.h"
#include "common.h"
#include <curl/curl.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "fixtures/mp3-gapless.h"

static int fails = 0;

static int decode_fixture(const unsigned char *wav, size_t size, const char *extension) {
    char cache[512];
    user_dir(cache,sizeof(cache),NULL,"",".cache/tompouce");
    char temporary[512], path[512];
    if (!cache[0] || snprintf(temporary,sizeof(temporary),"%s/web/test-stereo-XXXXXX",cache) >= (int)sizeof(temporary)) return -1;
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    ssize_t written = write(fd,wav,size);
    close(fd);
    int length = snprintf(path,sizeof(path),"%s.%s",temporary,extension);
    if (written != (ssize_t)size || length < 0 || length >= (int)sizeof(path) || rename(temporary,path)) { unlink(temporary); return -1; }
    int id = smp_acquire_file(path,36);
    unlink(path); return id;
}

static void test_stereo_and_length(void) {
    const unsigned char wav[] = {
        'R','I','F','F',44,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,2,0,68,172,0,0,16,177,2,0,4,0,16,0,
        'd','a','t','a',8,0,0,0,0,32,0,192,0,16,0,48
    };
    int id = decode_fixture(wav,sizeof(wav),"wav");
    float *audio = NULL; int frames = 0, channels = 0;
    smp_get_stereo(id,&audio,&frames,&channels);
    if (!audio || frames != 2 || channels != 2 || fabs(audio[0]-.25) > 1e-6 ||
        fabs(audio[1]+.5) > 1e-6 || fabs(audio[2]-.125) > 1e-6 || fabs(audio[3]-.375) > 1e-6) fails++;
    const unsigned count = 44100*5, bytes = count*2;
    unsigned char *long_wav = calloc((size_t)bytes+44,1);
    if (!long_wav) { fails++; return; }
    memcpy(long_wav,wav,44);
    long_wav[22] = 1; long_wav[28] = 136; long_wav[29] = 88; long_wav[30] = 1; long_wav[32] = 2;
    unsigned sizes[] = {bytes+36,bytes};
    for (int field = 0; field < 2; field++)
        for (int byte = 0; byte < 4; byte++) long_wav[(field ? 40 : 4)+byte] = (unsigned char)(sizes[field]>>(byte*8));
    long_wav[44+bytes-1] = 32;
    id = decode_fixture(long_wav,(size_t)bytes+44,"wav");
    free(long_wav);
    smp_get_stereo(id,&audio,&frames,&channels);
    if (!audio || frames != (int)count || channels != 1 || fabs(audio[(size_t)(count-1)*2]-.25) > 1e-6) fails++;
}

static void test_decoded_levels(void) {
    char cache[512];
    user_dir(cache,sizeof(cache),NULL,"",".cache/tompouce");
    char temporary[512], path[512];
    if (!cache[0] || snprintf(temporary,sizeof(temporary),"%s/web/test-pcm-XXXXXX",cache) >= (int)sizeof(temporary)) {
        fails++; return;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) { fails++; return; }
    /* Mono PCM16, 44100 Hz: +.25, -.5, +.125, 0. */
    const unsigned char wav[] = {
        'R','I','F','F',44,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,1,0,68,172,0,0,136,88,1,0,2,0,16,0,
        'd','a','t','a',8,0,0,0,0,32,0,192,0,16,0,0
    };
    ssize_t written = write(fd,wav,sizeof(wav));
    close(fd);
    int length = snprintf(path,sizeof(path),"%s.wav",temporary);
    if (written != (ssize_t)sizeof(wav) || length < 0 || length >= (int)sizeof(path) || rename(temporary,path)) {
        unlink(temporary); fails++; return;
    }
    int id = smp_acquire_file(path,36);
    int other = smp_acquire_file(path,48);
    float *buffer = NULL; int frames = 0;
    smp_get(id,&buffer,&frames);
    if (!buffer || frames != 4 || fabs(buffer[0]-.25) > 1e-6 || fabs(buffer[1]+.5) > 1e-6 ||
        id == other || smp_base_midi(id) != 36 || smp_base_midi(other) != 48) fails++;
    unlink(path);
}

static void expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        fails++;
    }
}

static void expect_str(const char *got, const char *want, const char *msg) {
    if (!got || !want || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s\n  got  %s\n  want %s\n", msg, got ? got : "(null)", want);
        fails++;
    }
}

static void test_gapless_mp3(void) {
    const unsigned char *fixtures[] = {gapless_stereo_mp3, gapless_mono_mp3};
    const size_t lengths[] = {gapless_stereo_mp3_len, gapless_mono_mp3_len};
    for (int i = 0; i < 2; i++) {
        int id = decode_fixture(fixtures[i], lengths[i], "mp3");
        float *buffer = NULL; int frames = 0, channels = 0;
        smp_get_stereo(id, &buffer, &frames, &channels);
        expect(buffer && frames == 22050, "MP3 encoder delay and end padding removed before resampling");
        expect(channels == (i ? 1 : 2), "gapless MP3 channels preserved");
        if (!buffer) continue;
        int first = 0;
        while (first < frames && fabs(buffer[first*2]) < .0001) first++;
        expect(first < 100, "gapless MP3 attack starts without decoder latency");
        if (!i) {
            /* This fixture is already 44.1 kHz, so resampling cannot create
               fractional PCM16 steps and hide an integer decoder regression. */
            int precise=0;
            for (int frame=0;frame<frames;frame++) for (int ch=0;ch<2;ch++) {
                double scaled=(double)buffer[frame*2+ch]*32768;
                if (fabs(scaled-round(scaled))>.01) precise++;
            }
            expect(precise>frames, "MP3 decoding preserves floating-point detail without PCM16 quantization");
        }
    }
    unsigned char *without_tag = malloc(gapless_stereo_mp3_len);
    assert(without_tag);
    memcpy(without_tag, gapless_stereo_mp3, gapless_stereo_mp3_len);
    size_t tag = 0;
    while (tag+4 < gapless_stereo_mp3_len && memcmp(without_tag+tag, "Info", 4) &&
           memcmp(without_tag+tag, "Xing", 4)) tag++;
    assert(tag+12 < gapless_stereo_mp3_len);
    memset(without_tag+tag, 0, 4);
    int id = decode_fixture(without_tag, gapless_stereo_mp3_len, "mp3");
    float *buffer = NULL; int frames = 0;
    smp_get(id, &buffer, &frames);
    expect(buffer && frames > 22050, "untagged MP3 is not trimmed heuristically");
    memcpy(without_tag, gapless_stereo_mp3, gapless_stereo_mp3_len);
    memset(without_tag+tag+8, 255, 4);
    id = decode_fixture(without_tag, gapless_stereo_mp3_len, "mp3");
    smp_get(id, &buffer, &frames);
    expect(buffer && frames > 0 && frames < 44100, "untrusted MP3 frame count cannot expand the decode");
    expect(decode_fixture(without_tag, 32, "mp3") < 0, "truncated MP3 rejected");
    free(without_tag);
}

static void test_default_map(void) {
    char url[512];
    int before = websamp_name_count();
    expect(websamp_default_url("hh", 0, url, (int)sizeof(url)), "default hh");
    expect_str(url, "https://raw.githubusercontent.com/tidalcycles/uzu-drumkit/main/hh/10_hh_switchangel.wav", "uzu hh");
    expect(websamp_default_url("misc", 2, url, (int)sizeof(url)), "default misc:2");
    expect_str(url, "https://raw.githubusercontent.com/tidalcycles/uzu-drumkit/main/misc/12_misc_switchangel_ludens.wav", "uzu misc");
    expect(websamp_default_url("mt", 0, url, (int)sizeof(url)), "default mt");
    expect_str(url, "https://raw.githubusercontent.com/tidalcycles/uzu-drumkit/main/mt/10_mt_mot4i.wav", "uzu mt");
    expect(websamp_default_url("lt", 0, url, (int)sizeof(url)), "default lt");
    expect_str(url, "https://raw.githubusercontent.com/tidalcycles/uzu-drumkit/main/lt/10_lt_mot4i.wav", "uzu lt");
    expect(websamp_default_url("ht", 0, url, (int)sizeof(url)), "default ht");
    expect_str(url, "https://raw.githubusercontent.com/tidalcycles/uzu-drumkit/main/ht/10_ht_mot4i.wav", "uzu ht");
    expect(websamp_default_url("bd", 0, url, (int)sizeof(url)), "default bd");
    expect_str(url, "https://raw.githubusercontent.com/tidalcycles/uzu-drumkit/main/bd/10_bd_switchangel.wav", "uzu bd overwrites dirt");
    expect(!websamp_default_url("nope", 0, url, (int)sizeof(url)), "no default nope");
    expect(websamp_name_count() == before, "default map is not the user map");
}

static void write_fixture_wav(const char *path, unsigned char level) {
    unsigned char wav[] = {
        'R','I','F','F',38,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,1,0,68,172,0,0,136,88,1,0,2,0,16,0,
        'd','a','t','a',2,0,0,0,0,32
    };
    FILE *f = fopen(path, "wb");
    assert(f);
    wav[45] = level;
    assert(fwrite(wav, 1, sizeof(wav), f) == sizeof(wav));
    assert(fclose(f) == 0);
}

static void expect_fixture_sample(const char *token, double pitch, int root, float level) {
    int id = websamp_acquire(token, pitch);
    float *buffer = NULL; int frames = 0;
    smp_get(id, &buffer, &frames);
    assert(buffer && frames == 1 && buffer[0] == level);
    assert(smp_base_midi(id) == root);
}

static void local_defaults_child(int legacy) {
    char root[] = "/tmp/tompouce-defaults-XXXXXX";
    char url[512];
    assert(mkdtemp(root));
    assert(chdir(root) == 0);
    assert(mkdir("Dirt-Samples", 0700) == 0);
    /* Exercise a relative banks root and manifest order, not filename order. */
    if (legacy) {
        assert(unsetenv("TOMPOUCE_BANKS") == 0);
        assert(setenv("STRUDELTERM_BANKS", ".", 1) == 0);
    } else {
        assert(setenv("TOMPOUCE_BANKS", ".", 1) == 0);
        assert(setenv("STRUDELTERM_BANKS", "/does-not-exist", 1) == 0);
    }
    const char *dirs[] = {"dough-samples", "dough-samples/piano",
        "VCSL", "mrid",
        "tidal-drum-machines", "tidal-drum-machines/machines",
        "tidal-drum-machines/machines/TestMachine"};
    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) assert(mkdir(dirs[i], 0700) == 0);
    const char *files[] = {"Dirt-Samples/z.wav", "Dirt-Samples/a.wav",
        "dough-samples/piano/c3.wav", "dough-samples/piano/c4.wav",
        "dough-samples/piano/c4b.wav", "dough-samples/piano/ds4.wav",
        "VCSL/ball whistle.wav", "mrid/gumki.wav",
        "tidal-drum-machines/machines/TestMachine/z.wav",
        "tidal-drum-machines/machines/TestMachine/a.wav",
        "tidal-drum-machines/machines/TestMachine/m.wav"};
    const unsigned char levels[] = {32, 64, 32, 64, 96, 16, 32, 64, 32, 64, 96};
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) write_fixture_wav(files[i], levels[i]);
    assert(symlink("a.wav", "Dirt-Samples/link.wav") == 0);
    assert(symlink(".", "Dirt-Samples/sub") == 0);
    FILE *manifest = fopen("Dirt-Samples/strudel.json", "w");
    assert(manifest);
    assert(fputs("{\"kit\":[\"z.wav\",\"a.wav\"],\"hh\":[\"a.wav\"],"
                 "\"blocked\":[\"link.wav\"],\"blocked_dir\":[\"sub/a.wav\"],"
                 "\"escape\":[\"../a.wav\"]}", manifest) >= 0);
    assert(fclose(manifest) == 0);
    manifest = fopen("dough-samples/vcsl.json", "w");
    assert(manifest);
    assert(fputs("{\"ballwhistle\":[\"ball%20whistle.wav\"],"
                 "\"encoded_escape\":[\"%2e%2e/Dirt-Samples/a.wav\"],"
                 "\"encoded_nul\":[\"ball%00whistle.wav\"]}",manifest) >= 0);
    assert(fclose(manifest) == 0);
    manifest = fopen("dough-samples/mridangam.json", "w");
    assert(manifest);
    assert(fputs("{\"gumki\":[\"gumki.wav\"]}",manifest) >= 0);
    assert(fclose(manifest) == 0);
    manifest = fopen("dough-samples/piano.json", "w");
    assert(manifest);
    assert(fputs("{\"piano\":{\"C3\":\"c3.wav\",\"C4\":[\"c4.wav\",\"c4b.wav\"],"
                 "\"Ds4\":\"ds4.wav\"}}", manifest) >= 0);
    assert(fclose(manifest) == 0);
    manifest = fopen("dough-samples/tidal-drum-machines.json", "w");
    assert(manifest);
    assert(fputs("{", manifest) >= 0);
    /* More than 512 names and 64 slots are normal in the real catalog. */
    for (int i = 0; i < 600; i++)
        assert(fprintf(manifest, "\"Machine%d_sd\":[\"TestMachine/z.wav\"],", i) > 0);
    assert(fputs("\"TestMachine_sd\":[\"TestMachine/z.wav\",\"TestMachine/a.wav\","
                 "\"TestMachine/m.wav\"],\"LargeMachine_sd\":[", manifest) >= 0);
    for (int i = 0; i < 108; i++)
        assert(fprintf(manifest, "%s\"TestMachine/%s.wav\"", i ? "," : "", i == 107 ? "a" : "z") > 0);
    assert(fputs("]}", manifest) >= 0);
    assert(fclose(manifest) == 0);
    assert(!mkdir("Custom",0700));
    assert(!mkdir("Dough-Waveforms",0700));
    write_fixture_wav("Custom/one.wav",32);
    write_fixture_wav("Custom/two.wav",64);
    write_fixture_wav("Custom/orphan.wav",96);
    manifest=fopen("Custom/my-bank.json","w"); assert(manifest);
    assert(fputs("{\"customkit\":[\"two.wav\",\"one.wav\"]}",manifest)>=0); assert(!fclose(manifest));
    write_fixture_wav("Dough-Waveforms/a.wav",32); write_fixture_wav("Dough-Waveforms/b.wav",64);
    manifest=fopen("Dough-Waveforms/strudel.json","w"); assert(manifest);
    assert(fputs("{\"wt_test\":{\"D2\":[\"b.wav\",\"a.wav\"]}}",manifest)>=0); assert(!fclose(manifest));
    assert(websamp_default_url("kit", 0, url, sizeof(url)) && url[0] == '/');
    expect_fixture_sample("customkit:0",-1,36,.5f);
    expect_fixture_sample("customkit:1",-1,36,.25f);
    expect_fixture_sample("wt_test:0",69,38,.5f);
    expect_fixture_sample("wt_test:1",69,38,.25f);
    websamp_load_code("samples('bubo:waveforms')");
    assert(websamp_name_count()==0); /* Installed alias needs no fetch/user-map overlay. */
    expect_fixture_sample("wt_test:1",69,38,.25f);
    char canonical[PATH_MAX];
    assert(realpath("Custom/one.wav",canonical)); assert(websamp_manifest_contains(canonical));
    assert(realpath("Dirt-Samples/a.wav",canonical)); assert(websamp_manifest_contains(canonical)); /* shadowed hh */
    assert(realpath("VCSL/ball whistle.wav",canonical)); assert(websamp_manifest_contains(canonical));
    assert(realpath("Custom/orphan.wav",canonical)); assert(!websamp_manifest_contains(canonical));
    for (int i = 0; i < 2; i++) {
        int id = websamp_acquire(i ? "kit:1" : "kit:0", -1);
        float *buffer = NULL; int frames = 0;
        smp_get(id, &buffer, &frames);
        assert(buffer && frames == 1 && buffer[0] == (i ? .5f : .25f));
    }
    expect_fixture_sample("piano", 60, 60, .5f);
    expect_fixture_sample("piano:0", 54, 48, .25f);
    expect_fixture_sample("piano:0", 54.0001, 60, .5f);
    expect_fixture_sample("piano:0", 63, 63, .125f);
    expect_fixture_sample("piano:-1", 60, 60, .75f);
    expect_fixture_sample("piano:-0.5", 60, 60, .5f);
    expect_fixture_sample("piano:0.5", 60, 60, .75f);
    expect_fixture_sample("TestMachine_sd:0", -1, 36, .25f);
    expect_fixture_sample("testmachine_sd:2", -1, 36, .75f);
    expect_fixture_sample("TestMachine_sd:-1", -1, 36, .75f);
    expect_fixture_sample("Machine599_sd", -1, 36, .25f);
    expect_fixture_sample("LargeMachine_sd:107", -1, 36, .5f);
    expect_fixture_sample("LargeMachine_sd:108", -1, 36, .25f);
    expect_fixture_sample("ballwhistle", -1, 36, .25f);
    expect_fixture_sample("gumki", -1, 36, .5f);
    assert(!websamp_default_url("encoded_escape", 0, url, sizeof(url)));
    assert(!websamp_default_url("encoded_nul", 0, url, sizeof(url)));
    assert(websamp_name_count() == 0);
    assert(!websamp_default_url("blocked", 0, url, sizeof(url)));
    assert(!websamp_default_url("blocked_dir", 0, url, sizeof(url)));
    assert(!websamp_default_url("escape", 0, url, sizeof(url)));
    assert(websamp_default_url("hh", 0, url, sizeof(url)));
    assert(strstr(url, "/uzu-drumkit/main/hh/10_hh_switchangel.wav"));
    websamp_load_code("samples({kit:'https://example.com/user.wav'})");
    assert(websamp_name_count() == 1 && websamp_slot_url("kit", 0, url, sizeof(url)));
    assert(!strcmp(url, "https://example.com/user.wav"));
    websamp_load_code("samples({piano:{C4:'https://example.com/custom.wav'},"
                      "TestMachine_sd:['https://example.com/snare.wav']})");
    assert(websamp_name_count() == 2 && websamp_slot_count("piano") == 1);
    assert(websamp_slot_url("piano", 0, url, sizeof(url)) && !strcmp(url, "https://example.com/custom.wav"));
    assert(websamp_slot_count("TestMachine_sd") == 1);
    assert(websamp_default_url("piano", 0, url, sizeof(url)) && url[0] == '/');
    websamp_load_code("");
    expect_fixture_sample("piano", 60, 60, .5f);
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) assert(unlink(files[i]) == 0);
    assert(!unlink("Custom/one.wav") && !unlink("Custom/two.wav") && !unlink("Custom/orphan.wav"));
    assert(!unlink("Custom/my-bank.json") && !rmdir("Custom"));
    assert(!unlink("Dough-Waveforms/a.wav") && !unlink("Dough-Waveforms/b.wav"));
    assert(!unlink("Dough-Waveforms/strudel.json") && !rmdir("Dough-Waveforms"));
    assert(unlink("Dirt-Samples/link.wav") == 0);
    assert(unlink("Dirt-Samples/sub") == 0);
    assert(unlink("Dirt-Samples/strudel.json") == 0);
    assert(rmdir("Dirt-Samples") == 0);
    assert(unlink("dough-samples/piano.json") == 0);
    assert(unlink("dough-samples/tidal-drum-machines.json") == 0);
    assert(unlink("dough-samples/vcsl.json") == 0);
    assert(unlink("dough-samples/mridangam.json") == 0);
    for (size_t i = sizeof(dirs)/sizeof(dirs[0]); i > 0; i--) assert(rmdir(dirs[i-1]) == 0);
    assert(chdir("/") == 0 && rmdir(root) == 0);
}

static void test_local_defaults(void) {
    /* Defaults initialize once; keep the fixture independent of the real map. */
    for (int legacy = 0; legacy < 2; legacy++) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) { local_defaults_child(legacy); _exit(0); }
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "offline local defaults and bank env compatibility");
    }
}

static void test_installed_catalogs(void) {
    /* Optional real-bank integration; the isolated fixture above is always
       tested even on machines without the large sample installation. */
    char url[512];
    float *buffer = NULL;
    int frames = 0, channels = 0;
    if (access("assets/banks/dough-samples/piano/C4v8.mp3", R_OK) == 0) {
        expect(websamp_default_url("piano", 0, url, sizeof(url)), "installed piano catalog");
        expect_str(url, "https://raw.githubusercontent.com/felixroos/dough-samples/main/piano/A0v8.mp3", "default piano URL");
        int id = websamp_acquire("piano", 60);
        smp_get_stereo(id, &buffer, &frames, &channels);
        expect(buffer && frames > 10*44100 && channels == 2, "default piano is a full stereo recording");
        expect(frames == 713786, "C4 decoded length matches WebAudio's gapless output");
        expect(smp_base_midi(id) == 60, "default piano C4 root");
    }
    if (access("assets/banks/tidal-drum-machines/machines/RolandTR909/rolandtr909-sd/naredrum.wav", R_OK) == 0) {
        expect(websamp_default_url("RolandTR909_sd", 0, url, sizeof(url)), "TR909 snare catalog");
        expect_str(url, "https://raw.githubusercontent.com/ritchse/tidal-drum-machines/main/machines/RolandTR909/rolandtr909-sd/naredrum.wav", "TR909 snare slot zero");
        int id = websamp_acquire("RolandTR909_sd:0", -1);
        smp_get_stereo(id, &buffer, &frames, &channels);
        expect(buffer && frames == 12766 && channels == 2, "TR909 slot zero decodes the manifest file");
        expect(smp_base_midi(id) == 36, "plain default arrays retain root MIDI 36");
        id = websamp_acquire("RolandTR909_sd:2", -1);
        smp_get_stereo(id, &buffer, &frames, &channels);
        expect(buffer && frames == 7858 && channels == 1, "TR909 slot two decodes sd02");
        expect(websamp_default_url("YamahaRM50_sd", 107, url, sizeof(url)), "full 108-slot default bank");
        expect_str(url, "https://raw.githubusercontent.com/ritchse/tidal-drum-machines/main/machines/YamahaRM50/yamaharm50-sd/SNAREDRUM_108.wav", "default banks are not truncated at 64 slots");
    }
}

static void test_imported_samples(void) {
    char root[] = "/tmp/tompouce-import-XXXXXX", path[512], assets[512];
    const char *old = getenv("XDG_CONFIG_HOME");
    char *saved = old ? strdup(old) : NULL;
    assert(!old || saved);
    assert(mkdtemp(root));
    assert(!setenv("XDG_CONFIG_HOME",root,1));
    snprintf(path,sizeof(path),"%s/tompouce",root); assert(!mkdir(path,0700));
    snprintf(path,sizeof(path),"%s/tompouce/sounds",root); assert(!mkdir(path,0700));
    snprintf(assets,sizeof(assets),"%s/tompouce/sounds/assets",root); assert(!mkdir(assets,0700));
    const unsigned char wav[] = {
        'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,1,0,68,172,0,0,136,88,1,0,2,0,16,0,
        'd','a','t','a',4,0,0,0,0,32,0,192
    };
    const char *filename = "0000000000000000000000000000000000000000000000000000000000000000.wav";
    snprintf(path,sizeof(path),"%s/tompouce/sounds/assets/%s",root,filename);
    FILE *file = fopen(path,"wb"); assert(file);
    assert(fwrite(wav,1,sizeof(wav),file) == sizeof(wav)); assert(!fclose(file));
    snprintf(path,sizeof(path),"%s/tompouce/sounds/index.json",root);
    file = fopen(path,"w"); assert(file);
    assert(fprintf(file,"{\"version\":1,\"sounds\":{\"importfixture\":[{\"label\":\"a.wav\",\"file\":\"%s\"}]}}",filename) > 0);
    assert(!fclose(file));
    expect(user_sounds_reload() == 1,"reload persistent imported catalog");
    int id = websamp_acquire("importfixture",36), frames = 0, channels = 0;
    float *pcm = NULL;
    smp_get_stereo(id,&pcm,&frames,&channels);
    expect(id >= 0 && pcm && frames == 2 && channels == 1 && fabs(pcm[0]-.25) < 1e-6,"imported sample resolves and decodes natively");
    expect(websamp_acquire("importfixture:-1",36) == id,"imported sample slot wraps");
    assert(!unlink(path));
    snprintf(path,sizeof(path),"%s/tompouce/sounds/assets/%s",root,filename); assert(!unlink(path));
    assert(!rmdir(assets));
    snprintf(path,sizeof(path),"%s/tompouce/sounds",root); assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/tompouce",root); assert(!rmdir(path)); assert(!rmdir(root));
    if (saved) { assert(!setenv("XDG_CONFIG_HOME",saved,1)); free(saved); }
    else assert(!unsetenv("XDG_CONFIG_HOME"));
    (void)user_sounds_reload();
}

static void test_imported_pitch_banks(void) {
    char root[] = "/tmp/tompouce-pitch-XXXXXX", path[512], assets[512];
    const char *old = getenv("XDG_CONFIG_HOME");
    char *saved = old ? strdup(old) : NULL;
    assert(!old || saved);
    assert(mkdtemp(root));
    assert(!setenv("XDG_CONFIG_HOME",root,1));
    snprintf(path,sizeof(path),"%s/tompouce",root); assert(!mkdir(path,0700));
    snprintf(path,sizeof(path),"%s/tompouce/sounds",root); assert(!mkdir(path,0700));
    snprintf(assets,sizeof(assets),"%s/tompouce/sounds/assets",root); assert(!mkdir(assets,0700));
    const unsigned char wav[] = {
        'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,1,0,68,172,0,0,136,88,1,0,2,0,16,0,
        'd','a','t','a',4,0,0,0,0,32,0,192
    };
    const char *digests[] = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "1111111111111111111111111111111111111111111111111111111111111111",
        "2222222222222222222222222222222222222222222222222222222222222222",
        "3333333333333333333333333333333333333333333333333333333333333333",
        "4444444444444444444444444444444444444444444444444444444444444444"
    };
    for (int i = 0; i < 5; i++) {
        snprintf(path,sizeof(path),"%.400s/%.68s.wav",assets,digests[i]);
        FILE *file = fopen(path,"wb"); assert(file);
        assert(fwrite(wav,1,sizeof(wav),file) == sizeof(wav)); assert(!fclose(file));
    }
    snprintf(path,sizeof(path),"%s/tompouce/sounds/index.json",root);
    FILE *file = fopen(path,"w"); assert(file);
    assert(fprintf(file,
        "{\"version\":2,\"sounds\":{"
        "\"pitchbank\":["
        "{\"label\":\"lo-a.wav\",\"file\":\"%s.wav\",\"midi\":48,\"region\":0,\"bank\":\"manifest:kit\"},"
        "{\"label\":\"lo-b.wav\",\"file\":\"%s.wav\",\"midi\":48,\"region\":0},"
        "{\"label\":\"hi.wav\",\"file\":\"%s.wav\",\"midi\":60,\"region\":1,\"bank\":\"manifest:kit\"},"
        "{\"label\":\"sub.wav\",\"file\":\"%s.wav\",\"midi\":12,\"region\":2},"
        "{\"label\":\"bad-midi.wav\",\"file\":\"%s.wav\",\"midi\":200},"
        "{\"label\":\"bad-region.wav\",\"file\":\"%s.wav\",\"midi\":60,\"region\":-1},"
        "{\"label\":\"bad-bank.wav\",\"file\":\"%s.wav\",\"midi\":60,\"region\":9,\"bank\":7}],"
        "\"flatbank\":[{\"label\":\"a.wav\",\"file\":\"%s.wav\"}]}}",
        digests[0],digests[1],digests[2],digests[4],digests[0],digests[0],digests[0],digests[3]) > 0);
    assert(!fclose(file));
    expect(user_sounds_reload() == 2,"version-2 manifest bank catalog reloads");
    float *pcm = NULL; int frames = 0, channels = 0, id;
    id = websamp_acquire("pitchbank",48);
    expect(id >= 0 && smp_base_midi(id) == 48,"pitch region selects nearest key");
    id = websamp_acquire("pitchbank",60);
    expect(id >= 0 && smp_base_midi(id) == 60,"higher pitch selects higher region");
    id = websamp_acquire("pitchbank",54);
    expect(id >= 0 && smp_base_midi(id) == 48,"pitch ties keep declaration order");
    id = websamp_acquire("pitchbank",59);
    expect(id >= 0 && smp_base_midi(id) == 60,"closer pitch wins over order");
    id = websamp_acquire("pitchbank",-1);
    expect(id >= 0 && smp_base_midi(id) == 12,"drum triggers select by raw pitch like upstream");
    id = websamp_acquire("pitchbank",72);
    expect(id >= 0 && smp_base_midi(id) == 60,"high pitches select the top region");
    id = websamp_acquire("pitchbank:1",48);
    smp_get_stereo(id,&pcm,&frames,&channels);
    expect(id >= 0 && smp_base_midi(id) == 48,"sample index addresses its region");
    int first = websamp_acquire("pitchbank:0",48);
    expect(websamp_acquire("pitchbank:2",48) == first,"sample index wraps within its region");
    expect(websamp_acquire("pitchbank:0",60) != first,"regions hold distinct samples");
    id = websamp_acquire("flatbank",72);
    expect(id >= 0 && smp_base_midi(id) == 36,"flat banks keep the recording default");
    int held = websamp_acquire("pitchbank:0",48);
    float *held_pcm = NULL;
    smp_get_stereo(held,&held_pcm,&frames,&channels);
    expect(held >= 0 && held_pcm,"reimport baseline resolves");
    snprintf(path,sizeof(path),"%s/tompouce/sounds/index.json",root);
    file = fopen(path,"w"); assert(file);
    assert(fprintf(file,"{\"version\":2,\"sounds\":{\"flatbank\":[{\"label\":\"a.wav\",\"file\":\"%s.wav\"}]}}",
        digests[3]) > 0); assert(!fclose(file));
    expect(user_sounds_reload() == 1,"reimport drops the removed bank");
    smp_get_stereo(held,&pcm,&frames,&channels);
    expect(pcm && frames == 2,"queued notes keep decoded audio across reimports");
    expect(websamp_acquire("missingbank",60) == -1 || websamp_acquire("missingbank",60) >= 0,
        "unknown imported names fall through to defaults");
    websamp_load_code("samples({pitchbank:['https://example.com/x.wav']})");
    expect(websamp_acquire("pitchbank",48) < 0,"explicit sample maps shadow imported banks");
    websamp_load_code("");
    snprintf(path,sizeof(path),"%s/tompouce/sounds/index.json",root);
    file = fopen(path,"w"); assert(file);
    assert(fprintf(file,"{\"version\":3,\"sounds\":{}}") > 0); assert(!fclose(file));
    expect(user_sounds_reload() < 0,"unknown index versions fail without side effects");
    expect(websamp_acquire("flatbank",72) >= 0,"rejected reloads preserve the live catalog");
    assert(!unlink(path));
    for (int i = 0; i < 5; i++) {
        snprintf(path,sizeof(path),"%.400s/%.68s.wav",assets,digests[i]); assert(!unlink(path));
    }
    assert(!rmdir(assets));
    snprintf(path,sizeof(path),"%s/tompouce/sounds",root); assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/tompouce",root); assert(!rmdir(path)); assert(!rmdir(root));
    if (saved) { assert(!setenv("XDG_CONFIG_HOME",saved,1)); free(saved); }
    else assert(!unsetenv("XDG_CONFIG_HOME"));
    (void)user_sounds_reload();
}

int main(void) {
    test_local_defaults();
    test_imported_samples();
    test_imported_pitch_banks();
    char url[512];
    websamp_load_code("samples({voice:{c3:['a.wav','b.wav'],c4:['c.wav','d.wav']}},'https://example.com/')");
    expect(websamp_slot_count("voice") == 4,"pitch-keyed sample arrays");
    expect(websamp_slot_for("voice:-1",48) == 1,"negative sample index wraps within pitch region");
    expect(websamp_slot_for("voice:-0.5",48) == 0,"negative half rounds towards positive infinity");
    expect(websamp_slot_for("voice:0.5",60) == 3,"fractional index rounds before wrapping");
    expect(websamp_slot_for("voice:2.5",60) == 3,"large fractional index wraps");
    expect(websamp_slot_for("voice:0",54) == 0,"pitch ties preserve declaration order");
    expect(websamp_slot_for("voice:0",54.0001) == 2,"fractional pitch selects closest root without rounding");
    char large_map[4096];
    size_t used = (size_t)snprintf(large_map,sizeof(large_map),"samples({kit:[");
    for (int i = 0; i < 64; i++) {
        int n = snprintf(large_map+used,sizeof(large_map)-used,"%s'%d.wav'",i ? "," : "",i);
        if (n < 0 || (size_t)n >= sizeof(large_map)-used) return 1;
        used += (size_t)n;
    }
    snprintf(large_map+used,sizeof(large_map)-used,"]},'https://example.com/')");
    websamp_load_code(large_map);
    expect(websamp_slot_count("kit") == 64,"banks are not truncated at 32 slots");
    expect(websamp_slot_for("kit:-1",-1) == 63,"negative index in a large bank");
    expect(websamp_github_to_url("github:tidalcycles/dirt-samples", 1, url, (int)sizeof(url)), "github json");
    expect_str(url, "https://raw.githubusercontent.com/tidalcycles/dirt-samples/main/strudel.json", "github json url");
    expect(websamp_github_to_url("github:tidalcycles/Dirt-Samples/master", 0, url, (int)sizeof(url)), "github dir");
    expect_str(url, "https://raw.githubusercontent.com/tidalcycles/Dirt-Samples/master/", "github dir url");
    expect(websamp_github_to_url("github:someone", 1, url, (int)sizeof(url)), "github default repo");
    expect_str(url, "https://raw.githubusercontent.com/someone/samples/main/strudel.json", "default repo url");
    expect(!websamp_github_to_url("github:../etc", 1, url, (int)sizeof(url)), "reject dotdot");
    expect(!websamp_github_to_url("https://example.com/a.wav", 1, url, (int)sizeof(url)), "not github");

    expect(websamp_url_allowed("https://cdn.freesound.org/previews/339/339809_5121236-lq.mp3"), "freesound");
    expect(websamp_url_allowed("https://raw.githubusercontent.com/tidalcycles/Dirt-Samples/master/bd/BT0AADA.wav"), "github raw");
    expect(!websamp_url_allowed("file:///etc/passwd"), "file");
    expect(!websamp_url_allowed("http://127.0.0.1/a.wav"), "loopback");
    expect(!websamp_url_allowed("http://localhost/a.wav"), "localhost");
    expect(!websamp_url_allowed("http://10.1.2.3/a.wav"), "rfc1918");
    expect(!websamp_url_allowed("http://169.254.169.254/latest"), "metadata");
    expect(!websamp_url_allowed("http://user:pass@example.com/a.wav"), "userinfo");
    expect(!websamp_url_allowed("https://example.com/a wav.mp3"), "space");

    expect(websamp_resolve("github:tidalcycles/Dirt-Samples/master", "bd/BT0AADA.wav", url, (int)sizeof(url)), "join github");
    expect_str(url, "https://raw.githubusercontent.com/tidalcycles/Dirt-Samples/master/bd/BT0AADA.wav", "joined");
    expect(websamp_resolve("https://example.com/pack", "/bd/x.wav", url, (int)sizeof(url)), "join slash");
    expect_str(url, "https://example.com/pack/bd/x.wav", "joined slash");
    expect(websamp_resolve("", "https://cdn.freesound.org/a.mp3", url, (int)sizeof(url)), "absolute");
    expect(!websamp_resolve("http://127.0.0.1/pack/", "a.wav", url, (int)sizeof(url)), "bad base");
    test_default_map();
    test_installed_catalogs();

    const char *code =
        "// samples('http://127.0.0.1/evil.json')\n"
        "samples({\n"
        "  bd: 'bd/BT0AADA.wav',\n"
        "  sd: ['sd/a.wav', 'sd/b.wav'],\n"
        "  bell: { b4: 'https://cdn.freesound.org/previews/339/bell.mp3' },\n"
        "  hidden: 'http://127.0.0.1/secret.wav'\n"
        "}, 'github:tidalcycles/Dirt-Samples/master')\n"
        "s(\"bd sd\")\n";
    websamp_load_code(code);
    expect(websamp_name_count() == 3, "three names, private dropped");
    expect(websamp_slot_count("bd") == 1, "bd slots");
    expect(websamp_slot_count("sd") == 2, "sd slots");
    expect(websamp_slot_midi("sd",0) == 36, "plain arrays use Strudel root MIDI 36");
    expect(websamp_slot_midi("bd",0) == 36, "plain strings use Strudel root MIDI 36");
    expect(websamp_slot_count("hidden") == 0, "hidden gone");
    expect(websamp_slot_url("bd", 0, url, (int)sizeof(url)), "bd url");
    expect_str(url, "https://raw.githubusercontent.com/tidalcycles/Dirt-Samples/master/bd/BT0AADA.wav", "bd resolved");
    expect(websamp_slot_url("sd", 1, url, (int)sizeof(url)), "sd 1");
    expect_str(url, "https://raw.githubusercontent.com/tidalcycles/Dirt-Samples/master/sd/b.wav", "sd resolved");
    expect(websamp_slot_midi("bell", 0) == 71, "b4 midi");
    expect(websamp_slot_url("bell", 0, url, (int)sizeof(url)), "bell url");
    expect_str(url, "https://cdn.freesound.org/previews/339/bell.mp3", "bell absolute");

    websamp_load_code("samples({ bd: 'no-base.wav' })\n");
    expect(websamp_name_count() == 0, "relative without base is dropped");

    curl_global_init(CURL_GLOBAL_ALL);
    websamp_load_code(
        "samples({ bd: 'bd/BT0AADA.wav' }, 'github:tidalcycles/Dirt-Samples/master')\n"
        "s(\"bd\")\n");
    int id = websamp_acquire("bd", -1);
    expect(id >= 0, "download Dirt-Samples bd");
    expect(smp_base_midi(id) == 36, "decoded plain sample retains playback root");
    int id2 = websamp_acquire("bd:0", -1);
    expect(id2 == id, "second acquire is cached");
    expect(websamp_acquire("nope", -1) < 0, "unknown name");
    test_decoded_levels();
    test_stereo_and_length();
    test_gapless_mp3();
    websamp_load_code("samples({ bass: ['00_c2_finger_long_neck.wav'] }, 'github:cleary/samples-flbass/main/')");
    int bass_id = websamp_acquire("bass:0",45);
    expect(bass_id >= 0, "download melodic array sample");
    expect(smp_base_midi(bass_id) == 36, "melodic array does not infer pitch from filename");
    websamp_load_code("samples('https://raw.githubusercontent.com/felixroos/dough-samples/main/piano.json')");
    expect(websamp_slot_count("piano") == 29, "default piano manifest");
    expect(websamp_slot_midi("piano",2) == 27, "Ds accidentals in sample root keys");
    expect(websamp_slot_url("piano",0,url,sizeof(url)), "piano URL resolved");
    expect_str(url,"https://raw.githubusercontent.com/felixroos/dough-samples/main/piano/A0v8.mp3","manifest _base overrides URL directory");
    int piano_id = websamp_acquire("piano:0",60);
    expect(piano_id >= 0 && smp_base_midi(piano_id) == 60,"n selects within closest pitch region");
    curl_global_cleanup();

    if (fails) {
        fprintf(stderr, "%d failed\n", fails);
        return 1;
    }
    printf("websamp ok\n");
    return 0;
}
