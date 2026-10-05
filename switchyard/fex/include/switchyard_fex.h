/* SPDX-License-Identifier: MIT */
#ifndef SWITCHYARD_FEX_H
#define SWITCHYARD_FEX_H

#include <stddef.h>
#include <stdint.h>
#include "switchyard_fex_admission.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
# define SWITCHYARD_FEX_API __attribute__((visibility("default")))
#else
# define SWITCHYARD_FEX_API
#endif

#define SWITCHYARD_FEX_ABI_VERSION 6u
#define SWITCHYARD_FEX_DOMAIN_VERSION 1u
#define SWITCHYARD_FEX_DOMAIN_SIZE_V1 32u
#define SWITCHYARD_FEX_DISPATCH_VERSION 1u
#define SWITCHYARD_FEX_DISPATCH_SIZE_V1 40u
#define SWITCHYARD_FEX_STATE_VERSION 1u
#define SWITCHYARD_FEX_EXECUTION_VERSION 1u
#define SWITCHYARD_FEX_STOP_VERSION 1u
#define SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION 1u
#define SWITCHYARD_FEX_FAULT_VERSION 1u
#define SWITCHYARD_FEX_EXECUTABLE_RANGE_VERSION 1u
#define SWITCHYARD_FEX_CONFIG_SIZE_V1 56u
#define SWITCHYARD_FEX_STATE_SIZE_V1 872u
#define SWITCHYARD_FEX_REGISTER_WINDOW_VERSION 1u
#define SWITCHYARD_FEX_REGISTER_WINDOW_SIZE_V1 40u
#define SWITCHYARD_FEX_REGISTER_WINDOW_DATA_SIZE_V1 408u
#define SWITCHYARD_FEX_EXECUTION_SIZE_V1 32u
#define SWITCHYARD_FEX_STOP_SIZE_V1 32u
#define SWITCHYARD_FEX_ARM64_HOST_CONTEXT_SIZE_V1 800u
#define SWITCHYARD_FEX_FAULT_SIZE_V1 48u
#define SWITCHYARD_FEX_EXECUTABLE_RANGE_SIZE_V1 32u
#define SWITCHYARD_FEX_PROVIDER_ABI_IDENTITY \
    "switchyard-fex-provider-abi-v6-darwin-low-shadow-external-stops-signal-repair-stack-query-exec-range-detached-shared-admission-register-window"

struct switchyard_fex_process;
struct switchyard_fex_thread;
struct switchyard_fex_stop;

/* Call these Darwin C APIs in the system x18 ABI, including callbacks into the
 * adapter. Guest/custom x18 ownership must end before calling host libraries.
 * Opaque objects are caller-owned.  A destroy call must not run concurrently
 * with any API call that uses the same process or thread pointer.  Thread
 * execution, state transfer, and cache operations otherwise report BUSY when
 * their supported synchronization contract would be violated. */

enum switchyard_fex_result
{
    SWITCHYARD_FEX_OK = 0,
    SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT = 1,
    SWITCHYARD_FEX_ERROR_ABI_MISMATCH = 2,
    SWITCHYARD_FEX_ERROR_UNSUPPORTED = 3,
    SWITCHYARD_FEX_ERROR_NO_MEMORY = 4,
    SWITCHYARD_FEX_ERROR_BUSY = 5,
    SWITCHYARD_FEX_ERROR_INITIALIZATION = 6,
    SWITCHYARD_FEX_ERROR_GUEST_FAULT = 7,
    SWITCHYARD_FEX_ERROR_INTERNAL = 8,
};

enum switchyard_fex_config_flags
{
    SWITCHYARD_FEX_CONFIG_MULTIBLOCK = 1u << 0,
    /* Requires Darwin's custom-x18 API and an embedding-owned signal path. */
    SWITCHYARD_FEX_CONFIG_CUSTOM_DISPATCH = 1u << 1,
    /* Embedding owns mutation closure for every thread; no mixed ownership. */
    SWITCHYARD_FEX_CONFIG_EXTERNAL_ADMISSION = 1u << 2,
};

