/* SPDX-License-Identifier: MIT */
/* Bounded same-byte translator/integration discriminator, not an application. */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel_bytes.h"

#ifdef _WIN32
#include <windows.h>
extern uint64_t sb_stack_call(void *, uint64_t, void *, uint64_t, void *);
#else
#include "switchyard_fex.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/ucontext.h>
#endif

#define BASE UINT64_C(0x10200000000)
#define SHADOW UINT64_C(0x10000000000)
#define SEED UINT64_C(0x13579)
#define TOTAL UINT64_C(33554432)
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"SB_FAIL line=%u\n",(unsigned int)__LINE__); exit(97); } } while (0)
static const char *const names[] = {"register","scalar","simd","direct","indirect","stack"};
static unsigned char *mapping;
static size_t page_size;
static uint64_t *data;
static uint64_t stack_top;
static uint64_t clock_frequency;
#ifndef _WIN32
static struct switchyard_fex_process *process;
static struct switchyard_fex_thread *thread;
static struct switchyard_fex_admission *cell;
static uint64_t epoch = 1;
static uint32_t doorbell;
static _Alignas(16) unsigned char registers[408];
static struct switchyard_fex_register_window window;
static uint64_t native_return;
static volatile sig_atomic_t callret_repairs,unaligned_repairs;
static struct sigaction previous_bus,previous_segv;
static void put64(size_t offset,uint64_t value) { memcpy(registers+offset,&value,8); }
static uint64_t get64(size_t offset) { uint64_t value; memcpy(&value,registers+offset,8); return value; }
static void wake(void *context) { (void)context; CHECK(0); }
/* Reuse the SDK's existing C-API test recovery, matching the Wine provider's
 * authenticated nonarchitectural predictor/unaligned-TSO repair. Never absorb
 * an unrelated guest fault. Ordinary system-x18 mode only in this harness. */
static void repair_signal(int number,siginfo_t *info,void *opaque)
{
    ucontext_t *context=opaque;
    struct switchyard_fex_arm64_host_context host={.size=sizeof(host),.version=SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION};
    unsigned int index;
    enum switchyard_fex_result result;
    uint32_t access;
    if (!info || !context || !context->uc_mcontext || !thread) _exit(128+number);
    access=(uint32_t)((context->uc_mcontext->__es.__esr>>6)&1);
    for (index=0;index<29;++index) host.gpr[index]=context->uc_mcontext->__ss.__x[index];
    host.gpr[29]=context->uc_mcontext->__ss.__fp;
    host.gpr[30]=context->uc_mcontext->__ss.__lr;
    memcpy(host.vector,context->uc_mcontext->__ns.__v,sizeof(host.vector));
    host.pc=context->uc_mcontext->__ss.__pc; host.pstate=context->uc_mcontext->__ss.__cpsr;
    host.fpcr=context->uc_mcontext->__ns.__fpcr; host.fpsr=context->uc_mcontext->__ns.__fpsr;
    result=switchyard_fex_thread_repair_callret_fault(thread,&host,access,(uintptr_t)info->si_addr);
    if (result==SWITCHYARD_FEX_OK) ++callret_repairs;
    else if (number==SIGBUS && switchyard_fex_thread_repair_unaligned_tso(thread,&host)==SWITCHYARD_FEX_OK) ++unaligned_repairs;
    else _exit(128+number);
    for (index=0;index<29;++index) context->uc_mcontext->__ss.__x[index]=host.gpr[index];
    context->uc_mcontext->__ss.__fp=host.gpr[29]; context->uc_mcontext->__ss.__lr=host.gpr[30];
    context->uc_mcontext->__ss.__pc=host.pc;
}
static enum switchyard_fex_result query(void *context,uint64_t address,struct switchyard_fex_executable_range *range)
{
    (void)context;
    if (address < BASE || address >= BASE+page_size) return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
    CHECK(switchyard_fex_admission_load(cell)&SWITCHYARD_FEX_ADMISSION_EXECUTING);
    range->base=BASE; range->length=page_size; range->flags=range->reserved=0;
    return SWITCHYARD_FEX_OK;
}
#endif

