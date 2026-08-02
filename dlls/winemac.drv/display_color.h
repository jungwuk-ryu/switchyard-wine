/*
 * macOS display colour information helpers
 *
 * Copyright 2026 Switchyard project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_MACDRV_DISPLAY_COLOR_H
#define __WINE_MACDRV_DISPLAY_COLOR_H

#include <stddef.h>
#include <stdint.h>

enum macdrv_display_color_valid
{
    MACDRV_DISPLAY_COLOR_BITS_PER_COLOR            = 0x00000001,
    MACDRV_DISPLAY_COLOR_PRIMARIES                  = 0x00000002,
    MACDRV_DISPLAY_COLOR_WHITE_POINT                = 0x00000004,
    MACDRV_DISPLAY_COLOR_MIN_LUMINANCE              = 0x00000008,
    MACDRV_DISPLAY_COLOR_MAX_LUMINANCE              = 0x00000010,
    MACDRV_DISPLAY_COLOR_MAX_FULL_FRAME_LUMINANCE   = 0x00000020,
    MACDRV_DISPLAY_COLOR_CURRENT_EDR_HEADROOM        = 0x00000040,
    MACDRV_DISPLAY_COLOR_POTENTIAL_EDR_HEADROOM      = 0x00000080,
    MACDRV_DISPLAY_COLOR_REFERENCE_EDR_HEADROOM      = 0x00000100,
    MACDRV_DISPLAY_COLOR_COLOR_SPACE                 = 0x00000200,
};

enum macdrv_display_color_capability
{
    MACDRV_DISPLAY_COLOR_WIDE_GAMUT = 0x00000001,
    MACDRV_DISPLAY_COLOR_PQ         = 0x00000002,
    MACDRV_DISPLAY_COLOR_HLG        = 0x00000004,
};

enum macdrv_display_color_space
{
    MACDRV_DISPLAY_COLOR_SPACE_UNKNOWN,
    MACDRV_DISPLAY_COLOR_SPACE_SRGB,
    MACDRV_DISPLAY_COLOR_SPACE_BT709,
    MACDRV_DISPLAY_COLOR_SPACE_EXTENDED_LINEAR_SRGB,
    MACDRV_DISPLAY_COLOR_SPACE_BT2020,
    MACDRV_DISPLAY_COLOR_SPACE_BT2100_PQ,
    MACDRV_DISPLAY_COLOR_SPACE_BT2100_HLG,
};

/*
 * Chromaticities use CIE 1931 xy coordinates. Luminance values are cd/m2.
 * EDR headroom values are ratios relative to the platform's SDR reference.
 * Every scalar or group is usable only when its corresponding valid bit is set.
 */
struct macdrv_display_color_info
{
    uint32_t valid;
    uint32_t capabilities;
    uint32_t bits_per_color;
    enum macdrv_display_color_space color_space;
    float red_x, red_y;
    float green_x, green_y;
    float blue_x, blue_y;
    float white_x, white_y;
    float min_luminance;
    float max_luminance;
    float max_full_frame_luminance;
    float current_edr_headroom;
    float potential_edr_headroom;
    float reference_edr_headroom;
};

struct macdrv_hdr10_metadata
{
    /* Chromaticities use 1 / 50000; mastering luminance uses 1 / 10000 nit. */
    uint16_t red_primary[2];
    uint16_t green_primary[2];
    uint16_t blue_primary[2];
    uint16_t white_point[2];
    uint32_t max_mastering_luminance;
    uint32_t min_mastering_luminance;
    uint16_t max_content_light_level;
    uint16_t max_frame_average_light_level;
};

void macdrv_parse_edid_color_info(const uint8_t *data, size_t size,
        struct macdrv_display_color_info *info);
void macdrv_parse_icc_color_info(const uint8_t *data, size_t size,
        struct macdrv_display_color_info *info);
void macdrv_merge_display_color_info(struct macdrv_display_color_info *dst,
        const struct macdrv_display_color_info *src);
int macdrv_hdr10_metadata_is_valid(const struct macdrv_hdr10_metadata *metadata);
void macdrv_serialize_hdr10_metadata(const struct macdrv_hdr10_metadata *metadata,
        uint8_t display_info[24], uint8_t content_info[4]);

#endif /* __WINE_MACDRV_DISPLAY_COLOR_H */
