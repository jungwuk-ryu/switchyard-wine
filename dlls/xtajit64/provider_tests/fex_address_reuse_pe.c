/* Actual x64 PE coverage for mapped-address dataflow across FPR operations. */
#if 0
#pragma makedep standalone
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

typedef unsigned int (*operation)(double *, double *, unsigned int);
extern unsigned int reuse_same(double *, double *, unsigned int);
extern unsigned int reuse_changed_base(double *, double *, unsigned int);
extern unsigned int reuse_integer_load(double *, double *, unsigned int);
extern unsigned int reuse_join(double *, double *, unsigned int);
extern unsigned int reuse_offset(double *, double *, unsigned int);
extern unsigned int reuse_vector(double *, double *, unsigned int);
extern const char reuse_same_store[];
static void *fault_page;
static LONG write_faults;

/* Only volatile Windows x64 registers are modified. In particular, arithmetic
 * on an XMM register must not destroy the carry flag set before the load. */
#define ENTRY(name) ".p2align 4\n.globl " #name "\n.def " #name "; .scl 2; .type 32; .endef\n" #name ":\nstc\n"
#define RETURN "setc %al\nmovzbl %al,%eax\nret\n"
__asm__(".text\n"
    ENTRY(reuse_same)
    "movsd (%rcx),%xmm0\naddsd %xmm0,%xmm0\n"
    ".globl reuse_same_store\nreuse_same_store:\nmovsd %xmm0,(%rcx)\n" RETURN
    ENTRY(reuse_changed_base)
    "movsd (%rcx),%xmm0\naddsd %xmm0,%xmm0\nlea 8(%rcx),%rcx\nmovsd %xmm0,(%rcx)\n" RETURN
    ENTRY(reuse_integer_load)
    "movsd (%rcx),%xmm0\naddsd %xmm0,%xmm0\nmov (%rdx),%r9\nmovsd %xmm0,(%rcx)\n" RETURN
    ENTRY(reuse_join)
    "movsd (%rcx),%xmm0\naddsd %xmm0,%xmm0\ntest %r8d,%r8d\njz 1f\n"
    "movsd (%rdx),%xmm1\njmp 1f\n1:\nstc\nmovsd %xmm0,(%rcx)\n" RETURN
    ENTRY(reuse_offset)
    "movsd 8(%rcx),%xmm0\naddsd %xmm0,%xmm0\nmovsd %xmm0,(%rcx)\n" RETURN
    ENTRY(reuse_vector)
    "movupd (%rcx),%xmm0\naddpd %xmm0,%xmm0\nmovupd %xmm0,(%rcx)\n" RETURN);

static int test_page(double *page)
{
    static const operation operations[] = {reuse_same, reuse_changed_base,
        reuse_integer_load, reuse_join, reuse_offset, reuse_vector};
    unsigned int repeat, index, branch;

    for (repeat = 0; repeat < 128; ++repeat)
        for (index = 0; index < sizeof(operations) / sizeof(operations[0]); ++index)
            for (branch = 0; branch < 2; ++branch)
            {
                double expected0 = index == 1 ? 3 : index == 4 ? 10 : 6;
                double expected1 = index == 1 ? 6 : index == 5 ? 10 : 5;

                page[0] = 3;
                page[1] = 5;
                page[2] = 17;
                page[3] = 19;
                if (operations[index](page, page + 2, branch) != 1 ||
                    page[0] != expected0 || page[1] != expected1 ||
                    page[2] != 17 || page[3] != 19)
                    return 0;
            }
    return 1;
}

static DWORD WINAPI worker(void *opaque)
{
    uintptr_t index = (uintptr_t)opaque;
    void *requested = (void *)(UINT64_C(0x20000000) + index * 0x10000);
    double *low = VirtualAlloc(requested, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    double *high = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD result = 1;

    if (low == requested && high && (uintptr_t)high >= UINT64_C(0x100000000) &&
        test_page(low) && test_page(high))
        result = 0;
    if (low && !VirtualFree(low, 0, MEM_RELEASE)) result = 1;
    if (high && !VirtualFree(high, 0, MEM_RELEASE)) result = 1;
    return result;
}

static LONG CALLBACK write_fault_handler(EXCEPTION_POINTERS *pointers)
{
    EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    CONTEXT *context = pointers->ContextRecord;
    DWORD old_protect;

    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->NumberParameters != 2 || record->ExceptionInformation[0] != 1 ||
        record->ExceptionInformation[1] != (ULONG_PTR)fault_page ||
        record->ExceptionAddress != reuse_same_store || context->Rip != (ULONG_PTR)reuse_same_store ||
        context->Rcx != (ULONG_PTR)fault_page || !(context->EFlags & 1) ||
        context->Xmm0.Low != UINT64_C(0x4018000000000000) ||
        !VirtualProtect(fault_page, 4096, PAGE_READWRITE, &old_protect))
        return EXCEPTION_CONTINUE_SEARCH;
    ++write_faults;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int test_write_fault(void *requested)
{
    double *page = VirtualAlloc(requested, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old_protect;
    int result = 0;

    if (!page) return 0;
    if ((requested && page != requested) || (!requested && (uintptr_t)page <= UINT32_MAX))
        goto cleanup;
    page[0] = 3;
    fault_page = page;
    write_faults = 0;
    if (!VirtualProtect(page, 4096, PAGE_READONLY, &old_protect)) goto cleanup;
    /* The successful load and FADD precede the faulting, reused-address store.
     * Resume the same guest RIP and require the computed XMM value and flags. */
    if (reuse_same(page, page, 0) == 1 && page[0] == 6 && write_faults == 1) result = 1;
cleanup:
    fault_page = NULL;
    if (!VirtualFree(page, 0, MEM_RELEASE)) result = 0;
    return result;
}

int main(void)
{
    HANDLE threads[8];
    unsigned int i, count = 0;
    int failed = 0;

    for (i = 0; i < 8; ++i)
    {
        threads[count] = CreateThread(NULL, 0, worker, (void *)(uintptr_t)i, 0, NULL);
        if (!threads[count])
        {
            failed = 1;
            break;
        }
        ++count;
    }
    if (count && WaitForMultipleObjects(count, threads, TRUE, 30000) != WAIT_OBJECT_0)
        failed = 1;
    for (i = 0; i < count; ++i)
    {
        DWORD code;
        if (!GetExitCodeThread(threads[i], &code) || code) failed = 1;
        if (!CloseHandle(threads[i])) failed = 1;
    }
    if (failed) return 1;
    {
        void *handler = AddVectoredExceptionHandler(1, write_fault_handler);
        if (!handler) return 1;
        if (!test_write_fault((void *)(uintptr_t)0x21000000) || !test_write_fault(NULL)) failed = 1;
        if (!RemoveVectoredExceptionHandler(handler)) failed = 1;
    }
    if (failed) return 1;
    puts("FEX_ADDRESS_REUSE_PASS low high flags base-change integer-load join offset vector write-fault threads=8");
    return 0;
}
