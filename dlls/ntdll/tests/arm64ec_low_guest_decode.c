/* Native unit tests for the pure ARM64EC translated fixed-low decoder. */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arm64ec_low_guest_decode.h"

#define LOW_LIMIT UINT64_C(0x100000000)

static unsigned int failures;

static void check_result( bool condition, const char *expression, unsigned int line )
{
    if (condition) return;
    fprintf( stderr, "line %u: check failed: %s\n", line, expression );
    failures++;
}

#define CHECK(expression) check_result( !!(expression), #expression, __LINE__ )

static uint32_t make_unsigned_offset_opc( unsigned int size_shift, unsigned int opc,
                                           unsigned int immediate,
                                           unsigned int rn, unsigned int rt )
{
    return ((uint32_t)size_shift << 30) | UINT32_C(0x39000000) |
           ((uint32_t)opc << 22) | ((uint32_t)immediate << 10) |
           ((uint32_t)rn << 5) | (uint32_t)rt;
}

static uint32_t make_unscaled_opc( unsigned int size_shift, unsigned int opc,
                                    unsigned int mode, int32_t immediate,
                                    unsigned int rn, unsigned int rt )
{
    return ((uint32_t)size_shift << 30) | UINT32_C(0x38000000) |
           ((uint32_t)opc << 22) | (((uint32_t)immediate & 0x1ff) << 12) |
           ((uint32_t)mode << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static uint32_t make_unscaled( unsigned int size_shift, bool load, unsigned int mode,
                               int32_t immediate, unsigned int rn, unsigned int rt )
{
    return make_unscaled_opc( size_shift, load, mode, immediate, rn, rt );
}

static uint32_t make_register_offset_opc( unsigned int size_shift, unsigned int opc,
                                           unsigned int option, bool scaled,
                                           unsigned int rm, unsigned int rn,
                                           unsigned int rt )
{
    return ((uint32_t)size_shift << 30) | UINT32_C(0x38200800) |
           ((uint32_t)opc << 22) | ((uint32_t)rm << 16) |
           ((uint32_t)option << 13) | ((uint32_t)scaled << 12) |
           ((uint32_t)rn << 5) | (uint32_t)rt;
}

static uint32_t make_register_offset( unsigned int size_shift, bool load,
                                      unsigned int option, bool scaled,
                                      unsigned int rm, unsigned int rn, unsigned int rt )
{
    return make_register_offset_opc( size_shift, load, option, scaled, rm, rn, rt );
}

static void expect_rejected( uint32_t instruction, uint64_t base,
                             uint64_t offset_register, uint64_t fault,
                             bool write, uint64_t low_limit )
{
    struct arm64ec_low_guest_access access;

    memset( &access, 0xa5, sizeof(access) );
    CHECK( !arm64ec_decode_low_guest_access( instruction, base, offset_register, fault, write,
                                              low_limit, &access ) );
    CHECK( !access.address );
    CHECK( !access.writeback );
    CHECK( !access.size );
    CHECK( !access.rn );
    CHECK( !access.rt );
    CHECK( !access.rt2 );
    CHECK( !access.pair_element_size );
    CHECK( !access.simd_scalar_size );
    CHECK( !access.sign_extend_size );
    CHECK( !access.write );
    CHECK( !access.load_32 );
    CHECK( !access.writeback_valid );
}

static uint64_t positive_register_delta( unsigned int option, uint64_t value,
                                          unsigned int shift )
{
    if (option == 2 || option == 6) value = (uint32_t)value;
    return value << shift;
}

static void test_unsigned_offset(void)
{
    static const struct
    {
        uint32_t instruction;
        unsigned int size;
        unsigned int rn;
        unsigned int rt;
        bool write;
    } tests[] =
    {
        {UINT32_C(0x39000062), 1, 3, 2, true},   /* strb w2,[x3] */
        {UINT32_C(0x39400062), 1, 3, 2, false},  /* ldrb w2,[x3] */
        {UINT32_C(0x79000062), 2, 3, 2, true},   /* strh w2,[x3] */
        {UINT32_C(0x79400062), 2, 3, 2, false},  /* ldrh w2,[x3] */
        {UINT32_C(0xb9000062), 4, 3, 2, true},   /* str w2,[x3] */
        {UINT32_C(0xb9400062), 4, 3, 2, false},  /* ldr w2,[x3] */
        {UINT32_C(0xf9000062), 8, 3, 2, true},   /* str x2,[x3] */
        {UINT32_C(0xf9400062), 8, 3, 2, false},  /* ldr x2,[x3] */
        {UINT32_C(0xf940028b), 8, 20, 11, false} /* msvcrt _initterm ldr */
    };
    struct arm64ec_low_guest_access access;
    unsigned int i;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    {
        uint64_t base = tests[i].instruction == UINT32_C(0xf940028b) ?
                        UINT64_C(0x402000) : UINT64_C(0x2000);

        CHECK( arm64ec_decode_low_guest_access( tests[i].instruction, base, 0, base,
                                                 tests[i].write, LOW_LIMIT, &access ) );
        CHECK( access.address == base );
        CHECK( access.writeback == base );
        CHECK( access.size == tests[i].size );
        CHECK( access.rn == tests[i].rn );
        CHECK( access.rt == tests[i].rt );
        CHECK( access.write == tests[i].write );
        CHECK( access.load_32 == (!tests[i].write && tests[i].size == 4) );
        CHECK( !access.sign_extend_size );
        CHECK( !access.writeback_valid );
    }

    CHECK( arm64ec_decode_low_guest_access( UINT32_C(0xf9400462), UINT64_C(0x2000), 0,
                                             UINT64_C(0x200f), false, LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x2008) );
    CHECK( access.size == 8 );
}

static void test_unscaled_pre_post(void)
{
    static const unsigned int modes[] = {0, 1, 3};
    struct arm64ec_low_guest_access access;
    unsigned int size_shift, load, i;

    for (size_shift = 0; size_shift <= 3; ++size_shift)
    {
        for (load = 0; load <= 1; ++load)
        {
            for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
            {
                uint32_t instruction = make_unscaled( size_shift, !!load, modes[i], 8, 3, 2 );
                uint64_t address = modes[i] == 1 ? UINT64_C(0x2000) : UINT64_C(0x2008);

                CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                                         address, !load, LOW_LIMIT, &access ) );
                CHECK( access.address == address );
                CHECK( access.writeback == UINT64_C(0x2008) );
                CHECK( access.size == (1u << size_shift) );
                CHECK( access.write == !load );
                CHECK( access.load_32 == (load && size_shift == 2) );
                CHECK( !access.sign_extend_size );
                CHECK( access.writeback_valid == (modes[i] != 0) );
            }
        }
    }

    CHECK( arm64ec_decode_low_guest_access( make_unscaled( 3, true, 3, -8, 3, 2 ),
                                             UINT64_C(0x2000), 0, UINT64_C(0x1ff8), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x1ff8) );
    CHECK( access.writeback == UINT64_C(0x1ff8) );

    /* Encoding 31 names SP for Rn and ZR for Rt, so this is not overlap. */
    CHECK( arm64ec_decode_low_guest_access( make_unscaled( 3, true, 1, 8, 31, 31 ),
                                             UINT64_C(0x2000), 0, UINT64_C(0x2000), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.rn == 31 && access.rt == 31 );
    CHECK( access.writeback == UINT64_C(0x2008) );

    /* Rn == Rt is defined when there is no writeback. */
    CHECK( arm64ec_decode_low_guest_access( make_unscaled( 3, true, 0, 0, 3, 3 ),
                                             UINT64_C(0x2000), 0, UINT64_C(0x2000), false,
                                             LOW_LIMIT, &access ) );
}

static void test_register_offset(void)
{
    static const unsigned int options[] = {2, 3, 6, 7};
    struct arm64ec_low_guest_access access;
    unsigned int size_shift, load, option_index, scaled;

    for (size_shift = 0; size_shift <= 3; ++size_shift)
    {
        for (load = 0; load <= 1; ++load)
        {
            for (option_index = 0; option_index < sizeof(options) / sizeof(options[0]);
                 ++option_index)
            {
                for (scaled = 0; scaled <= 1; ++scaled)
                {
                    unsigned int option = options[option_index];
                    unsigned int shift = scaled ? size_shift : 0;
                    uint32_t instruction = make_register_offset( size_shift, !!load, option,
                                                                  !!scaled, 8, 3, 2 );
                    uint64_t delta = positive_register_delta( option, 5, shift );
                    uint64_t address = UINT64_C(0x2000) + delta;
                    unsigned int rm = 0;

                    CHECK( arm64ec_low_guest_offset_register( instruction, &rm ) );
                    CHECK( rm == 8 );
                    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 5,
                                                             address, !load, LOW_LIMIT, &access ) );
                    CHECK( access.address == address );
                    CHECK( access.writeback == UINT64_C(0x2000) );
                    CHECK( access.size == (1u << size_shift) );
                    CHECK( access.rn == 3 );
                    CHECK( access.rt == 2 );
                    CHECK( access.write == !load );
                    CHECK( access.load_32 == (load && size_shift == 2) );
                    CHECK( !access.sign_extend_size );
                    CHECK( !access.writeback_valid );
                }
            }
        }
    }

    /* Exact fresh real-world fault: ntdll!wcslen ldrh w9,[x0,x8]. */
    CHECK( arm64ec_low_guest_offset_register( UINT32_C(0x78686809), NULL ) );
    CHECK( arm64ec_decode_low_guest_access( UINT32_C(0x78686809), UINT64_C(0x409df8),
                                             0, UINT64_C(0x409df8), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x409df8) );
    CHECK( access.size == 2 && access.rn == 0 && access.rt == 9 );

    /* Signed register extensions produce negative offsets without changing Rm. */
    CHECK( arm64ec_decode_low_guest_access( make_register_offset( 3, true, 6, false,
                                                                   3, 3, 2 ),
                                             UINT64_C(0x2000), UINT64_C(0xfffffff8),
                                             UINT64_C(0x1ff8), false, LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x1ff8) );
    CHECK( arm64ec_decode_low_guest_access( make_register_offset( 3, true, 6, true,
                                                                   3, 3, 2 ),
                                             UINT64_C(0x2000), UINT64_C(0xffffffff),
                                             UINT64_C(0x1ff8), false, LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x1ff8) );
    CHECK( arm64ec_decode_low_guest_access( make_register_offset( 3, true, 7, false,
                                                                   3, 3, 2 ),
                                             UINT64_C(0x2000), UINT64_C(0xfffffffffffffff8),
                                             UINT64_C(0x1ff8), false, LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( make_register_offset( 3, true, 7, true,
                                                                   3, 3, 2 ),
                                             UINT64_C(0x2000), UINT64_MAX,
                                             UINT64_C(0x1ff8), false, LOW_LIMIT, &access ) );

    /* Register 31 is ZR for Rm; Rn==Rm needs no writeback special case. */
    CHECK( arm64ec_decode_low_guest_access( make_register_offset( 3, true, 3, false,
                                                                   31, 3, 2 ),
                                             UINT64_C(0x2000), UINT64_MAX,
                                             UINT64_C(0x2000), false, LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( make_register_offset( 3, true, 3, false,
                                                                   3, 3, 2 ),
                                             UINT64_C(0x2000), 8, UINT64_C(0x2008),
                                             false, LOW_LIMIT, &access ) );
    CHECK( access.rn == 3 );

    /* Exact fresh application fault: ntdll!wcsrchr ldrh w11,[x10],#2. */
    CHECK( arm64ec_decode_low_guest_access( UINT32_C(0x7840254b),
                                             UINT64_C(0x409df8), 0,
                                             UINT64_C(0x409df8), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x409df8) );
    CHECK( access.writeback == UINT64_C(0x409dfa) );
    CHECK( access.size == 2 && access.rn == 10 && access.rt == 11 );
    CHECK( access.writeback_valid );
}

static void test_signed_scalar_load_decode(void)
{
    static const struct
    {
        unsigned int size_shift;
        unsigned int opc;
        unsigned int result_size;
    } legal[] =
    {
        {0, 3, 4},  /* ldrsb Wt */
        {0, 2, 8},  /* ldrsb Xt */
        {1, 3, 4},  /* ldrsh Wt */
        {1, 2, 8},  /* ldrsh Xt */
        {2, 2, 8}   /* ldrsw Xt */
    }, illegal[] =
    {
        {2, 3, 0},  /* reserved */
        {3, 2, 0},  /* prefetch */
        {3, 3, 0}   /* reserved */
    };
    static const unsigned int modes[] = {0, 1, 3};
    static const unsigned int options[] = {2, 3, 6, 7};
    struct arm64ec_low_guest_access access;
    uint32_t instruction;
    unsigned int i, j, scaled;

    /* Exact coherent-runtime failure: msvcrt!_fwrite_nolock reads its fixed-low
     * format string with ldrsb w0,[x21]. */
    instruction = make_unsigned_offset_opc( 0, 3, 0, 21, 0 );
    CHECK( instruction == UINT32_C(0x39c002a0) );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x404274), 0,
                                             UINT64_C(0x404274), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x404274) );
    CHECK( access.size == 1 && access.sign_extend_size == 4 );
    CHECK( access.rn == 21 && access.rt == 0 );
    CHECK( !access.write && !access.load_32 && !access.writeback_valid );

    for (i = 0; i < sizeof(legal) / sizeof(legal[0]); ++i)
    {
        uint64_t size = UINT64_C(1) << legal[i].size_shift;
        uint64_t address = UINT64_C(0x2000) + (UINT64_C(3) << legal[i].size_shift);

        instruction = make_unsigned_offset_opc( legal[i].size_shift, legal[i].opc,
                                                 3, 21, 2 );
        CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                                 address + size - 1, false,
                                                 LOW_LIMIT, &access ) );
        CHECK( access.address == address && access.writeback == UINT64_C(0x2000) );
        CHECK( access.size == size && access.sign_extend_size == legal[i].result_size );
        CHECK( access.rn == 21 && access.rt == 2 );
        CHECK( !access.write && !access.load_32 && !access.writeback_valid );
        expect_rejected( instruction, UINT64_C(0x2000), 0, address, true, LOW_LIMIT );

        for (j = 0; j < sizeof(modes) / sizeof(modes[0]); ++j)
        {
            uint64_t unscaled_address = modes[j] == 1 ? UINT64_C(0x3000) :
                                                        UINT64_C(0x3008);

            instruction = make_unscaled_opc( legal[i].size_shift, legal[i].opc,
                                               modes[j], 8, 21, 2 );
            CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x3000), 0,
                                                     unscaled_address, false,
                                                     LOW_LIMIT, &access ) );
            CHECK( access.address == unscaled_address );
            CHECK( access.writeback == UINT64_C(0x3008) );
            CHECK( access.size == size &&
                   access.sign_extend_size == legal[i].result_size );
            CHECK( access.writeback_valid == (modes[j] != 0) );
            expect_rejected( instruction, UINT64_C(0x3000), 0, unscaled_address,
                             true, LOW_LIMIT );
        }

        for (j = 0; j < sizeof(options) / sizeof(options[0]); ++j)
        {
            for (scaled = 0; scaled <= 1; ++scaled)
            {
                uint64_t delta = positive_register_delta( options[j], 5,
                                                           scaled ? legal[i].size_shift : 0 );
                uint64_t register_address = UINT64_C(0x4000) + delta;

                instruction = make_register_offset_opc( legal[i].size_shift, legal[i].opc,
                                                          options[j], !!scaled, 8, 21, 2 );
                CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x4000), 5,
                                                         register_address, false,
                                                         LOW_LIMIT, &access ) );
                CHECK( access.address == register_address );
                CHECK( access.writeback == UINT64_C(0x4000) );
                CHECK( access.size == size &&
                       access.sign_extend_size == legal[i].result_size );
                CHECK( !access.writeback_valid );
                expect_rejected( instruction, UINT64_C(0x4000), 5, register_address,
                                 true, LOW_LIMIT );
            }
        }
    }

    /* Cover every unsupported opc 2/3 source-size combination in each scalar
     * addressing family, including every writeback and register extension. */
    for (i = 0; i < sizeof(illegal) / sizeof(illegal[0]); ++i)
    {
        instruction = make_unsigned_offset_opc( illegal[i].size_shift, illegal[i].opc,
                                                 0, 21, 2 );
        expect_rejected( instruction, UINT64_C(0x2000), 0, UINT64_C(0x2000),
                         false, LOW_LIMIT );
        for (j = 0; j < sizeof(modes) / sizeof(modes[0]); ++j)
        {
            instruction = make_unscaled_opc( illegal[i].size_shift, illegal[i].opc,
                                               modes[j], 8, 21, 2 );
            expect_rejected( instruction, UINT64_C(0x3000), 0,
                             modes[j] == 1 ? UINT64_C(0x3000) : UINT64_C(0x3008),
                             false, LOW_LIMIT );
        }
        for (j = 0; j < sizeof(options) / sizeof(options[0]); ++j)
        {
            for (scaled = 0; scaled <= 1; ++scaled)
            {
                instruction = make_register_offset_opc( illegal[i].size_shift,
                                                          illegal[i].opc, options[j],
                                                          !!scaled, 8, 21, 2 );
                expect_rejected( instruction, UINT64_C(0x4000), 5,
                                 UINT64_C(0x4000) +
                                 positive_register_delta( options[j], 5,
                                     scaled ? illegal[i].size_shift : 0 ),
                                 false, LOW_LIMIT );
            }
        }
    }

    /* Rt==31 still performs the memory access, then discards the result.  The
     * post-index SP/ZR encoding is not an overlapping-register case. */
    instruction = make_unscaled_opc( 1, 3, 1, 2, 31, 31 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x5000), 0,
                                             UINT64_C(0x5001), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x5000) );
    CHECK( access.writeback == UINT64_C(0x5002) && access.writeback_valid );
    CHECK( access.rn == 31 && access.rt == 31 );
    CHECK( access.size == 2 && access.sign_extend_size == 4 );
    expect_rejected( make_unscaled_opc( 0, 3, 1, 1, 3, 3 ),
                     UINT64_C(0x5000), 0, UINT64_C(0x5000), false, LOW_LIMIT );

    /* Signed loads retain the same FAR-within-operand and window bounds. */
    instruction = make_unsigned_offset_opc( 1, 2, 0, 21, 2 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0xfff), 0,
                                             UINT64_C(0x1000), false,
                                             LOW_LIMIT, &access ) );
    expect_rejected( instruction, UINT64_C(0xfff), 0, UINT64_C(0x1001),
                     false, LOW_LIMIT );
    expect_rejected( instruction, LOW_LIMIT - 1, 0, LOW_LIMIT - 1,
                     false, LOW_LIMIT );

}

