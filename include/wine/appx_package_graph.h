/*
 * Pointer-free AppX package graph wire format
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_APPX_PACKAGE_GRAPH_H
#define __WINE_APPX_PACKAGE_GRAPH_H

#include <stddef.h>

/*
 * The graph itself contains only fixed-width little-endian values and
 * offsets.  These constants are shared by the process transport and the
 * graph consumer so cross-bitness startup never serializes native pointers.
 */
#define WINE_APPX_GRAPH_BLOB_VERSION                 5u
#define WINE_APPX_GRAPH_BLOB_HEADER_SIZE             144u
#define WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE     112u
#define WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE      72u
#define WINE_APPX_GRAPH_BLOB_CLASS_RECORD_SIZE       56u
#define WINE_APPX_GRAPH_OBJECT_ID_SIZE                16u
#define WINE_APPX_GRAPH_MAX_BLOB_SIZE                (16u * 1024u * 1024u)
#define WINE_APPX_GRAPH_MAX_PACKAGES                 256u
#define WINE_APPX_GRAPH_MAX_LOADER_FILES             16384u
#define WINE_APPX_GRAPH_MAX_CLASSES                  1024u
#define WINE_APPX_GRAPH_MAX_STRING_CHARS             32767u

/* Values shared with the package catalog and serialized at header offset 40. */
#define WINE_APPX_GRAPH_ARCHITECTURE_NEUTRAL          0u
#define WINE_APPX_GRAPH_ARCHITECTURE_X86              1u
#define WINE_APPX_GRAPH_ARCHITECTURE_X64              2u
#define WINE_APPX_GRAPH_ARCHITECTURE_ARM              3u
#define WINE_APPX_GRAPH_ARCHITECTURE_ARM64            4u
#define WINE_APPX_GRAPH_ARCHITECTURE_X86A64           5u

#if defined(__arm64ec__)
# define WINE_APPX_GRAPH_CURRENT_ARCHITECTURE \
    WINE_APPX_GRAPH_ARCHITECTURE_X64
#elif defined(__aarch64__)
# define WINE_APPX_GRAPH_CURRENT_ARCHITECTURE \
    WINE_APPX_GRAPH_ARCHITECTURE_ARM64
#elif defined(__arm__)
# define WINE_APPX_GRAPH_CURRENT_ARCHITECTURE \
    WINE_APPX_GRAPH_ARCHITECTURE_ARM
#elif defined(__x86_64__)
# define WINE_APPX_GRAPH_CURRENT_ARCHITECTURE \
    WINE_APPX_GRAPH_ARCHITECTURE_X64
#elif defined(__i386__)
# define WINE_APPX_GRAPH_CURRENT_ARCHITECTURE \
    WINE_APPX_GRAPH_ARCHITECTURE_X86
#endif

#define WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET     16u
#define WINE_APPX_GRAPH_HEADER_PACKAGE_COUNT_OFFSET  44u
#define WINE_APPX_GRAPH_HEADER_CLASS_COUNT_OFFSET    104u
#define WINE_APPX_GRAPH_HEADER_CLASSES_OFFSET        108u
#define WINE_APPX_GRAPH_HEADER_VOLUME_SERIAL_OFFSET  112u
#define WINE_APPX_GRAPH_HEADER_FILE_INDEX_HIGH_OFFSET 116u
#define WINE_APPX_GRAPH_HEADER_FILE_INDEX_LOW_OFFSET 120u
#define WINE_APPX_GRAPH_HEADER_OBJECT_ID_OFFSET       124u
#define WINE_APPX_GRAPH_HEADER_RESERVED_OFFSET        140u

#define WINE_APPX_GRAPH_LOADER_SEARCH_RANK_OFFSET    4u
#define WINE_APPX_GRAPH_LOADER_VOLUME_SERIAL_OFFSET  24u
#define WINE_APPX_GRAPH_LOADER_FILE_INDEX_HIGH_OFFSET 28u
#define WINE_APPX_GRAPH_LOADER_FILE_INDEX_LOW_OFFSET 32u
#define WINE_APPX_GRAPH_LOADER_RESERVED_OFFSET       36u
#define WINE_APPX_GRAPH_LOADER_CHANGE_TIME_OFFSET    40u
#define WINE_APPX_GRAPH_LOADER_FILE_SIZE_OFFSET      48u
#define WINE_APPX_GRAPH_LOADER_OBJECT_ID_OFFSET       56u
#define WINE_APPX_GRAPH_LOADER_EXPLICIT_ONLY         (~0u)
#define WINE_APPX_GRAPH_MAX_LOADER_SEARCH_PATHS      5u
#define WINE_APPX_GRAPH_CLASS_VOLUME_SERIAL_OFFSET   24u
#define WINE_APPX_GRAPH_CLASS_FILE_INDEX_HIGH_OFFSET 28u
#define WINE_APPX_GRAPH_CLASS_FILE_INDEX_LOW_OFFSET  32u
#define WINE_APPX_GRAPH_CLASS_LOADER_INDEX_OFFSET    36u
#define WINE_APPX_GRAPH_CLASS_CHANGE_TIME_OFFSET     40u
#define WINE_APPX_GRAPH_CLASS_FILE_SIZE_OFFSET       48u

