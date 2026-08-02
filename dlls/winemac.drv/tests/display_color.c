/*
 * macOS display colour parser tests
 *
 * Copyright 2026 Switchyard project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MACDRV_NATIVE_TEST
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define ok(condition, ...) do { if (!(condition)) { fprintf(stderr, __VA_ARGS__); exit(1); } } while (0)
#define START_TEST(name) int main(void)
#else
#include "wine/test.h"
#endif

/* The parser is deliberately platform-independent and is tested directly. */
#define MACDRV_DISPLAY_COLOR_TEST
#include "../display_color.c"

static void test_write_be32(uint8_t *data, uint32_t value)
{
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
}

static void write_fixed(uint8_t *data, double value)
{
    int32_t fixed = value * 65536.0;
    test_write_be32(data, fixed);
}

static void update_checksum(uint8_t *data)
{
    unsigned int i, sum = 0;

    for (i = 0; i < 127; ++i) sum += data[i];
    data[127] = (uint8_t)-sum;
}

static void set_edid_xy(uint8_t *edid, unsigned int index, double value)
{
    static const unsigned int msb_offsets[] = {27, 28, 29, 30, 31, 32, 33, 34};
    unsigned int code = value * 1024.0 + 0.5;
    unsigned int low_byte = index < 4 ? 25 : 26;
    unsigned int shift = 6 - (index % 4) * 2;

    edid[low_byte] |= (code & 3) << shift;
    edid[msb_offsets[index]] = code >> 2;
}

