/* Replace only this executable's imported data slot with an observer; the
 * generated policy is the actual Wine source. No real mode switch is made. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

void *update_tpidr;
uint64_t observed_x15, observed_tpidr, callee_request, returned_x15;
extern void observe_update(uintptr_t);
extern void invoke_policy(unsigned int, uint64_t);

__asm__(
  ".text\n.p2align 2\n.globl _observe_update\n_observe_update:\n"
  "adrp x11, _observed_x15@PAGE\n"
  "str x15, [x11, _observed_x15@PAGEOFF]\n"
  "adrp x11, _observed_tpidr@PAGE\n"
  "str x0, [x11, _observed_tpidr@PAGEOFF]\n"
  "adrp x11, _callee_request@PAGE\nldr x15, [x11, _callee_request@PAGEOFF]\nret\n"
  ".p2align 2\n.globl _invoke_policy\n_invoke_policy:\n"
  "stp x29, x30, [sp, #-16]!\nmov x29, sp\n"
  "mov x15, x1\nbl _set_custom_x18_abi_enabled_idempotent\n"
  "adrp x11, _returned_x15@PAGE\nstr x15, [x11, _returned_x15@PAGEOFF]\n"
  "ldp x29, x30, [sp], #16\nret\n");

int main(void)
{
    const uint64_t incoming[] = {0, 1, UINT64_MAX, UINT64_C(0xfeedface01234567)};
    uintptr_t observer = (uintptr_t)observe_update;
    uint64_t before, expected;
    unsigned int mode, i, request;
    /* The real policy authenticates its callback with BRAAZ, so provide an
     * actual IA/zero signed test target, not an incompatible pointer cast. */
    __asm__ volatile("paciza %0" : "+r"(observer));
    update_tpidr = (void *)observer;
    for (request = 0; request < 2; ++request)
    for (mode = 0; mode < 2; ++mode)
        for (i = 0; i < sizeof(incoming) / sizeof(incoming[0]); ++i)
        {
            __asm__ volatile("mrs %0, TPIDR_EL0" : "=r"(before));
            expected = before & UINT64_C(0xfffefffffff00000);
            if (mode) expected |= UINT64_C(0x1000000000000);
            observed_x15 = UINT64_C(0xdeadbeef);
            observed_tpidr = 0;
            callee_request = request;
            returned_x15 = UINT64_MAX;
            invoke_policy(mode, incoming[i]);
            if (observed_x15 || observed_tpidr != expected || returned_x15 != request)
            {
                printf("FAIL mode=%u incoming_x15=%" PRIu64 " entry_x15=%" PRIu64
                       " tpidr_payload_match=%d callee_request_preserved=%d\n", mode, incoming[i], observed_x15,
                       observed_tpidr == expected, returned_x15 == request);
                return 1;
            }
        }
    puts("actual Wine x18 entry contract passed: 16 cases, callee-set request preserved");
    return 0;
}
