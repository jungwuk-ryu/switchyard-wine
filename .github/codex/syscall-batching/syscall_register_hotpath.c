/*
 * Native Apple Silicon microbenchmark for xtajit64 syscall register transfer.
 */

#include <errno.h>
#include <inttypes.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ITERATIONS 200000u
#define WARMUP_ITERATIONS 10000u
#define SAMPLE_COUNT  nine_samples_are_required
#undef SAMPLE_COUNT
#define SAMPLE_COUNT 9u
#define SYSCALL_COUNT 4096u
#define DISPATCHER UINT64_C(0x00007fff12345000)
#define INITIAL_RIP UINT64_C(0x00007fff0abcde02)
#define INITIAL_R10 UINT64_C(0x1122334455667788)
#define INITIAL_RCX UINT64_C(0x8877665544332211)

static volatile uint64_t timing_sink;

static const int setup_regs[] =
{
    UC_X86_REG_RAX, UC_X86_REG_RIP, UC_X86_REG_R10, UC_X86_REG_RCX,
};

static const int result_regs[] =
{
    UC_X86_REG_RAX, UC_X86_REG_RIP, UC_X86_REG_R10, UC_X86_REG_RCX,
};

static const int syscall_read_regs[] =
{
    UC_X86_REG_RAX, UC_X86_REG_RIP, UC_X86_REG_R10,
};

static const int syscall_dispatch_regs[] =
{
    UC_X86_REG_RCX, UC_X86_REG_R10, UC_X86_REG_RIP,
};

struct register_snapshot
{
    uint64_t rax;
    uint64_t rip;
    uint64_t r10;
    uint64_t rcx;
};

static void die_uc( const char *operation, uc_err err )
{
    fprintf( stderr, "%s: %s\n", operation, uc_strerror( err ) );
    exit( 1 );
}

static uint64_t invalid_system_service(void)
{
    return (uint64_t)(int64_t)(int32_t)UINT32_C(0xc000001c);
}

static uc_engine *open_engine(void)
{
    uc_engine *uc = NULL;
    uc_err err = uc_open( UC_ARCH_X86, UC_MODE_64, &uc );

    if (err != UC_ERR_OK) die_uc( "uc_open", err );
    return uc;
}

static void set_state( uc_engine *uc, uint64_t rax )
{
    uint64_t rip = INITIAL_RIP;
    uint64_t r10 = INITIAL_R10;
    uint64_t rcx = INITIAL_RCX;
    void *values[] = {&rax, &rip, &r10, &rcx};
    uc_err err = uc_reg_write_batch( uc, setup_regs, values, ARRAY_SIZE(setup_regs) );

    if (err != UC_ERR_OK) die_uc( "set_state", err );
}

static struct register_snapshot snapshot( uc_engine *uc )
{
    struct register_snapshot result;
    void *values[] = {&result.rax, &result.rip, &result.r10, &result.rcx};
    uc_err err = uc_reg_read_batch( uc, result_regs, values, ARRAY_SIZE(result_regs) );

    if (err != UC_ERR_OK) die_uc( "snapshot", err );
    return result;
}

static uc_err scalar_prepare( uc_engine *uc, uint64_t dispatcher, uint32_t count,
                              uint64_t *next_rip )
{
    uint64_t rax, r10, rip;
    uc_err err;

    if ((err = uc_reg_read( uc, UC_X86_REG_RAX, &rax )) != UC_ERR_OK) return err;
    if ((err = uc_reg_read( uc, UC_X86_REG_RIP, &rip )) != UC_ERR_OK) return err;
    if (rax >= count)
    {
        rax = invalid_system_service();
        if ((err = uc_reg_write( uc, UC_X86_REG_RAX, &rax )) != UC_ERR_OK) return err;
        *next_rip = rip;
        return UC_ERR_OK;
    }
    if ((err = uc_reg_read( uc, UC_X86_REG_R10, &r10 )) != UC_ERR_OK) return err;
    if ((err = uc_reg_write( uc, UC_X86_REG_RCX, &r10 )) != UC_ERR_OK ||
        (err = uc_reg_write( uc, UC_X86_REG_R10, &rip )) != UC_ERR_OK ||
        (err = uc_reg_write( uc, UC_X86_REG_RIP, &dispatcher )) != UC_ERR_OK)
        return err;
    *next_rip = dispatcher;
    return UC_ERR_OK;
}

