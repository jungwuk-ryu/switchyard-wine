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

#ifndef MACDRV_DISPLAY_COLOR_TEST
#if 0
#pragma makedep unix
#endif
#include "config.h"
#endif

#include <math.h>
#include <string.h>

#include "display_color.h"

#define TAG(a,b,c,d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))
#define ICC_MAX_TAGS 4096

struct xyz
{
    double x, y, z;
};

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
            ((uint32_t)data[2] << 8) | data[3];
}

static void write_be16(uint8_t *data, uint16_t value)
{
    data[0] = value >> 8;
    data[1] = value;
}

static void write_be32(uint8_t *data, uint32_t value)
{
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
}

static int hdr10_chromaticity_is_valid(const uint16_t chromaticity[2])
{
    return chromaticity[0] <= 50000 && chromaticity[1] <= 50000 &&
            (uint32_t)chromaticity[0] + chromaticity[1] <= 50000;
}

int macdrv_hdr10_metadata_is_valid(const struct macdrv_hdr10_metadata *metadata)
{
    if (!metadata || !hdr10_chromaticity_is_valid(metadata->red_primary) ||
            !hdr10_chromaticity_is_valid(metadata->green_primary) ||
            !hdr10_chromaticity_is_valid(metadata->blue_primary) ||
            !hdr10_chromaticity_is_valid(metadata->white_point))
        return 0;

    /* Zero is the standards-defined representation for an unknown field.
     * Reject only impossible PQ values and relationships whose operands are
     * both known. */
    if (metadata->max_mastering_luminance > 10000u * 10000u ||
            (metadata->max_mastering_luminance && metadata->min_mastering_luminance >
            metadata->max_mastering_luminance))
        return 0;
    if (metadata->max_content_light_level > 10000 ||
            metadata->max_frame_average_light_level > 10000 ||
            (metadata->max_content_light_level && metadata->max_frame_average_light_level >
            metadata->max_content_light_level))
        return 0;

    return 1;
}

void macdrv_serialize_hdr10_metadata(const struct macdrv_hdr10_metadata *metadata,
        uint8_t display_info[24], uint8_t content_info[4])
{
    /* HEVC mastering_display_colour_volume() orders primaries G, B, R. */
    write_be16(display_info + 0, metadata->green_primary[0]);
    write_be16(display_info + 2, metadata->green_primary[1]);
    write_be16(display_info + 4, metadata->blue_primary[0]);
    write_be16(display_info + 6, metadata->blue_primary[1]);
    write_be16(display_info + 8, metadata->red_primary[0]);
    write_be16(display_info + 10, metadata->red_primary[1]);
    write_be16(display_info + 12, metadata->white_point[0]);
    write_be16(display_info + 14, metadata->white_point[1]);
    write_be32(display_info + 16, metadata->max_mastering_luminance);
    write_be32(display_info + 20, metadata->min_mastering_luminance);
    write_be16(content_info + 0, metadata->max_content_light_level);
    write_be16(content_info + 2, metadata->max_frame_average_light_level);
}

static double read_s15fixed16(const uint8_t *data)
{
    return (double)(int32_t)read_be32(data) / 65536.0;
}

static int edid_block_valid(const uint8_t *data)
{
    unsigned int sum = 0;
    unsigned int i;

    for (i = 0; i < 128; ++i) sum += data[i];
    return !(sum & 0xff);
}

static int chromaticity_valid(double x, double y)
{
    return isfinite(x) && isfinite(y) && x >= 0.0 && x <= 1.0 && y >= 0.0 && y <= 1.0 &&
            x + y > 0.0 && x + y <= 1.0;
}

static int xyz_to_xy(const struct xyz *xyz, float *x, float *y)
{
    double sum = xyz->x + xyz->y + xyz->z;
    double cx, cy;

    if (!isfinite(sum) || fabs(sum) < 1.0e-12) return 0;
    cx = xyz->x / sum;
    cy = xyz->y / sum;
    if (!chromaticity_valid(cx, cy)) return 0;
    *x = cx;
    *y = cy;
    return 1;
}

