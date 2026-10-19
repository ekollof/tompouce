#include "terminal_graphics.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static TerminalImage image;

static void test_scope_web_geometry(void) {
    const float samples[]={.5,.25,0,-.25,-.5,.5,0,-.5};
    const float late[]={-.5,-.25,0,.5,.25,0,-.25,-.5};
    const float inline_samples[]={.25,.125,0,-.125,-.25,.25,0,-.25};
    /* Coordinates independently gated against deployed drawing by
       scripts/scope-reference-qa.mjs (80 x 80 canvas). */
    const int expected_y[][8]={{60,65,70,50,60,70},
        {50,55,60,65,70,50,60,70},{65,70,50,60,70},
        {60,60,60,60,60,60,60,60},{50,45,40,35,30,50,40,30},
        {60,65,70},{40,50,60,20,40,60}};
    const int counts[]={6,8,5,8,8,3,6};
    for (int test=0;test<7;test++) {
        VisualOptions options; visual_options(&options,VISUAL_SCOPE,NULL);
        assert(options.scale==.25 && options.pos==.75 && options.thickness==3 && options.align);
        options.thickness=1; options.has_color=1; options.color=0xff0000ffu;
        if (test==1 || test==3 || test==4) options.align=0;
        if (test==2) options.trigger=.25;
        if (test==3) options.scale=0;
        if (test==4) { options.scale=-.25; options.pos=.5; }
        if (test==6) { options.scale=1; options.pos=.5; }
        image.width=80; image.height=80; image.compact=1;
        terminal_graphics_feedback(&image,NULL,VISUAL_SCOPE,&options,NULL,0,
            test==5?late:test==6?inline_samples:samples,8,0,0,1);
        for (int i=0;i<counts[test];i++) {
            const unsigned char *pixel=image.rgba+((size_t)expected_y[test][i]*80+i*10)*4;
            assert(pixel[0]==255 && pixel[3]==255);
        }
        /* Alignment must leave a blank suffix rather than zooming the trace. */
        for (int x=(counts[test]-1)*10+2;x<80;x++)
            for (int y=0;y<80;y++) assert(!image.rgba[((size_t)y*80+x)*4+3]);
        if (test==1) assert(!image.rgba[((size_t)60*80)*4+3]);
    }
    cJSON *config=cJSON_Parse("{\"scale\":-0.25,\"pos\":0.5,\"align\":false}");
    assert(config);
    VisualOptions options; visual_options(&options,VISUAL_SCOPE,config); cJSON_Delete(config);
    assert(options.scale==-.25 && options.pos==.5 && !options.align);
}

static void test_note_guides(void) {
    char name[8];
    assert(terminal_graphics_note_label(60,name,sizeof(name)) && !strcmp(name,"C4"));
    assert(terminal_graphics_note_label(61,name,sizeof(name)) && !strcmp(name,"C#4"));
    assert(terminal_graphics_note_label(0,name,sizeof(name)) && !strcmp(name,"C-1"));
    assert(terminal_graphics_note_label(127,name,sizeof(name)) && !strcmp(name,"G9"));
    assert(!terminal_graphics_note_label(128,name,sizeof(name)) && !name[0]);
    assert(!terminal_graphics_note_label(61,name,2));
    image.width = 320; image.height = 128;
    GraphicsNote notes[] = {{6,10.5,60},{7,9,64},{8,10.5,67}};
    terminal_graphics_roll(&image,notes,3,10);
    int labels = 0, keys = 0;
    for (int y = 0; y < image.height; y++) for (int x = 0; x < GRAPHICS_AXIS_WIDTH; x++) {
        const unsigned char *p = image.rgba+((size_t)y*image.width+x)*4;
        labels += p[0] == 190 && p[1] == 210 && p[3] == 255;
        keys += p[0] == 165 && p[1] == 185;
        assert(!(p[0] == 245 && p[1] == 205)); /* Notes cannot paint over labels. */
    }
    assert(labels > 50 && keys > 100);
}

