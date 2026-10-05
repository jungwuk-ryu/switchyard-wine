#!/usr/bin/env python3
"""Compile the real control-stack allocator/layout/publication with fault cases."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text(encoding="utf-8")
unix_source = Path(sys.argv[1]).with_name("unixlib_fex.c").read_text(encoding="utf-8")
unix_start = unix_source.index("static NTSTATUS control_stack_protect(")
native_protect = unix_source[unix_start:unix_source.index("static NTSTATUS memory_map(", unix_start)]
ntdll_source = Path(sys.argv[1]).parents[1].joinpath("ntdll/signal_arm64ec.c").read_text(encoding="utf-8")
ack_start = ntdll_source.index("static inline LONGLONG read_generation64(")
acknowledge = ntdll_source[ack_start:ntdll_source.index("static inline void mark_deferred_provider_resync(", ack_start)]


def region(start, end):
    first = source.index(start)
    return source[first:source.index(end, first + len(start))]


declarations = region("enum xtajit64_native_transition", "typedef NTSTATUS (WINAPI *arm64x_get_information)")
limit = region("static ULONG_PTR control_stack_limit(", "/* State itself is supplied")
layout = region("static BOOL flight_validate_recorder_layout(", "static BOOL flight_has_valid_recorder(")
allocate = region("static NTSTATUS allocate_transition_state(", "static NTSTATUS query_current_thread_teb(")
release = region("static NTSTATUS free_transition_state(", "static void *resolve_arm64ec_export(")
publish = region("static NTSTATUS synchronize_transition_state_mapping( struct xtajit64_thread_state *state,\n"
                 "                                                       BOOL *provider_touched )\n{",
                 "static NTSTATUS unregister_transition_state_mapping(")
fixture = r'''
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <pthread.h>
typedef uint64_t UINT64;
typedef uint32_t UINT32, ULONG, NTSTATUS;
typedef uintptr_t ULONG_PTR;
typedef size_t SIZE_T;
typedef int BOOL;
typedef int32_t LONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_NO_MEMORY 0xc0000017u
#define STATUS_INVALID_PARAMETER 0xc000000du
#define STATUS_INVALID_ADDRESS 0xc0000141u
#define STATUS_INVALID_HANDLE 0xc0000008u
#define TEST_ERROR 0xc0000022u
#define PAGE_NOACCESS 1
#define PAGE_READWRITE 4
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define MEM_PRIVATE 0x20000
#define MemoryBasicInformation 0
#define MemoryWineTranslatedViewInformation 1
#define WINE_TRANSLATED_VIEW_INFORMATION_VERSION 1
#define XTAJIT64_GUEST_PAGE_SIZE 0x1000
#define XTAJIT64_MAX_HOST_PAGE_SIZE 0x10000
#define XTAJIT64_X64_USER_ADDRESS_MAX 0x00007fffffffffffULL
#define XTAJIT64_CONTROL_STACK_SIZE 0x40000
#define XTAJIT64_MAX_TRANSITION_DEPTH 64
#define XTAJIT64_EC_ENTRY_CACHE_SIZE 32
#define XTAJIT64_THREAD_STATE_MAGIC 0x363454494a415458ull
#define XTAJIT64_FLIGHT_FRAME_ENTRY 1
#define XTAJIT64_FLIGHT_FRAME_EXIT 2
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define check(e) do { if (!(e)) { fprintf(stderr, "control stack check line %d: %s\n", __LINE__, #e); exit(1); } } while (0)
struct xtajit64_direct_capsule { uint64_t fields[8]; };
struct xtajit64_control_stack_params { UINT64 allocation, size; };
typedef struct { void *BaseAddress, *AllocationBase; SIZE_T RegionSize; ULONG State, Type, Protect; } MEMORY_BASIC_INFORMATION;
typedef struct { ULONG Version, Flags; void *GuestBase, *HostBase, *AllocationBase; SIZE_T RegionSize; ULONG Protect, Reserved; } WINE_TRANSLATED_VIEW_INFORMATION;
static struct { pthread_mutex_t mutex; ULONG native_page_size; BOOL initialized, shutting_down; NTSTATUS poison_status; }
    provider = {PTHREAD_MUTEX_INITIALIZER, 0, TRUE, FALSE, 0};
struct __attribute__((aligned(64))) xtajit64_flight_recorder { uint64_t magic; unsigned char payload[0x6880 - 8]; };
static ULONG native_page_size;
static BOOL flight_recorder_enabled;
static size_t actual_page, raw_size;
static void *raw, *owned;
static int fault_mode, protects, frees, header_reads, describes, maps, fail_describe, fail_map;
static int in_unix, deferred_mutations, queries;
static int interference;
static LONGLONG deferred_vm_sequence, deferred_vm_generation, deferred_vm_resynced_generation;
static LONG deferred_vm_active;
static pthread_mutex_t deferred_vm_resync_lock = PTHREAD_MUTEX_INITIALIZER;
#define ReadAcquire64(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define ReadAcquire(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define InterlockedExchange64(p,v) __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST)
static void RtlAcquireSRWLockExclusive(pthread_mutex_t *lock) { check(!pthread_mutex_lock(lock)); }
static void RtlReleaseSRWLockExclusive(pthread_mutex_t *lock) { check(!pthread_mutex_unlock(lock)); }
static void pe_mutation(BOOL success)
{
    deferred_vm_sequence += 2;
    if (success) ++deferred_vm_generation;
    ++deferred_mutations;
}
static void reset_deferred(void)
{
    deferred_mutations = interference = deferred_vm_active = 0;
    deferred_vm_sequence = deferred_vm_generation = deferred_vm_resynced_generation = 0;
}
static void *GetCurrentProcess(void) { return NULL; }
#define NtCurrentProcess GetCurrentProcess
static void xtajit64_flight_recorder_init(struct xtajit64_flight_recorder *rec)
{ memset(rec, 0, sizeof(*rec)); rec->magic = 123; }
static BOOL xtajit64_flight_validate_layout(ULONG_PTR base, SIZE_T size, ULONG_PTR top,
                                           const struct xtajit64_flight_recorder *rec)
{ (void)base; (void)size; (void)top; ++header_reads; return rec->magic == 123; }
static NTSTATUS NtAllocateVirtualMemory(void *process, void **out, ULONG_PTR bits, SIZE_T *size,
                                        ULONG type, ULONG protect)
{
    size_t alignment = native_page_size ? native_page_size : actual_page;
    (void)process;
    check(!bits && *size == XTAJIT64_CONTROL_STACK_SIZE && !*out &&
          type == (MEM_RESERVE | MEM_COMMIT) && protect == PAGE_READWRITE && !raw);
    if (fault_mode == 1) return TEST_ERROR;
    pe_mutation(TRUE);
    raw_size = *size + alignment;
    raw = mmap(NULL, raw_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    check(raw != MAP_FAILED);
    owned = (void *)(((uintptr_t)raw + alignment - 1) & ~(uintptr_t)(alignment - 1));
    *out = (char *)owned + (fault_mode == 3);
    if (fault_mode == 4) --*size;
    return 0;
}
NTSTATUS NtProtectVirtualMemory(void *process, void **base, SIZE_T *size, ULONG protect, ULONG *old)
{
    (void)process;
    check(!pthread_mutex_trylock(&provider.mutex));
    check(!pthread_mutex_unlock(&provider.mutex));
    if (!in_unix) pe_mutation(TRUE);
    /* Ordered schedules, not a timing assumption: another PE mutation may
     * finish successfully, fail, or remain active during native protection. */
    if (interference == 1) pe_mutation(TRUE);
    if (interference == 2) pe_mutation(FALSE);
    if (interference == 3) { ++deferred_vm_active; ++deferred_vm_sequence; }
    ++protects;
    check(*base == (char *)owned + native_page_size && *size == native_page_size && protect == PAGE_NOACCESS);
    if (fault_mode == 2) return TEST_ERROR;
    *old = PAGE_READWRITE;
    /* Smaller synthetic pages exercise arithmetic/publication, not host mprotect. */
    if (native_page_size >= actual_page && !getenv("CONTROL_STACK_NEGATIVE_NO_PROTECT"))
        check(!mprotect(*base, *size, PROT_NONE));
    if (fault_mode == 18) *old = PAGE_NOACCESS;
    if (fault_mode == 19) *base = (char *)*base + native_page_size;
    if (fault_mode == 20) *size *= 2;
    return 0;
}
static NTSTATUS NtQueryVirtualMemory(void *process, void *address, int kind, void *out, SIZE_T size, void *length)
{
    (void)process; (void)length;
    check(in_unix && address == owned);
    check(!pthread_mutex_trylock(&provider.mutex));
    check(!pthread_mutex_unlock(&provider.mutex));
    ++queries;
    if (kind == MemoryBasicInformation)
    {
        MEMORY_BASIC_INFORMATION *info = out;
        check(size == sizeof(*info));
        if (fault_mode == 5) return TEST_ERROR;
        *info = (MEMORY_BASIC_INFORMATION){owned, owned, XTAJIT64_CONTROL_STACK_SIZE, MEM_COMMIT, MEM_PRIVATE, PAGE_READWRITE};
        if (fault_mode == 6) info->AllocationBase = (char *)owned + native_page_size;
        if (fault_mode == 7) info->State = MEM_RESERVE;
        if (fault_mode == 8) info->Type = 0;
        if (fault_mode == 9) info->Protect = PAGE_NOACCESS;
        if (fault_mode == 10) info->RegionSize /= 2;
        if (fault_mode == 21) info->BaseAddress = (char *)owned + native_page_size;
    }
    else
    {
        WINE_TRANSLATED_VIEW_INFORMATION *info = out;
        check(kind == MemoryWineTranslatedViewInformation && size == sizeof(*info));
        if (fault_mode == 17) return TEST_ERROR;
        *info = (WINE_TRANSLATED_VIEW_INFORMATION){1, 0, owned, owned, owned, XTAJIT64_CONTROL_STACK_SIZE, PAGE_READWRITE, 0};
        if (fault_mode == 11) info->Version = 0;
        if (fault_mode == 12) info->Reserved = 1;
        if (fault_mode == 13) info->Flags = 1;
        if (fault_mode == 14) info->GuestBase = (char *)owned + 1;
        if (fault_mode == 15) info->AllocationBase = (char *)owned + 1;
        if (fault_mode == 16) info->RegionSize /= 2;
        if (fault_mode == 22) info->HostBase = (char *)owned + 1;
    }
    return 0;
}
static NTSTATUS NtFreeVirtualMemory(void *process, void **base, SIZE_T *size, ULONG type)
{
    (void)process;
    check(raw && *base && !*size && type == MEM_RELEASE);
    check(!munmap(raw, raw_size));
    raw = owned = NULL;
    pe_mutation(TRUE);
    ++frees;
    return 0;
}
struct xtajit64_memory_params { UINT64 guest, host, size, allocation_base; ULONG protect; };
static struct xtajit64_memory_params published[3];
static NTSTATUS get_allocation_base(const void *state, ULONG_PTR *base)
{ check(state == owned); *base = (ULONG_PTR)owned; return 0; }
static NTSTATUS describe_host_mapping(ULONG_PTR start, SIZE_T size, ULONG_PTR base, ULONG protect,
                                      struct xtajit64_memory_params *params)
{
    ++describes;
    check(base == (ULONG_PTR)owned);
    if (describes == fail_describe) return TEST_ERROR;
    *params = (struct xtajit64_memory_params){start, start, size, base, protect};
    return 0;
}
static NTSTATUS unix_memory_map(struct xtajit64_memory_params *params)
{
    check(describes == 3 && maps < 3);
    published[maps++] = *params;
    return maps == fail_map ? TEST_ERROR : 0;
}
#define XTAJIT64_CALL(name,args) unix_##name(args)
'''
native_wrapper = r'''
static NTSTATUS unix_control_stack_protect(struct xtajit64_control_stack_params *params)
{
    NTSTATUS status;
    check(!in_unix);
    in_unix = 1;
    status = control_stack_protect(params);
    in_unix = 0;
    return status;
}
'''
tests = r'''
_Static_assert(sizeof(struct xtajit64_thread_state) == 0xce0, "actual state layout");
_Static_assert(offsetof(struct xtajit64_thread_state, flight_recorder) == 0x848, "actual recorder offset");
static uintptr_t fault_first, fault_last;
static void guard_fault(int signal, siginfo_t *info, void *context)
{
    (void)context;
    _exit((signal == SIGBUS || signal == SIGSEGV) && (uintptr_t)info->si_addr >= fault_first &&
          (uintptr_t)info->si_addr < fault_last ? 0 : 92);
}
static void check_guard(struct xtajit64_thread_state *state)
{
    pid_t child;
    int status;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = guard_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    fault_first = (uintptr_t)state + native_page_size;
    fault_last = fault_first + native_page_size;
    child = fork();
    check(child >= 0);
    if (!child)
    {
        if (sigaction(SIGSEGV, &action, NULL) || sigaction(SIGBUS, &action, NULL)) _exit(94);
        *(volatile unsigned char *)(fault_last - 1) = 0x5a;
        _exit(93); /* An unprotected boundary must fail this oracle. */
    }
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    check(state->magic == XTAJIT64_THREAD_STATE_MAGIC);
}
int main(void)
{
    const ULONG pages[] = {4096, 16384, 65536};
    struct xtajit64_thread_state *state;
    LONGLONG token_sequence, token_generation;
    BOOL touched;
    unsigned int page, enabled;
    int saved_frees, which;
    alarm(15);
    actual_page = (size_t)sysconf(_SC_PAGESIZE);
    check(actual_page && !(actual_page & (actual_page - 1)) && actual_page <= 65536);
    for (page = 0; page < ARRAY_SIZE(pages); ++page)
    for (enabled = 0; enabled < 2; ++enabled)
    {
        provider.native_page_size = native_page_size = pages[page];
        flight_recorder_enabled = enabled;
        reset_deferred();
        check(capture_deferred_provider_sync_token(&token_sequence, &token_generation));
        state = NULL;
        check(!allocate_transition_state(&state) && state == owned && protects);
        check(deferred_mutations == 1);
        check(control_stack_limit((ULONG_PTR)state) == (ULONG_PTR)state + 2 * native_page_size);
        check(flight_validate_recorder_layout(state));
        check(state->control_stack_top - control_stack_limit((ULONG_PTR)state) >= 65536);
        check((state->flight_recorder != NULL) == enabled);
        if (enabled)
        {
            struct xtajit64_flight_recorder *rec = state->flight_recorder;
            header_reads = 0;
            state->flight_recorder = (void *)((uintptr_t)rec + 64);
            check(!flight_validate_recorder_layout(state) && !header_reads);
            state->flight_recorder = rec;
        }
        describes = maps = 0;
        check(!synchronize_transition_state_mapping(state, &touched) && touched && maps == 3);
        check(acknowledge_single_deferred_provider_mutation(token_sequence, token_generation));
        check(deferred_provider_sync_done());
        for (which = 0; which < 3; ++which)
        {
            check(published[which].protect == (which == 1 ? PAGE_NOACCESS : PAGE_READWRITE));
            check(published[which].guest == (ULONG_PTR)state + (ULONG_PTR)which * native_page_size);
            check(published[which].size == (which < 2 ? native_page_size : state->allocation_size - 2 * native_page_size));
        }
        for (which = 1; which <= 3; ++which)
        {
            describes = maps = 0; fail_describe = which;
            check(synchronize_transition_state_mapping(state, &touched) == TEST_ERROR && !touched && !maps);
            fail_describe = 0; fail_map = which; describes = maps = 0;
            check(synchronize_transition_state_mapping(state, &touched) == TEST_ERROR && touched && maps == which);
            fail_map = 0;
        }
        if (native_page_size >= actual_page) check_guard(state);
        check(!free_transition_state(state) && !raw && deferred_mutations == 2);
    }
    provider.native_page_size = native_page_size = (ULONG)actual_page;
    for (fault_mode = 1; fault_mode <= 22; ++fault_mode)
    {
        reset_deferred();
        check(capture_deferred_provider_sync_token(&token_sequence, &token_generation));
        saved_frees = frees;
        state = (void *)(uintptr_t)1;
        check(allocate_transition_state(&state) && state == (void *)(uintptr_t)1 && !raw);
        check(frees == saved_frees + (fault_mode != 1));
        check(!acknowledge_single_deferred_provider_mutation(token_sequence, token_generation));
        if (fault_mode != 1) check(!deferred_provider_sync_done());
    }
    check(!control_stack_limit(0) && !control_stack_limit(UINTPTR_MAX - 4095));
    fault_mode = 0;
    for (which = 1; which <= 3; ++which)
    {
        reset_deferred();
        check(capture_deferred_provider_sync_token(&token_sequence, &token_generation));
        interference = which;
        check(!allocate_transition_state(&state));
        describes = maps = 0;
        check(!synchronize_transition_state_mapping(state, &touched) && touched);
        check(!acknowledge_single_deferred_provider_mutation(token_sequence, token_generation));
        check(!deferred_provider_sync_done() && !deferred_vm_resynced_generation);
        if (interference == 3) { --deferred_vm_active; ++deferred_vm_sequence; ++deferred_vm_generation; }
        interference = 0;
        check(!free_transition_state(state) && !raw);
    }
    {
        struct xtajit64_control_stack_params params = {0, XTAJIT64_CONTROL_STACK_SIZE};
        int old_queries = queries;
        check(control_stack_protect(NULL) == STATUS_INVALID_PARAMETER);
        check(control_stack_protect(&params) == STATUS_INVALID_PARAMETER);
        params.allocation = UINT64_MAX;
        check(control_stack_protect(&params) == STATUS_INVALID_PARAMETER);
        params.allocation = 0x10000; --params.size;
        check(control_stack_protect(&params) == STATUS_INVALID_PARAMETER);
        ++params.size; provider.initialized = FALSE;
        check(control_stack_protect(&params) == STATUS_INVALID_HANDLE);
        provider.initialized = TRUE; provider.shutting_down = TRUE;
        check(control_stack_protect(&params) == STATUS_INVALID_HANDLE);
        provider.shutting_down = FALSE; provider.poison_status = TEST_ERROR;
        check(control_stack_protect(&params) == TEST_ERROR);
        provider.poison_status = 0; provider.native_page_size = 8191;
        check(control_stack_protect(&params) == STATUS_INVALID_PARAMETER);
        check(queries == old_queries && !raw);
    }
    puts("CONTROL_STACK_MODEL_PASS native-protect single-PE-mutation interference-stays-dirty pages=4K,16K,64K rollback authenticated-views native-fault");
    return 0;
}
'''
implementation = acknowledge + native_protect + native_wrapper + limit + layout + allocate + release + publish
with tempfile.TemporaryDirectory(prefix="xtajit64-control-stack-") as directory:
    root = Path(directory)
    cc = shlex.split(os.environ.get("CC", "cc"))
    sanitize = os.environ.get("SANITIZE", "0")
    if sanitize not in ("0", "undefined"):
        raise SystemExit("SANITIZE must be 0 or undefined")
    flags = (["-fsanitize=undefined", "-fno-sanitize-recover=all"]
             if sanitize == "undefined" else [])
    for name, code, expected in (
        ("current", implementation, 0),
        ("guard-permission-negative", implementation.replace("i == 1 ? PAGE_NOACCESS : PAGE_READWRITE", "PAGE_READWRITE"), 1),
        ("PE-protection-mutation-negative", implementation.replace(
            "    status = control_stack_protect(params);", "    status = control_stack_protect(params); pe_mutation(TRUE);"), 1),
    ):
        path, binary = root / (name + ".c"), root / name
        path.write_text(fixture + declarations + code + tests, encoding="utf-8")
        subprocess.run(cc + flags + ["-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)], check=True)
        env = dict(os.environ)
        env.pop("CONTROL_STACK_NEGATIVE_NO_PROTECT", None)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20, env=env)
        if result.returncode != expected:
            raise SystemExit(name + ": " + result.stdout + result.stderr)
        print(name + ": " + (result.stdout.strip() or "expected rejection"))
        if name == "current":
            env["CONTROL_STACK_NEGATIVE_NO_PROTECT"] = "1"
            negative = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20, env=env)
            if negative.returncode != 1:
                raise SystemExit("missing-native-protection negative control did not fail")
            print("missing-native-protection: expected rejection")
