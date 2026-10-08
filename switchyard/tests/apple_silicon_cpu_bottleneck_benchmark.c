/*
 * Apple Silicon CPU bottleneck benchmark for the native ARM64 CPU provider.
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <emmintrin.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NOINLINE __attribute__((noinline))

#define DEFAULT_TARGET_MS 80u
#define DEFAULT_TRIALS 9u
#define MAX_TRIALS 101u
#define MAX_CALIBRATION_STEPS 32u
#define MAX_ITERATIONS UINT64_C(1000000000)
#define BRANCH_VALUES 4096u
#define BRANCH_OPS 256u
#define INDIRECT_OPS 256u
#define POINTER_OPS 256u
#define LOAD_LINES 256u
#define LOAD_OPS 256u
#define FORWARD_SLOTS 64u
#define FORWARD_OPS 256u
#define CACHE_LINE_BYTES 128u

static volatile uint64_t benchmark_sink;
static LARGE_INTEGER qpc_frequency;

typedef uint64_t (*benchmark_runner)(uint64_t iterations, void *context);
typedef uint64_t (*indirect_target)(uint64_t value);
typedef uint64_t (__cdecl *generated_function)(void);

struct benchmark_case
{
    const char *name;
    const char *category;
    const char *unit;
    uint64_t operations_per_iteration;
    uint64_t maximum_iterations;
    benchmark_runner runner;
    void *context;
};

struct branch_context
{
    uint32_t *values;
    size_t mask;
};

struct indirect_context
{
    indirect_target *targets;
    unsigned int target_mask;
};

struct pointer_context
{
    uint32_t *next;
    uint32_t start;
};

struct load_context
{
    unsigned char *storage;
    size_t offset;
};

struct forwarding_context
{
    unsigned char *storage;
    size_t load_offset;
};

struct generated_code_context
{
    unsigned char *memory;
    SIZE_T allocation_size;
    size_t code_size;
    uint64_t instruction_count;
    generated_function function;
};

struct jit_publish_context
{
    unsigned char *memory;
    SIZE_T allocation_size;
    generated_function function;
};

static uint64_t read_qpc(void)
{
    LARGE_INTEGER value;

    if (!QueryPerformanceCounter(&value))
    {
        fprintf(stderr, "QueryPerformanceCounter failed: %lu\n", GetLastError());
        exit(1);
    }
    return (uint64_t)value.QuadPart;
}

static uint64_t elapsed_ticks(uint64_t start, uint64_t end)
{
    if (end < start)
    {
        fprintf(stderr, "performance counter moved backwards\n");
        exit(1);
    }
    return end - start;
}

static uint64_t parse_environment_value(const char *name, uint64_t fallback,
                                        uint64_t minimum, uint64_t maximum)
{
    const char *text = getenv(name);
    char *end;
    unsigned long long value;

    if (!text || !*text) return fallback;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || *end || value < minimum || value > maximum)
    {
        fprintf(stderr, "%s must be an integer from %" PRIu64 " through %" PRIu64 "\n",
                name, minimum, maximum);
        exit(2);
    }
    return (uint64_t)value;
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;

    return a > b ? 1 : a < b ? -1 : 0;
}

static double percentile(const double *sorted, unsigned int count, unsigned int numerator)
{
    uint64_t index = ((uint64_t)count * numerator + 99u) / 100u;

    if (!index) index = 1;
    if (index > count) index = count;
    return sorted[index - 1];
}

static uint64_t measure_ticks(const struct benchmark_case *benchmark,
                              uint64_t iterations, uint64_t *checksum)
{
    uint64_t start = read_qpc();
    uint64_t result = benchmark->runner(iterations, benchmark->context);
    uint64_t end = read_qpc();

    benchmark_sink ^= result;
    if (checksum) *checksum = result;
    return elapsed_ticks(start, end);
}

static uint64_t calibrate_iterations(const struct benchmark_case *benchmark,
                                     uint64_t target_ticks)
{
    uint64_t iterations = 1;
    uint64_t minimum_ticks = target_ticks / 8u;
    unsigned int step;

    if (!minimum_ticks) minimum_ticks = 1;
    for (step = 0; step < MAX_CALIBRATION_STEPS; ++step)
    {
        uint64_t elapsed = measure_ticks(benchmark, iterations, NULL);

        if (elapsed >= minimum_ticks)
        {
            long double estimate = (long double)iterations * target_ticks / elapsed;
            uint64_t calibrated;

            if (estimate < 1.0L) estimate = 1.0L;
            if (estimate > (long double)benchmark->maximum_iterations)
                estimate = (long double)benchmark->maximum_iterations;
            calibrated = (uint64_t)(estimate + 0.5L);
            if (!calibrated) calibrated = 1;
            return calibrated;
        }
        if (iterations >= benchmark->maximum_iterations / 2u)
            return benchmark->maximum_iterations;
        iterations *= 2u;
    }
    return iterations;
}

#define DEPENDENT_STEP(x) \
    ((x) = (x) * UINT64_C(0x9e3779b185ebca87) + UINT64_C(0xda942042e4dd58b5))

static NOINLINE uint64_t run_dependent_muladd(uint64_t iterations, void *context)
{
    uint64_t value = *(const uint64_t *)context;
    uint64_t iteration;
    unsigned int round;

    for (iteration = 0; iteration < iterations; ++iteration)
    {
        for (round = 0; round < 64u; ++round)
        {
            DEPENDENT_STEP(value); DEPENDENT_STEP(value);
            DEPENDENT_STEP(value); DEPENDENT_STEP(value);
            DEPENDENT_STEP(value); DEPENDENT_STEP(value);
            DEPENDENT_STEP(value); DEPENDENT_STEP(value);
        }
    }
    return value;
}

static NOINLINE uint64_t run_independent_muladd_8(uint64_t iterations, void *context)
{
    uint64_t seed = *(const uint64_t *)context;
    uint64_t a0 = seed + 0u, a1 = seed + 1u, a2 = seed + 2u, a3 = seed + 3u;
    uint64_t a4 = seed + 4u, a5 = seed + 5u, a6 = seed + 6u, a7 = seed + 7u;
    uint64_t iteration;
    unsigned int round;

    for (iteration = 0; iteration < iterations; ++iteration)
    {
        for (round = 0; round < 64u; ++round)
        {
            DEPENDENT_STEP(a0); DEPENDENT_STEP(a1);
            DEPENDENT_STEP(a2); DEPENDENT_STEP(a3);
            DEPENDENT_STEP(a4); DEPENDENT_STEP(a5);
            DEPENDENT_STEP(a6); DEPENDENT_STEP(a7);
        }
    }
    return a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7;
}

static NOINLINE uint64_t run_branch_stream(uint64_t iterations, void *opaque)
{
    const struct branch_context *context = opaque;
    uint64_t sum = UINT64_C(0x123456789abcdef0);
    uint64_t operation = 0;
    uint64_t iteration;
    unsigned int inner;

    for (iteration = 0; iteration < iterations; ++iteration)
    {
        for (inner = 0; inner < BRANCH_OPS; ++inner, ++operation)
        {
            uint32_t condition = context->values[(size_t)operation & context->mask];
            uint64_t delta = ((uint64_t)condition >> 1) | 1u;

            __asm__ volatile(
                "testl $1, %[condition]\n\t"
                "jz 1f\n\t"
                "addq %[delta], %[sum]\n\t"
                "jmp 2f\n"
                "1:\n\t"
                "subq %[delta], %[sum]\n"
                "2:"
                : [sum] "+r" (sum)
                : [condition] "r" (condition), [delta] "r" (delta)
                : "cc");
        }
    }
    return sum;
}

#define DEFINE_INDIRECT_TARGET(number, constant) \
    static NOINLINE uint64_t indirect_target_##number(uint64_t value) \
    { \
        __asm__ volatile("" : "+r" (value)); \
        return (value ^ UINT64_C(constant)) + UINT64_C(0x9e3779b97f4a7c15); \
    }

DEFINE_INDIRECT_TARGET(0,  0x243f6a8885a308d3)
DEFINE_INDIRECT_TARGET(1,  0x13198a2e03707344)
DEFINE_INDIRECT_TARGET(2,  0xa4093822299f31d0)
DEFINE_INDIRECT_TARGET(3,  0x082efa98ec4e6c89)
DEFINE_INDIRECT_TARGET(4,  0x452821e638d01377)
DEFINE_INDIRECT_TARGET(5,  0xbe5466cf34e90c6c)
DEFINE_INDIRECT_TARGET(6,  0xc0ac29b7c97c50dd)
DEFINE_INDIRECT_TARGET(7,  0x3f84d5b5b5470917)
DEFINE_INDIRECT_TARGET(8,  0x9216d5d98979fb1b)
DEFINE_INDIRECT_TARGET(9,  0xd1310ba698dfb5ac)
DEFINE_INDIRECT_TARGET(10, 0x2ffd72dbd01adfb7)
DEFINE_INDIRECT_TARGET(11, 0xb8e1afed6a267e96)
DEFINE_INDIRECT_TARGET(12, 0xba7c9045f12c7f99)
DEFINE_INDIRECT_TARGET(13, 0x24a19947b3916cf7)
DEFINE_INDIRECT_TARGET(14, 0x0801f2e2858efc16)
DEFINE_INDIRECT_TARGET(15, 0x636920d871574e69)

static NOINLINE uint64_t run_indirect_targets(uint64_t iterations, void *opaque)
{
    const struct indirect_context *context = opaque;
    uint64_t value = UINT64_C(0x6a09e667f3bcc909);
    uint32_t selector = 1u;
    uint64_t iteration;
    unsigned int inner;

    for (iteration = 0; iteration < iterations; ++iteration)
    {
        for (inner = 0; inner < INDIRECT_OPS; ++inner)
        {
            indirect_target target;

            selector = selector * 1664525u + 1013904223u;
            target = context->targets[(selector >> 16) & context->target_mask];
            value = target(value);
        }
    }
    return value;
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static uint32_t *build_pointer_cycle(size_t count, uint32_t seed)
{
    uint32_t *next;
    uint32_t *permutation;
    size_t i;

    if (!count || count > UINT32_MAX || count > SIZE_MAX / sizeof(*next)) return NULL;
    next = malloc(count * sizeof(*next));
    permutation = malloc(count * sizeof(*permutation));
    if (!next || !permutation)
    {
        free(next);
        free(permutation);
        return NULL;
    }
    for (i = 0; i < count; ++i) permutation[i] = (uint32_t)i;
    for (i = count - 1; i > 0; --i)
    {
        size_t other = (size_t)xorshift32(&seed) % (i + 1u);
        uint32_t temporary = permutation[i];

        permutation[i] = permutation[other];
        permutation[other] = temporary;
    }
    for (i = 0; i < count; ++i)
        next[permutation[i]] = permutation[(i + 1u) % count];
    free(permutation);
    return next;
}

static NOINLINE uint64_t run_pointer_chase(uint64_t iterations, void *opaque)
{
    const struct pointer_context *context = opaque;
    uint32_t index = context->start;
    uint64_t iteration;
    unsigned int inner;

    for (iteration = 0; iteration < iterations; ++iteration)
        for (inner = 0; inner < POINTER_OPS; ++inner)
            index = context->next[index];
    return index;
}

static NOINLINE uint64_t run_line_load(uint64_t iterations, void *opaque)
{
    const struct load_context *context = opaque;
    uint64_t sum = UINT64_C(0x510e527fade682d1);
    uint64_t iteration;
    unsigned int inner;

    for (iteration = 0; iteration < iterations; ++iteration)
    {
        for (inner = 0; inner < LOAD_OPS; ++inner)
        {
            const unsigned char *address = context->storage +
                    ((size_t)inner * CACHE_LINE_BYTES) + context->offset;
            __m128i value;

            __asm__ volatile("movdqu (%1), %0" : "=x" (value) : "r" (address) : "memory");
            sum ^= (uint64_t)_mm_cvtsi128_si64(value);
            sum += (uint64_t)_mm_cvtsi128_si64(_mm_srli_si128(value, 8));
        }
    }
    return sum;
}

static NOINLINE uint64_t run_store_load_forwarding(uint64_t iterations, void *opaque)
{
    const struct forwarding_context *context = opaque;
    uint64_t value = UINT64_C(0x1f83d9abfb41bd6b);
    uint64_t iteration;
    unsigned int inner;

    for (iteration = 0; iteration < iterations; ++iteration)
    {
        for (inner = 0; inner < FORWARD_OPS; ++inner)
        {
            unsigned char *address = context->storage +
                    ((size_t)(inner & (FORWARD_SLOTS - 1u)) * 16u);
            uint64_t loaded;

            if (!context->load_offset)
            {
                __asm__ volatile(
                    "movq %[value], (%[address])\n\t"
                    "movq (%[address]), %[loaded]"
                    : [loaded] "=r" (loaded)
                    : [value] "r" (value), [address] "r" (address)
                    : "memory");
            }
            else
            {
                const unsigned char *load_address = address + context->load_offset;

                __asm__ volatile(
                    "movq %[value], (%[address])\n\t"
                    "movq (%[load_address]), %[loaded]"
                    : [loaded] "=r" (loaded)
                    : [value] "r" (value), [address] "r" (address),
                      [load_address] "r" (load_address)
                    : "memory");
            }
            value = loaded ^ (value >> 7) ^ UINT64_C(0x5be0cd19137e2179);
        }
    }
    return value;
}

static int protect_code(void *memory, SIZE_T size, DWORD protection)
{
    DWORD previous;

    if (!VirtualProtect(memory, size, protection, &previous))
    {
        fprintf(stderr, "VirtualProtect(%#lx) failed: %lu\n",
                (unsigned long)protection, GetLastError());
        return 0;
    }
    return 1;
}

static int flush_code(void *memory, SIZE_T size)
{
    if (!FlushInstructionCache(GetCurrentProcess(), memory, size))
    {
        fprintf(stderr, "FlushInstructionCache failed: %lu\n", GetLastError());
        return 0;
    }
    return 1;
}

static int build_generated_code(struct generated_code_context *context, size_t requested_size)
{
    SYSTEM_INFO system_info;
    SIZE_T allocation_size;
    size_t position = 0;
    size_t add_count;
    size_t i;

    memset(context, 0, sizeof(*context));
    GetSystemInfo(&system_info);
    allocation_size = (requested_size + system_info.dwPageSize - 1u) &
            ~((SIZE_T)system_info.dwPageSize - 1u);
    context->memory = VirtualAlloc(NULL, allocation_size, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
    if (!context->memory)
    {
        fprintf(stderr, "VirtualAlloc for generated code failed: %lu\n", GetLastError());
        return 0;
    }
    context->allocation_size = allocation_size;

    /* xor eax,eax; repeated add rax,1; ret. */
    context->memory[position++] = 0x31;
    context->memory[position++] = 0xc0;
    add_count = (requested_size - position - 1u) / 4u;
    for (i = 0; i < add_count; ++i)
    {
        context->memory[position++] = 0x48;
        context->memory[position++] = 0x83;
        context->memory[position++] = 0xc0;
        context->memory[position++] = 0x01;
    }
    context->memory[position++] = 0xc3;
    context->code_size = position;
    context->instruction_count = add_count + 2u;
    if (!protect_code(context->memory, context->allocation_size, PAGE_EXECUTE_READ) ||
        !flush_code(context->memory, context->code_size))
        return 0;
    context->function = (generated_function)(uintptr_t)context->memory;
    if (context->function() != add_count)
    {
        fprintf(stderr, "generated code validation failed for %zu bytes\n", requested_size);
        return 0;
    }
    return 1;
}

