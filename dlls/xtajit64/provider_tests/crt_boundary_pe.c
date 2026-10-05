/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* An identical x64 PE boundary discriminator, not a whole-program score.
 * No CRT is linked: the selected UCRT functions are resolved through their
 * real prototypes. Internal kernels have separate buffers and no overlap.
 */
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <excpt.h>

#define CAPACITY 8192
#define MAX_LENGTH 4096
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#endif

typedef void *(__cdecl *copy_fn)(void *, const void *, SIZE_T);
typedef void *(__cdecl *set_fn)(void *, int, SIZE_T);
typedef int (__cdecl *compare_fn)(const void *, const void *, SIZE_T);

static copy_fn copies[2];
static set_fn sets[2];
static compare_fn compares[2];
static unsigned char source[CAPACITY], destination[CAPACITY];
static unsigned char *guard_source, *guard_destination;
static SIZE_T guard_unit;
static unsigned int fault_count;
static ULONG_PTR expected_fault;
static LARGE_INTEGER frequency;

/* Assembly calls only functions with the three integer-register argument
 * layouts above. FARPROC is an address carrier, never a C calling prototype.
 */
extern int boundary_check_abi(FARPROC address, const void *first, ULONG_PTR second,
                             SIZE_T length, ULONG_PTR *result);
extern void boundary_bad_abi(void);

struct line
{
    char bytes[512];
    DWORD length;
};

static void text(struct line *line, const char *value)
{
    while (*value)
    {
        if (line->length == sizeof(line->bytes)) ExitProcess(70);
        line->bytes[line->length++] = *value++;
    }
}

static void number(struct line *line, ULONGLONG value)
{
    char digits[20];
    unsigned int count = 0;

    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count)
    {
        if (line->length == sizeof(line->bytes)) ExitProcess(70);
        line->bytes[line->length++] = digits[--count];
    }
}

static void emit(struct line *line, DWORD handle)
{
    DWORD written;

    if (!WriteFile(GetStdHandle(handle), line->bytes, line->length, &written, NULL) ||
        written != line->length) ExitProcess(71);
}

static void fail(unsigned int code)
{
    struct line line = {{0}, 0};

    text(&line, "CRT_BOUNDARY_FAIL code=");
    number(&line, code);
    text(&line, "\n");
    emit(&line, STD_ERROR_HANDLE);
    ExitProcess(code);
}

__declspec(noinline) static void *internal_copy(void *dst, const void *src, SIZE_T length)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    SIZE_T index;

    for (index = 0; index < length; ++index) d[index] = s[index];
#ifdef BOUNDARY_NEGATIVE_CANARY
    if (length == 3) d[length] ^= 1;
#endif
    return dst;
}

__declspec(noinline) static void *internal_set(void *dst, int value, SIZE_T length)
{
    unsigned char *d = dst;
    SIZE_T index;

    for (index = 0; index < length; ++index) d[index] = (unsigned char)value;
    return dst;
}

__declspec(noinline) static int internal_compare(const void *first, const void *second, SIZE_T length)
{
    const unsigned char *a = first, *b = second;
    SIZE_T index;

    for (index = 0; index < length; ++index)
        if (a[index] != b[index]) return (int)a[index] - (int)b[index];
    return 0;
}

static int sign(int value)
{
    return (value > 0) - (value < 0);
}

static void reset_buffers(void)
{
    unsigned int index;

    for (index = 0; index < CAPACITY; ++index)
    {
        source[index] = (unsigned char)(index * 13 + 7);
        destination[index] = 0xa5;
    }
}

static void check_memory(unsigned int method)
{
    static const SIZE_T lengths[] = {0,1,2,3,7,15,16,31,32,63,64,65,127,255,256,511,1023,4096};
    static const unsigned int offsets[] = {0,1,7,15,31};
    unsigned int length_index, src_index, dst_index, index, position;
    SIZE_T length, src_offset, dst_offset;

    for (length_index = 0; length_index < ARRAY_SIZE(lengths); ++length_index)
        for (src_index = 0; src_index < ARRAY_SIZE(offsets); ++src_index)
            for (dst_index = 0; dst_index < ARRAY_SIZE(offsets); ++dst_index)
            {
                length = lengths[length_index];
                src_offset = 64 + offsets[src_index];
                dst_offset = 64 + offsets[dst_index];
                reset_buffers();
                if (copies[method](destination + dst_offset, source + src_offset, length) !=
                    destination + dst_offset) fail(20);
                for (index = 0; index < CAPACITY; ++index)
                {
                    unsigned char expected = index >= dst_offset && index - dst_offset < length ?
                        source[src_offset + index - dst_offset] : 0xa5;
                    if (destination[index] != expected || source[index] != (unsigned char)(index * 13 + 7))
                        fail(21);
                }
                if (compares[method](destination + dst_offset, source + src_offset, length)) fail(22);
                for (position = 0; position < 3 && length; ++position)
                {
                    SIZE_T changed = position == 0 ? 0 : position == 1 ? length / 2 : length - 1;
                    unsigned char original = destination[dst_offset + changed];
                    destination[dst_offset + changed] ^= 0x80;
                    if (sign(compares[method](destination + dst_offset, source + src_offset, length)) !=
                        sign((int)destination[dst_offset + changed] - (int)original) ||
                        sign(compares[method](source + src_offset, destination + dst_offset, length)) !=
                        -sign((int)destination[dst_offset + changed] - (int)original)) fail(22);
                    destination[dst_offset + changed] = original;
                }
                if (sets[method](destination + dst_offset, 0x1ad, length) != destination + dst_offset) fail(20);
                for (index = 0; index < CAPACITY; ++index)
                    if (destination[index] != (index >= dst_offset && index - dst_offset < length ? 0xad : 0xa5))
                        fail(21);
            }
}