static void test_signed_load_extension(void)
{
    static const struct
    {
        uint64_t value;
        unsigned int source_size;
        unsigned int result_size;
        uint64_t expected;
    } tests[] =
    {
        {UINT64_C(0x00), 1, 4, UINT64_C(0x00000000)},
        {UINT64_C(0x7f), 1, 8, UINT64_C(0x000000000000007f)},
        {UINT64_C(0x80), 1, 4, UINT64_C(0x00000000ffffff80)},
        {UINT64_C(0xff), 1, 8, UINT64_C(0xffffffffffffffff)},
        {UINT64_C(0x12345680), 1, 8, UINT64_C(0xffffffffffffff80)},
        {UINT64_C(0x7fff), 2, 4, UINT64_C(0x0000000000007fff)},
        {UINT64_C(0x8000), 2, 4, UINT64_C(0x00000000ffff8000)},
        {UINT64_C(0xffff), 2, 8, UINT64_C(0xffffffffffffffff)},
        {UINT64_C(0x12348000), 2, 8, UINT64_C(0xffffffffffff8000)},
        {UINT64_C(0x7fffffff), 4, 8, UINT64_C(0x000000007fffffff)},
        {UINT64_C(0x80000000), 4, 8, UINT64_C(0xffffffff80000000)},
        {UINT64_C(0xffffffff), 4, 8, UINT64_C(0xffffffffffffffff)}
    };
    uint64_t result;
    unsigned int i;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    {
        result = UINT64_MAX;
        CHECK( arm64ec_low_guest_extend_signed_load( tests[i].value,
                                                       tests[i].source_size,
                                                       tests[i].result_size,
                                                       &result ) );
        CHECK( result == tests[i].expected );
    }

    result = UINT64_C(0xa5a5a5a5a5a5a5a5);
    CHECK( !arm64ec_low_guest_extend_signed_load( 0, 0, 4, &result ) );
    CHECK( !arm64ec_low_guest_extend_signed_load( 0, 1, 2, &result ) );
    CHECK( !arm64ec_low_guest_extend_signed_load( 0, 2, 16, &result ) );
    CHECK( !arm64ec_low_guest_extend_signed_load( 0, 4, 4, &result ) );
    CHECK( !arm64ec_low_guest_extend_signed_load( 0, 8, 8, &result ) );
    CHECK( !arm64ec_low_guest_extend_signed_load( 0, 1, 4, NULL ) );
    CHECK( result == UINT64_C(0xa5a5a5a5a5a5a5a5) );
}

