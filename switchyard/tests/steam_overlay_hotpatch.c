#include <windows.h>

#include <stdio.h>
#include <string.h>

struct hotpatch_target
{
    const char *module;
    const char *function;
};

static int verify_hotpatch_target( const struct hotpatch_target *target )
{
    static const BYTE hotpatch_prologue[] = {0x8b, 0xff, 0x55, 0x8b, 0xec};
    HMODULE module;
    FARPROC function;
    unsigned int i;

    if (!(module = LoadLibraryA( target->module )))
    {
        fprintf( stderr, "Could not load %s: %lu\n", target->module, GetLastError() );
        return 0;
    }
    if (!(function = GetProcAddress( module, target->function )))
    {
        fprintf( stderr, "Could not resolve %s!%s: %lu\n", target->module,
                 target->function, GetLastError() );
        FreeLibrary( module );
        return 0;
    }
    if (memcmp( function, hotpatch_prologue, sizeof(hotpatch_prologue) ))
    {
        fprintf( stderr, "%s!%s has an unsupported entry sequence:", target->module,
                 target->function );
        for (i = 0; i < sizeof(hotpatch_prologue); ++i)
            fprintf( stderr, " %02x", ((const BYTE *)function)[i] );
        fputc( '\n', stderr );
        FreeLibrary( module );
        return 0;
    }

    FreeLibrary( module );
    return 1;
}

static int verify_private_swap_entry(void)
{
    FARPROC public_swap, private_swap;
    HMODULE module;

    if (!(module = LoadLibraryA( "opengl32.dll" )))
    {
        fprintf( stderr, "Could not load opengl32.dll: %lu\n", GetLastError() );
        return 0;
    }
    public_swap = GetProcAddress( module, "wglSwapBuffers" );
    private_swap = GetProcAddress( module, "__wine_wglSwapBuffers" );
    if (!public_swap || !private_swap)
    {
        fprintf( stderr, "Could not resolve the public and private OpenGL swap entry points\n" );
        FreeLibrary( module );
        return 0;
    }
    if (public_swap == private_swap)
    {
        fprintf( stderr, "The public and private OpenGL swap entry points are identical\n" );
        FreeLibrary( module );
        return 0;
    }

    FreeLibrary( module );
    return 1;
}

int main(void)
{
    static const struct hotpatch_target targets[] =
    {
        {"hid.dll", "HidP_GetButtonCaps"},
        {"hid.dll", "HidP_GetCaps"},
        {"hid.dll", "HidP_GetData"},
        {"hid.dll", "HidP_GetUsages"},
        {"hid.dll", "HidP_GetUsageValue"},
        {"hid.dll", "HidP_GetValueCaps"},
        {"hid.dll", "HidP_MaxDataListLength"},
        {"opengl32.dll", "wglSwapBuffers"},
        {"setupapi.dll", "SetupDiEnumDriverInfoA"},
    };
    unsigned int i;
    int success = 1;

    if (sizeof(void *) != 4)
    {
        fprintf( stderr, "Steam overlay hotpatch test must run as a 32-bit process\n" );
        return 1;
    }

    for (i = 0; i < ARRAYSIZE(targets); ++i)
        success &= verify_hotpatch_target( &targets[i] );
    success &= verify_private_swap_entry();

    if (!success) return 1;
    printf( "Steam overlay hotpatch entry points passed\n" );
    return 0;
}
