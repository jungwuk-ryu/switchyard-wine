/*
 * Native Unicorn hook-cost benchmark for the xtajit execution gate
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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>

#define CODE_ADDRESS 0x01000000u
#define CODE_SIZE    0x1000u

enum hook_mode
{
    MODE_NO_HOOK,
    MODE_PERMANENT_BLOCK,
    MODE_ONE_SHOT,
};

struct one_shot
{
    uc_hook hook;
    uc_err error;
    uint64_t calls;
};

struct measurement
{
    uint64_t median_ns;
    uint64_t hook_calls;
};

static uint64_t permanent_hook_calls;

static uint64_t elapsed_nanoseconds( const struct timespec *start,
                                     const struct timespec *end )
{
    time_t seconds = end->tv_sec - start->tv_sec;
    long nanoseconds = end->tv_nsec - start->tv_nsec;

    if (nanoseconds < 0)
    {
        --seconds;
        nanoseconds += 1000000000l;
    }
    return (uint64_t)seconds * 1000000000 + nanoseconds;
}

static int compare_u64( const void *left, const void *right )
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return a > b ? 1 : a < b ? -1 : 0;
}

static void permanent_block_hook( uc_engine *uc, uint64_t address,
                                  uint32_t size, void *user )
{
    (void)uc;
    (void)address;
    (void)size;
    (void)user;
    ++permanent_hook_calls;
}

static void one_shot_block_hook( uc_engine *uc, uint64_t address,
                                 uint32_t size, void *user )
{
    struct one_shot *state = user;

    (void)address;
    (void)size;
    ++state->calls;
    if (state->hook)
    {
        state->error = uc_hook_del( uc, state->hook );
        state->hook = 0;
    }
}

static int write_loop_code( uc_engine *uc, uint32_t iterations,
                            uint64_t *end )
{
    unsigned char code[] =
    {
        0xb9, 0, 0, 0, 0,  /* mov iterations,%ecx */
        0x31, 0xc0,        /* xor %eax,%eax */
        0x40,              /* inc %eax */
        0x49,              /* dec %ecx */
        0x75, 0xfc,        /* jne inc */
        0x90,              /* end marker */
    };
    uc_err err;

    memcpy( code + 1, &iterations, sizeof(iterations) );
    if ((err = uc_mem_write( uc, CODE_ADDRESS, code, sizeof(code) )) != UC_ERR_OK)
    {
        fprintf( stderr, "uc_mem_write failed: %s\n", uc_strerror( err ) );
        return 0;
    }
    *end = CODE_ADDRESS + sizeof(code) - 1;
    return 1;
}

static int arm_one_shot( uc_engine *uc, struct one_shot *state )
{
    uc_err err;

    state->hook = 0;
    state->error = UC_ERR_OK;
    if ((err = uc_hook_add( uc, &state->hook, UC_HOOK_BLOCK,
                            one_shot_block_hook, state, 1, 0 )) != UC_ERR_OK ||
        (err = uc_ctl_remove_cache( uc, CODE_ADDRESS,
                                    CODE_ADDRESS + 1 )) != UC_ERR_OK)
    {
        fprintf( stderr, "cannot arm one-shot hook: %s\n", uc_strerror( err ) );
        if (state->hook) uc_hook_del( uc, state->hook );
        state->hook = 0;
        return 0;
    }
    return 1;
}

static int run_start( uc_engine *uc, enum hook_mode mode,
                      struct one_shot *one_shot, uint64_t end,
                      uint32_t iterations )
{
    uint32_t eax = 0, ecx = 1;
    uint64_t calls = one_shot->calls;
    uc_err err;

    if (mode == MODE_ONE_SHOT && !arm_one_shot( uc, one_shot )) return 0;
    err = uc_emu_start( uc, CODE_ADDRESS, end, 0, 0 );
    if (one_shot->hook)
    {
        uc_hook_del( uc, one_shot->hook );
        one_shot->hook = 0;
    }
    if (err != UC_ERR_OK || one_shot->error != UC_ERR_OK ||
        uc_reg_read( uc, UC_X86_REG_EAX, &eax ) != UC_ERR_OK ||
        uc_reg_read( uc, UC_X86_REG_ECX, &ecx ) != UC_ERR_OK ||
        eax != iterations || ecx ||
        (mode == MODE_ONE_SHOT && one_shot->calls != calls + 1))
    {
        fprintf( stderr, "emulation validation failed: mode %u error %s "
                 "hook %s eax %#x/%#x ecx %#x calls %llu/%llu\n", mode,
                 uc_strerror( err ), uc_strerror( one_shot->error ), eax,
                 iterations, ecx, (unsigned long long)one_shot->calls,
                 (unsigned long long)(calls + 1) );
        return 0;
    }
    return 1;
}

static int measure_mode( enum hook_mode mode, uint32_t iterations,
                         unsigned int starts, unsigned int trials,
                         struct measurement *measurement )
{
    struct one_shot one_shot = {0};
    struct timespec start, end_time;
    uint64_t *samples, end;
    uc_engine *uc;
    uc_hook hook;
    uc_err err;
    unsigned int i, j;
    int ret = 0;