static void test_native_signed_load_extension(void)
{
#if defined(__aarch64__)
    static const uint8_t bytes[] = {UINT8_C(0x01), UINT8_C(0x7f), UINT8_C(0x80),
                                     UINT8_C(0xff)};
    static const uint16_t halves[] = {UINT16_C(0x0001), UINT16_C(0x7fff),
                                       UINT16_C(0x8000), UINT16_C(0xffff)};
    static const uint32_t words[] = {UINT32_C(0x00000001), UINT32_C(0x7fffffff),
                                      UINT32_C(0x80000000), UINT32_C(0xffffffff)};
    uint64_t native_w, native_x, expected;
    unsigned int i;

    for (i = 0; i < sizeof(bytes) / sizeof(bytes[0]); ++i)
    {
        __asm__ volatile( "ldrsb %w0, [%1]" : "=&r" (native_w) : "r" (&bytes[i]) : "memory" );
        __asm__ volatile( "ldrsb %x0, [%1]" : "=&r" (native_x) : "r" (&bytes[i]) : "memory" );
        CHECK( arm64ec_low_guest_extend_signed_load( bytes[i], 1, 4, &expected ) );
        CHECK( native_w == expected );
        CHECK( arm64ec_low_guest_extend_signed_load( bytes[i], 1, 8, &expected ) );
        CHECK( native_x == expected );
    }
    for (i = 0; i < sizeof(halves) / sizeof(halves[0]); ++i)
    {
        __asm__ volatile( "ldrsh %w0, [%1]" : "=&r" (native_w) : "r" (&halves[i]) : "memory" );
        __asm__ volatile( "ldrsh %x0, [%1]" : "=&r" (native_x) : "r" (&halves[i]) : "memory" );
        CHECK( arm64ec_low_guest_extend_signed_load( halves[i], 2, 4, &expected ) );
        CHECK( native_w == expected );
        CHECK( arm64ec_low_guest_extend_signed_load( halves[i], 2, 8, &expected ) );
        CHECK( native_x == expected );
    }
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
    {
        __asm__ volatile( "ldrsw %x0, [%1]" : "=&r" (native_x) : "r" (&words[i]) : "memory" );
        CHECK( arm64ec_low_guest_extend_signed_load( words[i], 4, 8, &expected ) );
        CHECK( native_x == expected );
    }
#endif
}