static uint64_t now(void)
{
#ifdef _WIN32
    LARGE_INTEGER value;
    CHECK(QueryPerformanceCounter(&value) && value.QuadPart>=0);
    return (uint64_t)value.QuadPart;
#else
    struct timespec value;
    CHECK(!clock_gettime(CLOCK_MONOTONIC,&value) && value.tv_sec>=0);
    return (uint64_t)value.tv_sec*UINT64_C(1000000000)+(uint64_t)value.tv_nsec;
#endif
}

static uint64_t invoke(unsigned int kind,uint64_t iterations)
{
    void *code=mapping+kind*256u;
#ifdef _WIN32
    return sb_stack_call(code,iterations,data,SEED,(void *)(uintptr_t)stack_top);
#else
    struct switchyard_fex_stop stop={.size=sizeof(stop),.version=SWITCHYARD_FEX_STOP_VERSION};
    uint64_t return_sp=stack_top-40;
    memcpy((void *)(uintptr_t)return_sp,&native_return,8);
    put64(SWITCHYARD_FEX_WINDOW_RIP,(uintptr_t)code);
    put64(SWITCHYARD_FEX_WINDOW_RSP,return_sp);
    put64(SWITCHYARD_FEX_WINDOW_RCX,iterations);
    put64(SWITCHYARD_FEX_WINDOW_RDX,(uintptr_t)data);
    put64(SWITCHYARD_FEX_WINDOW_R8,SEED);
    CHECK(switchyard_fex_experiment_execute_native_gate(thread,&window,&stop)==SWITCHYARD_FEX_OK &&
          stop.reason==SWITCHYARD_FEX_STOP_EC_TRANSITION && stop.rip==native_return);
    CHECK(get64(SWITCHYARD_FEX_WINDOW_RSP)==return_sp+8);
    return get64(SWITCHYARD_FEX_WINDOW_RAX);
#endif
}

/* Independent arithmetic oracle outside the timer: bounded256-lane work,
 * unsigned modulo arithmetic, NOT a native-ARM performance control. */
static uint64_t sum(uint64_t n,unsigned int stride,unsigned int lane)
{
    uint64_t cycle=0,remainder=0;
    unsigned int count=256u/stride,index;
    for (index=0;index<count;++index) cycle+=data[index*stride+lane];
    for (index=0;index<n%count;++index) remainder+=data[index*stride+lane];
    return cycle*(n/count)+remainder;
}
static uint64_t expected(unsigned int kind,uint64_t iterations)
{
    if (kind==1) return SEED+sum(iterations,1,0);
    if (kind==2) return SEED+sum(iterations,2,0);
    return SEED+iterations*(SEED+3);
}
static void validate_memory(unsigned int kind,uint64_t n)
{
    unsigned int index;
    for (index=0;index<256;++index) CHECK(data[index]==(uint64_t)(index*29u+7u));
    if (kind!=1 && kind!=2)
    {
        for (index=0;index<256;++index) CHECK(!data[256+index]);
        return;
    }
    for (index=0;index<256;++index)
    {
        unsigned int stride=kind==2?2u:1u,slot=index/stride,lane=index%stride,count=256u/stride;
        uint64_t value=0;
        if (n>slot)
        {
            uint64_t last=slot+((n-1-slot)/count)*count+1;
            value=sum(last,stride,lane)+(lane?0:SEED);
        }
        CHECK(data[256+index]==value);
    }
}
static void validate_stack(uint64_t expected_slot)
{
    uint64_t observed;
    /* Both adapters enter at stack_top-40. One push writes stack_top-48.
     * This slot is inside the owned RW stack, below the synthetic return. */
    memcpy(&observed,(const void *)(uintptr_t)(stack_top-48),sizeof(observed));
    CHECK(observed==expected_slot);
}
static void phase(unsigned int kind,const char *state,uint64_t iterations,uint64_t calls)
{
    uint64_t begin,end,checksum=0,index;
    memset(data+256,0,2048);
    begin=now();
    for (index=0;index<calls;++index) checksum+=invoke(kind,iterations);
    end=now();
    CHECK(end>begin && checksum==calls*expected(kind,iterations));
    validate_memory(kind,iterations);
    if (kind==5) validate_stack(BASE+SB_STACK_POP_OFFSET);
    fprintf(stderr,"SB_PHASE_V1 kind=%s state=%s iterations=%" PRIu64 " calls=%" PRIu64
            " ticks=%" PRIu64 " frequency=%" PRIu64 " checksum=%" PRIu64 "\n",
            names[kind],state,iterations,calls,end-begin,clock_frequency,checksum);
}

