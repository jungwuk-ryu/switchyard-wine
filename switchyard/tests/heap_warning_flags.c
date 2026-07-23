#include <windows.h>

#include <stdio.h>
#include <string.h>

#define BLOCK_FILL_TAIL 0xab

static BOOL has_debug_tail( const BYTE *block )
{
    unsigned int i;

    for (i = 1; i <= 8; ++i)
        if (block[i] != BLOCK_FILL_TAIL) return FALSE;
    return TRUE;
}

int main( int argc, char **argv )
{
    BOOL expect_validation, validation_enabled;
    HANDLE heap;
    BYTE *block;

    if (argc != 2) return 2;
    if (!strcmp(argv[1], "normal"))
        expect_validation = FALSE;
    else if (!strcmp(argv[1], "validated"))
        expect_validation = TRUE;
    else
        return 2;

    if (!(heap = HeapCreate( 0, 0, 0 ))) return 3;
    if (!(block = HeapAlloc( heap, HEAP_ZERO_MEMORY, 1 ))) return 4;

    validation_enabled = has_debug_tail( block );
    HeapFree( heap, 0, block );
    HeapDestroy( heap );

    if (validation_enabled != expect_validation)
    {
        fprintf( stderr, "heap validation was %s, expected %s\n",
                 validation_enabled ? "enabled" : "disabled",
                 expect_validation ? "enabled" : "disabled" );
        return 1;
    }

    return 0;
}