static void test_rejected_opcodes(void)
{
    static const struct
    {
        uint32_t instruction;
        bool write;
    } tests[] =
    {
        {UINT32_C(0xfd000062), true},   /* str d2,[x3] */
        {UINT32_C(0xfd400062), false},  /* ldr d2,[x3] */
        {UINT32_C(0xfc000062), true},   /* stur d2,[x3] */
        {UINT32_C(0xfc400062), false},  /* ldur d2,[x3] */
        {UINT32_C(0xa9400862), false},  /* pair load */
        {UINT32_C(0xc85f7c62), false},  /* exclusive load */
        {UINT32_C(0xf8400862), false},  /* unprivileged load */
        {UINT32_C(0xf8408463), false}   /* writeback with Rn == Rt */
    };
    unsigned int i;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
        expect_rejected( tests[i].instruction, UINT64_C(0x2000), 0, UINT64_C(0x2000),
                         tests[i].write, LOW_LIMIT );

    expect_rejected( UINT32_C(0xf9400062), UINT64_C(0x2000), 0, UINT64_C(0x2000),
                     true, LOW_LIMIT );
    expect_rejected( UINT32_C(0xf9000062), UINT64_C(0x2000), 0, UINT64_C(0x2000),
                     false, LOW_LIMIT );

    /* Register-offset reserved extension options fail closed. */
    for (i = 0; i < 8; ++i)
        if (i != 2 && i != 3 && i != 6 && i != 7)
            expect_rejected( make_register_offset( 3, true, i, false, 8, 3, 2 ),
                             UINT64_C(0x2000), 8, UINT64_C(0x2008), false, LOW_LIMIT );
    expect_rejected( UINT32_C(0xf8a86862), UINT64_C(0x2000), 8,
                     UINT64_C(0x2008), false, LOW_LIMIT ); /* register prefetch */
    expect_rejected( UINT32_C(0xfd686862), UINT64_C(0x2000), 8,
                     UINT64_C(0x2008), false, LOW_LIMIT ); /* SIMD register load */
}

static uint32_t make_q_pair( bool load, int32_t immediate,
                             unsigned int rn, unsigned int rt,
                             unsigned int rt2 )
{
    CHECK( !(immediate & 15) );
    return UINT32_C(0xad000000) | ((uint32_t)load << 22) |
           (((uint32_t)(immediate / 16) & 0x7f) << 15) |
           ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static uint32_t make_gpr_pair( bool load, bool wide, int32_t immediate,
                               unsigned int rn, unsigned int rt,
                               unsigned int rt2 )
{
    unsigned int element_size = wide ? 8 : 4;

    CHECK( !(immediate % (int32_t)element_size) );
    return (wide ? UINT32_C(0xa9000000) : UINT32_C(0x29000000)) |
           ((uint32_t)load << 22) |
           (((uint32_t)(immediate / (int32_t)element_size) & 0x7f) << 15) |
           ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static uint32_t make_d_post_index( bool load, int32_t immediate,
                                   unsigned int rn, unsigned int rt )
{
    return UINT32_C(0xfc000400) | ((uint32_t)load << 22) |
           (((uint32_t)immediate & 0x1ff) << 12) |
           ((uint32_t)rn << 5) | (uint32_t)rt;
}

static uint32_t make_q_unsigned_offset( bool load, unsigned int immediate,
                                        unsigned int rn, unsigned int rt )
{
    CHECK( immediate <= 0xfff );
    return UINT32_C(0x3d800000) | ((uint32_t)load << 22) |
           ((uint32_t)immediate << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static void test_q_pair_offset(void)
{
    const uint32_t exact_load = UINT32_C(0xad7f8540);
    const uint32_t exact_store = UINT32_C(0xad3f8540);
    struct arm64ec_low_guest_access access;
    uint32_t instruction;

    /* Exact application fault: ntdll!memmove ldp q0,q1,[x10,#-16]. */
    CHECK( arm64ec_decode_low_guest_access( exact_load, UINT64_C(0x40b050), 0,
                                             UINT64_C(0x40b042), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x40b040) );
    CHECK( access.writeback == UINT64_C(0x40b050) );
    CHECK( access.size == 32 && access.rn == 10 );
    CHECK( access.rt == 0 && access.rt2 == 1 );
    CHECK( !access.write && !access.writeback_valid && access.pair_element_size == 16 );

    CHECK( arm64ec_decode_low_guest_access( exact_store, UINT64_C(0x40b050), 0,
                                             UINT64_C(0x40b05f), true,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x40b040) && access.write );

    /* The signed imm7 scale is fixed at 16 bytes for Q registers. */
    instruction = make_q_pair( true, -1024, 3, 2, 4 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                             UINT64_C(0x1c00), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x1c00) );
    instruction = make_q_pair( true, 1008, 3, 2, 4 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                             UINT64_C(0x240f), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x23f0) );

    /* Rt and Rt2 are independent five-bit fields; Q31 does not name ZR. */
    instruction = make_q_pair( true, 0, 3, 31, 0 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                             UINT64_C(0x201f), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.rt == 31 && access.rt2 == 0 && access.pair_element_size == 16 );

    /* A pair load with overlapping destinations is architecturally unpredictable. */
    expect_rejected( make_q_pair( true, 0, 3, 4, 4 ), UINT64_C(0x2000), 0,
                     UINT64_C(0x2000), false, LOW_LIMIT );

    /* FAR can identify any byte in the complete 32-byte memory operand. */
    instruction = make_q_pair( true, 0, 3, 2, 4 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0xff0), 0,
                                             UINT64_C(0xff0), false,
                                             LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0xff0), 0,
                                             UINT64_C(0x1000), false,
                                             LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0xff0), 0,
                                             UINT64_C(0x100f), false,
                                             LOW_LIMIT, &access ) );
    expect_rejected( instruction, UINT64_C(0xff0), 0, UINT64_C(0xfef),
                     false, LOW_LIMIT );
    expect_rejected( instruction, UINT64_C(0xff0), 0, UINT64_C(0x1010),
                     false, LOW_LIMIT );

    CHECK( arm64ec_decode_low_guest_access( make_q_pair( true, -1024, 3, 2, 4 ),
                                             UINT64_C(0x401), 0, 1,
                                             false, LOW_LIMIT, &access ) );
    expect_rejected( make_q_pair( true, -1024, 3, 2, 4 ), 1023, 0, 1,
                     false, LOW_LIMIT );
    CHECK( arm64ec_decode_low_guest_access( make_q_pair( true, 0, 3, 2, 4 ),
                                             LOW_LIMIT - 32, 0, LOW_LIMIT - 1,
                                             false, LOW_LIMIT, &access ) );
    expect_rejected( make_q_pair( true, 0, 3, 2, 4 ), LOW_LIMIT - 31, 0,
                     LOW_LIMIT - 1, false, LOW_LIMIT );
    expect_rejected( make_q_pair( true, 1008, 3, 2, 4 ), UINT64_MAX - 1000,
                     0, UINT64_MAX - 1, false, UINT64_MAX );

    expect_rejected( exact_load, UINT64_C(0x40b050), 0, UINT64_C(0x40b042),
                     true, LOW_LIMIT );
    expect_rejected( exact_store, UINT64_C(0x40b050), 0, UINT64_C(0x40b042),
                     false, LOW_LIMIT );

    /* Reject every neighboring pair family and writeback address mode. */
    expect_rejected( UINT32_C(0xacff8540), UINT64_C(0x40b050), 0,
                     UINT64_C(0x40b040), false, LOW_LIMIT ); /* post-index Q */
    expect_rejected( UINT32_C(0xadff8540), UINT64_C(0x40b050), 0,
                     UINT64_C(0x40b040), false, LOW_LIMIT ); /* pre-index Q */
    expect_rejected( UINT32_C(0xac7f8540), UINT64_C(0x40b050), 0,
                     UINT64_C(0x40b040), false, LOW_LIMIT ); /* non-temporal Q */
    expect_rejected( UINT32_C(0x697f8540), UINT64_C(0x40b050), 0,
                     UINT64_C(0x40b040), false, LOW_LIMIT ); /* signed W pair */
    expect_rejected( UINT32_C(0x6d7f8540), UINT64_C(0x40b050), 0,
                     UINT64_C(0x40b040), false, LOW_LIMIT ); /* D pair */
    expect_rejected( UINT32_C(0x2d7f8540), UINT64_C(0x40b050), 0,
                     UINT64_C(0x40b040), false, LOW_LIMIT ); /* S pair */
    expect_rejected( UINT32_C(0xc85f7c62), UINT64_C(0x2000), 0,
                     UINT64_C(0x2000), false, LOW_LIMIT );  /* exclusive/atomic */
}