/* A wake runs in the system ABI after a CLOSED execution is consumed, on the
 * executing host thread. It may lock the embedding's mutation mutex; callers
 * must not hold that mutex while completing execution or reconstructing a fault.
 * It must not throw or reenter FEX. Keep wake/context alive through destruction.
 * Short state operations never invoke wake and may run under that mutex.
 *
 * The returned admission cell is adapter-owned and borrowed until thread
 * destruction. Only the embedding may close/reopen an external domain, under
 * its membership/mutation mutex. Close all cells before waiting for ACTIVE to
 * clear; mutate only while all are CLOSED+idle. Reopen after publication. Close
 * an idle cell BEFORE removing its owner from the embedding's list and keep the
 * owner alive through thread destruction/the last bridge access. FEX refuses
 * to destroy an externally managed thread whose cell is still open.
 * Never modify generation/ACTIVE/EXECUTING directly. They are FEX-owned. */
struct switchyard_fex_execution_domain
{
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    void (*wake)(void *context);
    void *context;
};

SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_create_with_domain(
    struct switchyard_fex_process *process, const struct switchyard_fex_execution_domain *domain,
    struct switchyard_fex_thread **thread, struct switchyard_fex_admission **admission);

#define SWITCHYARD_FEX_EXECUTABLE_RANGE_WRITABLE 1u
struct switchyard_fex_executable_range
{
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    uint64_t base;
    uint64_t length;
};

/* Compile-time callback, not a signal handler. Return a readable, executable
 * guest range containing address; failure or an invalid range denies access.
 * The embedding must pin mappings throughout execution and invalidate code
 * before publishing any permission or mapping change. Do not reenter FEX. */
typedef enum switchyard_fex_result (*switchyard_fex_executable_range_query)(
    void *context, uint64_t address, struct switchyard_fex_executable_range *range);

/* Register once, before creating threads. The callback and borrowed context
 * remain valid until process destruction. Without a callback, decode denies
 * all guest memory; no implicit executable address space is assumed. */
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_process_set_executable_range_query(
    struct switchyard_fex_process *process,
    switchyard_fex_executable_range_query query, void *context);

enum switchyard_fex_state_flags
{
    SWITCHYARD_FEX_STATE_YMM_HIGH_VALID = 1u << 0,
};

enum switchyard_fex_stop_reason
{
    SWITCHYARD_FEX_STOP_NONE = 0,
    SWITCHYARD_FEX_STOP_HLT = 1,
    SWITCHYARD_FEX_STOP_GUEST_FAULT = 2,
    SWITCHYARD_FEX_STOP_EC_TRANSITION = 3,
    SWITCHYARD_FEX_STOP_SYSCALL = 4,
    SWITCHYARD_FEX_STOP_SUSPEND = 5,
    SWITCHYARD_FEX_STOP_INVALID_INSTRUCTION = 6,
    SWITCHYARD_FEX_STOP_SINGLE_STEP = 7,
    SWITCHYARD_FEX_STOP_INTERNAL = 8,
};

struct switchyard_fex_u128
{
    uint64_t low;
    uint64_t high;
};

