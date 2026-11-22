/* Minimal LV2 MIDI instrument used by the confined-worker tests. A note-on
   starts a deterministic saw at the note frequency, a note-off stops it, and
   any CC scales the amplitude. No external runtime or UI is required. */
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const LV2_Atom_Sequence *events;
    float *out;
    double rate;
    double phase, frequency, amplitude, cc;
    int active;
} TestMidiSynth;

static LV2_Handle instantiate(const LV2_Descriptor *descriptor, double rate,
                              const char *bundle_path,
                              const LV2_Feature *const *features) {
    (void)descriptor; (void)bundle_path; (void)features;
    TestMidiSynth *synth=calloc(1,sizeof(TestMidiSynth));
    if (synth) { synth->rate=rate; synth->cc=1; }
    return synth;
}

static void connect_port(LV2_Handle handle, uint32_t port, void *data) {
    TestMidiSynth *synth=handle;
    if (!synth) return;
    if (port==0) synth->events=data;
    else if (port==1) synth->out=data;
}

static void generate(TestMidiSynth *synth, uint32_t begin, uint32_t end) {
    double phase=synth->phase;
    double scale=synth->active?synth->amplitude*synth->cc:0;
    for (uint32_t i=begin;i<end;i++) {
        double sample=(2*phase-1)*scale;
        if (synth->out) synth->out[i*2]=synth->out[i*2+1]=(float)sample;
        phase+=synth->frequency/synth->rate;
        if (phase>=1) phase-=1;
        else if (phase<0) phase+=1;
    }
    synth->phase=phase;
}

static void apply_message(TestMidiSynth *synth, const uint8_t *data, uint32_t size) {
    if (!synth || !data || size<1) return;
    unsigned status=data[0]&0xf0;
    if (status==0x90 && size>=3) {
        unsigned velocity=data[2];
        if (velocity==0) { synth->active=0; return; }
        int note=data[1]&0x7f;
        synth->frequency=440.0*pow(2.0,(note-69)/12.0);
        if (!(synth->frequency>=20 && synth->frequency<=20000)) synth->frequency=440;
        synth->amplitude=(double)velocity/127.0;
        synth->phase=0;
        synth->active=1;
    } else if (status==0x80 && size>=2) {
        synth->active=0;
    } else if (status==0xb0 && size>=3) {
        synth->cc=(double)data[2]/127.0;
    }
}

static void run(LV2_Handle handle, uint32_t sample_count) {
    TestMidiSynth *synth=handle;
    if (!synth || !synth->out) return;
    uint32_t at=0;
    if (synth->events) {
        LV2_ATOM_SEQUENCE_FOREACH(synth->events,event) {
            uint32_t offset=(uint32_t)event->time.frames;
            if (offset>sample_count) offset=sample_count;
            if (offset>at) { generate(synth,at,offset); at=offset; }
            const uint8_t *data=(const uint8_t *)(event+1);
            apply_message(synth,data,event->body.size);
        }
    }
    if (at<sample_count) generate(synth,at,sample_count);
}

static void cleanup(LV2_Handle handle) { free(handle); }

static const LV2_Descriptor descriptor={
    "urn:tompouce:test:midi-synth",
    instantiate,
    connect_port,
    NULL,
    run,
    NULL,
    cleanup,
    NULL
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
    return index==0?&descriptor:NULL;
}