static void destroy_generated_code(struct generated_code_context *context)
{
    if (context->memory) VirtualFree(context->memory, 0, MEM_RELEASE);
    memset(context, 0, sizeof(*context));
}

static NOINLINE uint64_t run_generated_code(uint64_t iterations, void *opaque)
{
    const struct generated_code_context *context = opaque;
    uint64_t sum = 0;
    uint64_t iteration;

    for (iteration = 0; iteration < iterations; ++iteration)
        sum += context->function();
    return sum;
}

static int build_jit_publish_context(struct jit_publish_context *context)
{
    SYSTEM_INFO system_info;
    uint32_t immediate = 0;

    memset(context, 0, sizeof(*context));
    GetSystemInfo(&system_info);
    context->allocation_size = system_info.dwPageSize;
    context->memory = VirtualAlloc(NULL, context->allocation_size,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!context->memory)
    {
        fprintf(stderr, "VirtualAlloc for JIT publication failed: %lu\n", GetLastError());
        return 0;
    }
    /* mov eax,imm32; ret */
    context->memory[0] = 0xb8;
    memcpy(context->memory + 1, &immediate, sizeof(immediate));
    context->memory[5] = 0xc3;
    if (!protect_code(context->memory, context->allocation_size, PAGE_EXECUTE_READ) ||
        !flush_code(context->memory, 6u))
        return 0;
    context->function = (generated_function)(uintptr_t)context->memory;
    if (context->function() != 0)
    {
        fprintf(stderr, "JIT publication validation failed\n");
        return 0;
    }
    return 1;
}

