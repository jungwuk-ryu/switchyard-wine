/*
 * Winemetal Wow64 companion dispatch table
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <stdatomic.h>
#include <sys/mman.h>
#ifdef __APPLE__
# include <mach/mach.h>
# include <mach/mach_vm.h>
#endif

#include "winemetal_private.h"

struct wmt_backend_tables
{
    unixlib_entry_t normal[WMT_UNIX_CALL_COUNT];
    unixlib_entry_t legacy[WMT_UNIX_CALL_COUNT];
};

static struct wine_wow64_unixlib_binding_v4 backend_binding;
static const struct wmt_backend_tables *backend_tables;
static _Atomic unsigned int backend_state;

#if defined(__APPLE__) && defined(__aarch64__)
static BOOL backend_range_has_protection( const void *ptr, SIZE_T size, vm_prot_t required,
                                          vm_prot_t forbidden )
{
    mach_vm_address_t address = (UINT_PTR)ptr;
    mach_vm_address_t end;

    if (!ptr || !size || size > ~(mach_vm_address_t)0 - address) return FALSE;
    end = address + size;
    while (address < end)
    {
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        mach_vm_address_t region = address;
        mach_vm_size_t region_size;

        if (mach_vm_region( mach_task_self(), &region, &region_size,
                            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                            &count, &object ) != KERN_SUCCESS)
            return FALSE;
        if (object != MACH_PORT_NULL) mach_port_deallocate( mach_task_self(), object );
        if (region > address || !region_size || region_size > ~(mach_vm_address_t)0 - region ||
            (info.protection & required) != required || (info.protection & forbidden))
            return FALSE;
        address = region + region_size < end ? region + region_size : end;
    }
    return TRUE;
}

static BOOL backend_entries_are_valid( const struct wmt_backend_tables *tables )
{
    const void *image = NULL;
    Dl_info info;
    unsigned int i, table;

    for (i = 0; i < WMT_UNIX_CALL_COUNT; ++i)
    {
        for (table = 0; table < 2; ++table)
        {
            unixlib_entry_t entry = table ? tables->legacy[i] : tables->normal[i];

            if ((!entry) != (i == 83)) return FALSE;
            if (!entry) continue;
            if (!backend_range_has_protection( (const void *)entry, 1,
                                                VM_PROT_READ | VM_PROT_EXECUTE, 0 ) ||
                !dladdr( (const void *)entry, &info ))
                return FALSE;
            if (image && image != info.dli_fbase) return FALSE;
            image = info.dli_fbase;
        }
    }
    return image != NULL;
}
#else
static BOOL backend_range_has_protection( const void *ptr, SIZE_T size,
                                          unsigned int required, unsigned int forbidden )
{
    (void)ptr;
    (void)size;
    (void)required;
    (void)forbidden;
    return FALSE;
}

static BOOL backend_entries_are_valid( const struct wmt_backend_tables *tables )
{
    (void)tables;
    return FALSE;
}
#endif

static BOOL copy_backend_tables( const struct wine_wow64_unixlib_binding_v4 *binding,
                                 struct wmt_backend_tables *tables )
{
    struct wmt_backend_tables verification;

#if defined(__APPLE__) && defined(__aarch64__)
    if (!backend_range_has_protection( binding->normal_funcs,
                                       sizeof(tables->normal), VM_PROT_READ, 0 ) ||
        !backend_range_has_protection( binding->legacy_wow64_funcs,
                                       sizeof(tables->legacy), VM_PROT_READ, 0 ))
        return FALSE;
#else
    return FALSE;
#endif
    memcpy( tables->normal, binding->normal_funcs, sizeof(tables->normal) );
    memcpy( tables->legacy, binding->legacy_wow64_funcs, sizeof(tables->legacy) );
    memcpy( verification.normal, binding->normal_funcs, sizeof(verification.normal) );
    memcpy( verification.legacy, binding->legacy_wow64_funcs, sizeof(verification.legacy) );
    return !memcmp( tables, &verification, sizeof(*tables) ) &&
           backend_entries_are_valid( tables );
}

static BOOL binding_is_valid( const struct wine_wow64_unixlib_binding_v4 *binding )
{
    const struct wine_wow64_unixlib_codec_v2 *codec;
    const struct wine_unixlib_owned_backing_codec_v2 *owned_backing_codec;

    if (!binding || binding->version != WINE_WOW64_UNIXLIB_BINDING_V4_VERSION ||
        binding->size != sizeof(*binding) || binding->entry_count != WMT_UNIX_CALL_COUNT ||
        binding->reserved || !binding->normal_funcs || !binding->legacy_wow64_funcs ||
        !(codec = binding->codec) || !(owned_backing_codec = binding->owned_backing_codec))
        return FALSE;
    if (codec->version != WINE_WOW64_UNIXLIB_CODEC_V2_VERSION || codec->size != sizeof(*codec) ||
        codec->capabilities != WINE_WOW64_UNIXLIB_CAP_SEPARATE_GUEST_ADDRESS_SPACE ||
        !codec->translate || !codec->copy_from_guest || !codec->copy_to_guest)
        return FALSE;
    if (owned_backing_codec->version != WINE_UNIXLIB_OWNED_BACKING_CODEC_V2_VERSION ||
        owned_backing_codec->size != sizeof(*owned_backing_codec) ||
        owned_backing_codec->capabilities !=
            WINE_UNIXLIB_OWNED_BACKING_CAP_ACQUIRE_RELEASE ||
        !owned_backing_codec->acquire_backing || !owned_backing_codec->release_backing)
        return FALSE;
    return TRUE;
}

static BOOL binding_codecs_are_equal( const struct wine_wow64_unixlib_binding_v4 *left,
                                      const struct wine_wow64_unixlib_binding_v4 *right )
{
    return left->version == right->version && left->size == right->size &&
           left->entry_count == right->entry_count && left->reserved == right->reserved &&
           left->codec == right->codec &&
           left->owned_backing_codec == right->owned_backing_codec;
}

static NTSTATUS bind_backend( const struct wine_wow64_unixlib_binding_v4 *binding )
{
    struct wine_wow64_unixlib_binding_v4 source;
    struct wmt_backend_tables *tables, candidate;
    unsigned int expected = 0, state;

    if (!binding) return STATUS_INVALID_PARAMETER;
    source = *binding;
    if (!binding_is_valid( &source )) return STATUS_INVALID_PARAMETER;
    if (atomic_compare_exchange_strong_explicit( &backend_state, &expected, 1,
                                                 memory_order_acq_rel, memory_order_acquire ))
    {
        if ((tables = mmap( NULL, sizeof(*tables), PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0 )) == MAP_FAILED)
        {
            atomic_store_explicit( &backend_state, 0, memory_order_release );
            return STATUS_NO_MEMORY;
        }
        if (!copy_backend_tables( &source, tables ))
        {
            munmap( tables, sizeof(*tables) );
            atomic_store_explicit( &backend_state, 0, memory_order_release );
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        if (mprotect( tables, sizeof(*tables), PROT_READ ) ||
#if defined(__APPLE__) && defined(__aarch64__)
            !backend_range_has_protection( tables, sizeof(*tables), VM_PROT_READ,
                                           VM_PROT_WRITE ))
#else
            TRUE)
#endif
        {
            munmap( tables, sizeof(*tables) );
            atomic_store_explicit( &backend_state, 0, memory_order_release );
            return STATUS_UNSUCCESSFUL;
        }
        backend_binding = source;
        backend_binding.normal_funcs = tables->normal;
        backend_binding.legacy_wow64_funcs = tables->legacy;
        backend_tables = tables;
        atomic_store_explicit( &backend_state, 2, memory_order_release );
        return STATUS_SUCCESS;
    }
    state = atomic_load_explicit( &backend_state, memory_order_acquire );
    if (state == 2 && binding_codecs_are_equal( &backend_binding, &source ) &&
        copy_backend_tables( &source, &candidate ) &&
        !memcmp( backend_tables, &candidate, sizeof(candidate) ))
        return STATUS_SUCCESS;
    return STATUS_INVALID_DEVICE_STATE;
}

static NTSTATUS release_backend_snapshot(void)
{
    const struct wmt_backend_tables *tables;
    int ret;

    if (atomic_load_explicit( &backend_state, memory_order_acquire ) != 2)
        return STATUS_INVALID_DEVICE_STATE;
    atomic_store_explicit( &backend_state, 1, memory_order_release );
    tables = backend_tables;
    backend_tables = NULL;
    memset( &backend_binding, 0, sizeof(backend_binding) );
    ret = munmap( (void *)tables, sizeof(*tables) );
    atomic_store_explicit( &backend_state, 0, memory_order_release );
    return ret ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

/* The loader drains registered calls before releasing the companion image. */
static void __attribute__((destructor)) unmap_backend_snapshot(void)
{
    if (atomic_load_explicit( &backend_state, memory_order_acquire ) == 2)
        release_backend_snapshot();
}

