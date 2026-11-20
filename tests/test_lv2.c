/* Confined LV2 metadata discovery; no plugin is loaded or executed. */
#include "lv2_host.h"
#include <assert.h>
#include <math.h>
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
    int audio_plugins=0, total_controls=0;
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
        Lv2ControlPort controls[LV2_CONTROLS_MAX];
        int ncontrols=lv2_catalog_controls(catalog,i,controls,LV2_CONTROLS_MAX);
        assert(ncontrols>=0 && ncontrols<=LV2_CONTROLS_MAX);
        total_controls+=ncontrols;
        for (int c=0;c<ncontrols;c++) {
            assert(controls[c].index<256 && controls[c].name[0]);
            assert(isfinite(controls[c].min) && isfinite(controls[c].max));
            assert(isfinite(controls[c].value) && controls[c].max>=controls[c].min);
        }
        assert(!lv2_catalog_entry(catalog,count,uri,name,library,&audio,&midi));
    }
    assert(audio_plugins>0);
    lv2_catalog_free(catalog);
    printf("LV2 discovery ok: %d plugins (%d with audio ports, %d control ports)\n",
           count,audio_plugins,total_controls);
    return 0;
}