static void inventory(void)
{
#ifdef _WIN32
    static const char *const modules[]={NULL,"ntdll.dll","kernel32.dll","kernelbase.dll","ucrtbase.dll","msvcrt.dll","xtajit64.dll"};
    size_t index;
    for (index=0;index<sizeof(modules)/sizeof(modules[0]);++index)
    {
        HMODULE module=GetModuleHandleA(modules[index]);
        char path[4096];
        const IMAGE_DOS_HEADER *dos=(const IMAGE_DOS_HEADER *)module;
        const IMAGE_NT_HEADERS *nt;
        DWORD length;
        if (!module) { fprintf(stderr,"SB_PE_ABSENT name=%s\n",modules[index]); continue; }
        length=GetModuleFileNameA(module,path,sizeof(path));
        CHECK(length && length<sizeof(path) && dos->e_magic==IMAGE_DOS_SIGNATURE && dos->e_lfanew>0 && dos->e_lfanew<4096);
        nt=(const IMAGE_NT_HEADERS *)((const unsigned char *)module+dos->e_lfanew);
        CHECK(nt->Signature==IMAGE_NT_SIGNATURE);
        fprintf(stderr,"SB_PE_IMAGE machine=%04x path=%s\n",(unsigned int)nt->FileHeader.Machine,path);
    }
#else
    uint32_t index,count=_dyld_image_count();
    CHECK(count<4096);
    for (index=0;index<count;++index)
    {
        const struct mach_header *header=_dyld_get_image_header(index);
        const char *name=_dyld_get_image_name(index);
        CHECK(header && name);
        fprintf(stderr,"SB_MACH_IMAGE cpu=%08x path=%s\n",(unsigned int)header->cputype,name);
    }
#endif
}

/* Reuse the existing attribution gate's bounded, absent-directory release
 * protocol, solely to authenticate actual loaded mappings BEFORE timers.
 * No profiler/logging is enabled during the performance interval. */
static void await_inventory_release(void)
{
#ifdef _WIN32
    WCHAR release[1024];
    DWORD length=GetEnvironmentVariableW(L"SWITCHYARD_SB_RELEASE",release,1024);
    ULONGLONG deadline;
    CHECK(length && length<1024 && GetFileAttributesW(release)==INVALID_FILE_ATTRIBUTES);
    fprintf(stderr,"SB_READY_V1\n"); fflush(stderr);
    deadline=GetTickCount64()+45000;
    while (GetTickCount64()<deadline)
    {
        DWORD attributes=GetFileAttributesW(release);
        if (attributes!=INVALID_FILE_ATTRIBUTES)
        {
            CHECK(attributes&FILE_ATTRIBUTE_DIRECTORY);
            return;
        }
        Sleep(1);
    }
#else
    const char *release=getenv("SWITCHYARD_SB_RELEASE");
    const struct timespec delay={0,1000000};
    struct stat info;
    uint64_t deadline=now()+UINT64_C(45000000000);
    CHECK(release && strlen(release)<4096 && lstat(release,&info)==-1 && errno==ENOENT);
    fprintf(stderr,"SB_READY_V1\n"); fflush(stderr);
    while (now()<deadline)
    {
        if (!lstat(release,&info)) { CHECK(S_ISDIR(info.st_mode)); return; }
        CHECK(errno==ENOENT);
        CHECK(!nanosleep(&delay,NULL) || errno==EINTR);
    }
#endif
    CHECK(0);
}