#ifdef WMT_NATIVE_TEST
NTSTATUS wmt_test_release_backend_snapshot(void)
{
    return release_backend_snapshot();
}
#endif

const struct wine_unixlib_owned_backing_codec_v2 *wmt_get_owned_backing_codec(void)
{
    if (atomic_load_explicit( &backend_state, memory_order_acquire ) != 2) return NULL;
    return backend_binding.owned_backing_codec;
}

NTSTATUS wmt_forward_call( unsigned int index, void *args )
{
    unixlib_entry_t entry;

    if (index >= WMT_UNIX_CALL_COUNT ||
        atomic_load_explicit( &backend_state, memory_order_acquire ) != 2)
        return STATUS_INVALID_DEVICE_STATE;
    if (!(entry = backend_tables->legacy[index])) return STATUS_NOT_SUPPORTED;
    return entry( args );
}

NTSTATUS wmt_normal_call( unsigned int index, void *args )
{
    unixlib_entry_t entry;

    if (index >= WMT_UNIX_CALL_COUNT ||
        atomic_load_explicit( &backend_state, memory_order_acquire ) != 2)
        return STATUS_INVALID_DEVICE_STATE;
    if (!(entry = backend_tables->normal[index])) return STATUS_NOT_SUPPORTED;
    return entry( args );
}

