#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    const char *expected = getenv("SWITCHYARD_AMD_UMD_EXPECTED");
    HMODULE module = LoadLibraryA("atidxx64.dll");
    int has_marker;

    if (!module)
    {
        DWORD error = GetLastError();

        if (expected && !strcmp(expected, "missing")
                && error == ERROR_MOD_NOT_FOUND)
        {
            printf("AMD_UMD_MISSING=1\n");
            return 0;
        }
        fprintf(stderr, "LoadLibraryA(atidxx64.dll) failed: %lu.\n",
                error);
        return 1;
    }

    has_marker = GetProcAddress(module, "switchyard_amd_umd_probe_marker") != NULL;
    printf("AMD_UMD_NATIVE_MARKER=%d\n", has_marker);
    FreeLibrary(module);

    if (expected && !strcmp(expected, "native"))
        return has_marker ? 0 : 2;
    if (expected && !strcmp(expected, "missing"))
        return 3;
    return 1;
}
