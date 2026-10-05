/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* ARM64EC-only, intrusive fixture on its own idle provider stack. No timing claim. */
#if 0
#pragma makedep standalone
#endif
#include <windows.h>
#include <winternl.h>

struct state_prefix { UINT64 magic, allocation_size, control_stack_top; };
typedef DWORD (WINAPI *x64_leaf)(void);

static void number(const char *label, DWORD length, SIZE_T value)
{
    char digits[32];
    DWORD written, offset = sizeof(digits);
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    digits[--offset] = '\n';
    do { digits[--offset] = '0' + value % 10; value /= 10; } while (value);
    if (!WriteFile(error, label, length, &written, NULL) || written != length ||
        !WriteFile(error, digits + offset, sizeof(digits) - offset, &written, NULL) ||
        written != sizeof(digits) - offset) ExitProcess(30);
}

static SIZE_T emit(BYTE *code, unsigned int kind, DWORD *expected)
{
    SIZE_T n = 0;
    unsigned int i;
    code[n++] = 0x31; code[n++] = 0xc0; /* xor eax,eax */
    if (kind == 0)
    {
        for (i = 0; i < 16384; ++i)
        { code[n++] = 0x83; code[n++] = 0xc0; code[n++] = 1; }
        *expected = 16384;
    }
    else if (kind == 1)
    {
        code[n++] = 0xb9; code[n++] = 1; code[n++] = 0; code[n++] = 0; code[n++] = 0;
        for (i = 0; i < 4096; ++i)
        {
            code[n++] = 0x85; code[n++] = 0xc9; /* test ecx,ecx */
            code[n++] = 0x74; code[n++] = 3;    /* jz skips add */
            code[n++] = 0x83; code[n++] = 0xc0; code[n++] = 1;
        }
        *expected = 4096;
    }
    else if (kind == 2)
    {
        code[n++] = 0x53; /* preserve nonvolatile RBX across CPUID */
        for (i = 0; i < 512; ++i)
        { code[n++] = 0x31; code[n++] = 0xc0; code[n++] = 0x0f; code[n++] = 0xa2; }
        code[n++] = 0x5b;
        code[n++] = 0xb8; code[n++] = 17; code[n++] = 0; code[n++] = 0; code[n++] = 0;
        *expected = 17;
    }
    else
    {
        code[n++] = 0x66; code[n++] = 0x0f; code[n++] = 0xef; code[n++] = 0xc0; /* pxor */
        code[n++] = 0x66; code[n++] = 0x0f; code[n++] = 0x76; code[n++] = 0xc9; /* pcmpeqd */
        for (i = 0; i < 8192; ++i)
        { code[n++] = 0x66; code[n++] = 0x0f; code[n++] = 0xd4; code[n++] = 0xc1; }
        code[n++] = 0x66; code[n++] = 0x0f; code[n++] = 0x7e; code[n++] = 0xc0; /* movd eax,xmm0 */
        *expected = (DWORD)-8192;
    }
    code[n++] = 0xc3;
    return n;
}

void mainCRTStartup(void)
{
    static const char success[] = "CONTROL_STACK_STRESS_PASS scalar=16384 branches=4096 cpuid=512 simd=8192 cold-rounds=3\n";
    CHPE_V2_CPU_AREA_INFO *cpu = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    struct state_prefix *state, original;
    MEMORY_BASIC_INFORMATION info;
    BYTE *code;
    volatile BYTE *low;
    SIZE_T page, capacity, i, used, peak = 0, size;
    ULONG_PTR sp;
    DWORD protect, expected, written;
    unsigned int kind, round;
    x64_leaf function;

    if (!cpu || cpu->InSimulation || !(state = cpu->EmulatorData[0])) ExitProcess(10);
    original = *state;
    if (original.magic != 0x363454494a415458ull || original.allocation_size != 0x40000 ||
        (ULONG_PTR)state > ~(ULONG_PTR)0 - original.allocation_size ||
        !VirtualQuery(state, &info, sizeof(info)) || info.AllocationBase != state) ExitProcess(11);
    page = info.RegionSize;
    if (page < 4096 || page > 65536 || (page & (page - 1))) ExitProcess(12);
    low = (BYTE *)state + 2 * page;
    if (original.control_stack_top <= (ULONG_PTR)low ||
        original.control_stack_top > (ULONG_PTR)state + original.allocation_size) ExitProcess(13);
    capacity = original.control_stack_top - (ULONG_PTR)low;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if (sp >= (ULONG_PTR)state && sp < (ULONG_PTR)state + original.allocation_size) ExitProcess(14);
    if (!(code = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) ExitProcess(15);
    _Static_assert(sizeof(function) == sizeof(code), "Windows code pointer size");
    __builtin_memcpy(&function, &code, sizeof(function));
    for (round = 0; round < 3; ++round)
    for (kind = 0; kind < 4; ++kind)
    {
        if (!VirtualProtect(code, 0x10000, PAGE_READWRITE, &protect)) ExitProcess(16);
        size = emit(code, kind, &expected);
        if (size > 0x10000 || !VirtualProtect(code, 0x10000, PAGE_EXECUTE_READ, &protect) ||
            !FlushInstructionCache(GetCurrentProcess(), code, size)) ExitProcess(17);
        if (cpu->InSimulation) ExitProcess(18);
        /* Every native activation has ended. Only this fixture's now-idle
         * control stack is overwritten; metadata and recorder stay untouched. */
        for (i = 0; i < capacity; ++i) low[i] = 0xa5;
        if (function() != expected || cpu->InSimulation) ExitProcess(19);
        for (i = 0; i < capacity && low[i] == 0xa5; ++i);
        used = capacity - i;
        if (!used || i < page || state->magic != original.magic ||
            state->allocation_size != original.allocation_size ||
            state->control_stack_top != original.control_stack_top) ExitProcess(20);
        if (used > peak) peak = used;
    }
    if (!VirtualFree(code, 0, MEM_RELEASE)) ExitProcess(21);
#define NUMBER(label, value) number(label, sizeof(label) - 1, value)
    NUMBER("CONTROL_STACK_STRESS_V1 usable_bytes=", capacity);
    NUMBER("CONTROL_STACK_STRESS_V1 touched_high_water_bytes=", peak);
#undef NUMBER
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), success, sizeof(success) - 1, &written, NULL) ||
        written != sizeof(success) - 1) ExitProcess(30);
    ExitProcess(0);
}