#define DEFINE_FORWARD(index) \
    static NTSTATUS forward_##index( void *args ) { return wmt_forward_call( index, args ); }

DEFINE_FORWARD(0)   DEFINE_FORWARD(1)   DEFINE_FORWARD(2)   DEFINE_FORWARD(3)
DEFINE_FORWARD(4)   DEFINE_FORWARD(5)   DEFINE_FORWARD(6)   DEFINE_FORWARD(7)
DEFINE_FORWARD(9)   DEFINE_FORWARD(10)  DEFINE_FORWARD(11)  DEFINE_FORWARD(12)
DEFINE_FORWARD(13)  DEFINE_FORWARD(14)  DEFINE_FORWARD(15)  DEFINE_FORWARD(16)
DEFINE_FORWARD(17)  DEFINE_FORWARD(23)  DEFINE_FORWARD(24)  DEFINE_FORWARD(25)
DEFINE_FORWARD(27)  DEFINE_FORWARD(28)  DEFINE_FORWARD(30)  DEFINE_FORWARD(31)
DEFINE_FORWARD(33)  DEFINE_FORWARD(39)  DEFINE_FORWARD(40)  DEFINE_FORWARD(41)
DEFINE_FORWARD(42)  DEFINE_FORWARD(43)  DEFINE_FORWARD(44)  DEFINE_FORWARD(46)
DEFINE_FORWARD(47)  DEFINE_FORWARD(48)  DEFINE_FORWARD(49)  DEFINE_FORWARD(50)
DEFINE_FORWARD(51)  DEFINE_FORWARD(52)  DEFINE_FORWARD(53)  DEFINE_FORWARD(55)
DEFINE_FORWARD(59)  DEFINE_FORWARD(62)  DEFINE_FORWARD(63)  DEFINE_FORWARD(64)
DEFINE_FORWARD(65)  DEFINE_FORWARD(66)  DEFINE_FORWARD(67)  DEFINE_FORWARD(68)
DEFINE_FORWARD(69)  DEFINE_FORWARD(72)  DEFINE_FORWARD(73)
DEFINE_FORWARD(75)  DEFINE_FORWARD(78)  DEFINE_FORWARD(80)  DEFINE_FORWARD(86)
DEFINE_FORWARD(87)  DEFINE_FORWARD(89)  DEFINE_FORWARD(90)  DEFINE_FORWARD(92)
DEFINE_FORWARD(93)  DEFINE_FORWARD(94)  DEFINE_FORWARD(95)  DEFINE_FORWARD(102)
DEFINE_FORWARD(103) DEFINE_FORWARD(104) DEFINE_FORWARD(105) DEFINE_FORWARD(106)
DEFINE_FORWARD(108) DEFINE_FORWARD(109) DEFINE_FORWARD(110) DEFINE_FORWARD(111)
DEFINE_FORWARD(121) DEFINE_FORWARD(122) DEFINE_FORWARD(123) DEFINE_FORWARD(124)
DEFINE_FORWARD(125) DEFINE_FORWARD(126) DEFINE_FORWARD(127) DEFINE_FORWARD(130)
DEFINE_FORWARD(132) DEFINE_FORWARD(135) DEFINE_FORWARD(136) DEFINE_FORWARD(137)

#undef DEFINE_FORWARD

/*
 * Keep this range statically initialized and const.  The loader validates that
 * the full 138-entry symbol is mapped read-only before publishing it.
 */