static uc_err batch_prepare( uc_engine *uc, uint64_t dispatcher, uint32_t count,
                             uint64_t *next_rip )
{
    uint64_t rax, r10, rip;
    void *read_values[] = {&rax, &rip, &r10};
    void *dispatch_values[] = {&r10, &rip, &dispatcher};
    uc_err err;

    if ((err = uc_reg_read_batch( uc, syscall_read_regs, read_values,
                                  ARRAY_SIZE(syscall_read_regs) )) != UC_ERR_OK)
        return err;
    if (rax >= count)
    {
        rax = invalid_system_service();
        if ((err = uc_reg_write_batch( uc, syscall_read_regs, read_values, 1 )) != UC_ERR_OK)
            return err;
        *next_rip = rip;
        return UC_ERR_OK;
    }
    if ((err = uc_reg_write_batch( uc, syscall_dispatch_regs, dispatch_values,
                                   ARRAY_SIZE(syscall_dispatch_regs) )) != UC_ERR_OK)
        return err;
    *next_rip = dispatcher;
    return UC_ERR_OK;
}

static void verify_snapshot( const char *name, const struct register_snapshot *actual,
                             uint64_t rax, uint64_t rip, uint64_t r10, uint64_t rcx )
{
    if (actual->rax == rax && actual->rip == rip &&
        actual->r10 == r10 && actual->rcx == rcx)
        return;
    fprintf( stderr,
             "%s mismatch: rax=%#" PRIx64 "/%#" PRIx64
             " rip=%#" PRIx64 "/%#" PRIx64
             " r10=%#" PRIx64 "/%#" PRIx64
             " rcx=%#" PRIx64 "/%#" PRIx64 "\n",
             name, actual->rax, rax, actual->rip, rip,
             actual->r10, r10, actual->rcx, rcx );
    exit( 1 );
}

static void verify_semantics(void)
{
    uc_engine *uc = open_engine();
    struct register_snapshot scalar, batch;
    uint64_t next_scalar = 0, next_batch = 0;
    uc_err err;

    set_state( uc, 7 );
    if ((err = scalar_prepare( uc, DISPATCHER, SYSCALL_COUNT, &next_scalar )) != UC_ERR_OK)
        die_uc( "scalar valid", err );
    scalar = snapshot( uc );
    verify_snapshot( "scalar valid", &scalar, 7, DISPATCHER,
                     INITIAL_RIP, INITIAL_R10 );
    if (next_scalar != DISPATCHER)
    {
        fprintf( stderr, "scalar valid next RIP mismatch\n" );
        exit( 1 );
    }

    set_state( uc, 7 );
    if ((err = batch_prepare( uc, DISPATCHER, SYSCALL_COUNT, &next_batch )) != UC_ERR_OK)
        die_uc( "batch valid", err );
    batch = snapshot( uc );
    verify_snapshot( "batch valid", &batch, 7, DISPATCHER,
                     INITIAL_RIP, INITIAL_R10 );
    if (memcmp( &scalar, &batch, sizeof(scalar) ) || next_scalar != next_batch)
    {
        fprintf( stderr, "scalar and batch valid paths diverged\n" );
        exit( 1 );
    }

    set_state( uc, SYSCALL_COUNT );
    if ((err = scalar_prepare( uc, DISPATCHER, SYSCALL_COUNT, &next_scalar )) != UC_ERR_OK)
        die_uc( "scalar invalid", err );
    scalar = snapshot( uc );
    verify_snapshot( "scalar invalid", &scalar, invalid_system_service(),
                     INITIAL_RIP, INITIAL_R10, INITIAL_RCX );
    if (next_scalar != INITIAL_RIP)
    {
        fprintf( stderr, "scalar invalid next RIP mismatch\n" );
        exit( 1 );
    }

    set_state( uc, SYSCALL_COUNT );
    if ((err = batch_prepare( uc, DISPATCHER, SYSCALL_COUNT, &next_batch )) != UC_ERR_OK)
        die_uc( "batch invalid", err );
    batch = snapshot( uc );
    verify_snapshot( "batch invalid", &batch, invalid_system_service(),
                     INITIAL_RIP, INITIAL_R10, INITIAL_RCX );
    if (memcmp( &scalar, &batch, sizeof(scalar) ) || next_scalar != next_batch)
    {
        fprintf( stderr, "scalar and batch invalid paths diverged\n" );
        exit( 1 );
    }
    uc_close( uc );
}