/* A borrowed, normalized little-endian x64 register window, not a CPUState
 * pointer or a complete x64 CONTEXT. data is a host pointer encoded as uint64_t;
 * data_size must be exactly 408. Its alignment may be one byte. The descriptor
 * is naturally aligned and at least size bytes long; flags/reserved must be 0.
 * Callers pin the readable/writable buffer and keep it exclusively owned until
 * return. Neither pointer is retained. Do not alias the descriptor, opaque
 * adapter objects or their private storage. No arbitrary pointer accessibility
 * is promised by size/geometry validation.
 *
 * Import models a native x64 entry: installs GPR/RIP/EFLAGS/MXCSR/XMM, resets
 * x87 to FCW 0x037f/empty, clears segment selectors/bases except CS 0x30 and
 * GS=gs_base, and PRESERVES absent YMM high halves. EFLAGS must fit uint32_t;
 * MXCSR has no reserved bits. The input's reserved word is ignored.
 * This is NEVER a full-state syscall/fault continuation. Use the complete-state
 * API for those. An internal import failure invalidates execution state.
 *
 * Export requires gs_base=0, initializes all 408 bytes (reserved=0), and leaves
 * the buffer unchanged on failure. The same StateAdmission excludes execution,
 * mutation and competing state calls; external caller buffer ownership is not
 * provided by that admission. Both functions use the ordinary system x18 ABI. */
struct switchyard_fex_register_window
{
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    uint64_t data;
    uint64_t data_size;
    uint64_t gs_base;
};

enum switchyard_fex_register_window_offset
{
    SWITCHYARD_FEX_WINDOW_RAX = 0,   SWITCHYARD_FEX_WINDOW_RBX = 8,
    SWITCHYARD_FEX_WINDOW_RCX = 16,  SWITCHYARD_FEX_WINDOW_RDX = 24,
    SWITCHYARD_FEX_WINDOW_RSI = 32,  SWITCHYARD_FEX_WINDOW_RDI = 40,
    SWITCHYARD_FEX_WINDOW_RBP = 48,  SWITCHYARD_FEX_WINDOW_RSP = 56,
    SWITCHYARD_FEX_WINDOW_R8 = 64,   SWITCHYARD_FEX_WINDOW_R9 = 72,
    SWITCHYARD_FEX_WINDOW_R10 = 80,  SWITCHYARD_FEX_WINDOW_R11 = 88,
    SWITCHYARD_FEX_WINDOW_R12 = 96,  SWITCHYARD_FEX_WINDOW_R13 = 104,
    SWITCHYARD_FEX_WINDOW_R14 = 112, SWITCHYARD_FEX_WINDOW_R15 = 120,
    SWITCHYARD_FEX_WINDOW_RIP = 128, SWITCHYARD_FEX_WINDOW_EFLAGS = 136,
    SWITCHYARD_FEX_WINDOW_MXCSR = 144, SWITCHYARD_FEX_WINDOW_RESERVED = 148,
    SWITCHYARD_FEX_WINDOW_XMM = 152,
};

/* ec_code_bitmap is an optional one-bit-per-page transition map, indexed by
 * guest address >> ec_page_shift through highest_user_address (inclusive).
 * It must remain valid until process destruction.  Publish mutations before
 * executing and invalidate the affected target range so previously linked x64
 * blocks cannot bypass a newly marked transition page. */
struct switchyard_fex_config
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t flags;
    uint32_t reserved;
    /* Either both zero (identity), or a power-of-two base and exactly4 GiB.
     * Other sizes are unsupported by the generated low-address transform and
     * are rejected before allocating or publishing a process. */
    uint64_t low_va_shadow_base;
    uint64_t low_va_shadow_size;
    uint64_t ec_code_bitmap;
    uint64_t highest_user_address;
    uint32_t ec_page_shift;
    uint32_t reserved2;
};

/* Per-execution embedding state.  suspend_doorbell points to a naturally
 * aligned uint32_t that remains valid until execution returns or a generated
 * code fault is reconstructed.  FEX reads it with acquire ordering at block
 * entry and backward edges.  expected_hlt_rip may be zero when HLT is not an
 * accepted terminal condition. */
struct switchyard_fex_execution
{
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    uint64_t expected_hlt_rip;
    uint64_t suspend_doorbell;
};