static void test_gpr_pair_offset(void)
{
    const uint32_t exact_load_w = UINT32_C(0x2940e813);
    struct arm64ec_low_guest_access access;
    uint32_t instruction;

    /* Exact application fault: user32!create_icon_frame ldp w19,w26,[x0,#4]. */
    CHECK( arm64ec_decode_low_guest_access( exact_load_w, UINT64_C(0x410480), 0,
                                             UINT64_C(0x410484), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x410484) );
    CHECK( access.writeback == UINT64_C(0x410480) );
    CHECK( access.size == 8 && access.pair_element_size == 4 );
    CHECK( access.rn == 0 && access.rt == 19 && access.rt2 == 26 );
    CHECK( !access.write && !access.writeback_valid );

    /* W and X pairs use imm7 scaled by their respective element width. */
    instruction = make_gpr_pair( true, false, -256, 3, 2, 4 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                             UINT64_C(0x1f00), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x1f00) && access.size == 8 &&
           access.pair_element_size == 4 );
    instruction = make_gpr_pair( true, false, 252, 3, 2, 4 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                             UINT64_C(0x2103), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x20fc) );

    instruction = make_gpr_pair( true, true, -512, 3, 2, 4 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                             UINT64_C(0x1e00), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x1e00) && access.size == 16 &&
           access.pair_element_size == 8 );
    instruction = make_gpr_pair( true, true, 504, 3, 2, 4 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                             UINT64_C(0x2207), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x21f8) );

    /* Register 31 is ZR for GPR pair operands, while Rt2 is independent. */
    instruction = make_gpr_pair( true, false, 0, 3, 31, 0 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                             UINT64_C(0x2007), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.rt == 31 && access.rt2 == 0 && access.pair_element_size == 4 );

    /* Overlapping load destinations are unpredictable; duplicate stores are defined. */
    expect_rejected( make_gpr_pair( true, false, 0, 3, 2, 2 ),
                     UINT64_C(0x2000), 0, UINT64_C(0x2000), false, LOW_LIMIT );
    CHECK( arm64ec_decode_low_guest_access( make_gpr_pair( false, false, 0, 3, 2, 2 ),
                                             UINT64_C(0x2000), 0, UINT64_C(0x2000),
                                             true, LOW_LIMIT, &access ) );

    /* FAR admission covers both pair elements and exact low-window bounds. */
    instruction = make_gpr_pair( true, true, 0, 3, 2, 4 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0xff8), 0,
                                             UINT64_C(0xff8), false,
                                             LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0xff8), 0,
                                             UINT64_C(0x1007), false,
                                             LOW_LIMIT, &access ) );
    expect_rejected( instruction, UINT64_C(0xff8), 0, UINT64_C(0xff7),
                     false, LOW_LIMIT );
    expect_rejected( instruction, UINT64_C(0xff8), 0, UINT64_C(0x1008),
                     false, LOW_LIMIT );
    CHECK( arm64ec_decode_low_guest_access( instruction, LOW_LIMIT - 16, 0,
                                             LOW_LIMIT - 1, false,
                                             LOW_LIMIT, &access ) );
    expect_rejected( instruction, LOW_LIMIT - 15, 0, LOW_LIMIT - 1,
                     false, LOW_LIMIT );
    expect_rejected( make_gpr_pair( true, true, 504, 3, 2, 4 ),
                     UINT64_MAX - 500, 0, UINT64_MAX - 1, false, UINT64_MAX );

    expect_rejected( exact_load_w, UINT64_C(0x410480), 0,
                     UINT64_C(0x410484), true, LOW_LIMIT );
    expect_rejected( make_gpr_pair( false, false, 4, 0, 19, 26 ),
                     UINT64_C(0x410480), 0, UINT64_C(0x410484), false, LOW_LIMIT );

    /* Reject writeback, non-temporal, signed-load, and atomic neighbors. */
    expect_rejected( UINT32_C(0x28c0e813), UINT64_C(0x410480), 0,
                     UINT64_C(0x410484), false, LOW_LIMIT ); /* post-index W */
    expect_rejected( UINT32_C(0x29c0e813), UINT64_C(0x410480), 0,
                     UINT64_C(0x410484), false, LOW_LIMIT ); /* pre-index W */
    expect_rejected( UINT32_C(0x2840e813), UINT64_C(0x410480), 0,
                     UINT64_C(0x410484), false, LOW_LIMIT ); /* non-temporal W */
    expect_rejected( UINT32_C(0x6940e813), UINT64_C(0x410480), 0,
                     UINT64_C(0x410484), false, LOW_LIMIT ); /* LDPSW */
    expect_rejected( UINT32_C(0xc85f7c62), UINT64_C(0x2000), 0,
                     UINT64_C(0x2000), false, LOW_LIMIT );  /* exclusive/atomic */
}

