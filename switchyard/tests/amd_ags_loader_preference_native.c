/*
 * Copyright 2026
 */

#include <windows.h>

__declspec(dllexport) int __stdcall amd_ags_probe_marker(void)
{
    return 0x5a;
}
