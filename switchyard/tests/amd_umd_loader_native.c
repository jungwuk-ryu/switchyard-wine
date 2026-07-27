#include <windows.h>

__declspec(dllexport) int WINAPI switchyard_amd_umd_probe_marker(void)
{
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