/* Reserve under the embedding's mutation mutex, then unlock before executing.
 * The same host thread must consume this generation exactly once, or reconstruct
 * a real JIT fault. No guest call/mutation is allowed while reserved. These do
 * not expose a native frame or entry; custom-caller embeddings use dispatch. */
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_prepare_execution(
    struct switchyard_fex_thread *thread, const struct switchyard_fex_execution *execution,
    uint64_t *generation);
/* PRIVATE experiment, not the ABI6 release contract. Native entry ONLY; never
 * call for an internal syscall/fault continuation. Same mutex/unlock/owner and
 * single-consumption contract as prepare_execution; generation unchanged on
 * failure, although an acquired generation may be consumed. */
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_experiment_prepare_native_window(
    struct switchyard_fex_thread *thread, const struct switchyard_fex_register_window *window,
    const struct switchyard_fex_execution *execution, uint64_t *generation);
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_execute_prepared(
    struct switchyard_fex_thread *thread, uint64_t generation, struct switchyard_fex_stop *stop);
/* PRIVATE system-ABI experiment. After an ordinary generated invocation,
 * classify and export the non-syscall window before consuming the SAME owned
 * execution. Wake/release still precedes any embedding mutex/native callback.
 * Invalid arguments or stale/wrong-owner generations do not execute, consume
 * or publish. Syscalls leave the window unchanged: resume uses full state.
 * The caller pins non-aliasing output/stop storage until this call returns. */
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_experiment_execute_export_window(
    struct switchyard_fex_thread *thread, uint64_t generation,
    struct switchyard_fex_stop *stop, const struct switchyard_fex_register_window *window);

/* PRIVATE native-gate experiment, NOT the ABI6 release contract. The trusted
 * Unix embedding binds once under its mutation mutex, after validating the
 * identity doorbell mapping and authenticating gs_base from Unix NtCurrentTeb.
 * No descriptor, opaque object, TLS-slot or admission pointer is exposed to PE.
 * The creator OS thread is the only permitted binding/execution owner; matching
 * numeric TEB/gs_base values do not authenticate another pthread.
 *
 * mapping_epoch points to an aligned, exclusively atomically accessed uint64_t
 * in embedding-owned storage pinned through successful adapter destruction.
 * Increment it without wrapping BEFORE closing all admission cells, drain every
 * ACTIVE cell, mutate/invalidate, then reopen. It is a cache-validation stamp,
 * never a second admission authority. The gate checks it AFTER acquiring the
 * existing sole execution cell, before reading a doorbell or importing state.
 * A mismatch reports UNSUPPORTED, consumes that reservation and wakes if CLOSED;
 * the embedding must revalidate/rebind through its cold path, not retry blindly.
 * All borrowed gate storage remains owned even if destruction fails.
 *
 * execute_native_gate is an ordinary system-ABI call, with no embedding mutex
 * held. Import/execute/stopped export share one generation, released/woken
 * BEFORE any native callback or return. No C++ activation, borrowed window or
 * raw TB continuation survives. Native entry defaults are NOT valid for full
 * syscall/fault/internal-suspend replay: those stops use existing full state.
 * Window.gs_base must match the Unix-bound value. Output data/stop must not
 * alias the epoch or doorbell; the ordinary window ownership rules also apply.
 * Invalid metadata/wrong owner/CLOSED/exhausted cells do not publish outputs.
 */
#define SWITCHYARD_FEX_NATIVE_GATE_VERSION 1u
#define SWITCHYARD_FEX_NATIVE_GATE_SIZE_V1 48u
struct switchyard_fex_native_gate
{
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    uint64_t mapping_epoch;
    uint64_t expected_epoch;
    uint64_t gs_base;
    uint64_t suspend_doorbell;
};
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_experiment_bind_native_gate(
    struct switchyard_fex_thread *thread, const struct switchyard_fex_native_gate *gate);
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_experiment_execute_native_gate(
    struct switchyard_fex_thread *thread, const struct switchyard_fex_register_window *window,
    struct switchyard_fex_stop *stop);

