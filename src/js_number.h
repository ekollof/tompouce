#ifndef TOMPOUCE_JS_NUMBER_H
#define TOMPOUCE_JS_NUMBER_H
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "cutils.h"
#include "libunicode.h"
/* QuickJS's standalone formatter/parser uses caller-owned temporary storage.
   Its upstream header has no include guard; native test translation units
   include both audio implementations. No JS context is used here. */
#include "dtoa.h"

static inline const char *js_number_spaces(const char *text, const char *end) {
    const unsigned char *p=(const unsigned char *)text;
    const unsigned char *limit=(const unsigned char *)end;
    while (p<limit) {
        uint32_t code; int bytes;
        if (*p<128) {code=*p;bytes=1;}
        else if (*p>=0xc2 && *p<=0xdf && limit-p>=2 && (p[1]&0xc0)==0x80) {
            code=((*p&31u)<<6)|(p[1]&63u); bytes=2;
        } else if (*p>=0xe0 && *p<=0xef && limit-p>=3 &&
                   (p[1]&0xc0)==0x80 && (p[2]&0xc0)==0x80) {
            code=((*p&15u)<<12)|((p[1]&63u)<<6)|(p[2]&63u); bytes=3;
            if (code<0x800 || (code>=0xd800 && code<=0xdfff)) break;
        } else break;
        if (!lre_is_space(code)) break;
        p+=bytes;
    }
    return (const char *)p;
}

static inline double js_number_parse(const char *text) {
    const char *limit=text+strlen(text),*end;
    text=js_number_spaces(text,limit);
    if (text==limit) return 0;
    const char *unsigned_text=(*text=='+' || *text=='-')?text+1:text;
    int prefix=unsigned_text[0]=='0' &&
        (unsigned_text[1]=='x' || unsigned_text[1]=='X' ||
         unsigned_text[1]=='o' || unsigned_text[1]=='O' ||
         unsigned_text[1]=='b' || unsigned_text[1]=='B');
    if (prefix && unsigned_text!=text) return NAN;
    JSATODTempMem temporary;
    double number=js_atod(text,&end,0,JS_ATOD_ACCEPT_BIN_OCT|
        (prefix?JS_ATOD_INT_ONLY:0),&temporary);
    if (end==text || js_number_spaces(end,limit)!=limit) return NAN;
    return number;
}
#endif
