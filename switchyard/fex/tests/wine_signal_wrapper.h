// SPDX-License-Identifier: MIT
#pragma once

#include <signal.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
bool wine_test_x18_init(bool public_only);
bool wine_test_x18_idempotent(void);
void wine_test_x18_transition(bool custom);
void wine_test_x18_signal(void (*handler)(int, siginfo_t *, void *),
                          int signal, siginfo_t *info, void *context);
#ifdef __cplusplus
}
#endif
