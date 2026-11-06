/* System-library functions first needed by Gravity Rush 2 (CUSA03694 v1.00).
 *
 * Fibers are implemented via pthread-based fiber emulation (ARM64-friendly): each guest fiber runs
 * on its own pthread with a dedicated stack, switching controlled by condition variables. The rest are
 * small contracts of a console without extra content, network or optional features: content
 * search/export/delete, error dialogs, temporary data, kernel queries, POSIX sockets. Image
 * codecs log their first use; they are filled in when the game is seen using them. libSecure is
 * the game's own sysdata/libSceSecure.prx, linked as a module.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/random.h>
#  if defined(__x86_64__) && (defined(__linux__) || defined(__FreeBSD__))
#    include <ucontext.h>
#  elif defined(__linux__) || defined(__FreeBSD__)
#    warning "ucontext.h present but makecontext/swapcontext unreliable on ARM64; guest fibers use pthread fallback"
#  else
#    warning "no ucontext.h for this POSIX target; guest fibers will use pthread fallback"
#  endif
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define KERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define KERR_EINVAL KERR(22)
#define KERR_ENOENT KERR(2)
#define KERR_EPERM KERR(1)

static void once(const char *what) {
    static const char *seen[128];
    static int count;
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (int i=0;i<count;++i) if (seen[i]==what) { pthread_mutex_unlock(&lock); return; }
    if (count<128) seen[count++]=what;
    pthread_mutex_unlock(&lock);
    printf("Runtime: %s (GR2 placeholder)\n",what);
    fflush(stdout);
}

/* ---- Kernel ---- */
static ABI int32_t kernel_queue_event_ok(void *eq,int32_t fd,uint64_t size,void *udata) {
    (void)eq; (void)fd; (void)size; (void)udata;
    once("sceKernelAdd/DeleteRead/WriteEvent (fd events never fire)");
    return 0;
}
static ABI int32_t kernel_current_cpu(void) {
#ifdef _WIN32
    int cpu=(int)GetCurrentProcessorNumber();
#else
    int cpu=sched_getcpu();
#endif
    return cpu<0 ? 0 : cpu%7; /* the game sees the PS4's seven usable cores */
}
static ABI int32_t kernel_is_stack(void *address,void **start,void **end) {
    void *base=NULL; size_t size=0;
#ifdef _WIN32
    ULONG_PTR low=0, high=0;
    GetCurrentThreadStackLimits(&low,&high);
    base=(void *)low; size=(size_t)(high-low);
#else
    pthread_attr_t attr;
    if (!pthread_getattr_np(pthread_self(),&attr)) {
        pthread_attr_getstack(&attr,&base,&size);
        pthread_attr_destroy(&attr);
    }
#endif
    int inside=base && (char *)address>=(char *)base && (char *)address<(char *)base+size;
    if (start) *start=inside ? base : NULL;
    if (end) *end=inside ? (char *)base+size : NULL;
    return 0;
}
static ABI int32_t kernel_load_start_module(const char *path,uint64_t args,const void *argp,uint32_t flags,
                                            const void *option,int32_t *result) {
    (void)args; (void)argp; (void)flags; (void)option;
    /* The game's own modules (/app0/...) are linked ahead of time (link_modules.py --modules),
     * so loading one succeeds: its exports are already bound to the eboot's imports. */
    static int32_t next_handle=0x100;
    const int own=path && !strncmp(path,"/app0/",6);
    printf("Runtime: sceKernelLoadStartModule(%s): %s\n",path ? path : "(null)",
           own ? "linked ahead of time" : "not available");
    fflush(stdout);
    if (result) *result=0;
    return own ? __atomic_fetch_add(&next_handle,1,__ATOMIC_RELAXED) : KERR_ENOENT;
}
static ABI int32_t kernel_set_gpo(uint32_t bits) { (void)bits; return 0; }
static ABI int32_t kernel_sleep(uint32_t seconds) { sleep(seconds); return 0; }
static ABI int32_t kernel_uuid_create(uint8_t *uuid) {
    if (!uuid) return KERR_EINVAL;
#ifdef _WIN32
    for (int i=0;i<16;++i) uuid[i]=(uint8_t)rand();
#else
    if (getrandom(uuid,16,0)!=16) for (int i=0;i<16;++i) uuid[i]=(uint8_t)rand();
#endif
    uuid[6]=(uint8_t)((uuid[6]&0x0f)|0x40); /* version 4 */
    uuid[8]=(uint8_t)((uuid[8]&0x3f)|0x80);
    return 0;
}

