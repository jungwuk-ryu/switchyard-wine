/* SPDX-License-Identifier: MIT */

#include "switchyard_fex.h"

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "admission_contract.h"

#define WORKER_COUNT 8

static _Alignas(16) unsigned char guest_code[32] = {
    0x48, 0xb8,
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
    0xeb, 0x02,
    0x0f, 0x0b,
    0x48, 0x83, 0xc0, 0x05,
    0xf4,
};

static sigjmp_buf fault_jump;
static volatile sig_atomic_t fault_armed;
static volatile sig_atomic_t captured_signal;
static uint64_t captured_fault_address;
static struct switchyard_fex_arm64_host_context captured_host_context;
static _Thread_local struct switchyard_fex_thread *signal_query_thread;
static uint64_t signal_query_stack;
static volatile sig_atomic_t signal_query_result;
static _Thread_local atomic_uint execution_doorbell;
static _Thread_local struct switchyard_fex_thread *unaligned_repair_thread;
static _Thread_local volatile sig_atomic_t unaligned_repair_count;
static _Thread_local volatile sig_atomic_t unaligned_repair_result;
static _Thread_local struct switchyard_fex_thread *callret_repair_thread;
static _Thread_local volatile sig_atomic_t callret_repair_count;
static _Alignas(8) uint64_t async_suspend_progress;

_Static_assert(_Alignof(atomic_uint) >= _Alignof(uint32_t),
               "test execution doorbell alignment drift");

static void capture_host_context(struct switchyard_fex_arm64_host_context *host,
                                 const ucontext_t *context)
{
    size_t index;

    memset(host, 0, sizeof(*host));
    host->size = sizeof(*host);
    host->version = SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION;
    for (index = 0; index < 29; ++index)
        host->gpr[index] = context->uc_mcontext->__ss.__x[index];
    host->gpr[29] = context->uc_mcontext->__ss.__fp;
    host->gpr[30] = context->uc_mcontext->__ss.__lr;
    memcpy(host->vector, context->uc_mcontext->__ns.__v, sizeof(host->vector));
    host->pc = context->uc_mcontext->__ss.__pc;
    host->pstate = context->uc_mcontext->__ss.__cpsr;
    host->fpcr = context->uc_mcontext->__ns.__fpcr;
    host->fpsr = context->uc_mcontext->__ns.__fpsr;
}

static void capture_jit_fault(int signal, siginfo_t *info, void *opaque_context)
{
    ucontext_t *context = opaque_context;

    if (callret_repair_thread && (signal == SIGBUS || signal == SIGSEGV))
    {
        struct switchyard_fex_arm64_host_context host;
        const uint32_t access = (context->uc_mcontext->__es.__esr >> 6) & 1;
        uint32_t instruction, callret_register;

        capture_host_context(&host, context);
        /* Independently decode Rn from this test's faulting generated pair
         * load/store. Do not duplicate the emitter's register allocation. */
        memcpy(&instruction, (const void *)(uintptr_t)host.pc, sizeof(instruction));
        callret_register = (instruction >> 5) & 31;
        if (callret_register >= 29 ||
            (instruction & 0xffff8000u) != (access ? 0xa9bf0000u : 0xa8c10000u)) _exit(93);
        {
            struct switchyard_fex_arm64_host_context rejected = host;
            if (switchyard_fex_thread_repair_callret_fault(
                    callret_repair_thread, &rejected, 2, (uintptr_t)info->si_addr) !=
                    SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT ||
                memcmp(&rejected, &host, sizeof(host)) ||
                switchyard_fex_thread_repair_callret_fault(
                    callret_repair_thread, &rejected, access, 0) !=
                    SWITCHYARD_FEX_ERROR_UNSUPPORTED ||
                memcmp(&rejected, &host, sizeof(host))) _exit(90);
            rejected.pc = (uintptr_t)capture_jit_fault;
            if (switchyard_fex_thread_repair_callret_fault(
                    callret_repair_thread, &rejected, access, (uintptr_t)info->si_addr) !=
                    SWITCHYARD_FEX_ERROR_UNSUPPORTED) _exit(91);
            rejected = host;
            rejected.gpr[callret_register] += 16;
            if (switchyard_fex_thread_repair_callret_fault(
                    callret_repair_thread, &rejected, access, (uintptr_t)info->si_addr) !=
                    SWITCHYARD_FEX_ERROR_UNSUPPORTED) _exit(92);
        }
        if (switchyard_fex_thread_repair_callret_fault(
                callret_repair_thread, &host, access, (uintptr_t)info->si_addr) == SWITCHYARD_FEX_OK)
        {
            struct switchyard_fex_arm64_host_context expected;
            capture_host_context(&expected, context);
            if (host.gpr[callret_register] == expected.gpr[callret_register] ||
                (host.gpr[callret_register] & 15)) _exit(94);
            expected.gpr[callret_register] = host.gpr[callret_register];
            if (memcmp(&expected, &host, sizeof(host))) _exit(95);
            context->uc_mcontext->__ss.__x[callret_register] = host.gpr[callret_register];
            ++callret_repair_count;
            return;
        }
    }

    if (signal == SIGBUS && unaligned_repair_thread)
    {
        struct switchyard_fex_arm64_host_context host;
        enum switchyard_fex_result result;

        capture_host_context(&host, context);
        result = switchyard_fex_thread_repair_unaligned_tso(
            unaligned_repair_thread, &host);
        unaligned_repair_result = (sig_atomic_t)result;
        if (result == SWITCHYARD_FEX_OK)
        {
            /* TSO repair changes only generated code and the retry PC. */
            context->uc_mcontext->__ss.__pc = host.pc;
            ++unaligned_repair_count;
            return;
        }
    }
    if (!fault_armed) _exit(128 + signal);
    capture_host_context(&captured_host_context, context);
    captured_fault_address = (uintptr_t)info->si_addr;
    captured_signal = signal;
    if (signal_query_thread)
        signal_query_result = (sig_atomic_t)switchyard_fex_thread_query_jit_stack(
            signal_query_thread, &captured_host_context, 0,
            captured_fault_address, &signal_query_stack);
    fault_armed = 0;
    siglongjmp(fault_jump, 1);
}

struct worker_data
{
    struct switchyard_fex_process *process;
    atomic_uint *passes;
    pthread_mutex_t *start_mutex;
    pthread_cond_t *start_condition;
    unsigned int *ready_count;
    int *start;
};

static uintptr_t read_x18(void)
{
    uintptr_t value;
    __asm__ volatile("mov %0, x18" : "=r"(value));
    return value;
}

static void fail_result(const char *operation, enum switchyard_fex_result result)
{
    fprintf(stderr, "%s failed: %s (%u)\n", operation,
            switchyard_fex_result_string(result), (unsigned int)result);
    exit(1);
}

static void require_result(const char *operation, enum switchyard_fex_result actual,
                           enum switchyard_fex_result expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "%s returned %s (%u), expected %s (%u)\n", operation,
                switchyard_fex_result_string(actual), (unsigned int)actual,
                switchyard_fex_result_string(expected), (unsigned int)expected);
        exit(1);
    }
}

static void initialize_state(struct switchyard_fex_x64_state *state,
                             unsigned char *stack, size_t stack_size)
{
    size_t i;

    memset(state, 0, sizeof(*state));
    state->size = sizeof(*state);
    state->version = SWITCHYARD_FEX_STATE_VERSION;
    state->flags = SWITCHYARD_FEX_STATE_YMM_HIGH_VALID;
    state->rip = (uintptr_t)guest_code;
    state->gpr[4] = (uintptr_t)(stack + stack_size);
    state->rflags = 0x246;
    state->mxcsr = 0x1f80;
    state->fcw = 0x37f;
    state->segment[1] = 0x30;
    state->segment_base[4] = 0x12345000;
    state->segment_base[5] = 0x23456000;
    for (i = 0; i < 16; ++i)
    {
        state->xmm[i].low = UINT64_C(0x1000) + i;
        state->xmm[i].high = UINT64_C(0x2000) + i;
        state->ymm_high[i].low = UINT64_C(0x3000) + i;
        state->ymm_high[i].high = UINT64_C(0x4000) + i;
    }
    for (i = 0; i < 8; ++i)
    {
        state->x87[i].low = UINT64_C(0x5000) + i;
        state->x87[i].high = UINT64_C(0x6000) + i;
    }
}

static void initialize_stop(struct switchyard_fex_stop *stop)
{
    memset(stop, 0, sizeof(*stop));
    stop->size = sizeof(*stop);
    stop->version = SWITCHYARD_FEX_STOP_VERSION;
}

static void initialize_execution(struct switchyard_fex_execution *execution,
                                 uint64_t expected_hlt_rip,
                                 atomic_uint *suspend_doorbell)
{
    memset(execution, 0, sizeof(*execution));
    execution->size = sizeof(*execution);
    execution->version = SWITCHYARD_FEX_EXECUTION_VERSION;
    execution->expected_hlt_rip = expected_hlt_rip;
    execution->suspend_doorbell = (uintptr_t)suspend_doorbell;
}

static enum switchyard_fex_result execute_until_hlt(
    struct switchyard_fex_thread *thread, uint64_t expected_hlt_rip,
    struct switchyard_fex_stop *stop)
{
    struct switchyard_fex_execution execution;

    atomic_store_explicit(&execution_doorbell, 0, memory_order_release);
    initialize_execution(&execution, expected_hlt_rip, &execution_doorbell);
    return switchyard_fex_thread_execute(thread, &execution, stop);
}

struct test_executable_policy
{
    uint64_t denied_base;
    uint64_t denied_size;
    unsigned int malformed;
};

/* Test fixtures deliberately execute x86 bytes from non-executable ARM host
 * arrays. Their logical guest permissions are explicit and independent of
 * Darwin host execute permission, just as they are in Wine. */
static enum switchyard_fex_result test_query_executable_range(
    void *opaque, uint64_t address, struct switchyard_fex_executable_range *range)
{
    const struct test_executable_policy *policy = opaque;

    range->base = 0;
    range->length = UINT64_MAX;
    range->flags = SWITCHYARD_FEX_EXECUTABLE_RANGE_WRITABLE;
    if (policy && policy->denied_size)
    {
        uint64_t end = policy->denied_base + policy->denied_size;
        if (address >= policy->denied_base && address < end)
            return SWITCHYARD_FEX_ERROR_GUEST_FAULT;
        if (address < policy->denied_base) range->length = policy->denied_base;
        else
        {
            range->base = end;
            range->length = UINT64_MAX - end;
        }
    }
    switch (policy ? policy->malformed : 0)
    {
    case 1: range->base = address + 1; range->length = 1; break;
    case 2: range->base = address; range->length = UINT64_MAX; break;
    case 3: range->flags = UINT32_MAX; break;
    case 4: ++range->version; break;
    case 5: --range->size; break;
    case 6: range->reserved = 1; break;
    case 7: range->length = 0; break;
    case 8: return SWITCHYARD_FEX_ERROR_INTERNAL;
    default: break;
    }
    return SWITCHYARD_FEX_OK;
}

struct suspend_worker_data
{
    struct switchyard_fex_thread *thread;
    atomic_uint *doorbell;
    enum switchyard_fex_result result;
    struct switchyard_fex_stop stop;
};

static void *suspend_worker_main(void *opaque)
{
    struct suspend_worker_data *data = opaque;
    struct switchyard_fex_execution execution;

    initialize_execution(&execution, 0, data->doorbell);
    initialize_stop(&data->stop);
    data->result = switchyard_fex_thread_execute(
        data->thread, &execution, &data->stop);
    return NULL;
}

static void *worker_main(void *opaque)
{
    struct worker_data *data = opaque;
    struct switchyard_fex_thread *thread = NULL;
    struct switchyard_fex_x64_state state;
    struct switchyard_fex_stop stop;
    _Alignas(16) unsigned char stack[4096] = {0};
    uintptr_t initial_x18 = read_x18();
    int prepared = 0;

    if (switchyard_fex_thread_create(data->process, &thread) == SWITCHYARD_FEX_OK)
    {
        initialize_state(&state, stack, sizeof(stack));
        prepared = switchyard_fex_thread_import_state(thread, &state) == SWITCHYARD_FEX_OK;
    }

    pthread_mutex_lock(data->start_mutex);
    ++*data->ready_count;
    if (*data->ready_count == WORKER_COUNT)
    {
        *data->start = 1;
        pthread_cond_broadcast(data->start_condition);
    }
    while (!*data->start)
        pthread_cond_wait(data->start_condition, data->start_mutex);
    pthread_mutex_unlock(data->start_mutex);

    initialize_stop(&stop);
    if (prepared &&
        execute_until_hlt(
            thread, (uintptr_t)&guest_code[18], &stop) == SWITCHYARD_FEX_OK &&
        stop.reason == SWITCHYARD_FEX_STOP_HLT &&
        switchyard_fex_thread_export_state(thread, &state) == SWITCHYARD_FEX_OK &&
        state.gpr[0] == UINT64_C(0x1122334455667795) && read_x18() == initial_x18)
    {
        atomic_fetch_add_explicit(data->passes, 1, memory_order_relaxed);
    }
    if (thread && switchyard_fex_thread_destroy(thread) != SWITCHYARD_FEX_OK)
        return NULL;
    return NULL;
}