#define WINE_APPX_GRAPH_PACKAGE_ACTIVE               0x01u
#define WINE_APPX_GRAPH_PACKAGE_FRAMEWORK            0x02u
#define WINE_APPX_GRAPH_PACKAGE_RESOURCE             0x04u
#define WINE_APPX_GRAPH_PACKAGE_SIGNED               0x08u
#define WINE_APPX_GRAPH_PACKAGE_DIRECT               0x10u
#define WINE_APPX_GRAPH_PACKAGE_KNOWN_FLAGS           0x1fu

/*
 * PackageDependencyData points at this descriptor only in the creating
 * process.  The child receives a validated graph blob at that field instead.
 * The tag prevents a child-local graph from being mistaken for a new explicit
 * override when that process creates another child.
 */
#define WINE_APPX_GRAPH_ATTACH_TAG                    0x48435441u /* "ATCH" */
#define WINE_APPX_GRAPH_ATTACH_VERSION                2u

/*
 * Private CreateProcess attribute used by the deployment broker.  Its value
 * points to a wine_appx_graph_attach descriptor and is borrowed only for the
 * synchronous CreateProcess call.  Number 30 is intentionally not published
 * in the Win32 PROC_THREAD_ATTRIBUTE namespace.
 */
#define WINE_PROC_THREAD_ATTRIBUTE_PACKAGE_GRAPH      0x0002001eu

struct wine_appx_graph_attach
{
    unsigned int tag;
    unsigned int version;
    unsigned int size;
    unsigned int reserved;
    unsigned long long blob __attribute__((aligned(8)));
    /*
     * Native-width Windows handles are serialized as fixed-width values so
     * the descriptor has one layout in native and WOW64 callers.  There must
     * be one generation lease for every package record, in graph order.
     * Wineserver takes its own references before the creating process may
     * close these handles.
     */
    unsigned long long leases __attribute__((aligned(8)));
    unsigned int lease_count;
    unsigned int lease_reserved;
};

/*
 * Fixed prefix of the new_process graph-bindings payload.  The image handle
 * lets wineserver bind the graph to the exact executable object; the serial
 * is the Windows volume identity independently queried from that handle.
 * Native st_dev/st_ino identity remains server-owned.
 */
struct wine_appx_graph_process_binding
{
    unsigned int image_handle;
    unsigned int volume_serial;
};

struct wine_appx_graph_string_ref
{
    unsigned int offset;
    unsigned int chars;
};

static inline unsigned int wine_appx_graph_read_u16( const unsigned char *data )
{
    return data[0] | ((unsigned int)data[1] << 8);
}

static inline unsigned int wine_appx_graph_read_u32( const unsigned char *data )
{
    return wine_appx_graph_read_u16( data ) |
           (wine_appx_graph_read_u16( data + 2 ) << 16);
}

static inline unsigned long long wine_appx_graph_read_u64(
    const unsigned char *data )
{
    return wine_appx_graph_read_u32( data ) |
           ((unsigned long long)wine_appx_graph_read_u32( data + 4 ) << 32);
}

static inline int wine_appx_graph_add_u32( unsigned int left, unsigned int right,
                                           unsigned int *result )
{
    if (~0u - left < right) return 0;
    *result = left + right;
    return 1;
}

static inline int wine_appx_graph_range( unsigned int offset, unsigned int count,
                                         unsigned int element_size, unsigned int limit,
                                         unsigned int *end )
{
    unsigned int bytes;

    if (count && element_size > ~0u / count) return 0;
    bytes = count * element_size;
    return wine_appx_graph_add_u32( offset, bytes, end ) &&
           offset <= limit && *end <= limit;
}