static FARPROC function_address(const void *representation, SIZE_T size)
{
    FARPROC result;

    if (size != sizeof(result)) fail(10);
    internal_copy(&result, representation, sizeof(result));
    return result;
}

static void check_abi(unsigned int method)
{
    static const SIZE_T lengths[] = {0,1,31,4096};
    ULONG_PTR result;
    unsigned int index;
    FARPROC address;

    for (index = 0; index < ARRAY_SIZE(lengths); ++index)
    {
        reset_buffers();
        address = function_address(&copies[method], sizeof(copies[method]));
        if (boundary_check_abi(address, destination, (ULONG_PTR)source, lengths[index], &result) ||
            result != (ULONG_PTR)destination) fail(23);
        address = function_address(&sets[method], sizeof(sets[method]));
        if (boundary_check_abi(address, destination, 0xad, lengths[index], &result) ||
            result != (ULONG_PTR)destination) fail(23);
        address = function_address(&compares[method], sizeof(compares[method]));
        if (boundary_check_abi(address, source, (ULONG_PTR)source, lengths[index], &result) ||
            (int)(DWORD)result) fail(23);
    }
}

static LONG fault_filter(EXCEPTION_POINTERS *exception, unsigned int write)
{
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;

    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters != 2 ||
        record->ExceptionInformation[0] != write || record->ExceptionInformation[1] < expected_fault ||
        record->ExceptionInformation[1] - expected_fault >= guard_unit) return EXCEPTION_CONTINUE_SEARCH;
    ++fault_count;
    return EXCEPTION_EXECUTE_HANDLER;
}

static unsigned char *guard_allocation(void)
{
    unsigned char *allocation = VirtualAlloc(NULL, guard_unit * 3, MEM_RESERVE, PAGE_NOACCESS);

    if (!allocation || VirtualAlloc(allocation + guard_unit, guard_unit, MEM_COMMIT, PAGE_READWRITE) !=
        allocation + guard_unit) fail(24);
    return allocation;
}

static void check_guard(unsigned int method)
{
    static const SIZE_T lengths[] = {0,1,3,15,16,31,32,63,64,127,256,4096};
    unsigned int index, byte, before;

    for (index = 0; index < ARRAY_SIZE(lengths); ++index)
    {
        SIZE_T length = lengths[index];
        unsigned char *src = guard_source + 2 * guard_unit - length;
        unsigned char *dst = guard_destination + 2 * guard_unit - length;

        for (byte = 0; byte < length; ++byte) src[byte] = (unsigned char)(byte * 13 + 7);
        if (copies[method](dst, src, length) != dst || compares[method](src, dst, length) ||
            sets[method](dst, 0xad, length) != dst) fail(24);
        for (byte = 0; byte < length; ++byte) if (dst[byte] != 0xad) fail(24);
    }
    before = fault_count;
    expected_fault = (ULONG_PTR)(guard_source + 2 * guard_unit);
    __try { copies[method](destination, (void *)expected_fault, 16); }
    __except(fault_filter(GetExceptionInformation(), 0)) {}
    expected_fault = (ULONG_PTR)(guard_destination + 2 * guard_unit);
    __try { sets[method]((void *)expected_fault, 0xad, 16); }
    __except(fault_filter(GetExceptionInformation(), 1)) {}
    expected_fault = (ULONG_PTR)(guard_source + 2 * guard_unit);
    __try { compares[method](source, (void *)expected_fault, 16); }
    __except(fault_filter(GetExceptionInformation(), 0)) {}
    if (fault_count - before != 3) fail(25);
}

__declspec(noinline) static void kernel(unsigned int kind, unsigned int method, SIZE_T length,
                                       unsigned int iterations)
{
    unsigned int iteration;

    for (iteration = 0; iteration < iterations; ++iteration)
        if (kind == 0) copies[method](destination + 64, source + 64, length);
        else if (kind == 1) sets[method](destination + 64, 0xad, length);
        else if (compares[method](source + 64, source + 64 + 4096, length)) fail(22);
}