static void test_simd_d_post_index(void)
{
    const uint32_t exact_load = UINT32_C(0xfc408560);
    const uint32_t exact_store = UINT32_C(0xfc008600);
    struct arm64ec_low_guest_access access;
    uint32_t instruction;

    /* Exact ntdll!memmove faults: ldr d0,[x11],#8 / str d0,[x16],#8. */
    CHECK( arm64ec_decode_low_guest_access( exact_load, UINT64_C(0x40b060), 0,
                                             UINT64_C(0x40b062), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x40b060) );
    CHECK( access.writeback == UINT64_C(0x40b068) );
    CHECK( access.size == 8 && access.simd_scalar_size == 8 );
    CHECK( access.rn == 11 && access.rt == 0 );
    CHECK( !access.write && access.writeback_valid );

    CHECK( arm64ec_decode_low_guest_access( exact_store, UINT64_C(0x40c000), 0,
                                             UINT64_C(0x40c007), true,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x40c000) );
    CHECK( access.writeback == UINT64_C(0x40c008) );
    CHECK( access.size == 8 && access.simd_scalar_size == 8 );
    CHECK( access.rn == 16 && access.rt == 0 );
    CHECK( access.write && access.writeback_valid );

    /* Signed imm9 affects only post-access writeback. */
    instruction = make_d_post_index( true, -256, 3, 2 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                             UINT64_C(0x2000), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x2000) &&
           access.writeback == UINT64_C(0x1f00) );
    instruction = make_d_post_index( true, 255, 3, 2 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                             UINT64_C(0x2007), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.writeback == UINT64_C(0x20ff) );

    /* D31 is a real SIMD register.  Equal Xn/Dt numbers are separate banks. */
    CHECK( arm64ec_decode_low_guest_access( make_d_post_index( true, 8, 31, 31 ),
                                             UINT64_C(0x2000), 0,
                                             UINT64_C(0x2000), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.rn == 31 && access.rt == 31 && access.simd_scalar_size == 8 );
    CHECK( arm64ec_decode_low_guest_access( make_d_post_index( true, 8, 11, 11 ),
                                             UINT64_C(0x2000), 0,
                                             UINT64_C(0x2000), false,
                                             LOW_LIMIT, &access ) );

    /* FAR can identify any byte of the complete eight-byte operand. */
    instruction = make_d_post_index( true, 8, 3, 2 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0xffc), 0,
                                             UINT64_C(0xffc), false,
                                             LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0xffc), 0,
                                             UINT64_C(0x1000), false,
                                             LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0xffc), 0,
                                             UINT64_C(0x1003), false,
                                             LOW_LIMIT, &access ) );
    expect_rejected( instruction, UINT64_C(0xffc), 0, UINT64_C(0xffb),
                     false, LOW_LIMIT );
    expect_rejected( instruction, UINT64_C(0xffc), 0, UINT64_C(0x1004),
                     false, LOW_LIMIT );
    instruction = make_d_post_index( true, 0, 3, 2 );
    CHECK( arm64ec_decode_low_guest_access( instruction, LOW_LIMIT - 8, 0,
                                             LOW_LIMIT - 1, false,
                                             LOW_LIMIT, &access ) );
    expect_rejected( instruction, LOW_LIMIT - 7, 0, LOW_LIMIT - 1,
                     false, LOW_LIMIT );

    /* Writeback must remain in the fixed-low guest domain without wrapping. */
    expect_rejected( make_d_post_index( true, -256, 3, 2 ), 255, 0, 255,
                     false, LOW_LIMIT );
    expect_rejected( make_d_post_index( true, 8, 3, 2 ), LOW_LIMIT - 8, 0,
                     LOW_LIMIT - 1, false, LOW_LIMIT );
    expect_rejected( exact_load, UINT64_C(0x40b060), 0, UINT64_C(0x40b062),
                     true, LOW_LIMIT );
    expect_rejected( exact_store, UINT64_C(0x40c000), 0, UINT64_C(0x40c000),
                     false, LOW_LIMIT );

    /* Keep every adjacent SIMD width/addressing class fail-closed. */
    expect_rejected( UINT32_C(0xfc408160), UINT64_C(0x40b060), 0,
                     UINT64_C(0x40b068), false, LOW_LIMIT ); /* LDUR D */
    expect_rejected( UINT32_C(0xfc408d60), UINT64_C(0x40b060), 0,
                     UINT64_C(0x40b068), false, LOW_LIMIT ); /* pre-index D */
    expect_rejected( UINT32_C(0xfd400560), UINT64_C(0x40b060), 0,
                     UINT64_C(0x40b068), false, LOW_LIMIT ); /* unsigned-offset D */
    expect_rejected( UINT32_C(0xbc408560), UINT64_C(0x40b060), 0,
                     UINT64_C(0x40b060), false, LOW_LIMIT ); /* post-index S */
    expect_rejected( UINT32_C(0x3cc10560), UINT64_C(0x40b060), 0,
                     UINT64_C(0x40b060), false, LOW_LIMIT ); /* post-index Q */
    expect_rejected( UINT32_C(0xfc606960), UINT64_C(0x40b060), 0,
                     UINT64_C(0x40b060), false, LOW_LIMIT ); /* register-offset D */
}