static void test_rasters(void) {
    image.width = 600; image.height = 60;
    float samples[1024];
    for (int i = 0; i < 1024; i++) samples[i] = .01f*((i%512)/256.0f-1);
    samples[0] = NAN; samples[1] = INFINITY;
    terminal_graphics_scope(&image,samples,1024);
    int top = 0, bottom = 0;
    for (int y = 0; y < image.height; y++) for (int x = 0; x < image.width; x++) {
        const unsigned char *p = image.rgba+((size_t)y*image.width+x)*4;
        if (p[3] == 255 && p[1] == 235) { top += y < 8; bottom += y > 50; }
    }
    assert(top > 20 && bottom > 20);
    int amplitude_labels = 0;
    for (int y = 0; y < image.height; y++) for (int x = 0; x < GRAPHICS_AXIS_WIDTH; x++) {
        const unsigned char *p = image.rgba+((size_t)y*image.width+x)*4;
        amplitude_labels += p[0] == 190 && p[1] == 210 && p[3] == 255;
        assert(!(p[0] == 110 && p[1] == 235));
    }
    assert(amplitude_labels > 50);
    GraphicsNote notes[] = {{6.5,7.5,60},{9,10.5,67},{0,1,10},{NAN,9,99}};
    terminal_graphics_roll(&image,notes,4,10);
    int past = 0, active = 0;
    for (int i = 0; i < image.width*image.height; i++) {
        const unsigned char *p = image.rgba+(size_t)i*4;
        past += p[0] == 110 && p[1] == 235 && p[3] >= 190;
        active += p[0] == 90 && p[1] == 215 && p[3] >= 190;
    }
    assert(past > 100 && active > 100);
}

static void test_overlapping_notes(void) {
    image.width = 480; image.height = 128;
    GraphicsNote notes[] = {{6.2,9.9,60},{7,9.5,60},{8,10.5,60}};
    terminal_graphics_roll(&image,notes,3,10);
    int runs = 0, previous = 0;
    for (int y = 0; y < image.height; y++) {
        const unsigned char *p = image.rgba+((size_t)y*image.width+310)*4;
        int occupied = (p[0] == 110 && p[1] == 235) || (p[0] == 90 && p[1] == 215);
        if (occupied && !previous) runs++;
        previous = occupied;
    }
    assert(runs == 3); /* All three simultaneous notes must remain visible. */
    int onsets = 0;
    for (int x = 0; x < image.width; x++) {
        int marked = 0;
        for (int y = 0; y < image.height; y++) {
            const unsigned char *p = image.rgba+((size_t)y*image.width+x)*4;
            marked |= p[0] == 245 && p[1] == 205 && p[2] == 105;
        }
        onsets += marked;
    }
    assert(onsets == 6); /* Three distinct two-pixel onset markers survive. */
}

static int digit(int c) {
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *p = strchr(alphabet,c);
    assert(p && c); return (int)(p-alphabet);
}

static void test_transport(void) {
    image.width = 43; image.height = 37;
    for (int i = 0; i < image.width*image.height*4; i++) image.rgba[i] = (unsigned char)i;
    FILE *f = tmpfile(); assert(f);
    assert(terminal_graphics_probe(f));
    assert(terminal_graphics_send(f,77,&image,3,5,43,2,7,25));
    assert(!terminal_graphics_send(f,77,&image,3,5,43,2,30,25));
    assert(terminal_graphics_delete(f,77)); assert(fflush(f) == 0);
    long size = ftell(f); assert(size > 0);
    char *text = malloc((size_t)size+1); assert(text);
    rewind(f); assert(fread(text,1,(size_t)size,f) == (size_t)size); text[size] = 0;
    assert(fclose(f) == 0);
    assert(strstr(text,"a=q,t=d,f=24;AAAA"));
    assert(strstr(text,"\033[4;6H\033_Ga=T,t=d,f=32,s=43,v=25,i=77,p=1,c=43,r=2,C=1,q=2,m=1;"));
    assert(strstr(text,"a=d,d=I,i=77,q=2"));
    unsigned char decoded[43*25*4]; size_t written = 0; int chunks = 0;
    char *at = strstr(text,"\033_Ga=T"); assert(at);
    while (at) {
        char *payload = strchr(at,';'), *end = payload ? strstr(payload,"\033\\") : NULL;
        assert(end); payload++;
        size_t length = (size_t)(end-payload);
        assert(length <= 4096 && length%4 == 0);
        for (size_t i = 0; i < length; i += 4) {
            unsigned v = (unsigned)digit(payload[i])<<18 | (unsigned)digit(payload[i+1])<<12;
            if (payload[i+2] != '=') v |= (unsigned)digit(payload[i+2])<<6;
            if (payload[i+3] != '=') v |= (unsigned)digit(payload[i+3]);
            assert(written < sizeof(decoded)); decoded[written++] = (unsigned char)(v>>16);
            if (payload[i+2] != '=') { assert(written < sizeof(decoded)); decoded[written++] = (unsigned char)(v>>8); }
            if (payload[i+3] != '=') { assert(written < sizeof(decoded)); decoded[written++] = (unsigned char)v; }
        }
        chunks++; at = strstr(end+2,"\033_Gq=2,m=");
    }
    assert(chunks == 2 && written == sizeof(decoded));
    assert(!memcmp(decoded,image.rgba+43*7*4,sizeof(decoded))); free(text);
    char reply[64]; snprintf(reply,sizeof(reply),"i=%u;OK",GRAPHICS_QUERY_ID);
    assert(terminal_graphics_reply(reply)); assert(!terminal_graphics_reply("i=1;OK"));
    assert(!terminal_graphics_reply("i=1414483200;OKjunk"));
}

