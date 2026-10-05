/* SPDX-License-Identifier: MIT */

/* Test-only link environment for the actual Wine mode/signal policy. There is
 * deliberately no fake TEB or guest-exception reconstruction. SIGTRAP resumption
 * is covered; server suspension, SIGUSR2 and PE exception dispatch are not. */
#include "wine_signal_wrapper.h"
#include <stdint.h>
#include <unistd.h>
#include <sys/ucontext.h>
#include <os/arch/arm64.h>
#include "wine/asm.h"

typedef int BOOL;
typedef uintptr_t ULONG_PTR;
#define TRUE 1
#define FALSE 0
#define HAVE_OS_CUSTOM_X18_ABI 1
#define REGn_sig(n, context) ((context)->uc_mcontext->__ss.__x[n])
#define PC_sig(context) ((context)->uc_mcontext->__ss.__pc)
#define SP_sig(context) ((context)->uc_mcontext->__ss.__sp)

struct thread_data { void *teb; };
struct syscall_frame { ULONG_PTR sp, pc, x[29]; };
static const void *pKiUserExceptionDispatcher, *pKiUserEmulationDispatcher;

static struct thread_data *get_thread_data(void) { return NULL; }
static struct syscall_frame *get_syscall_frame(struct thread_data *data)
{
    (void)data;
    _exit(98); /* No Wine syscall frame exists in this standalone test. */
}
static BOOL recover_lost_custom_x18_fault(struct thread_data *data, ucontext_t *context, siginfo_t *info)
{
    (void)data;
    (void)context;
    (void)info;
    _exit(98);
}
static void usr2_handler(int signal, siginfo_t *info, void *context)
{
    (void)signal;
    (void)info;
    (void)context;
    _exit(98);
}

#include "wine_x18_policy.inc"

bool wine_test_x18_init(bool public_only)
{
    if (!init_custom_x18_abi()) return false;
    if (public_only) use_idempotent_x18_transition = FALSE;
    return !custom_x18_abi_enabled();
}

bool wine_test_x18_idempotent(void) { return use_idempotent_x18_transition != FALSE; }

void wine_test_x18_transition(bool custom)
{
    if (custom) enter_windows_x18_abi();
    else enter_system_x18_abi();
}

void wine_test_x18_signal(void (*handler)(int, siginfo_t *, void *),
                          int signal, siginfo_t *info, void *context)
{
    dispatch_signal_with_system_x18(handler, signal, info, context);
}
