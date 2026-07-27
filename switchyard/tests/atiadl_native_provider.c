#include <windows.h>

__declspec(dllexport) INT_PTR WINAPI switchyard_native_adl_probe_marker(void)
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
