/*
 * macOS display timeline and presentation deadline scheduler
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#ifndef MACDRV_FRAME_SCHEDULER_STANDALONE
#include "config.h"
#endif

#include <CoreGraphics/CoreGraphics.h>
#include <float.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

#include "frame_scheduler.h"

#define MACDRV_FRAME_CLOCK_COUNT 32
#define MACDRV_DEFAULT_FRAME_RATE 60
#define MACDRV_MINIMUM_SAMPLE_RATE 10
#define MACDRV_MAXIMUM_SAMPLE_RATE 1000
#define MACDRV_DISPLAY_REFRESH_SECONDS 1
#define MACDRV_DISCONTINUITY_SECONDS 0.250
#define MACDRV_MAXIMUM_PERIOD_SECONDS 1.0

struct macdrv_frame_clock
{
    uint64_t sequence;
    uint64_t host_time;
    uint64_t previous_host_time;
    uint64_t period;
    uint64_t generation;
    CGDirectDisplayID display_id;
    unsigned int refs;
};

static pthread_mutex_t frame_clock_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct macdrv_frame_clock frame_clocks[MACDRV_FRAME_CLOCK_COUNT];
static uint64_t frame_configuration_generation = 1;
static mach_timebase_info_data_t frame_timebase;
static pthread_once_t frame_timebase_once = PTHREAD_ONCE_INIT;

static void frame_timebase_initialize(void)
{
    if (mach_timebase_info(&frame_timebase) != KERN_SUCCESS ||
        !frame_timebase.numer || !frame_timebase.denom)
    {
        frame_timebase.numer = 1;
        frame_timebase.denom = 1;
    }
}

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
    return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

static uint64_t saturating_multiply(uint64_t left, uint64_t right)
{
    if (!left || !right) return 0;
    return left > UINT64_MAX / right ? UINT64_MAX : left * right;
}

static uint64_t saturating_multiply_divide(uint64_t value, uint64_t multiplier,
                                           uint64_t divisor, bool round_up)
{
    __uint128_t product, result;

    if (!divisor) return UINT64_MAX;
    product = (__uint128_t)value * multiplier;
    if (round_up && product) product += divisor - 1;
    result = product / divisor;
    return result > UINT64_MAX ? UINT64_MAX : (uint64_t)result;
}

uint64_t macdrv_frame_time_now(void)
{
    return mach_absolute_time();
}

uint64_t macdrv_frame_ticks_from_seconds(double seconds)
{
    long double nanoseconds, ticks;

    if (!(seconds > 0.0) || !isfinite(seconds)) return 0;
    pthread_once(&frame_timebase_once, frame_timebase_initialize);
    nanoseconds = (long double)seconds * 1000000000.0L;
    ticks = nanoseconds * frame_timebase.denom / frame_timebase.numer;
    if (ticks >= (long double)UINT64_MAX) return UINT64_MAX;
    return ticks < 1.0L ? 1 : (uint64_t)ceill(ticks);
}

uint64_t macdrv_frame_ticks_from_rate(unsigned int rate)
{
    uint64_t denominator;

    if (!rate) return 0;
    pthread_once(&frame_timebase_once, frame_timebase_initialize);
    denominator = saturating_multiply(frame_timebase.numer, rate);
    return saturating_multiply_divide(1000000000u, frame_timebase.denom,
                                      denominator, true);
}

double macdrv_frame_seconds_from_ticks(uint64_t ticks)
{
    long double seconds;

    pthread_once(&frame_timebase_once, frame_timebase_initialize);
    seconds = (long double)ticks * frame_timebase.numer /
              ((long double)frame_timebase.denom * 1000000000.0L);
    return seconds > DBL_MAX ? DBL_MAX : (double)seconds;
}

static void frame_clock_write_begin(struct macdrv_frame_clock *clock)
{
    /* Register/reset/unregister callers stop their CVDisplayLink first.  The
     * only concurrent writer while it runs is that link's own serial callback. */
    __atomic_add_fetch(&clock->sequence, 1, __ATOMIC_ACQ_REL);
}