static void build_edid(uint8_t edid[256])
{
    static const uint8_t header[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    static const double xy[] = {0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290};
    uint8_t *cta = edid + 128;
    unsigned int i;

    memset(edid, 0, 256);
    memcpy(edid, header, sizeof(header));
    edid[18] = 1;
    edid[19] = 4;
    for (i = 0; i < ARRAY_SIZE(xy); ++i) set_edid_xy(edid, i, xy[i]);
    edid[126] = 1;
    update_checksum(edid);

    cta[0] = 0x02;
    cta[1] = 0x03;
    cta[2] = 11;
    cta[4] = (7 << 5) | 6;
    cta[5] = 0x06; /* HDR Static Metadata extended tag. */
    cta[6] = 0x0c; /* SMPTE ST 2084 and HLG. */
    cta[7] = 0x01; /* Static Metadata Type 1. */
    cta[8] = 96;   /* 400 cd/m2. */
    cta[9] = 64;   /* 200 cd/m2 full-frame. */
    cta[10] = 51;  /* 0.16 cd/m2 minimum. */
    update_checksum(cta);
}

static void test_edid(void)
{
    struct macdrv_display_color_info info;
    uint8_t edid[256], copy[256];

    build_edid(edid);
    macdrv_parse_edid_color_info(edid, sizeof(edid), &info);
    ok((info.valid & (MACDRV_DISPLAY_COLOR_PRIMARIES | MACDRV_DISPLAY_COLOR_WHITE_POINT)) ==
            (MACDRV_DISPLAY_COLOR_PRIMARIES | MACDRV_DISPLAY_COLOR_WHITE_POINT),
            "Unexpected chromaticity validity %#x.\n", info.valid);
    ok(fabs(info.red_x - 0.6396484375) < 0.00001, "Unexpected red x %.8f.\n", info.red_x);
    ok(fabs(info.white_y - 0.3291015625) < 0.00001, "Unexpected white y %.8f.\n", info.white_y);
    ok(info.capabilities == (MACDRV_DISPLAY_COLOR_PQ | MACDRV_DISPLAY_COLOR_HLG),
            "Unexpected capabilities %#x.\n", info.capabilities);
    ok(fabs(info.max_luminance - 400.0) < 0.001, "Unexpected maximum %.8f.\n", info.max_luminance);
    ok(fabs(info.max_full_frame_luminance - 200.0) < 0.001,
            "Unexpected full-frame maximum %.8f.\n", info.max_full_frame_luminance);
    ok(fabs(info.min_luminance - 0.16) < 0.001, "Unexpected minimum %.8f.\n", info.min_luminance);

    macdrv_parse_edid_color_info(edid, 127, &info);
    ok(!info.valid && !info.capabilities, "Truncated EDID produced %#x/%#x.\n", info.valid, info.capabilities);

    memcpy(copy, edid, sizeof(copy));
    copy[20] ^= 1;
    macdrv_parse_edid_color_info(copy, sizeof(copy), &info);
    ok(!info.valid && !info.capabilities, "Bad base checksum produced %#x/%#x.\n", info.valid, info.capabilities);

    memcpy(copy, edid, sizeof(copy));
    copy[200] ^= 1;
    macdrv_parse_edid_color_info(copy, sizeof(copy), &info);
    ok((info.valid & MACDRV_DISPLAY_COLOR_PRIMARIES) != 0, "Valid base chromaticities were lost.\n");
    ok(!(info.valid & MACDRV_DISPLAY_COLOR_MAX_LUMINANCE) && !info.capabilities,
            "Bad CTA checksum produced %#x/%#x.\n", info.valid, info.capabilities);

    memcpy(copy, edid, sizeof(copy));
    copy[25] = (copy[25] & 0x0f) | 0xf0;
    copy[27] = copy[28] = 0xff;
    update_checksum(copy);
    macdrv_parse_edid_color_info(copy, sizeof(copy), &info);
    ok(!(info.valid & MACDRV_DISPLAY_COLOR_PRIMARIES),
            "Physically invalid EDID primaries were exposed, validity %#x.\n", info.valid);

    memcpy(copy, edid, sizeof(copy));
    copy[128 + 7] = 0;
    update_checksum(copy + 128);
    macdrv_parse_edid_color_info(copy, sizeof(copy), &info);
    ok(!(info.valid & (MACDRV_DISPLAY_COLOR_MIN_LUMINANCE | MACDRV_DISPLAY_COLOR_MAX_LUMINANCE |
            MACDRV_DISPLAY_COLOR_MAX_FULL_FRAME_LUMINANCE)),
            "Luminance without Static Metadata Type 1 was exposed, validity %#x.\n", info.valid);
    ok(info.capabilities == (MACDRV_DISPLAY_COLOR_PQ | MACDRV_DISPLAY_COLOR_HLG),
            "EOTF capabilities were lost, capabilities %#x.\n", info.capabilities);

    memcpy(copy, edid, sizeof(copy));
    copy[128 + 8] = 64;
    copy[128 + 9] = 96;
    update_checksum(copy + 128);
    macdrv_parse_edid_color_info(copy, sizeof(copy), &info);
    ok(info.valid & MACDRV_DISPLAY_COLOR_MAX_LUMINANCE, "Valid maximum luminance was lost.\n");
    ok(!(info.valid & MACDRV_DISPLAY_COLOR_MAX_FULL_FRAME_LUMINANCE),
            "Full-frame luminance above peak was exposed.\n");

    memcpy(copy, edid, sizeof(copy));
    copy[128 + 8] = 0;
    copy[128 + 9] = 0;
    copy[128 + 10] = 0;
    update_checksum(copy + 128);
    macdrv_parse_edid_color_info(copy, sizeof(copy), &info);
    ok(!(info.valid & (MACDRV_DISPLAY_COLOR_MIN_LUMINANCE | MACDRV_DISPLAY_COLOR_MAX_LUMINANCE |
            MACDRV_DISPLAY_COLOR_MAX_FULL_FRAME_LUMINANCE)),
            "Unavailable CTA luminance values produced validity %#x.\n", info.valid);

    memcpy(copy, edid, sizeof(copy));
    copy[128 + 2] = 6;
    copy[128 + 4] = (7 << 5) | 31;
    update_checksum(copy + 128);
    macdrv_parse_edid_color_info(copy, sizeof(copy), &info);
    ok(!(info.valid & MACDRV_DISPLAY_COLOR_MAX_LUMINANCE) && !info.capabilities,
            "Overflowing CTA block produced %#x/%#x.\n", info.valid, info.capabilities);
}

static size_t add_xyz_tag(uint8_t *icc, size_t record, size_t offset, uint32_t signature,
        double x, double y, double z)
{
    test_write_be32(icc + 132 + record * 12, signature);
    test_write_be32(icc + 136 + record * 12, offset);
    test_write_be32(icc + 140 + record * 12, 20);
    test_write_be32(icc + offset, TAG('X','Y','Z',' '));
    write_fixed(icc + offset + 8, x);
    write_fixed(icc + offset + 12, y);
    write_fixed(icc + offset + 16, z);
    return offset + 20;
}

static size_t add_chad_tag(uint8_t *icc, size_t record, size_t offset, int singular)
{
    unsigned int i;

    test_write_be32(icc + 132 + record * 12, TAG('c','h','a','d'));
    test_write_be32(icc + 136 + record * 12, offset);
    test_write_be32(icc + 140 + record * 12, 44);
    test_write_be32(icc + offset, TAG('s','f','3','2'));
    for (i = 0; i < 9; ++i) write_fixed(icc + offset + 8 + i * 4, !singular && i % 4 == 0 ? 1.0 : 0.0);
    return offset + 44;
}

static size_t build_icc(uint8_t *icc, size_t capacity, int d50, int chad)
{
    size_t offset = 224;

    memset(icc, 0, capacity);
    test_write_be32(icc + 12, TAG('m','n','t','r'));
    test_write_be32(icc + 16, TAG('R','G','B',' '));
    test_write_be32(icc + 20, TAG('X','Y','Z',' '));
    test_write_be32(icc + 36, TAG('a','c','s','p'));
    test_write_be32(icc + 128, chad ? 6 : 5);
    offset = add_xyz_tag(icc, 0, offset, TAG('r','X','Y','Z'), 0.64, 0.33, 0.03);
    offset = add_xyz_tag(icc, 1, offset, TAG('g','X','Y','Z'), 0.30, 0.60, 0.10);
    offset = add_xyz_tag(icc, 2, offset, TAG('b','X','Y','Z'), 0.15, 0.06, 0.79);
    if (d50)
        offset = add_xyz_tag(icc, 3, offset, TAG('w','t','p','t'), 0.9642, 1.0, 0.8249);
    else
        offset = add_xyz_tag(icc, 3, offset, TAG('w','t','p','t'), 0.95047, 1.0, 1.08883);
    offset = add_xyz_tag(icc, 4, offset, TAG('l','u','m','i'), 0.0, 500.0, 0.0);
    if (chad) offset = add_chad_tag(icc, 5, offset, chad == 2);
    test_write_be32(icc, offset);
    return offset;
}

static void test_icc(void)
{
    struct macdrv_display_color_info info;
    uint8_t icc[512];
    size_t size;

    size = build_icc(icc, sizeof(icc), 0, 0);
    macdrv_parse_icc_color_info(icc, size, &info);
    ok((info.valid & (MACDRV_DISPLAY_COLOR_PRIMARIES | MACDRV_DISPLAY_COLOR_WHITE_POINT |
            MACDRV_DISPLAY_COLOR_MAX_FULL_FRAME_LUMINANCE)) ==
            (MACDRV_DISPLAY_COLOR_PRIMARIES | MACDRV_DISPLAY_COLOR_WHITE_POINT |
            MACDRV_DISPLAY_COLOR_MAX_FULL_FRAME_LUMINANCE), "Unexpected validity %#x.\n", info.valid);
    ok(fabs(info.red_x - 0.64) < 0.0001, "Unexpected red x %.8f.\n", info.red_x);
    ok(fabs(info.white_x - 0.3127) < 0.0001, "Unexpected white x %.8f.\n", info.white_x);
    ok(fabs(info.max_full_frame_luminance - 500.0) < 0.001,
            "Unexpected luminance %.8f.\n", info.max_full_frame_luminance);

    size = build_icc(icc, sizeof(icc), 1, 0);
    macdrv_parse_icc_color_info(icc, size, &info);
    ok(!(info.valid & (MACDRV_DISPLAY_COLOR_PRIMARIES | MACDRV_DISPLAY_COLOR_WHITE_POINT)),
            "D50 PCS data without chad was exposed, validity %#x.\n", info.valid);
    ok(info.valid & MACDRV_DISPLAY_COLOR_MAX_FULL_FRAME_LUMINANCE, "Valid luminance was lost.\n");

    size = build_icc(icc, sizeof(icc), 1, 1);
    macdrv_parse_icc_color_info(icc, size, &info);
    ok((info.valid & (MACDRV_DISPLAY_COLOR_PRIMARIES | MACDRV_DISPLAY_COLOR_WHITE_POINT)) ==
            (MACDRV_DISPLAY_COLOR_PRIMARIES | MACDRV_DISPLAY_COLOR_WHITE_POINT),
            "D50 data with invertible chad was rejected, validity %#x.\n", info.valid);

    size = build_icc(icc, sizeof(icc), 1, 2);
    macdrv_parse_icc_color_info(icc, size, &info);
    ok(!(info.valid & (MACDRV_DISPLAY_COLOR_PRIMARIES | MACDRV_DISPLAY_COLOR_WHITE_POINT)),
            "D50 data with singular chad was exposed, validity %#x.\n", info.valid);

    size = build_icc(icc, sizeof(icc), 0, 0);
    test_write_be32(icc + 136, UINT32_MAX);
    macdrv_parse_icc_color_info(icc, size, &info);
    ok(!(info.valid & MACDRV_DISPLAY_COLOR_PRIMARIES), "Invalid tag offset produced primaries.\n");

    build_icc(icc, sizeof(icc), 0, 0);
    macdrv_parse_icc_color_info(icc, 131, &info);
    ok(!info.valid, "Truncated ICC produced validity %#x.\n", info.valid);

    size = build_icc(icc, sizeof(icc), 0, 0);
    test_write_be32(icc + 128, UINT32_MAX);
    macdrv_parse_icc_color_info(icc, size, &info);
    ok(!info.valid, "Overflowing tag table produced validity %#x.\n", info.valid);
}

static void test_hdr10_serialization(void)
{
    static const uint8_t expected_display_info[] =
    {
        /* HEVC mastering_display_colour_volume() orders G, B, R, then W. */
        0x33, 0xc2, 0x86, 0xc4, 0x1d, 0x4c, 0x0b, 0xb8,
        0x84, 0xd0, 0x3e, 0x80, 0x3d, 0x13, 0x40, 0x42,
        0x00, 0x98, 0x96, 0x80, 0x00, 0x00, 0x00, 0x32,
    };
    static const uint8_t expected_content_info[] = {0x03, 0xe8, 0x01, 0x90};
    static const struct macdrv_hdr10_metadata metadata =
    {
        .red_primary = {34000, 16000},
        .green_primary = {13250, 34500},
        .blue_primary = {7500, 3000},
        .white_point = {15635, 16450},
        .max_mastering_luminance = 1000 * 10000,
        .min_mastering_luminance = 50,
        .max_content_light_level = 1000,
        .max_frame_average_light_level = 400,
    };
    uint8_t display_info[24], content_info[4];

    memset(display_info, 0xcc, sizeof(display_info));
    memset(content_info, 0xcc, sizeof(content_info));
    macdrv_serialize_hdr10_metadata(&metadata, display_info, content_info);
    ok(!memcmp(display_info, expected_display_info, sizeof(display_info)),
            "Unexpected serialized mastering display metadata.\n");
    ok(!memcmp(content_info, expected_content_info, sizeof(content_info)),
            "Unexpected serialized content light metadata.\n");
}

static void test_hdr10_validation(void)
{
    struct macdrv_hdr10_metadata metadata = {0};

    ok(macdrv_hdr10_metadata_is_valid(&metadata),
            "All-unknown HDR10 metadata was rejected.\n");
    ok(!macdrv_hdr10_metadata_is_valid(NULL), "NULL HDR10 metadata was accepted.\n");

    metadata.red_primary[0] = 50000;
    metadata.red_primary[1] = 1;
    ok(!macdrv_hdr10_metadata_is_valid(&metadata),
            "Out-of-gamut HDR10 chromaticity was accepted.\n");
    memset(&metadata, 0, sizeof(metadata));

    metadata.max_mastering_luminance = 10000u * 10000u + 1;
    ok(!macdrv_hdr10_metadata_is_valid(&metadata),
            "Out-of-range mastering luminance was accepted.\n");
    metadata.max_mastering_luminance = 1000u * 10000u;
    metadata.min_mastering_luminance = metadata.max_mastering_luminance + 1;
    ok(!macdrv_hdr10_metadata_is_valid(&metadata),
            "Inverted mastering luminance range was accepted.\n");
    memset(&metadata, 0, sizeof(metadata));

    metadata.max_content_light_level = 400;
    metadata.max_frame_average_light_level = 401;
    ok(!macdrv_hdr10_metadata_is_valid(&metadata),
            "Frame-average light above content peak was accepted.\n");
    metadata.max_content_light_level = 0;
    ok(macdrv_hdr10_metadata_is_valid(&metadata),
            "Unknown content peak incorrectly invalidated a known frame average.\n");
}

static void test_multi_display_merge(void)
{
    struct macdrv_display_color_info display[2] = {{0}};
    struct macdrv_display_color_info edid = {0}, profile = {0};

    edid.valid = MACDRV_DISPLAY_COLOR_PRIMARIES | MACDRV_DISPLAY_COLOR_WHITE_POINT |
            MACDRV_DISPLAY_COLOR_MAX_LUMINANCE;
    edid.capabilities = MACDRV_DISPLAY_COLOR_PQ;
    edid.red_x = 0.64f;
    edid.red_y = 0.33f;
    edid.white_x = 0.3127f;
    edid.white_y = 0.3290f;
    edid.max_luminance = 1000.0f;

    profile.valid = MACDRV_DISPLAY_COLOR_PRIMARIES |
            MACDRV_DISPLAY_COLOR_MAX_FULL_FRAME_LUMINANCE;
    profile.capabilities = MACDRV_DISPLAY_COLOR_WIDE_GAMUT;
    profile.red_x = 0.68f;
    profile.red_y = 0.32f;
    profile.max_full_frame_luminance = 600.0f;

    macdrv_merge_display_color_info(&display[0], &edid);
    macdrv_merge_display_color_info(&display[0], &profile);
    macdrv_merge_display_color_info(&display[1], &profile);

    ok(display[0].red_x == edid.red_x && display[0].red_y == edid.red_y,
            "A lower-priority profile replaced display 0 EDID primaries.\n");
    ok(display[0].max_full_frame_luminance == profile.max_full_frame_luminance,
            "Profile did not fill display 0's unavailable full-frame luminance.\n");
    ok(display[0].capabilities ==
            (MACDRV_DISPLAY_COLOR_PQ | MACDRV_DISPLAY_COLOR_WIDE_GAMUT),
            "Display 0 capabilities were not merged, got %#x.\n", display[0].capabilities);
    ok(display[1].red_x == profile.red_x &&
            !(display[1].valid & MACDRV_DISPLAY_COLOR_MAX_LUMINANCE) &&
            !(display[1].capabilities & MACDRV_DISPLAY_COLOR_PQ),
            "Independent display topology state leaked between displays.\n");
}

static void test_bounded_inputs(void)
{
    struct macdrv_display_color_info info;
    uint8_t data[512];
    uint32_t state = 0x12345678;
    size_t i, j;

    for (i = 0; i < sizeof(data); ++i)
    {
        state = state * 1664525 + 1013904223;
        data[i] = state >> 24;
    }
    for (i = 0; i <= sizeof(data); ++i)
    {
        macdrv_parse_edid_color_info(data, i, &info);
        ok(!(info.valid & ~0x3ff), "EDID parser returned invalid flags %#x for size %lu.\n",
                info.valid, (unsigned long)i);
        macdrv_parse_icc_color_info(data, i, &info);
        ok(!(info.valid & ~0x3ff), "ICC parser returned invalid flags %#x for size %lu.\n",
                info.valid, (unsigned long)i);
        for (j = 0; j < sizeof(data); j += 53) data[j] ^= i + j;
    }
}

START_TEST(display_color)
{
    test_edid();
    test_icc();
    test_hdr10_serialization();
    test_hdr10_validation();
    test_multi_display_merge();
    test_bounded_inputs();
}
