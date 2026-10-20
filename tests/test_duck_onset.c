#include <assert.h>
#include <cjson/cJSON.h>
#include <inttypes.h>
#include "../src/duck_onset.c"

static void check(const char *suffix,double base,double attack) {
    const DuckOnsetText *text=duck_onset_prepare(suffix); assert(text);
    double minimum,end; duck_onset_times(text,base,attack,&minimum,&end);
    size_t capacity=strlen(suffix)+100;
    char *joined=malloc(capacity); assert(joined);
    char first[32],last[32]; JSDTOATempMem temporary;
    js_dtoa(first,base,10,0,JS_DTOA_FORMAT_FREE,&temporary);
    js_dtoa(last,attack,10,0,JS_DTOA_FORMAT_FREE,&temporary);
    assert(snprintf(joined,capacity,"%s%s",first,suffix)>0);
    double expected=js_number_parse(joined);
    if (!(isnan(expected)?isnan(minimum):minimum==expected))
        fprintf(stderr,"minimum %.17g != %.17g, base %.17g suffix %.30s\n",minimum,expected,base,suffix);
    assert(isnan(expected)?isnan(minimum):minimum==expected);
    assert(snprintf(joined,capacity,"%s%s%s",first,suffix,last)>0);
    expected=js_number_parse(joined);
    assert(isnan(expected)?isnan(end):end==expected);
    free(joined);
    assert(duck_onset_prepare(suffix)==text);
}

static void reference(void) {
    char *input=malloc(8*1024*1024+1); assert(input);
    size_t length=fread(input,1,8*1024*1024,stdin); assert(!ferror(stdin) && feof(stdin));
    input[length]=0; cJSON *cases=cJSON_Parse(input),*results=cJSON_CreateArray(); free(input);
    assert(cases && results);
    const cJSON *item;
    cJSON_ArrayForEach(item,cases) {
        const char *onset=cJSON_GetObjectItemCaseSensitive(item,"onset")->valuestring;
        double base=cJSON_GetObjectItemCaseSensitive(item,"base")->valuedouble;
        double attack=cJSON_GetObjectItemCaseSensitive(item,"attack")->valuedouble;
        const DuckOnsetText *text=duck_onset_prepare(onset); assert(text);
        double values[2]; duck_onset_times(text,base,attack,&values[0],&values[1]);
        cJSON *result=cJSON_CreateArray(); assert(result);
        for (int i=0;i<2;i++) {
            char formatted[32]; uint64_t encoded; memcpy(&encoded,&values[i],sizeof(encoded));
            if (isnan(values[i])) snprintf(formatted,sizeof(formatted),"NaN");
            else snprintf(formatted,sizeof(formatted),"%016" PRIx64,encoded);
            cJSON_AddItemToArray(result,cJSON_CreateString(formatted));
        }
        cJSON_AddItemToArray(results,result);
    }
    char *json=cJSON_PrintUnformatted(results); assert(json); puts(json); free(json);
    cJSON_Delete(cases); cJSON_Delete(results);
}

int main(int argc,char **argv) {
    if (argc>1 && !strcmp(argv[1],"--reference")) {reference();return 0;}
    char *suffix=malloc(100100); assert(suffix);
    const double bases[]={0,.3,.4005442176870748,1,1e-7,1e21};
    const double attacks[]={.002,.1,1,1e21};
    const char *shorts[]={""," ","e0","e-1","1 ",".25","x10","b1","o7","x","b","o","bad","\xc2\xa0"};
    for (size_t i=0;i<sizeof(shorts)/sizeof(shorts[0]);i++)
        for (size_t b=0;b<sizeof(bases)/sizeof(bases[0]);b++)
            for (size_t a=0;a<sizeof(attacks)/sizeof(attacks[0]);a++) check(shorts[i],bases[b],attacks[a]);
    const char *prefixes[]={"","1","e","e-","x","b","o"};
    const char *tails[]={"","1","e-1"," ","\xe2\x80\x83",".1","bad"};
    for (size_t p=0;p<sizeof(prefixes)/sizeof(prefixes[0]);p++)
        for (size_t t=0;t<sizeof(tails)/sizeof(tails[0]);t++) {
            size_t length=strlen(prefixes[p]); memcpy(suffix,prefixes[p],length);
            memset(suffix+length,'0',10000); length+=10000;
            memcpy(suffix+length,tails[t],strlen(tails[t])+1);
            for (size_t b=0;b<sizeof(bases)/sizeof(bases[0]);b++)
                for (size_t a=0;a<sizeof(attacks)/sizeof(attacks[0]);a++) check(suffix,bases[b],attacks[a]);
        }
    /* Retain every prepared reference while filling the bounded cache. */
    const DuckOnsetText *retained=duck_onset_prepare("e0"); assert(retained);
    for (unsigned i=128;used<CACHE && i<4096;i++) {
        memset(suffix,'0',i); suffix[i]=0; assert(duck_onset_prepare(suffix));
    }
    assert(used==CACHE && duck_onset_prepare("e0")==retained);
    memset(suffix,'0',100000); suffix[100000]=0;
    assert(!duck_onset_prepare(suffix));
    assert(duck_onset_prepare("invalid") && used==CACHE);
    double minimum,end; duck_onset_times(retained,.3,1,&minimum,&end);
    assert(minimum==.3 && end==3);
    free(suffix);
    puts("Bounded onset summaries preserve numeric concatenation, long zero runs, radix literals and cache references");
    return 0;
}