static void test_flag_import_round_trip(struct switchyard_fex_thread *thread,
                                        const struct switchyard_fex_x64_state *original)
{
    struct switchyard_fex_x64_state input = *original;
    struct switchyard_fex_x64_state expected = {0}, output = {0};
    uint32_t random = UINT32_C(0x983feb21);
    uint32_t iteration;

    expected.size = sizeof(expected);
    expected.version = SWITCHYARD_FEX_STATE_VERSION;
    require_result("flag baseline export",
                   switchyard_fex_thread_export_state(thread, &expected), SWITCHYARD_FEX_OK);
    for (iteration = 0; iteration < 65536 + 4096; ++iteration)
    {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        input.rflags = iteration < 65536 ? iteration : random;
        require_result("flag combination import",
                       switchyard_fex_thread_import_state(thread, &input), SWITCHYARD_FEX_OK);
        output.size = sizeof(output);
        output.version = SWITCHYARD_FEX_STATE_VERSION;
        require_result("flag combination export",
                       switchyard_fex_thread_export_state(thread, &output), SWITCHYARD_FEX_OK);
        /* Cover every low-16 combination plus high flag bits. The internal
         * representation reserves bits 24--27 for NZCV; only architectural
         * bits through ID (21) are asserted here. Reserved bit 1 and IF are
         * forced on by the user-mode import contract, not taken from input. */
        if ((output.rflags & UINT32_C(0x003fffff)) !=
            ((input.rflags | UINT32_C(0x202)) & UINT32_C(0x003fffff)))
        {
            fprintf(stderr, "flag combination failed: input=%" PRIx64 " output=%" PRIx64 "\n",
                    input.rflags, output.rflags);
            exit(1);
        }
        expected.rflags = output.rflags;
        if (memcmp(&output, &expected, sizeof(output)))
        {
            fprintf(stderr, "flag import modified unrelated CPU state\n");
            exit(1);
        }
    }
    require_result("restore state after flag combinations",
                   switchyard_fex_thread_import_state(thread, original), SWITCHYARD_FEX_OK);
}