static void parse_edid_chromaticities(const uint8_t *data, struct macdrv_display_color_info *info)
{
    unsigned int rx, ry, gx, gy, bx, by, wx, wy;

    rx = ((unsigned int)data[27] << 2) | ((data[25] >> 6) & 3);
    ry = ((unsigned int)data[28] << 2) | ((data[25] >> 4) & 3);
    gx = ((unsigned int)data[29] << 2) | ((data[25] >> 2) & 3);
    gy = ((unsigned int)data[30] << 2) | (data[25] & 3);
    bx = ((unsigned int)data[31] << 2) | ((data[26] >> 6) & 3);
    by = ((unsigned int)data[32] << 2) | ((data[26] >> 4) & 3);
    wx = ((unsigned int)data[33] << 2) | ((data[26] >> 2) & 3);
    wy = ((unsigned int)data[34] << 2) | (data[26] & 3);

    info->red_x = rx / 1024.0;
    info->red_y = ry / 1024.0;
    info->green_x = gx / 1024.0;
    info->green_y = gy / 1024.0;
    info->blue_x = bx / 1024.0;
    info->blue_y = by / 1024.0;
    info->white_x = wx / 1024.0;
    info->white_y = wy / 1024.0;

    if (chromaticity_valid(info->red_x, info->red_y) &&
            chromaticity_valid(info->green_x, info->green_y) &&
            chromaticity_valid(info->blue_x, info->blue_y))
        info->valid |= MACDRV_DISPLAY_COLOR_PRIMARIES;
    if (chromaticity_valid(info->white_x, info->white_y))
        info->valid |= MACDRV_DISPLAY_COLOR_WHITE_POINT;
}

static double cta_luminance(unsigned int code)
{
    /* CTA-861 HDR Static Metadata Data Block: 50 * 2^(CV / 32) cd/m2. */
    return 50.0 * exp2(code / 32.0);
}

static void parse_cta_hdr_block(const uint8_t *data, size_t length,
        struct macdrv_display_color_info *info)
{
    double max_luminance, value;

    /* Extended tag, EOTFs, static metadata descriptor flags. */
    if (length < 3 || data[0] != 0x06) return;
    if (data[1] & 0x04) info->capabilities |= MACDRV_DISPLAY_COLOR_PQ;
    if (data[1] & 0x08) info->capabilities |= MACDRV_DISPLAY_COLOR_HLG;

    if (length < 4 || !(data[2] & 0x01)) return;
    /* CTA-861 reserves code value 0 to mean that the field is unavailable. */
    max_luminance = cta_luminance(data[3]);
    if (data[3] && isfinite(max_luminance))
    {
        info->max_luminance = max_luminance;
        info->valid |= MACDRV_DISPLAY_COLOR_MAX_LUMINANCE;
    }

    if (length >= 5)
    {
        value = cta_luminance(data[4]);
        if (data[4] && isfinite(value) &&
                (!(info->valid & MACDRV_DISPLAY_COLOR_MAX_LUMINANCE) || value <= max_luminance))
        {
            info->max_full_frame_luminance = value;
            info->valid |= MACDRV_DISPLAY_COLOR_MAX_FULL_FRAME_LUMINANCE;
        }
    }

    if (length >= 6 && data[5] && (info->valid & MACDRV_DISPLAY_COLOR_MAX_LUMINANCE))
    {
        value = max_luminance * ((double)data[5] / 255.0) * ((double)data[5] / 255.0) / 100.0;
        if (isfinite(value) && value <= max_luminance)
        {
            info->min_luminance = value;
            info->valid |= MACDRV_DISPLAY_COLOR_MIN_LUMINANCE;
        }
    }
}

static void parse_cta_extension(const uint8_t *data, struct macdrv_display_color_info *info)
{
    size_t end, offset = 4;

    if (data[0] != 0x02 || data[1] < 3) return;
    end = data[2];
    if (!end) return;
    if (end < 4 || end > 127) return;

    while (offset < end)
    {
        size_t length = data[offset] & 0x1f;
        unsigned int tag = data[offset] >> 5;

        ++offset;
        if (length > end - offset) return;
        if (tag == 7) parse_cta_hdr_block(data + offset, length, info);
        offset += length;
    }
}

