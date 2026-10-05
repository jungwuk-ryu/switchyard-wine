/*
 * Runtime-computed x64 unaligned memory accesses through the Wine provider.
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep standalone
#endif

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define THREAD_COUNT 8
#define REGION_SIZE  65536
#define TEST_VALUE   UINT64_C(0x8877665544332211)

/* Explicit x64 instructions avoid both C alignment UB and compiler folding
 * into memcpy. The address is unknown until the Windows allocation returns. */
__attribute__((noinline)) static void store_value( void *address, unsigned int size, uint64_t value )
{
    switch (size)
    {
    case 1: __asm__ volatile( "movb %b1,(%0)" : : "r"(address), "r"(value) : "memory" ); break;
    case 2: __asm__ volatile( "movw %w1,(%0)" : : "r"(address), "r"(value) : "memory" ); break;
    case 4: __asm__ volatile( "movl %k1,(%0)" : : "r"(address), "r"(value) : "memory" ); break;
    case 8: __asm__ volatile( "movq %1,(%0)" : : "r"(address), "r"(value) : "memory" ); break;
    }
}

__attribute__((noinline)) static uint64_t load_value( const void *address, unsigned int size )
{
    uint64_t value = 0;

    switch (size)
    {
    case 1: __asm__ volatile( "movzbq (%1),%0" : "=r"(value) : "r"(address) : "memory" ); break;
    case 2: __asm__ volatile( "movzwq (%1),%0" : "=r"(value) : "r"(address) : "memory" ); break;
    case 4: __asm__ volatile( "movl (%1),%k0" : "=r"(value) : "r"(address) : "memory" ); break;
    case 8: __asm__ volatile( "movq (%1),%0" : "=r"(value) : "r"(address) : "memory" ); break;
    }
    return value;
}

static unsigned int check_region( unsigned char *region )
{
    unsigned int size, iteration, byte, offset, failures = 0;
    uint64_t mask;

    for (size = 1; size <= 8; size *= 2)
    {
        mask = size == 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
        /* Cross the granule first, then revisit every low-bit alignment. */
        for (iteration = 0; iteration < 18; ++iteration)
        {
            offset = iteration && iteration < 17 ? iteration - 1 : 15;
            memset( region, 0xa5, 64 );
            store_value( region + 16 + offset, size, TEST_VALUE );
            if (load_value( region + 16 + offset, size ) != (TEST_VALUE & mask)) ++failures;
            for (byte = 0; byte < 64; ++byte)
            {
                unsigned char expected = 0xa5;
                if (byte >= 16 + offset && byte < 16 + offset + size)
                    expected = TEST_VALUE >> ((byte - 16 - offset) * 8);
                if (region[byte] != expected) ++failures;
            }
        }
    }
    return failures;
}

struct worker_data
{
    HANDLE start;
    unsigned char *region;
    uint64_t value;
    uint64_t actual;
};

static DWORD WINAPI worker( void *argument )
{
    struct worker_data *data = argument;
    uint64_t result;
    unsigned int i;

    if (WaitForSingleObject( data->start, 10000 ) != WAIT_OBJECT_0) return 0x101;
    for (i = 0; i < 256; ++i)
    {
        /* This site is intentionally distinct from the warmed serial sites. */
        __asm__ volatile( "movq %1,(%0)" : : "r"(data->region + 15), "r"(data->value) : "memory" );
        __asm__ volatile( "movq (%1),%0" : "=r"(result) : "r"(data->region + 15) : "memory" );
        if (result != data->value)
        {
            data->actual = result;
            return 0x102;
        }
    }
    return 0;
}

int main(void)
{
    unsigned char *low, *high;
    struct worker_data data[THREAD_COUNT];
    HANDLE threads[THREAD_COUNT], start;
    unsigned int i, count = 0, failures = 0;
    DWORD status, low_error, high_error;

    low = VirtualAlloc( (void *)UINT64_C(0x21000000), REGION_SIZE,
                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    low_error = low ? 0 : GetLastError();
    high = VirtualAlloc( NULL, REGION_SIZE,
                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    high_error = high ? 0 : GetLastError();
    if (!low || !high || (uintptr_t)low >= UINT64_C(0x100000000) ||
        (uintptr_t)high < UINT64_C(0x100000000))
    {
        fprintf( stderr, "test allocation failed: low=%lu high=%lu\n", low_error, high_error );
        failures = 1;
        goto done;
    }
    failures += check_region( low );
    failures += check_region( high );
    if (failures) goto done;
    puts( "X64_UNALIGNED_TSO_PE_SERIAL_PASS low+high widths=1,2,4,8 offsets=0..15" );
    fflush( stdout );
    start = CreateEventW( NULL, TRUE, FALSE, NULL );
    if (!start)
    {
        fprintf( stderr, "CreateEvent failed: %lu\n", GetLastError() );
        ++failures;
        goto done;
    }
    for (i = 0; i < THREAD_COUNT; ++i)
    {
        data[i].start = start;
        data[i].region = (i & 1 ? high : low) + 1024 + 64 * i;
        data[i].value = TEST_VALUE + i;
        data[i].actual = 0;
        threads[i] = CreateThread( NULL, 0, worker, data + i, 0, NULL );
        if (!threads[i])
        {
            fprintf( stderr, "CreateThread %u failed: %lu\n", i, GetLastError() );
            ++failures;
            break;
        }
        ++count;
    }
    if (!SetEvent( start ))
    {
        fprintf( stderr, "SetEvent failed: %lu\n", GetLastError() );
        ++failures;
    }
    for (i = 0; i < count; ++i)
    {
        /* A harness timeout owns termination; never free a live worker's data. */
        if (WaitForSingleObject( threads[i], INFINITE ) != WAIT_OBJECT_0 ||
            !GetExitCodeThread( threads[i], &status ))
        {
            fprintf( stderr, "worker %u completion query failed: %lu\n", i, GetLastError() );
            ++failures;
        }
        else if (status)
        {
            fprintf( stderr, "worker %u returned %#lx, actual=%#llx\n",
                     i, status, (unsigned long long)data[i].actual );
            ++failures;
        }
        CloseHandle( threads[i] );
    }
    CloseHandle( start );
done:
    if (high && !VirtualFree( high, 0, MEM_RELEASE ))
    {
        fprintf( stderr, "high mapping release failed: %lu\n", GetLastError() );
        ++failures;
    }
    if (low && !VirtualFree( low, 0, MEM_RELEASE ))
    {
        fprintf( stderr, "low mapping release failed: %lu\n", GetLastError() );
        ++failures;
    }
    if (failures)
    {
        fprintf( stderr, "unaligned TSO failures: %u\n", failures );
        return 1;
    }
    puts( "X64_UNALIGNED_TSO_PE_PASS low+high widths=1,2,4,8 offsets=0..15 threads=8" );
    return 0;
}