static void destroy_jit_publish_context(struct jit_publish_context *context)
{
    if (context->memory) VirtualFree(context->memory, 0, MEM_RELEASE);
    memset(context, 0, sizeof(*context));
}

static NOINLINE uint64_t run_jit_publish(uint64_t iterations, void *opaque)
{
    struct jit_publish_context *context = opaque;
    uint64_t sum = 0;
    uint64_t iteration;

    for (iteration = 0; iteration < iterations; ++iteration)
    {
        uint32_t immediate = (uint32_t)iteration;

        if (!protect_code(context->memory, context->allocation_size, PAGE_READWRITE)) exit(1);
        memcpy(context->memory + 1, &immediate, sizeof(immediate));
        if (!protect_code(context->memory, context->allocation_size, PAGE_EXECUTE_READ) ||
            !flush_code(context->memory, 6u))
            exit(1);
        sum += context->function();
    }
    return sum;
}

static void fill_branch_values(uint32_t *predictable, uint32_t *unpredictable)
{
    uint32_t state = 0x6d2b79f5u;
    size_t i;

    for (i = 0; i < BRANCH_VALUES; ++i)
    {
        predictable[i] = (uint32_t)i * 2u + 1u;
        unpredictable[i] = xorshift32(&state);
    }
}

static int benchmark_selected(const char *filter, const char *name)
{
    return !filter || !*filter || !strcmp(filter, "all") || !strcmp(filter, name);
}

