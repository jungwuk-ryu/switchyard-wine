#include <windows.h>

#include <stdio.h>

int main(void)
{
    FARPROC marker;
    HMODULE module;

    module = LoadLibraryA("atiadlxx.dll");
    if (!module)
    {
        fprintf(stderr, "LoadLibraryA(atiadlxx.dll) failed: %lu.\n",
                GetLastError());
        return 1;
    }

    marker = GetProcAddress(module, "switchyard_native_adl_probe_marker");
    if (!marker || marker() != 1)
    {
        fprintf(stderr, "The native ADL provider did not win load order.\n");
        FreeLibrary(module);
        return 2;
    }

    puts("ADL_NATIVE_PROVIDER=1");
    FreeLibrary(module);
    return 0;
}