static double ticks_to_ns( uint64_t ticks )
{
    mach_timebase_info_data_t info;

    if (mach_timebase_info( &info ) != KERN_SUCCESS || !info.denom)
    {
        fprintf( stderr, "mach_timebase_info failed\n" );
        exit( 1 );
    }
    return (double)ticks * (double)info.numer / (double)info.denom;
}

static double measure( uc_engine *uc,
                       uc_err (*prepare)(uc_engine *, uint64_t, uint32_t, uint64_t *),
                       unsigned int iterations )
{
    uint64_t begin, end, next_rip = 0;
    unsigned int i;
    uc_err err;

    set_state( uc, 7 );
    begin = mach_continuous_time();
    for (i = 0; i < iterations; ++i)
    {
        if ((err = prepare( uc, DISPATCHER, SYSCALL_COUNT, &next_rip )) != UC_ERR_OK)
            die_uc( "timed syscall prepare", err );
        timing_sink ^= next_rip + i;
    }
    end = mach_continuous_time();
    return ticks_to_ns( end - begin ) / iterations;
}

static int compare_double( const void *left, const void *right )
{
    double a = *(const double *)left;
    double b = *(const double *)right;

    return a < b ? -1 : a > b;
}

static void cpu_model( char *buffer, size_t capacity )
{
    size_t size = capacity;

    if (sysctlbyname( "machdep.cpu.brand_string", buffer, &size, NULL, 0 ) || !size)
        snprintf( buffer, capacity, "unknown (%d)", errno );
    else buffer[capacity - 1] = 0;
}

int main(void)
{
    uc_engine *scalar_uc, *batch_uc;
    double scalar_samples[SAMPLE_COUNT], batch_samples[SAMPLE_COUNT];
    double scalar_ns, batch_ns, speedup;
    char model[256];
    unsigned int i;

#if !defined(__aarch64__) && !defined(__arm64__)
# error This benchmark must be compiled for native arm64.
#endif

    verify_semantics();
    scalar_uc = open_engine();
    batch_uc = open_engine();

    (void)measure( scalar_uc, scalar_prepare, WARMUP_ITERATIONS );
    (void)measure( batch_uc, batch_prepare, WARMUP_ITERATIONS );
    for (i = 0; i < SAMPLE_COUNT; ++i)
    {
        if (i & 1)
        {
            batch_samples[i] = measure( batch_uc, batch_prepare, ITERATIONS );
            scalar_samples[i] = measure( scalar_uc, scalar_prepare, ITERATIONS );
        }
        else
        {
            scalar_samples[i] = measure( scalar_uc, scalar_prepare, ITERATIONS );
            batch_samples[i] = measure( batch_uc, batch_prepare, ITERATIONS );
        }
    }
    qsort( scalar_samples, SAMPLE_COUNT, sizeof(double), compare_double );
    qsort( batch_samples, SAMPLE_COUNT, sizeof(double), compare_double );
    scalar_ns = scalar_samples[SAMPLE_COUNT / 2];
    batch_ns = batch_samples[SAMPLE_COUNT / 2];
    speedup = scalar_ns / batch_ns;
    cpu_model( model, sizeof(model) );

    printf( "APPLE_SILICON_CPU model=%s\n", model );
    printf( "APPLE_SILICON_HOTPATH syscall_scalar_ns=%.3f syscall_batch_ns=%.3f speedup=%.3fx\n",
            scalar_ns, batch_ns, speedup );
    printf( "APPLE_SILICON_HOTPATH api_calls_per_syscall=6->2 removed=4\n" );

    uc_close( scalar_uc );
    uc_close( batch_uc );
    if (!(batch_ns < scalar_ns * 0.95))
    {
        fprintf( stderr, "batched syscall register transfer did not beat scalar calls\n" );
        return 1;
    }
    return timing_sink == UINT64_MAX ? 1 : 0;
}