void macdrv_parse_edid_color_info(const uint8_t *data, size_t size,
        struct macdrv_display_color_info *info)
{
    static const uint8_t header[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    size_t blocks, i;

    memset(info, 0, sizeof(*info));
    if (!data || size < 128 || memcmp(data, header, sizeof(header)) || !edid_block_valid(data) || data[18] != 1)
        return;

    parse_edid_chromaticities(data, info);
    blocks = (size_t)data[126] + 1;
    if (blocks > size / 128) blocks = size / 128;
    for (i = 1; i < blocks; ++i)
    {
        const uint8_t *extension = data + i * 128;

        if (edid_block_valid(extension)) parse_cta_extension(extension, info);
    }
}

static int find_icc_tag(const uint8_t *data, size_t profile_size, uint32_t signature,
        const uint8_t **tag, size_t *tag_size)
{
    uint32_t count, i;

    if (profile_size < 132) return 0;
    count = read_be32(data + 128);
    if (count > ICC_MAX_TAGS || count > (profile_size - 132) / 12) return 0;

    for (i = 0; i < count; ++i)
    {
        const uint8_t *record = data + 132 + (size_t)i * 12;
        uint32_t offset, size;

        if (read_be32(record) != signature) continue;
        offset = read_be32(record + 4);
        size = read_be32(record + 8);
        if (offset > profile_size || size > profile_size - offset) return 0;
        *tag = data + offset;
        *tag_size = size;
        return 1;
    }
    return 0;
}

static int read_icc_xyz(const uint8_t *tag, size_t size, struct xyz *xyz)
{
    if (size < 20 || read_be32(tag) != TAG('X','Y','Z',' ')) return 0;
    xyz->x = read_s15fixed16(tag + 8);
    xyz->y = read_s15fixed16(tag + 12);
    xyz->z = read_s15fixed16(tag + 16);
    return isfinite(xyz->x) && isfinite(xyz->y) && isfinite(xyz->z);
}

static int read_icc_matrix(const uint8_t *tag, size_t size, double matrix[9])
{
    unsigned int i;

    if (size < 44 || read_be32(tag) != TAG('s','f','3','2')) return 0;
    for (i = 0; i < 9; ++i) matrix[i] = read_s15fixed16(tag + 8 + i * 4);
    return 1;
}

static int invert_matrix(const double m[9], double inverse[9])
{
    double determinant;

    determinant = m[0] * (m[4] * m[8] - m[5] * m[7]) -
            m[1] * (m[3] * m[8] - m[5] * m[6]) +
            m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (!isfinite(determinant) || fabs(determinant) < 1.0e-12) return 0;

    inverse[0] =  (m[4] * m[8] - m[5] * m[7]) / determinant;
    inverse[1] = -(m[1] * m[8] - m[2] * m[7]) / determinant;
    inverse[2] =  (m[1] * m[5] - m[2] * m[4]) / determinant;
    inverse[3] = -(m[3] * m[8] - m[5] * m[6]) / determinant;
    inverse[4] =  (m[0] * m[8] - m[2] * m[6]) / determinant;
    inverse[5] = -(m[0] * m[5] - m[2] * m[3]) / determinant;
    inverse[6] =  (m[3] * m[7] - m[4] * m[6]) / determinant;
    inverse[7] = -(m[0] * m[7] - m[1] * m[6]) / determinant;
    inverse[8] =  (m[0] * m[4] - m[1] * m[3]) / determinant;
    return 1;
}

static void transform_xyz(const double matrix[9], struct xyz *xyz)
{
    struct xyz result;

    result.x = matrix[0] * xyz->x + matrix[1] * xyz->y + matrix[2] * xyz->z;
    result.y = matrix[3] * xyz->x + matrix[4] * xyz->y + matrix[5] * xyz->z;
    result.z = matrix[6] * xyz->x + matrix[7] * xyz->y + matrix[8] * xyz->z;
    *xyz = result;
}

static int is_d50(const struct xyz *white)
{
    float x, y;

    if (!xyz_to_xy(white, &x, &y)) return 0;
    return fabs(x - 0.3457) < 0.002 && fabs(y - 0.3585) < 0.002;
}

void macdrv_parse_icc_color_info(const uint8_t *data, size_t size,
        struct macdrv_display_color_info *info)
{
    const uint8_t *red_tag, *green_tag, *blue_tag, *white_tag, *luminance_tag, *chad_tag;
    size_t red_size, green_size, blue_size, white_size, luminance_size, chad_size;
    struct xyz red, green, blue, white, luminance;
    double chad[9], inverse[9];
    size_t profile_size;
    int have_primaries, have_white, have_chad;

    memset(info, 0, sizeof(*info));
    if (!data || size < 132 || read_be32(data + 36) != TAG('a','c','s','p') ||
            read_be32(data + 12) != TAG('m','n','t','r') || read_be32(data + 16) != TAG('R','G','B',' ') ||
            read_be32(data + 20) != TAG('X','Y','Z',' '))
        return;
    profile_size = read_be32(data);
    if (profile_size < 132 || profile_size > size) return;

    have_primaries = find_icc_tag(data, profile_size, TAG('r','X','Y','Z'), &red_tag, &red_size) &&
            find_icc_tag(data, profile_size, TAG('g','X','Y','Z'), &green_tag, &green_size) &&
            find_icc_tag(data, profile_size, TAG('b','X','Y','Z'), &blue_tag, &blue_size) &&
            read_icc_xyz(red_tag, red_size, &red) && read_icc_xyz(green_tag, green_size, &green) &&
            read_icc_xyz(blue_tag, blue_size, &blue);
    have_white = find_icc_tag(data, profile_size, TAG('w','t','p','t'), &white_tag, &white_size) &&
            read_icc_xyz(white_tag, white_size, &white);
    have_chad = find_icc_tag(data, profile_size, TAG('c','h','a','d'), &chad_tag, &chad_size) &&
            read_icc_matrix(chad_tag, chad_size, chad) && invert_matrix(chad, inverse);