static void measure(unsigned int kind, unsigned int method, SIZE_T length)
{
    static const char *names[] = {"memcpy", "memset", "memcmp"};
    unsigned int iterations = length >= 1024 ? 50000 : 250000, index;
    LARGE_INTEGER begin, end;
    struct line line = {{0}, 0};

    reset_buffers();
    kernel(kind, method, length, 1024); /* Same call/length warms the measured code. */
    if (!QueryPerformanceCounter(&begin)) fail(30);
    kernel(kind, method, length, iterations);
    if (!QueryPerformanceCounter(&end) || end.QuadPart <= begin.QuadPart) fail(30);
    for (index = 0; index < CAPACITY; ++index)
    {
        unsigned char expected = 0xa5;
        if (index >= 64 && index - 64 < length && kind != 2)
            expected = kind == 0 ? source[index] : 0xad;
        if (destination[index] != expected || source[index] != (unsigned char)(index * 13 + 7)) fail(21);
    }
    text(&line, "CRT_BOUNDARY_PHASE_V1 name="); text(&line, names[kind]);
    text(&line, " method="); text(&line, method ? "imported" : "internal");
    text(&line, " length="); number(&line, length);
    text(&line, " iterations="); number(&line, iterations);
    text(&line, " ticks="); number(&line, end.QuadPart - begin.QuadPart);
    text(&line, " frequency="); number(&line, frequency.QuadPart);
    text(&line, "\n"); emit(&line, STD_ERROR_HANDLE);
}

void mainCRTStartup(void)
{
    static const SIZE_T lengths[] = {0,16,64,1024};
    static const char *names[] = {"memcpy", "memset", "memcmp"};
    FARPROC procedures[3];
    HMODULE module;
    SYSTEM_INFO info;
    unsigned int index, method, kind, length_index, order;
    char reverse[2];
    DWORD environment_length;
    struct line line = {{0}, 0};

    _Static_assert(sizeof(void *) == 8, "This fixture requires x64 Windows PE");
    _Static_assert(sizeof(FARPROC) == sizeof(copy_fn) && sizeof(FARPROC) == sizeof(set_fn) &&
                   sizeof(FARPROC) == sizeof(compare_fn), "Windows function address representation");
    environment_length = GetEnvironmentVariableA("SWITCHYARD_BOUNDARY_REVERSE", reverse, sizeof(reverse));
    if (environment_length != 1 || (reverse[0] != '0' && reverse[0] != '1')) fail(11);
    order = reverse[0] == '1';
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) fail(30);
    module = LoadLibraryA("ucrtbase.dll");
    if (!module) fail(10);
    for (index = 0; index < 3; ++index)
    {
        if (!(procedures[index] = GetProcAddress(module, names[index]))) fail(10);
        line.length = 0;
        text(&line, "CRT_BOUNDARY_TARGET_V1 name="); text(&line, names[index]);
        text(&line, " rva=");
        /* Same-module ownership is validated by the host against exact PE metadata. */
        number(&line, (ULONG_PTR)function_address(&procedures[index], sizeof(procedures[index])) - (ULONG_PTR)module);
        text(&line, "\n"); emit(&line, STD_ERROR_HANDLE);
    }
    internal_copy(&copies[1], &procedures[0], sizeof(copies[1]));
    internal_copy(&sets[1], &procedures[1], sizeof(sets[1]));
    internal_copy(&compares[1], &procedures[2], sizeof(compares[1]));
    copies[0] = internal_copy; sets[0] = internal_set; compares[0] = internal_compare;
#ifdef BOUNDARY_NEGATIVE_ABI
    {
        void (*bad)(void) = boundary_bad_abi;
        ULONG_PTR result;
        if (boundary_check_abi(function_address(&bad, sizeof(bad)), destination, (ULONG_PTR)source, 0, &result))
            fail(23);
    }
#endif
    GetSystemInfo(&info);
    guard_unit = info.dwAllocationGranularity;
    if (guard_unit < MAX_LENGTH || guard_unit > 1024 * 1024 || guard_unit < info.dwPageSize ||
        !info.dwPageSize || guard_unit % info.dwPageSize) fail(24);
    guard_source = guard_allocation(); guard_destination = guard_allocation();
    for (method = 0; method < 2; ++method)
    {
        check_memory(method);
        check_abi(method);
        check_guard(method);
    }
    if (fault_count != 6 || !VirtualFree(guard_source, 0, MEM_RELEASE) ||
        !VirtualFree(guard_destination, 0, MEM_RELEASE)) fail(24);
    for (kind = 0; kind < 3; ++kind)
        for (length_index = 0; length_index < ARRAY_SIZE(lengths); ++length_index)
            for (method = 0; method < 2; ++method) measure(kind, method ^ order, lengths[length_index]);
    if (!FreeLibrary(module)) fail(10);
    line.length = 0;
    text(&line, "CRT_BOUNDARY_PASS memory=900 abi=24 guard=24 faults=6 phases=24\n");
    emit(&line, STD_OUTPUT_HANDLE);
    ExitProcess(0);
}