static void test_simd_q_unsigned_offset(void)
{
    const uint32_t exact_load = UINT32_C(0x3dc00320);
    const uint32_t exact_store = UINT32_C(0x3d800320);
    struct arm64ec_low_guest_access access;
    uint32_t instruction, immediate, load, rt;

    /* Exact combase!CoCreateInstanceEx fault: ldr q0,[x25]. */
    CHECK( arm64ec_decode_low_guest_access( exact_load, UINT64_C(0x409e28), 0,
                                             UINT64_C(0x409e28), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x409e28) &&
           access.writeback == UINT64_C(0x409e28) );
    CHECK( access.size == 16 && access.simd_scalar_size == 16 );
    CHECK( access.rn == 25 && access.rt == 0 );
    CHECK( !access.write && !access.writeback_valid );

    CHECK( arm64ec_decode_low_guest_access( exact_store, UINT64_C(0x40c008), 0,
                                             UINT64_C(0x40c017), true,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x40c008) &&
           access.writeback == UINT64_C(0x40c008) );
    CHECK( access.size == 16 && access.simd_scalar_size == 16 );
    CHECK( access.rn == 25 && access.rt == 0 );
    CHECK( access.write && !access.writeback_valid );

    /* Exercise every unsigned immediate, transfer register, and direction;
     * cycle the immediate through all base-register encodings as well. */
    for (load = 0; load < 2; ++load)
        for (immediate = 0; immediate <= 0xfff; ++immediate)
            for (rt = 0; rt < 32; ++rt)
            {
                uint64_t address = UINT64_C(0x100000) + ((uint64_t)immediate << 4);
                unsigned int rn = immediate & 0x1f;

                instruction = make_q_unsigned_offset( !!load, immediate, rn, rt );
                CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x100000), 0,
                                                         address + 15, !load,
                                                         LOW_LIMIT, &access ) );
                CHECK( access.address == address && access.writeback == UINT64_C(0x100000) );
                CHECK( access.size == 16 && access.simd_scalar_size == 16 );
                CHECK( access.rn == rn && access.rt == rt && access.write == !load );
                CHECK( !access.writeback_valid );
            }

    /* The unsigned imm12 is scaled by 16; Q31 and SP are valid registers. */
    instruction = make_q_unsigned_offset( true, 0xfff, 31, 31 );
    CHECK( instruction == UINT32_C(0x3dffffff) );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                             UINT64_C(0x11fff), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x11ff0) &&
           access.writeback == UINT64_C(0x2000) );
    CHECK( access.rn == 31 && access.rt == 31 &&
           access.simd_scalar_size == 16 && !access.writeback_valid );
    instruction = make_q_unsigned_offset( false, 0xfff, 31, 31 );
    CHECK( instruction == UINT32_C(0x3dbfffff) );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x2000), 0,
                                             UINT64_C(0x11ff0), true,
                                             LOW_LIMIT, &access ) );

    /* FAR can identify any byte when the operand crosses a 4K or 16K boundary. */
    instruction = make_q_unsigned_offset( true, 0, 3, 2 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0xff8), 0,
                                             UINT64_C(0xff8), false,
                                             LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0xff8), 0,
                                             UINT64_C(0x1000), false,
                                             LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0xff8), 0,
                                             UINT64_C(0x1007), false,
                                             LOW_LIMIT, &access ) );
    expect_rejected( instruction, UINT64_C(0xff8), 0, UINT64_C(0xff7),
                     false, LOW_LIMIT );
    expect_rejected( instruction, UINT64_C(0xff8), 0, UINT64_C(0x1008),
                     false, LOW_LIMIT );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x3ff8), 0,
                                             UINT64_C(0x4000), false,
                                             LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x3ff8), 0,
                                             UINT64_C(0x4007), false,
                                             LOW_LIMIT, &access ) );

    /* Full operand and scaled address must remain in the fixed-low window. */
    CHECK( arm64ec_decode_low_guest_access( instruction, LOW_LIMIT - 16, 0,
                                             LOW_LIMIT - 1, false,
                                             LOW_LIMIT, &access ) );
    expect_rejected( instruction, LOW_LIMIT - 15, 0, LOW_LIMIT - 1,
                     false, LOW_LIMIT );
    instruction = make_q_unsigned_offset( true, 1, 3, 2 );
    CHECK( arm64ec_decode_low_guest_access( instruction, LOW_LIMIT - 32, 0,
                                             LOW_LIMIT - 1, false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == LOW_LIMIT - 16 );
    expect_rejected( instruction, LOW_LIMIT - 31, 0, LOW_LIMIT - 1,
                     false, LOW_LIMIT );
    expect_rejected( make_q_unsigned_offset( true, 0xfff, 3, 2 ),
                     UINT64_MAX - UINT64_C(0x8000), 0, UINT64_MAX - UINT64_C(0x8000),
                     false, UINT64_MAX );
    expect_rejected( exact_load, 0, 0, 0, false, LOW_LIMIT );
    expect_rejected( exact_load, UINT64_C(0x409e28), 0, UINT64_C(0x409e28),
                     true, LOW_LIMIT );
    expect_rejected( exact_store, UINT64_C(0x40c008), 0, UINT64_C(0x40c008),
                     false, LOW_LIMIT );

    /* Keep every adjacent width, address class, structure, and atomic form closed. */
    expect_rejected( UINT32_C(0xfd400320), UINT64_C(0x409e28), 0,
                     UINT64_C(0x409e28), false, LOW_LIMIT ); /* unsigned D */
    expect_rejected( UINT32_C(0xbd400320), UINT64_C(0x409e28), 0,
                     UINT64_C(0x409e28), false, LOW_LIMIT ); /* unsigned S */
    expect_rejected( UINT32_C(0x7d400320), UINT64_C(0x409e28), 0,
                     UINT64_C(0x409e28), false, LOW_LIMIT ); /* unsigned H */
    expect_rejected( UINT32_C(0x3d400320), UINT64_C(0x409e28), 0,
                     UINT64_C(0x409e28), false, LOW_LIMIT ); /* unsigned B */
    expect_rejected( UINT32_C(0x3ce06b20), UINT64_C(0x409e28), 0,
                     UINT64_C(0x409e28), false, LOW_LIMIT ); /* register Q */
    expect_rejected( UINT32_C(0x3cc00320), UINT64_C(0x409e28), 0,
                     UINT64_C(0x409e28), false, LOW_LIMIT ); /* unscaled Q */
    expect_rejected( UINT32_C(0x3c800320), UINT64_C(0x409e28), 0,
                     UINT64_C(0x409e28), true, LOW_LIMIT );  /* unscaled Q store */
    expect_rejected( UINT32_C(0x3cc10720), UINT64_C(0x409e28), 0,
                     UINT64_C(0x409e28), false, LOW_LIMIT ); /* post-index Q */
    expect_rejected( UINT32_C(0x3cc10f20), UINT64_C(0x409e28), 0,
                     UINT64_C(0x409e38), false, LOW_LIMIT ); /* pre-index Q */
    expect_rejected( UINT32_C(0x4c407320), UINT64_C(0x409e28), 0,
                     UINT64_C(0x409e28), false, LOW_LIMIT ); /* LD1 structure */
    expect_rejected( UINT32_C(0x4c007320), UINT64_C(0x409e28), 0,
                     UINT64_C(0x409e28), true, LOW_LIMIT );  /* ST1 structure */
    expect_rejected( UINT32_C(0xc87f0720), UINT64_C(0x409e28), 0,
                     UINT64_C(0x409e28), false, LOW_LIMIT ); /* LDXP atomic */
}

static void test_native_simd_q_unsigned_offset(void)
{
#if defined(__aarch64__)
    struct vector128
    {
        uint64_t low;
        uint64_t high;
    } source[2] = {{UINT64_C(0xaaaaaaaaaaaaaaaa), UINT64_C(0xbbbbbbbbbbbbbbbb)},
                   {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)}},
      loaded = {0, 0}, input = {UINT64_C(0x8877665544332211),
                                UINT64_C(0x1122334455667788)},
      destination[3] = {{UINT64_C(0x1111111111111111), UINT64_C(0x2222222222222222)},
                        {0, 0},
                        {UINT64_C(0x3333333333333333), UINT64_C(0x4444444444444444)}};

    /* Execute full 128-bit Q loads and stores with a scaled unsigned offset. */
    __asm__ volatile( "ldr q0, [%1, #16]\n\t"
                      "str q0, [%0]"
                      :: "r" (&loaded), "r" (&source[0]) : "v0", "memory" );
    CHECK( loaded.low == source[1].low && loaded.high == source[1].high );

    __asm__ volatile( "ldr q0, [%1]\n\t"
                      "str q0, [%0, #16]"
                      :: "r" (&destination[0]), "r" (&input) : "v0", "memory" );
    CHECK( destination[1].low == input.low && destination[1].high == input.high );
    CHECK( destination[0].low == UINT64_C(0x1111111111111111) &&
           destination[0].high == UINT64_C(0x2222222222222222) );
    CHECK( destination[2].low == UINT64_C(0x3333333333333333) &&
           destination[2].high == UINT64_C(0x4444444444444444) );
#endif
}

static void test_native_simd_d_post_index(void)
{
#if defined(__aarch64__)
    struct vector128
    {
        uint64_t low;
        uint64_t high;
    } loaded = {0, 0}, input = {UINT64_C(0x0123456789abcdef),
                                UINT64_C(0xfedcba9876543210)};
    struct destination
    {
        uint64_t value;
        uint64_t guard;
    } destination = {0, UINT64_C(0xa5a5a5a5a5a5a5a5)};
    uint64_t source = UINT64_C(0x8877665544332211);
    void *load_ptr = &source;
    void *store_ptr = &destination.value;

    /* Execute the native D-register semantics used by the signal bridge:
     * a D load zeroes the upper half of Vt, while a D store consumes only
     * the low 64 bits.  Both post-index forms update their base by eight. */
    __asm__ volatile( "movi v0.16b, #0xff\n\t"
                      "ldr d0, [%0], #8\n\t"
                      "str q0, [%1]"
                      : "+&r" (load_ptr)
                      : "r" (&loaded)
                      : "v0", "memory" );
    CHECK( load_ptr == (char *)&source + sizeof(source) );
    CHECK( loaded.low == source );
    CHECK( !loaded.high );

    __asm__ volatile( "ldr q0, [%1]\n\t"
                      "str d0, [%0], #8"
                      : "+r" (store_ptr)
                      : "r" (&input)
                      : "v0", "memory" );
    CHECK( store_ptr == (char *)&destination.value + sizeof(destination.value) );
    CHECK( destination.value == input.low );
    CHECK( destination.guard == UINT64_C(0xa5a5a5a5a5a5a5a5) );
#endif
}

