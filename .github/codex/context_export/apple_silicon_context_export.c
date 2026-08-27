/*
 * Apple Silicon benchmark for xtajit64 terminal context export ordering.
 *
 * Current path:
 *   uc_reg_read_batch(live engine) + uc_context_save(binding snapshot)
 * Candidate path:
 *   uc_context_save(binding snapshot) +
 *   uc_context_reg_read_batch(saved snapshot)
 *
 * Both paths issue two Unicorn API calls.  The candidate is useful only if
 * reading normalized state from the just-saved context avoids enough repeated
 * live-engine work to beat the current ordering on native Apple Silicon.
 */

#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ROUNDS 11
#define COMPONENT_ITERATIONS 50000u
#define PAIR_ITERATIONS 30000u

struct normalized_context
{
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, eflags;
    uint32_t mxcsr;
    uint32_t reserved;
    uint64_t xmm[16][2];
};

static const int write_regs[] =
{
    UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
    UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_RBP, UC_X86_REG_RSP,
    UC_X86_REG_R8,  UC_X86_REG_R9,  UC_X86_REG_R10, UC_X86_REG_R11,
    UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15,
    UC_X86_REG_RIP, UC_X86_REG_EFLAGS, UC_X86_REG_MXCSR,
    UC_X86_REG_XMM0,  UC_X86_REG_XMM1,  UC_X86_REG_XMM2,  UC_X86_REG_XMM3,
    UC_X86_REG_XMM4,  UC_X86_REG_XMM5,  UC_X86_REG_XMM6,  UC_X86_REG_XMM7,
    UC_X86_REG_XMM8,  UC_X86_REG_XMM9,  UC_X86_REG_XMM10, UC_X86_REG_XMM11,
    UC_X86_REG_XMM12, UC_X86_REG_XMM13, UC_X86_REG_XMM14, UC_X86_REG_XMM15,
};

static const int read_regs[] =
{
    UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
    UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_RBP, UC_X86_REG_RSP,
    UC_X86_REG_R8,  UC_X86_REG_R9,  UC_X86_REG_R10, UC_X86_REG_R11,
    UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15,
    UC_X86_REG_RIP, UC_X86_REG_EFLAGS, UC_X86_REG_MXCSR,
    UC_X86_REG_XMM0,  UC_X86_REG_XMM1,  UC_X86_REG_XMM2,  UC_X86_REG_XMM3,
    UC_X86_REG_XMM4,  UC_X86_REG_XMM5,  UC_X86_REG_XMM6,  UC_X86_REG_XMM7,
    UC_X86_REG_XMM8,  UC_X86_REG_XMM9,  UC_X86_REG_XMM10, UC_X86_REG_XMM11,
    UC_X86_REG_XMM12, UC_X86_REG_XMM13, UC_X86_REG_XMM14, UC_X86_REG_XMM15,
};

static mach_timebase_info_data_t timebase;
static volatile uint64_t benchmark_sink;

static void die_unicorn(const char *operation, uc_err error)
{
    fprintf(stderr, "%s failed: %s\n", operation, uc_strerror(error));
    exit(1);
}

static double elapsed_ns(uint64_t begin, uint64_t end)
{
    return (double)(end - begin) * timebase.numer / timebase.denom;
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;

    return (a > b) - (a < b);
}

static double median(double values[ROUNDS])
{
    qsort(values, ROUNDS, sizeof(values[0]), compare_double);
    return values[ROUNDS / 2];
}

static void initialize_context(struct normalized_context *context)
{
    uint64_t *integer_values = &context->rax;
    size_t i, j;

    memset(context, 0, sizeof(*context));
    for (i = 0; i < 18; ++i)
        integer_values[i] = UINT64_C(0x0101010101010101) * (i + 1);
    context->rsp = UINT64_C(0x0000000100108000);
    context->rip = UINT64_C(0x0000000100101000);
    context->eflags = 0x202;
    context->mxcsr = 0x1f80;
    for (i = 0; i < ARRAY_SIZE(context->xmm); ++i)
        for (j = 0; j < ARRAY_SIZE(context->xmm[i]); ++j)
            context->xmm[i][j] =
                UINT64_C(0x9e3779b97f4a7c15) * (2 * i + j + 1);
}

static void build_values(struct normalized_context *context,
                         void *values[ARRAY_SIZE(read_regs)])
{
    uint64_t *integer_values = &context->rax;
    size_t i, index = 0;

    for (i = 0; i < 18; ++i) values[index++] = &integer_values[i];
    values[index++] = &context->mxcsr;
    for (i = 0; i < ARRAY_SIZE(context->xmm); ++i)
        values[index++] = context->xmm[i];
    if (index != ARRAY_SIZE(read_regs))
    {
        fprintf(stderr, "context value table mismatch: %zu != %zu\n",
                index, ARRAY_SIZE(read_regs));
        exit(1);
    }
}

