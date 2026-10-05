/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real x64 Windows APIs only: suspend/context edits, native calls and unwind. */
#if 0
#pragma makedep standalone
#endif
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CYCLES 16
#define INITIAL_TOKEN UINT64_C(0x5566778899aabbcc)

volatile LONG suspend_ready, suspend_command, suspend_callbacks;
volatile DWORD suspend_callback_tid;
volatile DWORD64 suspend_expected_rsp, suspend_final_token;
M128A suspend_final_vector;
const M128A suspend_vectors[10] = {
    {0x0123456789abc006, 0x1122334455660006}, {0x0123456789abc007, 0x1122334455660007},
    {0x0123456789abc008, 0x1122334455660008}, {0x0123456789abc009, 0x1122334455660009},
    {0x0123456789abc00a, 0x112233445566000a}, {0x0123456789abc00b, 0x112233445566000b},
    {0x0123456789abc00c, 0x112233445566000c}, {0x0123456789abc00d, 0x112233445566000d},
    {0x0123456789abc00e, 0x112233445566000e}, {0x0123456789abc00f, 0x112233445566000f}
};

extern void suspend_body(void);
extern const char suspend_loop_begin[], suspend_loop_end[];

/* Frame = eight nonvolatile GPRs + 200 bytes (shadow32, XMM160, MXCSR8).
 * SEH metadata deliberately describes every saved nonvolatile register. */
__asm__(
    ".text\n.p2align 4\n.globl suspend_body\n"
    ".def suspend_body; .scl 2; .type 32; .endef\n.seh_proc suspend_body\n"
    "suspend_body:\n"
    "push %rbx\n.seh_pushreg %rbx\npush %rbp\n.seh_pushreg %rbp\n"
    "push %rsi\n.seh_pushreg %rsi\npush %rdi\n.seh_pushreg %rdi\n"
    "push %r12\n.seh_pushreg %r12\npush %r13\n.seh_pushreg %r13\n"
    "push %r14\n.seh_pushreg %r14\npush %r15\n.seh_pushreg %r15\n"
    "sub $200,%rsp\n.seh_stackalloc 200\n"
    "movdqu %xmm6,32(%rsp)\n.seh_savexmm %xmm6,32\n"
    "movdqu %xmm7,48(%rsp)\n.seh_savexmm %xmm7,48\n"
    "movdqu %xmm8,64(%rsp)\n.seh_savexmm %xmm8,64\n"
    "movdqu %xmm9,80(%rsp)\n.seh_savexmm %xmm9,80\n"
    "movdqu %xmm10,96(%rsp)\n.seh_savexmm %xmm10,96\n"
    "movdqu %xmm11,112(%rsp)\n.seh_savexmm %xmm11,112\n"
    "movdqu %xmm12,128(%rsp)\n.seh_savexmm %xmm12,128\n"
    "movdqu %xmm13,144(%rsp)\n.seh_savexmm %xmm13,144\n"
    "movdqu %xmm14,160(%rsp)\n.seh_savexmm %xmm14,160\n"
    "movdqu %xmm15,176(%rsp)\n.seh_savexmm %xmm15,176\n.seh_endprologue\n"
    "stmxcsr 192(%rsp)\nmovl $0x1f80,196(%rsp)\nldmxcsr 196(%rsp)\n"
    "movabs $0x1122334455667788,%rbx\nmovabs $0x2233445566778899,%rbp\n"
    "movabs $0x33445566778899aa,%rsi\nmovabs $0x445566778899aabb,%rdi\n"
    "movabs $0x5566778899aabbcc,%r12\nmovabs $0x66778899aabbccdd,%r13\n"
    "movabs $0x778899aabbccddee,%r14\nmovabs $0x1234567890abcdef,%r15\n"
    "movdqu suspend_vectors(%rip),%xmm6\nmovdqu suspend_vectors+16(%rip),%xmm7\n"
    "movdqu suspend_vectors+32(%rip),%xmm8\nmovdqu suspend_vectors+48(%rip),%xmm9\n"
    "movdqu suspend_vectors+64(%rip),%xmm10\nmovdqu suspend_vectors+80(%rip),%xmm11\n"
    "movdqu suspend_vectors+96(%rip),%xmm12\nmovdqu suspend_vectors+112(%rip),%xmm13\n"
    "movdqu suspend_vectors+128(%rip),%xmm14\nmovdqu suspend_vectors+144(%rip),%xmm15\n"
    "mov %rsp,suspend_expected_rsp(%rip)\nmov $1,%eax\n"
    "lock xchgl %eax,suspend_ready(%rip)\n"
    ".globl suspend_loop_begin\nsuspend_loop_begin:\n"
    "mov suspend_command(%rip),%eax\ncmp $1,%eax\nje 1f\n"
    "cmp $2,%eax\nje 2f\njmp suspend_loop_begin\n"
    "1: call *__imp_GetCurrentThreadId(%rip)\n"
    "mov %eax,suspend_callback_tid(%rip)\n"
    "movl $0,suspend_command(%rip)\nlock incl suspend_callbacks(%rip)\n"
    "jmp suspend_loop_begin\n"
    ".globl suspend_loop_end\nsuspend_loop_end:\n2:\n"
    "mov %r12,suspend_final_token(%rip)\nmovdqu %xmm8,suspend_final_vector(%rip)\n"
    "ldmxcsr 192(%rsp)\n"
    "movdqu 32(%rsp),%xmm6\nmovdqu 48(%rsp),%xmm7\nmovdqu 64(%rsp),%xmm8\n"
    "movdqu 80(%rsp),%xmm9\nmovdqu 96(%rsp),%xmm10\nmovdqu 112(%rsp),%xmm11\n"
    "movdqu 128(%rsp),%xmm12\nmovdqu 144(%rsp),%xmm13\n"
    "movdqu 160(%rsp),%xmm14\nmovdqu 176(%rsp),%xmm15\n"
    "add $200,%rsp\npop %r15\npop %r14\npop %r13\npop %r12\n"
    "pop %rdi\npop %rsi\npop %rbp\npop %rbx\nret\n.seh_endproc\n");

