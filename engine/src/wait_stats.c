#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/shim/lantern_diag_c.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/syscall.h>
#include <dlfcn.h>
#include <dirent.h>
#include <ucontext.h>
#include <signal.h>
#include <sys/uio.h>
#include <sys/random.h>
#include <sys/resource.h>
#endif
#include <sys/time.h>
#include <x86intrin.h>
#ifndef _WIN32
#include <sys/random.h>
#include <sys/resource.h>
#endif
#ifdef _WIN32
#include <windows.h>
static void sample_start(void) {}
static void sample_report(void) {}
static uintptr_t thread_stack_hi(void) {
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    return (uintptr_t)hi;
}
static int thread_id(void) { return (int)GetCurrentThreadId(); }
static void thread_name(char *out, size_t size) {
    out[0] = 0;
    PWSTR wide = NULL;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &wide)) && wide) {
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)size, NULL, NULL);
        out[size - 1] = 0;
        LocalFree(wide);
    }
}
#else
static void sample_start(void);
static void sample_report(void);
static uintptr_t thread_stack_hi(void) {
    pthread_attr_t attr; void *base=NULL; size_t size=0;
    if (!pthread_getattr_np(pthread_self(),&attr)) { pthread_attr_getstack(&attr,&base,&size); pthread_attr_destroy(&attr); }
    return (uintptr_t)base+size;
}
static int thread_id(void) { return (int)syscall(SYS_gettid); }
static void thread_name(char *out, size_t size) { pthread_getname_np(pthread_self(),out,size); }
#endif

typedef struct { _Atomic uint64_t key; uint64_t site, site2, site3; int tid, kind; char name[16]; _Atomic uint64_t count, ns; } WaitSite;
static WaitSite wait_sites[512];
static _Thread_local int wait_tid;
void runtime_guest_call_sites(uint64_t out[3]);
static void guest_call_sites(uint64_t out[3]) {
    const uintptr_t text_lo=0x800000000ull, text_hi=0x800000000ull+0x50d96dcull;
    static _Thread_local uintptr_t stack_hi;
    if (!stack_hi) stack_hi=thread_stack_hi();
    const uintptr_t *sp=(const uintptr_t *)__builtin_frame_address(0);
    int found=0;
    out[0]=out[1]=out[2]=0;
    for (int i=0;i<400 && found<3 && (uintptr_t)(sp+i+1)<=stack_hi;++i) if (sp[i]>=text_lo && sp[i]<text_hi) out[found++]=sp[i]-text_lo;
}
void runtime_wait_note(int kind, uint64_t ns) {
    if (!lantern_switch_c(LANTERN_SW_ID_frame_stats)) return;
    if (!wait_tid) wait_tid=thread_id();
    uint64_t sites[3];
    guest_call_sites(sites);
    const uint64_t key=((sites[0]<<20)^(sites[1]*0x9E3779B1ull)^(sites[2]<<7)^((uint64_t)wait_tid<<4)^(uint64_t)kind)|1;
    for (uint64_t i=0,slot=(key*0x9E3779B97F4A7C15ull)>>55;i<512;++i) {
        WaitSite *w=&wait_sites[(slot+i)%512];
        uint64_t expected=0;
        if (atomic_load(&w->key)==key || atomic_compare_exchange_strong(&w->key,&expected,key)) {
            if (!w->tid) { w->site=sites[0]; w->site2=sites[1]; w->site3=sites[2]; w->kind=kind; thread_name(w->name,sizeof(w->name)); w->tid=wait_tid; }
            atomic_fetch_add(&w->count,1); atomic_fetch_add(&w->ns,ns);
            return;
        }
    }
}
void runtime_wait_report(double frames) {
    sample_start();
    sample_report();
    static const char *kinds[]={"cond","mutex","sema","sleep"};
    struct { WaitSite *w; uint64_t ns, count; } rows[512]; int n=0;
    for (int i=0;i<512;++i) {
        uint64_t ns=atomic_exchange(&wait_sites[i].ns,0), count=atomic_exchange(&wait_sites[i].count,0);
        if (count) { rows[n].w=&wait_sites[i]; rows[n].ns=ns; rows[n].count=count; ++n; }
    }
    if (!n || frames<=0) return;
    static FILE *dump; static int dump_checked;
    if (!dump_checked) { const char *path=getenv("BB_WAIT_LOG"); if (path && *path) dump=fopen(path,"w"); dump_checked=1; }
    if (dump) {
        for (int i=0;i<n;++i)
            fprintf(dump,"%s %d %s %#lx %#lx %#lx %.2f %.3f\n", rows[i].w->name, rows[i].w->tid, kinds[rows[i].w->kind&3],
                    (unsigned long)rows[i].w->site, (unsigned long)rows[i].w->site2, (unsigned long)rows[i].w->site3,
                    rows[i].count/frames, rows[i].ns/(frames*1e6));
        fprintf(dump,"--\n"); fflush(dump);
    }
    for (int i=1;i<n;++i) for (int j=i;j>0 && rows[j].ns>rows[j-1].ns;--j) { __typeof__(rows[0]) t=rows[j]; rows[j]=rows[j-1]; rows[j-1]=t; }
    printf("Guest waits per frame (thread kind +site):");
    for (int i=0;i<n && i<12;++i) if (rows[i].ns/rows[i].count<5000000)
        printf("%s %s %s +%#lx<+%#lx<+%#lx %.1fx %.2f ms", i ? ";" : "", rows[i].w->name, kinds[rows[i].w->kind&3],
               (unsigned long)rows[i].w->site, (unsigned long)rows[i].w->site2, (unsigned long)rows[i].w->site3,
               rows[i].count/frames, rows[i].ns/(frames*1e6));
    printf("\n");
}

