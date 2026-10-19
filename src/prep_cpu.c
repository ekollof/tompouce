#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "prep_pool.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sched.h>
#endif

enum { CGROUP_PATH=4096, CGROUP_LINE=16384 };

static unsigned affinity_cpus(void) {
#ifdef __linux__
    cpu_set_t set;
    if (!sched_getaffinity(0,sizeof(set),&set)) {
        int count=CPU_COUNT(&set);
        if (count>0) return (unsigned)count;
    }
#endif
    long count=sysconf(_SC_NPROCESSORS_ONLN);
    return count>0 && count<=INT_MAX?(unsigned)count:1;
}

static int positive_integer(const char *text, uint64_t *value) {
    if (!text || !*text) return 0;
    for (const char *p=text;*p;p++) if (*p<'0' || *p>'9') return 0;
    char *end=NULL;
    errno=0;
    unsigned long long number=strtoull(text,&end,10);
    if (errno || !end || *end || !number) return 0;
    *value=number;
    return 1;
}

static unsigned quota_value(const char *limit, const char *period) {
    uint64_t value,interval;
    if (!positive_integer(limit,&value) || !positive_integer(period,&interval)) return 0;
    uint64_t cpus=value/interval+(value%interval!=0);
    return cpus>INT_MAX?INT_MAX:(unsigned)cpus;
}

/* mountinfo escapes spaces, tabs, newlines and backslashes as octal. */
static int decode_path(const char *encoded, char out[CGROUP_PATH]) {
    size_t used=0;
    for (size_t i=0;encoded[i];i++) {
        unsigned char c=(unsigned char)encoded[i];
        if (c=='\\') {
            if (!encoded[i+1] || !encoded[i+2] || !encoded[i+3]) return 0;
            unsigned value=0;
            for (int j=1;j<=3;j++) {
                if (encoded[i+j]<'0' || encoded[i+j]>'7') return 0;
                value=value*8+(unsigned)(encoded[i+j]-'0');
            }
            if (!value || value>255) return 0;
            c=(unsigned char)value; i+=3;
        }
        if (used+1>=CGROUP_PATH) return 0;
        out[used++]=(char)c;
    }
    out[used]=0;
    return used && out[0]=='/';
}

static int path_beneath(const char *path, const char *root) {
    size_t length=strlen(root);
    return !strcmp(root,"/") || (!strncmp(path,root,length) &&
        (!path[length] || path[length]=='/'));
}

static int safe_path(const char *path) {
    if (*path!='/') return 0;
    for (const char *p=path;*p;) {
        if (*p=='/') { p++; continue; }
        size_t length=strcspn(p,"/");
        if ((length==1 && p[0]=='.') || (length==2 && p[0]=='.' && p[1]=='.')) return 0;
        p+=length;
    }
    return 1;
}

static int has_controller(const char *list, const char *name) {
    size_t length=strlen(name);
    for (const char *p=list;*p;) {
        size_t n=strcspn(p,",");
        if (n==length && !strncmp(p,name,n)) return 1;
        p+=n; if (*p) p++;
    }
    return 0;
}

static unsigned read_quota(const char *directory, int unified) {
    char path[CGROUP_PATH],limit[64],period[64];
    int n=snprintf(path,sizeof(path),"%s/%s",directory,
        unified?"cpu.max":"cpu.cfs_quota_us");
    if (n<0 || (size_t)n>=sizeof(path)) return 0;
    FILE *file=fopen(path,"r");
    if (!file) return 0;
    int fields=unified?fscanf(file,"%63s %63s",limit,period):fscanf(file,"%63s",limit);
    fclose(file);
    if (unified) return fields==2?quota_value(limit,period):0;
    if (fields!=1) return 0;
    n=snprintf(path,sizeof(path),"%s/cpu.cfs_period_us",directory);
    if (n<0 || (size_t)n>=sizeof(path)) return 0;
    file=fopen(path,"r");
    if (!file) return 0;
    fields=fscanf(file,"%63s",period); fclose(file);
    return fields==1?quota_value(limit,period):0;
}