static void frame_clock_write_end(struct macdrv_frame_clock *clock)
{
    __atomic_add_fetch(&clock->sequence, 1, __ATOMIC_RELEASE);
}

static uint64_t current_configuration_generation(void)
{
    return __atomic_load_n(&frame_configuration_generation, __ATOMIC_ACQUIRE);
}

struct macdrv_frame_clock *macdrv_frame_clock_register(CGDirectDisplayID display_id,
                                                       uint64_t fallback_period)
{
    struct macdrv_frame_clock *free_clock = NULL, *clock = NULL;
    unsigned int i;

    if (!display_id) return NULL;
    if (!fallback_period) fallback_period = macdrv_frame_ticks_from_rate(MACDRV_DEFAULT_FRAME_RATE);

    pthread_mutex_lock(&frame_clock_mutex);
    for (i = 0; i < MACDRV_FRAME_CLOCK_COUNT; ++i)
    {
        if (frame_clocks[i].refs && frame_clocks[i].display_id == display_id)
        {
            clock = &frame_clocks[i];
            break;
        }
        if (!frame_clocks[i].refs && !free_clock) free_clock = &frame_clocks[i];
    }
    if (!clock && (clock = free_clock))
    {
        frame_clock_write_begin(clock);
        __atomic_store_n(&clock->display_id, display_id, __ATOMIC_RELAXED);
        __atomic_store_n(&clock->host_time, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&clock->previous_host_time, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&clock->period, fallback_period, __ATOMIC_RELAXED);
        __atomic_store_n(&clock->generation, current_configuration_generation(), __ATOMIC_RELAXED);
        frame_clock_write_end(clock);
    }
    if (clock) ++clock->refs;
    pthread_mutex_unlock(&frame_clock_mutex);
    return clock;
}

void macdrv_frame_clock_unregister(struct macdrv_frame_clock *clock)
{
    if (!clock) return;
    pthread_mutex_lock(&frame_clock_mutex);
    if (clock->refs && !--clock->refs)
    {
        macdrv_frame_configuration_changed();
        frame_clock_write_begin(clock);
        __atomic_store_n(&clock->display_id, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&clock->host_time, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&clock->previous_host_time, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&clock->period, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&clock->generation, current_configuration_generation(), __ATOMIC_RELAXED);
        frame_clock_write_end(clock);
    }
    pthread_mutex_unlock(&frame_clock_mutex);
}

void macdrv_frame_clock_reset(struct macdrv_frame_clock *clock, uint64_t fallback_period)
{
    if (!clock) return;
    if (!fallback_period) fallback_period = macdrv_frame_ticks_from_rate(MACDRV_DEFAULT_FRAME_RATE);
    frame_clock_write_begin(clock);
    __atomic_store_n(&clock->host_time, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&clock->previous_host_time, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&clock->period, fallback_period, __ATOMIC_RELAXED);
    __atomic_store_n(&clock->generation, current_configuration_generation(), __ATOMIC_RELAXED);
    frame_clock_write_end(clock);
}

void macdrv_frame_clock_publish(struct macdrv_frame_clock *clock, uint64_t output_host_time,
                                uint64_t refresh_period)
{
    const uint64_t minimum_period = macdrv_frame_ticks_from_rate(MACDRV_MAXIMUM_SAMPLE_RATE);
    const uint64_t maximum_period = macdrv_frame_ticks_from_rate(MACDRV_MINIMUM_SAMPLE_RATE);
    uint64_t sample_period = refresh_period;

    if (!clock || !output_host_time) return;
    {
        uint64_t previous = __atomic_load_n(&clock->previous_host_time, __ATOMIC_RELAXED);

        if (previous && output_host_time <= previous) return;
        if (!sample_period && previous)
            sample_period = output_host_time - previous;
    }

    frame_clock_write_begin(clock);
    if (sample_period >= minimum_period && sample_period <= maximum_period)
        __atomic_store_n(&clock->period, sample_period, __ATOMIC_RELAXED);
    __atomic_store_n(&clock->previous_host_time, output_host_time, __ATOMIC_RELAXED);
    __atomic_store_n(&clock->host_time, output_host_time, __ATOMIC_RELAXED);
    __atomic_store_n(&clock->generation, current_configuration_generation(), __ATOMIC_RELAXED);
    frame_clock_write_end(clock);
}

