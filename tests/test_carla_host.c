/* Managed Carla lifecycle and project-root bounds without a running desktop.
   The owned fake host proves process startup/stop and path handling; the Python
   integration test covers the real Carla project and PipeWire MIDI link. */
#include "carla_host.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *text) {
    int fd=open(path,O_CREAT|O_TRUNC|O_WRONLY,0600);
    assert(fd>=0);
    size_t length=strlen(text);
    assert(write(fd,text,length)==(ssize_t)length);
    close(fd);
}

int main(void) {
    char root[]="/tmp/tompouce-carla-test-XXXXXX";
    assert(mkdtemp(root));
    char project[512];
    snprintf(project,sizeof(project),"%s/fake.carxp",root);
    write_file(project,"<CARLA-PROJECT><Plugin><Info><Name>fake-node</Name>"
        "</Info></Plugin></CARLA-PROJECT>\n");
    assert(!setenv("TOMPOUCE_CARLA_ROOT",root,1));
    const char *fixture=getenv("TOMPOUCE_CARLA_TEST_HOST");
    assert(fixture && *fixture);
    assert(!setenv("TOMPOUCE_CARLA",fixture,1));

    char error[256]="";
    int slot=carla_acquire("fake.carxp",NULL,error,sizeof(error));
    if (slot<0) printf("carla acquire failed: %s\n",error);
    assert(slot>=0);
    for (int i=0;i<100 && carla_active_count()<1;i++) usleep(10000);
    assert(carla_active_count()==1);
    assert(carla_acquire("fake.carxp","fake-node",error,sizeof(error))==slot);
    assert(carla_ready_count()==0); /* no PipeWire/MIDI link in this fixture */
    carla_stop_all();
    assert(carla_active_count()==0);

    assert(carla_acquire("../fake.carxp",NULL,error,sizeof(error))<0);
    assert(carla_acquire(project,"fake-node",error,sizeof(error))<0);
    assert(carla_acquire("missing.carxp",NULL,error,sizeof(error))<0);
    assert(carla_acquire("fake.carxp","bad:node",error,sizeof(error))<0);
    carla_shutdown();
    assert(carla_active_count()==0);
    unlink(project);
    rmdir(root);
    puts("managed Carla project bounds and process lifecycle ok");
    return 0;
}