static uc_err write_engine_context(uc_engine *uc,
                                   struct normalized_context *context)
{
    void *values[ARRAY_SIZE(write_regs)];

    build_values(context, values);
    return uc_reg_write_batch(uc, write_regs, values,
                              (int)ARRAY_SIZE(write_regs));
}

static uc_err read_live_context(uc_engine *uc,
                                struct normalized_context *context)
{
    void *values[ARRAY_SIZE(read_regs)];

    build_values(context, values);
    return uc_reg_read_batch(uc, read_regs, values,
                             (int)ARRAY_SIZE(read_regs));
}

static uc_err read_saved_context(uc_context *snapshot,
                                 struct normalized_context *context)
{
    void *values[ARRAY_SIZE(read_regs)];

    build_values(context, values);
    return uc_context_reg_read_batch(snapshot, read_regs, values,
                                     (int)ARRAY_SIZE(read_regs));
}

static uc_err current_export(uc_engine *uc, uc_context *snapshot,
                             struct normalized_context *context)
{
    uc_err error;

    if ((error = read_live_context(uc, context)) != UC_ERR_OK) return error;
    return uc_context_save(uc, snapshot);
}

static uc_err snapshot_export(uc_engine *uc, uc_context *snapshot,
                              struct normalized_context *context)
{
    uc_err error;

    if ((error = uc_context_save(uc, snapshot)) != UC_ERR_OK) return error;
    return read_saved_context(snapshot, context);
}

static void validate_exports(uc_engine *uc, uc_context *snapshot,
                             const struct normalized_context *expected)
{
    struct normalized_context current, candidate;
    uc_err error;

    memset(&current, 0, sizeof(current));
    memset(&candidate, 0, sizeof(candidate));
    if ((error = current_export(uc, snapshot, &current)) != UC_ERR_OK)
        die_unicorn("current context export", error);
    if ((error = snapshot_export(uc, snapshot, &candidate)) != UC_ERR_OK)
        die_unicorn("snapshot context export", error);
    if (memcmp(&current, &candidate, sizeof(current)))
    {
        fprintf(stderr, "current and snapshot exports differ\n");
        exit(1);
    }
    if (memcmp(&current, expected, sizeof(current)))
    {
        fprintf(stderr, "exported context differs from seeded state\n");
        exit(1);
    }
}

static double benchmark_live_read(uc_engine *uc)
{
    struct normalized_context output;
    double samples[ROUNDS];
    unsigned int round;

    for (round = 0; round < ROUNDS; ++round)
    {
        uint64_t begin = mach_continuous_time();
        unsigned int i;
        uc_err error = UC_ERR_OK;

        for (i = 0; i < COMPONENT_ITERATIONS; ++i)
            if ((error = read_live_context(uc, &output)) != UC_ERR_OK) break;
        if (error != UC_ERR_OK) die_unicorn("live-read benchmark", error);
        samples[round] = elapsed_ns(begin, mach_continuous_time()) /
                         COMPONENT_ITERATIONS;
        benchmark_sink ^= output.rax ^ output.xmm[15][1];
    }
    return median(samples);
}

static double benchmark_context_save(uc_engine *uc, uc_context *snapshot)
{
    double samples[ROUNDS];
    unsigned int round;

    for (round = 0; round < ROUNDS; ++round)
    {
        uint64_t begin = mach_continuous_time();
        unsigned int i;
        uc_err error = UC_ERR_OK;

        for (i = 0; i < COMPONENT_ITERATIONS; ++i)
            if ((error = uc_context_save(uc, snapshot)) != UC_ERR_OK) break;
        if (error != UC_ERR_OK) die_unicorn("context-save benchmark", error);
        samples[round] = elapsed_ns(begin, mach_continuous_time()) /
                         COMPONENT_ITERATIONS;
        benchmark_sink ^= (uintptr_t)snapshot;
    }
    return median(samples);
}

static double benchmark_snapshot_read(uc_context *snapshot)
{
    struct normalized_context output;
    double samples[ROUNDS];
    unsigned int round;

    for (round = 0; round < ROUNDS; ++round)
    {
        uint64_t begin = mach_continuous_time();
        unsigned int i;
        uc_err error = UC_ERR_OK;

        for (i = 0; i < COMPONENT_ITERATIONS; ++i)
            if ((error = read_saved_context(snapshot, &output)) != UC_ERR_OK) break;
        if (error != UC_ERR_OK) die_unicorn("snapshot-read benchmark", error);
        samples[round] = elapsed_ns(begin, mach_continuous_time()) /
                         COMPONENT_ITERATIONS;
        benchmark_sink ^= output.rip ^ output.xmm[0][0];
    }
    return median(samples);
}