static inline int wine_appx_graph_validate_file_identity(
    const unsigned char *record, unsigned int volume_offset,
    unsigned int high_offset, unsigned int low_offset )
{
    /*
     * Volume serial zero is valid; it is still compared as part of the tuple.
     * The file index is the only reliable presence discriminator.
     */
    (void)volume_offset;
    return wine_appx_graph_read_u32( record + high_offset ) ||
           wine_appx_graph_read_u32( record + low_offset );
}

static inline int wine_appx_graph_validate_object_id(
    const unsigned char *record, unsigned int object_id_offset )
{
    unsigned int i;

    /*
     * The native object token is an opaque fixed-width byte string.  Keeping
     * it inline makes the graph pointer-free across native and WOW64 readers;
     * all-zero remains reserved for an absent token.
     */
    for (i = 0; i < WINE_APPX_GRAPH_OBJECT_ID_SIZE; i++)
        if (record[object_id_offset + i]) return 1;
    return 0;
}

static inline struct wine_appx_graph_string_ref wine_appx_graph_get_ref(
    const unsigned char *record, unsigned int offset )
{
    struct wine_appx_graph_string_ref ref;

    ref.offset = wine_appx_graph_read_u32( record + offset );
    ref.chars = wine_appx_graph_read_u32( record + offset + 4 );
    return ref;
}

static inline int wine_appx_graph_validate_ref(
    const unsigned char *data, unsigned int size, unsigned int strings_offset,
    struct wine_appx_graph_string_ref ref, int allow_empty,
    unsigned int *expected_offset )
{
    unsigned int bytes, end, i;

    if (!ref.chars || ref.chars > WINE_APPX_GRAPH_MAX_STRING_CHARS + 1 ||
        (ref.offset & 1) || ref.offset != *expected_offset ||
        ref.chars > ~0u / 2)
        return 0;
    bytes = ref.chars * 2;
    if (!wine_appx_graph_add_u32( ref.offset, bytes, &end ) ||
        ref.offset < strings_offset || end > size ||
        (!allow_empty && ref.chars == 1) ||
        wine_appx_graph_read_u16( data + end - 2 ))
        return 0;

    for (i = 0; i + 1 < ref.chars; i++)
    {
        unsigned int ch = wine_appx_graph_read_u16( data + ref.offset + i * 2 );

        if (!ch) return 0;
        if (ch >= 0xd800 && ch <= 0xdbff)
        {
            unsigned int low;

            if (++i + 1 >= ref.chars) return 0;
            low = wine_appx_graph_read_u16( data + ref.offset + i * 2 );
            if (low < 0xdc00 || low > 0xdfff) return 0;
        }
        else if (ch >= 0xdc00 && ch <= 0xdfff) return 0;
    }
    *expected_offset = end;
    return 1;
}

/*
 * Activatable-class lookup is intentionally locale-independent.  Folding only
 * ASCII gives the serializer, transport validator, and binary-search consumer
 * one stable order on every host while leaving non-ASCII UTF-16 code units in
 * their wire order.
 */
static inline unsigned int wine_appx_graph_fold_class_char( unsigned int ch )
{
    if (ch >= 'A' && ch <= 'Z') return ch + ('a' - 'A');
    return ch;
}

static inline int wine_appx_graph_compare_class_refs(
    const unsigned char *data, struct wine_appx_graph_string_ref left,
    struct wine_appx_graph_string_ref right )
{
    unsigned int left_length = left.chars - 1;
    unsigned int right_length = right.chars - 1;
    unsigned int length = left_length < right_length ? left_length : right_length;
    unsigned int i;

    for (i = 0; i < length; i++)
    {
        unsigned int left_ch = wine_appx_graph_fold_class_char(
            wine_appx_graph_read_u16( data + left.offset + i * 2 ) );
        unsigned int right_ch = wine_appx_graph_fold_class_char(
            wine_appx_graph_read_u16( data + right.offset + i * 2 ) );

        if (left_ch != right_ch) return left_ch < right_ch ? -1 : 1;
    }
    if (left_length == right_length) return 0;
    return left_length < right_length ? -1 : 1;
}

static inline int wine_appx_graph_refs_equal(
    const unsigned char *data, struct wine_appx_graph_string_ref left,
    struct wine_appx_graph_string_ref right )
{
    unsigned int i;

    if (left.chars != right.chars) return 0;
    for (i = 0; i < left.chars; i++)
        if (wine_appx_graph_read_u16( data + left.offset + i * 2 ) !=
            wine_appx_graph_read_u16( data + right.offset + i * 2 ))
            return 0;
    return 1;
}