static void fail(const char *where, unsigned int cycle)
{
    fprintf(stderr, "SUSPEND_CONTEXT_FAIL %s cycle=%u error=%lu\n", where, cycle, GetLastError());
    /* Never free shared test state or close a handle under a suspended worker. */
    ExitProcess(1);
}

static void wait_value(volatile LONG *value, LONG expected, const char *where, unsigned int cycle)
{
    ULONGLONG start = GetTickCount64();

    while (InterlockedCompareExchange(value, 0, 0) != expected)
    {
        if (GetTickCount64() - start > 5000) fail(where, cycle);
        Sleep(1); /* Bounded test rendezvous, not a runtime retry. */
    }
}

static DWORD WINAPI worker(void *arg)
{
    (void)arg;
    suspend_body();
    return 0;
}

static void check_context(const CONTEXT *ctx, DWORD64 token, const M128A *vector,
                          DWORD mxcsr, unsigned int cycle)
{
    M128A xmm[10];
    CONTEXT unwind;
    PRUNTIME_FUNCTION entry;
    DWORD64 image_base, frame, return_pc;
    void *handler_data = NULL;
    SIZE_T copied;

    if (ctx->Rip < (uintptr_t)suspend_loop_begin || ctx->Rip >= (uintptr_t)suspend_loop_end ||
        ctx->Rsp != suspend_expected_rsp) fail("guest pc/sp", cycle);
    if (ctx->Rbx != UINT64_C(0x1122334455667788) || ctx->Rbp != UINT64_C(0x2233445566778899) ||
        ctx->Rsi != UINT64_C(0x33445566778899aa) || ctx->Rdi != UINT64_C(0x445566778899aabb) ||
        ctx->R12 != token || ctx->R13 != UINT64_C(0x66778899aabbccdd) ||
        ctx->R14 != UINT64_C(0x778899aabbccddee) || ctx->R15 != UINT64_C(0x1234567890abcdef))
        fail("nonvolatile GPR", cycle);
    memcpy(xmm, suspend_vectors, sizeof(xmm));
    xmm[2] = *vector;
    if (memcmp(&ctx->Xmm6, xmm, sizeof(xmm)) || ctx->MxCsr != mxcsr || ctx->FltSave.MxCsr != mxcsr)
        fail("nonvolatile SIMD/MXCSR", cycle);
    if (!(entry = RtlLookupFunctionEntry(ctx->Rip, &image_base, NULL))) fail("unwind entry", cycle);
    if (!ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)(ctx->Rsp + 264),
                           &return_pc, sizeof(return_pc), &copied) || copied != sizeof(return_pc))
        fail("saved return address", cycle);
    unwind = *ctx;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx->Rip, entry, &unwind, &handler_data, &frame, NULL);
    if (unwind.Rsp != ctx->Rsp + 272 || unwind.Rip != return_pc)
        fail("virtual unwind caller", cycle);
}

int main(void)
{
    HANDLE thread;
    CONTEXT context;
    DWORD thread_id, exit_code, mxcsr = 0x1f80;
    DWORD64 token = INITIAL_TOKEN;
    M128A vector = suspend_vectors[2];
    unsigned int cycle;

    thread = CreateThread(NULL, 0, worker, NULL, 0, &thread_id);
    if (!thread) fail("create", 0);
    wait_value(&suspend_ready, 1, "ready", 0);
    for (cycle = 0; cycle < CYCLES; ++cycle)
    {
        if (SuspendThread(thread) != 0 || SuspendThread(thread) != 1) fail("nested suspend", cycle);
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(thread, &context)) fail("get context", cycle);
        check_context(&context, token, &vector, mxcsr, cycle);
        token += 17;
        vector.Low += 19;
        vector.High += 23;
        mxcsr = 0x1f80 | ((cycle & 3) << 13);
        context.R12 = token;
        context.Xmm8 = vector;
        context.MxCsr = context.FltSave.MxCsr = mxcsr;
        if (!SetThreadContext(thread, &context)) fail("set context", cycle);
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(thread, &context)) fail("get edited context", cycle);
        check_context(&context, token, &vector, mxcsr, cycle);
        if (ResumeThread(thread) != 2) fail("nested resume", cycle);
        if (InterlockedCompareExchange(&suspend_callbacks, 0, 0) != (LONG)cycle)
            fail("suspended callback advanced", cycle);
        InterlockedExchange(&suspend_command, 1);
        if (ResumeThread(thread) != 1) fail("resume", cycle);
        wait_value(&suspend_callbacks, cycle + 1, "native callback", cycle);
        if (suspend_callback_tid != thread_id) fail("native TEB identity", cycle);
    }
    InterlockedExchange(&suspend_command, 2);
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code) fail("join", CYCLES);
    if (suspend_final_token != token || memcmp(&suspend_final_vector, &vector, sizeof(vector)))
        fail("final state", CYCLES);
    if (!CloseHandle(thread)) fail("close", CYCLES);
    puts("SUSPEND_CONTEXT_PASS cycles=16 nested context-edit gpr simd mxcsr native-call unwind");
    return 0;
}
