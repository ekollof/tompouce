/* Confined LV2 metadata discovery; no plugin is loaded or executed. */
#include "lv2_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char error[256]="";
    Lv2Catalog *catalog=lv2_catalog_scan(error,sizeof(error));
    if (!catalog) {
        printf("LV2 discovery unavailable: %s\n",error[0]?error:"unknown");
        return 0;
    }
    int count=lv2_catalog_count(catalog);
    assert(count>0 && count<=256);
    int audio_plugins=0;
    for (int i=0;i<count;i++) {
        char uri[LV2_URI_MAX], name[LV2_NAME_MAX], library[LV2_URI_MAX];
        int audio=0, midi=0;
        assert(lv2_catalog_entry(catalog,i,uri,name,library,&audio,&midi));
        assert(*uri && *name && *library);
        assert(strstr(uri,"..")==NULL);
        assert(strstr(library,"://")==NULL && strstr(library,"..")==NULL);
        size_t length=strlen(library);
        assert(length>3 && !strcmp(library+length-3,".so"));
        assert(audio || midi);
        if (audio) audio_plugins++;
        assert(!lv2_catalog_entry(catalog,count,uri,name,library,&audio,&midi));
    }
    assert(audio_plugins>0);
    lv2_catalog_free(catalog);
    printf("LV2 discovery ok: %d plugins (%d with audio ports)\n",count,audio_plugins);
    return 0;
}