static void test_fault_and_window_boundaries(void)
{
    const uint32_t load_x = UINT32_C(0xf9400062);
    struct arm64ec_low_guest_access access;

    /* An eight-byte operand may fault in either 4K page it spans. */
    CHECK( arm64ec_decode_low_guest_access( load_x, UINT64_C(0xffc), 0, UINT64_C(0xffc),
                                             false, LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( load_x, UINT64_C(0xffc), 0, UINT64_C(0x1000),
                                             false, LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( load_x, UINT64_C(0xffc), 0, UINT64_C(0x1003),
                                             false, LOW_LIMIT, &access ) );
    expect_rejected( load_x, UINT64_C(0xffc), 0, UINT64_C(0xffb), false, LOW_LIMIT );
    expect_rejected( load_x, UINT64_C(0xffc), 0, UINT64_C(0x1004), false, LOW_LIMIT );

    CHECK( arm64ec_decode_low_guest_access( load_x, LOW_LIMIT - 8, 0, LOW_LIMIT - 1,
                                             false, LOW_LIMIT, &access ) );
    expect_rejected( load_x, LOW_LIMIT - 7, 0, LOW_LIMIT - 1, false, LOW_LIMIT );
    expect_rejected( load_x, LOW_LIMIT, 0, LOW_LIMIT, false, LOW_LIMIT );
    expect_rejected( load_x, 0, 0, 0, false, LOW_LIMIT );

    /* Positive offset and native integer overflow are rejected before addition. */
    expect_rejected( UINT32_C(0xf9400462), LOW_LIMIT - 8, 0, LOW_LIMIT - 1,
                     false, LOW_LIMIT );
    expect_rejected( UINT32_C(0xf9400462), UINT64_MAX - 4, 0, UINT64_MAX - 1,
                     false, UINT64_MAX );

    /* Negative pre-index cannot wrap below the low window. */
    expect_rejected( make_unscaled( 3, true, 3, -8, 3, 2 ), 4, 0, 4,
                     false, LOW_LIMIT );
    CHECK( !arm64ec_decode_low_guest_access( load_x, UINT64_C(0x2000), 0,
                                              UINT64_C(0x2000), false, LOW_LIMIT, NULL ) );
}

static void test_register_offset_boundaries(void)
{
    const uint32_t load_x = make_register_offset( 3, true, 3, false, 8, 3, 2 );
    const uint32_t scaled_load_x = make_register_offset( 3, true, 3, true, 8, 3, 2 );
    const uint32_t signed_load_x = make_register_offset( 3, true, 7, false, 8, 3, 2 );
    struct arm64ec_low_guest_access access;
    uint64_t result;

    CHECK( arm64ec_decode_low_guest_access( load_x, UINT64_C(0xff8), 4,
                                             UINT64_C(0xffc), false, LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( load_x, UINT64_C(0xff8), 4,
                                             UINT64_C(0x1000), false, LOW_LIMIT, &access ) );
    CHECK( arm64ec_decode_low_guest_access( load_x, UINT64_C(0xff8), 4,
                                             UINT64_C(0x1003), false, LOW_LIMIT, &access ) );
    expect_rejected( load_x, UINT64_C(0xff8), 4, UINT64_C(0xffb), false, LOW_LIMIT );
    expect_rejected( load_x, UINT64_C(0xff8), 4, UINT64_C(0x1004), false, LOW_LIMIT );

    CHECK( arm64ec_decode_low_guest_access( load_x, LOW_LIMIT - 9, 1,
                                             LOW_LIMIT - 1, false, LOW_LIMIT, &access ) );
    expect_rejected( load_x, LOW_LIMIT - 8, 1, LOW_LIMIT - 1, false, LOW_LIMIT );
    expect_rejected( load_x, LOW_LIMIT - 8, 8, LOW_LIMIT - 1, false, LOW_LIMIT );
    expect_rejected( load_x, UINT64_MAX - 4, 8, UINT64_MAX - 1,
                     false, UINT64_MAX );
    expect_rejected( scaled_load_x, UINT64_C(0x2000), UINT64_C(0x2000000000000000),
                     UINT64_C(0x2000), false, LOW_LIMIT );
    expect_rejected( signed_load_x, 4, UINT64_C(0xfffffffffffffff8), 4,
                     false, LOW_LIMIT );
    expect_rejected( signed_load_x, UINT64_C(0x2000), UINT64_MAX,
                     UINT64_C(0x1ff8), true, LOW_LIMIT );

    CHECK( !arm64ec_low_guest_add_delta( 0, false, 0, 0, &result ) );
    CHECK( !arm64ec_low_guest_add_delta( UINT64_C(0x2000), false, 0,
                                          LOW_LIMIT, NULL ) );
}

static void test_translation_fault_class(void)
{
    unsigned int ec, level;

    for (ec = 0x24; ec <= 0x25; ++ec)
        for (level = 0; level < 4; ++level)
            CHECK( arm64ec_low_guest_is_translation_fault( ((uint64_t)ec << 26) |
                                                             UINT64_C(0x04) | level ) );

    CHECK( !arm64ec_low_guest_is_translation_fault( (UINT64_C(0x24) << 26) | 0x0c ) );
    CHECK( !arm64ec_low_guest_is_translation_fault( (UINT64_C(0x24) << 26) |
                                                     UINT64_C(0x04) | (UINT64_C(1) << 7) ) );
    CHECK( !arm64ec_low_guest_is_translation_fault( (UINT64_C(0x24) << 26) |
                                                     UINT64_C(0x04) | (UINT64_C(1) << 10) ) );
    CHECK( !arm64ec_low_guest_is_translation_fault( (UINT64_C(0x20) << 26) | 0x04 ) );
}

static void test_fixed_low_first_fault(void)
{
    /* The frozen pre-bridge failure is RtlImageNtHeader+0x34:
     * ldrh w8,[x8], with x8 and FAR both equal to guest 0x00400000. */
    const uint32_t instruction = UINT32_C(0x79400108);
    struct arm64ec_low_guest_access access;

    CHECK( arm64ec_low_guest_base_register( instruction ) == 8 );
    CHECK( arm64ec_decode_low_guest_access( instruction, UINT64_C(0x400000), 0,
                                             UINT64_C(0x400000), false,
                                             LOW_LIMIT, &access ) );
    CHECK( access.address == UINT64_C(0x400000) );
    CHECK( access.size == sizeof(uint16_t) );
    CHECK( access.rn == 8 );
    CHECK( access.rt == 8 );
    CHECK( !access.write );
    CHECK( !access.writeback_valid );
}

int main(void)
{
    test_unsigned_offset();
    test_unscaled_pre_post();
    test_register_offset();
    test_signed_scalar_load_decode();
    test_signed_load_extension();
    test_native_signed_load_extension();
    test_q_pair_offset();
    test_gpr_pair_offset();
    test_simd_q_unsigned_offset();
    test_native_simd_q_unsigned_offset();
    test_simd_d_post_index();
    test_native_simd_d_post_index();
    test_rejected_opcodes();
    test_fault_and_window_boundaries();
    test_register_offset_boundaries();
    test_translation_fault_class();
    test_fixed_low_first_fault();

    if (failures)
    {
        fprintf( stderr, "ARM64EC low guest decoder: %u failure(s)\n", failures );
        return 1;
    }
    puts( "ARM64EC low guest decoder native tests passed" );
    return 0;
}
