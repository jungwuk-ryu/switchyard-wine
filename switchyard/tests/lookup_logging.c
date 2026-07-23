#include <windows.h>

int main(void)
{
    HMODULE module;

    if (!(module = LoadLibraryW( L"xinput1_4.dll" ))) return 1;
    if (GetProcAddress( module, "XInputGetDSoundAudioDeviceGuids" )) return 2;
    FreeLibrary( module );

    if (GetModuleHandleW( L"C:\\windows\\System32\\d3d.dll" )) return 3;
    return 0;
}
