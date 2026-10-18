#ifndef TOMPOUCE_JS_NUMBER_H
#define TOMPOUCE_JS_NUMBER_H
#include <stdint.h>
/* QuickJS's standalone formatter/parser uses caller-owned temporary storage.
   Its upstream header has no include guard; native test translation units
   include both audio implementations. No JS context is used here. */
#include "dtoa.h"
#endif
