/*
 * Switchyard Wine WoW64 large-address-aware override smoke test.
 *
 * This executable must remain a 32-bit PE image without the
 * IMAGE_FILE_LARGE_ADDRESS_AWARE characteristic.
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>

int main( int argc, char **argv )
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)GetModuleHandleW( NULL );
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)((BYTE *)dos + dos->e_lfanew);
    MEMORYSTATUS memory = {0};
    SYSTEM_INFO system;
    BOOL enabled;
    void *allocation;

    if (argc != 2 || (strcmp( argv[1], "enabled" ) && strcmp( argv[1], "disabled" )))
    {
        fprintf( stderr, "usage: %s enabled|disabled\n", argv[0] );
        return 2;
    }
    enabled = !strcmp( argv[1], "enabled" );
    memory.dwLength = sizeof(memory);

    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386)
    {
        fprintf( stderr, "test executable is not an i386 PE image\n" );
        return 1;
    }
    if (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE)
    {
        fprintf( stderr, "test executable is unexpectedly large-address-aware\n" );
        return 1;
    }

    GetSystemInfo( &system );
    GlobalMemoryStatus( &memory );
    allocation = VirtualAlloc( NULL, 0x1000000, MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS );
    if (!allocation)
    {
        fprintf( stderr, "16 MiB top-down reservation failed: %lu\n", GetLastError() );
        return 1;
    }

    printf( "mode=%s maximum=%p total=%#lx allocation=%p\n", argv[1],
            system.lpMaximumApplicationAddress, (unsigned long)memory.dwTotalVirtual, allocation );

    if (enabled)
    {
        if ((ULONG_PTR)system.lpMaximumApplicationAddress != 0xfffeffff ||
            memory.dwTotalVirtual <= LONG_MAX || (ULONG_PTR)allocation <= LONG_MAX)
        {
            fprintf( stderr, "large-address-aware override did not expose the upper 2 GiB\n" );
            return 1;
        }
    }
    else if ((ULONG_PTR)system.lpMaximumApplicationAddress != 0x7ffeffff ||
             memory.dwTotalVirtual > LONG_MAX || (ULONG_PTR)allocation > LONG_MAX)
    {
        fprintf( stderr, "large-address-aware opt-out did not preserve the 2 GiB limit\n" );
        return 1;
    }

    VirtualFree( allocation, 0, MEM_RELEASE );
    return 0;
}
