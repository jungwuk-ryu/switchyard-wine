/* Focused tests for dlls/winemac.drv/frame_scheduler.c. */

#include <CoreGraphics/CoreGraphics.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_scheduler.h"

#define STRESS_PUBLISHES 250000
#define READER_COUNT 3

static unsigned int failures;

#define check(condition, ...) do { if (!(condition)) { \
    fprintf(stderr, "failure at %s:%u: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; \
} } while (0)

static void test_time_conversion(void)
{
    uint64_t period_60 = macdrv_frame_ticks_from_rate(60);
    uint64_t period_120 = macdrv_frame_ticks_from_rate(120);
    double seconds_60 = macdrv_frame_seconds_from_ticks(period_60);

    check(period_60 && period_120, "rate conversion returned zero");
    check(period_60 >= period_120 * 2 - 1 && period_60 <= period_120 * 2 + 1,
          "60/120 Hz periods are inconsistent: %llu, %llu",
          (unsigned long long)period_60, (unsigned long long)period_120);
    check(seconds_60 > 0.016 && seconds_60 < 0.017,
          "60 Hz conversion produced %.9f seconds", seconds_60);
    check(!macdrv_frame_ticks_from_rate(0), "zero rate produced a period");
    check(!macdrv_frame_ticks_from_seconds(0.0), "zero seconds produced ticks");
    check(!macdrv_frame_ticks_from_seconds(-1.0), "negative seconds produced ticks");
    check(macdrv_frame_ticks_from_seconds(1.0e300) == UINT64_MAX,
          "huge seconds did not saturate");
}

static void test_deadline_policy(void)
{
    struct macdrv_frame_deadline deadline = {0};
    uint64_t wait;

    wait = macdrv_frame_deadline_reserve(&deadline, 1000, 900, 100, 1, true);
    check(!wait && deadline.next == 1100, "first reservation was %llu, next %llu",
          (unsigned long long)wait, (unsigned long long)deadline.next);

    wait = macdrv_frame_deadline_reserve(&deadline, 1050, 900, 100, 1, true);
    check(wait == 1100 && deadline.next == 1200,
          "on-time reservation was %llu, next %llu",
          (unsigned long long)wait, (unsigned long long)deadline.next);

    /* A late producer presents once immediately and reserves the next phase;
     * it never runs multiple catch-up frames. */
    wait = macdrv_frame_deadline_reserve(&deadline, 1250, 900, 100, 1, true);
    check(!wait && deadline.next == 1350, "missed deadline caught up incorrectly");

    wait = macdrv_frame_deadline_reserve(&deadline, 1260, 900, 100, 2, true);
    check(!wait && deadline.generation == 2, "generation change delayed a frame");

    wait = macdrv_frame_deadline_reserve(&deadline, 1270, 0, 100, 2, false);
    check(!wait && !deadline.initialized, "suspend did not reset the deadline");

    wait = macdrv_frame_deadline_reserve(&deadline, UINT64_MAX - 5,
                                         UINT64_MAX - 20, 10, 3, true);
    check(!wait && deadline.next >= UINT64_MAX - 5,
          "overflow wrapped next deadline to %llu", (unsigned long long)deadline.next);

    memset(&deadline, 0, sizeof(deadline));
    {
        uint64_t now = macdrv_frame_time_now();
        uint64_t maximum_period = macdrv_frame_ticks_from_rate(1);

        macdrv_frame_deadline_reserve(&deadline, now, now, maximum_period * 2, 4, true);
        check(deadline.next - now == maximum_period,
              "resource-bound interval was not clamped to one second");
    }

    memset(&deadline, 0, sizeof(deadline));
    {
        uint64_t now = macdrv_frame_time_now();
        uint64_t slow_period = macdrv_frame_ticks_from_rate(1);

        wait = macdrv_frame_deadline_reserve(&deadline, now, now, slow_period, 4, true);
        check(!wait, "first one-Hz reservation waited");
        wait = macdrv_frame_deadline_reserve(&deadline, now + slow_period / 10,
                                             now, slow_period, 4, true);
        check(wait == now + slow_period, "one-Hz reservation was capped unexpectedly");
    }
}

static void test_evolving_predictive_phase(void)
{
    struct macdrv_frame_deadline deadline = {0};
    uint64_t wait;

    wait = macdrv_frame_deadline_reserve(&deadline, 1000, 1100, 200, 1, true);
    check(!wait && deadline.next == 1100,
          "predictive phase did not seed the first deadline");
    wait = macdrv_frame_deadline_reserve(&deadline, 1050, 1200, 200, 1, true);
    check(wait == 1100 && deadline.next == 1300,
          "evolving phase shortened or re-anchored a reserved interval");
    wait = macdrv_frame_deadline_reserve(&deadline, 1310, 1400, 200, 1, true);
    check(!wait && deadline.next == 1510,
          "late predictive phase produced a catch-up interval");
}

static uint64_t fuzz_state = 0xd1b54a32d192ed03ULL;

static uint64_t fuzz_next(void)
{
    fuzz_state ^= fuzz_state >> 12;
    fuzz_state ^= fuzz_state << 25;
    fuzz_state ^= fuzz_state >> 27;
    return fuzz_state * 0x2545f4914f6cdd1dULL;
}

static void test_random_deadlines(void)
{
    struct macdrv_frame_deadline deadline = {0};
    const uint64_t maximum_period = macdrv_frame_ticks_from_rate(1);
    uint64_t now = macdrv_frame_time_now(), wait;
    unsigned int i;

    for (i = 0; i < 100000; ++i)
    {
        uint64_t period = 1 + fuzz_next() % maximum_period;
        uint64_t phase_age = fuzz_next() % (maximum_period * 8);
        uint64_t phase;

        now += fuzz_next() % (maximum_period / 2 + 1);
        phase = now >= phase_age ? now - phase_age : 0;
        if (!(i % 997)) memset(&deadline, 0, sizeof(deadline));
        wait = macdrv_frame_deadline_reserve(&deadline, now, phase, period,
                                             1 + i / 4096, true);
        check(!wait || (wait > now && wait - now <= maximum_period),
              "random deadline returned invalid wait at iteration %u", i);
        check(deadline.initialized && deadline.next > deadline.last_now &&
              deadline.next - deadline.last_now <= maximum_period,
              "random deadline reserved invalid future phase at iteration %u", i);
        if (failures) break;
    }
}

static void test_long_run_drift(void)
{
    struct macdrv_frame_deadline deadline = {0};
    const uint64_t phase = macdrv_frame_time_now();
    const uint64_t period = macdrv_frame_ticks_from_rate(120);
    uint64_t now = phase + 17, expected, wait;
    unsigned int i;

    wait = macdrv_frame_deadline_reserve(&deadline, now, phase, period, 7, true);
    check(!wait, "first drift reservation waited");
    for (i = 1; i < 100000; ++i)
    {
        expected = phase + (uint64_t)i * period;
        now = deadline.next - period / 3;
        wait = macdrv_frame_deadline_reserve(&deadline, now, phase, period, 7, true);
        check(wait == expected, "deadline drift at %u: got %llu expected %llu", i,
              (unsigned long long)wait, (unsigned long long)expected);
        if (failures) break;
    }
}

static void test_display_selection(void)
{
    const CGDirectDisplayID ids[] = {10, 20};
    const CGRect bounds[] = {CGRectMake(0, 0, 100, 100), CGRectMake(100, 0, 100, 100)};
    CGDirectDisplayID selected;

    selected = macdrv_frame_select_display(CGRectMake(10, 10, 60, 60), 0, 10,
                                            ids, bounds, 2);
    check(selected == 10, "left display selection returned %u", selected);
    selected = macdrv_frame_select_display(CGRectMake(120, 10, 60, 60), 10, 10,
                                            ids, bounds, 2);
    check(selected == 20, "display migration returned %u", selected);
    selected = macdrv_frame_select_display(CGRectMake(75, 10, 50, 50), 20, 10,
                                            ids, bounds, 2);
    check(selected == 20, "equal-overlap selection did not retain current display");
    selected = macdrv_frame_select_display(CGRectMake(500, 500, 10, 10), 20, 10,
                                            ids, bounds, 2);
    check(selected == 20, "offscreen selection did not retain an online display");
    selected = macdrv_frame_select_display(CGRectMake(500, 500, 10, 10), 30, 10,
                                            ids, bounds, 2);
    check(selected == 10, "hot-unplug selection did not fall back to main display");
    selected = macdrv_frame_select_display(CGRectMake(75, 10, 50, 50), 30, 10,
                                            ids, bounds, 2);
    check(selected == 10, "equal-overlap selection did not prefer main display");
}

struct clock_stress
{
    struct macdrv_frame_clock *clock;
    uint64_t period;
    volatile bool done;
    volatile unsigned int errors;
};

static void *clock_writer(void *opaque)
{
    struct clock_stress *stress = opaque;
    uint64_t host_time = stress->period;
    unsigned int i;

    for (i = 0; i < STRESS_PUBLISHES; ++i)
    {
        if (!(i % 4096)) macdrv_frame_configuration_changed();
        macdrv_frame_clock_publish(stress->clock, host_time, 0);
        host_time += stress->period;
    }
    __atomic_store_n(&stress->done, true, __ATOMIC_RELEASE);
    return NULL;
}

static void *clock_reader(void *opaque)
{
    struct clock_stress *stress = opaque;
    struct macdrv_frame_timing timing;
    uint64_t previous = 0;

    while (!__atomic_load_n(&stress->done, __ATOMIC_ACQUIRE))
    {
        if (!macdrv_frame_clock_snapshot(stress->clock, &timing) || !timing.period)
            __atomic_add_fetch(&stress->errors, 1, __ATOMIC_RELAXED);
        if (timing.host_time && timing.host_time < previous)
            __atomic_add_fetch(&stress->errors, 1, __ATOMIC_RELAXED);
        if (timing.host_time) previous = timing.host_time;
    }
    return NULL;
}

static void test_concurrent_clock_snapshot(void)
{
    struct clock_stress stress = {0};
    struct macdrv_frame_timing timing;
    uint64_t previous_generation;
    pthread_t writer, readers[READER_COUNT];
    bool writer_started = false;
    unsigned int i, reader_count = 0;

    stress.period = macdrv_frame_ticks_from_rate(120);
    stress.clock = macdrv_frame_clock_register(0x12345678, stress.period);
    check(!!stress.clock, "failed to reserve a display clock slot");
    if (!stress.clock) return;

    if (!pthread_create(&writer, NULL, clock_writer, &stress)) writer_started = true;
    else check(false, "failed to create writer");
    for (i = 0; i < READER_COUNT; ++i)
    {
        if (!pthread_create(&readers[reader_count], NULL, clock_reader, &stress))
            ++reader_count;
        else
            check(false, "failed to create reader %u", i);
    }
    if (writer_started) pthread_join(writer, NULL);
    else __atomic_store_n(&stress.done, true, __ATOMIC_RELEASE);
    for (i = 0; i < reader_count; ++i) pthread_join(readers[i], NULL);
    check(!stress.errors, "concurrent snapshots observed %u inconsistent samples",
          stress.errors);

    macdrv_frame_configuration_changed();
    check(macdrv_frame_clock_snapshot(stress.clock, &timing) && timing.generation,
          "configuration transition invalidated a live fallback clock");
    check(timing.display_id == 0x12345678,
          "clock snapshot returned the wrong display ID %u", timing.display_id);
    previous_generation = timing.generation;
    macdrv_frame_clock_unregister(stress.clock);
    check(!macdrv_frame_clock_snapshot(stress.clock, &timing),
          "unregistered display clock remained valid");

    stress.clock = macdrv_frame_clock_register(0x87654321, stress.period);
    check(stress.clock && macdrv_frame_clock_snapshot(stress.clock, &timing) &&
          timing.display_id == 0x87654321,
          "reused display-clock slot retained stale identity");
    check(timing.generation != previous_generation,
          "reused display-clock slot retained stale generation");
    {
        uint64_t host_time = macdrv_frame_time_now();
        uint64_t hinted_period = macdrv_frame_ticks_from_rate(60);

        macdrv_frame_clock_publish(stress.clock, host_time, hinted_period);
        check(macdrv_frame_clock_snapshot(stress.clock, &timing) &&
              timing.host_time == host_time && timing.period == hinted_period,
              "valid refresh-period hint was not published");
        macdrv_frame_clock_publish(stress.clock, host_time - 1,
                                   macdrv_frame_ticks_from_rate(30));
        check(macdrv_frame_clock_snapshot(stress.clock, &timing) &&
              timing.host_time == host_time && timing.period == hinted_period,
              "non-monotonic callback corrupted the clock snapshot");
    }
    macdrv_frame_clock_unregister(stress.clock);
}

static void test_scheduler_state_transitions(void)
{
    struct macdrv_frame_scheduler scheduler;
    CGRect main_bounds = CGDisplayBounds(CGMainDisplayID());
    uint64_t generation;

    macdrv_frame_scheduler_init(&scheduler);
    macdrv_frame_scheduler_set_window(&scheduler, main_bounds, true);
    check(scheduler.active && scheduler.display_id && scheduler.fallback_period,
          "active scheduler did not resolve its display");
    generation = scheduler.configuration_generation;
    scheduler.deadline.initialized = true;
    macdrv_frame_scheduler_set_window(&scheduler, CGRectInset(main_bounds, 1, 1), true);
    check(scheduler.deadline.initialized,
          "same-display window movement reset a live deadline");
    macdrv_frame_scheduler_set_window(&scheduler, main_bounds, false);
    check(!scheduler.deadline.initialized, "background/suspend transition kept a deadline");
    macdrv_frame_configuration_changed();
    macdrv_frame_scheduler_set_window(&scheduler, main_bounds, true);
    check(scheduler.configuration_generation != generation,
          "display configuration change did not advance scheduler generation");
}

int main(void)
{
    test_time_conversion();
    test_deadline_policy();
    test_evolving_predictive_phase();
    test_random_deadlines();
    test_long_run_drift();
    test_display_selection();
    test_concurrent_clock_snapshot();
    test_scheduler_state_transitions();

    if (failures)
    {
        fprintf(stderr, "%u macOS frame scheduler test(s) failed.\n", failures);
        return 1;
    }
    puts("macOS frame scheduler timing/state/concurrency tests passed.");
    return 0;
}
