/*
 * Apple Silicon nanosecond benchmark for the xtajit64 Unicorn
 * context-transfer API boundary.
 *
 * The benchmark first verifies scalar and batch register-transfer semantics,
 * then reports the median cost of a full write/read boundary.
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
#define ROUNDS 9
#define CONTEXT_ITERATIONS 12000u

struct x64_context
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

static const int integer_regs[] =
{
    UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
    UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_RBP, UC_X86_REG_RSP,
    UC_X86_REG_R8,  UC_X86_REG_R9,  UC_X86_REG_R10, UC_X86_REG_R11,
    UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15,
    UC_X86_REG_RIP, UC_X86_REG_EFLAGS,
};

static const int xmm_regs[] =
{
    UC_X86_REG_XMM0,  UC_X86_REG_XMM1,  UC_X86_REG_XMM2,  UC_X86_REG_XMM3,
    UC_X86_REG_XMM4,  UC_X86_REG_XMM5,  UC_X86_REG_XMM6,  UC_X86_REG_XMM7,
    UC_X86_REG_XMM8,  UC_X86_REG_XMM9,  UC_X86_REG_XMM10, UC_X86_REG_XMM11,
    UC_X86_REG_XMM12, UC_X86_REG_XMM13, UC_X86_REG_XMM14, UC_X86_REG_XMM15,
};

static const int context_write_regs[] =
{
    UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
    UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_RBP, UC_X86_REG_RSP,
    UC_X86_REG_R8,  UC_X86_REG_R9,  UC_X86_REG_R10, UC_X86_REG_R11,
    UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15,
    UC_X86_REG_RIP, UC_X86_REG_EFLAGS, UC_X86_REG_GS_BASE, UC_X86_REG_MXCSR,
    UC_X86_REG_XMM0,  UC_X86_REG_XMM1,  UC_X86_REG_XMM2,  UC_X86_REG_XMM3,
    UC_X86_REG_XMM4,  UC_X86_REG_XMM5,  UC_X86_REG_XMM6,  UC_X86_REG_XMM7,
    UC_X86_REG_XMM8,  UC_X86_REG_XMM9,  UC_X86_REG_XMM10, UC_X86_REG_XMM11,
    UC_X86_REG_XMM12, UC_X86_REG_XMM13, UC_X86_REG_XMM14, UC_X86_REG_XMM15,
};

static const int context_read_regs[] =
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

static volatile uint64_t benchmark_sink;
static mach_timebase_info_data_t timebase;

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

static uc_err scalar_write(uc_engine *uc, const struct x64_context *context,
                           uint64_t gs_base)
{
    const uint64_t *values = &context->rax;
    uc_err error;
    size_t i;

    for (i = 0; i < ARRAY_SIZE(integer_regs); ++i)
        if ((error = uc_reg_write(uc, integer_regs[i], &values[i])) != UC_ERR_OK)
            return error;
    if ((error = uc_reg_write(uc, UC_X86_REG_GS_BASE, &gs_base)) != UC_ERR_OK)
        return error;
    if ((error = uc_reg_write(uc, UC_X86_REG_MXCSR, &context->mxcsr)) != UC_ERR_OK)
        return error;
    for (i = 0; i < ARRAY_SIZE(xmm_regs); ++i)
        if ((error = uc_reg_write(uc, xmm_regs[i], context->xmm[i])) != UC_ERR_OK)
            return error;
    return UC_ERR_OK;
}

static uc_err scalar_read(uc_engine *uc, struct x64_context *context)
{
    uint64_t *values = &context->rax;
    uc_err error;
    size_t i;

    for (i = 0; i < ARRAY_SIZE(integer_regs); ++i)
        if ((error = uc_reg_read(uc, integer_regs[i], &values[i])) != UC_ERR_OK)
            return error;
    if ((error = uc_reg_read(uc, UC_X86_REG_MXCSR, &context->mxcsr)) != UC_ERR_OK)
        return error;
    for (i = 0; i < ARRAY_SIZE(xmm_regs); ++i)
        if ((error = uc_reg_read(uc, xmm_regs[i], context->xmm[i])) != UC_ERR_OK)
            return error;
    return UC_ERR_OK;
}

static uc_err batch_write(uc_engine *uc, const struct x64_context *context,
                          uint64_t gs_base)
{
    const uint64_t *integer_values = &context->rax;
    void *values[ARRAY_SIZE(context_write_regs)];
    size_t i, index = 0;

    for (i = 0; i < ARRAY_SIZE(integer_regs); ++i)
        values[index++] = (void *)&integer_values[i];
    values[index++] = &gs_base;
    values[index++] = (void *)&context->mxcsr;
    for (i = 0; i < ARRAY_SIZE(xmm_regs); ++i)
        values[index++] = (void *)context->xmm[i];
    return uc_reg_write_batch(uc, context_write_regs, values,
                              (int)ARRAY_SIZE(context_write_regs));
}

static uc_err batch_read(uc_engine *uc, struct x64_context *context)
{
    uint64_t *integer_values = &context->rax;
    void *values[ARRAY_SIZE(context_read_regs)];
    size_t i, index = 0;

    for (i = 0; i < ARRAY_SIZE(integer_regs); ++i)
        values[index++] = &integer_values[i];
    values[index++] = &context->mxcsr;
    for (i = 0; i < ARRAY_SIZE(xmm_regs); ++i)
        values[index++] = context->xmm[i];
    return uc_reg_read_batch(uc, context_read_regs, values,
                             (int)ARRAY_SIZE(context_read_regs));
}

static void initialize_context(struct x64_context *context)
{
    uint64_t *values = &context->rax;
    size_t i, j;

    memset(context, 0, sizeof(*context));
    for (i = 0; i < ARRAY_SIZE(integer_regs); ++i)
        values[i] = UINT64_C(0x100000001) * (i + 1);
    context->rsp = UINT64_C(0x0000000100004000);
    context->rip = UINT64_C(0x0000000100001000);
    context->eflags = 0x202;
    context->mxcsr = 0x1f80;
    for (i = 0; i < ARRAY_SIZE(context->xmm); ++i)
        for (j = 0; j < ARRAY_SIZE(context->xmm[i]); ++j)
            context->xmm[i][j] =
                UINT64_C(0x9e3779b97f4a7c15) * (i * 2 + j + 1);
}

static void validate_context_transfer(uc_engine *uc,
                                      const struct x64_context *input,
                                      uint64_t gs_base)
{
    struct x64_context scalar_state, batch_state;
    uint64_t observed_gs = 0;
    uc_err error;

    memset(&scalar_state, 0, sizeof(scalar_state));
    memset(&batch_state, 0, sizeof(batch_state));
    if ((error = scalar_write(uc, input, gs_base)) != UC_ERR_OK)
        die_unicorn("scalar context write", error);
    if ((error = batch_read(uc, &scalar_state)) != UC_ERR_OK)
        die_unicorn("batch context read", error);
    if ((error = batch_write(uc, input, gs_base)) != UC_ERR_OK)
        die_unicorn("batch context write", error);
    if ((error = scalar_read(uc, &batch_state)) != UC_ERR_OK)
        die_unicorn("scalar context read", error);
    if (memcmp(&scalar_state, &batch_state, sizeof(scalar_state)))
    {
        fprintf(stderr, "scalar and batch context state differ\n");
        exit(1);
    }
    if ((error = uc_reg_read(uc, UC_X86_REG_GS_BASE, &observed_gs)) != UC_ERR_OK)
        die_unicorn("GS base read", error);
    if (observed_gs != gs_base)
    {
        fprintf(stderr, "GS base mismatch: %#llx != %#llx\n",
                (unsigned long long)observed_gs,
                (unsigned long long)gs_base);
        exit(1);
    }
}

static double benchmark_context(uc_engine *uc,
                                const struct x64_context *input,
                                uint64_t gs_base, bool batch)
{
    struct x64_context output;
    double samples[ROUNDS];
    unsigned int round;

    for (round = 0; round < ROUNDS; ++round)
    {
        uint64_t begin = mach_continuous_time();
        unsigned int i;
        uc_err error = UC_ERR_OK;

        for (i = 0; i < CONTEXT_ITERATIONS; ++i)
        {
            if (batch)
            {
                if ((error = batch_write(uc, input, gs_base)) != UC_ERR_OK ||
                    (error = batch_read(uc, &output)) != UC_ERR_OK)
                    break;
            }
            else
            {
                if ((error = scalar_write(uc, input, gs_base)) != UC_ERR_OK ||
                    (error = scalar_read(uc, &output)) != UC_ERR_OK)
                    break;
            }
        }
        if (error != UC_ERR_OK) die_unicorn("context benchmark", error);
        samples[round] =
            elapsed_ns(begin, mach_continuous_time()) / CONTEXT_ITERATIONS;
        benchmark_sink ^= output.rax ^ output.xmm[15][1];
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
    struct x64_context context;
    uint64_t gs_base = UINT64_C(0x0000000101000000);
    double scalar_ns, batch_ns;
    uc_engine *uc = NULL;
    uc_err error;
    size_t i;

    if (mach_timebase_info(&timebase) != KERN_SUCCESS)
    {
        fprintf(stderr, "mach_timebase_info failed\n");
        return 1;
    }
    print_cpu_model();

    if ((error = uc_open(UC_ARCH_X86, UC_MODE_64, &uc)) != UC_ERR_OK)
        die_unicorn("uc_open", error);
    initialize_context(&context);
    validate_context_transfer(uc, &context, gs_base);
    for (i = 0; i < 100; ++i)
    {
        if ((error = scalar_write(uc, &context, gs_base)) != UC_ERR_OK ||
            (error = scalar_read(uc, &context)) != UC_ERR_OK ||
            (error = batch_write(uc, &context, gs_base)) != UC_ERR_OK ||
            (error = batch_read(uc, &context)) != UC_ERR_OK)
            die_unicorn("context warmup", error);
    }
    scalar_ns = benchmark_context(uc, &context, gs_base, false);
    batch_ns = benchmark_context(uc, &context, gs_base, true);
    uc_close(uc);

    printf("APPLE_SILICON_HOTPATH context_scalar_ns=%.3f "
           "context_batch_ns=%.3f speedup=%.3fx\n",
           scalar_ns, batch_ns, scalar_ns / batch_ns);
    printf("APPLE_SILICON_HOTPATH api_calls_per_boundary=71->2 "
           "removed=69\n");

    if (!(batch_ns < scalar_ns))
    {
        fprintf(stderr, "batch context transfer did not beat scalar calls\n");
        return 1;
    }
    return benchmark_sink == UINT64_MAX ? 1 : 0;
}