static void test_feedback(void) {
    static TerminalFeedbackState state;
    float samples[4096],bands[SPECTRUM_BANDS];
    for (int i=0;i<4096;i++) samples[i]=.5f*(float)sin(6.283185307179586*1000*i/44100);
    terminal_graphics_fft(samples,4096,44100,bands);
    int peak=0; for (int i=1;i<SPECTRUM_BANDS;i++) if (bands[i]>bands[peak]) peak=i;
    double hz=20*pow(1000,(peak+.5)/SPECTRUM_BANDS);
    assert(hz>900 && hz<1100 && bands[peak]>-9 && bands[peak]<-5);
    terminal_graphics_fft(NULL,0,44100,bands);
    for (int i=0;i<SPECTRUM_BANDS;i++) assert(bands[i]==-160);
    VisualNote notes[3]={{.start=7,.end=11,.pitch=60,.gain=1},
        {.start=8,.end=12,.pitch=64,.gain=1}, {.start=9,.end=13,.pitch=67.1,.gain=1}};
    for (int kind=1;kind<=VISUAL_KINDS;kind++) for (int compact=0;compact<2;compact++) {
        VisualOptions options; visual_options(&options,kind,NULL);
        memset(&state,0,sizeof(state)); image.width=compact?160:400; image.height=compact?16:100; image.compact=compact;
        terminal_graphics_feedback(&image,&state,kind,&options,notes,3,samples,4096,10,0,2);
        int pixels=0;
        for (int i=0;i<image.width*image.height;i++) if (image.rgba[i*4+3]) pixels++;
        assert(pixels>0);
    }
    uint32_t c; assert(visual_color("hsl(120,100%,50%)",&c) && c==0x00ff00ffu);
    assert(visual_color("hsla(240,100%,50%,0.5)",&c) && c==0x0000ff80u);
    assert(!visual_color("rgb(nan,0,0)",&c)); assert(!visual_color("#fffgarbage",&c));
    assert(visual_kind("_wordfall",9)==VISUAL_WORDFALL);
    VisualOptions scope_options; visual_options(&scope_options,VISUAL_SCOPE,NULL);
    image.width=200; image.height=12; image.compact=1;
    for (int i=0;i<4096;i++) samples[i]=.01f*((i%512)/256.0f-1);
    terminal_graphics_feedback(&image,NULL,VISUAL_SCOPE,&scope_options,NULL,0,samples,4096,10,0,2);
    unsigned char quiet[200*12*4]; memcpy(quiet,image.rgba,sizeof(quiet));
    for (int i=0;i<4096;i++) samples[i]*=50;
    terminal_graphics_feedback(&image,NULL,VISUAL_SCOPE,&scope_options,NULL,0,samples,4096,10,0,2);
    assert(memcmp(quiet,image.rgba,sizeof(quiet)));
    cJSON *root_options=cJSON_Parse("{\"root\":\"C2\"}"); assert(root_options);
    visual_options(&scope_options,VISUAL_PITCHWHEEL,root_options); cJSON_Delete(root_options);
    assert(fabs(scope_options.root-65.406391325)<1e-6);
    VisualNote overlap[2]={{.start=7,.end=11,.pitch=60,.gain=1},
                          {.start=8,.end=12,.pitch=60,.gain=1}};
    image.width=400; image.height=128; image.compact=0;
    visual_options(&scope_options,VISUAL_PIANOROLL,NULL);
    scope_options.playhead=1;
    terminal_graphics_feedback(&image,NULL,VISUAL_PIANOROLL,&scope_options,overlap,2,NULL,0,10,0,1);
    int amber=0;
    for (int i=0;i<image.width*image.height;i++)
        if (image.rgba[i*4]==255 && image.rgba[i*4+1]==190 && image.rgba[i*4+2]==80) amber++;
    assert(amber>=4); /* Two same-pitch onset markers survive subsequent fills. */
    VisualNote mixed[3]={{.start=9,.end=11,.pitch=36,.gain=1,.label="C2"},
        {.start=9,.end=11,.pitch=0,.gain=1,.label="BD",.sound="bd:0"},
        {.start=9,.end=11,.pitch=0,.gain=1,.label="HH",.sound="hh:0"}};
    scope_options.vertical=1; scope_options.playhead=.5;
    terminal_graphics_feedback(&image,NULL,VISUAL_PIANOROLL,&scope_options,mixed,3,NULL,0,10,0,1);
    int runs=0,longest=0,length=0;
    for (int x=0;x<image.width;x++) {
        const unsigned char *pixel=image.rgba+((size_t)80*image.width+x)*4;
        int bar=pixel[0]==90 && pixel[1]==215 && pixel[2]==245;
        if (bar) { if (!length) runs++; length++; if (length>longest) longest=length; }
        else length=0;
    }
    assert(runs==3 && longest<40); /* Drums share uniform lanes with pitched notes. */
    int guide_pixels=0;
    for (int i=0;i<image.width*14;i++) if (image.rgba[i*4+3]) guide_pixels++;
    assert(guide_pixels>0); /* Vertical pitch guide is above the time plot. */
    mixed[0].gain=.3; image.compact=1; image.width=200; image.height=16;
    terminal_graphics_feedback(&image,NULL,VISUAL_PIANOROLL,&scope_options,mixed,3,NULL,0,10,0,1);
    const unsigned char *bass=image.rgba+((size_t)10*image.width+86)*4;
    assert(bass[0]==90 && bass[1]==215 && bass[2]==245 && bass[3]==255);
    image.compact=0;
}