bool macdrv_frame_clock_snapshot(const struct macdrv_frame_clock *clock,
                                 struct macdrv_frame_timing *timing)
{
    uint64_t sequence;

    memset(timing, 0, sizeof(*timing));
    if (!clock) return false;
    for (;;)
    {
        sequence = __atomic_load_n(&clock->sequence, __ATOMIC_ACQUIRE);
        if (sequence & 1) continue;
        timing->host_time = __atomic_load_n(&clock->host_time, __ATOMIC_RELAXED);
        timing->period = __atomic_load_n(&clock->period, __ATOMIC_RELAXED);
        timing->generation = __atomic_load_n(&clock->generation, __ATOMIC_RELAXED);
        timing->display_id = __atomic_load_n(&clock->display_id, __ATOMIC_RELAXED);
        if (sequence == __atomic_load_n(&clock->sequence, __ATOMIC_ACQUIRE)) break;
    }

    /* Unregister publishes a zero period through the same sequence.  Slots
     * have process lifetime, so the display ID and period identify a live
     * generation without a separate refcount read (and data race). */
    timing->valid = timing->display_id && timing->period;
    return timing->valid;
}

void macdrv_frame_configuration_changed(void)
{
    uint64_t old_generation, new_generation;

    old_generation = __atomic_load_n(&frame_configuration_generation, __ATOMIC_ACQUIRE);
    do
    {
        new_generation = old_generation == UINT64_MAX ? 1 : old_generation + 1;
    } while (!__atomic_compare_exchange_n(&frame_configuration_generation,
             &old_generation, new_generation, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

static struct macdrv_frame_clock *find_frame_clock(CGDirectDisplayID display_id)
{
    struct macdrv_frame_clock *clock = NULL;
    unsigned int i;

    pthread_mutex_lock(&frame_clock_mutex);
    for (i = 0; i < MACDRV_FRAME_CLOCK_COUNT; ++i)
    {
        if (frame_clocks[i].refs && frame_clocks[i].display_id == display_id)
        {
            clock = &frame_clocks[i];
            break;
        }
    }
    pthread_mutex_unlock(&frame_clock_mutex);
    return clock;
}

static double rect_intersection_area(CGRect left, CGRect right)
{
    CGRect intersection = CGRectIntersection(left, right);

    if (CGRectIsNull(intersection) || CGRectIsEmpty(intersection)) return 0.0;
    return intersection.size.width * intersection.size.height;
}

CGDirectDisplayID macdrv_frame_select_display(CGRect rect, CGDirectDisplayID current,
                                              CGDirectDisplayID main_display,
                                              const CGDirectDisplayID *displays,
                                              const CGRect *bounds, unsigned int count)
{
    CGDirectDisplayID selected = 0;
    bool current_online = false;
    double best_area = -1.0;
    unsigned int i;

    for (i = 0; i < count; ++i)
    {
        double area = rect_intersection_area(rect, bounds[i]);

        if (displays[i] == current) current_online = true;
        if (area > best_area ||
            (area == best_area && displays[i] == current && selected != current) ||
            (area == best_area && selected != current && displays[i] == main_display))
        {
            selected = displays[i];
            best_area = area;
        }
    }
    if (best_area <= 0.0) selected = current_online ? current : main_display;
    if (!selected) selected = main_display;
    return selected;
}

static CGDirectDisplayID display_for_rect(CGRect rect, CGDirectDisplayID current)
{
    CGDirectDisplayID displays[MACDRV_FRAME_CLOCK_COUNT];
    CGRect bounds[MACDRV_FRAME_CLOCK_COUNT];
    uint32_t count = 0, i;

    if (CGGetOnlineDisplayList(MACDRV_FRAME_CLOCK_COUNT, displays, &count) != kCGErrorSuccess)
        count = 0;
    for (i = 0; i < count; ++i) bounds[i] = CGDisplayBounds(displays[i]);
    return macdrv_frame_select_display(rect, current, CGMainDisplayID(), displays, bounds, count);
}

static uint64_t display_mode_period(CGDirectDisplayID display_id)
{
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display_id);
    double rate = mode ? CGDisplayModeGetRefreshRate(mode) : 0.0;
    uint64_t period;

    if (mode) CGDisplayModeRelease(mode);
    /* Variable-refresh internal panels commonly report zero here.  Core Video
     * samples supersede this value as soon as a host window owns a link. */
    period = rate > 0.0 && isfinite(rate) ? macdrv_frame_ticks_from_seconds(1.0 / rate) : 0;
    return period ? period : macdrv_frame_ticks_from_rate(MACDRV_DEFAULT_FRAME_RATE);
}

void macdrv_frame_scheduler_init(struct macdrv_frame_scheduler *scheduler)
{
    memset(scheduler, 0, sizeof(*scheduler));
}

void macdrv_frame_scheduler_reset(struct macdrv_frame_scheduler *scheduler)
{
    memset(&scheduler->deadline, 0, sizeof(scheduler->deadline));
}

void macdrv_frame_scheduler_set_window(struct macdrv_frame_scheduler *scheduler,
                                       CGRect window_rect, bool active)
{
    const uint64_t now = macdrv_frame_time_now();
    const uint64_t refresh_period = macdrv_frame_ticks_from_seconds(MACDRV_DISPLAY_REFRESH_SECONDS);
    const uint64_t generation = current_configuration_generation();
    CGDirectDisplayID display_id;
    struct macdrv_frame_clock *clock;
    uint64_t fallback_period;
    bool configuration_changed, display_changed, geometry_changed, periodic_refresh;

    geometry_changed = !scheduler->has_window_rect ||
                       !CGRectEqualToRect(scheduler->window_rect, window_rect);
    configuration_changed = scheduler->configuration_generation != generation;
    periodic_refresh = !scheduler->last_display_refresh ||
                       now < scheduler->last_display_refresh ||
                       now - scheduler->last_display_refresh >= refresh_period;

    if (scheduler->active != active)
    {
        scheduler->active = active;
        macdrv_frame_scheduler_reset(scheduler);
    }
    scheduler->window_rect = window_rect;
    scheduler->has_window_rect = true;
    if (!active) return;
    if (!geometry_changed && !configuration_changed && !periodic_refresh) return;

    display_id = display_for_rect(window_rect, scheduler->display_id);
    display_changed = display_id != scheduler->display_id;
    if (display_changed || configuration_changed || periodic_refresh ||
        !scheduler->fallback_period)
    {
        clock = find_frame_clock(display_id);
        fallback_period = display_mode_period(display_id);
        if (display_changed || configuration_changed || clock != scheduler->clock ||
            fallback_period != scheduler->fallback_period)
            macdrv_frame_scheduler_reset(scheduler);
        scheduler->clock = clock;
        scheduler->fallback_period = fallback_period;
        scheduler->last_display_refresh = now;
    }
    scheduler->display_id = display_id;
    scheduler->configuration_generation = generation;
}

static uint64_t first_phase_after(uint64_t now, uint64_t phase, uint64_t period)
{
    uint64_t steps;

    if (phase > now) return phase;
    steps = (now - phase) / period;
    if (steps != UINT64_MAX) ++steps;
    return saturating_add(phase, saturating_multiply(steps, period));
}

uint64_t macdrv_frame_deadline_reserve(struct macdrv_frame_deadline *deadline,
                                       uint64_t now, uint64_t phase, uint64_t period,
                                       uint64_t generation, bool active)
{
    uint64_t minimum_discontinuity =
             macdrv_frame_ticks_from_seconds(MACDRV_DISCONTINUITY_SECONDS);
    uint64_t maximum_period =
             macdrv_frame_ticks_from_seconds(MACDRV_MAXIMUM_PERIOD_SECONDS);
    uint64_t discontinuity_limit, wait_until = 0;
    bool discontinuity;

    if (!active || !period)
    {
        memset(deadline, 0, sizeof(*deadline));
        return 0;
    }
    /* Conversion guarantees a nonzero tick for positive finite input.  Keep
     * the pure entry point defensive if a future platform clock violates that
     * assumption, so untrusted interval input can never reach a zero divisor. */
    if (!minimum_discontinuity) minimum_discontinuity = 1;
    if (!maximum_period) maximum_period = UINT64_MAX;

    /* A registry-controlled cap or application swap interval must never turn
     * into an unbounded sleep.  One second still represents every supported
     * ForeignSurfaceMaxFrameRate value (1..1000) and bounds teardown latency
     * for hostile or corrupt swap-interval input. */
    if (period > maximum_period) period = maximum_period;
    discontinuity_limit = saturating_multiply(period, 4);
    if (discontinuity_limit < minimum_discontinuity)
        discontinuity_limit = minimum_discontinuity;

    discontinuity = !deadline->initialized || deadline->generation != generation ||
                    now < deadline->last_now ||
                    (deadline->last_now && now - deadline->last_now > discontinuity_limit);
    if (discontinuity)
    {
        if (!phase || (phase > now && phase - now > discontinuity_limit) ||
            (phase <= now && now - phase > discontinuity_limit))
            phase = now;
        deadline->next = first_phase_after(now, phase, period);
    }
    else if (deadline->next > now && deadline->next - now <= maximum_period)
    {
        /* Advance from the reserved deadline, not from callback or completion
         * time.  Evolving predictive CV timestamps and incommensurate FPS caps
         * therefore cannot shorten a later interval or accumulate drift. */
        wait_until = deadline->next;
        deadline->next = saturating_add(wait_until, period);
    }
    else
    {
        /* One late present is allowed immediately.  Restarting from now keeps
         * the following interval whole and prevents a catch-up burst. */
        deadline->next = saturating_add(now, period);
    }
    if (deadline->next <= (wait_until ? wait_until : now) ||
        deadline->next - (wait_until ? wait_until : now) > maximum_period)
        deadline->next = saturating_add(wait_until ? wait_until : now, maximum_period);
    deadline->last_now = wait_until ? wait_until : now;
    deadline->generation = generation;
    deadline->initialized = true;
    return wait_until;
}

void macdrv_frame_scheduler_wait(struct macdrv_frame_scheduler *scheduler,
                                 unsigned int swap_interval, unsigned int maximum_frame_rate)
{
    struct macdrv_frame_timing timing = {0};
    uint64_t now, period = 0, cap_period, phase = 0, wait_until;
    bool display_anchored = false;

    now = macdrv_frame_time_now();
    if (scheduler->clock) macdrv_frame_clock_snapshot(scheduler->clock, &timing);
    if (timing.valid && timing.display_id == scheduler->display_id &&
        timing.generation == scheduler->configuration_generation &&
        ((timing.host_time <= now && now - timing.host_time <=
          macdrv_frame_ticks_from_seconds(MACDRV_DISCONTINUITY_SECONDS)) ||
         (timing.host_time > now && timing.host_time - now <=
          macdrv_frame_ticks_from_seconds(MACDRV_DISCONTINUITY_SECONDS))))
        period = timing.period;
    else
    {
        timing.host_time = 0;
        timing.generation = scheduler->configuration_generation;
        period = scheduler->fallback_period;
    }

    if (swap_interval)
    {
        period = saturating_multiply(period, swap_interval);
        display_anchored = true;
    }
    else
        period = 0;
    cap_period = macdrv_frame_ticks_from_rate(maximum_frame_rate);
    if (cap_period > period)
    {
        period = cap_period;
        display_anchored = false;
    }
    if (display_anchored) phase = timing.host_time;

    wait_until = macdrv_frame_deadline_reserve(&scheduler->deadline, now,
            phase, period, timing.generation, scheduler->active && period);
    if (wait_until && mach_wait_until(wait_until) != KERN_SUCCESS)
        macdrv_frame_scheduler_reset(scheduler);
}