    /* ICC PCS values are D50-adapted when a chad tag is present. Undo it. */
    if (have_chad)
    {
        if (have_primaries)
        {
            transform_xyz(inverse, &red);
            transform_xyz(inverse, &green);
            transform_xyz(inverse, &blue);
        }
        if (have_white) transform_xyz(inverse, &white);
    }
    /* A D50 PCS white without a chad matrix cannot be converted back to native chromaticities. */
    else if (!have_white || is_d50(&white))
    {
        have_primaries = 0;
        have_white = 0;
    }

    if (have_primaries && xyz_to_xy(&red, &info->red_x, &info->red_y) &&
            xyz_to_xy(&green, &info->green_x, &info->green_y) &&
            xyz_to_xy(&blue, &info->blue_x, &info->blue_y))
        info->valid |= MACDRV_DISPLAY_COLOR_PRIMARIES;
    if (have_white && xyz_to_xy(&white, &info->white_x, &info->white_y))
        info->valid |= MACDRV_DISPLAY_COLOR_WHITE_POINT;

    if (find_icc_tag(data, profile_size, TAG('l','u','m','i'), &luminance_tag, &luminance_size) &&
            read_icc_xyz(luminance_tag, luminance_size, &luminance) && luminance.y > 0.0)
    {
        /* ICC luminanceTag is the display white luminance in cd/m2 (a full-frame white measurement). */
        info->max_full_frame_luminance = luminance.y;
        info->valid |= MACDRV_DISPLAY_COLOR_MAX_FULL_FRAME_LUMINANCE;
    }
}

void macdrv_merge_display_color_info(struct macdrv_display_color_info *dst,
        const struct macdrv_display_color_info *src)
{
    uint32_t copy;

    dst->capabilities |= src->capabilities;

    if ((src->valid & MACDRV_DISPLAY_COLOR_BITS_PER_COLOR) &&
            !(dst->valid & MACDRV_DISPLAY_COLOR_BITS_PER_COLOR))
        dst->bits_per_color = src->bits_per_color;
    if ((src->valid & MACDRV_DISPLAY_COLOR_PRIMARIES) && !(dst->valid & MACDRV_DISPLAY_COLOR_PRIMARIES))
    {
        dst->red_x = src->red_x; dst->red_y = src->red_y;
        dst->green_x = src->green_x; dst->green_y = src->green_y;
        dst->blue_x = src->blue_x; dst->blue_y = src->blue_y;
    }
    if ((src->valid & MACDRV_DISPLAY_COLOR_WHITE_POINT) && !(dst->valid & MACDRV_DISPLAY_COLOR_WHITE_POINT))
    {
        dst->white_x = src->white_x; dst->white_y = src->white_y;
    }
    if ((src->valid & MACDRV_DISPLAY_COLOR_MIN_LUMINANCE) &&
            !(dst->valid & MACDRV_DISPLAY_COLOR_MIN_LUMINANCE))
        dst->min_luminance = src->min_luminance;
    if ((src->valid & MACDRV_DISPLAY_COLOR_MAX_LUMINANCE) &&
            !(dst->valid & MACDRV_DISPLAY_COLOR_MAX_LUMINANCE))
        dst->max_luminance = src->max_luminance;
    if ((src->valid & MACDRV_DISPLAY_COLOR_MAX_FULL_FRAME_LUMINANCE) &&
            !(dst->valid & MACDRV_DISPLAY_COLOR_MAX_FULL_FRAME_LUMINANCE))
        dst->max_full_frame_luminance = src->max_full_frame_luminance;
    if ((src->valid & MACDRV_DISPLAY_COLOR_CURRENT_EDR_HEADROOM) &&
            !(dst->valid & MACDRV_DISPLAY_COLOR_CURRENT_EDR_HEADROOM))
        dst->current_edr_headroom = src->current_edr_headroom;
    if ((src->valid & MACDRV_DISPLAY_COLOR_POTENTIAL_EDR_HEADROOM) &&
            !(dst->valid & MACDRV_DISPLAY_COLOR_POTENTIAL_EDR_HEADROOM))
        dst->potential_edr_headroom = src->potential_edr_headroom;
    if ((src->valid & MACDRV_DISPLAY_COLOR_REFERENCE_EDR_HEADROOM) &&
            !(dst->valid & MACDRV_DISPLAY_COLOR_REFERENCE_EDR_HEADROOM))
        dst->reference_edr_headroom = src->reference_edr_headroom;
    if ((src->valid & MACDRV_DISPLAY_COLOR_COLOR_SPACE) &&
            !(dst->valid & MACDRV_DISPLAY_COLOR_COLOR_SPACE))
        dst->color_space = src->color_space;

    copy = src->valid & ~dst->valid;
    dst->valid |= copy;
}