/* ---- POSIX sockets: no network ---- */
#define GUEST_ENETDOWN 50 /* FreeBSD */
static ABI int32_t socket_down(void) {
    once("POSIX sockets (network is down)");
    *runtime_errno()=GUEST_ENETDOWN;
    return -1;
}

/* ---- Fibers ---- */
#define FIBER_MAGIC UINT64_C(0x5245424946325247) /* "GR2FIBER" */
#define FIBER_ERR_NULL ((int32_t)0x80590001)
#define FIBER_ERR_INVALID ((int32_t)0x80590004)
#define FIBER_ERR_PERMISSION ((int32_t)0x80590005)
#define FIBER_ERR_STATE ((int32_t)0x80590006)
typedef void (ABI *FiberEntry)(uint64_t arg_on_initialize,uint64_t arg_on_run);
#ifndef _WIN32
/* ARM64 / pthread-based fiber emulation — ucontext makecontext/swapcontext unreliable on ARM64.
 * Each guest fiber becomes a dedicated pthread; switching via condition variables. */
typedef struct HostFiber {
    FiberEntry entry;
    uint64_t arg_on_initialize, arg_to_deliver;
    int started, running, finished;
    char name[32];
    pthread_t thread;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
} HostFiber;
typedef struct { uint64_t magic; HostFiber *host; } GuestFiber;
static __thread struct { HostFiber *current; } fiber_tls;
static HostFiber *fiber_of(void *guest) {
    GuestFiber *g=guest;
    return g && g->magic==FIBER_MAGIC ? g->host : NULL;
}
/* Per-fiber wrapper: runs entry, then waits on cv for resume or cleanup. */
static void *fiber_thread_func(void *arg) {
    HostFiber *f=arg;
    f->entry(f->arg_on_initialize,f->arg_to_deliver);
    pthread_mutex_lock(&f->mtx);
    f->finished=1; f->running=0;
    pthread_cond_signal(&f->cv);
    pthread_mutex_unlock(&f->mtx);
    while(1) {
        pthread_mutex_lock(&f->mtx);
        while(!f->finished && !f->running) pthread_cond_wait(&f->cv,&f->mtx);
        if(f->finished) break;
        f->running=0;
        pthread_mutex_unlock(&f->mtx);
    }
    return NULL;
}
static ABI int32_t fiber_initialize(void *guest,const char *name,FiberEntry entry,uint64_t arg_on_initialize,
                                    void *context,uint64_t context_size,const void *option,uint32_t build_version) {
    (void)option; (void)build_version;
    if (!guest || !entry) return FIBER_ERR_NULL;
    HostFiber *f=calloc(1,sizeof(*f));
    if (!f) return FIBER_ERR_INVALID;
    f->entry=entry; f->arg_on_initialize=arg_on_initialize;
    snprintf(f->name,sizeof(f->name),"%s",name ? name : "");
    size_t size=(size_t)context_size;
    void *stack=context;
    int stack_owned=0;
    if (!stack || size<16384) { size=size<65536 ? 65536 : size; stack=malloc(size); stack_owned=1; }
    pthread_attr_t attr;
    if(pthread_attr_init(&attr)==0 && stack) {
        pthread_attr_setstacksize(&attr,size);
        pthread_attr_setstack(&attr,stack,0);
    }
    int rc=pthread_create(&f->thread,&attr,fiber_thread_func,f);
    if(rc!=0) { if(stack_owned) free(stack); pthread_attr_destroy(&attr); free(f); return FIBER_ERR_INVALID; }
    pthread_attr_destroy(&attr);
    ((GuestFiber *)guest)->magic=FIBER_MAGIC;
    ((GuestFiber *)guest)->host=f;
    return 0;
}
static ABI int32_t fiber_finalize(void *guest) {
    HostFiber *f=fiber_of(guest);
    if (!f) return FIBER_ERR_INVALID;
    pthread_mutex_lock(&f->mtx);
    f->running=0; f->finished=1;
    pthread_cond_signal(&f->cv);
    pthread_mutex_unlock(&f->mtx);
    pthread_join(f->thread,NULL);
    ((GuestFiber *)guest)->magic=0;
    free(f);
    return 0;
}
static void fiber_mark_done(HostFiber *f) {
    pthread_mutex_lock(&f->mtx);
    f->running=0; f->finished=1;
    pthread_cond_signal(&f->cv);
    pthread_mutex_unlock(&f->mtx);
}
static void fiber_set_resume(HostFiber *f,uint64_t arg) {
    pthread_mutex_lock(&f->mtx);
    f->arg_to_deliver=arg; f->running=1; f->finished=0;
    pthread_cond_signal(&f->cv);
    pthread_mutex_unlock(&f->mtx);
}
static ABI int32_t fiber_run(void *guest,uint64_t arg_on_run_to,uint64_t *arg_on_return) {
    (void)arg_on_return;
    HostFiber *f=fiber_of(guest);
    if (!f) return FIBER_ERR_INVALID;
    pthread_mutex_lock(&f->mtx);
    int running=f->running;
    if(running) { pthread_mutex_unlock(&f->mtx); return FIBER_ERR_STATE; }
    f->arg_to_deliver=arg_on_run_to; f->running=1; f->finished=0;
    pthread_cond_signal(&f->cv);
    pthread_mutex_unlock(&f->mtx);
    pthread_mutex_lock(&f->mtx);
    while(f->running) pthread_cond_wait(&f->cv,&f->mtx);
    pthread_mutex_unlock(&f->mtx);
    return 0;
}
static ABI int32_t fiber_switch(void *guest,uint64_t arg_on_run_to,uint64_t *arg_on_run) {
    (void)arg_on_run;
    HostFiber *to=fiber_of(guest), *from=fiber_tls.current;
    if(!to) return FIBER_ERR_INVALID;
    if(!from) return FIBER_ERR_PERMISSION;
    if(to->running) return FIBER_ERR_STATE;
    fiber_set_resume(to,arg_on_run_to);
    return 0;
}
static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return,uint64_t *arg_on_run) {
    (void)arg_on_return; (void)arg_on_run;
    HostFiber *from=fiber_tls.current;
    if(!from) return FIBER_ERR_PERMISSION;
    fiber_mark_done(from);
    return 0;
}
#else
/* Windows: Win32 fibers. Each fiber gets its own stack from the system (the game's context
 * memory is not needed); the thread becomes a fiber the first time it runs one. */
