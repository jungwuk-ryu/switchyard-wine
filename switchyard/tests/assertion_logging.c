#include <windows.h>
#include <stdio.h>
#include <string.h>

#ifndef STATUS_ASSERTION_FAILURE
#define STATUS_ASSERTION_FAILURE ((DWORD)0xc0000420)
#endif

static LONG CALLBACK assertion_handler(EXCEPTION_POINTERS *exception)
{
    if (exception->ExceptionRecord->ExceptionCode != STATUS_ASSERTION_FAILURE)
        return EXCEPTION_CONTINUE_SEARCH;

#ifdef __x86_64__
    exception->ContextRecord->Rip += 2;
#else
#error This test currently requires x86_64.
#endif
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void raise_assertion(void)
{
    __asm__ volatile("int $0x2c");
}

static void bypass_top_level_exception_filter(void)
{
    BYTE *peb;

    __asm__ volatile("movq %%gs:0x60,%0" : "=r" (peb));
    /* Make UnhandledExceptionFilter return EXCEPTION_CONTINUE_SEARCH. */
    peb[2] = TRUE;
}

int main(int argc, char **argv)
{
    PVOID handler;

    if (argc != 2) return 2;
    if (!strcmp(argv[1], "unhandled"))
    {
        bypass_top_level_exception_filter();
        raise_assertion();
        return 3;
    }
    if (strcmp(argv[1], "handled")) return 2;

    handler = AddVectoredExceptionHandler(TRUE, assertion_handler);
    if (!handler) return 4;
    raise_assertion();
    RemoveVectoredExceptionHandler(handler);
    puts("continued");
    return 0;
}