void runtime_guest_call_sites(uint64_t out[3]) { guest_call_sites(out); }

#ifndef _WIN32
static _Atomic uint64_t sample_keys[1024], sample_counts[1024];
static uint64_t sample_rips[1024], sample_callers[1024];
static _Atomic uint64_t samples_total;
static void sample_handler(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    const ucontext_t *uc=(const ucontext_t *)context;
    uint64_t rip=(uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    const uint64_t text_lo=0x800000000ull, text_hi=0x800000000ull+0x50d96dcull;
    uint64_t caller=0;
    if (rip<text_lo || rip>=text_hi) {
        uint64_t stack[256]={0};
        struct iovec local={stack,sizeof(stack)}, remote={(void *)uc->uc_mcontext.gregs[REG_RSP],sizeof(stack)};
        const ssize_t got=process_vm_readv(getpid(),&local,1,&remote,1,0);
        for (ssize_t i=0;i<got/8;++i) if (stack[i]>=text_lo && stack[i]<text_hi) { caller=stack[i]-text_lo; break; }
    }
    const uint64_t key=(rip*0x9E3779B97F4A7C15ull)^caller^1;
    atomic_fetch_add(&samples_total,1);
    for (uint64_t i=0,slot=(key*0x9E3779B97F4A7C15ull)>>54;i<1024;++i) {
        _Atomic uint64_t *k=&sample_keys[(slot+i)%1024];
        uint64_t expected=0;
        if (atomic_load(k)==key || atomic_compare_exchange_strong(k,&expected,key)) {
            sample_rips[(slot+i)%1024]=rip; sample_callers[(slot+i)%1024]=caller;
            atomic_fetch_add(&sample_counts[(slot+i)%1024],1);
            return;
        }
    }
}
static void *sampler_main(void *arg) {
    const char *name=(const char *)arg;
    pid_t tid=0;
    while (!tid) {
        DIR *dir=opendir("/proc/self/task");
        struct dirent *e;
        while (dir && (e=readdir(dir))) {
            char path[300], comm[32]={0};
            snprintf(path,sizeof(path),"/proc/self/task/%s/comm",e->d_name);
            FILE *f=fopen(path,"r");
            if (!f) continue;
            if (fgets(comm,sizeof(comm),f) && !strncmp(comm,name,strlen(name))) tid=(pid_t)atoi(e->d_name);
            fclose(f);
        }
        if (dir) closedir(dir);
        if (!tid) sleep(1);
    }
    printf("Runtime: sampling thread %s (%d)\n",name,(int)tid);
    for (;;) {
        if (syscall(SYS_tgkill,getpid(),tid,SIGPROF)) break;
        struct timespec t={0,500000};
        nanosleep(&t,NULL);
    }
    return NULL;
}
static void sample_start(void) {
    static int started;
    const char *name=getenv("BB_SAMPLE_THREAD");
    if (started || !name || !*name) return;
    started=1;
    struct sigaction action={0};
    action.sa_sigaction=sample_handler;
    action.sa_flags=SA_SIGINFO|SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF,&action,NULL);
    pthread_t thread;
    pthread_create(&thread,NULL,sampler_main,(void *)name);
    pthread_detach(thread);
}
static void sample_report(void) {
    static int got_dumped;
    if (!got_dumped) {
        got_dumped=1;
        const char *got=getenv("BB_DUMP_GOT");
        if (got && *got) {
            const uint64_t target=*(const uint64_t *)(0x800000000ull+strtoull(got,NULL,0));
            Dl_info dl={0};
            dladdr((void *)target,&dl);
            printf("Runtime: GOT %s -> %#lx %s:%s+%#lx\n",got,(unsigned long)target,dl.dli_fname ? dl.dli_fname : "?",
                   dl.dli_sname ? dl.dli_sname : "?",(unsigned long)(target-(uint64_t)(dl.dli_sname ? dl.dli_saddr : dl.dli_fbase)));
        }
    }
    const uint64_t total=atomic_exchange(&samples_total,0);
    if (!total) return;
    struct { uint64_t rip, caller, count; } rows[1024]; int n=0;
    for (int i=0;i<1024;++i) { uint64_t c=atomic_exchange(&sample_counts[i],0); if (c) { rows[n].rip=sample_rips[i]; rows[n].caller=sample_callers[i]; rows[n].count=c; ++n; } }
    for (int i=1;i<n;++i) for (int j=i;j>0 && rows[j].count>rows[j-1].count;--j) { __typeof__(rows[0]) t=rows[j]; rows[j]=rows[j-1]; rows[j-1]=t; }
    printf("Thread samples (%llu):",(unsigned long long)total);
    for (int i=0;i<n && i<16;++i) {
        const uint64_t text_lo=0x800000000ull, text_hi=0x800000000ull+0x50d96dcull;
        if (rows[i].rip>=text_lo && rows[i].rip<text_hi) {
            printf("%s +%#lx %.1f%%",i?";":"",(unsigned long)(rows[i].rip-text_lo),100.0*rows[i].count/total);
            continue;
        }
        Dl_info dl={0};
        dladdr((void *)rows[i].rip,&dl);
        const char *module=dl.dli_fname ? strrchr(dl.dli_fname,'/') : NULL;
        printf("%s host:%s%s%s+%#lx (from +%#lx) %.1f%%",i?";":"",module ? module+1 : "?",dl.dli_sname ? ":" : "",
               dl.dli_sname ? dl.dli_sname : "",(unsigned long)(rows[i].rip-(dl.dli_sname ? (uint64_t)dl.dli_saddr : (uint64_t)dl.dli_fbase)),
               (unsigned long)rows[i].caller,100.0*rows[i].count/total);
    }
    printf("\n");
}
#endif