int main(void)
{
    unsigned int index,kind;
    uint64_t byte_hash=UINT64_C(14695981039346656037);
#ifdef _WIN32
    SYSTEM_INFO information;
    LARGE_INTEGER frequency;
    DWORD old_protect;
    GetNativeSystemInfo(&information); page_size=information.dwPageSize;
    /* Windows guest pages4KiB, host16KiB. Match identity DATA/STACK placement
     * to the native harness using the fixed measured16KiB geometry. Fail closed
     * on other hosts; this synthetic fixture is not runtime capability policy. */
    CHECK(page_size==4096);
    page_size=16384;
    mapping=VirtualAlloc((void *)(uintptr_t)BASE,page_size*6,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(mapping==(void *)(uintptr_t)BASE);
    CHECK(QueryPerformanceFrequency(&frequency) && frequency.QuadPart>0);
    clock_frequency=(uint64_t)frequency.QuadPart;
#else
    long actual_page=sysconf(_SC_PAGESIZE);
    uint64_t *bitmap;
    size_t bitmap_count;
    struct switchyard_fex_config config={.size=sizeof(config),.abi_version=SWITCHYARD_FEX_ABI_VERSION,
        .flags=SWITCHYARD_FEX_CONFIG_MULTIBLOCK|SWITCHYARD_FEX_CONFIG_EXTERNAL_ADMISSION,
        .low_va_shadow_base=SHADOW,.low_va_shadow_size=UINT64_C(0x100000000),.ec_page_shift=12};
    struct switchyard_fex_execution_domain domain={.size=sizeof(domain),.version=SWITCHYARD_FEX_DOMAIN_VERSION,.wake=wake};
    struct switchyard_fex_native_gate gate={.size=sizeof(gate),.version=SWITCHYARD_FEX_NATIVE_GATE_VERSION,
        .mapping_epoch=(uintptr_t)&epoch,.expected_epoch=1,.gs_base=(uintptr_t)&domain,.suspend_doorbell=(uintptr_t)&doorbell};
    CHECK(actual_page==16384); page_size=(size_t)actual_page;
    {
        mach_vm_address_t address=BASE;
        /* VM_FLAGS_FIXED without OVERWRITE fails on occupied ranges; unlike
         * MAP_FIXED it cannot replace somebody else's existing mapping. mmap
         * hints are advisory and did not honor this identity address here. */
        CHECK(mach_vm_allocate(mach_task_self(),&address,page_size*6,VM_FLAGS_FIXED)==KERN_SUCCESS && address==BASE);
        mapping=(unsigned char *)(uintptr_t)address;
    }
    native_return=BASE+5*page_size;
    config.highest_user_address=BASE+6*page_size-1;
    bitmap_count=(size_t)((config.highest_user_address>>12)/64+1);
    CHECK(bitmap_count<UINT64_C(8388608)); /* <=64MiB virtual, bounded sparse bitmap. */
    bitmap=calloc(bitmap_count,sizeof(*bitmap)); CHECK(bitmap);
    bitmap[(native_return>>12)/64]=UINT64_C(1)<<((native_return>>12)&63);
    config.ec_code_bitmap=(uintptr_t)bitmap;
    CHECK(switchyard_fex_process_create(&config,&process)==SWITCHYARD_FEX_OK);
    CHECK(switchyard_fex_process_set_executable_range_query(process,query,NULL)==SWITCHYARD_FEX_OK);
    CHECK(switchyard_fex_thread_create_with_domain(process,&domain,&thread,&cell)==SWITCHYARD_FEX_OK);
    CHECK(switchyard_fex_experiment_bind_native_gate(thread,&gate)==SWITCHYARD_FEX_OK);
    {
        struct sigaction action={.sa_sigaction=repair_signal,.sa_flags=SA_SIGINFO};
        CHECK(!sigemptyset(&action.sa_mask));
        CHECK(!sigaction(SIGBUS,&action,&previous_bus) && !sigaction(SIGSEGV,&action,&previous_segv));
    }
    window=(struct switchyard_fex_register_window){.size=sizeof(window),.version=SWITCHYARD_FEX_REGISTER_WINDOW_VERSION,
        .data=(uintptr_t)registers,.data_size=sizeof(registers),.gs_base=gate.gs_base};
    put64(SWITCHYARD_FEX_WINDOW_EFLAGS,0x202);
    { uint32_t mxcsr=0x1f80; memcpy(registers+SWITCHYARD_FEX_WINDOW_MXCSR,&mxcsr,4); }
    clock_frequency=UINT64_C(1000000000);
#endif
    CHECK(sb_bytes_length<page_size && sb_bytes_length>1280 &&
          SB_STACK_POP_OFFSET>=1280 && SB_STACK_POP_OFFSET+2<=sb_bytes_length);
    memcpy(mapping,sb_bytes,sb_bytes_length);
    CHECK(!memcmp(mapping,sb_bytes,sb_bytes_length));
    for (index=0;index<sb_bytes_length;++index) byte_hash=(byte_hash^mapping[index])*UINT64_C(1099511628211);
    data=(uint64_t *)(mapping+page_size);
    for (index=0;index<256;++index) data[index]=index*29u+7u;
    stack_top=BASE+4*page_size-64;
#ifdef _WIN32
    CHECK(VirtualProtect(mapping,page_size,PAGE_EXECUTE_READ,&old_protect));
    CHECK(VirtualProtect(mapping+4*page_size,page_size,PAGE_NOACCESS,&old_protect));
    CHECK(FlushInstructionCache(GetCurrentProcess(),mapping,page_size));
#else
    CHECK(!mprotect(mapping,page_size,PROT_READ));
    CHECK(!mprotect(mapping+4*page_size,page_size,PROT_NONE));
#endif
    inventory();
    fprintf(stderr,"SB_BYTES_V1 bytes=%u fnv=%016" PRIx64 " base=%016" PRIx64 " stack=%016" PRIx64 "\n",
            sb_bytes_length,byte_hash,BASE,stack_top);
    await_inventory_release();
    for (kind=0;kind<6;++kind)
    {
        phase(kind,"cold",1024,1);
        phase(kind,"warm-long",TOTAL,1);
        phase(kind,"warm-many",32,TOTAL/32);
    }
    /* Zero/one limits are unscored and AFTER cold/steady timers. Verify that
     * zero performs no inner stack write and one performs an exact round-trip. */
    {
        uint64_t zero=0;
        memcpy((void *)(uintptr_t)(stack_top-48),&zero,sizeof(zero));
        CHECK(invoke(5,0)==expected(5,0)); validate_stack(0);
        CHECK(invoke(5,1)==expected(5,1)); validate_stack(BASE+SB_STACK_POP_OFFSET);
        validate_memory(5,1);
        fprintf(stderr,"SB_STACK_ORACLE zero=1 one=1 phases=3 exact_slot=1\n");
    }
#ifdef _WIN32
    CHECK(VirtualFree(mapping,0,MEM_RELEASE));
#else
    fprintf(stderr,"SB_RECOVERY_V1 callret=%d unaligned=%d\n",(int)callret_repairs,(int)unaligned_repairs);
    /* A repair handler borrows the SDK thread only while it is alive. Retire
     * both handlers BEFORE thread destruction and guest mapping teardown. */
    CHECK(!sigaction(SIGBUS,&previous_bus,NULL) && !sigaction(SIGSEGV,&previous_segv,NULL));
    switchyard_fex_admission_close(cell);
    CHECK(switchyard_fex_thread_destroy(thread)==SWITCHYARD_FEX_OK);
    CHECK(switchyard_fex_process_destroy(process)==SWITCHYARD_FEX_OK);
    CHECK(mach_vm_deallocate(mach_task_self(),(mach_vm_address_t)(uintptr_t)mapping,page_size*6)==KERN_SUCCESS);
    free(bitmap);
#endif
    puts("SB_PASS kernels=6 phases=18 exact_memory=1 exact_stack=1 bytes_equal=1");
    return 0;
}