typedef struct HostFiber {
    void *fiber;
    FiberEntry entry;
    uint64_t arg_on_initialize, arg_to_deliver;
    uint64_t *resume_out;
    int started, running;
    char name[32];
} HostFiber;
typedef struct { uint64_t magic; HostFiber *host; } GuestFiber;
static __thread struct {
    void *thread_fiber;
    HostFiber *current;
    uint64_t *thread_return_out;
} fiber_tls;
static HostFiber *fiber_of(void *guest) {
    GuestFiber *g=guest;
    return g && g->magic==FIBER_MAGIC ? g->host : NULL;
}
static void WINAPI fiber_trampoline(void *param) {
    HostFiber *f=param;
    f->entry(f->arg_on_initialize,f->arg_to_deliver);
    fprintf(stderr,"STOP: fiber %s returned from its entry function\n",f->name);
    abort();
}
static ABI int32_t fiber_initialize(void *guest,const char *name,FiberEntry entry,uint64_t arg_on_initialize,
                                    void *context,uint64_t context_size,const void *option,uint32_t build_version) {
    (void)context; (void)option; (void)build_version;
    if (!guest || !entry) return FIBER_ERR_NULL;
    HostFiber *f=calloc(1,sizeof(*f));
    if (!f) return FIBER_ERR_INVALID;
    f->entry=entry; f->arg_on_initialize=arg_on_initialize;
    snprintf(f->name,sizeof(f->name),"%s",name ? name : "");
    SIZE_T size=context_size<65536 ? 65536 : (SIZE_T)context_size;
    f->fiber=CreateFiber(size,fiber_trampoline,f);
    if (!f->fiber) { free(f); return FIBER_ERR_INVALID; }
    ((GuestFiber *)guest)->magic=FIBER_MAGIC;
    ((GuestFiber *)guest)->host=f;
    return 0;
}
static ABI int32_t fiber_finalize(void *guest) {
    HostFiber *f=fiber_of(guest);
    if (!f) return FIBER_ERR_INVALID;
    if (f->running) return FIBER_ERR_STATE;
    ((GuestFiber *)guest)->magic=0;
    DeleteFiber(f->fiber);
    free(f);
    return 0;
}
static void fiber_enter(HostFiber *f,uint64_t arg) {
    if (f->started && f->resume_out) *f->resume_out=arg;
    f->arg_to_deliver=arg;
    f->started=1; f->running=1;
    fiber_tls.current=f;
}
static ABI int32_t fiber_run(void *guest,uint64_t arg_on_run_to,uint64_t *arg_on_return) {
    HostFiber *f=fiber_of(guest);
    if (!f) return FIBER_ERR_INVALID;
    if (fiber_tls.current) return FIBER_ERR_PERMISSION;
    if (f->running) return FIBER_ERR_STATE;
    if (!fiber_tls.thread_fiber) {
        fiber_tls.thread_fiber=IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
        if (!fiber_tls.thread_fiber) return FIBER_ERR_INVALID;
    }
    fiber_tls.thread_return_out=arg_on_return;
    fiber_enter(f,arg_on_run_to);
    SwitchToFiber(f->fiber);
    fiber_tls.current=NULL;
    return 0;
}
static ABI int32_t fiber_switch(void *guest,uint64_t arg_on_run_to,uint64_t *arg_on_run) {
    HostFiber *to=fiber_of(guest), *from=fiber_tls.current;
    if (!to) return FIBER_ERR_INVALID;
    if (!from) return FIBER_ERR_PERMISSION;
    if (to->running) return FIBER_ERR_STATE;
    from->resume_out=arg_on_run; from->running=0;
    fiber_enter(to,arg_on_run_to);
    SwitchToFiber(to->fiber);
    return 0;
}
static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return,uint64_t *arg_on_run) {
    HostFiber *from=fiber_tls.current;
    if (!from) return FIBER_ERR_PERMISSION;
    if (fiber_tls.thread_return_out) *fiber_tls.thread_return_out=arg_on_return;
    from->resume_out=arg_on_run; from->running=0;
    fiber_tls.current=NULL;
    SwitchToFiber(fiber_tls.thread_fiber);
    return 0;
}
#endif

