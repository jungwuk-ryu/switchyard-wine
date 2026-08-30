/* Native truth-table tests for ARM64EC signal-return ownership policy. */

#include <stdbool.h>
#include <stdio.h>

#include "arm64ec_emulation_dispatch.h"

struct dispatch_case
{
    bool arm64ec;
    bool guest_return_requested;
    bool simulation_active;
    bool target_is_ec_code;
    bool expected;
};

struct suspend_case
{
    bool arm64ec;
    bool suspend_pending;
    bool syscall_callback_active;
    bool simulation_active;
    bool guest_return_requested;
    bool expected;
};

static const struct dispatch_case cases[] =
{
    {false, false, false, false, false},
    {false, false, false, true,  false},
    {false, false, true,  false, false},
    {false, false, true,  true,  false},
    {false, true,  false, false, false},
    {false, true,  false, true,  false},
    {false, true,  true,  false, false},
    {false, true,  true,  true,  false},
    {true,  false, false, false, true},
    {true,  false, false, true,  false},
    {true,  false, true,  false, false},
    {true,  false, true,  true,  false},
    {true,  true,  false, false, true},
    {true,  true,  false, true,  false},
    {true,  true,  true,  false, true},
    {true,  true,  true,  true,  false},
};

static const struct dispatch_case pending_cases[] =
{
    {false, false, false, false, false},
    {false, false, false, true,  false},
    {false, true,  false, false, false},
    {false, true,  false, true,  false},
    {true,  false, false, false, false},
    {true,  false, false, true,  false},
    {true,  true,  false, false, true},
    {true,  true,  false, true,  false},
};

static const struct suspend_case suspend_cases[] =
{
    {false, false, false, false, false, false},
    {true,  false, false, true,  true,  false},
    {true,  true,  true,  false, true,  false},
    {true,  true,  true,  true,  true,  false},
    {false, true,  false, false, false, true},
    {true,  true,  false, false, false, true},
    {true,  true,  false, false, true,  true},
    {false, true,  false, true,  true,  false},
    {true,  true,  false, true,  false, false},
    {true,  true,  false, true,  true,  true},
};

int main(void)
{
    unsigned int failures = 0;
    unsigned int i;

    /* A live SIGUSR1 ucontext is native AArch64 state.  Without the one-shot
     * producer request, a non-EC PC must never be sent to x64 emulation. */
    {
        volatile unsigned int restore_flags = 0;
        bool requested = arm64ec_consume_emulation_dispatch_request(
            &restore_flags, 0x10000 );
        bool result = arm64ec_emulation_dispatch_pending( true, requested, false );

        if (requested || result)
        {
            fprintf( stderr,
                     "live native SIGUSR1 context acquired guest provenance: "
                     "requested=%u dispatch=%u\n", requested, result );
            failures++;
        }
    }

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        const struct dispatch_case *test = &cases[i];
        bool result = arm64ec_emulation_dispatch_required(
            test->arm64ec, test->guest_return_requested,
            test->simulation_active, test->target_is_ec_code );

        if (result == test->expected) continue;
        fprintf( stderr,
                 "producer case %u: arm64ec=%u requested=%u simulation=%u ec=%u "
                 "returned %u, expected %u\n",
                 i, test->arm64ec, test->guest_return_requested,
                 test->simulation_active,
                 test->target_is_ec_code,
                 result, test->expected );
        failures++;
    }

    for (i = 0; i < sizeof(pending_cases) / sizeof(pending_cases[0]); ++i)
    {
        const struct dispatch_case *test = &pending_cases[i];
        const unsigned int request_flag = 0x10000;
        const unsigned int unrelated_flag = 0x2;
        volatile unsigned int restore_flags = unrelated_flag |
            (test->guest_return_requested ? request_flag : 0);
        bool requested = arm64ec_consume_emulation_dispatch_request(
            &restore_flags, request_flag );
        bool result = arm64ec_emulation_dispatch_pending(
            test->arm64ec, requested,
            test->target_is_ec_code );

        if (requested != test->guest_return_requested ||
            restore_flags != unrelated_flag ||
            arm64ec_consume_emulation_dispatch_request(
                &restore_flags, request_flag ))
        {
            fprintf( stderr,
                     "consumer case %u did not consume exactly one request: "
                     "requested=%u expected=%u flags=%#x\n",
                     i, requested, test->guest_return_requested,
                     (unsigned int)restore_flags );
            failures++;
            continue;
        }
        if (result == test->expected) continue;
        fprintf( stderr,
                 "consumer case %u: arm64ec=%u requested=%u ec=%u "
                 "returned %u, expected %u\n",
                 i, test->arm64ec, requested,
                 test->target_is_ec_code, result, test->expected );
        failures++;
    }

    for (i = 0; i < sizeof(suspend_cases) / sizeof(suspend_cases[0]); ++i)
    {
        const struct suspend_case *test = &suspend_cases[i];
        bool result = arm64ec_suspend_handoff_ready(
            test->arm64ec, test->suspend_pending,
            test->syscall_callback_active, test->simulation_active,
            test->guest_return_requested );

        if (result == test->expected) continue;
        fprintf( stderr,
                 "suspend case %u: arm64ec=%u pending=%u callback=%u "
                 "simulation=%u guest=%u returned %u, expected %u\n",
                 i, test->arm64ec, test->suspend_pending,
                 test->syscall_callback_active, test->simulation_active,
                 test->guest_return_requested, result, test->expected );
        failures++;
    }

    if (failures) return 1;
    puts( "ARM64EC emulation-dispatch ownership policy: ok" );
    return 0;
}
