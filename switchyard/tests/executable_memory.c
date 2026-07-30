#include <windows.h>

#include <stdio.h>
#include <string.h>

typedef int (*generated_func)(void);

int main(void)
{
    static const unsigned char return_42[] =
    {
        0xb8, 0x2a, 0x00, 0x00, 0x00, /* mov $42, %eax */
        0xc3                          /* ret */
    };
    union
    {
        void *data;
        generated_func func;
    } code;
    int result;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    code.data = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT,
                             PAGE_EXECUTE_READWRITE);
    if (!code.data)
    {
        fprintf(stderr, "VirtualAlloc failed: %lu\n", GetLastError());
        return 1;
    }
    if ((ULONG_PTR)code.data >= 0x200000000ULL)
    {
        fprintf(stderr, "Allocation %p is outside WINE_RESERVE\n", code.data);
        VirtualFree(code.data, 0, MEM_RELEASE);
        return 1;
    }

    memcpy(code.data, return_42, sizeof(return_42));
    if (!FlushInstructionCache(GetCurrentProcess(), code.data,
                               sizeof(return_42)))
    {
        fprintf(stderr, "FlushInstructionCache failed: %lu\n", GetLastError());
        VirtualFree(code.data, 0, MEM_RELEASE);
        return 1;
    }

    result = code.func();
    VirtualFree(code.data, 0, MEM_RELEASE);
    if (result != 42)
    {
        fprintf(stderr, "Generated code returned %d instead of 42\n", result);
        return 1;
    }

    puts("Executable memory test passed");
    return 0;
}