static int run_case(const struct benchmark_case *benchmark, uint64_t target_ticks,
                    unsigned int trials, uint64_t run_id)
{
    double *samples;
    uint64_t iterations;
    uint64_t checksum = 0;
    unsigned int trial;

    samples = calloc(trials, sizeof(*samples));
    if (!samples)
    {
        fprintf(stderr, "cannot allocate benchmark samples\n");
        return 0;
    }
    iterations = calibrate_iterations(benchmark, target_ticks);
    measure_ticks(benchmark, iterations, NULL);
    for (trial = 0; trial < trials; ++trial)
    {
        uint64_t ticks = measure_ticks(benchmark, iterations, &checksum);
        long double operations = (long double)iterations *
                benchmark->operations_per_iteration;
        long double nanoseconds = (long double)ticks * 1000000000.0L /
                (uint64_t)qpc_frequency.QuadPart;

        samples[trial] = (double)(nanoseconds / operations);
        if (!(samples[trial] > 0.0) || !isfinite(samples[trial]))
        {
            fprintf(stderr, "invalid measurement for %s\n", benchmark->name);
            free(samples);
            return 0;
        }
    }
    qsort(samples, trials, sizeof(*samples), compare_double);
    printf("apple_cpu_metric schema=1 run=%" PRIu64 " status=ok case=%s category=%s unit=%s "
           "trials=%u iterations=%" PRIu64 " operations_per_iteration=%" PRIu64 " "
           "min_ns=%.6f p50_ns=%.6f p95_ns=%.6f max_ns=%.6f checksum=%" PRIu64 "\n",
           run_id, benchmark->name, benchmark->category, benchmark->unit, trials, iterations,
           benchmark->operations_per_iteration, samples[0], percentile(samples, trials, 50),
           percentile(samples, trials, 95), samples[trials - 1], checksum);
    free(samples);
    return 1;
}