static void test_spectrum(void) {
    static TerminalFeedbackState state;
    float samples[4096],bands[SPECTRUM_BANDS];
    /* A broadband impulse must not acquire holes from empty logarithmic bins. */
    memset(samples,0,sizeof(samples)); samples[2048]=1;
    terminal_graphics_fft(samples,4096,44100,bands);
    for (int b=0;b<SPECTRUM_BANDS;b++) assert(bands[b]>-70 && bands[b]<-50);

    for (int i=0;i<4096;i++) samples[i]=.5f*(float)sin(6.283185307179586*1000*i/44100);
    VisualOptions options; visual_options(&options,VISUAL_SPECTRUM,NULL);
    /* Continuous tone, irregular 20Hz foreground rendering, full and braille sizes. */
    for (int compact=0;compact<2;compact++) for (int speed=0;speed<3;speed++) {
        const double speeds[]={.125,1,8}; options.speed=speeds[speed];
        double span=4/options.speed, now=0;
        memset(&state,0,sizeof(state));
        image.width=compact?160:768; image.height=compact?16:128; image.compact=compact;
        for (int frame=0;now<span+1;frame++) {
            now+=frame%3==0?.061:.047;
            terminal_graphics_feedback(&image,&state,VISUAL_SPECTRUM,&options,NULL,0,samples,4096,now,0,2);
        }
        int left=terminal_graphics_plot_left(&image);
        int band=(int)(log(1000.0/20)/log(1000)*SPECTRUM_BANDS);
        int y=image.height-1-(int)((band+.5)*image.height/SPECTRUM_BANDS);
        for (int x=left;x<image.width;x++)
            assert(image.rgba[((size_t)y*image.width+x)*4+3]>100);
        /* speed zero freezes both history and its image. */
        static unsigned char frozen[sizeof(image.rgba)];
        memcpy(frozen,image.rgba,(size_t)image.width*image.height*4);
        options.speed=0;
        terminal_graphics_feedback(&image,&state,VISUAL_SPECTRUM,&options,NULL,0,samples,4096,now,0,2);
        assert(!memcmp(frozen,image.rgba,(size_t)image.width*image.height*4));
        int head=state.head;
        terminal_graphics_feedback(&image,&state,VISUAL_SPECTRUM,&options,NULL,0,NULL,0,now+10,0,2);
        assert(state.head==head && !memcmp(frozen,image.rgba,(size_t)image.width*image.height*4));
    }

    /* Seed known levels to test hue changes, silence and explicit-color priority. */
    visual_options(&options,VISUAL_SPECTRUM,NULL); options.speed=0;
    memset(&state,0,sizeof(state)); state.count=2; state.head=2; state.last_time=4;
    state.spectrum_span=4; state.spectrum_end=4;
    state.times[0]=0; state.times[1]=4;
    for (int b=0;b<SPECTRUM_BANDS;b++) state.levels[0][b]=state.levels[1][b]=-80+b;
    image.width=200; image.height=128; image.compact=0;
    terminal_graphics_feedback(&image,&state,VISUAL_SPECTRUM,&options,NULL,0,NULL,0,4,0,2);
    const unsigned char *weak=image.rgba+((size_t)95*image.width+image.width-1)*4;
    const unsigned char *strong=image.rgba+((size_t)31*image.width+image.width-1)*4;
    assert(weak[3]>0 && strong[3]>weak[3] && memcmp(weak,strong,3));
    assert(!image.rgba[((size_t)(image.height-1)*image.width+image.width-1)*4+3]);
    assert(!image.rgba[((size_t)31*image.width+image.width/2)*4+3]); /* Real observation gap. */
    options.has_color=1; options.color=0x12345680u;
    terminal_graphics_feedback(&image,&state,VISUAL_SPECTRUM,&options,NULL,0,NULL,0,4,0,2);
    assert(weak[0]==0x12 && weak[1]==0x34 && weak[2]==0x56 && weak[3]<128);
    options.has_color=0; options.explicit_colors=1; options.active=0x345678ffu;
    terminal_graphics_feedback(&image,&state,VISUAL_SPECTRUM,&options,NULL,0,NULL,0,4,0,2);
    assert(strong[0]==0x34 && strong[1]==0x56 && strong[2]==0x78);
    VisualNote colored={.start=3,.end=5,.flags=VISUAL_COLOR,.color=0xff1234ffu};
    terminal_graphics_feedback(&image,&state,VISUAL_SPECTRUM,&options,&colored,1,NULL,0,4,0,2);
    assert(strong[0]==255 && strong[1]==0x12 && strong[2]==0x34);

    /* No invented pre-start history, and missing audio remains empty. */
    memset(&state,0,sizeof(state)); visual_options(&options,VISUAL_SPECTRUM,NULL);
    terminal_graphics_feedback(&image,&state,VISUAL_SPECTRUM,&options,NULL,0,samples,4096,10,0,2);
    assert(!image.rgba[((size_t)63*image.width+100)*4+3]);
    memset(&state,0,sizeof(state));
    terminal_graphics_feedback(&image,&state,VISUAL_SPECTRUM,&options,NULL,0,NULL,0,10,0,2);
    for (int y=0;y<image.height;y++) for (int x=GRAPHICS_AXIS_WIDTH;x<image.width;x++)
        assert(!image.rgba[((size_t)y*image.width+x)*4+3]);
}

