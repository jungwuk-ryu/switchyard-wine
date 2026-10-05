/* SPDX-License-Identifier: MIT */
#include "switchyard_fex_admission.h"

/* Compiled and executed as both C and C++; returns the first failing line. */
static int test_admission_contract(void)
{
    struct switchyard_fex_admission cell = {0};
    uint64_t first = 0, second = 0, untouched = UINT64_MAX;
#define ADMISSION_CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
    ADMISSION_CHECK(switchyard_fex_admission_acquire(&cell, 1, &first) == 1 && (first >> 3) == 1);
    ADMISSION_CHECK(switchyard_fex_admission_acquire(&cell, 0, &untouched) == 0 && untouched == UINT64_MAX);
    switchyard_fex_admission_close(&cell);
    ADMISSION_CHECK(switchyard_fex_admission_load(&cell) == (first | SWITCHYARD_FEX_ADMISSION_CLOSED));
    ADMISSION_CHECK(switchyard_fex_admission_release(&cell, first));
    ADMISSION_CHECK(switchyard_fex_admission_load(&cell) == (8 | SWITCHYARD_FEX_ADMISSION_CLOSED));
    ADMISSION_CHECK(!switchyard_fex_admission_release(&cell, first));
    ADMISSION_CHECK(switchyard_fex_admission_acquire(&cell, 1, &untouched) == 0 && untouched == UINT64_MAX);
    switchyard_fex_admission_reopen(&cell);
    ADMISSION_CHECK(switchyard_fex_admission_acquire(&cell, 1, &second) == 1 && (second >> 3) == 2);
    switchyard_fex_admission_close(&cell);
    ADMISSION_CHECK(!switchyard_fex_admission_release(&cell, first));
    ADMISSION_CHECK(!switchyard_fex_admission_release(&cell, second | SWITCHYARD_FEX_ADMISSION_CLOSED));
    ADMISSION_CHECK(!switchyard_fex_admission_release(&cell, 0));
    ADMISSION_CHECK(switchyard_fex_admission_load(&cell) == (second | SWITCHYARD_FEX_ADMISSION_CLOSED));
    /* Reopening cannot steal an active lease either. */
    switchyard_fex_admission_reopen(&cell);
    ADMISSION_CHECK(switchyard_fex_admission_load(&cell) == second);
    ADMISSION_CHECK(switchyard_fex_admission_release(&cell, second));
    ADMISSION_CHECK(switchyard_fex_admission_acquire(&cell, 0, &first) == 1 && (first >> 3) == 2);
    ADMISSION_CHECK(!switchyard_fex_admission_release(&cell, second));
    switchyard_fex_admission_close(&cell);
    ADMISSION_CHECK(switchyard_fex_admission_release(&cell, first));
    ADMISSION_CHECK(switchyard_fex_admission_load(&cell) == (16 | SWITCHYARD_FEX_ADMISSION_CLOSED));
    switchyard_fex_admission_reopen(&cell);
    /* Fixture-only initialization at the last representable generation. */
    __atomic_store_n(&cell.word, UINT64_MAX & ~SWITCHYARD_FEX_ADMISSION_FLAGS, __ATOMIC_RELEASE);
    ADMISSION_CHECK(switchyard_fex_admission_acquire(&cell, 1, &untouched) == -1 && untouched == UINT64_MAX);
    ADMISSION_CHECK(switchyard_fex_admission_load(&cell) == (UINT64_MAX & ~SWITCHYARD_FEX_ADMISSION_FLAGS));
    ADMISSION_CHECK(switchyard_fex_admission_acquire(&cell, 0, &first) == 1);
    ADMISSION_CHECK(switchyard_fex_admission_release(&cell, first));
#undef ADMISSION_CHECK
    return 0;
}