/* An invocation capability, not a cached translated-block address. Prepare
 * and complete run in the system ABI on the same OS thread. Between them the
 * embedding may invoke entry exactly once with frame in x0, in custom-x18 mode
 * with the Unix-authenticated value passed to prepare. Entry returns in that
 * same custom mode. It uses AAPCS callee saves; the embedding preserves FPCR.
 * No C++ activation remains on the invoking stack after entry returns.
 *
 * Until complete (or successful fault reconstruction), the embedding must pin
 * the thread, doorbell and all guest mappings/code. It must not make a native
 * guest call or allow mutations during that interval. Complete consumes the
 * generation once; it does not execute entry. Never invoke a consumed/stale
 * descriptor. Nonlocal exits must use reconstruct_jit_fault before reentry.
 * Addresses are opaque to C consumers: do not cast entry to a C callback or
 * dereference frame. The embedding's assembly bridge owns the custom ABI. */
struct switchyard_fex_dispatch
{
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    uint64_t entry;
    uint64_t frame;
    uint64_t generation;
};

SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_prepare_dispatch(
    struct switchyard_fex_thread *thread,
    const struct switchyard_fex_execution *execution, uint64_t authenticated_x18,
    struct switchyard_fex_dispatch *dispatch);
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_complete_dispatch(
    struct switchyard_fex_thread *thread, uint64_t generation,
    struct switchyard_fex_stop *stop);

/* Register order is RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8--R15.
 * Segment order is ES, CS, SS, DS, FS, GS.  Callers must initialize size,
 * version, and flags before import. */
struct switchyard_fex_x64_state
{
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    uint64_t gpr[16];
    uint64_t rip;
    uint64_t rflags;
    uint32_t mxcsr;
    uint16_t fcw;
    uint8_t abridged_ftw;
    uint8_t reserved_fp;
    uint16_t segment[6];
    uint32_t reserved_segment;
    uint64_t segment_base[6];
    struct switchyard_fex_u128 xmm[16];
    struct switchyard_fex_u128 ymm_high[16];
    struct switchyard_fex_u128 x87[8];
};

struct switchyard_fex_stop
{
    uint32_t size;
    uint32_t version;
    uint32_t reason;
    uint8_t signal;
    uint8_t trap_number;
    uint8_t signal_code;
    uint8_t reserved;
    uint16_t error_code;
    uint16_t reserved_error;
    uint32_t reserved2;
    uint64_t rip;
};

/* Snapshot of the original native ARM64 context at a fault in generated FEX
 * code.  GPR order is x0--x30 and vector order is v0--v31.  This deliberately
 * contains no Darwin or Wine structure pointers, so the stable adapter ABI can
 * be populated from either a Darwin ucontext or Wine's ARM64_NT_CONTEXT. */
struct switchyard_fex_arm64_host_context
{
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    uint64_t gpr[31];
    struct switchyard_fex_u128 vector[32];
    uint64_t pc;
    uint64_t pstate;
    uint32_t fpcr;
    uint32_t fpsr;
};

struct switchyard_fex_fault
{
    uint32_t size;
    uint32_t version;
    uint32_t signal;
    uint32_t access;
    uint64_t host_pc;
    uint64_t guest_rip;
    uint64_t host_address;
    uint64_t guest_address;
};