static void test_orientations(void) {
    const int kinds[]={VISUAL_PIANOROLL,VISUAL_PUNCHCARD,VISUAL_WORDFALL};
    const VisualNote note={.start=9,.end=10,.pitch=60,.gain=1,.label="LONG LABEL THAT MUST FIT"};
    for (int k=0;k<3;k++) for (int vertical=0;vertical<2;vertical++)
        for (int reverse=0;reverse<2;reverse++) for (int flip=0;flip<2;flip++) {
            VisualOptions options; visual_options(&options,kinds[k],NULL);
            options.vertical=vertical; options.flip_time=reverse; options.flip_values=flip;
            options.labels=1;
            image.width=400; image.height=128; image.compact=0;
            terminal_graphics_feedback(&image,NULL,kinds[k],&options,&note,1,NULL,0,10,0,1);
            int onset=vertical?14+(int)((reverse?.75:.25)*113):72+(int)((reverse?.75:.25)*327);
            int amber=0;
            for (int p=0;p<(vertical?image.width:image.height);p++) {
                size_t at=(size_t)(vertical?onset:p)*image.width+(vertical?p:onset);
                const unsigned char *pixel=image.rgba+at*4;
                if (pixel[0]==255 && pixel[1]==190 && pixel[2]==80) amber++;
            }
            assert(amber>0);
            if (vertical) {
                int y=14+(int)((reverse?.5:.25)*113)+2;
                for (int x=0;x<image.width;x++) {
                    const unsigned char *pixel=image.rgba+((size_t)y*image.width+x)*4;
                    if (pixel[0]==190 && pixel[1]==210 && pixel[2]==225) assert(x>=187 && x<=212);
                }
            }
        }
}