typedef unsigned (*QuotaReader)(const char *,int);

/* Inspect every visible ancestor, including alternate mounts of a subtree.
   Limits above a cgroup namespace's visible root cannot be read from here. */
static unsigned quota_for_mount(char *line, const char *membership, int unified,
                                QuotaReader reader) {
    char *separator=strstr(line," - ");
    if (!separator) return 0;
    *separator=0;
    char *save=NULL;
    char *filesystem=strtok_r(separator+3," \n",&save);
    char *source=strtok_r(NULL," \n",&save);
    char *options=strtok_r(NULL," \n",&save);
    if (!filesystem || !source || !options ||
        strcmp(filesystem,unified?"cgroup2":"cgroup") ||
        (!unified && !has_controller(options,"cpu"))) return 0;
    char *fields[5]; save=NULL;
    for (int i=0;i<5;i++) {
        fields[i]=strtok_r(i?NULL:line," ",&save);
        if (!fields[i]) return 0;
    }
    char root[CGROUP_PATH],mount[CGROUP_PATH],path[CGROUP_PATH];
    if (!decode_path(fields[3],root) || !decode_path(fields[4],mount) ||
        !safe_path(root) || !safe_path(mount) || !safe_path(membership) ||
        !path_beneath(membership,root)) return 0;
    const char *relative=membership+(!strcmp(root,"/")?0:strlen(root));
    int n=snprintf(path,sizeof(path),"%s%s",!strcmp(mount,"/")?"":mount,relative);
    if (n<0 || (size_t)n>=sizeof(path)) return 0;
    if (!*path) { path[0]='/'; path[1]=0; }
    unsigned quota=0;
    for (;;) {
        unsigned value=reader(path,unified);
        if (value && (!quota || value<quota)) quota=value;
        if (!strcmp(path,mount)) break;
        char *slash=strrchr(path,'/');
        if (!slash || !path_beneath(path,mount)) break;
        if (slash==path) path[1]=0; else *slash=0;
    }
    return quota;
}

static unsigned cgroup_quota_files(FILE *membership, FILE *mounts, QuotaReader reader) {
    char line[CGROUP_LINE],v1[CGROUP_PATH]={0},v2[CGROUP_PATH]={0};
    while (fgets(line,sizeof(line),membership)) {
        if (!strchr(line,'\n') && !feof(membership)) return 0;
        char *first=strchr(line,':');
        char *second=first?strchr(first+1,':'):NULL;
        if (!second) continue;
        *first=0; *second=0;
        char *path=second+1;
        path[strcspn(path,"\n")]=0;
        if (!safe_path(path) || strlen(path)>=CGROUP_PATH) continue;
        if (!strcmp(line,"0") && !first[1]) memcpy(v2,path,strlen(path)+1);
        else if (has_controller(first+1,"cpu")) memcpy(v1,path,strlen(path)+1);
    }
    unsigned quota=0;
    while (fgets(line,sizeof(line),mounts)) {
        if (!strchr(line,'\n') && !feof(mounts)) return 0;
        int unified=strstr(line," - cgroup2 ")!=NULL;
        const char *path=unified?v2:v1;
        if (!*path) continue;
        unsigned value=quota_for_mount(line,path,unified,reader);
        if (value && (!quota || value<quota)) quota=value;
    }
    return quota;
}

int prep_pool_cpu_budget(void) {
    unsigned budget=affinity_cpus(),quota=0;
    FILE *membership=fopen("/proc/self/cgroup","r");
    FILE *mounts=fopen("/proc/self/mountinfo","r");
    if (membership && mounts) quota=cgroup_quota_files(membership,mounts,read_quota);
    if (membership) fclose(membership);
    if (mounts) fclose(mounts);
    if (quota && quota<budget) budget=quota;
    return (int)budget;
}