static const unixlib_entry_t wmt_implementation_funcs[WMT_UNIX_CALL_COUNT] =
{
    [0] = forward_0, [1] = forward_1, [2] = forward_2, [3] = forward_3,
    [4] = forward_4, [5] = forward_5, [6] = forward_6, [7] = forward_7,
    [8] = wmt_unsupported_8,
    [9] = forward_9, [10] = forward_10, [11] = forward_11, [12] = forward_12,
    [13] = forward_13, [14] = forward_14, [15] = forward_15, [16] = forward_16,
    [17] = forward_17,
    [18] = wmt_call_18, [19] = wmt_call_19, [20] = wmt_call_20, [21] = wmt_call_21,
    [22] = wmt_unsupported_22,
    [23] = forward_23, [24] = forward_24, [25] = forward_25,
    [26] = wmt_call_26, [27] = forward_27, [28] = forward_28, [29] = wmt_call_29,
    [30] = forward_30, [31] = forward_31, [32] = wmt_call_32, [33] = forward_33,
    [34] = wmt_call_34, [35] = wmt_unsupported_pipeline,
    [36] = wmt_call_36, [37] = wmt_unsupported_no_output, [38] = wmt_call_38,
    [39] = forward_39, [40] = forward_40, [41] = forward_41, [42] = forward_42,
    [43] = forward_43, [44] = forward_44, [45] = wmt_unsupported_no_output,
    [46] = forward_46, [47] = forward_47, [48] = forward_48, [49] = forward_49,
    [50] = forward_50, [51] = forward_51, [52] = forward_52, [53] = forward_53,
    [54] = wmt_unsupported_capture, [55] = forward_55,
    [56] = wmt_unsupported_info_ret, [57] = wmt_unsupported_info_ret,
    [58] = wmt_unsupported_no_output, [59] = forward_59,
    [60] = wmt_call_60, [61] = wmt_call_61,
    [62] = forward_62, [63] = forward_63, [64] = forward_64, [65] = forward_65,
    [66] = forward_66, [67] = forward_67, [68] = forward_68, [69] = forward_69,
    [70] = wmt_call_70, [71] = wmt_call_71, [72] = forward_72, [73] = forward_73,
    /* AIR pointer schemas are unsafe in the high shadow; opaque destroy/free calls remain safe. */
    [74] = wmt_unsupported_air_initialize, [75] = forward_75,
    [76] = wmt_unsupported_air_compile, [77] = wmt_unsupported_air_get_bitcode,
    [78] = forward_78, [79] = wmt_unsupported_air_get_error, [80] = forward_80,
    [81] = wmt_unsupported_air_pipeline_compile,
    [82] = wmt_unsupported_air_pipeline_compile, [83] = wmt_unsupported_no_output,
    [84] = wmt_unsupported_air_pipeline_compile,
    [85] = wmt_unsupported_air_pipeline_compile, [86] = forward_86, [87] = forward_87,
    [88] = wmt_unsupported_air_get_arguments, [89] = forward_89, [90] = forward_90,
    [91] = wmt_unsupported_enumerate,
    [92] = forward_92, [93] = forward_93, [94] = forward_94, [95] = forward_95,
    [96] = wmt_unsupported_display_description,
    [97] = wmt_call_97, [98] = wmt_call_98,
    [99] = wmt_unsupported_query_display, [100] = wmt_unsupported_no_output,
    [101] = wmt_call_101,
    [102] = forward_102, [103] = forward_103, [104] = forward_104,
    [105] = forward_105, [106] = forward_106,
    [107] = wmt_unsupported_no_output,
    [108] = forward_108, [109] = forward_109, [110] = forward_110, [111] = forward_111,
    [112] = wmt_unsupported_archive, [113] = wmt_unsupported_archive_serialize,
    [114] = wmt_call_114, [115] = wmt_call_115,
    [116] = wmt_unsupported_cache_get, [117] = wmt_call_117,
    [118] = wmt_unsupported_no_output, [119] = wmt_call_119,
    [120] = wmt_unsupported_shared_texture,
    [121] = forward_121, [122] = forward_122, [123] = forward_123,
    [124] = forward_124, [125] = forward_125, [126] = forward_126, [127] = forward_127,
    [128] = wmt_unsupported_resolve_counter_range,
    [129] = wmt_unsupported_sample_buffer_encoder,
    [130] = forward_130, [131] = wmt_unsupported_pipeline, [132] = forward_132,
    [133] = wmt_unsupported_no_output, [134] = wmt_unsupported_no_output,
    [135] = forward_135, [136] = forward_136, [137] = forward_137,
};

C_ASSERT( ARRAY_SIZE(wmt_implementation_funcs) == WMT_UNIX_CALL_COUNT );
struct wmt_dispatch_args_8 { unsigned char bytes[8]; };
struct wmt_dispatch_args_16 { unsigned char bytes[16]; };
struct wmt_dispatch_args_24 { unsigned char bytes[24]; };
struct wmt_dispatch_args_32 { unsigned char bytes[32]; };
struct wmt_dispatch_args_40 { unsigned char bytes[40]; };
struct wmt_dispatch_args_48 { unsigned char bytes[48]; };
struct wmt_dispatch_args_72 { unsigned char bytes[72]; };
struct wmt_dispatch_args_96 { unsigned char bytes[96]; };
struct wmt_dispatch_args_136 { unsigned char bytes[136]; };

static const struct wine_unixlib_dispatch_entry_v2 wow64_dispatch_metadata[] =
{
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_40, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_MAY_CALLBACK |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_RETAINS_ADDRESS ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_40, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_48, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_96, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_72, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_40, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_40, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_40, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    { 0, WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED },
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_40, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_40, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_40, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_48, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_40, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_MAY_CALLBACK ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_136, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_136, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                   WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_32, WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_24, WINE_UNIXLIB_DISPATCH_ENTRY_NESTED ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_8, 0 ),
    WINE_UNIXLIB_DISPATCH_ARGS_V2( struct wmt_dispatch_args_16, 0 ),
};

static NTSTATUS wmt_dispatch_call( unsigned int index, void *args )
{
    const struct wine_unixlib_dispatch_entry_v2 *metadata;
    struct ntdll_wow64_unixlib_call_context context;
    void *guest_args;
    NTSTATUS publish_status, status;

    if (index >= WMT_UNIX_CALL_COUNT) return STATUS_INVALID_PARAMETER;
    metadata = &wow64_dispatch_metadata[index];
    if ((status = ntdll_wow64_get_unixlib_call_context( &context ))) return status;
    if (context.args_size != metadata->args_size || context.flags != metadata->flags ||
        (context.args_size && !args) ||
        (UINT_PTR)context.guest_args > UINT32_MAX)
        return STATUS_INVALID_PARAMETER;

    if (context.args_size)
    {
        if ((status = ntdll_wow64_guest32_to_host( (ULONG)(UINT_PTR)context.guest_args,
                                                   &guest_args )))
            return status;
        if ((status = ntdll_wow64_probe_user_write( guest_args, context.args_size )))
            return status;
    }

    status = wmt_implementation_funcs[index]( args );
    if (!context.args_size) return status;
    publish_status = ntdll_wow64_atomic_write_user( guest_args, args, context.args_size );
    if (!publish_status) return status;
    if (index == WMT_CALL_NEW_BUFFER) return wmt_rollback_18( args, publish_status );
    return publish_status;
}