static void test_palette(void) {
    terminal_graphics_palette_reset();
    assert(terminal_graphics_palette_reply("10;rgb:1111/2222/3333"));
    assert(terminal_graphics_palette_reply("11;rgb:eeee/eeee/eeee"));
    assert(terminal_graphics_palette_reply("4;14;rgb:ab/cd/ef"));
    assert(terminal_graphics_palette_reply("4;10;#123456"));
    assert(terminal_graphics_palette_reply("4;3;rgb:f/8/0"));
    assert(terminal_graphics_palette_reply("4;4;rgb:50/60/70"));
    assert(terminal_graphics_theme(GRAPHICS_TEXT,255)==0x112233ffu);
    assert(terminal_graphics_theme(GRAPHICS_ACTIVE,255)==0xabcdefffu);
    assert(terminal_graphics_theme(GRAPHICS_INACTIVE,191)==0x123456bfu);
    assert(terminal_graphics_theme(GRAPHICS_ONSET,255)==0xff8800ffu);
    assert(terminal_graphics_theme(GRAPHICS_SPECTRUM_LOW,255)==0x506070ffu);
    unsigned generation=terminal_graphics_palette_generation();
    assert(!terminal_graphics_palette_reply("4;16;rgb:ff/ff/ff"));
    assert(!terminal_graphics_palette_reply("10;rgb:fffff/00/00"));
    assert(!terminal_graphics_palette_reply("10;rgb:ff/00/00garbage"));
    assert(!terminal_graphics_palette_reply("10;rgb:ff/00"));
    assert(!terminal_graphics_palette_reply("4;-1;#123456"));
    assert(terminal_graphics_palette_generation()==generation);
    VisualOptions options; visual_options(&options,VISUAL_SCOPE,NULL);
    float samples[16]={0}; image.width=200; image.height=48; image.compact=0;
    terminal_graphics_feedback(&image,NULL,VISUAL_SCOPE,&options,NULL,0,samples,16,0,0,1);
    const unsigned char *trace=image.rgba+((size_t)36*image.width+90)*4;
    assert(trace[0]==0xab && trace[1]==0xcd && trace[2]==0xef);
    cJSON *explicit=cJSON_Parse("{\"active\":\"red\",\"inactive\":\"blue\",\"playheadColor\":\"pink\"}"); assert(explicit);
    visual_options(&options,VISUAL_SCOPE,explicit); cJSON_Delete(explicit);
    assert(options.explicit_colors==7);
    terminal_graphics_feedback(&image,NULL,VISUAL_SCOPE,&options,NULL,0,samples,16,0,0,1);
    assert(trace[0]==255 && trace[1]==0 && trace[2]==0);
    visual_options(&options,VISUAL_PITCHWHEEL,NULL);
    VisualNote red={.start=0,.end=2,.pitch=60,.gain=1,.color=0xff0000ffu,.flags=VISUAL_COLOR};
    terminal_graphics_feedback(&image,NULL,VISUAL_PITCHWHEEL,&options,&red,1,NULL,0,1,0,1);
    int red_pixels=0;
    for (int i=0;i<image.width*image.height;i++)
        if (image.rgba[i*4]==255 && image.rgba[i*4+1]==0 && image.rgba[i*4+2]==0) red_pixels++;
    assert(red_pixels>0); /* Pattern colors remain literal, independent of the palette. */
    assert(terminal_graphics_palette_reply("4;14;rgb:12/34/56"));
    assert(terminal_graphics_palette_generation()>generation);
    terminal_graphics_palette_reset();
    assert(terminal_graphics_theme(GRAPHICS_ACTIVE,255)==0x5ad7f5ffu);
}