static void test_lea_results(struct switchyard_fex_process *process,
                             struct switchyard_fex_thread *thread,
                             unsigned char *stack, size_t stack_size)
{
    static _Alignas(16) unsigned char code[16];
    /* Base/index16 means absent; base17 denotes RIP/EIP-relative. */
    static const struct {
        unsigned char bytes[16];
        unsigned int hlt, width, addr32, base, index;
    } cases[] = {
        {{0x8d, 0x84, 0xd1, 0xff, 0xff, 0xff, 0xff, 0xf4}, 7, 32, 0, 1, 2},
        {{0x67, 0x8d, 0x84, 0xd1, 0xff, 0xff, 0xff, 0xff, 0xf4}, 8, 32, 1, 1, 2},
        {{0x66, 0x8d, 0x84, 0xd1, 0xff, 0xff, 0xff, 0xff, 0xf4}, 8, 16, 0, 1, 2},
        {{0x48, 0x8d, 0x84, 0xd1, 0xff, 0xff, 0xff, 0xff, 0xf4}, 8, 64, 0, 1, 2},
        {{0x67, 0x48, 0x8d, 0x84, 0xd1, 0xff, 0xff, 0xff, 0xff, 0xf4}, 9, 64, 1, 1, 2},
        {{0x64, 0x8d, 0x84, 0xd1, 0xff, 0xff, 0xff, 0xff, 0xf4}, 8, 32, 0, 1, 2},
        {{0x65, 0x66, 0x8d, 0x84, 0xd1, 0xff, 0xff, 0xff, 0xff, 0xf4}, 9, 16, 0, 1, 2},
        {{0x8d, 0x84, 0xd0, 0xff, 0xff, 0xff, 0xff, 0xf4}, 7, 32, 0, 0, 2},
        {{0x8d, 0x84, 0xc1, 0xff, 0xff, 0xff, 0xff, 0xf4}, 7, 32, 0, 1, 0},
        {{0x8d, 0x05, 0xff, 0xff, 0xff, 0xff, 0xf4}, 6, 32, 0, 17, 16},
        {{0x67, 0x48, 0x8d, 0x05, 0x01, 0x00, 0x00, 0x80, 0xf4}, 8, 64, 1, 17, 16},
        {{0x8d, 0x04, 0x25, 0x00, 0x00, 0x00, 0x80, 0xf4}, 7, 32, 0, 16, 16},
    };
    static const uint64_t values[] = {0, 1, UINT64_C(0xffffffff), UINT64_C(0x100000000),
                                     UINT64_C(0xffffffff00000000), UINT64_MAX};
    static const uint32_t flags[] = {0x202, 0x203, 0x206, 0x212, 0x242, 0x282, 0xa02, 0xed7};
    size_t site, value, flag;

    for (site = 0; site < sizeof(cases) / sizeof(cases[0]); ++site)
    {
        uint32_t displacement;
        /* The sole guest thread is idle. Invalidate before changing its code. */
        require_result("LEA code invalidation",
                       switchyard_fex_process_invalidate_code(process, (uintptr_t)code, sizeof(code)),
                       SWITCHYARD_FEX_OK);
        memcpy(code, cases[site].bytes, sizeof(code));
        memcpy(&displacement, code + cases[site].hlt - sizeof(displacement), sizeof(displacement));
        for (value = 0; value < sizeof(values) / sizeof(values[0]); ++value)
        for (flag = 0; flag < sizeof(flags) / sizeof(flags[0]); ++flag)
        {
            struct switchyard_fex_x64_state state, exported;
            struct switchyard_fex_stop stop;
            const uintptr_t initial_x18 = read_x18();
            const uint64_t hlt = (uintptr_t)code + cases[site].hlt;
            uint64_t expected = displacement;

            /* Defined modulo2^64 sign extension, including INT32_MIN. */
            if (displacement & 0x80000000u) expected -= UINT64_C(1) << 32;
            initialize_state(&state, stack, stack_size);
            state.rip = (uintptr_t)code;
            state.rflags = flags[flag];
            state.gpr[0] = UINT64_C(0xa5a55a5a00000000) | (values[value] & UINT64_C(0xffffffff));
            state.gpr[1] = values[value];
            state.gpr[2] = values[(value + 3) % (sizeof(values) / sizeof(values[0]))];
            if (cases[site].base < 16) expected += state.gpr[cases[site].base];
            else if (cases[site].base == 17) expected += hlt;
            if (cases[site].index < 16) expected += state.gpr[cases[site].index] << 3;
            if (cases[site].addr32) expected = (uint32_t)expected;
            if (cases[site].width == 16)
                expected = (state.gpr[0] & ~UINT64_C(0xffff)) | (expected & UINT64_C(0xffff));
            else if (cases[site].width == 32) expected = (uint32_t)expected;
            require_result("LEA state import", switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
            initialize_stop(&stop);
            require_result("LEA execution", execute_until_hlt(thread, hlt, &stop), SWITCHYARD_FEX_OK);
            initialize_state(&exported, stack, stack_size);
            require_result("LEA state export", switchyard_fex_thread_export_state(thread, &exported), SWITCHYARD_FEX_OK);
            state.gpr[0] = expected;
            if (stop.reason != SWITCHYARD_FEX_STOP_HLT || stop.rip != hlt ||
                memcmp(state.gpr, exported.gpr, sizeof(state.gpr)) ||
                (state.rflags & 0xed5) != (exported.rflags & 0xed5) ||
                state.mxcsr != exported.mxcsr || state.fcw != exported.fcw ||
                memcmp(state.xmm, exported.xmm, sizeof(state.xmm)) ||
                memcmp(state.ymm_high, exported.ymm_high, sizeof(state.ymm_high)) ||
                memcmp(state.x87, exported.x87, sizeof(state.x87)) || read_x18() != initial_x18)
            {
                fprintf(stderr, "LEA result/state mismatch site=%zu value=%zu flag=%zu\n", site, value, flag);
                exit(1);
            }
        }
    }
}

static void run_process_cycle(int run_full_test, uint32_t config_flags)
{
    struct switchyard_fex_config config = {
        .size = sizeof(config),
        .abi_version = SWITCHYARD_FEX_ABI_VERSION,
        .flags = config_flags,
    };
    struct switchyard_fex_process *process = NULL;
    struct switchyard_fex_process *second_process = NULL;
    struct switchyard_fex_thread *thread = NULL;
    struct switchyard_fex_x64_state state;
    struct switchyard_fex_x64_state exported;
    struct switchyard_fex_execution execution;
    struct switchyard_fex_stop stop;
    _Alignas(16) unsigned char stack[4096] = {0};
    uintptr_t initial_x18 = read_x18();
    enum switchyard_fex_result result;
    size_t register_index;

    guest_code[2] = 0x88;
    require_result("process create", switchyard_fex_process_create(&config, &process),
                   SWITCHYARD_FEX_OK);
    require_result("NULL executable range callback rejection",
                   switchyard_fex_process_set_executable_range_query(process, NULL, NULL),
                   SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    require_result("executable range callback registration",
                   switchyard_fex_process_set_executable_range_query(process, test_query_executable_range, NULL),
                   SWITCHYARD_FEX_OK);
    require_result("duplicate executable range callback rejection",
                   switchyard_fex_process_set_executable_range_query(process, test_query_executable_range, NULL),
                   SWITCHYARD_FEX_ERROR_BUSY);
    require_result("second process rejection",
                   switchyard_fex_process_create(&config, &second_process),
                   SWITCHYARD_FEX_ERROR_BUSY);
    require_result("thread create", switchyard_fex_thread_create(process, &thread),
                   SWITCHYARD_FEX_OK);
    require_result("destroy process with live thread", switchyard_fex_process_destroy(process),
                   SWITCHYARD_FEX_ERROR_BUSY);

    test_lea_results(process, thread, stack, sizeof(stack));

    initialize_state(&state, stack, sizeof(stack));
    for (register_index = 0; register_index < 16; ++register_index)
        if (register_index != 4)
            state.gpr[register_index] = UINT64_C(0x1234000000000000) + register_index;
    {
        struct switchyard_fex_x64_state invalid_state = state;

        invalid_state.rflags = UINT64_MAX;
        require_result("wide rflags rejection",
                       switchyard_fex_thread_import_state(thread, &invalid_state),
                       SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
        invalid_state = state;
        invalid_state.mxcsr = UINT32_MAX;
        require_result("reserved MXCSR rejection",
                       switchyard_fex_thread_import_state(thread, &invalid_state),
                       SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
        invalid_state = state;
        invalid_state.segment_base[0] = UINT64_MAX;
        require_result("wide cached segment rejection",
                       switchyard_fex_thread_import_state(thread, &invalid_state),
                       SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    }
    require_result("state import", switchyard_fex_thread_import_state(thread, &state),
                   SWITCHYARD_FEX_OK);
    exported.size = sizeof(exported);
    exported.version = SWITCHYARD_FEX_STATE_VERSION;
    require_result("state export", switchyard_fex_thread_export_state(thread, &exported),
                   SWITCHYARD_FEX_OK);
    if (exported.rflags != state.rflags || exported.mxcsr != state.mxcsr ||
        exported.segment_base[4] != state.segment_base[4] ||
        exported.segment_base[5] != state.segment_base[5] ||
        memcmp(exported.xmm, state.xmm, sizeof(state.xmm)) ||
        memcmp(exported.ymm_high, state.ymm_high, sizeof(state.ymm_high)) ||
        memcmp(exported.x87, state.x87, sizeof(state.x87)))
    {
        fprintf(stderr, "x64 state round trip failed\n");
        exit(1);
    }
    if (run_full_test) test_flag_import_round_trip(thread, &state);

    memset(&stop, 0, sizeof(stop));
    stop.size = sizeof(stop) - 1;
    stop.version = SWITCHYARD_FEX_STOP_VERSION;
    require_result("short stop rejection",
                   execute_until_hlt(
                       thread, (uintptr_t)&guest_code[18], &stop),
                   SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    stop.size = sizeof(stop);
    stop.version = SWITCHYARD_FEX_STOP_VERSION + 1;
    require_result("stop ABI rejection",
                   execute_until_hlt(
                       thread, (uintptr_t)&guest_code[18], &stop),
                   SWITCHYARD_FEX_ERROR_ABI_MISMATCH);
    initialize_stop(&stop);
    initialize_execution(&execution, (uintptr_t)&guest_code[18],
                         &execution_doorbell);
    execution.size--;
    require_result("short execution rejection",
                   switchyard_fex_thread_execute(thread, &execution, &stop),
                   SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    initialize_execution(&execution, (uintptr_t)&guest_code[18],
                         &execution_doorbell);
    execution.version++;
    require_result("execution ABI rejection",
                   switchyard_fex_thread_execute(thread, &execution, &stop),
                   SWITCHYARD_FEX_ERROR_ABI_MISMATCH);
    initialize_execution(&execution, (uintptr_t)&guest_code[18],
                         &execution_doorbell);
    execution.suspend_doorbell++;
    require_result("unaligned execution doorbell rejection",
                   switchyard_fex_thread_execute(thread, &execution, &stop),
                   SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    initialize_execution(&execution, (uintptr_t)&guest_code[18],
                         &execution_doorbell);
    execution.suspend_doorbell = 0;
    require_result("missing execution doorbell rejection",
                   switchyard_fex_thread_execute(thread, &execution, &stop),
                   SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    initialize_stop(&stop);
    result = execute_until_hlt(
        thread, (uintptr_t)&guest_code[18], &stop);
    if (result != SWITCHYARD_FEX_OK) fail_result("first execution", result);
    if (stop.reason != SWITCHYARD_FEX_STOP_HLT || stop.rip != (uintptr_t)&guest_code[18])
    {
        fprintf(stderr, "first execution returned the wrong stop\n");
        exit(1);
    }
    exported.size = sizeof(exported);
    exported.version = SWITCHYARD_FEX_STATE_VERSION;
    require_result("post-execution export", switchyard_fex_thread_export_state(thread, &exported),
                   SWITCHYARD_FEX_OK);
    if (exported.gpr[0] != UINT64_C(0x112233445566778d))
    {
        fprintf(stderr, "first result mismatch: 0x%016" PRIx64 "\n", exported.gpr[0]);
        exit(1);
    }
    for (register_index = 1; register_index < 16; ++register_index)
        if (exported.gpr[register_index] != state.gpr[register_index])
        {
            fprintf(stderr, "generated-code GPR %zu preservation failed\n", register_index);
            exit(1);
        }
    if (memcmp(exported.xmm, state.xmm, sizeof(state.xmm)) ||
        memcmp(exported.ymm_high, state.ymm_high, sizeof(state.ymm_high)) ||
        memcmp(exported.x87, state.x87, sizeof(state.x87)))
    {
        fprintf(stderr, "generated-code vector/x87 preservation failed\n");
        exit(1);
    }

    guest_code[2] = 0x90;
    require_result("code invalidation",
                   switchyard_fex_process_invalidate_code(
                       process, (uintptr_t)guest_code, sizeof(guest_code)),
                   SWITCHYARD_FEX_OK);
    initialize_state(&state, stack, sizeof(stack));
    require_result("second state import", switchyard_fex_thread_import_state(thread, &state),
                   SWITCHYARD_FEX_OK);
    initialize_stop(&stop);
    require_result("second execution",
                   execute_until_hlt(
                       thread, (uintptr_t)&guest_code[18], &stop),
                   SWITCHYARD_FEX_OK);
    exported.size = sizeof(exported);
    exported.version = SWITCHYARD_FEX_STATE_VERSION;
    require_result("second export", switchyard_fex_thread_export_state(thread, &exported),
                   SWITCHYARD_FEX_OK);
    if (exported.gpr[0] != UINT64_C(0x1122334455667795))
    {
        fprintf(stderr, "second result mismatch: 0x%016" PRIx64 "\n", exported.gpr[0]);
        exit(1);
    }

    if (run_full_test)
    {
        static _Alignas(16) unsigned char syscall_code[3] = {0x0f, 0x05, 0xf4};
        static _Alignas(16) unsigned char int2e_code[3] = {0xcd, 0x2e, 0xf4};
        static _Alignas(16) unsigned char invalid_code[2] = {0x0f, 0x0b};
        static _Alignas(16) unsigned char single_step_code[2] = {0xf1, 0xf4};
        static _Alignas(16) unsigned char async_suspend_code[33];
        /* In ARM64EC mode only int 0x2e is a syscall.  int 0x80 therefore
         * produces a guest GP fault. */
        static _Alignas(16) unsigned char fault_code[2] = {0xcd, 0x80};
        pthread_t workers[WORKER_COUNT];
        atomic_uint passes = 0;
        pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t start_condition = PTHREAD_COND_INITIALIZER;
        unsigned int ready_count = 0;
        int start = 0;
        struct worker_data data = {
            process, &passes, &start_mutex, &start_condition, &ready_count, &start
        };
        size_t i;

        require_result("syscall code invalidation",
                       switchyard_fex_process_invalidate_code(
                           process, (uintptr_t)syscall_code, sizeof(syscall_code)),
                       SWITCHYARD_FEX_OK);
        initialize_state(&state, stack, sizeof(stack));
        state.rip = (uintptr_t)syscall_code;
        require_result("syscall state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("syscall stop",
                       execute_until_hlt(thread, (uintptr_t)&syscall_code[2], &stop),
                       SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_SYSCALL ||
            stop.rip != (uintptr_t)&syscall_code[2] || stop.signal)
        {
            fprintf(stderr, "SYSCALL returned the wrong external stop\n");
            exit(1);
        }
        exported.size = sizeof(exported);
        exported.version = SWITCHYARD_FEX_STATE_VERSION;
        require_result("syscall state export",
                       switchyard_fex_thread_export_state(thread, &exported),
                       SWITCHYARD_FEX_OK);
        if (exported.rip != (uintptr_t)&syscall_code[2] ||
            exported.gpr[1] != (uintptr_t)&syscall_code[2])
        {
            fprintf(stderr, "SYSCALL did not preserve its architectural continuation\n");
            exit(1);
        }
        initialize_stop(&stop);
        require_result("post-syscall thread reuse",
                       execute_until_hlt(thread, (uintptr_t)&syscall_code[2], &stop),
                       SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_HLT)
        {
            fprintf(stderr, "post-SYSCALL execution returned the wrong stop\n");
            exit(1);
        }

        require_result("int2e code invalidation",
                       switchyard_fex_process_invalidate_code(
                           process, (uintptr_t)int2e_code, sizeof(int2e_code)),
                       SWITCHYARD_FEX_OK);
        initialize_state(&state, stack, sizeof(stack));
        state.rip = (uintptr_t)int2e_code;
        require_result("int2e state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("int2e syscall stop",
                       execute_until_hlt(thread, (uintptr_t)&int2e_code[2], &stop),
                       SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_SYSCALL ||
            stop.rip != (uintptr_t)&int2e_code[2] || stop.signal)
        {
            fprintf(stderr, "INT 2E returned the wrong external stop\n");
            exit(1);
        }

        require_result("invalid code invalidation",
                       switchyard_fex_process_invalidate_code(
                           process, (uintptr_t)invalid_code, sizeof(invalid_code)),
                       SWITCHYARD_FEX_OK);
        initialize_state(&state, stack, sizeof(stack));
        state.rip = (uintptr_t)invalid_code;
        require_result("invalid code state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("invalid instruction stop",
                       execute_until_hlt(thread, 0, &stop), SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_INVALID_INSTRUCTION ||
            stop.rip != (uintptr_t)invalid_code || stop.signal != SIGILL ||
            stop.trap_number != 6)
        {
            fprintf(stderr, "UD2 returned the wrong external stop\n");
            exit(1);
        }

        require_result("single-step code invalidation",
                       switchyard_fex_process_invalidate_code(
                           process, (uintptr_t)single_step_code,
                           sizeof(single_step_code)),
                       SWITCHYARD_FEX_OK);
        initialize_state(&state, stack, sizeof(stack));
        state.rip = (uintptr_t)single_step_code;
        require_result("single-step state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("single-step stop",
                       execute_until_hlt(thread, 0, &stop), SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_SINGLE_STEP ||
            stop.rip != (uintptr_t)&single_step_code[1] ||
            stop.signal != SIGTRAP || stop.trap_number != 1)
        {
            fprintf(stderr, "INT1 returned the wrong external stop\n");
            exit(1);
        }

        initialize_state(&state, stack, sizeof(stack));
        require_result("suspend state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        atomic_store_explicit(&execution_doorbell, 1, memory_order_release);
        initialize_execution(&execution, (uintptr_t)&guest_code[18],
                             &execution_doorbell);
        initialize_stop(&stop);
        require_result("preset suspend stop",
                       switchyard_fex_thread_execute(thread, &execution, &stop),
                       SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_SUSPEND ||
            stop.rip != (uintptr_t)guest_code || stop.signal)
        {
            fprintf(stderr, "preset doorbell returned the wrong suspend stop\n");
            exit(1);
        }
        atomic_store_explicit(&execution_doorbell, 0, memory_order_release);
        initialize_stop(&stop);
        require_result("post-suspend thread reuse",
                       execute_until_hlt(thread, (uintptr_t)&guest_code[18], &stop),
                       SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_HLT)
        {
            fprintf(stderr, "post-suspend execution returned the wrong stop\n");
            exit(1);
        }

        {
            static const unsigned char async_template[33] = {
                0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
                0x48, 0xc7, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x48, 0xb9, 0x00, 0xe1, 0xf5, 0x05, 0x00, 0x00, 0x00, 0x00,
                0x48, 0xff, 0xc9,
                0x75, 0xfb,
                0xf4,
            };
            struct suspend_worker_data suspend_data;
            atomic_uint suspend_doorbell = 0;
            pthread_t suspend_worker;
            unsigned int spin;

            memcpy(async_suspend_code, async_template, sizeof(async_template));
            {
                const uint64_t progress_address =
                    (uintptr_t)&async_suspend_progress;
                memcpy(async_suspend_code + 2, &progress_address,
                       sizeof(progress_address));
            }
            require_result("async suspend code invalidation",
                           switchyard_fex_process_invalidate_code(
                               process, (uintptr_t)async_suspend_code,
                               sizeof(async_suspend_code)),
                           SWITCHYARD_FEX_OK);
            initialize_state(&state, stack, sizeof(stack));
            state.rip = (uintptr_t)async_suspend_code;
            require_result("async suspend state import",
                           switchyard_fex_thread_import_state(thread, &state),
                           SWITCHYARD_FEX_OK);
            __atomic_store_n(&async_suspend_progress, 0, __ATOMIC_RELEASE);
            memset(&suspend_data, 0, sizeof(suspend_data));
            suspend_data.thread = thread;
            suspend_data.doorbell = &suspend_doorbell;
            if (pthread_create(&suspend_worker, NULL, suspend_worker_main,
                               &suspend_data))
            {
                fprintf(stderr, "async suspend pthread_create failed\n");
                exit(1);
            }
            for (spin = 0; spin < 5000000; ++spin)
            {
                if (__atomic_load_n(&async_suspend_progress, __ATOMIC_ACQUIRE))
                    break;
                sched_yield();
            }
            if (spin == 5000000)
            {
                atomic_store_explicit(&suspend_doorbell, 1,
                                      memory_order_release);
                pthread_join(suspend_worker, NULL);
                fprintf(stderr, "async suspend guest loop did not start\n");
                exit(1);
            }
            atomic_store_explicit(&suspend_doorbell, 1, memory_order_release);
            if (pthread_join(suspend_worker, NULL))
            {
                fprintf(stderr, "async suspend pthread_join failed\n");
                exit(1);
            }
            if (suspend_data.result != SWITCHYARD_FEX_OK ||
                suspend_data.stop.reason != SWITCHYARD_FEX_STOP_SUSPEND ||
                suspend_data.stop.rip != (uintptr_t)&async_suspend_code[27] ||
                suspend_data.stop.signal)
            {
                fprintf(stderr,
                        "running backward edge returned result=%u reason=%u "
                        "rip=%#" PRIx64 " expected=%#" PRIx64 " signal=%u\n",
                        (unsigned int)suspend_data.result,
                        suspend_data.stop.reason, suspend_data.stop.rip,
                        (uint64_t)(uintptr_t)&async_suspend_code[27],
                        suspend_data.stop.signal);
                exit(1);
            }
        }

        require_result("fault code invalidation",
                       switchyard_fex_process_invalidate_code(
                           process, (uintptr_t)fault_code, sizeof(fault_code)),
                       SWITCHYARD_FEX_OK);
        state.rip = (uintptr_t)fault_code;
        require_result("fault state import", switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("fault classification",
                       execute_until_hlt(
                           thread, (uintptr_t)&guest_code[18], &stop),
                       SWITCHYARD_FEX_ERROR_GUEST_FAULT);
        if (stop.reason != SWITCHYARD_FEX_STOP_GUEST_FAULT || stop.rip != (uintptr_t)fault_code)
        {
            fprintf(stderr, "guest GP fault was misclassified as HLT\n");
            exit(1);
        }

        require_result("thread destroy before concurrency", switchyard_fex_thread_destroy(thread),
                       SWITCHYARD_FEX_OK);
        thread = NULL;
        require_result("fresh shared code cache", switchyard_fex_thread_create(process, &thread),
                       SWITCHYARD_FEX_OK);
        require_result("clear shared code cache", switchyard_fex_thread_clear_code_cache(thread),
                       SWITCHYARD_FEX_OK);
        require_result("temporary thread destroy", switchyard_fex_thread_destroy(thread),
                       SWITCHYARD_FEX_OK);
        thread = NULL;

        for (i = 0; i < WORKER_COUNT; ++i)
            if (pthread_create(&workers[i], NULL, worker_main, &data))
            {
                fprintf(stderr, "pthread_create failed\n");
                exit(1);
            }
        for (i = 0; i < WORKER_COUNT; ++i)
            if (pthread_join(workers[i], NULL))
            {
                fprintf(stderr, "pthread_join failed\n");
                exit(1);
            }
        if (atomic_load_explicit(&passes, memory_order_relaxed) != WORKER_COUNT)
        {
            fprintf(stderr, "concurrency test passed %u/%u\n",
                    atomic_load_explicit(&passes, memory_order_relaxed), WORKER_COUNT);
            exit(1);
        }
        pthread_cond_destroy(&start_condition);
        pthread_mutex_destroy(&start_mutex);
    }
    else
    {
        require_result("thread destroy", switchyard_fex_thread_destroy(thread),
                       SWITCHYARD_FEX_OK);
        thread = NULL;
    }

    if (read_x18() != initial_x18)
    {
        fprintf(stderr, "x18 changed across the process cycle\n");
        exit(1);
    }
    require_result("process destroy", switchyard_fex_process_destroy(process),
                   SWITCHYARD_FEX_OK);
}

static void store_u64(unsigned char *destination, uint64_t value)
{
    memcpy(destination, &value, sizeof(value));
}

/* One runtime-computed base-register access for each width and direction.
 * Each site first crosses the 16-byte granule, then visits every low bit and
 * retries the original address without invalidation. A repaired site must
 * perform exact-width accesses and must not repeatedly trap. */
static const unsigned char dynamic_tso_code[8][16] = {
    {0x88, 0x06, 0xf4},       {0x8a, 0x06, 0xf4},
    {0x66, 0x89, 0x06, 0xf4}, {0x66, 0x8b, 0x06, 0xf4},
    {0x89, 0x06, 0xf4},       {0x8b, 0x06, 0xf4},
    {0x48, 0x89, 0x06, 0xf4}, {0x48, 0x8b, 0x06, 0xf4},
};

struct tso_worker_data
{
    struct worker_data gate;
    uint64_t rip;
    unsigned char *destination;
    atomic_uint *repairs;
};

static void *tso_worker_main(void *opaque)
{
    struct tso_worker_data *data = opaque;
    struct switchyard_fex_thread *thread = NULL;
    struct switchyard_fex_x64_state state;
    struct switchyard_fex_stop stop;
    _Alignas(16) unsigned char stack[4096];
    const uint64_t value = UINT64_C(0x123456789abcdef0);
    const uintptr_t initial_x18 = read_x18();
    uint64_t actual = 0;
    int prepared = 0;

    if (switchyard_fex_thread_create(data->gate.process, &thread) == SWITCHYARD_FEX_OK)
    {
        initialize_state(&state, stack, sizeof(stack));
        state.rip = data->rip;
        state.gpr[0] = value;
        state.gpr[6] = (uintptr_t)data->destination;
        prepared = switchyard_fex_thread_import_state(thread, &state) == SWITCHYARD_FEX_OK;
    }
    pthread_mutex_lock(data->gate.start_mutex);
    if (++*data->gate.ready_count == WORKER_COUNT)
    {
        *data->gate.start = 1;
        pthread_cond_broadcast(data->gate.start_condition);
    }
    while (!*data->gate.start)
        pthread_cond_wait(data->gate.start_condition, data->gate.start_mutex);
    pthread_mutex_unlock(data->gate.start_mutex);

    unaligned_repair_thread = thread;
    unaligned_repair_count = 0;
    initialize_stop(&stop);
    if (prepared && execute_until_hlt(thread, data->rip + 3, &stop) == SWITCHYARD_FEX_OK)
    {
        memcpy(&actual, data->destination, sizeof(actual));
        if (stop.reason == SWITCHYARD_FEX_STOP_HLT && stop.rip == data->rip + 3 &&
            actual == value && read_x18() == initial_x18 && unaligned_repair_count <= 1)
            atomic_fetch_add_explicit(data->gate.passes, 1, memory_order_relaxed);
    }
    unaligned_repair_thread = NULL;
    atomic_fetch_add_explicit(data->repairs, (unsigned int)unaligned_repair_count, memory_order_relaxed);
    if (thread && switchyard_fex_thread_destroy(thread) != SWITCHYARD_FEX_OK)
        atomic_fetch_sub_explicit(data->gate.passes, 1, memory_order_relaxed);
    return NULL;
}

static void test_concurrent_tso(struct switchyard_fex_process *process,
                                uint64_t rip, unsigned char *destinations)
{
    struct sigaction action = {0}, old_bus;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    pthread_t workers[WORKER_COUNT];
    struct tso_worker_data data[WORKER_COUNT];
    atomic_uint passes = 0, repairs = 0;
    unsigned int ready = 0, i;
    int start = 0;

    require_result("concurrent TSO invalidation",
                   switchyard_fex_process_invalidate_code(process, rip, 16), SWITCHYARD_FEX_OK);
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = capture_jit_fault;
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGBUS, &action, &old_bus)) exit(1);
    for (i = 0; i < WORKER_COUNT; ++i)
    {
        data[i] = (struct tso_worker_data) {
            .gate = {process, &passes, &mutex, &condition, &ready, &start},
            .rip = rip, .destination = destinations + i * 32 + 15, .repairs = &repairs,
        };
        if (pthread_create(&workers[i], NULL, tso_worker_main, &data[i])) exit(1);
    }
    for (i = 0; i < WORKER_COUNT; ++i)
        if (pthread_join(workers[i], NULL)) exit(1);
    if (sigaction(SIGBUS, &old_bus, NULL)) exit(1);
    pthread_cond_destroy(&condition);
    pthread_mutex_destroy(&mutex);
    if (atomic_load(&passes) != WORKER_COUNT || !atomic_load(&repairs))
    {
        fprintf(stderr, "shared-code TSO repair failed passes=%u repairs=%u\n",
                atomic_load(&passes), atomic_load(&repairs));
        exit(1);
    }
}

static void test_dynamic_tso(struct switchyard_fex_process *process,
                             struct switchyard_fex_thread *thread,
                             uint64_t code_address, uint64_t data_address,
                             unsigned char *data, uint64_t stack_top)
{
    struct sigaction action = {0}, old_bus;
    struct switchyard_fex_x64_state state, exported;
    struct switchyard_fex_stop stop;
    _Alignas(16) unsigned char scratch[256];
    unsigned char expected_bytes[48];
    const uint64_t value = UINT64_C(0x8877665544332211);
    unsigned int site, step;
    unsigned int total_repairs = 0;

    sigemptyset(&action.sa_mask);
    action.sa_sigaction = capture_jit_fault;
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGBUS, &action, &old_bus))
    {
        fprintf(stderr, "failed to install unaligned TSO signal handler\n");
        exit(1);
    }
    for (site = 0; site < 8; ++site)
    {
        const unsigned int width = 1u << (site / 2);
        const int load = (int)(site & 1);
        const uint64_t rip = code_address + site * sizeof(dynamic_tso_code[0]);
        const uint64_t hlt = rip + ((width == 2 || width == 8) ? 3u : 2u);
        sig_atomic_t first_repairs = 0;

        require_result("dynamic TSO code invalidation",
                       switchyard_fex_process_invalidate_code(process, rip, 16),
                       SWITCHYARD_FEX_OK);
        unaligned_repair_count = 0;
        for (step = 0; step < 18; ++step)
        {
            const unsigned int offset = (step == 0 || step == 17) ? 15 : step - 1;
            uint64_t expected_rax = value;
            const uintptr_t initial_x18 = read_x18();

            memset(data, 0xa5, sizeof(expected_bytes));
            memset(expected_bytes, 0xa5, sizeof(expected_bytes));
            if (load)
            {
                uint64_t loaded = 0;
                memcpy(&loaded, data + offset, width);
                if (width == 4 || width == 8) expected_rax = loaded;
                else expected_rax = (value & ~(UINT64_MAX >> (64 - width * 8))) | loaded;
            }
            else memcpy(expected_bytes + offset, &value, width);
            initialize_state(&state, scratch, sizeof(scratch));
            state.rip = rip;
            state.gpr[0] = value;
            state.gpr[4] = stack_top;
            state.gpr[6] = data_address + offset;
            require_result("dynamic TSO state import",
                           switchyard_fex_thread_import_state(thread, &state),
                           SWITCHYARD_FEX_OK);
            unaligned_repair_thread = thread;
            initialize_stop(&stop);
            require_result("dynamic TSO execution",
                           execute_until_hlt(thread, hlt, &stop), SWITCHYARD_FEX_OK);
            unaligned_repair_thread = NULL;
            exported.size = sizeof(exported);
            exported.version = SWITCHYARD_FEX_STATE_VERSION;
            require_result("dynamic TSO state export",
                           switchyard_fex_thread_export_state(thread, &exported),
                           SWITCHYARD_FEX_OK);
            state.gpr[0] = expected_rax;
            if (stop.reason != SWITCHYARD_FEX_STOP_HLT || stop.rip != hlt ||
                memcmp(data, expected_bytes, sizeof(expected_bytes)) ||
                memcmp(state.gpr, exported.gpr, sizeof(state.gpr)) ||
                state.rflags != exported.rflags || state.mxcsr != exported.mxcsr ||
                memcmp(state.xmm, exported.xmm, sizeof(state.xmm)) ||
                memcmp(state.ymm_high, exported.ymm_high, sizeof(state.ymm_high)) ||
                memcmp(state.x87, exported.x87, sizeof(state.x87)) ||
                read_x18() != initial_x18)
            {
                fprintf(stderr, "dynamic TSO mismatch site=%u offset=%u\n", site, offset);
                exit(1);
            }
            if (!step) first_repairs = unaligned_repair_count;
            if (unaligned_repair_count != first_repairs || first_repairs > 1)
            {
                fprintf(stderr, "dynamic TSO site repeatedly faults site=%u step=%u\n", site, step);
                exit(1);
            }
        }
        total_repairs += (unsigned int)first_repairs;
    }
    if (!total_repairs)
    {
        fprintf(stderr, "dynamic TSO regression did not exercise signal repair\n");
        exit(1);
    }
    if (sigaction(SIGBUS, &old_bus, NULL))
    {
        fprintf(stderr, "failed to restore unaligned TSO signal handler\n");
        exit(1);
    }
}

static void test_external_callret_balance(struct switchyard_fex_process *process,
                                         struct switchyard_fex_thread *thread,
                                         uint64_t ec_target, uint64_t stack_top)
{
    static const unsigned char code_template[] = {
        0xff, 0xd0,                         /* call rax (external EC target) */
        0xe9, 0xf9, 0xff, 0xff, 0xff,       /* jmp back to call */
    };
    const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *code = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANON, -1, 0);
    struct switchyard_fex_x64_state state;
    struct switchyard_fex_stop stop;
    unsigned char scratch[64];
    unsigned int iteration;
    const uintptr_t initial_x18 = read_x18();
    struct sigaction action = {0}, previous_bus;

    if (code == MAP_FAILED)
    {
        fprintf(stderr, "external call-ret code allocation failed\n");
        exit(1);
    }
    memcpy(code, code_template, sizeof(code_template));
    code[0x40] = 0xc3; /* Unmatched RET to exercise the opposite guard. */
    if (mprotect(code, page_size, PROT_READ)) exit(1);
    initialize_state(&state, scratch, sizeof(scratch));
    state.rip = (uintptr_t)code;
    state.gpr[0] = ec_target;
    state.gpr[4] = stack_top;
    action.sa_sigaction = capture_jit_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGBUS, &action, &previous_bus)) exit(1);
    if (sigsetjmp(fault_jump, 1))
    {
        fprintf(stderr, "external calls exhausted internal call-ret stack: signal %d\n", captured_signal);
        exit(1);
    }
    fault_armed = 1;
    callret_repair_thread = thread;
    callret_repair_count = 0;
    /* More than the default 1-MiB downward headroom at 16 bytes per call. */
    for (iteration = 0; iteration < 70000; ++iteration)
    {
        require_result("external call-ret import",
                       switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("external call-ret execute", execute_until_hlt(thread, 0, &stop),
                       SWITCHYARD_FEX_OK);
        require_result("external call-ret export",
                       switchyard_fex_thread_export_state(thread, &state), SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_EC_TRANSITION || stop.rip != ec_target ||
            state.gpr[4] != stack_top - 8 || read_x18() != initial_x18)
        {
            fprintf(stderr, "external call-ret architectural state mismatch\n");
            exit(1);
        }
        /* The native callee consumes the guest return address without a JIT RET. */
        state.rip = (uintptr_t)code + 2;
        state.gpr[4] += 8;
    }
    if (callret_repair_count != 1)
    {
        fprintf(stderr, "call-ret overflow did not repair exactly one guard fault\n");
        exit(1);
    }
    store_u64((unsigned char *)(uintptr_t)(stack_top - 8), ec_target);
    for (iteration = 0; iteration < 210000; ++iteration)
    {
        state.rip = (uintptr_t)code + 0x40;
        state.gpr[4] = stack_top - 8;
        require_result("unmatched return import",
                       switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("unmatched return execute", execute_until_hlt(thread, 0, &stop),
                       SWITCHYARD_FEX_OK);
        require_result("unmatched return export",
                       switchyard_fex_thread_export_state(thread, &state), SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_EC_TRANSITION || stop.rip != ec_target ||
            state.gpr[4] != stack_top || read_x18() != initial_x18)
        {
            fprintf(stderr, "unmatched return architectural state mismatch\n");
            exit(1);
        }
    }
    fault_armed = 0;
    callret_repair_thread = NULL;
    if (callret_repair_count != 2)
    {
        fprintf(stderr, "external call-ret guard recovery was not exercised\n");
        exit(1);
    }
    if (sigaction(SIGBUS, &previous_bus, NULL)) exit(1);
    require_result("external call-ret invalidation",
                   switchyard_fex_process_invalidate_code(process, (uintptr_t)code, page_size),
                   SWITCHYARD_FEX_OK);
    if (munmap(code, page_size)) exit(1);
}

static void run_low_shadow_cycle(void)
{
    static const unsigned char code_template[49] = {
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,             /* mov rax, data */
        0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0,             /* mov rcx, value */
        0x48, 0x89, 0x08,                                /* mov [rax], rcx */
        0x48, 0x83, 0x00, 0x05,                          /* add qword [rax], 5 */
        0x48, 0x8b, 0x10,                                /* mov rdx, [rax] */
        0x52,                                            /* push rdx */
        0x5b,                                            /* pop rbx */
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,             /* mov rax, high_data */
        0x48, 0x89, 0x18,                                /* mov [rax], rbx */
        0x48, 0x8b, 0x30,                                /* mov rsi, [rax] */
        0xf4,                                            /* hlt */
    };
    static const unsigned char memory_contract_template[] = {
        0x48, 0xbf, 0x44, 0x44, 0x33, 0x33, 0x22, 0x22, 0x11, 0x11,
        0x48, 0xc7, 0x07, 0x0a, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0x47, 0x08, 0x78, 0x56, 0x34, 0x12,
        0x48, 0xc7, 0xc1, 0x05, 0x00, 0x00, 0x00,
        0xf0, 0x48, 0x0f, 0xc1, 0x0f,
        0x48, 0x89, 0x8f, 0xb8, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0xc2, 0x14, 0x00, 0x00, 0x00,
        0x48, 0x87, 0x17,
        0x48, 0x89, 0x97, 0xb0, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0xc0, 0x14, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0xc6, 0x1e, 0x00, 0x00, 0x00,
        0xf0, 0x48, 0x0f, 0xb1, 0x37,
        0xf3, 0x0f, 0x6f, 0x07,
        0xf3, 0x0f, 0x7f, 0x47, 0x10,
        0x48, 0x89, 0xfe,
        0x48, 0x8d, 0x7f, 0x20,
        0x48, 0xc7, 0xc1, 0x04, 0x00, 0x00, 0x00,
        0xf3, 0x48, 0xa5,
        0x48, 0x8d, 0x7f, 0x20,
        0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x48, 0xc7, 0xc1, 0x04, 0x00, 0x00, 0x00,
        0xf3, 0x48, 0xab,
        0x66, 0x0f, 0xe7, 0x47, 0x20,
        0x0f, 0xae, 0xf8,
        0x0f, 0x18, 0x0f,
        0x0f, 0xae, 0x3f,
        0x48, 0x8d, 0x7f, 0x40,
        0x48, 0xc7, 0x07, 0x01, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0x47, 0x08, 0x02, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0xc2, 0x02, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0xc3, 0x03, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0xc1, 0x04, 0x00, 0x00, 0x00,
        0xf0, 0x48, 0x0f, 0xc7, 0x0f,
        0xf4,
    };
    static const unsigned char fault_template[] = {
        0x48, 0x8b, 0x06, /* mov rax,[rsi] */
        0xf4,
    };
    static const unsigned char write_fault_template[] = {
        0x48, 0x89, 0x06, /* mov [rsi],rax */
        0xf4,
    };
    static const unsigned char high_stack_template[] = {
        0x56, /* push rsi */
        0x53, /* push rbx */
        0x5a, /* pop rdx */
        0x59, /* pop rcx */
        0xf4,
    };
    static const unsigned char high_stack_call_template[] = {
        0xff, 0x15, 0x0a, 0x00, 0x00, 0x00, /* call qword ptr [rip+0xa] */
        0xf4,                               /* return target */
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* target pointer */
        0x48, 0xba, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0xc3,
    };
    static const unsigned char unaligned_store_template[] = {
        0x48, 0xb8, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x48, 0x89, 0x05, 0x00, 0x00, 0x00, 0x00, /* mov [rip+disp32],rax */
        0xf4,
    };
    const uint64_t shadow_base = UINT64_C(0x10000000000);
    const uint64_t shadow_size = UINT64_C(0x100000000);
    const uint64_t guest_code_address = UINT64_C(0x02000000);
    const long host_page_size_long = sysconf(_SC_PAGESIZE);
    size_t host_page_size;
    size_t mapping_size;
    unsigned char *mapping;
    unsigned char *code;
    unsigned char *memory_contract_code;
    unsigned char *fault_code;
    unsigned char *write_fault_code;
    unsigned char *boundary_fault_code;
    unsigned char *transition_branch_code;
    unsigned char *transition_target_code;
    unsigned char *high_stack_code;
    unsigned char *high_stack_call_code;
    unsigned char *unaligned_store_code;
    unsigned char *boundary_mapping = MAP_FAILED;
    unsigned char *high_stack_mapping = MAP_FAILED;
    unsigned char *high_unaligned_mapping = MAP_FAILED;
    uint64_t guest_data_address;
    uint64_t guest_memory_contract_address;
    uint64_t guest_fault_code_address;
    uint64_t guest_write_fault_code_address;
    uint64_t guest_boundary_fault_code_address;
    uint64_t guest_transition_branch_address;
    uint64_t guest_transition_address;
    uint64_t guest_high_stack_code_address;
    uint64_t guest_high_stack_call_code_address;
    uint64_t guest_unaligned_store_code_address;
    uint64_t guest_stack_top;
    uint64_t guest_guard_address;
    uint64_t high_data = 0;
    uint64_t *low_data;
    uint64_t *low_words;
    _Alignas(8) uint64_t ec_bitmap[256] = {0};
    struct test_executable_policy executable_policy = {0};
    struct switchyard_fex_config config = {
        .size = sizeof(config),
        .abi_version = SWITCHYARD_FEX_ABI_VERSION,
        .flags = SWITCHYARD_FEX_CONFIG_MULTIBLOCK,
        .low_va_shadow_base = shadow_base,
        .low_va_shadow_size = shadow_size,
    };
    struct switchyard_fex_process *process = NULL;
    struct switchyard_fex_thread *thread = NULL;
    struct switchyard_fex_x64_state state;
    struct switchyard_fex_x64_state exported;
    struct switchyard_fex_execution execution;
    struct switchyard_fex_stop stop;
    _Alignas(16) unsigned char state_scratch[256] = {0};
    uint64_t expected;
    unsigned int iteration;
    uint32_t ec_page_shift = 0;

    if (host_page_size_long < 4096 ||
        (host_page_size_long & (host_page_size_long - 1)) != 0)
    {
        fprintf(stderr, "invalid host page size for low-shadow test\n");
        exit(1);
    }
    host_page_size = (size_t)host_page_size_long;
    if (host_page_size > SIZE_MAX / 4)
    {
        fprintf(stderr, "host page size overflow\n");
        exit(1);
    }
    mapping_size = host_page_size * 4;
    mapping = mmap((void *)(uintptr_t)(shadow_base + guest_code_address), mapping_size,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED ||
        (uintptr_t)mapping != shadow_base + guest_code_address)
    {
        if (mapping != MAP_FAILED) munmap(mapping, mapping_size);
        fprintf(stderr, "failed to obtain exact Wine low-shadow test mapping\n");
        exit(1);
    }

    code = mapping;
    memory_contract_code = code + 0x100;
    fault_code = code + 0x300;
    write_fault_code = code + 0x320;
    boundary_fault_code = code + 0x340;
    transition_branch_code = code + 0x380;
    high_stack_code = code + 0x3c0;
    high_stack_call_code = code + 0x400;
    unaligned_store_code = code + 0x480;
    transition_target_code = code + host_page_size * 2;
    low_data = (uint64_t *)(mapping + host_page_size);
    low_words = low_data;
    guest_data_address = guest_code_address + host_page_size;
    guest_memory_contract_address = guest_code_address + 0x100;
    guest_fault_code_address = guest_code_address + 0x300;
    guest_write_fault_code_address = guest_code_address + 0x320;
    guest_boundary_fault_code_address = guest_code_address + 0x340;
    guest_transition_branch_address = guest_code_address + 0x380;
    guest_high_stack_code_address = guest_code_address + 0x3c0;
    guest_high_stack_call_code_address = guest_code_address + 0x400;
    guest_unaligned_store_code_address = guest_code_address + 0x480;
    guest_transition_address = guest_code_address + host_page_size * 2;
    guest_stack_top = guest_code_address + host_page_size * 3;
    guest_guard_address = guest_stack_top;
    memcpy(code, code_template, sizeof(code_template));
    memcpy(memory_contract_code, memory_contract_template,
           sizeof(memory_contract_template));
    memcpy(fault_code, fault_template, sizeof(fault_template));
    memcpy(write_fault_code, write_fault_template, sizeof(write_fault_template));
    memcpy(boundary_fault_code, fault_template, sizeof(fault_template));
    memcpy(high_stack_code, high_stack_template, sizeof(high_stack_template));
    memcpy(high_stack_call_code, high_stack_call_template,
           sizeof(high_stack_call_template));
    memcpy(unaligned_store_code, unaligned_store_template,
           sizeof(unaligned_store_template));
    memcpy(code + 0x500, dynamic_tso_code, sizeof(dynamic_tso_code));
    store_u64(high_stack_call_code + 16,
              guest_high_stack_call_code_address + 24);
    {
        const int64_t displacement =
            (int64_t)(guest_data_address + 12) -
            (int64_t)(guest_unaligned_store_code_address + 17);
        const int32_t displacement32 = (int32_t)displacement;

        if ((int64_t)displacement32 != displacement)
        {
            fprintf(stderr, "unaligned RIP-relative displacement overflow\n");
            exit(1);
        }
        memcpy(unaligned_store_code + 13, &displacement32,
               sizeof(displacement32));
    }
    transition_branch_code[0] = 0xe9;
    {
        const int64_t displacement =
            (int64_t)guest_transition_address -
            (int64_t)(guest_transition_branch_address + 5);
        const int32_t displacement32 = (int32_t)displacement;

        if ((int64_t)displacement32 != displacement)
        {
            fprintf(stderr, "transition branch displacement overflow\n");
            exit(1);
        }
        memcpy(transition_branch_code + 1, &displacement32,
               sizeof(displacement32));
    }
    transition_target_code[0] = 0xf4;
    store_u64(code + 2, guest_data_address);
    store_u64(memory_contract_code + 2, guest_data_address);
    store_u64(code + 12, UINT64_C(0x1122334455667788));
    store_u64(code + 34, (uintptr_t)&high_data);
    if (mprotect(code, host_page_size, PROT_READ))
    {
        fprintf(stderr, "failed to protect low-shadow code page\n");
        exit(1);
    }
    if (mprotect(mapping + host_page_size * 3, host_page_size, PROT_NONE))
    {
        fprintf(stderr, "failed to protect low-shadow guard page\n");
        exit(1);
    }

    while ((UINT64_C(1) << ec_page_shift) < host_page_size)
        ++ec_page_shift;
    config.ec_code_bitmap = (uintptr_t)ec_bitmap;
    config.highest_user_address =
        guest_code_address + mapping_size - 1;
    config.ec_page_shift = ec_page_shift;

    require_result("low-shadow process create",
                   switchyard_fex_process_create(&config, &process), SWITCHYARD_FEX_OK);
    require_result("low-shadow executable policy registration",
                   switchyard_fex_process_set_executable_range_query(
                       process, test_query_executable_range, &executable_policy), SWITCHYARD_FEX_OK);
    require_result("low-shadow thread create",
                   switchyard_fex_thread_create(process, &thread), SWITCHYARD_FEX_OK);

    {
        struct switchyard_fex_arm64_host_context host = {0};

        host.size = sizeof(host);
        host.version = SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION;
        host.pc = (uintptr_t)run_low_shadow_cycle;
        require_result("idle TSO repair rejection",
                       switchyard_fex_thread_repair_unaligned_tso(thread, &host),
                       SWITCHYARD_FEX_ERROR_BUSY);
        host.size--;
        require_result("short TSO context rejection",
                       switchyard_fex_thread_repair_unaligned_tso(thread, &host),
                       SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
        host.size++;
        host.version++;
        require_result("TSO context ABI rejection",
                       switchyard_fex_thread_repair_unaligned_tso(thread, &host),
                       SWITCHYARD_FEX_ERROR_ABI_MISMATCH);
        host.version--;
        host.flags = 1;
        require_result("TSO context flags rejection",
                       switchyard_fex_thread_repair_unaligned_tso(thread, &host),
                       SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    }

    high_stack_mapping = mmap(NULL, host_page_size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
    if (high_stack_mapping == MAP_FAILED ||
        (uintptr_t)high_stack_mapping < shadow_size ||
        (uintptr_t)high_stack_mapping >= shadow_base)
    {
        if (high_stack_mapping != MAP_FAILED)
            munmap(high_stack_mapping, host_page_size);
        fprintf(stderr, "failed to obtain identity-mapped high stack test page\n");
        exit(1);
    }

    high_unaligned_mapping = mmap(NULL, host_page_size * 2,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANON, -1, 0);
    if (high_unaligned_mapping == MAP_FAILED ||
        (uintptr_t)high_unaligned_mapping < shadow_size ||
        (uintptr_t)high_unaligned_mapping >= shadow_base)
    {
        if (high_unaligned_mapping != MAP_FAILED)
            munmap(high_unaligned_mapping, host_page_size * 2);
        fprintf(stderr, "failed to obtain identity-mapped unaligned-store pages\n");
        exit(1);
    }

    {
        unsigned char *high_code = high_unaligned_mapping;
        unsigned char *high_target = high_unaligned_mapping + host_page_size + 12;
        const int64_t displacement =
            (int64_t)(uintptr_t)high_target -
            (int64_t)((uintptr_t)high_code + 17);
        const int32_t displacement32 = (int32_t)displacement;
        uint64_t unaligned_value;

        if ((int64_t)displacement32 != displacement)
        {
            fprintf(stderr, "identity unaligned displacement overflow\n");
            exit(1);
        }
        memcpy(high_code, unaligned_store_template,
               sizeof(unaligned_store_template));
        memcpy(high_code + 13, &displacement32, sizeof(displacement32));
        memcpy(high_code + 0x80, dynamic_tso_code, sizeof(dynamic_tso_code));
        memset(high_target, 0, sizeof(unaligned_value));
        if (mprotect(high_code, host_page_size, PROT_READ | PROT_EXEC))
        {
            fprintf(stderr, "failed to protect identity unaligned code page\n");
            exit(1);
        }
        require_result("identity unaligned code invalidation",
                       switchyard_fex_process_invalidate_code(
                           process, (uintptr_t)high_code,
                           sizeof(unaligned_store_template)),
                       SWITCHYARD_FEX_OK);
        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = (uintptr_t)high_code;
        state.gpr[4] = (uintptr_t)(high_stack_mapping + host_page_size);
        require_result("identity unaligned store state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("identity unaligned store execution",
                       execute_until_hlt(
                           thread, (uintptr_t)high_code +
                                       sizeof(unaligned_store_template) - 1,
                           &stop),
                       SWITCHYARD_FEX_OK);
        memcpy(&unaligned_value, high_target, sizeof(unaligned_value));
        if (stop.reason != SWITCHYARD_FEX_STOP_HLT ||
            stop.rip != (uintptr_t)high_code +
                            sizeof(unaligned_store_template) - 1 ||
            unaligned_value != UINT64_C(0x0000000100000002))
        {
            fprintf(stderr, "identity unaligned qword store mismatch\n");
            exit(1);
        }
    }

    {
        uint64_t unaligned_value;

        memset((unsigned char *)low_data, 0, 16);
        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_unaligned_store_code_address;
        state.gpr[4] = guest_stack_top;
        require_result("unaligned RIP-relative store state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("unaligned RIP-relative store execution",
                       execute_until_hlt(
                           thread, guest_unaligned_store_code_address +
                                       sizeof(unaligned_store_template) - 1,
                           &stop),
                       SWITCHYARD_FEX_OK);
        memcpy(&unaligned_value, (unsigned char *)low_data + 12,
               sizeof(unaligned_value));
        if (stop.reason != SWITCHYARD_FEX_STOP_HLT ||
            stop.rip != guest_unaligned_store_code_address +
                            sizeof(unaligned_store_template) - 1 ||
            unaligned_value != UINT64_C(0x0000000100000002))
        {
            fprintf(stderr, "unaligned RIP-relative qword store mismatch\n");
            exit(1);
        }
    }

    test_dynamic_tso(process, thread, (uintptr_t)high_unaligned_mapping + 0x80,
                     (uintptr_t)high_unaligned_mapping + host_page_size,
                     high_unaligned_mapping + host_page_size,
                     (uintptr_t)high_stack_mapping + host_page_size);
    test_dynamic_tso(process, thread, guest_code_address + 0x500,
                     guest_data_address, (unsigned char *)low_data, guest_stack_top);
    test_concurrent_tso(process,
                        (uintptr_t)high_unaligned_mapping + 0x80 + 6 * 16,
                        high_unaligned_mapping + host_page_size + 0x100);

    {
        const uint64_t transition_page =
            guest_transition_address >> ec_page_shift;
        const size_t transition_word = (size_t)(transition_page >> 6);
        const uint64_t transition_mask =
            UINT64_C(1) << (transition_page & 63);

        if (transition_word >= sizeof(ec_bitmap) / sizeof(ec_bitmap[0]))
        {
            fprintf(stderr, "transition bitmap test geometry overflow\n");
            exit(1);
        }
        __atomic_fetch_or(&ec_bitmap[transition_word], transition_mask,
                          __ATOMIC_RELEASE);

        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_transition_address;
        state.gpr[4] = guest_stack_top;
        require_result("direct EC transition state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("direct EC transition stop",
                       execute_until_hlt(thread, 0, &stop),
                       SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_EC_TRANSITION ||
            stop.rip != guest_transition_address || stop.signal)
        {
            fprintf(stderr, "dispatcher returned the wrong EC transition stop\n");
            exit(1);
        }

        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_transition_branch_address;
        state.gpr[4] = guest_stack_top;
        require_result("branch EC transition state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("branch EC transition stop",
                       execute_until_hlt(thread, guest_transition_address, &stop),
                       SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_EC_TRANSITION ||
            stop.rip != guest_transition_address || stop.signal)
        {
            fprintf(stderr, "direct branch returned the wrong EC transition stop\n");
            exit(1);
        }

        __atomic_fetch_and(&ec_bitmap[transition_word], ~transition_mask,
                           __ATOMIC_RELEASE);
        require_result("cleared transition target invalidation",
                       switchyard_fex_process_invalidate_code(
                           process, guest_transition_address, host_page_size),
                       SWITCHYARD_FEX_OK);
        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_transition_branch_address;
        state.gpr[4] = guest_stack_top;
        require_result("cleared transition state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        atomic_store_explicit(&execution_doorbell, 0, memory_order_release);
        initialize_execution(&execution, guest_transition_address,
                             &execution_doorbell);
        initialize_stop(&stop);
        require_result("cleared transition execution",
                       switchyard_fex_thread_execute(thread, &execution, &stop),
                       SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_HLT ||
            stop.rip != guest_transition_address)
        {
            fprintf(stderr, "cleared EC transition remained stale in JIT code\n");
            exit(1);
        }

        /* Warm direct dispatcher entry, then deliberately retain the cached
         * target to prove live EC classification precedes the L1 fast path.
         * This does not replace the normal VM invalidation tested below. */
        for (unsigned int i = 0; i < 2; ++i)
        {
            initialize_state(&state, state_scratch, sizeof(state_scratch));
            state.rip = guest_transition_address;
            state.gpr[4] = guest_stack_top;
            require_result("warm transition target import",
                           switchyard_fex_thread_import_state(thread, &state),
                           SWITCHYARD_FEX_OK);
            initialize_stop(&stop);
            require_result("warm transition target execution",
                           execute_until_hlt(thread, guest_transition_address, &stop),
                           SWITCHYARD_FEX_OK);
            if (stop.reason != SWITCHYARD_FEX_STOP_HLT || stop.rip != guest_transition_address)
            {
                fprintf(stderr, "warm transition target did not execute x64 code\n");
                exit(1);
            }
        }
        __atomic_fetch_or(&ec_bitmap[transition_word], transition_mask,
                          __ATOMIC_RELEASE);
        require_result("cached EC target import",
                       switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("cached EC target execution",
                       execute_until_hlt(thread, guest_transition_address, &stop), SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_EC_TRANSITION || stop.rip != guest_transition_address)
        {
            fprintf(stderr, "cached x64 target bypassed live EC classification\n");
            exit(1);
        }
        require_result("re-marked transition target invalidation",
                       switchyard_fex_process_invalidate_code(
                           process, guest_transition_address, host_page_size),
                       SWITCHYARD_FEX_OK);
        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_transition_branch_address;
        state.gpr[4] = guest_stack_top;
        require_result("re-marked transition state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("re-marked transition execution",
                       execute_until_hlt(thread, guest_transition_address, &stop),
                       SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_EC_TRANSITION ||
            stop.rip != guest_transition_address)
        {
            fprintf(stderr, "re-marked EC target remained linked as x64 code\n");
            exit(1);
        }
        test_external_callret_balance(process, thread, guest_transition_address,
                                      (uintptr_t)high_stack_mapping + host_page_size);
        __atomic_fetch_and(&ec_bitmap[transition_word], ~transition_mask,
                           __ATOMIC_RELEASE);
    }

    boundary_mapping = mmap(
        (void *)(uintptr_t)(shadow_base + shadow_size - host_page_size),
        host_page_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (boundary_mapping == MAP_FAILED ||
        (uintptr_t)boundary_mapping !=
            shadow_base + shadow_size - host_page_size)
    {
        if (boundary_mapping != MAP_FAILED)
            munmap(boundary_mapping, host_page_size);
        fprintf(stderr, "failed to obtain low-shadow boundary page\n");
        exit(1);
    }
    memset(boundary_mapping, 0x5a, host_page_size);

    {
        struct switchyard_fex_arm64_host_context idle_host = {
            .size = sizeof(idle_host),
            .version = SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION,
            .pc = (uintptr_t)run_low_shadow_cycle,
        };
        struct switchyard_fex_x64_state idle_state = {
            .size = sizeof(idle_state), .version = SWITCHYARD_FEX_STATE_VERSION,
        };
        struct switchyard_fex_fault idle_fault = {
            .size = sizeof(idle_fault), .version = SWITCHYARD_FEX_FAULT_VERSION,
        };

        require_result("idle fault reconstruction rejection",
                       switchyard_fex_thread_reconstruct_jit_fault(
                           thread, &idle_host, SIGSEGV, 0, 0,
                           &idle_state, &idle_fault),
                       SWITCHYARD_FEX_ERROR_BUSY);
    }

    for (iteration = 0; iteration < 2; ++iteration)
    {
        *low_data = 0;
        high_data = 0;
        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_code_address;
        state.gpr[4] = guest_stack_top;
        require_result("low-shadow state import",
                       switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("low-shadow execution",
                       execute_until_hlt(
                           thread, guest_code_address + sizeof(code_template) - 1, &stop),
                       SWITCHYARD_FEX_OK);
        exported.size = sizeof(exported);
        exported.version = SWITCHYARD_FEX_STATE_VERSION;
        require_result("low-shadow state export",
                       switchyard_fex_thread_export_state(thread, &exported), SWITCHYARD_FEX_OK);
        expected = UINT64_C(0x1122334455667788) + (iteration ? 7 : 5);
        if (stop.reason != SWITCHYARD_FEX_STOP_HLT ||
            stop.rip != guest_code_address + sizeof(code_template) - 1 ||
            *low_data != expected || high_data != expected ||
            exported.gpr[2] != expected || exported.gpr[3] != expected ||
            exported.gpr[6] != expected || exported.gpr[4] != guest_stack_top)
        {
            fprintf(stderr, "low-shadow state or memory mismatch on iteration %u\n", iteration);
            exit(1);
        }

        if (iteration == 0)
        {
            if (mprotect(code, host_page_size, PROT_READ | PROT_WRITE))
            {
                fprintf(stderr, "failed to reopen low-shadow code page\n");
                exit(1);
            }
            code[26] = 7;
            if (mprotect(code, host_page_size, PROT_READ))
            {
                fprintf(stderr, "failed to republish low-shadow code page\n");
                exit(1);
            }
            require_result("low-shadow code invalidation",
                           switchyard_fex_process_invalidate_code(
                               process, guest_code_address, sizeof(code_template)),
                           SWITCHYARD_FEX_OK);
        }
    }

    {
        uint64_t *high_stack_slot =
            (uint64_t *)(high_stack_mapping + host_page_size) - 1;
        const uint64_t high_stack_top =
            (uintptr_t)(high_stack_mapping + host_page_size);

        high_stack_slot[-1] = 0;
        high_stack_slot[0] = 0;
        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_high_stack_code_address;
        state.gpr[4] = high_stack_top;
        state.gpr[3] = UINT64_C(0x8877665544332211);
        state.gpr[6] = UINT64_C(0x1122334455667788);
        require_result("identity high-stack state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("identity high-stack execution",
                       execute_until_hlt(
                           thread, guest_high_stack_code_address +
                                       sizeof(high_stack_template) - 1,
                           &stop),
                       SWITCHYARD_FEX_OK);
        initialize_state(&exported, state_scratch, sizeof(state_scratch));
        require_result("identity high-stack state export",
                       switchyard_fex_thread_export_state(thread, &exported),
                       SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_HLT ||
            stop.rip != guest_high_stack_code_address +
                            sizeof(high_stack_template) - 1 ||
            exported.gpr[4] != high_stack_top ||
            exported.gpr[2] != UINT64_C(0x8877665544332211) ||
            exported.gpr[1] != UINT64_C(0x1122334455667788) ||
            high_stack_slot[-1] != UINT64_C(0x8877665544332211) ||
            high_stack_slot[0] != UINT64_C(0x1122334455667788))
        {
            fprintf(stderr, "identity-mapped high stack contract mismatch\n");
            exit(1);
        }

        high_stack_slot[0] = 0;
        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_high_stack_call_code_address;
        state.gpr[4] = high_stack_top;
        require_result("identity high-stack indirect-call state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("identity high-stack indirect-call execution",
                       execute_until_hlt(
                           thread, guest_high_stack_call_code_address + 6,
                           &stop),
                       SWITCHYARD_FEX_OK);
        initialize_state(&exported, state_scratch, sizeof(state_scratch));
        require_result("identity high-stack indirect-call state export",
                       switchyard_fex_thread_export_state(thread, &exported),
                       SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_HLT ||
            stop.rip != guest_high_stack_call_code_address + 6 ||
            exported.gpr[4] != high_stack_top ||
            exported.gpr[2] != UINT64_C(0x1122334455667788) ||
            high_stack_slot[0] != guest_high_stack_call_code_address + 6)
        {
            fprintf(stderr, "identity-mapped high-stack indirect call mismatch\n");
            exit(1);
        }
    }

    memset(low_words, 0, 32 * sizeof(*low_words));
    initialize_state(&state, state_scratch, sizeof(state_scratch));
    state.rip = guest_memory_contract_address;
    state.gpr[4] = guest_stack_top;
    require_result("low-shadow memory-contract state import",
                   switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
    initialize_stop(&stop);
    require_result("low-shadow memory-contract execution",
                   execute_until_hlt(
                       thread, guest_memory_contract_address +
                                   sizeof(memory_contract_template) - 1,
                       &stop),
                   SWITCHYARD_FEX_OK);
    if (stop.reason != SWITCHYARD_FEX_STOP_HLT ||
        stop.rip != guest_memory_contract_address +
                        sizeof(memory_contract_template) - 1 ||
        low_words[0] != 30 || low_words[1] != UINT64_C(0x12345678) ||
        low_words[2] != 30 || low_words[3] != UINT64_C(0x12345678) ||
        low_words[4] != 30 || low_words[5] != UINT64_C(0x12345678) ||
        low_words[6] != 30 || low_words[7] != UINT64_C(0x12345678) ||
        low_words[12] != UINT64_C(0x1122334455667788) ||
        low_words[13] != UINT64_C(0x1122334455667788) ||
        low_words[14] != UINT64_C(0x1122334455667788) ||
        low_words[15] != UINT64_C(0x1122334455667788) ||
        low_words[20] != 30 || low_words[21] != UINT64_C(0x12345678) ||
        low_words[22] != 15 || low_words[23] != 10 ||
        low_words[24] != 3 || low_words[25] != 4)
    {
        fprintf(stderr, "low-shadow extended memory contract mismatch\n");
        exit(1);
    }

    {
        struct sigaction action;
        struct sigaction old_segv;
        struct sigaction old_bus;
        struct switchyard_fex_x64_state recovered = {
            .size = sizeof(recovered), .version = SWITCHYARD_FEX_STATE_VERSION,
        };
        struct switchyard_fex_fault fault = {
            .size = sizeof(fault), .version = SWITCHYARD_FEX_FAULT_VERSION,
        };

        memset(&action, 0, sizeof(action));
        sigemptyset(&action.sa_mask);
        action.sa_sigaction = capture_jit_fault;
        action.sa_flags = SA_SIGINFO;
        if (sigaction(SIGSEGV, &action, &old_segv) ||
            sigaction(SIGBUS, &action, &old_bus))
        {
            fprintf(stderr, "failed to install reconstruction signal handler\n");
            exit(1);
        }

        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_fault_code_address;
        state.gpr[4] = guest_stack_top;
        state.gpr[6] = 0;
        require_result("fault reconstruction state import",
                       switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
        captured_signal = 0;
        captured_fault_address = UINT64_MAX;
        signal_query_thread = thread;
        signal_query_stack = UINT64_MAX;
        signal_query_result = SWITCHYARD_FEX_ERROR_INTERNAL;
        if (sigsetjmp(fault_jump, 1) == 0)
        {
            fault_armed = 1;
            initialize_stop(&stop);
            (void)execute_until_hlt(
                thread, guest_fault_code_address + sizeof(fault_template) - 1, &stop);
            fprintf(stderr, "faulting JIT execution returned instead of transferring control\n");
            exit(1);
        }
        {
            struct switchyard_fex_arm64_host_context invalid_context =
                captured_host_context;
            const struct switchyard_fex_arm64_host_context original_context =
                captured_host_context;
            uint64_t stack = UINT64_MAX;

            signal_query_thread = NULL;
            require_result("signal-time guest stack query",
                           (enum switchyard_fex_result)signal_query_result,
                           SWITCHYARD_FEX_OK);
            if (signal_query_stack != guest_stack_top)
            {
                fprintf(stderr, "signal-time guest stack query mismatch\n");
                exit(1);
            }
            invalid_context.version++;
            require_result("stack query context ABI rejection",
                           switchyard_fex_thread_query_jit_stack(
                               thread, &invalid_context, 0, captured_fault_address, &stack),
                           SWITCHYARD_FEX_ERROR_ABI_MISMATCH);
            require_result("stack query access rejection",
                           switchyard_fex_thread_query_jit_stack(
                               thread, &captured_host_context, 2, captured_fault_address, &stack),
                           SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
            invalid_context = captured_host_context;
            invalid_context.pc = (uintptr_t)run_low_shadow_cycle;
            require_result("stack query unrelated host rejection",
                           switchyard_fex_thread_query_jit_stack(
                               thread, &invalid_context, 0, captured_fault_address, &stack),
                           SWITCHYARD_FEX_ERROR_UNSUPPORTED);
            require_result("native frame cannot masquerade as a decoder execute fault",
                           switchyard_fex_thread_reconstruct_jit_fault(
                               thread, &invalid_context, (uint32_t)captured_signal,
                               8, shadow_base + state.rip, &recovered, &fault),
                           SWITCHYARD_FEX_ERROR_UNSUPPORTED);
            if (stack != UINT64_MAX)
            {
                fprintf(stderr, "rejected stack query changed its output\n");
                exit(1);
            }
            for (unsigned int repeat = 0; repeat < 2; ++repeat)
            {
                require_result("non-consuming active stack query",
                               switchyard_fex_thread_query_jit_stack(
                                   thread, &captured_host_context, 0, captured_fault_address, &stack),
                               SWITCHYARD_FEX_OK);
                if (stack != guest_stack_top ||
                    memcmp(&captured_host_context, &original_context, sizeof(original_context)))
                {
                    fprintf(stderr, "stack query changed the host context or stack\n");
                    exit(1);
                }
            }

            invalid_context = captured_host_context;
            invalid_context.version++;
            require_result("active fault context ABI rejection",
                           switchyard_fex_thread_reconstruct_jit_fault(
                               thread, &invalid_context, (uint32_t)captured_signal,
                               0, captured_fault_address, &recovered, &fault),
                           SWITCHYARD_FEX_ERROR_ABI_MISMATCH);
            require_result("active fault access rejection",
                           switchyard_fex_thread_reconstruct_jit_fault(
                               thread, &captured_host_context,
                               (uint32_t)captured_signal, 2,
                               captured_fault_address, &recovered, &fault),
                           SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
            invalid_context = captured_host_context;
            invalid_context.pc = (uintptr_t)run_low_shadow_cycle;
            require_result("unrelated active TSO repair rejection",
                           switchyard_fex_thread_repair_unaligned_tso(
                               thread, &invalid_context),
                           SWITCHYARD_FEX_ERROR_UNSUPPORTED);
            require_result("unrelated active host fault rejection",
                           switchyard_fex_thread_reconstruct_jit_fault(
                               thread, &invalid_context, (uint32_t)captured_signal,
                               0, captured_fault_address, &recovered, &fault),
                           SWITCHYARD_FEX_ERROR_UNSUPPORTED);
        }
        require_result("JIT fault reconstruction",
                       switchyard_fex_thread_reconstruct_jit_fault(
                           thread, &captured_host_context, (uint32_t)captured_signal,
                           0, captured_fault_address, &recovered, &fault),
                       SWITCHYARD_FEX_OK);
        {
            uint64_t stack = UINT64_MAX;
            require_result("consumed execution stack query rejection",
                           switchyard_fex_thread_query_jit_stack(
                               thread, &captured_host_context, 0, captured_fault_address, &stack),
                           SWITCHYARD_FEX_ERROR_BUSY);
            if (stack != UINT64_MAX)
            {
                fprintf(stderr, "idle stack query changed its output\n");
                exit(1);
            }
        }
        if ((captured_signal != SIGSEGV && captured_signal != SIGBUS) ||
            recovered.rip != guest_fault_code_address || recovered.gpr[6] != 0 ||
            memcmp(recovered.gpr, state.gpr, sizeof(state.gpr)) ||
            recovered.rflags != state.rflags || recovered.mxcsr != state.mxcsr ||
            recovered.fcw != state.fcw ||
            recovered.abridged_ftw != state.abridged_ftw ||
            memcmp(recovered.segment, state.segment, sizeof(state.segment)) ||
            memcmp(recovered.segment_base, state.segment_base,
                   sizeof(state.segment_base)) ||
            memcmp(recovered.xmm, state.xmm, sizeof(state.xmm)) ||
            memcmp(recovered.ymm_high, state.ymm_high,
                   sizeof(state.ymm_high)) ||
            memcmp(recovered.x87, state.x87, sizeof(state.x87)) ||
            fault.signal != (uint32_t)captured_signal || fault.access != 0 ||
            fault.host_pc != captured_host_context.pc ||
            fault.guest_rip != guest_fault_code_address ||
            fault.host_address != 0 || fault.guest_address != 0)
        {
            fprintf(stderr, "JIT fault reconstruction mismatch\n");
            exit(1);
        }

        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_fault_code_address;
        state.gpr[4] = guest_stack_top;
        state.gpr[6] = guest_guard_address;
        require_result("shadow fault reconstruction state import",
                       switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
        captured_signal = 0;
        captured_fault_address = UINT64_MAX;
        if (sigsetjmp(fault_jump, 1) == 0)
        {
            fault_armed = 1;
            initialize_stop(&stop);
            (void)execute_until_hlt(
                thread, guest_fault_code_address + sizeof(fault_template) - 1, &stop);
            fprintf(stderr, "shadow faulting JIT execution returned unexpectedly\n");
            exit(1);
        }
        require_result("shadow JIT fault reconstruction",
                       switchyard_fex_thread_reconstruct_jit_fault(
                           thread, &captured_host_context, (uint32_t)captured_signal,
                           0, captured_fault_address, &recovered, &fault),
                       SWITCHYARD_FEX_OK);
        if ((captured_signal != SIGSEGV && captured_signal != SIGBUS) ||
            recovered.rip != guest_fault_code_address ||
            recovered.gpr[6] != guest_guard_address ||
            fault.host_address != shadow_base + guest_guard_address ||
            fault.guest_address != guest_guard_address)
        {
            fprintf(stderr, "shadow JIT fault reconstruction mismatch\n");
            exit(1);
        }

        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_write_fault_code_address;
        state.gpr[0] = UINT64_C(0xa5a55a5adeadbeef);
        state.gpr[4] = guest_stack_top;
        state.gpr[6] = guest_guard_address;
        require_result("write fault reconstruction state import",
                       switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
        captured_signal = 0;
        captured_fault_address = UINT64_MAX;
        if (sigsetjmp(fault_jump, 1) == 0)
        {
            fault_armed = 1;
            initialize_stop(&stop);
            (void)execute_until_hlt(
                thread,
                guest_write_fault_code_address + sizeof(write_fault_template) - 1,
                &stop);
            fprintf(stderr, "write-faulting JIT execution returned unexpectedly\n");
            exit(1);
        }
        require_result("write JIT fault reconstruction",
                       switchyard_fex_thread_reconstruct_jit_fault(
                           thread, &captured_host_context, (uint32_t)captured_signal,
                           1, captured_fault_address, &recovered, &fault),
                       SWITCHYARD_FEX_OK);
        if ((captured_signal != SIGSEGV && captured_signal != SIGBUS) ||
            recovered.rip != guest_write_fault_code_address ||
            recovered.gpr[0] != UINT64_C(0xa5a55a5adeadbeef) ||
            fault.access != 1 ||
            fault.guest_rip != guest_write_fault_code_address ||
            fault.host_address != shadow_base + guest_guard_address ||
            fault.guest_address != guest_guard_address)
        {
            fprintf(stderr, "write JIT fault reconstruction mismatch\n");
            exit(1);
        }

        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_boundary_fault_code_address;
        state.gpr[4] = guest_stack_top;
        state.gpr[6] = shadow_size - 4;
        require_result("boundary fault reconstruction state import",
                       switchyard_fex_thread_import_state(thread, &state),
                       SWITCHYARD_FEX_OK);
        captured_signal = 0;
        captured_fault_address = UINT64_MAX;
        if (sigsetjmp(fault_jump, 1) == 0)
        {
            fault_armed = 1;
            initialize_stop(&stop);
            (void)execute_until_hlt(
                thread,
                guest_boundary_fault_code_address + sizeof(fault_template) - 1,
                &stop);
            fprintf(stderr, "4GiB-crossing JIT read returned unexpectedly\n");
            exit(1);
        }
        require_result("boundary JIT fault reconstruction",
                       switchyard_fex_thread_reconstruct_jit_fault(
                           thread, &captured_host_context,
                           (uint32_t)captured_signal, 0,
                           captured_fault_address, &recovered, &fault),
                       SWITCHYARD_FEX_OK);
        if ((captured_signal != SIGSEGV && captured_signal != SIGBUS) ||
            recovered.rip != guest_boundary_fault_code_address ||
            recovered.gpr[6] != shadow_size - 4 ||
            fault.guest_rip != guest_boundary_fault_code_address ||
            fault.host_address != shadow_base + shadow_size - 4 ||
            fault.guest_address != shadow_size - 4)
        {
            fprintf(stderr,
                    "4GiB boundary reconstruction mismatch host=%#" PRIx64
                    " guest=%#" PRIx64 "\n",
                    fault.host_address, fault.guest_address);
            exit(1);
        }

        if (sigaction(SIGSEGV, &old_segv, NULL) ||
            sigaction(SIGBUS, &old_bus, NULL))
        {
            fprintf(stderr, "failed to restore reconstruction signal handler\n");
            exit(1);
        }

        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_fault_code_address;
        state.gpr[4] = guest_stack_top;
        state.gpr[6] = guest_data_address;
        require_result("post-fault state import",
                       switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("post-fault thread reuse",
                       execute_until_hlt(
                           thread, guest_fault_code_address + sizeof(fault_template) - 1,
                           &stop),
                       SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_HLT)
        {
            fprintf(stderr, "post-fault thread reuse returned the wrong stop\n");
            exit(1);
        }
    }

    for (iteration = 0; iteration < 9; ++iteration)
    {
        executable_policy.denied_base = guest_fault_code_address;
        executable_policy.denied_size = iteration ? 0 : sizeof(fault_template);
        executable_policy.malformed = iteration;
        require_result("executable policy invalidation",
                       switchyard_fex_process_invalidate_code(
                           process, guest_fault_code_address, sizeof(fault_template)), SWITCHYARD_FEX_OK);
        initialize_state(&state, state_scratch, sizeof(state_scratch));
        state.rip = guest_fault_code_address;
        state.gpr[4] = guest_stack_top;
        require_result("denied executable state import",
                       switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
        initialize_stop(&stop);
        require_result("denied or malformed executable range",
                       execute_until_hlt(thread, 0, &stop), SWITCHYARD_FEX_ERROR_GUEST_FAULT);
        initialize_state(&exported, state_scratch, sizeof(state_scratch));
        require_result("denied executable state export",
                       switchyard_fex_thread_export_state(thread, &exported), SWITCHYARD_FEX_OK);
        if (stop.reason != SWITCHYARD_FEX_STOP_GUEST_FAULT || stop.signal != 11 ||
            stop.trap_number != 14 || stop.rip != state.rip ||
            exported.rip != state.rip || exported.gpr[4] != state.gpr[4])
        {
            fprintf(stderr, "denied executable range lost guest fault state\n");
            exit(1);
        }
    }
    memset(&executable_policy, 0, sizeof(executable_policy));
    require_result("executable permission restoration invalidation",
                   switchyard_fex_process_invalidate_code(
                       process, guest_fault_code_address, sizeof(fault_template)), SWITCHYARD_FEX_OK);
    state.gpr[6] = guest_data_address;
    require_result("restored executable state import",
                   switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
    initialize_stop(&stop);
    require_result("restored executable range execution",
                   execute_until_hlt(thread, guest_fault_code_address + sizeof(fault_template) - 1, &stop),
                   SWITCHYARD_FEX_OK);

    require_result("low-shadow thread destroy",
                   switchyard_fex_thread_destroy(thread), SWITCHYARD_FEX_OK);
    require_result("low-shadow process destroy",
                   switchyard_fex_process_destroy(process), SWITCHYARD_FEX_OK);
    if (munmap(boundary_mapping, host_page_size))
    {
        fprintf(stderr, "failed to release low-shadow boundary page\n");
        exit(1);
    }
    if (munmap(high_stack_mapping, host_page_size))
    {
        fprintf(stderr, "failed to release identity-mapped high stack page\n");
        exit(1);
    }
    if (munmap(high_unaligned_mapping, host_page_size * 2))
    {
        fprintf(stderr, "failed to release identity unaligned-store pages\n");
        exit(1);
    }
    if (munmap(mapping, mapping_size))
    {
        fprintf(stderr, "failed to release low-shadow test mapping\n");
        exit(1);
    }
}

static void run_boundary_guard_collision_rejection(void)
{
    const uint64_t shadow_base = UINT64_C(0x10000000000);
    const uint64_t shadow_size = UINT64_C(0x100000000);
    const long page_size_long = sysconf(_SC_PAGESIZE);
    struct switchyard_fex_config config = {
        .size = sizeof(config),
        .abi_version = SWITCHYARD_FEX_ABI_VERSION,
        .low_va_shadow_base = shadow_base,
        .low_va_shadow_size = shadow_size,
    };
    struct switchyard_fex_process *process = NULL;
    unsigned char *occupied;

    if (page_size_long <= 0)
    {
        fprintf(stderr, "invalid page size for boundary guard collision test\n");
        exit(1);
    }
    occupied = mmap((void *)(uintptr_t)(shadow_base + shadow_size),
                    (size_t)page_size_long, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
    if (occupied == MAP_FAILED ||
        (uintptr_t)occupied != shadow_base + shadow_size)
    {
        if (occupied != MAP_FAILED) munmap(occupied, (size_t)page_size_long);
        fprintf(stderr, "failed to obtain boundary guard collision mapping\n");
        exit(1);
    }
    occupied[0] = 0xa5;
    require_result("occupied boundary guard rejection",
                   switchyard_fex_process_create(&config, &process),
                   SWITCHYARD_FEX_ERROR_INITIALIZATION);
    if (process || occupied[0] != 0xa5)
    {
        fprintf(stderr, "boundary guard initialization overwrote its collision\n");
        exit(1);
    }
    if (munmap(occupied, (size_t)page_size_long))
    {
        fprintf(stderr, "failed to release boundary guard collision mapping\n");
        exit(1);
    }
}

static void run_external_boundary_guard_acceptance(void)
{
    const uint64_t shadow_base = UINT64_C(0x10000000000);
    const uint64_t shadow_size = UINT64_C(0x100000000);
    const long page_size_long = sysconf(_SC_PAGESIZE);
    struct switchyard_fex_config config = {
        .size = sizeof(config),
        .abi_version = SWITCHYARD_FEX_ABI_VERSION,
        .low_va_shadow_base = shadow_base,
        .low_va_shadow_size = shadow_size,
    };
    struct switchyard_fex_process *process = NULL;
    unsigned char *guard;

    if (page_size_long <= 0)
    {
        fprintf(stderr, "invalid page size for external boundary guard test\n");
        exit(1);
    }
    guard = mmap((void *)(uintptr_t)(shadow_base + shadow_size),
                 (size_t)page_size_long, PROT_NONE,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
    if (guard == MAP_FAILED ||
        (uintptr_t)guard != shadow_base + shadow_size)
    {
        if (guard != MAP_FAILED) munmap(guard, (size_t)page_size_long);
        fprintf(stderr, "failed to obtain external boundary guard mapping\n");
        exit(1);
    }

    require_result("external boundary guard acceptance",
                   switchyard_fex_process_create(&config, &process),
                   SWITCHYARD_FEX_OK);
    require_result("external boundary guard process destroy",
                   switchyard_fex_process_destroy(process), SWITCHYARD_FEX_OK);
    if (mprotect(guard, (size_t)page_size_long, PROT_READ | PROT_WRITE))
    {
        fprintf(stderr, "FEX released the embedding-owned boundary guard\n");
        exit(1);
    }
    guard[0] = 0x5a;
    if (munmap(guard, (size_t)page_size_long))
    {
        fprintf(stderr, "failed to release external boundary guard\n");
        exit(1);
    }
}

static void unexpected_domain_wake(void *context)
{
    (void)context;
    abort();
}

static void test_missing_executable_policy(void)
{
    struct switchyard_fex_config config = {
        .size = sizeof(config), .abi_version = SWITCHYARD_FEX_ABI_VERSION,
    };
    struct switchyard_fex_process *process = NULL;
    struct switchyard_fex_thread *thread = NULL;
    struct switchyard_fex_x64_state state;
    struct switchyard_fex_stop stop;
    _Alignas(16) unsigned char stack[256] = {0};

    require_result("missing-policy process create",
                   switchyard_fex_process_create(&config, &process), SWITCHYARD_FEX_OK);
    {
        struct switchyard_fex_admission *cell = NULL;
        const struct switchyard_fex_execution_domain domain = {
            .size = sizeof(domain), .version = SWITCHYARD_FEX_DOMAIN_VERSION,
            .wake = unexpected_domain_wake,
        };
        require_result("internal process rejects external membership",
                       switchyard_fex_thread_create_with_domain(process, &domain, &thread, &cell),
                       SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
        if (thread || cell) abort();
    }
    require_result("missing-policy thread create",
                   switchyard_fex_thread_create(process, &thread), SWITCHYARD_FEX_OK);
    require_result("late executable policy registration rejection",
                   switchyard_fex_process_set_executable_range_query(process, test_query_executable_range, NULL),
                   SWITCHYARD_FEX_ERROR_BUSY);
    initialize_state(&state, stack, sizeof(stack));
    require_result("missing-policy state import",
                   switchyard_fex_thread_import_state(thread, &state), SWITCHYARD_FEX_OK);
    initialize_stop(&stop);
    require_result("missing executable policy must deny decode",
                   execute_until_hlt(thread, 0, &stop), SWITCHYARD_FEX_ERROR_GUEST_FAULT);
    if (stop.signal != 11 || stop.trap_number != 14 || stop.rip != state.rip)
    {
        fprintf(stderr, "missing executable policy returned an incorrect guest fault\n");
        exit(1);
    }
    require_result("missing-policy thread destroy",
                   switchyard_fex_thread_destroy(thread), SWITCHYARD_FEX_OK);
    require_result("missing-policy process destroy",
                   switchyard_fex_process_destroy(process), SWITCHYARD_FEX_OK);
}

int main(void)
{
    int admission_line = test_admission_contract();
    if (admission_line)
    {
        fprintf(stderr, "C admission contract failed at line %d\n", admission_line);
        return 1;
    }
    _Alignas(8) uint64_t validation_bitmap[2] = {0};
    struct switchyard_fex_config bad_config = {
        .size = sizeof(bad_config),
        .abi_version = SWITCHYARD_FEX_ABI_VERSION + 1,
    };
    struct switchyard_fex_config invalid_shadow_config = {
        .size = sizeof(invalid_shadow_config),
        .abi_version = SWITCHYARD_FEX_ABI_VERSION,
        .low_va_shadow_base = UINT64_C(0x10000000000),
    };
    struct switchyard_fex_config incomplete_bitmap_config = {
        .size = sizeof(incomplete_bitmap_config),
        .abi_version = SWITCHYARD_FEX_ABI_VERSION,
        .highest_user_address = 0x1000,
        .ec_page_shift = 12,
    };
    struct switchyard_fex_config unaligned_bitmap_config = {
        .size = sizeof(unaligned_bitmap_config),
        .abi_version = SWITCHYARD_FEX_ABI_VERSION,
        .ec_code_bitmap = (uintptr_t)validation_bitmap + 1,
        .highest_user_address = 0x1000,
        .ec_page_shift = 12,
    };
    struct switchyard_fex_config invalid_bitmap_shift_config = {
        .size = sizeof(invalid_bitmap_shift_config),
        .abi_version = SWITCHYARD_FEX_ABI_VERSION,
        .ec_code_bitmap = (uintptr_t)validation_bitmap,
        .highest_user_address = 0x1000,
        .ec_page_shift = 11,
    };
    struct switchyard_fex_config overflowing_bitmap_config = {
        .size = sizeof(overflowing_bitmap_config),
        .abi_version = SWITCHYARD_FEX_ABI_VERSION,
        .ec_code_bitmap = UINT64_MAX - 7,
        .highest_user_address = UINT64_MAX,
        .ec_page_shift = 12,
    };
    struct switchyard_fex_process *process = NULL;

    if (switchyard_fex_abi_version() != SWITCHYARD_FEX_ABI_VERSION ||
        strcmp(switchyard_fex_provider_abi_identity(), SWITCHYARD_FEX_PROVIDER_ABI_IDENTITY) ||
        strcmp(switchyard_fex_upstream_revision(),
               "73dc3b3eddaf72745c743fe8ec76ed66f87636dc"))
    {
        fprintf(stderr, "provider identity mismatch\n");
        return 1;
    }
    require_result("ABI rejection", switchyard_fex_process_create(&bad_config, &process),
                   SWITCHYARD_FEX_ERROR_ABI_MISMATCH);
    require_result("invalid low shadow rejection",
                   switchyard_fex_process_create(&invalid_shadow_config, &process),
                   SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    for (unsigned shift = 0; shift < 64; ++shift)
    {
        struct switchyard_fex_config unsupported_shadow_config = {
            .size = sizeof(unsupported_shadow_config),
            .abi_version = SWITCHYARD_FEX_ABI_VERSION,
            .low_va_shadow_base = UINT64_C(1) << 40,
            .low_va_shadow_size = UINT64_C(1) << shift,
        };
        if (shift == 32) continue;
        process = (struct switchyard_fex_process *)(uintptr_t)1;
        require_result("unsupported low shadow size rejection",
                       switchyard_fex_process_create(&unsupported_shadow_config, &process),
                       SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
        if (process) { fprintf(stderr, "invalid geometry published a process\n"); return 1; }
    }
    require_result("incomplete EC bitmap rejection",
                   switchyard_fex_process_create(&incomplete_bitmap_config, &process),
                   SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    require_result("unaligned EC bitmap rejection",
                   switchyard_fex_process_create(&unaligned_bitmap_config, &process),
                   SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    require_result("invalid EC bitmap shift rejection",
                   switchyard_fex_process_create(&invalid_bitmap_shift_config, &process),
                   SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    require_result("overflowing EC bitmap rejection",
                   switchyard_fex_process_create(&overflowing_bitmap_config, &process),
                   SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);

    test_missing_executable_policy();
    run_process_cycle(1, 0);
    run_process_cycle(0, SWITCHYARD_FEX_CONFIG_MULTIBLOCK);
    run_boundary_guard_collision_rejection();
    run_external_boundary_guard_acceptance();
    run_low_shadow_cycle();
    printf("switchyard_fex_c_api_test result=pass concurrency=%u/%u revision=%s\n",
           WORKER_COUNT, WORKER_COUNT, switchyard_fex_upstream_revision());
    return 0;
}
