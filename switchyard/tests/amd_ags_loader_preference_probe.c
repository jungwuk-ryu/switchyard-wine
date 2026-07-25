/*
 * Copyright 2026
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int has_marker( HMODULE module )
{
    return GetProcAddress(module, "amd_ags_probe_marker") != NULL;
}

int main(void)
{
    const char *expected = getenv("SWITCHYARD_AGS_EXPECTED");
    HMODULE module;
    char module_path[MAX_PATH];
    DWORD len;

    module = LoadLibraryA("amd_ags_x64.dll");
    if (!module)
    {
        if (expected && !strcmp(expected, "missing") && GetLastError() == ERROR_MOD_NOT_FOUND)
        {
            printf("MODULE_MISSING=1\n");
            return 0;
        }
        fprintf(stderr, "LoadLibraryA failed: %lu\n", GetLastError());
        return 1;
    }

    len = GetModuleFileNameA(module, module_path, MAX_PATH);
    if (len && len < MAX_PATH)
        printf("MODULE_PATH=%s\n", module_path);
    printf("NATIVE_MARKER=%d\n", has_marker(module));

    if (expected && *expected)
    {
        if (!strcmp(expected, "native"))
            return has_marker(module) ? 0 : 2;
        if (!strcmp(expected, "builtin"))
            return has_marker(module) ? 3 : 0;
        if (!strcmp(expected, "missing"))
            return 4;

        fprintf(stderr, "Unknown expected mode: %s\n", expected);
    }

    return 0;
}