/* ---- App content: temporary data, no add-on entitlements ---- */
static ABI int32_t appcontent_temp_mount2(uint32_t option,char *mount_point) {
    (void)option;
    if (!mount_point) return (int32_t)0x80D90002;
    memset(mount_point,0,16);
    strcpy(mount_point,"/temp0"); /* runtime_file maps it into the user directory */
    return 0;
}
static ABI int32_t appcontent_temp_unmount(const char *mount_point) { (void)mount_point; return 0; }
static ABI int32_t appcontent_entitlement_key(const void *label,void *key) {
    (void)label; (void)key;
    return (int32_t)0x80D90007; /* SCE_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT */
}

/* ---- Content search / export / delete: nothing installed, exports accepted and dropped ---- */
static ABI int32_t content_ok(void) { once("sceContentSearch/Export/Delete (no content)"); return 0; }
static ABI int32_t content_search_count(uint32_t *count) { if (count) *count=0; return 0; }
static ABI int32_t content_search_search(const void *condition,uint32_t start,uint32_t max,void *ids,uint32_t *found) {
    (void)condition; (void)start; (void)max; (void)ids;
    if (found) *found=0;
    return 0;
}
static ABI int32_t content_search_my_index(uint32_t *index) { if (index) *index=0; return 0; }
static ABI int32_t content_search_not_found(void) { return (int32_t)0x80D60003; }

/* ---- Error dialog: logged, finishes at once ---- */
typedef struct { uint64_t size; int32_t error_code; int32_t user_id; } ErrorDialogParam;
static int error_dialog_status;
static ABI int32_t error_dialog_init(void) { error_dialog_status=1; return 0; }
static ABI int32_t error_dialog_open(const ErrorDialogParam *param) {
    printf("Runtime: sceErrorDialogOpen(error %#x)\n",param ? (unsigned)param->error_code : 0u);
    fflush(stdout);
    error_dialog_status=3; /* FINISHED */
    return 0;
}
static ABI int32_t error_dialog_status_get(void) { return error_dialog_status; }
static ABI int32_t error_dialog_term(void) { error_dialog_status=0; return 0; }

/* ---- Save data transfer (PS3/Vita import): never available ---- */
static ABI int32_t savedata_transferring_mount(const void *mount,void *result) {
    (void)mount; (void)result;
    return (int32_t)0x809F0008; /* SCE_SAVE_DATA_ERROR_NOT_FOUND */
}

