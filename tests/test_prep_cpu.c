/* Read-only hierarchy fixtures; no cgroup settings or host quotas are changed. */
#include "../src/prep_cpu.c"
#include <assert.h>

static const char *limited;
static unsigned limit_value;
static int expected_unified,visited;
static const char *other_limit;

static unsigned fixture_quota(const char *path, int unified) {
    assert(unified==expected_unified);
    visited++;
    if (other_limit && !strcmp(path,other_limit)) return 1;
    return !strcmp(path,limited)?limit_value:0;
}

static void hierarchy(const char *membership, const char *mounts,
                      const char *limit, int unified, unsigned expected) {
    FILE *groups=tmpfile(),*mountinfo=tmpfile();
    assert(groups && mountinfo);
    assert(fputs(membership,groups)>=0 && fputs(mounts,mountinfo)>=0);
    rewind(groups); rewind(mountinfo);
    limited=limit; limit_value=2; expected_unified=unified; visited=0;
    assert(cgroup_quota_files(groups,mountinfo,fixture_quota)==expected);
    assert(!fclose(groups) && !fclose(mountinfo));
}

int main(void) {
    const char *v2="31 20 0:28 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n";
    hierarchy("0::/user.slice/app.scope\n",v2,"/sys/fs/cgroup/user.slice",1,2);
    assert(visited==3);
    hierarchy("0::/user.slice/app.scope\n",v2,"/sys/fs/cgroup/user.slice/app.scope",1,2);
    hierarchy("0::/user.slice/app.scope\n",v2,"/sys/fs/cgroup",1,2);
    other_limit="/sys/fs/cgroup/user.slice";
    hierarchy("0::/user.slice/app.scope\n",v2,"/sys/fs/cgroup/user.slice/app.scope",1,1);
    other_limit=NULL;
    hierarchy("0::/user.slice/app.scope\n",v2,"/other/scope",1,0);
    hierarchy("7:cpuacct,cpu:/batch/job\n",
        "32 20 0:29 / /custom/cpu rw - cgroup cgroup rw,cpu,cpuacct\n",
        "/custom/cpu/batch",0,2);
    hierarchy("7:cpuacct:/batch/job\n",
        "32 20 0:29 / /custom/cpu rw - cgroup cgroup rw,cpu,cpuacct\n",
        "/custom/cpu/batch",0,0);
    hierarchy("0::/tenant/job\n",
        "31 20 0:28 /tenant /mount\\040point rw - cgroup2 cgroup rw\n",
        "/mount point",1,2);
    hierarchy("0::/tenant-other/job\n",
        "31 20 0:28 /tenant /mount rw - cgroup2 cgroup rw\n",
        "/mount",1,0);
    hierarchy("0::/../outside\n",v2,"/sys/fs/cgroup",1,0);
    hierarchy("0::/\n",v2,"/sys/fs/cgroup",1,2);
    hierarchy("0::/job\n",
        "31 20 0:28 / /tenant-view rw - cgroup2 cgroup rw\n",
        "/tenant-view/job",1,2);
    assert(quota_value("150000","100000")==2);
    assert(quota_value("1","100000")==1);
    assert(quota_value("max","100000")==0);
    assert(quota_value("-1","100000")==0);
    assert(quota_value("0","100000")==0);
    assert(quota_value("100","0")==0);
    assert(quota_value("18446744073709551615","18446744073709551615")==1);
    assert(quota_value("18446744073709551616","100000")==0);
    assert(quota_value("18446744073709551615","1")==INT_MAX);
    assert(prep_pool_cpu_budget()>=1);
    puts("CPU budget resolves nested v1/v2 membership, mount roots, escaped paths and ancestor quotas");
    return 0;
}