int main(void)
{
    static indirect_target all_targets[16] =
    {
        indirect_target_0, indirect_target_1, indirect_target_2, indirect_target_3,
        indirect_target_4, indirect_target_5, indirect_target_6, indirect_target_7,
        indirect_target_8, indirect_target_9, indirect_target_10, indirect_target_11,
        indirect_target_12, indirect_target_13, indirect_target_14, indirect_target_15,
    };
    indirect_target single_target[1] = {indirect_target_0};
    uint64_t seed = UINT64_C(0xbb67ae8584caa73b);
    uint32_t *predictable = NULL, *unpredictable = NULL;
    unsigned char *load_storage = NULL, *forward_storage = NULL;
    struct branch_context predictable_branch, unpredictable_branch;
    struct indirect_context one_indirect, sixteen_indirect;
    struct pointer_context pointer_8k, pointer_256k, pointer_16m;
    struct load_context aligned_load, cross_128b_load;
    struct forwarding_context exact_forward, partial_forward;
    struct generated_code_context code_4k, code_64k, code_256k;
    struct jit_publish_context jit_publish;
    struct benchmark_case benchmarks[17];
    const char *filter = getenv("APPLE_CPU_BENCH_CASE");
    uint64_t target_ms, target_ticks, run_id;
    unsigned int trials;
    size_t benchmark_count = 0;
    size_t selected_count = 0;
    size_t i;
    int result = 1;

    memset(&pointer_8k, 0, sizeof(pointer_8k));
    memset(&pointer_256k, 0, sizeof(pointer_256k));
    memset(&pointer_16m, 0, sizeof(pointer_16m));
    memset(&code_4k, 0, sizeof(code_4k));
    memset(&code_64k, 0, sizeof(code_64k));
    memset(&code_256k, 0, sizeof(code_256k));
    memset(&jit_publish, 0, sizeof(jit_publish));
    if (!QueryPerformanceFrequency(&qpc_frequency) || qpc_frequency.QuadPart <= 0)
    {
        fprintf(stderr, "QueryPerformanceFrequency failed\n");
        return 1;
    }
    run_id = parse_environment_value("APPLE_CPU_BENCH_RUN", 0u, 0u, UINT32_MAX);
    target_ms = parse_environment_value("APPLE_CPU_BENCH_TARGET_MS", DEFAULT_TARGET_MS,
                                        10u, 10000u);
    trials = (unsigned int)parse_environment_value("APPLE_CPU_BENCH_TRIALS", DEFAULT_TRIALS,
                                                    3u, MAX_TRIALS);
    target_ticks = (uint64_t)(((long double)qpc_frequency.QuadPart * target_ms) / 1000.0L);
    if (!target_ticks) target_ticks = 1;

    predictable = malloc(BRANCH_VALUES * sizeof(*predictable));
    unpredictable = malloc(BRANCH_VALUES * sizeof(*unpredictable));
    load_storage = _aligned_malloc((LOAD_LINES + 1u) * CACHE_LINE_BYTES, CACHE_LINE_BYTES);
    forward_storage = _aligned_malloc(FORWARD_SLOTS * 16u + 16u, 16u);
    if (!predictable || !unpredictable || !load_storage || !forward_storage)
    {
        fprintf(stderr, "cannot allocate benchmark data\n");
        goto done;
    }
    fill_branch_values(predictable, unpredictable);
    for (i = 0; i < (LOAD_LINES + 1u) * CACHE_LINE_BYTES; ++i)
        load_storage[i] = (unsigned char)(i * 131u + 17u);
    for (i = 0; i < FORWARD_SLOTS * 16u + 16u; ++i)
        forward_storage[i] = (unsigned char)(i * 29u + 3u);

    pointer_8k.next = build_pointer_cycle(8u * 1024u / sizeof(uint32_t), 0x12345678u);
    pointer_256k.next = build_pointer_cycle(256u * 1024u / sizeof(uint32_t), 0x9abcdef0u);
    pointer_16m.next = build_pointer_cycle(16u * 1024u * 1024u / sizeof(uint32_t),
                                           0x31415926u);
    pointer_8k.start = pointer_256k.start = pointer_16m.start = 0;
    if (!pointer_8k.next || !pointer_256k.next || !pointer_16m.next)
    {
        fprintf(stderr, "cannot allocate pointer-chase working sets\n");
        goto done;
    }
    if (!build_generated_code(&code_4k, 4u * 1024u) ||
        !build_generated_code(&code_64k, 64u * 1024u) ||
        !build_generated_code(&code_256k, 256u * 1024u) ||
        !build_jit_publish_context(&jit_publish))
        goto done;

    predictable_branch.values = predictable;
    predictable_branch.mask = BRANCH_VALUES - 1u;
    unpredictable_branch.values = unpredictable;
    unpredictable_branch.mask = BRANCH_VALUES - 1u;
    one_indirect.targets = single_target;
    one_indirect.target_mask = 0u;
    sixteen_indirect.targets = all_targets;
    sixteen_indirect.target_mask = 15u;
    aligned_load.storage = load_storage;
    aligned_load.offset = 0u;
    cross_128b_load.storage = load_storage;
    cross_128b_load.offset = CACHE_LINE_BYTES - 8u;
    exact_forward.storage = forward_storage;
    exact_forward.load_offset = 0u;
    partial_forward.storage = forward_storage;
    partial_forward.load_offset = 1u;

#define ADD_CASE(case_name, case_category, case_unit, ops, max_iters, function, state) \
    do \
    { \
        benchmarks[benchmark_count].name = case_name; \
        benchmarks[benchmark_count].category = case_category; \
        benchmarks[benchmark_count].unit = case_unit; \
        benchmarks[benchmark_count].operations_per_iteration = ops; \
        benchmarks[benchmark_count].maximum_iterations = max_iters; \
        benchmarks[benchmark_count].runner = function; \
        benchmarks[benchmark_count].context = state; \
        ++benchmark_count; \
    } while (0)

    ADD_CASE("dependent_muladd", "execution", "guest_op", 512u, MAX_ITERATIONS,
             run_dependent_muladd, &seed);
    ADD_CASE("independent_muladd_8", "execution", "guest_op", 512u, MAX_ITERATIONS,
             run_independent_muladd_8, &seed);
    ADD_CASE("branch_predictable", "branch", "decision", BRANCH_OPS, MAX_ITERATIONS,
             run_branch_stream, &predictable_branch);
    ADD_CASE("branch_unpredictable", "branch", "decision", BRANCH_OPS, MAX_ITERATIONS,
             run_branch_stream, &unpredictable_branch);
    ADD_CASE("indirect_targets_1", "branch", "indirect_call", INDIRECT_OPS,
             MAX_ITERATIONS, run_indirect_targets, &one_indirect);
    ADD_CASE("indirect_targets_16", "branch", "indirect_call", INDIRECT_OPS,
             MAX_ITERATIONS, run_indirect_targets, &sixteen_indirect);
    ADD_CASE("pointer_chase_8k", "memory", "dependent_load", POINTER_OPS,
             MAX_ITERATIONS, run_pointer_chase, &pointer_8k);
    ADD_CASE("pointer_chase_256k", "memory", "dependent_load", POINTER_OPS,
             MAX_ITERATIONS, run_pointer_chase, &pointer_256k);
    ADD_CASE("pointer_chase_16m", "memory", "dependent_load", POINTER_OPS,
             MAX_ITERATIONS, run_pointer_chase, &pointer_16m);
    ADD_CASE("line_load_aligned", "memory", "load_16b", LOAD_OPS, MAX_ITERATIONS,
             run_line_load, &aligned_load);
    ADD_CASE("line_load_cross_128b", "memory", "load_16b", LOAD_OPS, MAX_ITERATIONS,
             run_line_load, &cross_128b_load);
    ADD_CASE("store_load_exact", "memory", "store_load_pair", FORWARD_OPS,
             MAX_ITERATIONS, run_store_load_forwarding, &exact_forward);
    ADD_CASE("store_load_partial_overlap", "memory", "store_load_pair", FORWARD_OPS,
             MAX_ITERATIONS, run_store_load_forwarding, &partial_forward);
    ADD_CASE("code_footprint_4k", "instruction_delivery", "guest_instruction",
             code_4k.instruction_count, 1000000u, run_generated_code, &code_4k);
    ADD_CASE("code_footprint_64k", "instruction_delivery", "guest_instruction",
             code_64k.instruction_count, 100000u, run_generated_code, &code_64k);
    ADD_CASE("code_footprint_256k", "instruction_delivery", "guest_instruction",
             code_256k.instruction_count, 10000u, run_generated_code, &code_256k);
    ADD_CASE("jit_republish_page", "jit", "publication", 1u, 1000000u,
             run_jit_publish, &jit_publish);
#undef ADD_CASE

    if (benchmark_count != ARRAY_SIZE(benchmarks))
    {
        fprintf(stderr, "internal benchmark registration mismatch\n");
        goto done;
    }
    printf("apple_cpu_benchmark schema=1 run=%" PRIu64 " benchmark_version=1 "
           "architecture=x86_64 target_ms=%" PRIu64 " trials=%u "
           "qpc_frequency=%" PRIu64 " filter=%s\n",
           run_id, target_ms, trials, (uint64_t)qpc_frequency.QuadPart,
           filter && *filter ? filter : "all");
    for (i = 0; i < benchmark_count; ++i)
    {
        if (!benchmark_selected(filter, benchmarks[i].name)) continue;
        ++selected_count;
        if (!run_case(&benchmarks[i], target_ticks, trials, run_id)) goto done;
    }
    if (!selected_count)
    {
        fprintf(stderr, "APPLE_CPU_BENCH_CASE did not match a benchmark: %s\n", filter);
        goto done;
    }
    printf("apple_cpu_complete schema=1 run=%" PRIu64 " cases=%zu sink=%" PRIu64 "\n",
           run_id, selected_count, benchmark_sink);
    result = 0;

done:
    destroy_jit_publish_context(&jit_publish);
    destroy_generated_code(&code_256k);
    destroy_generated_code(&code_64k);
    destroy_generated_code(&code_4k);
    free(pointer_16m.next);
    free(pointer_256k.next);
    free(pointer_8k.next);
    if (forward_storage) _aligned_free(forward_storage);
    if (load_storage) _aligned_free(load_storage);
    free(unpredictable);
    free(predictable);
    return result;
}