/* ---- Image codecs and libSecure: logged until the game is seen using them ---- */
static ABI int32_t png_unsupported(void) { once("scePngDec*"); return (int32_t)0x80690001; }
static ABI int32_t jpeg_dec_unsupported(void) { once("sceJpegDec*"); return (int32_t)0x80650001; }
static ABI int32_t jpeg_enc_unsupported(void) { once("sceJpegEnc*"); return (int32_t)0x80650101; }

static ABI int32_t playgo_dialog_close(void) { return 0; }

static const RuntimeExport exports[]={
    {"sceKernelAddReadEvent",kernel_queue_event_ok}, {"sceKernelAddWriteEvent",kernel_queue_event_ok},
    {"sceKernelDeleteReadEvent",kernel_queue_event_ok}, {"sceKernelDeleteWriteEvent",kernel_queue_event_ok},
    {"sceKernelGetCurrentCpu",kernel_current_cpu}, {"sceKernelIsStack",kernel_is_stack},
    {"sceKernelLoadStartModule",kernel_load_start_module}, {"sceKernelSetGPO",kernel_set_gpo},
    {"sceKernelSleep",kernel_sleep}, {"sceKernelUuidCreate",kernel_uuid_create},
    {"socket",socket_down}, {"bind",socket_down}, {"listen",socket_down}, {"accept",socket_down},
    {"send",socket_down}, {"recv",socket_down}, {"select",socket_down}, {"shutdown",socket_down},
    {"_sceFiberInitializeImpl",fiber_initialize}, {"sceFiberFinalize",fiber_finalize},
    {"sceFiberRun",fiber_run}, {"sceFiberSwitch",fiber_switch},
    {"sceFiberReturnToThread",fiber_return_to_thread},
    {"sceAppContentTemporaryDataMount2",appcontent_temp_mount2},
    {"sceAppContentTemporaryDataUnmount",appcontent_temp_unmount},
    {"sceAppContentGetEntitlementKey",appcontent_entitlement_key},
    {"sceContentSearchInit",content_ok}, {"sceContentSearchTerm",content_ok},
    {"sceContentSearchGetNumOfContent",content_search_count},
    {"sceContentSearchSearchContent",content_search_search},
    {"sceContentSearchGetMyApplicationIndex",content_search_my_index},
    {"sceContentSearchOpenMetadataByContentId",content_search_not_found},
    {"sceContentSearchGetMetadataValue",content_search_not_found},
    {"sceContentSearchCloseMetadata",content_ok},
    {"sceContentExportInit",content_ok}, {"sceContentExportTerm",content_ok},
    {"sceContentExportStart",content_ok}, {"sceContentExportFinish",content_ok},
    {"sceContentExportFromData",content_ok}, {"sceContentExportFromDataWithThumbnail",content_ok},
    {"sceContentDeleteInitialize",content_ok}, {"sceContentDeleteTerminate",content_ok},
    {"sceContentDeleteById",content_ok},
    {"sceErrorDialogInitialize",error_dialog_init}, {"sceErrorDialogOpen",error_dialog_open},
    {"sceErrorDialogUpdateStatus",error_dialog_status_get}, {"sceErrorDialogGetStatus",error_dialog_status_get},
    {"sceErrorDialogTerminate",error_dialog_term},
    {"sceSaveDataTransferringMount",savedata_transferring_mount},
    {"scePngDecQueryMemorySize",png_unsupported}, {"scePngDecCreate",png_unsupported},
    {"scePngDecParseHeader",png_unsupported}, {"scePngDecDecode",png_unsupported},
    {"scePngDecDelete",png_unsupported},
    {"sceJpegDecQueryMemorySize",jpeg_dec_unsupported}, {"sceJpegDecCreate",jpeg_dec_unsupported},
    {"sceJpegDecParseHeader",jpeg_dec_unsupported}, {"sceJpegDecDecode",jpeg_dec_unsupported},
    {"sceJpegDecDelete",jpeg_dec_unsupported},
    {"sceJpegEncQueryMemorySize",jpeg_enc_unsupported}, {"sceJpegEncCreate",jpeg_enc_unsupported},
    {"sceJpegEncEncode",jpeg_enc_unsupported}, {"sceJpegEncDelete",jpeg_enc_unsupported},
    {"scePlayGoDialogClose",playgo_dialog_close},
};
uintptr_t runtime_gr2_resolve(const char *name) {
    return RUNTIME_LOOKUP(exports,name);
}