static NTSTATUS dispatch_0( void *args ) { return wmt_dispatch_call( 0, args ); }
static NTSTATUS dispatch_1( void *args ) { return wmt_dispatch_call( 1, args ); }
static NTSTATUS dispatch_2( void *args ) { return wmt_dispatch_call( 2, args ); }
static NTSTATUS dispatch_3( void *args ) { return wmt_dispatch_call( 3, args ); }
static NTSTATUS dispatch_4( void *args ) { return wmt_dispatch_call( 4, args ); }
static NTSTATUS dispatch_5( void *args ) { return wmt_dispatch_call( 5, args ); }
static NTSTATUS dispatch_6( void *args ) { return wmt_dispatch_call( 6, args ); }
static NTSTATUS dispatch_7( void *args ) { return wmt_dispatch_call( 7, args ); }
static NTSTATUS dispatch_8( void *args ) { return wmt_dispatch_call( 8, args ); }
static NTSTATUS dispatch_9( void *args ) { return wmt_dispatch_call( 9, args ); }
static NTSTATUS dispatch_10( void *args ) { return wmt_dispatch_call( 10, args ); }
static NTSTATUS dispatch_11( void *args ) { return wmt_dispatch_call( 11, args ); }
static NTSTATUS dispatch_12( void *args ) { return wmt_dispatch_call( 12, args ); }
static NTSTATUS dispatch_13( void *args ) { return wmt_dispatch_call( 13, args ); }
static NTSTATUS dispatch_14( void *args ) { return wmt_dispatch_call( 14, args ); }
static NTSTATUS dispatch_15( void *args ) { return wmt_dispatch_call( 15, args ); }
static NTSTATUS dispatch_16( void *args ) { return wmt_dispatch_call( 16, args ); }
static NTSTATUS dispatch_17( void *args ) { return wmt_dispatch_call( 17, args ); }
static NTSTATUS dispatch_18( void *args ) { return wmt_dispatch_call( 18, args ); }
static NTSTATUS dispatch_19( void *args ) { return wmt_dispatch_call( 19, args ); }
static NTSTATUS dispatch_20( void *args ) { return wmt_dispatch_call( 20, args ); }
static NTSTATUS dispatch_21( void *args ) { return wmt_dispatch_call( 21, args ); }
static NTSTATUS dispatch_22( void *args ) { return wmt_dispatch_call( 22, args ); }
static NTSTATUS dispatch_23( void *args ) { return wmt_dispatch_call( 23, args ); }
static NTSTATUS dispatch_24( void *args ) { return wmt_dispatch_call( 24, args ); }
static NTSTATUS dispatch_25( void *args ) { return wmt_dispatch_call( 25, args ); }
static NTSTATUS dispatch_26( void *args ) { return wmt_dispatch_call( 26, args ); }
static NTSTATUS dispatch_27( void *args ) { return wmt_dispatch_call( 27, args ); }
static NTSTATUS dispatch_28( void *args ) { return wmt_dispatch_call( 28, args ); }
static NTSTATUS dispatch_29( void *args ) { return wmt_dispatch_call( 29, args ); }
static NTSTATUS dispatch_30( void *args ) { return wmt_dispatch_call( 30, args ); }
static NTSTATUS dispatch_31( void *args ) { return wmt_dispatch_call( 31, args ); }
static NTSTATUS dispatch_32( void *args ) { return wmt_dispatch_call( 32, args ); }
static NTSTATUS dispatch_33( void *args ) { return wmt_dispatch_call( 33, args ); }
static NTSTATUS dispatch_34( void *args ) { return wmt_dispatch_call( 34, args ); }
static NTSTATUS dispatch_35( void *args ) { return wmt_dispatch_call( 35, args ); }
static NTSTATUS dispatch_36( void *args ) { return wmt_dispatch_call( 36, args ); }
static NTSTATUS dispatch_37( void *args ) { return wmt_dispatch_call( 37, args ); }
static NTSTATUS dispatch_38( void *args ) { return wmt_dispatch_call( 38, args ); }
static NTSTATUS dispatch_39( void *args ) { return wmt_dispatch_call( 39, args ); }
static NTSTATUS dispatch_40( void *args ) { return wmt_dispatch_call( 40, args ); }
static NTSTATUS dispatch_41( void *args ) { return wmt_dispatch_call( 41, args ); }
static NTSTATUS dispatch_42( void *args ) { return wmt_dispatch_call( 42, args ); }
static NTSTATUS dispatch_43( void *args ) { return wmt_dispatch_call( 43, args ); }
static NTSTATUS dispatch_44( void *args ) { return wmt_dispatch_call( 44, args ); }
static NTSTATUS dispatch_45( void *args ) { return wmt_dispatch_call( 45, args ); }
static NTSTATUS dispatch_46( void *args ) { return wmt_dispatch_call( 46, args ); }
static NTSTATUS dispatch_47( void *args ) { return wmt_dispatch_call( 47, args ); }
static NTSTATUS dispatch_48( void *args ) { return wmt_dispatch_call( 48, args ); }
static NTSTATUS dispatch_49( void *args ) { return wmt_dispatch_call( 49, args ); }
static NTSTATUS dispatch_50( void *args ) { return wmt_dispatch_call( 50, args ); }
static NTSTATUS dispatch_51( void *args ) { return wmt_dispatch_call( 51, args ); }
static NTSTATUS dispatch_52( void *args ) { return wmt_dispatch_call( 52, args ); }
static NTSTATUS dispatch_53( void *args ) { return wmt_dispatch_call( 53, args ); }
static NTSTATUS dispatch_54( void *args ) { return wmt_dispatch_call( 54, args ); }
static NTSTATUS dispatch_55( void *args ) { return wmt_dispatch_call( 55, args ); }
static NTSTATUS dispatch_56( void *args ) { return wmt_dispatch_call( 56, args ); }
static NTSTATUS dispatch_57( void *args ) { return wmt_dispatch_call( 57, args ); }
static NTSTATUS dispatch_58( void *args ) { return wmt_dispatch_call( 58, args ); }
static NTSTATUS dispatch_59( void *args ) { return wmt_dispatch_call( 59, args ); }
static NTSTATUS dispatch_60( void *args ) { return wmt_dispatch_call( 60, args ); }
static NTSTATUS dispatch_61( void *args ) { return wmt_dispatch_call( 61, args ); }
static NTSTATUS dispatch_62( void *args ) { return wmt_dispatch_call( 62, args ); }
static NTSTATUS dispatch_63( void *args ) { return wmt_dispatch_call( 63, args ); }
static NTSTATUS dispatch_64( void *args ) { return wmt_dispatch_call( 64, args ); }
static NTSTATUS dispatch_65( void *args ) { return wmt_dispatch_call( 65, args ); }
static NTSTATUS dispatch_66( void *args ) { return wmt_dispatch_call( 66, args ); }
static NTSTATUS dispatch_67( void *args ) { return wmt_dispatch_call( 67, args ); }
static NTSTATUS dispatch_68( void *args ) { return wmt_dispatch_call( 68, args ); }
static NTSTATUS dispatch_69( void *args ) { return wmt_dispatch_call( 69, args ); }
static NTSTATUS dispatch_70( void *args ) { return wmt_dispatch_call( 70, args ); }
static NTSTATUS dispatch_71( void *args ) { return wmt_dispatch_call( 71, args ); }
static NTSTATUS dispatch_72( void *args ) { return wmt_dispatch_call( 72, args ); }
static NTSTATUS dispatch_73( void *args ) { return wmt_dispatch_call( 73, args ); }
static NTSTATUS dispatch_74( void *args ) { return wmt_dispatch_call( 74, args ); }
static NTSTATUS dispatch_75( void *args ) { return wmt_dispatch_call( 75, args ); }
static NTSTATUS dispatch_76( void *args ) { return wmt_dispatch_call( 76, args ); }
static NTSTATUS dispatch_77( void *args ) { return wmt_dispatch_call( 77, args ); }
static NTSTATUS dispatch_78( void *args ) { return wmt_dispatch_call( 78, args ); }
static NTSTATUS dispatch_79( void *args ) { return wmt_dispatch_call( 79, args ); }
static NTSTATUS dispatch_80( void *args ) { return wmt_dispatch_call( 80, args ); }
static NTSTATUS dispatch_81( void *args ) { return wmt_dispatch_call( 81, args ); }
static NTSTATUS dispatch_82( void *args ) { return wmt_dispatch_call( 82, args ); }
static NTSTATUS dispatch_83( void *args ) { return wmt_dispatch_call( 83, args ); }
static NTSTATUS dispatch_84( void *args ) { return wmt_dispatch_call( 84, args ); }
static NTSTATUS dispatch_85( void *args ) { return wmt_dispatch_call( 85, args ); }
static NTSTATUS dispatch_86( void *args ) { return wmt_dispatch_call( 86, args ); }
static NTSTATUS dispatch_87( void *args ) { return wmt_dispatch_call( 87, args ); }
static NTSTATUS dispatch_88( void *args ) { return wmt_dispatch_call( 88, args ); }
static NTSTATUS dispatch_89( void *args ) { return wmt_dispatch_call( 89, args ); }
static NTSTATUS dispatch_90( void *args ) { return wmt_dispatch_call( 90, args ); }
static NTSTATUS dispatch_91( void *args ) { return wmt_dispatch_call( 91, args ); }
static NTSTATUS dispatch_92( void *args ) { return wmt_dispatch_call( 92, args ); }
static NTSTATUS dispatch_93( void *args ) { return wmt_dispatch_call( 93, args ); }
static NTSTATUS dispatch_94( void *args ) { return wmt_dispatch_call( 94, args ); }
static NTSTATUS dispatch_95( void *args ) { return wmt_dispatch_call( 95, args ); }
static NTSTATUS dispatch_96( void *args ) { return wmt_dispatch_call( 96, args ); }
static NTSTATUS dispatch_97( void *args ) { return wmt_dispatch_call( 97, args ); }
static NTSTATUS dispatch_98( void *args ) { return wmt_dispatch_call( 98, args ); }
static NTSTATUS dispatch_99( void *args ) { return wmt_dispatch_call( 99, args ); }
static NTSTATUS dispatch_100( void *args ) { return wmt_dispatch_call( 100, args ); }
static NTSTATUS dispatch_101( void *args ) { return wmt_dispatch_call( 101, args ); }
static NTSTATUS dispatch_102( void *args ) { return wmt_dispatch_call( 102, args ); }
static NTSTATUS dispatch_103( void *args ) { return wmt_dispatch_call( 103, args ); }
static NTSTATUS dispatch_104( void *args ) { return wmt_dispatch_call( 104, args ); }
static NTSTATUS dispatch_105( void *args ) { return wmt_dispatch_call( 105, args ); }
static NTSTATUS dispatch_106( void *args ) { return wmt_dispatch_call( 106, args ); }
static NTSTATUS dispatch_107( void *args ) { return wmt_dispatch_call( 107, args ); }
static NTSTATUS dispatch_108( void *args ) { return wmt_dispatch_call( 108, args ); }
static NTSTATUS dispatch_109( void *args ) { return wmt_dispatch_call( 109, args ); }
static NTSTATUS dispatch_110( void *args ) { return wmt_dispatch_call( 110, args ); }
static NTSTATUS dispatch_111( void *args ) { return wmt_dispatch_call( 111, args ); }
static NTSTATUS dispatch_112( void *args ) { return wmt_dispatch_call( 112, args ); }
static NTSTATUS dispatch_113( void *args ) { return wmt_dispatch_call( 113, args ); }
static NTSTATUS dispatch_114( void *args ) { return wmt_dispatch_call( 114, args ); }
static NTSTATUS dispatch_115( void *args ) { return wmt_dispatch_call( 115, args ); }
static NTSTATUS dispatch_116( void *args ) { return wmt_dispatch_call( 116, args ); }
static NTSTATUS dispatch_117( void *args ) { return wmt_dispatch_call( 117, args ); }
static NTSTATUS dispatch_118( void *args ) { return wmt_dispatch_call( 118, args ); }
static NTSTATUS dispatch_119( void *args ) { return wmt_dispatch_call( 119, args ); }
static NTSTATUS dispatch_120( void *args ) { return wmt_dispatch_call( 120, args ); }
static NTSTATUS dispatch_121( void *args ) { return wmt_dispatch_call( 121, args ); }
static NTSTATUS dispatch_122( void *args ) { return wmt_dispatch_call( 122, args ); }
static NTSTATUS dispatch_123( void *args ) { return wmt_dispatch_call( 123, args ); }
static NTSTATUS dispatch_124( void *args ) { return wmt_dispatch_call( 124, args ); }
static NTSTATUS dispatch_125( void *args ) { return wmt_dispatch_call( 125, args ); }
static NTSTATUS dispatch_126( void *args ) { return wmt_dispatch_call( 126, args ); }
static NTSTATUS dispatch_127( void *args ) { return wmt_dispatch_call( 127, args ); }
static NTSTATUS dispatch_128( void *args ) { return wmt_dispatch_call( 128, args ); }
static NTSTATUS dispatch_129( void *args ) { return wmt_dispatch_call( 129, args ); }
static NTSTATUS dispatch_130( void *args ) { return wmt_dispatch_call( 130, args ); }
static NTSTATUS dispatch_131( void *args ) { return wmt_dispatch_call( 131, args ); }
static NTSTATUS dispatch_132( void *args ) { return wmt_dispatch_call( 132, args ); }
static NTSTATUS dispatch_133( void *args ) { return wmt_dispatch_call( 133, args ); }
static NTSTATUS dispatch_134( void *args ) { return wmt_dispatch_call( 134, args ); }
static NTSTATUS dispatch_135( void *args ) { return wmt_dispatch_call( 135, args ); }
static NTSTATUS dispatch_136( void *args ) { return wmt_dispatch_call( 136, args ); }
static NTSTATUS dispatch_137( void *args ) { return wmt_dispatch_call( 137, args ); }

DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    dispatch_0,
    dispatch_1,
    dispatch_2,
    dispatch_3,
    dispatch_4,
    dispatch_5,
    dispatch_6,
    dispatch_7,
    dispatch_8,
    dispatch_9,
    dispatch_10,
    dispatch_11,
    dispatch_12,
    dispatch_13,
    dispatch_14,
    dispatch_15,
    dispatch_16,
    dispatch_17,
    dispatch_18,
    dispatch_19,
    dispatch_20,
    dispatch_21,
    dispatch_22,
    dispatch_23,
    dispatch_24,
    dispatch_25,
    dispatch_26,
    dispatch_27,
    dispatch_28,
    dispatch_29,
    dispatch_30,
    dispatch_31,
    dispatch_32,
    dispatch_33,
    dispatch_34,
    dispatch_35,
    dispatch_36,
    dispatch_37,
    dispatch_38,
    dispatch_39,
    dispatch_40,
    dispatch_41,
    dispatch_42,
    dispatch_43,
    dispatch_44,
    dispatch_45,
    dispatch_46,
    dispatch_47,
    dispatch_48,
    dispatch_49,
    dispatch_50,
    dispatch_51,
    dispatch_52,
    dispatch_53,
    dispatch_54,
    dispatch_55,
    dispatch_56,
    dispatch_57,
    dispatch_58,
    dispatch_59,
    dispatch_60,
    dispatch_61,
    dispatch_62,
    dispatch_63,
    dispatch_64,
    dispatch_65,
    dispatch_66,
    dispatch_67,
    dispatch_68,
    dispatch_69,
    dispatch_70,
    dispatch_71,
    dispatch_72,
    dispatch_73,
    dispatch_74,
    dispatch_75,
    dispatch_76,
    dispatch_77,
    dispatch_78,
    dispatch_79,
    dispatch_80,
    dispatch_81,
    dispatch_82,
    dispatch_83,
    dispatch_84,
    dispatch_85,
    dispatch_86,
    dispatch_87,
    dispatch_88,
    dispatch_89,
    dispatch_90,
    dispatch_91,
    dispatch_92,
    dispatch_93,
    dispatch_94,
    dispatch_95,
    dispatch_96,
    dispatch_97,
    dispatch_98,
    dispatch_99,
    dispatch_100,
    dispatch_101,
    dispatch_102,
    dispatch_103,
    dispatch_104,
    dispatch_105,
    dispatch_106,
    dispatch_107,
    dispatch_108,
    dispatch_109,
    dispatch_110,
    dispatch_111,
    dispatch_112,
    dispatch_113,
    dispatch_114,
    dispatch_115,
    dispatch_116,
    dispatch_117,
    dispatch_118,
    dispatch_119,
    dispatch_120,
    dispatch_121,
    dispatch_122,
    dispatch_123,
    dispatch_124,
    dispatch_125,
    dispatch_126,
    dispatch_127,
    dispatch_128,
    dispatch_129,
    dispatch_130,
    dispatch_131,
    dispatch_132,
    dispatch_133,
    dispatch_134,
    dispatch_135,
    dispatch_136,
    dispatch_137,
};

