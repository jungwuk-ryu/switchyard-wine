/* Real AMD64 PE exception boundary regression (no provider-private calls). */
#if 0
#pragma makedep standalone
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SOFTWARE_EXCEPTION 0xe0426401u
#define REGISTER_VALUE UINT64_C(0x13572468abcdef09)
#define RESUMED_VALUE UINT64_C(0x24681357fedcba90)

volatile uintptr_t exception_expected_rsp;
const uint64_t exception_vector[2] = {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)};
static volatile LONG handler_calls, failures;
static DWORD expected_code;
static ULONG_PTR expected_access, expected_address, expected_pc, resume_pc;

extern uint64_t exception_read(void *address);
extern uint64_t exception_write(void *address);
extern uint64_t exception_execute(void *address);
extern const char exception_read_fault[], exception_read_resume[];
extern const char exception_write_fault[], exception_write_resume[];
extern const char exception_execute_resume[];

/* Leaf functions do not modify nonvolatile registers or the stack. Snapshot
 * the actual fault-time RSP, not the caller's approximate frame address. */
__asm__(
    ".text\n.p2align 4\n"
    ".globl exception_read\n.def exception_read; .scl 2; .type 32; .endef\n"
    "exception_read:\n"
    "mov %rsp,exception_expected_rsp(%rip)\n"
    "movabs $0x13572468abcdef09,%rax\n"
    "movdqu exception_vector(%rip),%xmm0\n"
    "stc\n"
    ".globl exception_read_fault\nexception_read_fault:\n"
    "mov (%rcx),%rax\n"
    ".globl exception_read_resume\nexception_read_resume:\nret\n"
    ".p2align 4\n"
    ".globl exception_write\n.def exception_write; .scl 2; .type 32; .endef\n"
    "exception_write:\n"
    "mov %rsp,exception_expected_rsp(%rip)\n"
    "movabs $0x13572468abcdef09,%rax\n"
    "movdqu exception_vector(%rip),%xmm0\n"
    "stc\n"
    ".globl exception_write_fault\nexception_write_fault:\n"
    "mov %rax,(%rcx)\n"
    ".globl exception_write_resume\nexception_write_resume:\nret\n"
    ".p2align 4\n"
    ".globl exception_execute\n.def exception_execute; .scl 2; .type 32; .endef\n"
    "exception_execute:\n"
    "mov %rsp,exception_expected_rsp(%rip)\n"
    "movabs $0x13572468abcdef09,%rax\n"
    "movdqu exception_vector(%rip),%xmm0\n"
    "stc\n"
    "jmp *%rcx\n"
    ".globl exception_execute_resume\nexception_execute_resume:\nret\n");

