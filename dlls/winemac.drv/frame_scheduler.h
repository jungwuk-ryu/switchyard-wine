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

#ifndef __WINE_MACDRV_FRAME_SCHEDULER_H
#define __WINE_MACDRV_FRAME_SCHEDULER_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>

struct macdrv_frame_clock;

struct macdrv_frame_timing
{
    uint64_t host_time;
    uint64_t period;
    uint64_t generation;
    CGDirectDisplayID display_id;
    bool valid;
};

struct macdrv_frame_deadline
{
    uint64_t next;
    uint64_t last_now;
    uint64_t generation;
    bool initialized;
};

struct macdrv_frame_scheduler
{
    struct macdrv_frame_deadline deadline;
    struct macdrv_frame_clock *clock;
    CGRect window_rect;
    CGDirectDisplayID display_id;
    uint64_t fallback_period;
    uint64_t configuration_generation;
    uint64_t last_display_refresh;
    bool active;
    bool has_window_rect;
};

uint64_t macdrv_frame_time_now(void);
uint64_t macdrv_frame_ticks_from_seconds(double seconds);
uint64_t macdrv_frame_ticks_from_rate(unsigned int rate);
double macdrv_frame_seconds_from_ticks(uint64_t ticks);

struct macdrv_frame_clock *macdrv_frame_clock_register(CGDirectDisplayID display_id,
                                                       uint64_t fallback_period);
void macdrv_frame_clock_unregister(struct macdrv_frame_clock *clock);
void macdrv_frame_clock_reset(struct macdrv_frame_clock *clock, uint64_t fallback_period);
void macdrv_frame_clock_publish(struct macdrv_frame_clock *clock, uint64_t output_host_time,
                                uint64_t refresh_period);
bool macdrv_frame_clock_snapshot(const struct macdrv_frame_clock *clock,
                                 struct macdrv_frame_timing *timing);
void macdrv_frame_configuration_changed(void);

CGDirectDisplayID macdrv_frame_select_display(CGRect window_rect,
                                               CGDirectDisplayID current_display,
                                               CGDirectDisplayID main_display,
                                               const CGDirectDisplayID *display_ids,
                                               const CGRect *display_bounds,
                                               unsigned int display_count);

void macdrv_frame_scheduler_init(struct macdrv_frame_scheduler *scheduler);
void macdrv_frame_scheduler_reset(struct macdrv_frame_scheduler *scheduler);
void macdrv_frame_scheduler_set_window(struct macdrv_frame_scheduler *scheduler,
                                       CGRect window_rect, bool active);
void macdrv_frame_scheduler_wait(struct macdrv_frame_scheduler *scheduler,
                                 unsigned int swap_interval, unsigned int maximum_frame_rate);

/* Pure deadline reservation entry point used by the platform wrapper and
 * focused timing tests.  A zero return means that this producer may present
 * immediately.  The state always reserves the first strictly future phase
 * before returning, preventing a late producer from causing a catch-up burst. */
uint64_t macdrv_frame_deadline_reserve(struct macdrv_frame_deadline *deadline,
                                       uint64_t now, uint64_t phase, uint64_t period,
                                       uint64_t generation, bool active);

#endif /* __WINE_MACDRV_FRAME_SCHEDULER_H */