    if (!(samples = calloc( trials, sizeof(*samples) ))) return 0;
    if ((err = uc_open( UC_ARCH_X86, UC_MODE_32, &uc )) != UC_ERR_OK)
    {
        fprintf( stderr, "uc_open failed: %s\n", uc_strerror( err ) );
        free( samples );
        return 0;
    }
    if ((err = uc_mem_map( uc, CODE_ADDRESS, CODE_SIZE,
                           UC_PROT_READ | UC_PROT_EXEC )) != UC_ERR_OK ||
        !write_loop_code( uc, iterations, &end ))
    {
        if (err != UC_ERR_OK)
            fprintf( stderr, "uc_mem_map failed: %s\n", uc_strerror( err ) );
        goto done;
    }
    if (mode == MODE_PERMANENT_BLOCK &&
        (err = uc_hook_add( uc, &hook, UC_HOOK_BLOCK,
                            permanent_block_hook, NULL, 1, 0 )) != UC_ERR_OK)
    {
        fprintf( stderr, "permanent hook add failed: %s\n", uc_strerror( err ) );
        goto done;
    }
    if (!run_start( uc, mode, &one_shot, end, iterations )) goto done;

    permanent_hook_calls = 0;
    one_shot.calls = 0;
    for (i = 0; i < trials; ++i)
    {
        clock_gettime( CLOCK_MONOTONIC, &start );
        for (j = 0; j < starts; ++j)
            if (!run_start( uc, mode, &one_shot, end, iterations )) goto done;
        clock_gettime( CLOCK_MONOTONIC, &end_time );
        samples[i] = elapsed_nanoseconds( &start, &end_time );
    }
    qsort( samples, trials, sizeof(*samples), compare_u64 );
    measurement->median_ns = samples[trials / 2];
    measurement->hook_calls = mode == MODE_PERMANENT_BLOCK ?
                              permanent_hook_calls : one_shot.calls;
    ret = 1;

done:
    uc_close( uc );
    free( samples );
    return ret;
}

static unsigned long parse_value( const char *name, unsigned long fallback,
                                  unsigned long maximum )
{
    const char *value = getenv( name );
    char *end;
    unsigned long result;

    if (!value || !*value) return fallback;
    errno = 0;
    result = strtoul( value, &end, 10 );
    if (errno || *end || !result || result > maximum)
    {
        fprintf( stderr, "%s must be from 1 through %lu\n", name, maximum );
        exit( 2 );
    }
    return result;
}

static const char *mode_name( enum hook_mode mode )
{
    switch (mode)
    {
    case MODE_NO_HOOK: return "no-hook";
    case MODE_PERMANENT_BLOCK: return "permanent-block";
    case MODE_ONE_SHOT: return "one-shot";
    }
    return "invalid";
}

static void print_long_result( enum hook_mode mode,
                               const struct measurement *measurement,
                               uint32_t iterations )
{
    double instructions = (double)iterations * 3 + 2;
    double millions_per_second = instructions * 1000.0 / measurement->median_ns;

    printf( "long %-15s %9.2f M guest-insn/s median %llu us hooks %llu\n",
            mode_name( mode ), millions_per_second,
            (unsigned long long)(measurement->median_ns / 1000),
            (unsigned long long)measurement->hook_calls );
}

static void print_short_result( enum hook_mode mode,
                                const struct measurement *measurement,
                                unsigned int starts )
{
    double nanoseconds_per_start = (double)measurement->median_ns / starts;

    printf( "short %-14s %9.0f starts/s %8.1f ns/start hooks %llu\n",
            mode_name( mode ), 1000000000.0 / nanoseconds_per_start,
            nanoseconds_per_start,
            (unsigned long long)measurement->hook_calls );
}

int main(void)
{
    struct measurement long_results[3], short_results[3];
    unsigned long iterations = parse_value( "XTAJIT_PERF_GUEST_LOOPS",
                                            5000000, 100000000 );
    unsigned long starts = parse_value( "XTAJIT_PERF_SHORT_STARTS",
                                        10000, 1000000 );
    unsigned long trials = parse_value( "XTAJIT_PERF_TRIALS", 9, 101 );
    unsigned int major, minor;
    enum hook_mode mode;

    uc_version( &major, &minor );
    printf( "Unicorn %u.%u; %lu trials; %lu long loops; %lu short starts\n",
            major, minor, trials, iterations, starts );
    for (mode = MODE_NO_HOOK; mode <= MODE_ONE_SHOT; ++mode)
    {
        if (!measure_mode( mode, iterations, 1, trials,
                           &long_results[mode] ) ||
            !measure_mode( mode, 16, starts, trials,
                           &short_results[mode] ))
            return 1;
        print_long_result( mode, &long_results[mode], iterations );
        print_short_result( mode, &short_results[mode], starts );
    }
    printf( "long one-shot/permanent throughput %.2fx; one-shot/no-hook %.1f%%\n",
            (double)long_results[MODE_PERMANENT_BLOCK].median_ns /
                long_results[MODE_ONE_SHOT].median_ns,
            (double)long_results[MODE_NO_HOOK].median_ns * 100.0 /
                long_results[MODE_ONE_SHOT].median_ns );
    printf( "short one-shot/permanent starts %.2fx; one-shot/no-hook %.1f%%\n",
            (double)short_results[MODE_PERMANENT_BLOCK].median_ns /
                short_results[MODE_ONE_SHOT].median_ns,
            (double)short_results[MODE_NO_HOOK].median_ns * 100.0 /
                short_results[MODE_ONE_SHOT].median_ns );
    return 0;
}
