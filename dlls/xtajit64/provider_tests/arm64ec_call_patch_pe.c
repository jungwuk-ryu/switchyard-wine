/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* CRT-free ARM64EC caller: live x64 FFS patches must not be bypassed by
 * native-target metadata or cached translations. Only this process's own
 * exported thunks are modified. Dynamic private-code reprotection is a
 * separate contract in private_code_reprotect_pe.c.
 */
#include <windows.h>

typedef DWORD (WINAPI *leaf_function)(void);

#ifdef TEST_METADATA_NO_CODE_CHECK
/* Deliberately incorrect test-only checker. A negative-control build must
 * fail at the first patched call (exit 21), proving that address metadata
 * without live-code validation cannot satisfy this regression. */
void *metadata_source, *metadata_target, *metadata_previous_checker;
extern void *__os_arm64x_check_icall;

static void __attribute__((naked)) metadata_check(void)
{
    __asm__("adrp x16, metadata_source\n\t"
            "ldr x16, [x16, #:lo12:metadata_source]\n\t"
            "cmp x11, x16\n\t"
            "b.ne 1f\n\t"
            "adrp x16, metadata_target\n\t"
            "ldr x11, [x16, #:lo12:metadata_target]\n\t"
            "ret\n"
            "1:\n\t"
            "adrp x16, metadata_previous_checker\n\t"
            "ldr x16, [x16, #:lo12:metadata_previous_checker]\n\t"
            "br x16");
}
#endif

__declspec(dllexport) __declspec(noinline) DWORD WINAPI patch_leaf(void)
{
    return 0x12345678;
}

__declspec(dllexport) __declspec(noinline) DWORD WINAPI patch_other(void)
{
    return 0x87654321;
}

static leaf_function leaf_from_address(const void *address)
{
    leaf_function function;

    _Static_assert(sizeof(function) == sizeof(address), "Windows code pointer size");
    __builtin_memcpy(&function, &address, sizeof(function));
    return function;
}

static BYTE *export_address(const char *name)
{
    FARPROC function = GetProcAddress(GetModuleHandleW(NULL), name);
    BYTE *address;

    _Static_assert(sizeof(function) == sizeof(address), "Windows export pointer size");
    __builtin_memcpy(&address, &function, sizeof(address));
    if (!address) ExitProcess(10);
    return address;
}

static BOOL equal_bytes(const BYTE *left, const BYTE *right, SIZE_T count)
{
    SIZE_T i;

    for (i = 0; i < count; ++i)
        if (left[i] != right[i]) return FALSE;
    return TRUE;
}

static void report_protect_failure(BYTE *address)
{
    static const char digits[] = "0123456789abcdef";
    char message[] = "CALL_PATCH_PROTECT error=00000000 protect=00000000 region=00000000\n";
    MEMORY_BASIC_INFORMATION info = {0};
    DWORD values[3] = {GetLastError(), 0, 0}, written;
    static const unsigned int offsets[] = {25, 42, 58};
    unsigned int value, digit;

    if (VirtualQuery(address, &info, sizeof(info)) == sizeof(info))
    {
        values[1] = info.Protect;
        values[2] = info.RegionSize;
    }
    for (value = 0; value < 3; ++value)
        for (digit = 0; digit < 8; ++digit)
            message[offsets[value] + digit] = digits[(values[value] >> (28 - 4 * digit)) & 15];
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, sizeof(message) - 1, &written, NULL);
}

static void replace_code(BYTE *address, const BYTE *bytes, SIZE_T count)
{
    DWORD old_protect, unused;
    SIZE_T i;

    if (!VirtualProtect(address, count, PAGE_READWRITE, &old_protect))
    {
        report_protect_failure(address);
        ExitProcess(11);
    }
    for (i = 0; i < count; ++i) address[i] = bytes[i];
    if (!VirtualProtect(address, count, old_protect, &unused) ||
        !FlushInstructionCache(GetCurrentProcess(), address, count)) ExitProcess(12);
}

static void check_call(leaf_function function, DWORD expected, DWORD failure)
{
    const void *before, *after;
    unsigned int i;

    __asm__ volatile("mov %0, x18" : "=r" (before));
    SetLastError(0x24681357);
    for (i = 0; i < 32; ++i)
        if (function() != expected || GetLastError() != 0x24681357) ExitProcess(failure);
    __asm__ volatile("mov %0, x18" : "=r" (after));
    if (after != before) ExitProcess(13);
}

void mainCRTStartup(void)
{
    static const BYTE ffwd_prefix[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x5d, 0xe9};
    static const BYTE patched_a[] = {0xb8, 0x44, 0x33, 0x22, 0x11, 0xc3};
    static const BYTE patched_b[] = {0xb8, 0x88, 0x77, 0x66, 0x55, 0xc3};
    static const char success[] = "ARM64EC_CALL_PATCH_PASS ffs patch repatch restore retarget x18 last-error\n";
    BYTE original[16], redirected[16], *address = export_address("patch_leaf");
    BYTE *other = export_address("patch_other");
    leaf_function function = leaf_from_address(address);
    LONG offset, other_offset;
    LONGLONG destination, displacement;
    DWORD written;
    SIZE_T i;

    if (!equal_bytes(address, ffwd_prefix, sizeof(ffwd_prefix)) ||
        !equal_bytes(other, ffwd_prefix, sizeof(ffwd_prefix))) ExitProcess(14);
    for (i = 0; i < sizeof(original); ++i) original[i] = redirected[i] = address[i];
#ifdef TEST_METADATA_NO_CODE_CHECK
    __builtin_memcpy(&offset, address + sizeof(ffwd_prefix), sizeof(offset));
    metadata_source = address;
    metadata_target = (void *)((ULONG_PTR)address + 14 + (LONG_PTR)offset);
    metadata_previous_checker = __os_arm64x_check_icall;
    __os_arm64x_check_icall = metadata_check;
#endif
    check_call(function, 0x12345678, 20);

    replace_code(address, patched_a, sizeof(patched_a));
    if (export_address("patch_leaf") != address) ExitProcess(15);
    check_call(function, 0x11223344, 21);
    replace_code(address, patched_b, sizeof(patched_b));
    check_call(function, 0x55667788, 22);
    replace_code(address, original, sizeof(original));
    check_call(function, 0x12345678, 23);

    /* Changing only the relative branch must not reuse its former native
     * destination even though the canonical FFS prefix remains unchanged. */
    __builtin_memcpy(&other_offset, other + sizeof(ffwd_prefix), sizeof(other_offset));
    destination = (LONGLONG)(ULONG_PTR)other + 14 + other_offset;
    displacement = destination - ((LONGLONG)(ULONG_PTR)address + 14);
    if (displacement < (-2147483647LL - 1) || displacement > 2147483647LL) ExitProcess(16);
    offset = (LONG)displacement;
    __builtin_memcpy(redirected + sizeof(ffwd_prefix), &offset, sizeof(offset));
    replace_code(address, redirected, sizeof(redirected));
    check_call(function, 0x87654321, 24);
    replace_code(address, original, sizeof(original));
    check_call(function, 0x12345678, 25);

    if (!equal_bytes(address, original, sizeof(original))) ExitProcess(32);
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), success, sizeof(success) - 1, &written, NULL) ||
        written != sizeof(success) - 1) ExitProcess(33);
    ExitProcess(0);
}