WINE_UNIXLIB_DISPATCH_SOURCE_V2(__wine_unix_call_wow64_funcs, wow64_dispatch_metadata);

C_ASSERT( ARRAY_SIZE(__wine_unix_call_wow64_funcs) == WMT_UNIX_CALL_COUNT );
C_ASSERT( ARRAY_SIZE(wow64_dispatch_metadata) == WMT_UNIX_CALL_COUNT );


C_ASSERT( sizeof(struct wine_wow64_unixlib_alias_v2) == 48 );
C_ASSERT( sizeof(struct wine_wow64_unixlib_codec_v2) == 56 );
C_ASSERT( offsetof(struct wine_wow64_unixlib_codec_v2, translate) == 16 );
C_ASSERT( offsetof(struct wine_wow64_unixlib_codec_v2, acquire_alias) == 40 );
C_ASSERT( sizeof(struct wine_wow64_unixlib_binding_v4) == 48 );
C_ASSERT( offsetof(struct wine_wow64_unixlib_binding_v4, normal_funcs) == 16 );
C_ASSERT( offsetof(struct wine_wow64_unixlib_binding_v4, codec) == 32 );
C_ASSERT( offsetof(struct wine_wow64_unixlib_binding_v4, owned_backing_codec) == 40 );
C_ASSERT( sizeof(struct wine_wow64_unixlib_companion_v4) == 56 );
C_ASSERT( offsetof(struct wine_wow64_unixlib_companion_v4, abi_sha256) == 16 );
C_ASSERT( offsetof(struct wine_wow64_unixlib_companion_v4, bind) == 48 );
C_ASSERT( sizeof(struct wine_unixlib_owned_backing_v2) == 56 );
C_ASSERT( offsetof(struct wine_unixlib_owned_backing_v2, guest_address) == 48 );
C_ASSERT( sizeof(struct wine_unixlib_owned_backing_codec_v2) == 32 );
C_ASSERT( offsetof(struct wine_unixlib_owned_backing_codec_v2, acquire_backing) == 16 );
C_ASSERT( offsetof(struct wine_unixlib_owned_backing_codec_v2, release_backing) == 24 );

/* SHA-256 of abi-schema-v4.txt exact bytes (ASCII, LF, final newline). */
DECLSPEC_EXPORT const struct wine_wow64_unixlib_companion_v4
__wine_unix_call_wow64_companion_v4 =
{
    WINE_WOW64_UNIXLIB_COMPANION_V4_VERSION,
    sizeof(struct wine_wow64_unixlib_companion_v4),
    WMT_UNIX_CALL_COUNT,
    0,
    WINE_WOW64_UNIXLIB_COMPANION_V4_ABI_SHA256,
    bind_backend,
};