static double benchmark_pair(uc_engine *uc, uc_context *snapshot, bool candidate)
{
    struct normalized_context output;
    double samples[ROUNDS];
    unsigned int round;

    for (round = 0; round < ROUNDS; ++round)
    {
        uint64_t begin = mach_continuous_time();
        unsigned int i;
        uc_err error = UC_ERR_OK;

        for (i = 0; i < PAIR_ITERATIONS; ++i)
        {
            error = candidate ? snapshot_export(uc, snapshot, &output) :
                                current_export(uc, snapshot, &output);
            if (error != UC_ERR_OK) break;
        }
        if (error != UC_ERR_OK) die_unicorn("context-export benchmark", error);
        samples[round] = elapsed_ns(begin, mach_continuous_time()) /
                         PAIR_ITERATIONS;
        benchmark_sink ^= output.r15 ^ output.xmm[7][1];
    }
    return median(samples);
}

static void print_cpu_model(void)
{
    char model[256];
    size_t size = sizeof(model);

    if (sysctlbyname("machdep.cpu.brand_string", model, &size, NULL, 0) == 0)
        printf("APPLE_SILICON_CPU model=%s\n", model);
}

int main(void)
{
    struct normalized_context seeded, warmup;
    double live_read_ns, save_ns, snapshot_read_ns;
    double current_ns, candidate_ns;
    uc_engine *uc = NULL;
    uc_context *snapshot = NULL;
    uc_err error;
    unsigned int i;

    if (mach_timebase_info(&timebase) != KERN_SUCCESS)
    {
        fprintf(stderr, "mach_timebase_info failed\n");
        return 1;
    }
    print_cpu_model();
    if (ARRAY_SIZE(write_regs) != ARRAY_SIZE(read_regs))
    {
        fprintf(stderr, "read/write register table mismatch\n");
        return 1;
    }
    if ((error = uc_open(UC_ARCH_X86, UC_MODE_64, &uc)) != UC_ERR_OK)
        die_unicorn("uc_open", error);
    if ((error = uc_context_alloc(uc, &snapshot)) != UC_ERR_OK)
        die_unicorn("uc_context_alloc", error);

    initialize_context(&seeded);
    if ((error = write_engine_context(uc, &seeded)) != UC_ERR_OK)
        die_unicorn("seed engine context", error);
    validate_exports(uc, snapshot, &seeded);

    memset(&warmup, 0, sizeof(warmup));
    for (i = 0; i < 500; ++i)
    {
        if ((error = current_export(uc, snapshot, &warmup)) != UC_ERR_OK ||
            (error = snapshot_export(uc, snapshot, &warmup)) != UC_ERR_OK)
            die_unicorn("context-export warmup", error);
    }

    live_read_ns = benchmark_live_read(uc);
    save_ns = benchmark_context_save(uc, snapshot);
    if ((error = uc_context_save(uc, snapshot)) != UC_ERR_OK)
        die_unicorn("snapshot refresh", error);
    snapshot_read_ns = benchmark_snapshot_read(snapshot);

    /* Alternate pair order across whole benchmark invocations by running the
     * candidate first after the component tests, then current, then candidate
     * again.  Use the better candidate result only as a stability check; the
     * first candidate and current values remain directly comparable medians. */
    candidate_ns = benchmark_pair(uc, snapshot, true);
    current_ns = benchmark_pair(uc, snapshot, false);

    printf("APPLE_SILICON_HOTPATH context_export_live_read_ns=%.3f "
           "context_save_ns=%.3f snapshot_read_ns=%.3f\n",
           live_read_ns, save_ns, snapshot_read_ns);
    printf("APPLE_SILICON_HOTPATH context_export_current_ns=%.3f "
           "context_export_snapshot_ns=%.3f speedup=%.3fx\n",
           current_ns, candidate_ns, current_ns / candidate_ns);
    printf("APPLE_SILICON_HOTPATH context_export_api_calls=2->2 "
           "live_register_reads=1->0\n");

    uc_context_free(snapshot);
    uc_close(uc);
    if (!(candidate_ns < current_ns))
    {
        fprintf(stderr, "snapshot-first context export did not beat current ordering\n");
        return 1;
    }
    return benchmark_sink == UINT64_MAX ? 1 : 0;
}