/*
 * Allocation-free transport validation.  It proves the complete fixed layout,
 * every offset/range, record metadata, and the exact contiguous UTF-16 string
 * inventory before PackageDependencyData is published.  Consumers still apply
 * their domain-specific identity, path and ordering validation before using a
 * graph for activation or loader resolution.
 */
static inline int wine_appx_graph_validate_blob( const void *blob, size_t blob_size )
{
    static const unsigned char magic[8] = {'S','W','X','G','R','A','P','H'};
    const unsigned char *data = blob;
    unsigned int size, package_count, packages_offset, packages_end;
    unsigned int loader_count, loaders_offset, loaders_end;
    unsigned int class_count, classes_offset, classes_end;
    unsigned int strings_offset, strings_size, strings_end;
    unsigned int target_architecture, activation_kind, expected;
    struct wine_appx_graph_string_ref previous_class_id = {0};
    unsigned int i, j;

    if (!data || blob_size < WINE_APPX_GRAPH_BLOB_HEADER_SIZE ||
        blob_size > WINE_APPX_GRAPH_MAX_BLOB_SIZE ||
        blob_size > ~0u)
        return 0;
    size = blob_size;
    for (i = 0; i < sizeof(magic); i++)
        if (data[i] != magic[i]) return 0;
    if (wine_appx_graph_read_u32( data + 8 ) != WINE_APPX_GRAPH_BLOB_VERSION ||
        wine_appx_graph_read_u32( data + 12 ) != WINE_APPX_GRAPH_BLOB_HEADER_SIZE ||
        wine_appx_graph_read_u32( data + 16 ) != size ||
        wine_appx_graph_read_u32( data + 20 ))
        return 0;
    if (!wine_appx_graph_validate_file_identity(
            data, WINE_APPX_GRAPH_HEADER_VOLUME_SERIAL_OFFSET,
            WINE_APPX_GRAPH_HEADER_FILE_INDEX_HIGH_OFFSET,
            WINE_APPX_GRAPH_HEADER_FILE_INDEX_LOW_OFFSET ) ||
        !wine_appx_graph_validate_object_id(
            data, WINE_APPX_GRAPH_HEADER_OBJECT_ID_OFFSET ) ||
        wine_appx_graph_read_u32(
            data + WINE_APPX_GRAPH_HEADER_RESERVED_OFFSET ))
        return 0;

    target_architecture = wine_appx_graph_read_u32( data + 40 );
    package_count = wine_appx_graph_read_u32( data + 44 );
    packages_offset = wine_appx_graph_read_u32( data + 48 );
    loader_count = wine_appx_graph_read_u32( data + 52 );
    loaders_offset = wine_appx_graph_read_u32( data + 56 );
    strings_offset = wine_appx_graph_read_u32( data + 60 );
    strings_size = wine_appx_graph_read_u32( data + 64 );
    activation_kind = wine_appx_graph_read_u32( data + 68 );
    class_count = wine_appx_graph_read_u32(
        data + WINE_APPX_GRAPH_HEADER_CLASS_COUNT_OFFSET );
    classes_offset = wine_appx_graph_read_u32(
        data + WINE_APPX_GRAPH_HEADER_CLASSES_OFFSET );

    if (!target_architecture || target_architecture > 5 ||
        activation_kind < 1 || activation_kind > 3 ||
        !package_count || package_count > WINE_APPX_GRAPH_MAX_PACKAGES ||
        loader_count > WINE_APPX_GRAPH_MAX_LOADER_FILES ||
        packages_offset != WINE_APPX_GRAPH_BLOB_HEADER_SIZE ||
        !wine_appx_graph_range( packages_offset, package_count,
                                WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE,
                                size, &packages_end ) ||
        loaders_offset != packages_end ||
        !wine_appx_graph_range( loaders_offset, loader_count,
                                WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE,
                                size, &loaders_end ) ||
        class_count > WINE_APPX_GRAPH_MAX_CLASSES ||
        classes_offset != loaders_end ||
        !wine_appx_graph_range( classes_offset, class_count,
                                WINE_APPX_GRAPH_BLOB_CLASS_RECORD_SIZE,
                                size, &classes_end ) ||
        strings_offset != classes_end ||
        !wine_appx_graph_add_u32( strings_offset, strings_size, &strings_end ) ||
        strings_end != size)
        return 0;

    expected = strings_offset;
    if (!wine_appx_graph_validate_ref( data, size, strings_offset,
                                       wine_appx_graph_get_ref( data, 72 ), 0, &expected ) ||
        !wine_appx_graph_validate_ref( data, size, strings_offset,
                                       wine_appx_graph_get_ref( data, 80 ), 0, &expected ) ||
        !wine_appx_graph_validate_ref( data, size, strings_offset,
                                       wine_appx_graph_get_ref( data, 88 ), 0, &expected ) ||
        !wine_appx_graph_validate_ref( data, size, strings_offset,
                                       wine_appx_graph_get_ref( data, 96 ), 1, &expected ))
        return 0;

    for (i = 0; i < package_count; i++)
    {
        const unsigned char *record = data + packages_offset +
                                      i * WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
        static const unsigned int ref_offsets[7] = {56, 64, 72, 80, 88, 96, 104};
        unsigned int architecture = wine_appx_graph_read_u32( record + 8 );
        unsigned int flags = wine_appx_graph_read_u32( record + 12 );

        if (architecture > 5 ||
            (architecture != 0 && architecture != target_architecture) ||
            (flags & ~WINE_APPX_GRAPH_PACKAGE_KNOWN_FLAGS) ||
            !(flags & WINE_APPX_GRAPH_PACKAGE_ACTIVE) ||
            !(flags & WINE_APPX_GRAPH_PACKAGE_SIGNED) ||
            wine_appx_graph_read_u32( record + 16 ) != i ||
            wine_appx_graph_read_u32( record + 20 ) ||
            (!i && (flags & (WINE_APPX_GRAPH_PACKAGE_FRAMEWORK |
                             WINE_APPX_GRAPH_PACKAGE_RESOURCE |
                             WINE_APPX_GRAPH_PACKAGE_DIRECT))) ||
            /*
             * This full-trust graph revision intentionally models only the
             * framework dependency closure.  Resource and ordinary package
             * dependencies remain outside the activation/loader boundary.
             */
            (i && (!(flags & WINE_APPX_GRAPH_PACKAGE_FRAMEWORK) ||
                   (flags & WINE_APPX_GRAPH_PACKAGE_RESOURCE))))
            return 0;
        for (j = 0; j < 7; j++)
            if (!wine_appx_graph_validate_ref(
                    data, size, strings_offset,
                    wine_appx_graph_get_ref( record, ref_offsets[j] ),
                    j == 2, &expected ))
                return 0;
    }

    for (i = 0; i < loader_count; i++)
    {
        const unsigned char *record = data + loaders_offset +
                                      i * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;

        if (wine_appx_graph_read_u32( record ) >= package_count ||
            (wine_appx_graph_read_u32(
                 record + WINE_APPX_GRAPH_LOADER_SEARCH_RANK_OFFSET ) >=
                 WINE_APPX_GRAPH_MAX_LOADER_SEARCH_PATHS &&
             wine_appx_graph_read_u32(
                 record + WINE_APPX_GRAPH_LOADER_SEARCH_RANK_OFFSET ) !=
                 WINE_APPX_GRAPH_LOADER_EXPLICIT_ONLY) ||
            !wine_appx_graph_validate_file_identity(
                record, WINE_APPX_GRAPH_LOADER_VOLUME_SERIAL_OFFSET,
                WINE_APPX_GRAPH_LOADER_FILE_INDEX_HIGH_OFFSET,
                WINE_APPX_GRAPH_LOADER_FILE_INDEX_LOW_OFFSET ) ||
            !wine_appx_graph_validate_object_id(
                record, WINE_APPX_GRAPH_LOADER_OBJECT_ID_OFFSET ) ||
            wine_appx_graph_read_u32(
                record + WINE_APPX_GRAPH_LOADER_RESERVED_OFFSET ) ||
            !wine_appx_graph_read_u64(
                record + WINE_APPX_GRAPH_LOADER_CHANGE_TIME_OFFSET ) ||
            (wine_appx_graph_read_u64(
                record + WINE_APPX_GRAPH_LOADER_CHANGE_TIME_OFFSET ) >> 63) ||
            !wine_appx_graph_read_u64(
                record + WINE_APPX_GRAPH_LOADER_FILE_SIZE_OFFSET ) ||
            (wine_appx_graph_read_u64(
                record + WINE_APPX_GRAPH_LOADER_FILE_SIZE_OFFSET ) >> 63) ||
            !wine_appx_graph_validate_ref( data, size, strings_offset,
                                           wine_appx_graph_get_ref( record, 8 ),
                                           0, &expected ) ||
            !wine_appx_graph_validate_ref( data, size, strings_offset,
                                           wine_appx_graph_get_ref( record, 16 ),
                                           0, &expected ))
            return 0;
    }

    for (i = 0; i < class_count; i++)
    {
        const unsigned char *record = data + classes_offset +
                                      i * WINE_APPX_GRAPH_BLOB_CLASS_RECORD_SIZE;
        unsigned int loader_index = wine_appx_graph_read_u32(
            record + WINE_APPX_GRAPH_CLASS_LOADER_INDEX_OFFSET );
        const unsigned char *loader_record;
        struct wine_appx_graph_string_ref class_id =
            wine_appx_graph_get_ref( record, 8 );
        struct wine_appx_graph_string_ref class_path =
            wine_appx_graph_get_ref( record, 16 );

        if (wine_appx_graph_read_u32( record ) >= package_count ||
            wine_appx_graph_read_u32( record + 4 ) > 2 ||
            loader_index >= loader_count ||
            !wine_appx_graph_validate_file_identity(
                record, WINE_APPX_GRAPH_CLASS_VOLUME_SERIAL_OFFSET,
                WINE_APPX_GRAPH_CLASS_FILE_INDEX_HIGH_OFFSET,
                WINE_APPX_GRAPH_CLASS_FILE_INDEX_LOW_OFFSET ) ||
            !wine_appx_graph_read_u64(
                record + WINE_APPX_GRAPH_CLASS_CHANGE_TIME_OFFSET ) ||
            (wine_appx_graph_read_u64(
                record + WINE_APPX_GRAPH_CLASS_CHANGE_TIME_OFFSET ) >> 63) ||
            !wine_appx_graph_read_u64(
                record + WINE_APPX_GRAPH_CLASS_FILE_SIZE_OFFSET ) ||
            (wine_appx_graph_read_u64(
                record + WINE_APPX_GRAPH_CLASS_FILE_SIZE_OFFSET ) >> 63) ||
            !wine_appx_graph_validate_ref( data, size, strings_offset,
                                           class_id,
                                           0, &expected ) ||
            !wine_appx_graph_validate_ref( data, size, strings_offset,
                                           class_path,
                                           0, &expected ))
            return 0;
        loader_record = data + loaders_offset +
            loader_index * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
        if (wine_appx_graph_read_u32( loader_record ) !=
                wine_appx_graph_read_u32( record ) ||
            !wine_appx_graph_refs_equal(
                data, wine_appx_graph_get_ref( loader_record, 16 ),
                class_path ) ||
            wine_appx_graph_read_u32(
                loader_record +
                WINE_APPX_GRAPH_LOADER_VOLUME_SERIAL_OFFSET ) !=
                wine_appx_graph_read_u32(
                    record + WINE_APPX_GRAPH_CLASS_VOLUME_SERIAL_OFFSET ) ||
            wine_appx_graph_read_u32(
                loader_record +
                WINE_APPX_GRAPH_LOADER_FILE_INDEX_HIGH_OFFSET ) !=
                wine_appx_graph_read_u32(
                    record +
                    WINE_APPX_GRAPH_CLASS_FILE_INDEX_HIGH_OFFSET ) ||
            wine_appx_graph_read_u32(
                loader_record +
                WINE_APPX_GRAPH_LOADER_FILE_INDEX_LOW_OFFSET ) !=
                wine_appx_graph_read_u32(
                    record +
                    WINE_APPX_GRAPH_CLASS_FILE_INDEX_LOW_OFFSET ) ||
            wine_appx_graph_read_u64(
                loader_record +
                WINE_APPX_GRAPH_LOADER_CHANGE_TIME_OFFSET ) !=
                wine_appx_graph_read_u64(
                    record + WINE_APPX_GRAPH_CLASS_CHANGE_TIME_OFFSET ) ||
            wine_appx_graph_read_u64(
                loader_record +
                WINE_APPX_GRAPH_LOADER_FILE_SIZE_OFFSET ) !=
                wine_appx_graph_read_u64(
                    record + WINE_APPX_GRAPH_CLASS_FILE_SIZE_OFFSET ))
            return 0;
        if (i && wine_appx_graph_compare_class_refs(
                     data, previous_class_id, class_id ) >= 0)
            return 0;
        previous_class_id = class_id;
    }
    return expected == size;
}

#endif /* __WINE_APPX_PACKAGE_GRAPH_H */
