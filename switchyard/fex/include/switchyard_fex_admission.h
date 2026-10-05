/* SPDX-License-Identifier: MIT */
#ifndef SWITCHYARD_FEX_ADMISSION_H
#define SWITCHYARD_FEX_ADMISSION_H

#include <stdint.h>

/* One modification order owns both admission and closure. A separate gate and
 * active counter cannot exclude an entering executor without another lock.
 * ACTIVE pins state even for short import/export operations; EXECUTING adds a
 * generation-bearing execution capability. CLOSED is owned by the domain's
 * mutation mutex, and must survive a concurrent release of either operation.
 * This shared POD has no C++ atomic layout dependency. All accesses are atomic.
 */
struct switchyard_fex_admission {
    uint64_t word;
};

#define SWITCHYARD_FEX_ADMISSION_ACTIVE UINT64_C(1)
#define SWITCHYARD_FEX_ADMISSION_EXECUTING UINT64_C(2)
#define SWITCHYARD_FEX_ADMISSION_CLOSED UINT64_C(4)
#define SWITCHYARD_FEX_ADMISSION_FLAGS UINT64_C(7)
#define SWITCHYARD_FEX_ADMISSION_GENERATION_MAX (UINT64_MAX >> 3)

#ifdef __cplusplus
static_assert(sizeof(switchyard_fex_admission) == 8 && alignof(switchyard_fex_admission) == 8);
static_assert(__atomic_always_lock_free(8, nullptr));
#else
_Static_assert(sizeof(struct switchyard_fex_admission) == 8 &&
               _Alignof(struct switchyard_fex_admission) == 8, "admission layout");
_Static_assert(__atomic_always_lock_free(8, 0), "lock-free admission required");
#endif

static inline uint64_t switchyard_fex_admission_load(const struct switchyard_fex_admission *cell)
{
    return __atomic_load_n(&cell->word, __ATOMIC_ACQUIRE);
}

/* Return 1 on success, 0 when busy, -1 on generation exhaustion. A failed
 * acquisition leaves both the cell and the caller's token untouched. */
static inline int switchyard_fex_admission_acquire(struct switchyard_fex_admission *cell,
                                                  int execution, uint64_t *token)
{
    uint64_t previous = switchyard_fex_admission_load(cell), next;
    if (previous & SWITCHYARD_FEX_ADMISSION_FLAGS) return 0;
    if (execution && (previous >> 3) == SWITCHYARD_FEX_ADMISSION_GENERATION_MAX) return -1;
    next = execution ? (previous + 8) | SWITCHYARD_FEX_ADMISSION_ACTIVE |
                                       SWITCHYARD_FEX_ADMISSION_EXECUTING :
                       previous | SWITCHYARD_FEX_ADMISSION_ACTIVE;
    if (!__atomic_compare_exchange_n(&cell->word, &previous, next, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
        return 0;
    *token = next;
    return 1;
}

static inline int switchyard_fex_admission_release_owned(struct switchyard_fex_admission *cell,
                                                        uint64_t token, uint64_t *released)
{
    uint64_t previous = switchyard_fex_admission_load(cell);
    if (!(token & SWITCHYARD_FEX_ADMISSION_ACTIVE) || (token & SWITCHYARD_FEX_ADMISSION_CLOSED)) return 0;
    do {
        if ((previous & ~SWITCHYARD_FEX_ADMISSION_CLOSED) != token) return 0;
    } while (!__atomic_compare_exchange_n(&cell->word, &previous,
              previous & ~(SWITCHYARD_FEX_ADMISSION_ACTIVE | SWITCHYARD_FEX_ADMISSION_EXECUTING),
              1, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    if (released) *released = previous;
    return 1;
}

static inline int switchyard_fex_admission_release(struct switchyard_fex_admission *cell, uint64_t token)
{
    return switchyard_fex_admission_release_owned(cell, token, 0);
}

/* Close/reopen require the domain owner's mutex. The caller must close every member
 * before scanning for ACTIVE, and may mutate only if all are idle. Reopen on
 * every path, including a failed scan, while membership is still protected. */
static inline void switchyard_fex_admission_close(struct switchyard_fex_admission *cell)
{
    __atomic_fetch_or(&cell->word, SWITCHYARD_FEX_ADMISSION_CLOSED, __ATOMIC_ACQ_REL);
}

static inline void switchyard_fex_admission_reopen(struct switchyard_fex_admission *cell)
{
    __atomic_fetch_and(&cell->word, ~SWITCHYARD_FEX_ADMISSION_CLOSED, __ATOMIC_RELEASE);
}

#endif