static LONG CALLBACK exception_handler(EXCEPTION_POINTERS *pointers)
{
    EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    CONTEXT *context = pointers->ContextRecord;

    if (record->ExceptionCode != expected_code) return EXCEPTION_CONTINUE_SEARCH;
    ++handler_calls;
    if (expected_code == SOFTWARE_EXCEPTION)
    {
        if (record->NumberParameters != 1 || record->ExceptionInformation[0] != REGISTER_VALUE)
            ++failures;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (context->Rip != expected_pc) return EXCEPTION_CONTINUE_SEARCH;
    if ((ULONG_PTR)record->ExceptionAddress != expected_pc ||
        record->NumberParameters != 2 || record->ExceptionInformation[0] != expected_access ||
        record->ExceptionInformation[1] != expected_address ||
        context->Rsp != exception_expected_rsp || context->Rcx != expected_address ||
        context->Rax != REGISTER_VALUE || !(context->EFlags & 1) ||
        context->Xmm0.Low != exception_vector[0] ||
        (uint64_t)context->Xmm0.High != exception_vector[1])
        ++failures;
    context->Rip = resume_pc;
    context->Rax = RESUMED_VALUE;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int test_mapping(void *address, const char *name)
{
    uint64_t value;
    LONG previous;

    expected_code = EXCEPTION_ACCESS_VIOLATION;
    expected_address = (ULONG_PTR)address;
    expected_access = 0;
    expected_pc = (ULONG_PTR)exception_read_fault;
    resume_pc = (ULONG_PTR)exception_read_resume;
    previous = handler_calls;
    value = exception_read(address);
    if (value != RESUMED_VALUE || handler_calls != previous + 1 || failures) return 0;
    printf("EXCEPTION_PE_PASS %s read\n", name);
    fflush(stdout);

    expected_access = 1;
    expected_pc = (ULONG_PTR)exception_write_fault;
    resume_pc = (ULONG_PTR)exception_write_resume;
    previous = handler_calls;
    value = exception_write(address);
    if (value != RESUMED_VALUE || handler_calls != previous + 1 || failures) return 0;
    printf("EXCEPTION_PE_PASS %s write\n", name);
    fflush(stdout);
    return 1;
}

static int test_execute(void *address, const char *name)
{
    LONG previous = handler_calls;

    expected_code = EXCEPTION_ACCESS_VIOLATION;
    expected_address = expected_pc = (ULONG_PTR)address;
    expected_access = 8;
    resume_pc = (ULONG_PTR)exception_execute_resume;
    if (exception_execute(address) != RESUMED_VALUE || failures || handler_calls != previous + 1)
        return 0;
    printf("EXCEPTION_PE_PASS %s execute denied\n", name);
    fflush(stdout);
    return 1;
}

int main(int argc, char **argv)
{
    const ULONG_PTR argument = REGISTER_VALUE;
    void *handler = NULL, *low = NULL, *high = NULL;
    int status = 1;

    handler = AddVectoredExceptionHandler(1, exception_handler);
    if (!handler) goto cleanup;
    expected_code = SOFTWARE_EXCEPTION;
    RaiseException(SOFTWARE_EXCEPTION, 0, 1, &argument);
    if (handler_calls != 1 || failures) goto cleanup;
    puts("EXCEPTION_PE_PASS software");
    fflush(stdout);

    low = VirtualAlloc((void *)(uintptr_t)0x21000000, 0x10000,
                       MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    high = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (low != (void *)(uintptr_t)0x21000000 || !high || (uintptr_t)high <= UINT32_MAX)
        goto cleanup;
    if (argc == 2 && !strcmp(argv[1], "execute"))
    {
        DWORD old_protect;
        LONG previous;

        if (!test_execute(low, "low") || !test_execute(high, "high")) goto cleanup;
        if (!VirtualProtect(high, 0x10000, PAGE_READWRITE, &old_protect)) goto cleanup;
        *(unsigned char *)high = 0xc3; /* ret */
        if (!FlushInstructionCache(GetCurrentProcess(), high, 1) ||
            !test_execute(high, "read-write")) goto cleanup;
        if (!VirtualProtect(high, 0x10000, PAGE_EXECUTE_READ, &old_protect)) goto cleanup;
        previous = handler_calls;
        if (exception_execute(high) != REGISTER_VALUE || failures || handler_calls != previous)
            goto cleanup;
        puts("EXCEPTION_PE_PASS execute permission granted");
        fflush(stdout);
        if (!VirtualProtect(high, 0x10000, PAGE_READONLY, &old_protect) ||
            !test_execute(high, "revoked")) goto cleanup;
        status = 0;
        goto cleanup;
    }
    if (argc != 1) goto cleanup;
    if (!test_mapping(low, "low") || !test_mapping(high, "high")) goto cleanup;
    if (handler_calls != 5 || failures) goto cleanup;
    status = 0;

cleanup:
    if (high && !VirtualFree(high, 0, MEM_RELEASE)) status = 1;
    if (low && !VirtualFree(low, 0, MEM_RELEASE)) status = 1;
    if (handler && !RemoveVectoredExceptionHandler(handler)) status = 1;
    if (status) fprintf(stderr, "EXCEPTION_PE_FAIL calls=%ld failures=%ld last_error=%lu\n",
                        handler_calls, failures, GetLastError());
    return status;
}