#if defined(__cplusplus)
static_assert(sizeof(struct switchyard_fex_register_window) == SWITCHYARD_FEX_REGISTER_WINDOW_SIZE_V1);
static_assert(offsetof(struct switchyard_fex_register_window, data) == 16);
static_assert(offsetof(struct switchyard_fex_register_window, gs_base) == 32);
static_assert(sizeof(struct switchyard_fex_execution_domain) == SWITCHYARD_FEX_DOMAIN_SIZE_V1);
static_assert(offsetof(struct switchyard_fex_execution_domain, wake) == 16);
static_assert(offsetof(struct switchyard_fex_execution_domain, context) == 24);
static_assert(sizeof(struct switchyard_fex_dispatch) == SWITCHYARD_FEX_DISPATCH_SIZE_V1);
static_assert(sizeof(struct switchyard_fex_config) == SWITCHYARD_FEX_CONFIG_SIZE_V1);
static_assert(sizeof(struct switchyard_fex_x64_state) == SWITCHYARD_FEX_STATE_SIZE_V1);
static_assert(sizeof(struct switchyard_fex_execution) == SWITCHYARD_FEX_EXECUTION_SIZE_V1);
static_assert(sizeof(struct switchyard_fex_stop) == SWITCHYARD_FEX_STOP_SIZE_V1);
static_assert(sizeof(struct switchyard_fex_arm64_host_context) ==
              SWITCHYARD_FEX_ARM64_HOST_CONTEXT_SIZE_V1);
static_assert(sizeof(struct switchyard_fex_fault) == SWITCHYARD_FEX_FAULT_SIZE_V1);
static_assert(sizeof(struct switchyard_fex_executable_range) == SWITCHYARD_FEX_EXECUTABLE_RANGE_SIZE_V1);
#else
_Static_assert(sizeof(struct switchyard_fex_register_window) == SWITCHYARD_FEX_REGISTER_WINDOW_SIZE_V1 &&
               offsetof(struct switchyard_fex_register_window, data) == 16 &&
               offsetof(struct switchyard_fex_register_window, gs_base) == 32,
               "switchyard_fex_register_window ABI drift");
_Static_assert(sizeof(struct switchyard_fex_execution_domain) == SWITCHYARD_FEX_DOMAIN_SIZE_V1,
               "switchyard_fex_execution_domain ABI drift");
_Static_assert(offsetof(struct switchyard_fex_execution_domain, wake) == 16 &&
               offsetof(struct switchyard_fex_execution_domain, context) == 24,
               "switchyard_fex_execution_domain pointer layout");
_Static_assert(sizeof(struct switchyard_fex_dispatch) == SWITCHYARD_FEX_DISPATCH_SIZE_V1,
               "switchyard_fex_dispatch ABI drift");
_Static_assert(sizeof(struct switchyard_fex_config) == SWITCHYARD_FEX_CONFIG_SIZE_V1,
               "switchyard_fex_config ABI drift");
_Static_assert(sizeof(struct switchyard_fex_x64_state) == SWITCHYARD_FEX_STATE_SIZE_V1,
               "switchyard_fex_x64_state ABI drift");
_Static_assert(sizeof(struct switchyard_fex_execution) == SWITCHYARD_FEX_EXECUTION_SIZE_V1,
               "switchyard_fex_execution ABI drift");
_Static_assert(sizeof(struct switchyard_fex_stop) == SWITCHYARD_FEX_STOP_SIZE_V1,
               "switchyard_fex_stop ABI drift");
_Static_assert(sizeof(struct switchyard_fex_arm64_host_context) ==
                   SWITCHYARD_FEX_ARM64_HOST_CONTEXT_SIZE_V1,
               "switchyard_fex_arm64_host_context ABI drift");
_Static_assert(sizeof(struct switchyard_fex_fault) == SWITCHYARD_FEX_FAULT_SIZE_V1,
               "switchyard_fex_fault ABI drift");
_Static_assert(sizeof(struct switchyard_fex_executable_range) == SWITCHYARD_FEX_EXECUTABLE_RANGE_SIZE_V1,
               "switchyard_fex_executable_range ABI drift");
#endif

SWITCHYARD_FEX_API uint32_t switchyard_fex_abi_version(void);
SWITCHYARD_FEX_API const char *switchyard_fex_provider_abi_identity(void);
SWITCHYARD_FEX_API const char *switchyard_fex_upstream_revision(void);
SWITCHYARD_FEX_API const char *switchyard_fex_result_string(enum switchyard_fex_result result);

SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_process_create(
    const struct switchyard_fex_config *config,
    struct switchyard_fex_process **out_process);
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_process_destroy(
    struct switchyard_fex_process *process);

SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_create(
    struct switchyard_fex_process *process,
    struct switchyard_fex_thread **out_thread);
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_destroy(
    struct switchyard_fex_thread *thread);
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_import_state(
    struct switchyard_fex_thread *thread,
    const struct switchyard_fex_x64_state *state);
/* Initialize only size/version before export. On OK every state byte, including
 * reserved fields, is initialized. Do not consume the payload on failure. */
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_export_state(
    struct switchyard_fex_thread *thread,
    struct switchyard_fex_x64_state *state);
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_import_register_window(
    struct switchyard_fex_thread *thread,
    const struct switchyard_fex_register_window *window);
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_export_register_window(
    struct switchyard_fex_thread *thread,
    const struct switchyard_fex_register_window *window);

/* Execute until FEX reaches an embedding-owned boundary, a recognized guest
 * exception, or the optional designated HLT.  Callers initialize execution and
 * stop size/version fields before every call. */
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_execute(
    struct switchyard_fex_thread *thread,
    const struct switchyard_fex_execution *execution,
    struct switchyard_fex_stop *stop);

/* Repair an Apple Silicon alignment fault raised by generated FEX code without
 * consuming the active execution.  This must be called from the faulting host
 * thread before its native stack or Unix-call context is abandoned.  On
 * success the generated instruction has been safely backpatched and the
 * mutable host context contains the PC/GPR state from which execution retries. */
SWITCHYARD_FEX_API enum switchyard_fex_result
switchyard_fex_thread_repair_unaligned_tso(
    struct switchyard_fex_thread *thread,
    struct switchyard_fex_arm64_host_context *host_context);

/* Recover a provider-owned return-prediction stack guard fault. This stack is
 * a cache, not the architectural x64 stack. Only an authenticated generated
 * push/pop at this thread's exact guard boundary may be retried. access is
 * 0 for read or 1 for write, as in a Windows access-violation record. */
SWITCHYARD_FEX_API enum switchyard_fex_result
switchyard_fex_thread_repair_callret_fault(
    struct switchyard_fex_thread *thread,
    struct switchyard_fex_arm64_host_context *host_context,
    uint32_t access, uint64_t host_fault_address);

/* Query the fault-time architectural guest RSP without changing state or
 * consuming execution. Signal-context only, on the interrupted executing
 * thread. No locks, allocation, or Darwin calls; process/code ownership stays
 * pinned by that active execution. Non-JIT PCs always reject: compiler frames
 * own C++ resources and cannot be abandoned. Output is unchanged on failure. */
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_query_jit_stack(
    struct switchyard_fex_thread *thread,
    const struct switchyard_fex_arm64_host_context *host_context,
    uint32_t access, uint64_t host_fault_address, uint64_t *guest_rsp);

/* Reconstruct a guest x64 state from the original ARM64 register snapshot when
 * Wine has identified a fault in generated FEX code.  The call must run on the
 * same host thread whose execute call was interrupted.  On success it consumes
 * that active execution, allowing Wine to discard the interrupted native stack
 * and later resume through a fresh BeginSimulation entry. */
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_reconstruct_jit_fault(
    struct switchyard_fex_thread *thread,
    const struct switchyard_fex_arm64_host_context *host_context,
    uint32_t signal,
    uint32_t access,
    uint64_t host_fault_address,
    struct switchyard_fex_x64_state *state,
    struct switchyard_fex_fault *fault);

SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_process_invalidate_code(
    struct switchyard_fex_process *process,
    uint64_t guest_address,
    uint64_t length);
SWITCHYARD_FEX_API enum switchyard_fex_result switchyard_fex_thread_clear_code_cache(
    struct switchyard_fex_thread *thread);

#ifdef __cplusplus
}
#endif

#endif