int main(int argc, char **argv) {
    if (argc>1 && !strcmp(argv[1],"--feedback-demo")) {
        static TerminalFeedbackState state;
        VisualNote notes[4]={{.start=7,.end=11,.pitch=60,.gain=1,.label="C4"},
            {.start=8,.end=12,.pitch=64,.gain=1,.label="E4"},
            {.start=9,.end=13,.pitch=67,.gain=1,.label="G4"},
            {.start=9.5,.end=10.5,.pitch=60,.gain=1,.label="C4"}};
        float samples[4096];
        for (int i=0;i<4096;i++) samples[i]=.05f*((i%350)/175.0f-1);
        fputs("\033[2J",stdout);
        for (int kind=1;kind<=VISUAL_KINDS;kind++) {
            VisualOptions options; visual_options(&options,kind,NULL);
            memset(&state,0,sizeof(state)); image.width=384; image.height=100;
            if (kind==VISUAL_SPECTRUM) for (int j=0;j<120;j++)
                terminal_graphics_feedback(&image,&state,kind,&options,notes,4,samples,4096,8+j/60.0,0,2);
            terminal_graphics_feedback(&image,&state,kind,&options,notes,4,samples,4096,10,0,2);
            int row=1+(kind-1)/2*8,col=2+(kind-1)%2*42;
            fprintf(stdout,"\033[%d;%dH%s",row,col,visual_name(kind));
            if (!terminal_graphics_send(stdout,77+(unsigned)kind,&image,row,col-1,38,6,0,100)) return 1;
        }
        fputs("\033[34;1HPress Enter to close.",stdout); fflush(stdout); (void)getchar();
        for (int kind=1;kind<=VISUAL_KINDS;kind++) terminal_graphics_delete(stdout,77+(unsigned)kind);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1],"--demo")) {
        fputs("\033[2J\033[1;1HTompouce Kitty graphics preview: scope + piano roll",stdout);
        float saw[1024]; for (int i = 0; i < 1024; i++) saw[i] = .01f*((i%350)/175.0f-1);
        image.width = 720; image.height = 100;
        terminal_graphics_scope(&image,saw,1024);
        if (!terminal_graphics_send(stdout,77,&image,2,2,72,5,0,100)) return 1;
        GraphicsNote notes[] = {{6.1,9.8,60},{6.8,8,64},{7.4,8.8,67},{9,10.5,72},
                                {7,9.5,60},{8,10.5,60}};
        terminal_graphics_roll(&image,notes,6,10);
        if (!terminal_graphics_send(stdout,78,&image,9,2,72,5,0,100)) return 1;
        fputs("\033[16;1HPress Enter to close.",stdout); fflush(stdout); (void)getchar();
        terminal_graphics_delete(stdout,77); terminal_graphics_delete(stdout,78);
        return 0;
    }
    test_note_guides(); test_rasters(); test_overlapping_notes(); test_transport(); test_feedback(); test_spectrum(); test_orientations(); test_palette(); test_scope_web_geometry();
    puts("Kitty waveform, piano roll, clipped chunked transport and capability checks ok");
    return 0;
}
