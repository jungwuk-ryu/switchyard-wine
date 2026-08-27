/*
 * Apple Silicon benchmark for the xtajit64 translated-block stop poll.
 *
 * The benchmark executes one self-looping x86 translation block repeatedly
 * through Unicorn.  Every callback performs the same EC classification and
 * stop semantics; the common no-stop polling sequence is varied independently.
 */

#include <mach/mach_time.h>
#include <stdatomic.h>
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
#define ITERATIONS UINT64_C(500000)
#define WARMUP_ITERATIONS UINT64_C(20000)
#define CODE_BASE UINT64_C(0x01000000)
#define CODE_MAP_SIZE UINT64_C(0x1000)
#define EC_PAGE_SIZE UINT64_C(0x4000)

#define STOP_NONE 0u
#define STOP_EC 1u
#define STOP_DOORBELL 2u
#define STOP_PAUSE 3u

struct poll_state
{
    uint64_t bitmap[64];
    const uint64_t *ec_bitmap;
    uint64_t ec_page_size;
    uint64_t highest_user_address;
    volatile uint32_t doorbell;
    volatile uint32_t *suspend_doorbell;
    atomic_bool pause_requested;
    uint64_t hook_count;
    unsigned int stop_reason;
};

struct poll_engine
{
    uc_engine *uc;
    uc_hook hook;
    struct poll_state state;
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

static bool is_ec_code(const struct poll_state *state, uint64_t address)
{
    uint64_t page, word;

    if (!state->ec_bitmap || address > state->highest_user_address) return false;
    page = address / state->ec_page_size;
    word = state->ec_bitmap[page / 64];
    return (word >> (page & 63)) & 1;
}

static void baseline_block_hook(uc_engine *uc, uint64_t address,
                                uint32_t size, void *user)
{
    struct poll_state *state = user;

    (void)size;
    ++state->hook_count;
    if (is_ec_code(state, address))
    {
        state->stop_reason = STOP_EC;
        uc_emu_stop(uc);
        return;
    }
    if (state->suspend_doorbell && *state->suspend_doorbell)
    {
        state->stop_reason = STOP_DOORBELL;
        uc_emu_stop(uc);
        return;
    }
    if (!atomic_load_explicit(&state->pause_requested, memory_order_acquire))
        return;
    state->stop_reason = STOP_PAUSE;
    uc_emu_stop(uc);
}

static void direct_acquire_block_hook(uc_engine *uc, uint64_t address,
                                      uint32_t size, void *user)
{
    struct poll_state *state = user;
    uint32_t doorbell;

    (void)size;
    ++state->hook_count;
    if (is_ec_code(state, address))
    {
        state->stop_reason = STOP_EC;
        uc_emu_stop(uc);
        return;
    }
    doorbell = *state->suspend_doorbell;
    if (doorbell)
    {
        state->stop_reason = STOP_DOORBELL;
        uc_emu_stop(uc);
        return;
    }
    if (!atomic_load_explicit(&state->pause_requested, memory_order_acquire))
        return;
    state->stop_reason = STOP_PAUSE;
    uc_emu_stop(uc);
}

static void nullable_relaxed_block_hook(uc_engine *uc, uint64_t address,
                                        uint32_t size, void *user)
{
    struct poll_state *state = user;

    (void)size;
    ++state->hook_count;
    if (is_ec_code(state, address))
    {
        state->stop_reason = STOP_EC;
        uc_emu_stop(uc);
        return;
    }
    if (state->suspend_doorbell && *state->suspend_doorbell)
    {
        state->stop_reason = STOP_DOORBELL;
        uc_emu_stop(uc);
        return;
    }
    if (!atomic_load_explicit(&state->pause_requested, memory_order_relaxed))
        return;
    state->stop_reason = STOP_PAUSE;
    uc_emu_stop(uc);
}

static void direct_relaxed_block_hook(uc_engine *uc, uint64_t address,
                                      uint32_t size, void *user)
{
    struct poll_state *state = user;
    uint32_t doorbell;

    (void)size;
    ++state->hook_count;
    if (is_ec_code(state, address))
    {
        state->stop_reason = STOP_EC;
        uc_emu_stop(uc);
        return;
    }
    doorbell = *state->suspend_doorbell;
    if (doorbell)
    {
        state->stop_reason = STOP_DOORBELL;
        uc_emu_stop(uc);
        return;
    }
    if (!atomic_load_explicit(&state->pause_requested, memory_order_relaxed))
        return;
    state->stop_reason = STOP_PAUSE;
    uc_emu_stop(uc);
}

static void initialize_state(struct poll_state *state)
{
    memset(state, 0, sizeof(*state));
    state->ec_bitmap = state->bitmap;
    state->ec_page_size = EC_PAGE_SIZE;
    state->highest_user_address = UINT64_C(0x00007fffffffffff);
    state->suspend_doorbell = &state->doorbell;
    atomic_init(&state->pause_requested, false);
}

static void open_poll_engine(struct poll_engine *engine,
                             void (*callback)(uc_engine *, uint64_t,
                                              uint32_t, void *))
{
    static const uint8_t code[] =
    {
        0x48, 0xff, 0xc9, /* dec %rcx */
        0x75, 0xfb,       /* jne CODE_BASE */
    };
    uc_err error;

    memset(engine, 0, sizeof(*engine));
    initialize_state(&engine->state);
    if ((error = uc_open(UC_ARCH_X86, UC_MODE_64, &engine->uc)) != UC_ERR_OK)
        die_unicorn("uc_open", error);
    if ((error = uc_mem_map(engine->uc, CODE_BASE, CODE_MAP_SIZE,
                            UC_PROT_ALL)) != UC_ERR_OK)
        die_unicorn("uc_mem_map", error);
    if ((error = uc_mem_write(engine->uc, CODE_BASE, code, sizeof(code))) != UC_ERR_OK)
        die_unicorn("uc_mem_write", error);
    if ((error = uc_hook_add(engine->uc, &engine->hook, UC_HOOK_BLOCK,
                             callback, &engine->state, 1, 0)) != UC_ERR_OK)
        die_unicorn("uc_hook_add", error);
}

static void close_poll_engine(struct poll_engine *engine)
{
    if (engine->uc) uc_close(engine->uc);
    memset(engine, 0, sizeof(*engine));
}

static uint64_t ec_page_index(void)
{
    return CODE_BASE / EC_PAGE_SIZE;
}

static void set_ec_classification(struct poll_state *state, bool enabled)
{
    uint64_t page = ec_page_index();
    uint64_t mask = UINT64_C(1) << (page & 63);

    if (enabled) state->bitmap[page / 64] |= mask;
    else state->bitmap[page / 64] &= ~mask;
}

static void prepare_run(struct poll_engine *engine, uint64_t iterations)
{
    uc_err error;

    engine->state.doorbell = 0;
    atomic_store_explicit(&engine->state.pause_requested, false,
                          memory_order_relaxed);
    set_ec_classification(&engine->state, false);
    engine->state.hook_count = 0;
    engine->state.stop_reason = STOP_NONE;
    if ((error = uc_reg_write(engine->uc, UC_X86_REG_RCX, &iterations)) != UC_ERR_OK)
        die_unicorn("uc_reg_write RCX", error);
}

static void verify_completed_run(struct poll_engine *engine, uint64_t iterations)
{
    uint64_t rcx = UINT64_MAX;
    uc_err error;

    if ((error = uc_reg_read(engine->uc, UC_X86_REG_RCX, &rcx)) != UC_ERR_OK)
        die_unicorn("uc_reg_read RCX", error);
    if (rcx || engine->state.hook_count != iterations ||
        engine->state.stop_reason != STOP_NONE)
    {
        fprintf(stderr,
                "block loop mismatch: rcx=%llu hooks=%llu expected=%llu stop=%u\n",
                (unsigned long long)rcx,
                (unsigned long long)engine->state.hook_count,
                (unsigned long long)iterations,
                engine->state.stop_reason);
        exit(1);
    }
    benchmark_sink ^= rcx ^ engine->state.hook_count;
}

static double measure_once(struct poll_engine *engine, uint64_t iterations)
{
    uint64_t begin, end;
    uc_err error;

    prepare_run(engine, iterations);
    begin = mach_continuous_time();
    error = uc_emu_start(engine->uc, CODE_BASE, CODE_BASE + 5, 0, 0);
    end = mach_continuous_time();
    if (error != UC_ERR_OK) die_unicorn("uc_emu_start", error);
    verify_completed_run(engine, iterations);
    return elapsed_ns(begin, end) / (double)iterations;
}

static void verify_stop_case(struct poll_engine *engine, uint32_t doorbell,
                             bool pause_requested, bool ec,
                             unsigned int expected_reason)
{
    const uint64_t iterations = 100;
    uint64_t rcx = iterations;
    uc_err error;

    engine->state.doorbell = doorbell;
    atomic_store_explicit(&engine->state.pause_requested, pause_requested,
                          memory_order_relaxed);
    set_ec_classification(&engine->state, ec);
    engine->state.hook_count = 0;
    engine->state.stop_reason = STOP_NONE;
    if ((error = uc_reg_write(engine->uc, UC_X86_REG_RCX, &rcx)) != UC_ERR_OK)
        die_unicorn("stop-case RCX write", error);
    if ((error = uc_emu_start(engine->uc, CODE_BASE, CODE_BASE + 5, 0, 0)) != UC_ERR_OK)
        die_unicorn("stop-case emulation", error);
    if ((error = uc_reg_read(engine->uc, UC_X86_REG_RCX, &rcx)) != UC_ERR_OK)
        die_unicorn("stop-case RCX read", error);
    if (engine->state.hook_count != 1 ||
        engine->state.stop_reason != expected_reason || rcx != iterations)
    {
        fprintf(stderr,
                "stop-case mismatch: hooks=%llu reason=%u/%u rcx=%llu/%llu\n",
                (unsigned long long)engine->state.hook_count,
                engine->state.stop_reason, expected_reason,
                (unsigned long long)rcx,
                (unsigned long long)iterations);
        exit(1);
    }
}

static void verify_stop_semantics(struct poll_engine *engine)
{
    verify_stop_case(engine, 1, true, true, STOP_EC);
    verify_stop_case(engine, 1, true, false, STOP_DOORBELL);
    verify_stop_case(engine, 0, true, false, STOP_PAUSE);
    prepare_run(engine, 1);
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
    struct poll_engine baseline, direct_acquire, nullable_relaxed, direct_relaxed;
    double baseline_samples[ROUNDS], direct_acquire_samples[ROUNDS];
    double nullable_relaxed_samples[ROUNDS], direct_relaxed_samples[ROUNDS];
    double baseline_ns, direct_acquire_ns, nullable_relaxed_ns, direct_relaxed_ns;
    unsigned int round;

    if (mach_timebase_info(&timebase) != KERN_SUCCESS)
    {
        fprintf(stderr, "mach_timebase_info failed\n");
        return 1;
    }
    print_cpu_model();
    if (ec_page_index() / 64 >= ARRAY_SIZE(baseline.state.bitmap))
    {
        fprintf(stderr, "benchmark EC bitmap is too small\n");
        return 1;
    }

    open_poll_engine(&baseline, baseline_block_hook);
    open_poll_engine(&direct_acquire, direct_acquire_block_hook);
    open_poll_engine(&nullable_relaxed, nullable_relaxed_block_hook);
    open_poll_engine(&direct_relaxed, direct_relaxed_block_hook);
    verify_stop_semantics(&baseline);
    verify_stop_semantics(&direct_acquire);
    verify_stop_semantics(&nullable_relaxed);
    verify_stop_semantics(&direct_relaxed);
    (void)measure_once(&baseline, WARMUP_ITERATIONS);
    (void)measure_once(&direct_acquire, WARMUP_ITERATIONS);
    (void)measure_once(&nullable_relaxed, WARMUP_ITERATIONS);
    (void)measure_once(&direct_relaxed, WARMUP_ITERATIONS);

    for (round = 0; round < ROUNDS; ++round)
    {
        if (round & 1)
        {
            direct_relaxed_samples[round] = measure_once(&direct_relaxed, ITERATIONS);
            nullable_relaxed_samples[round] = measure_once(&nullable_relaxed, ITERATIONS);
            direct_acquire_samples[round] = measure_once(&direct_acquire, ITERATIONS);
            baseline_samples[round] = measure_once(&baseline, ITERATIONS);
        }
        else
        {
            baseline_samples[round] = measure_once(&baseline, ITERATIONS);
            direct_acquire_samples[round] = measure_once(&direct_acquire, ITERATIONS);
            nullable_relaxed_samples[round] = measure_once(&nullable_relaxed, ITERATIONS);
            direct_relaxed_samples[round] = measure_once(&direct_relaxed, ITERATIONS);
        }
    }
    baseline_ns = median(baseline_samples);
    direct_acquire_ns = median(direct_acquire_samples);
    nullable_relaxed_ns = median(nullable_relaxed_samples);
    direct_relaxed_ns = median(direct_relaxed_samples);

    printf("APPLE_SILICON_HOTPATH block_poll_baseline_ns=%.3f "
           "direct_acquire_ns=%.3f nullable_relaxed_ns=%.3f "
           "direct_relaxed_ns=%.3f speedup=%.3fx\n",
           baseline_ns, direct_acquire_ns, nullable_relaxed_ns,
           direct_relaxed_ns, baseline_ns / direct_relaxed_ns);
    printf("APPLE_SILICON_HOTPATH block_poll_nullable_checks=1->0 "
           "pause_order=acquire->relaxed common_path=negative-early-return\n");

    close_poll_engine(&baseline);
    close_poll_engine(&direct_acquire);
    close_poll_engine(&nullable_relaxed);
    close_poll_engine(&direct_relaxed);
    if (!(direct_relaxed_ns < baseline_ns))
    {
        fprintf(stderr, "direct relaxed block poll did not beat baseline\n");
        return 1;
    }
    return benchmark_sink == UINT64_MAX ? 1 : 0;
}
