/*
 * Copyright 2024 Stefan Dösinger for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "vkd3d_private.h"

#define VKD3D_CACHE_FORMAT_VERSION 1u
#define VKD3D_CACHE_ABI_VERSION 1u
#define VKD3D_CACHE_HEADER_SIZE 176u
#define VKD3D_CACHE_HEADER_HASH_OFFSET 144u
#define VKD3D_CACHE_SHA256_SIZE 32u
#define VKD3D_CACHE_HEX_SIZE (VKD3D_CACHE_SHA256_SIZE * 2u)
#define VKD3D_CACHE_DEFAULT_VALUE_SIZE (128u * 1024u * 1024u)
#define VKD3D_CACHE_DEFAULT_MEMORY_SIZE 1024u
#define VKD3D_CACHE_DEFAULT_MEMORY_ENTRIES 128u
#define VKD3D_CACHE_MAXIMUM_VALUE_SIZE (1024u * 1024u * 1024u)
#define VKD3D_CACHE_MAXIMUM_KEY_SIZE (128u * 1024u * 1024u)
#define VKD3D_CACHE_MAXIMUM_DISK_ENTRIES 65536u
#define VKD3D_CACHE_IO_BUFFER_SIZE (64u * 1024u)
#define VKD3D_CACHE_MANIFEST_SIZE 128u
#define VKD3D_CACHE_MANIFEST_HASH_OFFSET 96u
#define VKD3D_CACHE_QUOTA_STATE_SIZE 104u
#define VKD3D_CACHE_QUOTA_HASH_OFFSET 72u
#define VKD3D_CACHE_QUOTA_DIRTY 0x1u
#define VKD3D_CACHE_ROOT_FORMAT_VERSION 1u
#define VKD3D_CACHE_ROOT_MAXIMUM_NAMESPACES 128u
#define VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS 32u
#define VKD3D_CACHE_ROOT_MAXIMUM_ENTRIES 65536u
#define VKD3D_CACHE_ROOT_MAXIMUM_SCAN_NAMESPACES (2u * VKD3D_CACHE_ROOT_MAXIMUM_NAMESPACES)
#define VKD3D_CACHE_ROOT_MAXIMUM_SCAN_OBJECTS \
        (VKD3D_CACHE_ROOT_MAXIMUM_SCAN_NAMESPACES + 8u)
#define VKD3D_CACHE_ROOT_MAXIMUM_ENTRY_SCAN_OBJECTS \
        (VKD3D_CACHE_ROOT_MAXIMUM_ENTRIES \
        + 2u * VKD3D_CACHE_ROOT_MAXIMUM_SCAN_NAMESPACES + 1u)
#define VKD3D_CACHE_ROOT_NAMESPACE_SIZE 56u
#define VKD3D_CACHE_ROOT_RESERVATION_SIZE 112u
#define VKD3D_CACHE_ROOT_STATE_HEADER_SIZE 64u
#define VKD3D_CACHE_ROOT_STATE_SIZE (VKD3D_CACHE_ROOT_STATE_HEADER_SIZE \
        + VKD3D_CACHE_ROOT_MAXIMUM_NAMESPACES * VKD3D_CACHE_ROOT_NAMESPACE_SIZE \
        + VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS * VKD3D_CACHE_ROOT_RESERVATION_SIZE \
        + VKD3D_CACHE_SHA256_SIZE)
#define VKD3D_CACHE_ROOT_HASH_OFFSET (VKD3D_CACHE_ROOT_STATE_SIZE - VKD3D_CACHE_SHA256_SIZE)
#define VKD3D_CACHE_ROOT_MAXIMUM_PHYSICAL_SIZE (4ull * 1024u * 1024u * 1024u)
#define VKD3D_CACHE_ROOT_RESERVATION_ACTIVE 0x1u

/* Root state is a fixed 10,848-byte little-endian record. The 64-byte header
 * contains magic/version/size/flags, namespace/reservation/entry counts, the
 * fixed byte cap, committed bytes, and a serial. It is followed by 128 56-byte
 * namespace records (digest, bytes, last-use time, entry count), 32 112-byte
 * reservation records (namespace/entry/temporary digests, bytes, flags), and
 * a SHA-256 over all preceding bytes. */

#define VKD3D_CACHE_HEADER_FORMAT_OFFSET 8u
#define VKD3D_CACHE_HEADER_SIZE_OFFSET 12u
#define VKD3D_CACHE_HEADER_ABI_OFFSET 16u
#define VKD3D_CACHE_HEADER_FLAGS_OFFSET 20u
#define VKD3D_CACHE_HEADER_KEY_SIZE_OFFSET 24u
#define VKD3D_CACHE_HEADER_VALUE_SIZE_OFFSET 32u
#define VKD3D_CACHE_HEADER_SESSION_VERSION_OFFSET 40u
#define VKD3D_CACHE_HEADER_NAMESPACE_OFFSET 48u
#define VKD3D_CACHE_HEADER_KEY_HASH_OFFSET 80u
#define VKD3D_CACHE_HEADER_PAYLOAD_HASH_OFFSET 112u

#define VKD3D_CACHE_OPERATION_LOCK_OFFSET 0u
#define VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET 1u
#define VKD3D_CACHE_ROOT_LOCK_OFFSET 0u
#define VKD3D_CACHE_ROOT_RESERVATION_LOCK_OFFSET(i) (1u + (i))

static const uint8_t vkd3d_cache_magic[8] = {'V', 'K', 'D', '3', 'D', 'C', '0', '1'};
static const WCHAR vkd3d_cache_base_name[] = L"vkd3d-cache";
static const WCHAR vkd3d_cache_entries_name[] =
        L"0000000000000000000000000000000000000000000000000000000000000001";
static const WCHAR vkd3d_cache_temp_name[] =
        L"0000000000000000000000000000000000000000000000000000000000000002";
static const WCHAR vkd3d_cache_lock_name[] =
        L"0000000000000000000000000000000000000000000000000000000000000003";
static const WCHAR vkd3d_cache_marker_name[] =
        L"0000000000000000000000000000000000000000000000000000000000000004";
static const WCHAR vkd3d_cache_manifest_name[] =
        L"0000000000000000000000000000000000000000000000000000000000000005";
static const WCHAR vkd3d_cache_quota_name[] =
        L"0000000000000000000000000000000000000000000000000000000000000006";
static const WCHAR vkd3d_cache_root_lock_name[] = L"vkd3d-root-lock";
static const WCHAR vkd3d_cache_root_state_name[] = L"vkd3d-root-state";
static const WCHAR vkd3d_cache_root_state_temp_name[] = L"vkd3d-root-state-tmp";
static const uint8_t vkd3d_cache_manifest_magic[8] = {'V', 'K', 'D', '3', 'D', 'M', '0', '1'};
static const uint8_t vkd3d_cache_quota_magic[8] = {'V', 'K', 'D', '3', 'D', 'Q', '0', '1'};
static const uint8_t vkd3d_cache_root_magic[8] = {'V', 'K', 'D', '3', 'D', 'R', '0', '1'};

static void vkd3d_cache_make_temp_name(const uint8_t key_hash[VKD3D_CACHE_SHA256_SIZE],
        uint32_t attempt, WCHAR name[VKD3D_CACHE_HEX_SIZE + 1]);

struct vkd3d_sha256_context
{
    uint32_t state[8];
    uint64_t size;
    size_t block_size;
    uint8_t block[64];
};

struct vkd3d_cache_entry_header
{
    uint8_t hash[VKD3D_CACHE_SHA256_SIZE];
    uint64_t key_size;
    uint64_t value_size;
};

struct shader_cache_entry
{
    struct vkd3d_cache_entry_header h;
    struct rb_entry entry;
    struct list lru_entry;
    uint8_t *payload;
};

struct shader_cache_key
{
    uint8_t hash[VKD3D_CACHE_SHA256_SIZE];
};

struct vkd3d_shader_cache_disk
{
    WCHAR *cache_root_path;
    WCHAR *root_lock_path;
    WCHAR *root_state_path;
    WCHAR *root_state_temp_path;
    WCHAR *root_path;
    WCHAR *entries_path;
    WCHAR *temp_path;
    WCHAR *lock_path;
    WCHAR *marker_path;
    WCHAR *manifest_path;
    WCHAR *quota_path;
    HANDLE cache_root_handle;
    HANDLE root_lock_handle;
    HANDLE root_handle;
    HANDLE entries_handle;
    HANDLE temp_handle;
    HANDLE lock_handle;
    uint8_t namespace_hash[VKD3D_CACHE_SHA256_SIZE];
    uint64_t maximum_size;
    uint64_t maximum_physical_size;
    uint32_t maximum_entry_count;
    bool lifecycle_locked;
};

/* The root lock serializes aggregate state. Transient nested acquisition order
 * is root, namespace lifecycle, namespace operation, memory mutex. The shared
 * lifecycle range held for an open cache is an activity lease: eviction holds
 * root and probes the exclusive range fail-immediately, so it never waits on
 * an active namespace. Disk reads take only the operation range shared and the
 * memory mutex, and therefore remain root-lock-free. */

struct vkd3d_shader_cache
{
    unsigned int refcount;
    struct vkd3d_mutex lock;

    struct rb_tree tree;
    struct list lru_list;
    uint64_t memory_size;
    uint64_t maximum_memory_size;
    uint32_t entry_count;
    uint32_t maximum_entry_count;

    D3D12_SHADER_CACHE_MODE mode;
    D3D12_SHADER_CACHE_FLAGS flags;
    uint64_t version;
    uint64_t maximum_value_size;
    struct vkd3d_shader_cache_disk disk;
};

struct vkd3d_cache_disk_candidate
{
    uint8_t hash[VKD3D_CACHE_SHA256_SIZE];
    FILETIME last_write_time;
    uint64_t value_size;
    uint64_t file_size;
};

struct vkd3d_cache_quota_state
{
    uint64_t value_size;
    uint64_t physical_size;
    uint32_t entry_count;
};

struct vkd3d_cache_root_namespace
{
    uint8_t hash[VKD3D_CACHE_SHA256_SIZE];
    uint64_t physical_size;
    uint64_t last_used;
    uint32_t entry_count;
};

struct vkd3d_cache_root_reservation
{
    uint8_t namespace_hash[VKD3D_CACHE_SHA256_SIZE];
    uint8_t entry_hash[VKD3D_CACHE_SHA256_SIZE];
    uint8_t temp_hash[VKD3D_CACHE_SHA256_SIZE];
    uint64_t physical_size;
    uint32_t flags;
};

struct vkd3d_cache_root_state
{
    uint64_t physical_size;
    uint64_t serial;
    uint32_t namespace_count;
    uint32_t entry_count;
    struct vkd3d_cache_root_namespace namespaces[VKD3D_CACHE_ROOT_MAXIMUM_NAMESPACES];
    struct vkd3d_cache_root_reservation reservations[VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS];
};

static uint32_t vkd3d_cache_rotr32(uint32_t value, unsigned int count)
{
    return (value >> count) | (value << (32 - count));
}

static uint32_t vkd3d_cache_read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
            | ((uint32_t)p[2] << 8) | p[3];
}

static void vkd3d_cache_write_be32(uint8_t *p, uint32_t value)
{
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
}

static void vkd3d_cache_sha256_transform(struct vkd3d_sha256_context *ctx, const uint8_t block[64])
{
    static const uint32_t constants[64] =
    {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t a, b, c, d, e, f, g, h, s0, s1, choice, majority, t1, t2;
    uint32_t words[64];
    unsigned int i;

    for (i = 0; i < 16; ++i)
        words[i] = vkd3d_cache_read_be32(&block[i * 4]);
    for (; i < ARRAY_SIZE(words); ++i)
    {
        s0 = vkd3d_cache_rotr32(words[i - 15], 7)
                ^ vkd3d_cache_rotr32(words[i - 15], 18) ^ (words[i - 15] >> 3);
        s1 = vkd3d_cache_rotr32(words[i - 2], 17)
                ^ vkd3d_cache_rotr32(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < ARRAY_SIZE(words); ++i)
    {
        s1 = vkd3d_cache_rotr32(e, 6) ^ vkd3d_cache_rotr32(e, 11) ^ vkd3d_cache_rotr32(e, 25);
        choice = (e & f) ^ (~e & g);
        t1 = h + s1 + choice + constants[i] + words[i];
        s0 = vkd3d_cache_rotr32(a, 2) ^ vkd3d_cache_rotr32(a, 13) ^ vkd3d_cache_rotr32(a, 22);
        majority = (a & b) ^ (a & c) ^ (b & c);
        t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void vkd3d_cache_sha256_init(struct vkd3d_sha256_context *ctx)
{
    static const uint32_t initial_state[8] =
    {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };

    memcpy(ctx->state, initial_state, sizeof(initial_state));
    ctx->size = 0;
    ctx->block_size = 0;
}

static void vkd3d_cache_sha256_update(struct vkd3d_sha256_context *ctx, const void *data, size_t size)
{
    const uint8_t *p = data;
    size_t count;

    if (!size)
        return;
    if (!data)
    {
        ERR("Unexpected NULL input to shader cache SHA-256 update.\n");
        return;
    }
    ctx->size += size;
    while (size)
    {
        count = min(size, sizeof(ctx->block) - ctx->block_size);
        memcpy(&ctx->block[ctx->block_size], p, count);
        ctx->block_size += count;
        p += count;
        size -= count;

        if (ctx->block_size == sizeof(ctx->block))
        {
            vkd3d_cache_sha256_transform(ctx, ctx->block);
            ctx->block_size = 0;
        }
    }
}

static void vkd3d_cache_sha256_final(struct vkd3d_sha256_context *ctx,
        uint8_t hash[VKD3D_CACHE_SHA256_SIZE])
{
    uint64_t bit_size = ctx->size * 8;
    unsigned int i;

    ctx->block[ctx->block_size++] = 0x80;
    if (ctx->block_size > 56)
    {
        memset(&ctx->block[ctx->block_size], 0, sizeof(ctx->block) - ctx->block_size);
        vkd3d_cache_sha256_transform(ctx, ctx->block);
        ctx->block_size = 0;
    }
    memset(&ctx->block[ctx->block_size], 0, 56 - ctx->block_size);
    for (i = 0; i < 8; ++i)
        ctx->block[63 - i] = bit_size >> (i * 8);
    vkd3d_cache_sha256_transform(ctx, ctx->block);

    for (i = 0; i < ARRAY_SIZE(ctx->state); ++i)
        vkd3d_cache_write_be32(&hash[i * 4], ctx->state[i]);
}

static void vkd3d_cache_sha256(const void *data, size_t size,
        uint8_t hash[VKD3D_CACHE_SHA256_SIZE])
{
    struct vkd3d_sha256_context ctx;

    vkd3d_cache_sha256_init(&ctx);
    vkd3d_cache_sha256_update(&ctx, data, size);
    vkd3d_cache_sha256_final(&ctx, hash);
}

static uint64_t vkd3d_cache_hash_memory_key(const void *key, size_t size)
{
    static const uint64_t fnv_prime = 0x00000100000001b3;
    const uint8_t *p = key;
    uint64_t value = 0xcbf29ce484222325;
    size_t i;

    for (i = 0; i < size; ++i)
        value = (value ^ p[i]) * fnv_prime;
    return value;
}

static void vkd3d_cache_write_u32(uint8_t *p, uint32_t value)
{
    p[0] = value;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}

static uint32_t vkd3d_cache_read_u32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void vkd3d_cache_write_u64(uint8_t *p, uint64_t value)
{
    unsigned int i;

    for (i = 0; i < 8; ++i)
        p[i] = value >> (i * 8);
}

static uint64_t vkd3d_cache_read_u64(const uint8_t *p)
{
    uint64_t value = 0;
    unsigned int i;

    for (i = 0; i < 8; ++i)
        value |= (uint64_t)p[i] << (i * 8);
    return value;
}

static HRESULT vkd3d_cache_last_error(void)
{
    DWORD error = GetLastError();

    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

static void vkd3d_cache_hash_to_hex(const uint8_t hash[VKD3D_CACHE_SHA256_SIZE],
        WCHAR hex[VKD3D_CACHE_HEX_SIZE + 1])
{
    static const WCHAR digits[] = L"0123456789abcdef";
    unsigned int i;

    for (i = 0; i < VKD3D_CACHE_SHA256_SIZE; ++i)
    {
        hex[i * 2] = digits[hash[i] >> 4];
        hex[i * 2 + 1] = digits[hash[i] & 0xf];
    }
    hex[VKD3D_CACHE_HEX_SIZE] = 0;
}

static bool vkd3d_cache_is_hex_name(const WCHAR *name)
{
    unsigned int i;

    for (i = 0; i < VKD3D_CACHE_HEX_SIZE; ++i)
    {
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f')))
            return false;
    }
    return !name[VKD3D_CACHE_HEX_SIZE];
}

static HRESULT vkd3d_cache_path_join(const WCHAR *parent, const WCHAR *name, WCHAR **path)
{
    size_t parent_length, name_length;
    size_t length;
    WCHAR *p;

    if (!parent || !name || !path)
        return E_INVALIDARG;
    parent_length = lstrlenW(parent);
    name_length = lstrlenW(name);
    if (parent_length > (SIZE_MAX / sizeof(WCHAR)) - name_length - 2)
        return E_OUTOFMEMORY;
    length = parent_length + name_length + 2;
    if (!(p = vkd3d_malloc(length * sizeof(*p))))
        return E_OUTOFMEMORY;

    if (parent_length)
        memcpy(p, parent, parent_length * sizeof(*p));
    if (parent_length && parent[parent_length - 1] != '\\' && parent[parent_length - 1] != '/')
        p[parent_length++] = '\\';
    memcpy(&p[parent_length], name, (name_length + 1) * sizeof(*p));
    *path = p;
    return S_OK;
}

static HRESULT vkd3d_cache_get_dynamic_path(DWORD (*get_path)(DWORD, WCHAR *), WCHAR **path)
{
    DWORD capacity = MAX_PATH, length;
    WCHAR *p;

    for (;;)
    {
        if (!(p = vkd3d_malloc((size_t)capacity * sizeof(*p))))
            return E_OUTOFMEMORY;
        length = get_path(capacity, p);
        if (length && length < capacity)
        {
            *path = p;
            return S_OK;
        }
        vkd3d_free(p);
        if (!length)
            return vkd3d_cache_last_error();
        if (capacity > 32768 / 2)
            return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
        capacity *= 2;
    }
}

static DWORD vkd3d_cache_get_current_directory(DWORD size, WCHAR *path)
{
    return GetCurrentDirectoryW(size, path);
}

static HRESULT vkd3d_cache_get_module_path(WCHAR **path)
{
    DWORD capacity = MAX_PATH, length;
    WCHAR *p;

    for (;;)
    {
        if (!(p = vkd3d_malloc((size_t)capacity * sizeof(*p))))
            return E_OUTOFMEMORY;
        SetLastError(ERROR_SUCCESS);
        length = GetModuleFileNameW(NULL, p, capacity);
        if (length && length < capacity)
        {
            *path = p;
            return S_OK;
        }
        vkd3d_free(p);
        if (!length)
            return vkd3d_cache_last_error();
        if (capacity > 32768 / 2)
            return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
        capacity *= 2;
    }
}

static DWORD vkd3d_cache_get_temp_directory(DWORD size, WCHAR *path)
{
    typedef DWORD (WINAPI *vkd3d_get_temp_path_proc)(DWORD, WCHAR *);
    vkd3d_get_temp_path_proc get_temp_path2;
    HMODULE kernel32;

    if ((kernel32 = GetModuleHandleW(L"kernel32.dll"))
            && (get_temp_path2 = (vkd3d_get_temp_path_proc)GetProcAddress(kernel32, "GetTempPath2W")))
    {
        return get_temp_path2(size, path);
    }
    return GetTempPathW(size, path);
}

static HRESULT vkd3d_cache_open_directory(const WCHAR *path, HANDLE *handle)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE h;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return vkd3d_cache_last_error();
    if (!GetFileInformationByHandle(h, &info))
    {
        HRESULT hr = vkd3d_cache_last_error();
        CloseHandle(h);
        return hr;
    }
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        WARN("Refusing unsafe shader cache directory %s.\n", debugstr_w(path, sizeof(WCHAR)));
        CloseHandle(h);
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }

    *handle = h;
    return S_OK;
}

static HRESULT vkd3d_cache_ensure_directory(const WCHAR *path, HANDLE *handle)
{
    DWORD error;

    if (!CreateDirectoryW(path, NULL))
    {
        error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS)
            return HRESULT_FROM_WIN32(error);
    }
    return vkd3d_cache_open_directory(path, handle);
}

static HRESULT vkd3d_cache_lock_range(HANDLE file, DWORD offset, bool exclusive, bool immediate)
{
    OVERLAPPED overlapped;
    DWORD flags = 0;

    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.u.Offset = offset;
    if (exclusive)
        flags |= LOCKFILE_EXCLUSIVE_LOCK;
    if (immediate)
        flags |= LOCKFILE_FAIL_IMMEDIATELY;
    if (!LockFileEx(file, flags, 0, 1, 0, &overlapped))
        return vkd3d_cache_last_error();
    return S_OK;
}

static HRESULT vkd3d_cache_unlock_range(HANDLE file, DWORD offset)
{
    OVERLAPPED overlapped;

    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.u.Offset = offset;
    if (!UnlockFileEx(file, 0, 1, 0, &overlapped))
        return vkd3d_cache_last_error();
    return S_OK;
}

static HRESULT vkd3d_cache_open_lock_file(const WCHAR *path, DWORD creation, HANDLE *handle)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE h;

    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, creation, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return vkd3d_cache_last_error();
    if (!GetFileInformationByHandle(h, &info))
    {
        HRESULT hr = vkd3d_cache_last_error();
        CloseHandle(h);
        return hr;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
    {
        WARN("Refusing unsafe shader cache lock file %s.\n", debugstr_w(path, sizeof(WCHAR)));
        CloseHandle(h);
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }
    *handle = h;
    return S_OK;
}

static HRESULT vkd3d_cache_regular_file_exists(const WCHAR *path, bool *exists)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE file;
    HRESULT hr;
    DWORD error;

    *exists = false;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return S_OK;
        return HRESULT_FROM_WIN32(error);
    }
    if (!GetFileInformationByHandle(file, &info))
        hr = vkd3d_cache_last_error();
    else if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        hr = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    else
    {
        *exists = true;
        hr = S_OK;
    }
    CloseHandle(file);
    return hr;
}

static HRESULT vkd3d_cache_file_read(HANDLE file, void *data, size_t size)
{
    uint8_t *p = data;
    DWORD count, request;

    while (size)
    {
        request = size > UINT32_MAX ? UINT32_MAX : size;
        if (!ReadFile(file, p, request, &count, NULL))
            return vkd3d_cache_last_error();
        if (!count)
            return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        p += count;
        size -= count;
    }
    return S_OK;
}

static HRESULT vkd3d_cache_file_write(HANDLE file, const void *data, size_t size)
{
    const uint8_t *p = data;
    DWORD count, request;

    while (size)
    {
        request = size > UINT32_MAX ? UINT32_MAX : size;
        if (!WriteFile(file, p, request, &count, NULL))
            return vkd3d_cache_last_error();
        if (!count)
            return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        p += count;
        size -= count;
    }
    return S_OK;
}

static HRESULT vkd3d_cache_root_lock(struct vkd3d_shader_cache_disk *disk)
{
    return vkd3d_cache_lock_range(disk->root_lock_handle,
            VKD3D_CACHE_ROOT_LOCK_OFFSET, true, false);
}

static HRESULT vkd3d_cache_root_unlock(struct vkd3d_shader_cache_disk *disk, HRESULT operation_hr)
{
    HRESULT hr;

    if (FAILED(hr = vkd3d_cache_unlock_range(disk->root_lock_handle,
            VKD3D_CACHE_ROOT_LOCK_OFFSET)))
    {
        ERR("Failed to release shader cache root lock, hr %#lx.\n", hr);
        if (SUCCEEDED(operation_hr))
            operation_hr = hr;
    }
    return operation_hr;
}

static uint64_t vkd3d_cache_filetime_to_u64(const FILETIME *time)
{
    return (uint64_t)time->dwLowDateTime | (uint64_t)time->dwHighDateTime << 32;
}

static uint8_t *vkd3d_cache_root_namespace_data(uint8_t *data, uint32_t index)
{
    return &data[VKD3D_CACHE_ROOT_STATE_HEADER_SIZE
            + index * VKD3D_CACHE_ROOT_NAMESPACE_SIZE];
}

static uint8_t *vkd3d_cache_root_reservation_data(uint8_t *data, uint32_t index)
{
    return &data[VKD3D_CACHE_ROOT_STATE_HEADER_SIZE
            + VKD3D_CACHE_ROOT_MAXIMUM_NAMESPACES * VKD3D_CACHE_ROOT_NAMESPACE_SIZE
            + index * VKD3D_CACHE_ROOT_RESERVATION_SIZE];
}

static HRESULT vkd3d_cache_build_root_state(const struct vkd3d_cache_root_state *state,
        uint8_t data[VKD3D_CACHE_ROOT_STATE_SIZE])
{
    uint64_t namespace_size = 0, reservation_size = 0;
    uint32_t namespace_entries = 0;
    uint32_t reservation_count = 0;
    uint8_t *p;
    unsigned int i, j;

    if (state->namespace_count > VKD3D_CACHE_ROOT_MAXIMUM_NAMESPACES)
        return E_INVALIDARG;
    memset(data, 0, VKD3D_CACHE_ROOT_STATE_SIZE);
    memcpy(data, vkd3d_cache_root_magic, sizeof(vkd3d_cache_root_magic));
    vkd3d_cache_write_u32(&data[8], VKD3D_CACHE_ROOT_FORMAT_VERSION);
    vkd3d_cache_write_u32(&data[12], VKD3D_CACHE_ROOT_STATE_SIZE);
    vkd3d_cache_write_u32(&data[20], state->namespace_count);
    vkd3d_cache_write_u32(&data[28], state->entry_count);
    vkd3d_cache_write_u64(&data[32], VKD3D_CACHE_ROOT_MAXIMUM_PHYSICAL_SIZE);
    vkd3d_cache_write_u64(&data[40], state->physical_size);
    vkd3d_cache_write_u64(&data[48], state->serial);

    for (i = 0; i < state->namespace_count; ++i)
    {
        const struct vkd3d_cache_root_namespace *ns = &state->namespaces[i];

        if (namespace_size > UINT64_MAX - ns->physical_size)
            return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        namespace_size += ns->physical_size;
        if (namespace_entries > UINT32_MAX - ns->entry_count)
            return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        namespace_entries += ns->entry_count;
        for (j = 0; j < i; ++j)
        {
            if (!memcmp(ns->hash, state->namespaces[j].hash, sizeof(ns->hash)))
                return E_INVALIDARG;
        }
        p = vkd3d_cache_root_namespace_data(data, i);
        memcpy(p, ns->hash, VKD3D_CACHE_SHA256_SIZE);
        vkd3d_cache_write_u64(&p[32], ns->physical_size);
        vkd3d_cache_write_u64(&p[40], ns->last_used);
        vkd3d_cache_write_u32(&p[48], ns->entry_count);
    }
    if (namespace_size != state->physical_size || namespace_entries != state->entry_count)
        return E_INVALIDARG;

    for (i = 0; i < VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS; ++i)
    {
        const struct vkd3d_cache_root_reservation *reservation = &state->reservations[i];
        bool namespace_found = false;

        if (!reservation->flags)
            continue;
        if (reservation->flags != VKD3D_CACHE_ROOT_RESERVATION_ACTIVE
                || !reservation->physical_size
                || reservation_size > UINT64_MAX - reservation->physical_size)
            return E_INVALIDARG;
        for (j = 0; j < state->namespace_count; ++j)
        {
            if (!memcmp(reservation->namespace_hash, state->namespaces[j].hash,
                    VKD3D_CACHE_SHA256_SIZE))
            {
                namespace_found = true;
                break;
            }
        }
        if (!namespace_found)
            return E_INVALIDARG;
        reservation_size += reservation->physical_size;
        ++reservation_count;
        p = vkd3d_cache_root_reservation_data(data, i);
        memcpy(p, reservation->namespace_hash, VKD3D_CACHE_SHA256_SIZE);
        memcpy(&p[32], reservation->entry_hash, VKD3D_CACHE_SHA256_SIZE);
        memcpy(&p[64], reservation->temp_hash, VKD3D_CACHE_SHA256_SIZE);
        vkd3d_cache_write_u64(&p[96], reservation->physical_size);
        vkd3d_cache_write_u32(&p[104], reservation->flags);
    }
    if (state->physical_size > VKD3D_CACHE_ROOT_MAXIMUM_PHYSICAL_SIZE
            || reservation_size > VKD3D_CACHE_ROOT_MAXIMUM_PHYSICAL_SIZE - state->physical_size)
        return DXGI_ERROR_CACHE_FULL;
    if (state->entry_count > VKD3D_CACHE_ROOT_MAXIMUM_ENTRIES
            || reservation_count > VKD3D_CACHE_ROOT_MAXIMUM_ENTRIES - state->entry_count)
        return DXGI_ERROR_CACHE_FULL;
    vkd3d_cache_write_u32(&data[24], reservation_count);
    vkd3d_cache_sha256(data, VKD3D_CACHE_ROOT_HASH_OFFSET,
            &data[VKD3D_CACHE_ROOT_HASH_OFFSET]);
    return S_OK;
}

static HRESULT vkd3d_cache_parse_root_state(const uint8_t data[VKD3D_CACHE_ROOT_STATE_SIZE],
        struct vkd3d_cache_root_state *state)
{
    uint8_t hash[VKD3D_CACHE_SHA256_SIZE];
    uint32_t namespace_count, reservation_count = 0;
    const uint8_t *p;
    unsigned int i;
    HRESULT hr;

    vkd3d_cache_sha256(data, VKD3D_CACHE_ROOT_HASH_OFFSET, hash);
    if (memcmp(data, vkd3d_cache_root_magic, sizeof(vkd3d_cache_root_magic))
            || vkd3d_cache_read_u32(&data[8]) != VKD3D_CACHE_ROOT_FORMAT_VERSION
            || vkd3d_cache_read_u32(&data[12]) != VKD3D_CACHE_ROOT_STATE_SIZE
            || vkd3d_cache_read_u32(&data[16])
            || vkd3d_cache_read_u64(&data[32]) != VKD3D_CACHE_ROOT_MAXIMUM_PHYSICAL_SIZE
            || memcmp(hash, &data[VKD3D_CACHE_ROOT_HASH_OFFSET], sizeof(hash)))
        return HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
    namespace_count = vkd3d_cache_read_u32(&data[20]);
    if (namespace_count > VKD3D_CACHE_ROOT_MAXIMUM_NAMESPACES)
        return HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);

    memset(state, 0, sizeof(*state));
    state->namespace_count = namespace_count;
    state->entry_count = vkd3d_cache_read_u32(&data[28]);
    state->physical_size = vkd3d_cache_read_u64(&data[40]);
    state->serial = vkd3d_cache_read_u64(&data[48]);
    for (i = 0; i < namespace_count; ++i)
    {
        p = vkd3d_cache_root_namespace_data((uint8_t *)data, i);
        memcpy(state->namespaces[i].hash, p, VKD3D_CACHE_SHA256_SIZE);
        state->namespaces[i].physical_size = vkd3d_cache_read_u64(&p[32]);
        state->namespaces[i].last_used = vkd3d_cache_read_u64(&p[40]);
        state->namespaces[i].entry_count = vkd3d_cache_read_u32(&p[48]);
    }
    for (i = 0; i < VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS; ++i)
    {
        p = vkd3d_cache_root_reservation_data((uint8_t *)data, i);
        state->reservations[i].flags = vkd3d_cache_read_u32(&p[104]);
        if (!state->reservations[i].flags)
            continue;
        memcpy(state->reservations[i].namespace_hash, p, VKD3D_CACHE_SHA256_SIZE);
        memcpy(state->reservations[i].entry_hash, &p[32], VKD3D_CACHE_SHA256_SIZE);
        memcpy(state->reservations[i].temp_hash, &p[64], VKD3D_CACHE_SHA256_SIZE);
        state->reservations[i].physical_size = vkd3d_cache_read_u64(&p[96]);
        ++reservation_count;
    }
    if (reservation_count != vkd3d_cache_read_u32(&data[24]))
        return HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
    if (FAILED(hr = vkd3d_cache_build_root_state(state,
            (uint8_t[VKD3D_CACHE_ROOT_STATE_SIZE]){0})))
        return HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
    return S_OK;
}

static HRESULT vkd3d_cache_read_root_state_path(const WCHAR *path,
        struct vkd3d_cache_root_state *state)
{
    uint8_t data[VKD3D_CACHE_ROOT_STATE_SIZE];
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE file;
    HRESULT hr;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return vkd3d_cache_last_error();
    if (!GetFileInformationByHandle(file, &info))
        hr = vkd3d_cache_last_error();
    else if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)
            || info.nFileSizeHigh || info.nFileSizeLow != VKD3D_CACHE_ROOT_STATE_SIZE)
        hr = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
    else if (FAILED(hr = vkd3d_cache_file_read(file, data, sizeof(data))))
        ;
    else
        hr = vkd3d_cache_parse_root_state(data, state);
    CloseHandle(file);
    return hr;
}

static HRESULT vkd3d_cache_read_root_state(struct vkd3d_shader_cache_disk *disk,
        struct vkd3d_cache_root_state *state)
{
    return vkd3d_cache_read_root_state_path(disk->root_state_path, state);
}

static HRESULT vkd3d_cache_write_root_state(struct vkd3d_shader_cache_disk *disk,
        struct vkd3d_cache_root_state *state)
{
    uint8_t data[VKD3D_CACHE_ROOT_STATE_SIZE];
    BY_HANDLE_FILE_INFORMATION info;
    bool exists;
    HANDLE file = INVALID_HANDLE_VALUE;
    HRESULT hr;

    ++state->serial;
    if (FAILED(hr = vkd3d_cache_build_root_state(state, data)))
        return hr;
    if (FAILED(hr = vkd3d_cache_regular_file_exists(disk->root_state_path, &exists)))
        return hr;
    if (FAILED(hr = vkd3d_cache_regular_file_exists(disk->root_state_temp_path, &exists)))
        return hr;
    if (exists && !DeleteFileW(disk->root_state_temp_path))
        return vkd3d_cache_last_error();

    file = CreateFileW(disk->root_state_temp_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
            CREATE_NEW, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT
            | FILE_FLAG_WRITE_THROUGH, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return vkd3d_cache_last_error();
    if (!GetFileInformationByHandle(file, &info))
        hr = vkd3d_cache_last_error();
    else if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        hr = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    else if (FAILED(hr = vkd3d_cache_file_write(file, data, sizeof(data))))
        ;
    else if (!FlushFileBuffers(file))
        hr = vkd3d_cache_last_error();
    else
        hr = S_OK;
    CloseHandle(file);
    if (SUCCEEDED(hr) && !MoveFileExW(disk->root_state_temp_path, disk->root_state_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        hr = vkd3d_cache_last_error();
    if (FAILED(hr) && !DeleteFileW(disk->root_state_temp_path)
            && GetLastError() != ERROR_FILE_NOT_FOUND)
        WARN("Failed to remove root quota state temporary file %s, error %lu.\n",
                debugstr_w(disk->root_state_temp_path, sizeof(WCHAR)), GetLastError());
    return hr;
}

static void vkd3d_cache_hash_tagged_data(struct vkd3d_sha256_context *ctx,
        uint32_t tag, const void *data, size_t size)
{
    uint8_t header[12];

    vkd3d_cache_write_u32(header, tag);
    vkd3d_cache_write_u64(&header[4], size);
    vkd3d_cache_sha256_update(ctx, header, sizeof(header));
    vkd3d_cache_sha256_update(ctx, data, size);
}

static HRESULT vkd3d_cache_compute_namespace_hash(const struct vkd3d_shader_cache_desc *desc,
        uint8_t hash[VKD3D_CACHE_SHA256_SIZE])
{
    static const char name[] = "vkd3d-application-shader-cache-identity";
    struct vkd3d_sha256_context ctx;
    WCHAR *module_path;
    uint32_t flags = desc->flags;
    HRESULT hr;

    if (FAILED(hr = vkd3d_cache_get_module_path(&module_path)))
        return hr;

    vkd3d_cache_sha256_init(&ctx);
    vkd3d_cache_hash_tagged_data(&ctx, 1, name, sizeof(name));
    vkd3d_cache_hash_tagged_data(&ctx, 2, module_path,
            lstrlenW(module_path) * sizeof(*module_path));
    vkd3d_cache_hash_tagged_data(&ctx, 5, &desc->identifier, sizeof(desc->identifier));
    vkd3d_cache_hash_tagged_data(&ctx, 7, &flags, sizeof(flags));
    if (desc->flags & D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED)
        vkd3d_cache_hash_tagged_data(&ctx, 8, desc->driver_identity, desc->driver_identity_size);
    vkd3d_cache_sha256_final(&ctx, hash);
    vkd3d_free(module_path);
    return S_OK;
}

static HRESULT vkd3d_cache_get_application_root(WCHAR **root_path)
{
    uint8_t hash[VKD3D_CACHE_SHA256_SIZE];
    WCHAR hex[VKD3D_CACHE_HEX_SIZE + 1];
    WCHAR *module_path = NULL, *base_path = NULL, *path = NULL;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HRESULT hr;

    if (FAILED(hr = vkd3d_cache_get_module_path(&module_path)))
        goto done;
    vkd3d_cache_sha256(module_path, lstrlenW(module_path) * sizeof(*module_path), hash);
    vkd3d_cache_hash_to_hex(hash, hex);

    if (FAILED(hr = vkd3d_cache_get_dynamic_path(vkd3d_cache_get_temp_directory, &base_path)))
        goto done;
    if (FAILED(hr = vkd3d_cache_path_join(base_path, vkd3d_cache_base_name, &path)))
        goto done;
    if (FAILED(hr = vkd3d_cache_ensure_directory(path, &handle)))
        goto done;
    CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
    vkd3d_free(base_path);
    base_path = path;
    path = NULL;

    if (FAILED(hr = vkd3d_cache_path_join(base_path, hex, &path)))
        goto done;
    if (FAILED(hr = vkd3d_cache_ensure_directory(path, &handle)))
        goto done;
    CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;

    *root_path = path;
    path = NULL;
done:
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    vkd3d_free(module_path);
    vkd3d_free(base_path);
    vkd3d_free(path);
    return hr;
}

static HRESULT vkd3d_cache_get_working_root(WCHAR **root_path)
{
    WCHAR *working_path = NULL, *path = NULL;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HRESULT hr;

    if (FAILED(hr = vkd3d_cache_get_dynamic_path(vkd3d_cache_get_current_directory, &working_path)))
        goto done;
    if (FAILED(hr = vkd3d_cache_path_join(working_path, vkd3d_cache_base_name, &path)))
        goto done;
    if (FAILED(hr = vkd3d_cache_ensure_directory(path, &handle)))
        goto done;
    CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;

    *root_path = path;
    path = NULL;
done:
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    vkd3d_free(working_path);
    vkd3d_free(path);
    return hr;
}

static void vkd3d_cache_disk_cleanup(struct vkd3d_shader_cache_disk *disk)
{
    if (disk->root_lock_handle != INVALID_HANDLE_VALUE)
        CloseHandle(disk->root_lock_handle);
    if (disk->lock_handle != INVALID_HANDLE_VALUE)
        CloseHandle(disk->lock_handle);
    if (disk->temp_handle != INVALID_HANDLE_VALUE)
        CloseHandle(disk->temp_handle);
    if (disk->entries_handle != INVALID_HANDLE_VALUE)
        CloseHandle(disk->entries_handle);
    if (disk->root_handle != INVALID_HANDLE_VALUE)
        CloseHandle(disk->root_handle);
    if (disk->cache_root_handle != INVALID_HANDLE_VALUE)
        CloseHandle(disk->cache_root_handle);
    vkd3d_free(disk->marker_path);
    vkd3d_free(disk->manifest_path);
    vkd3d_free(disk->quota_path);
    vkd3d_free(disk->lock_path);
    vkd3d_free(disk->temp_path);
    vkd3d_free(disk->entries_path);
    vkd3d_free(disk->root_path);
    vkd3d_free(disk->root_state_temp_path);
    vkd3d_free(disk->root_state_path);
    vkd3d_free(disk->root_lock_path);
    vkd3d_free(disk->cache_root_path);
    memset(disk, 0, sizeof(*disk));
    disk->cache_root_handle = INVALID_HANDLE_VALUE;
    disk->root_lock_handle = INVALID_HANDLE_VALUE;
    disk->root_handle = INVALID_HANDLE_VALUE;
    disk->entries_handle = INVALID_HANDLE_VALUE;
    disk->temp_handle = INVALID_HANDLE_VALUE;
    disk->lock_handle = INVALID_HANDLE_VALUE;
}

static HRESULT vkd3d_cache_delete_hex_files(const WCHAR *directory, uint32_t maximum_objects)
{
    WIN32_FIND_DATAW data;
    WCHAR *search_path = NULL, *path = NULL;
    HANDLE search;
    HRESULT first_error = S_OK, hr;
    uint32_t object_count = 0;
    DWORD error;

    if (FAILED(hr = vkd3d_cache_path_join(directory, L"*", &search_path)))
        return hr;
    search = FindFirstFileW(search_path, &data);
    vkd3d_free(search_path);
    if (search == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    }

    do
    {
        if (++object_count > maximum_objects)
        {
            if (SUCCEEDED(first_error))
                first_error = DXGI_ERROR_CACHE_FULL;
            break;
        }
        if (!vkd3d_cache_is_hex_name(data.cFileName))
            continue;
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        {
            WARN("Refusing unsafe object in shader cache directory %s.\n",
                    debugstr_w(directory, sizeof(WCHAR)));
            if (SUCCEEDED(first_error))
                first_error = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
            continue;
        }
        if (FAILED(hr = vkd3d_cache_path_join(directory, data.cFileName, &path)))
        {
            if (SUCCEEDED(first_error))
                first_error = hr;
            continue;
        }
        if (!DeleteFileW(path))
        {
            error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && SUCCEEDED(first_error))
                first_error = HRESULT_FROM_WIN32(error);
        }
        vkd3d_free(path);
        path = NULL;
    }
    while (FindNextFileW(search, &data));

    error = GetLastError();
    if (error != ERROR_NO_MORE_FILES && SUCCEEDED(first_error))
        first_error = HRESULT_FROM_WIN32(error);
    FindClose(search);
    vkd3d_free(path);
    return first_error;
}

static bool vkd3d_cache_hex_to_hash(const WCHAR *hex, uint8_t hash[VKD3D_CACHE_SHA256_SIZE])
{
    unsigned int high, low, i;

    if (!vkd3d_cache_is_hex_name(hex))
        return false;
    for (i = 0; i < VKD3D_CACHE_SHA256_SIZE; ++i)
    {
        high = hex[2 * i] <= '9' ? hex[2 * i] - '0' : hex[2 * i] - 'a' + 10;
        low = hex[2 * i + 1] <= '9' ? hex[2 * i + 1] - '0' : hex[2 * i + 1] - 'a' + 10;
        hash[i] = high << 4 | low;
    }
    return true;
}

static int vkd3d_cache_root_find_namespace(const struct vkd3d_cache_root_state *state,
        const uint8_t hash[VKD3D_CACHE_SHA256_SIZE])
{
    unsigned int i;

    for (i = 0; i < state->namespace_count; ++i)
    {
        if (!memcmp(state->namespaces[i].hash, hash, VKD3D_CACHE_SHA256_SIZE))
            return i;
    }
    return -1;
}

static HRESULT vkd3d_cache_root_check_inventory(struct vkd3d_shader_cache_disk *disk,
        const struct vkd3d_cache_root_state *state, bool *matches, uint32_t *object_count)
{
    bool seen[VKD3D_CACHE_ROOT_MAXIMUM_NAMESPACES] = {0};
    uint8_t hash[VKD3D_CACHE_SHA256_SIZE];
    WIN32_FIND_DATAW data;
    WCHAR *search_path = NULL;
    HANDLE search;
    HRESULT hr;
    DWORD error;
    int index;
    unsigned int i;

    *matches = true;
    *object_count = 0;
    if (FAILED(hr = vkd3d_cache_path_join(disk->cache_root_path, L"*", &search_path)))
        return hr;
    search = FindFirstFileW(search_path, &data);
    vkd3d_free(search_path);
    if (search == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    }

    hr = S_OK;
    do
    {
        if (++*object_count > VKD3D_CACHE_ROOT_MAXIMUM_SCAN_OBJECTS)
        {
            hr = DXGI_ERROR_CACHE_FULL;
            break;
        }
        if (!vkd3d_cache_is_hex_name(data.cFileName))
            continue;
        if (!vkd3d_cache_hex_to_hash(data.cFileName, hash))
        {
            hr = E_UNEXPECTED;
            break;
        }
        index = vkd3d_cache_root_find_namespace(state, hash);
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                || data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT
                || index < 0 || seen[index])
        {
            *matches = false;
            continue;
        }
        seen[index] = true;
    }
    while (FindNextFileW(search, &data));
    error = GetLastError();
    if (SUCCEEDED(hr) && error != ERROR_NO_MORE_FILES)
        hr = HRESULT_FROM_WIN32(error);
    FindClose(search);
    if (FAILED(hr))
        return hr;

    for (i = 0; i < state->namespace_count; ++i)
    {
        if (!seen[i])
        {
            *matches = false;
            break;
        }
    }
    return S_OK;
}

static void vkd3d_cache_root_remove_namespace(struct vkd3d_cache_root_state *state,
        uint32_t index)
{
    VKD3D_ASSERT(index < state->namespace_count);
    VKD3D_ASSERT(state->physical_size >= state->namespaces[index].physical_size);
    VKD3D_ASSERT(state->entry_count >= state->namespaces[index].entry_count);
    state->physical_size -= state->namespaces[index].physical_size;
    state->entry_count -= state->namespaces[index].entry_count;
    --state->namespace_count;
    if (index != state->namespace_count)
        state->namespaces[index] = state->namespaces[state->namespace_count];
    memset(&state->namespaces[state->namespace_count], 0,
            sizeof(state->namespaces[state->namespace_count]));
}

static HRESULT vkd3d_cache_root_check_no_reservations(struct vkd3d_shader_cache_disk *disk)
{
    unsigned int locked_count = 0, i;
    HRESULT hr = S_OK, unlock_hr;

    for (i = 0; i < VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS; ++i)
    {
        hr = vkd3d_cache_lock_range(disk->root_lock_handle,
                VKD3D_CACHE_ROOT_RESERVATION_LOCK_OFFSET(i), true, true);
        if (FAILED(hr))
            break;
        ++locked_count;
    }
    while (locked_count)
    {
        --locked_count;
        unlock_hr = vkd3d_cache_unlock_range(disk->root_lock_handle,
                VKD3D_CACHE_ROOT_RESERVATION_LOCK_OFFSET(locked_count));
        if (FAILED(unlock_hr) && SUCCEEDED(hr))
            hr = unlock_hr;
    }
    return hr;
}

static HRESULT vkd3d_cache_open_entry(const WCHAR *path, HANDLE *file, uint64_t *file_size,
        FILETIME *last_write_time);

static HRESULT vkd3d_cache_root_scan_entry_directory(const WCHAR *entries_path,
        uint64_t *physical_size, uint32_t *entry_count,
        uint32_t *candidate_count, uint32_t *object_count)
{
    WIN32_FIND_DATAW data;
    WCHAR *search_path = NULL, *path = NULL;
    uint64_t file_size;
    HANDLE search, file;
    HRESULT hr;
    DWORD error;

    if (FAILED(hr = vkd3d_cache_path_join(entries_path, L"*", &search_path)))
        return hr;
    search = FindFirstFileW(search_path, &data);
    vkd3d_free(search_path);
    if (search == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    }

    hr = S_OK;
    do
    {
        if (++*object_count > VKD3D_CACHE_ROOT_MAXIMUM_ENTRY_SCAN_OBJECTS)
        {
            hr = DXGI_ERROR_CACHE_FULL;
            break;
        }
        if (!vkd3d_cache_is_hex_name(data.cFileName))
            continue;
        if (*candidate_count == VKD3D_CACHE_ROOT_MAXIMUM_ENTRIES)
        {
            hr = DXGI_ERROR_CACHE_FULL;
            break;
        }
        ++*candidate_count;
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        {
            WARN("Ignoring unsafe object in shader cache entry directory %s.\n",
                    debugstr_w(entries_path, sizeof(WCHAR)));
            continue;
        }
        if (FAILED(hr = vkd3d_cache_path_join(entries_path, data.cFileName, &path)))
            break;
        file = INVALID_HANDLE_VALUE;
        if (FAILED(hr = vkd3d_cache_open_entry(path, &file, &file_size, NULL)))
        {
            if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                hr = S_OK;
            vkd3d_free(path);
            path = NULL;
            if (FAILED(hr))
                break;
            continue;
        }
        CloseHandle(file);
        if (*physical_size > UINT64_MAX - file_size)
        {
            hr = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            break;
        }
        *physical_size += file_size;
        if (*entry_count == UINT32_MAX)
        {
            hr = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            break;
        }
        ++*entry_count;
        vkd3d_free(path);
        path = NULL;
    }
    while (FindNextFileW(search, &data));
    error = GetLastError();
    if (SUCCEEDED(hr) && error != ERROR_NO_MORE_FILES)
        hr = HRESULT_FROM_WIN32(error);
    FindClose(search);
    vkd3d_free(path);
    return hr;
}

static HRESULT vkd3d_cache_root_scan_namespace(struct vkd3d_shader_cache_disk *disk,
        const WCHAR *name, struct vkd3d_cache_root_namespace *ns, bool clean_temp,
        uint32_t *entry_candidate_count, uint32_t *entry_object_count)
{
    BY_HANDLE_FILE_INFORMATION info;
    WCHAR *namespace_path = NULL, *entries_path = NULL, *temp_path = NULL;
    WCHAR *lock_path = NULL, *manifest_path = NULL;
    HANDLE namespace_handle = INVALID_HANDLE_VALUE, entries_handle = INVALID_HANDLE_VALUE;
    HANDLE temp_handle = INVALID_HANDLE_VALUE, lock_handle = INVALID_HANDLE_VALUE;
    HANDLE manifest = INVALID_HANDLE_VALUE;
    bool operation_locked = false, lifecycle_locked = false;
    HRESULT hr;
    DWORD error;

    memset(ns, 0, sizeof(*ns));
    if (!vkd3d_cache_hex_to_hash(name, ns->hash))
        return E_INVALIDARG;
    if (FAILED(hr = vkd3d_cache_path_join(disk->cache_root_path, name, &namespace_path))
            || FAILED(hr = vkd3d_cache_open_directory(namespace_path, &namespace_handle))
            || FAILED(hr = vkd3d_cache_path_join(namespace_path,
                    vkd3d_cache_entries_name, &entries_path))
            || FAILED(hr = vkd3d_cache_path_join(namespace_path,
                    vkd3d_cache_temp_name, &temp_path))
            || FAILED(hr = vkd3d_cache_path_join(namespace_path,
                    vkd3d_cache_lock_name, &lock_path))
            || FAILED(hr = vkd3d_cache_path_join(namespace_path,
                    vkd3d_cache_manifest_name, &manifest_path))
            || FAILED(hr = vkd3d_cache_open_lock_file(lock_path, OPEN_ALWAYS, &lock_handle)))
        goto done;
    if (FAILED(hr = vkd3d_cache_lock_range(lock_handle,
            VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET, false, false)))
        goto done;
    lifecycle_locked = true;
    if (FAILED(hr = vkd3d_cache_lock_range(lock_handle,
            VKD3D_CACHE_OPERATION_LOCK_OFFSET, true, false)))
        goto done;
    operation_locked = true;

    hr = vkd3d_cache_open_directory(entries_path, &entries_handle);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        hr = S_OK;
    else if (SUCCEEDED(hr))
        hr = vkd3d_cache_root_scan_entry_directory(entries_path,
                &ns->physical_size, &ns->entry_count,
                entry_candidate_count, entry_object_count);
    if (FAILED(hr))
        goto done;
    hr = vkd3d_cache_open_directory(temp_path, &temp_handle);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        hr = S_OK;
    else if (SUCCEEDED(hr) && clean_temp)
        hr = vkd3d_cache_delete_hex_files(temp_path,
                VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS + 2u);
    if (FAILED(hr))
        goto done;

    manifest = CreateFileW(manifest_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (manifest != INVALID_HANDLE_VALUE)
    {
        if (!GetFileInformationByHandle(manifest, &info))
            hr = vkd3d_cache_last_error();
        else if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            hr = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        else
            ns->last_used = vkd3d_cache_filetime_to_u64(&info.ftLastWriteTime);
    }
    else
    {
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            hr = HRESULT_FROM_WIN32(error);
    }
    if (SUCCEEDED(hr) && !ns->last_used)
    {
        if (!GetFileInformationByHandle(namespace_handle, &info))
            hr = vkd3d_cache_last_error();
        else
            ns->last_used = vkd3d_cache_filetime_to_u64(&info.ftLastWriteTime);
    }

done:
    if (manifest != INVALID_HANDLE_VALUE)
        CloseHandle(manifest);
    if (operation_locked)
    {
        HRESULT unlock_hr = vkd3d_cache_unlock_range(lock_handle,
                VKD3D_CACHE_OPERATION_LOCK_OFFSET);
        if (FAILED(unlock_hr) && SUCCEEDED(hr))
            hr = unlock_hr;
    }
    if (lifecycle_locked)
    {
        HRESULT unlock_hr = vkd3d_cache_unlock_range(lock_handle,
                VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET);
        if (FAILED(unlock_hr) && SUCCEEDED(hr))
            hr = unlock_hr;
    }
    if (lock_handle != INVALID_HANDLE_VALUE)
        CloseHandle(lock_handle);
    if (temp_handle != INVALID_HANDLE_VALUE)
        CloseHandle(temp_handle);
    if (entries_handle != INVALID_HANDLE_VALUE)
        CloseHandle(entries_handle);
    if (namespace_handle != INVALID_HANDLE_VALUE)
        CloseHandle(namespace_handle);
    vkd3d_free(manifest_path);
    vkd3d_free(lock_path);
    vkd3d_free(temp_path);
    vkd3d_free(entries_path);
    vkd3d_free(namespace_path);
    return hr;
}

static HRESULT vkd3d_cache_root_clear_namespace(struct vkd3d_shader_cache_disk *disk,
        const uint8_t namespace_hash[VKD3D_CACHE_SHA256_SIZE], bool *removed);

static HRESULT vkd3d_cache_root_scan(struct vkd3d_shader_cache_disk *disk,
        struct vkd3d_cache_root_state *state, bool clean_temp)
{
    WIN32_FIND_DATAW data;
    WCHAR *search_path = NULL;
    HANDLE search;
    HRESULT hr;
    uint32_t namespace_candidate_count = 0, namespace_object_count = 0;
    uint32_t entry_candidate_count = 0, entry_object_count = 0;
    DWORD error;

    memset(state, 0, sizeof(*state));
    if (FAILED(hr = vkd3d_cache_path_join(disk->cache_root_path, L"*", &search_path)))
        return hr;
    search = FindFirstFileW(search_path, &data);
    vkd3d_free(search_path);
    if (search == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    }

    hr = S_OK;
    do
    {
        struct vkd3d_cache_root_namespace *ns;

        if (++namespace_object_count > VKD3D_CACHE_ROOT_MAXIMUM_SCAN_OBJECTS)
        {
            hr = DXGI_ERROR_CACHE_FULL;
            break;
        }
        if (!vkd3d_cache_is_hex_name(data.cFileName))
            continue;
        if (namespace_candidate_count == VKD3D_CACHE_ROOT_MAXIMUM_SCAN_NAMESPACES)
        {
            hr = DXGI_ERROR_CACHE_FULL;
            break;
        }
        ++namespace_candidate_count;
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                || data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        {
            WARN("Ignoring unsafe shader cache namespace candidate %s.\n",
                    debugstr_w(data.cFileName, sizeof(WCHAR)));
            continue;
        }
        if (state->namespace_count == VKD3D_CACHE_ROOT_MAXIMUM_NAMESPACES)
        {
            struct vkd3d_cache_root_namespace extra;
            bool attempted[VKD3D_CACHE_ROOT_MAXIMUM_NAMESPACES] = {0};
            bool extra_attempted = false, removed;
            uint32_t attempted_count = 0, candidate, i;

            if (FAILED(hr = vkd3d_cache_root_scan_namespace(disk,
                    data.cFileName, &extra, clean_temp,
                    &entry_candidate_count, &entry_object_count)))
                break;
            for (;;)
            {
                candidate = UINT32_MAX;
                for (i = 0; i < state->namespace_count; ++i)
                {
                    if (!attempted[i] && (candidate == UINT32_MAX
                            || state->namespaces[i].last_used
                                    < state->namespaces[candidate].last_used))
                        candidate = i;
                }
                if (!extra_attempted && (candidate == UINT32_MAX
                        || extra.last_used < state->namespaces[candidate].last_used))
                    candidate = state->namespace_count;
                if (candidate == UINT32_MAX)
                {
                    hr = DXGI_ERROR_CACHE_FULL;
                    break;
                }
                hr = vkd3d_cache_root_clear_namespace(disk,
                        candidate == state->namespace_count ? extra.hash
                        : state->namespaces[candidate].hash, &removed);
                if (hr == S_FALSE)
                {
                    hr = S_OK;
                    if (candidate == state->namespace_count)
                        extra_attempted = true;
                    else
                    {
                        attempted[candidate] = true;
                        ++attempted_count;
                    }
                    if (attempted_count == state->namespace_count && extra_attempted)
                        hr = DXGI_ERROR_CACHE_FULL;
                    if (SUCCEEDED(hr))
                        continue;
                    break;
                }
                if (FAILED(hr))
                    break;
                if (!removed)
                {
                    if (candidate == state->namespace_count)
                    {
                        extra.physical_size = 0;
                        extra.entry_count = 0;
                        extra_attempted = true;
                    }
                    else
                    {
                        VKD3D_ASSERT(state->physical_size
                                >= state->namespaces[candidate].physical_size);
                        VKD3D_ASSERT(state->entry_count
                                >= state->namespaces[candidate].entry_count);
                        state->physical_size -= state->namespaces[candidate].physical_size;
                        state->entry_count -= state->namespaces[candidate].entry_count;
                        state->namespaces[candidate].physical_size = 0;
                        state->namespaces[candidate].entry_count = 0;
                        attempted[candidate] = true;
                        ++attempted_count;
                    }
                    continue;
                }
                if (candidate != state->namespace_count)
                {
                    vkd3d_cache_root_remove_namespace(state, candidate);
                    if (state->physical_size > UINT64_MAX - extra.physical_size
                            || state->entry_count > UINT32_MAX - extra.entry_count)
                    {
                        hr = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
                        break;
                    }
                    state->namespaces[state->namespace_count++] = extra;
                    state->physical_size += extra.physical_size;
                    state->entry_count += extra.entry_count;
                }
                break;
            }
            if (FAILED(hr))
                break;
            continue;
        }
        ns = &state->namespaces[state->namespace_count];
        if (FAILED(hr = vkd3d_cache_root_scan_namespace(disk, data.cFileName, ns, clean_temp,
                &entry_candidate_count, &entry_object_count)))
            break;
        if (state->physical_size > UINT64_MAX - ns->physical_size)
        {
            hr = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            break;
        }
        state->physical_size += ns->physical_size;
        if (state->entry_count > UINT32_MAX - ns->entry_count)
        {
            hr = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            break;
        }
        state->entry_count += ns->entry_count;
        ++state->namespace_count;
    }
    while (FindNextFileW(search, &data));
    error = GetLastError();
    if (SUCCEEDED(hr) && error != ERROR_NO_MORE_FILES)
        hr = HRESULT_FROM_WIN32(error);
    FindClose(search);
    return hr;
}

static HRESULT vkd3d_cache_delete_known_regular_file(const WCHAR *path)
{
    bool exists;
    DWORD error;
    HRESULT hr;

    if (FAILED(hr = vkd3d_cache_regular_file_exists(path, &exists)) || !exists)
        return hr;
    if (!DeleteFileW(path))
    {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    }
    return S_OK;
}

static bool vkd3d_cache_is_known_namespace_object(const WCHAR *name)
{
    return !lstrcmpW(name, vkd3d_cache_entries_name)
            || !lstrcmpW(name, vkd3d_cache_temp_name)
            || !lstrcmpW(name, vkd3d_cache_lock_name)
            || !lstrcmpW(name, vkd3d_cache_marker_name)
            || !lstrcmpW(name, vkd3d_cache_manifest_name)
            || !lstrcmpW(name, vkd3d_cache_quota_name);
}

static HRESULT vkd3d_cache_namespace_has_unknown_objects(const WCHAR *namespace_path,
        bool *has_unknown)
{
    WIN32_FIND_DATAW data;
    WCHAR *search_path = NULL;
    HANDLE search;
    HRESULT hr;
    DWORD error;

    *has_unknown = false;
    if (FAILED(hr = vkd3d_cache_path_join(namespace_path, L"*", &search_path)))
        return hr;
    search = FindFirstFileW(search_path, &data);
    vkd3d_free(search_path);
    if (search == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    }
    do
    {
        if (!lstrcmpW(data.cFileName, L".") || !lstrcmpW(data.cFileName, L".."))
            continue;
        if (!vkd3d_cache_is_known_namespace_object(data.cFileName))
        {
            *has_unknown = true;
            break;
        }
    }
    while (FindNextFileW(search, &data));
    error = GetLastError();
    FindClose(search);
    if (!*has_unknown && error != ERROR_NO_MORE_FILES)
        return HRESULT_FROM_WIN32(error);
    return S_OK;
}

static HRESULT vkd3d_cache_root_clear_namespace(struct vkd3d_shader_cache_disk *disk,
        const uint8_t namespace_hash[VKD3D_CACHE_SHA256_SIZE], bool *removed)
{
    WCHAR name[VKD3D_CACHE_HEX_SIZE + 1];
    WCHAR *namespace_path = NULL, *entries_path = NULL, *temp_path = NULL;
    WCHAR *lock_path = NULL, *marker_path = NULL, *manifest_path = NULL, *quota_path = NULL;
    HANDLE namespace_handle = INVALID_HANDLE_VALUE, entries_handle = INVALID_HANDLE_VALUE;
    HANDLE temp_handle = INVALID_HANDLE_VALUE, lock_handle = INVALID_HANDLE_VALUE;
    bool operation_locked = false, lifecycle_locked = false, has_unknown = true;
    bool entries_removed = false, temp_removed = false;
    HRESULT hr = S_OK, cleanup_hr;
    DWORD error;

    *removed = false;
    vkd3d_cache_hash_to_hex(namespace_hash, name);
    if (FAILED(hr = vkd3d_cache_path_join(disk->cache_root_path, name, &namespace_path))
            || FAILED(hr = vkd3d_cache_open_directory(namespace_path, &namespace_handle))
            || FAILED(hr = vkd3d_cache_path_join(namespace_path,
                    vkd3d_cache_entries_name, &entries_path))
            || FAILED(hr = vkd3d_cache_path_join(namespace_path,
                    vkd3d_cache_temp_name, &temp_path))
            || FAILED(hr = vkd3d_cache_path_join(namespace_path,
                    vkd3d_cache_lock_name, &lock_path))
            || FAILED(hr = vkd3d_cache_path_join(namespace_path,
                    vkd3d_cache_marker_name, &marker_path))
            || FAILED(hr = vkd3d_cache_path_join(namespace_path,
                    vkd3d_cache_manifest_name, &manifest_path))
            || FAILED(hr = vkd3d_cache_path_join(namespace_path,
                    vkd3d_cache_quota_name, &quota_path))
            || FAILED(hr = vkd3d_cache_open_lock_file(lock_path, OPEN_EXISTING, &lock_handle)))
        goto done;

    hr = vkd3d_cache_lock_range(lock_handle,
            VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET, true, true);
    if (hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
    {
        hr = S_FALSE;
        goto done;
    }
    if (FAILED(hr))
        goto done;
    lifecycle_locked = true;
    if (FAILED(hr = vkd3d_cache_lock_range(lock_handle,
            VKD3D_CACHE_OPERATION_LOCK_OFFSET, true, false)))
        goto done;
    operation_locked = true;

    cleanup_hr = vkd3d_cache_open_directory(entries_path, &entries_handle);
    if (cleanup_hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            || cleanup_hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        entries_removed = true;
    else if (FAILED(cleanup_hr))
        hr = cleanup_hr;
    else if (FAILED(cleanup_hr = vkd3d_cache_delete_hex_files(entries_path,
            VKD3D_CACHE_MAXIMUM_DISK_ENTRIES + 2u)))
        hr = cleanup_hr;

    cleanup_hr = vkd3d_cache_open_directory(temp_path, &temp_handle);
    if (cleanup_hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            || cleanup_hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        temp_removed = true;
    else if (FAILED(cleanup_hr) && SUCCEEDED(hr))
        hr = cleanup_hr;
    else if (SUCCEEDED(cleanup_hr)
            && FAILED(cleanup_hr = vkd3d_cache_delete_hex_files(temp_path,
                    VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS + 2u)) && SUCCEEDED(hr))
        hr = cleanup_hr;
    if (FAILED(hr))
        goto done;

    if (entries_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(entries_handle);
        entries_handle = INVALID_HANDLE_VALUE;
        if (RemoveDirectoryW(entries_path))
            entries_removed = true;
        else if ((error = GetLastError()) != ERROR_DIR_NOT_EMPTY)
        {
            hr = HRESULT_FROM_WIN32(error);
            goto done;
        }
    }
    if (temp_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(temp_handle);
        temp_handle = INVALID_HANDLE_VALUE;
        if (RemoveDirectoryW(temp_path))
            temp_removed = true;
        else if ((error = GetLastError()) != ERROR_DIR_NOT_EMPTY)
        {
            hr = HRESULT_FROM_WIN32(error);
            goto done;
        }
    }
    if (!entries_removed || !temp_removed)
        goto done;
    if (FAILED(hr = vkd3d_cache_namespace_has_unknown_objects(namespace_path, &has_unknown))
            || has_unknown)
        goto done;
    if (FAILED(hr = vkd3d_cache_delete_known_regular_file(marker_path))
            || FAILED(hr = vkd3d_cache_delete_known_regular_file(quota_path))
            || FAILED(hr = vkd3d_cache_delete_known_regular_file(manifest_path)))
        goto done;

done:
    if (operation_locked)
    {
        cleanup_hr = vkd3d_cache_unlock_range(lock_handle,
                VKD3D_CACHE_OPERATION_LOCK_OFFSET);
        if (FAILED(cleanup_hr) && SUCCEEDED(hr))
            hr = cleanup_hr;
        operation_locked = false;
    }
    if (lifecycle_locked)
    {
        cleanup_hr = vkd3d_cache_unlock_range(lock_handle,
                VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET);
        if (FAILED(cleanup_hr) && SUCCEEDED(hr))
            hr = cleanup_hr;
        lifecycle_locked = false;
    }
    if (lock_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(lock_handle);
        lock_handle = INVALID_HANDLE_VALUE;
    }
    if (temp_handle != INVALID_HANDLE_VALUE)
        CloseHandle(temp_handle);
    if (entries_handle != INVALID_HANDLE_VALUE)
        CloseHandle(entries_handle);
    if (namespace_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(namespace_handle);
        namespace_handle = INVALID_HANDLE_VALUE;
    }
    if (SUCCEEDED(hr) && entries_removed && temp_removed && !has_unknown)
    {
        if (FAILED(hr = vkd3d_cache_delete_known_regular_file(lock_path)))
            ;
        else if (!RemoveDirectoryW(namespace_path))
        {
            error = GetLastError();
            hr = error == ERROR_DIR_NOT_EMPTY ? S_FALSE : HRESULT_FROM_WIN32(error);
        }
        else
            *removed = true;
    }
    vkd3d_free(quota_path);
    vkd3d_free(manifest_path);
    vkd3d_free(marker_path);
    vkd3d_free(lock_path);
    vkd3d_free(temp_path);
    vkd3d_free(entries_path);
    vkd3d_free(namespace_path);
    return hr;
}

static uint64_t vkd3d_cache_root_reserved_size(const struct vkd3d_cache_root_state *state)
{
    uint64_t size = 0;
    unsigned int i;

    for (i = 0; i < VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS; ++i)
    {
        if (state->reservations[i].flags)
            size += state->reservations[i].physical_size;
    }
    return size;
}

static uint32_t vkd3d_cache_root_reservation_count(const struct vkd3d_cache_root_state *state)
{
    uint32_t count = 0;
    unsigned int i;

    for (i = 0; i < VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS; ++i)
    {
        if (state->reservations[i].flags)
            ++count;
    }
    return count;
}

static HRESULT vkd3d_cache_root_discard_namespace_reservations(
        struct vkd3d_shader_cache_disk *disk, struct vkd3d_cache_root_state *state,
        const uint8_t namespace_hash[VKD3D_CACHE_SHA256_SIZE])
{
    HRESULT hr, unlock_hr;
    unsigned int i;

    for (i = 0; i < VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS; ++i)
    {
        if (!state->reservations[i].flags
                || memcmp(state->reservations[i].namespace_hash,
                        namespace_hash, VKD3D_CACHE_SHA256_SIZE))
            continue;
        if (FAILED(hr = vkd3d_cache_lock_range(disk->root_lock_handle,
                VKD3D_CACHE_ROOT_RESERVATION_LOCK_OFFSET(i), true, true)))
            return hr;
        memset(&state->reservations[i], 0, sizeof(state->reservations[i]));
        unlock_hr = vkd3d_cache_unlock_range(disk->root_lock_handle,
                VKD3D_CACHE_ROOT_RESERVATION_LOCK_OFFSET(i));
        if (FAILED(unlock_hr))
            return unlock_hr;
    }
    return S_OK;
}

static HRESULT vkd3d_cache_root_make_space(struct vkd3d_shader_cache_disk *disk,
        struct vkd3d_cache_root_state *state, uint64_t requested_size,
        uint32_t requested_entries, bool need_namespace_slot)
{
    bool attempted[VKD3D_CACHE_ROOT_MAXIMUM_NAMESPACES] = {0};
    uint64_t reserved_size;
    uint32_t reservation_count;
    unsigned int attempted_count = 0, candidate, i;
    bool removed;
    HRESULT hr;

    if (requested_size > VKD3D_CACHE_ROOT_MAXIMUM_PHYSICAL_SIZE)
        return DXGI_ERROR_CACHE_FULL;
    if (requested_entries > VKD3D_CACHE_ROOT_MAXIMUM_ENTRIES)
        return DXGI_ERROR_CACHE_FULL;
    reserved_size = vkd3d_cache_root_reserved_size(state);
    reservation_count = vkd3d_cache_root_reservation_count(state);
    while ((need_namespace_slot
            && state->namespace_count >= VKD3D_CACHE_ROOT_MAXIMUM_NAMESPACES)
            || reserved_size > VKD3D_CACHE_ROOT_MAXIMUM_PHYSICAL_SIZE - requested_size
            || state->physical_size > VKD3D_CACHE_ROOT_MAXIMUM_PHYSICAL_SIZE
                    - requested_size - reserved_size
            || reservation_count > VKD3D_CACHE_ROOT_MAXIMUM_ENTRIES - requested_entries
            || state->entry_count > VKD3D_CACHE_ROOT_MAXIMUM_ENTRIES
                    - requested_entries - reservation_count)
    {
        if (attempted_count == state->namespace_count)
            return DXGI_ERROR_CACHE_FULL;
        candidate = UINT32_MAX;
        for (i = 0; i < state->namespace_count; ++i)
        {
            if (!attempted[i] && (candidate == UINT32_MAX
                    || state->namespaces[i].last_used < state->namespaces[candidate].last_used))
                candidate = i;
        }
        VKD3D_ASSERT(candidate != UINT32_MAX);
        attempted[candidate] = true;
        ++attempted_count;
        hr = vkd3d_cache_root_clear_namespace(disk, state->namespaces[candidate].hash, &removed);
        if (hr == S_FALSE)
            continue;
        if (FAILED(hr))
            return hr;

        if (FAILED(hr = vkd3d_cache_root_discard_namespace_reservations(disk, state,
                state->namespaces[candidate].hash)))
            return hr;

        VKD3D_ASSERT(state->physical_size >= state->namespaces[candidate].physical_size);
        VKD3D_ASSERT(state->entry_count >= state->namespaces[candidate].entry_count);
        state->physical_size -= state->namespaces[candidate].physical_size;
        state->entry_count -= state->namespaces[candidate].entry_count;
        state->namespaces[candidate].physical_size = 0;
        state->namespaces[candidate].entry_count = 0;
        if (removed)
        {
            --state->namespace_count;
            if (candidate != state->namespace_count)
            {
                state->namespaces[candidate] = state->namespaces[state->namespace_count];
                attempted[candidate] = attempted[state->namespace_count];
            }
            memset(&state->namespaces[state->namespace_count], 0,
                    sizeof(state->namespaces[state->namespace_count]));
            --attempted_count;
        }
        reserved_size = vkd3d_cache_root_reserved_size(state);
        reservation_count = vkd3d_cache_root_reservation_count(state);
    }
    return S_OK;
}

static bool vkd3d_cache_is_corrupt_hresult(HRESULT hr);

static HRESULT vkd3d_cache_ensure_root_state(struct vkd3d_shader_cache_disk *disk,
        struct vkd3d_cache_root_state *state)
{
    HRESULT hr, temp_hr;

    hr = vkd3d_cache_read_root_state(disk, state);
    if (SUCCEEDED(hr))
        return S_OK;
    temp_hr = vkd3d_cache_read_root_state_path(disk->root_state_temp_path, state);
    if (SUCCEEDED(temp_hr))
    {
        bool exists;

        if (FAILED(hr = vkd3d_cache_regular_file_exists(disk->root_state_path, &exists)))
            return hr;
        (void)exists;
        if (!MoveFileExW(disk->root_state_temp_path, disk->root_state_path,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return vkd3d_cache_last_error();
        return S_OK;
    }
    if (hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            && !vkd3d_cache_is_corrupt_hresult(hr))
        return hr;
    if (FAILED(temp_hr) && temp_hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            && !vkd3d_cache_is_corrupt_hresult(temp_hr))
        return temp_hr;
    if (FAILED(hr = vkd3d_cache_root_check_no_reservations(disk)))
        return hr;
    WARN("Rebuilding shader cache root quota state, hr %#lx, temporary hr %#lx.\n", hr, temp_hr);
    if (FAILED(hr = vkd3d_cache_root_scan(disk, state, true)))
        return hr;
    if (FAILED(hr = vkd3d_cache_root_make_space(disk, state, 0, 0, false)))
        return hr;
    return vkd3d_cache_write_root_state(disk, state);
}

static HRESULT vkd3d_cache_root_rebuild_if_idle(struct vkd3d_shader_cache_disk *disk,
        struct vkd3d_cache_root_state *state)
{
    HRESULT hr;

    if (FAILED(hr = vkd3d_cache_root_check_no_reservations(disk)))
        return hr;
    if (FAILED(hr = vkd3d_cache_root_scan(disk, state, true)))
        return hr;
    if (FAILED(hr = vkd3d_cache_root_make_space(disk, state, 0, 0, false)))
        return hr;
    return vkd3d_cache_write_root_state(disk, state);
}

static HRESULT vkd3d_cache_root_set_namespace_physical(
        struct vkd3d_cache_root_state *state,
        const uint8_t namespace_hash[VKD3D_CACHE_SHA256_SIZE], uint64_t physical_size,
        uint32_t entry_count)
{
    struct vkd3d_cache_root_namespace *ns;
    int index;

    if ((index = vkd3d_cache_root_find_namespace(state, namespace_hash)) < 0)
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    ns = &state->namespaces[index];
    VKD3D_ASSERT(state->physical_size >= ns->physical_size);
    VKD3D_ASSERT(state->entry_count >= ns->entry_count);
    state->physical_size -= ns->physical_size;
    state->entry_count -= ns->entry_count;
    if (state->physical_size > UINT64_MAX - physical_size)
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    if (state->entry_count > UINT32_MAX - entry_count)
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    ns->physical_size = physical_size;
    ns->entry_count = entry_count;
    state->physical_size += physical_size;
    state->entry_count += entry_count;
    return S_OK;
}

static HRESULT vkd3d_cache_root_reserve(struct vkd3d_shader_cache_disk *disk,
        struct vkd3d_cache_root_state *state,
        const uint8_t namespace_hash[VKD3D_CACHE_SHA256_SIZE],
        const uint8_t entry_hash[VKD3D_CACHE_SHA256_SIZE],
        const uint8_t temp_hash[VKD3D_CACHE_SHA256_SIZE], uint64_t physical_size,
        uint32_t *reservation_index)
{
    struct vkd3d_cache_root_reservation *reservation;
    unsigned int i;
    HRESULT hr;

retry:
    if (FAILED(hr = vkd3d_cache_root_make_space(disk, state, physical_size, 1, false)))
    {
        if (vkd3d_cache_root_reserved_size(state)
                && SUCCEEDED(vkd3d_cache_root_rebuild_if_idle(disk, state)))
            goto retry;
        if (FAILED(vkd3d_cache_write_root_state(disk, state)))
            WARN("Failed to persist partial shader cache root eviction.\n");
        return hr;
    }
    for (i = 0; i < VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS; ++i)
    {
        if (!state->reservations[i].flags)
            break;
    }
    if (i == VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS)
    {
        if (SUCCEEDED(hr = vkd3d_cache_root_rebuild_if_idle(disk, state)))
            goto retry;
        return hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION)
                ? DXGI_ERROR_CACHE_FULL : hr;
    }
    if (FAILED(hr = vkd3d_cache_lock_range(disk->root_lock_handle,
            VKD3D_CACHE_ROOT_RESERVATION_LOCK_OFFSET(i), true, true)))
        return hr;

    reservation = &state->reservations[i];
    memcpy(reservation->namespace_hash, namespace_hash, VKD3D_CACHE_SHA256_SIZE);
    memcpy(reservation->entry_hash, entry_hash, VKD3D_CACHE_SHA256_SIZE);
    memcpy(reservation->temp_hash, temp_hash, VKD3D_CACHE_SHA256_SIZE);
    reservation->physical_size = physical_size;
    reservation->flags = VKD3D_CACHE_ROOT_RESERVATION_ACTIVE;
    if (FAILED(hr = vkd3d_cache_write_root_state(disk, state)))
    {
        memset(reservation, 0, sizeof(*reservation));
        vkd3d_cache_unlock_range(disk->root_lock_handle,
                VKD3D_CACHE_ROOT_RESERVATION_LOCK_OFFSET(i));
        return hr;
    }
    *reservation_index = i;
    return S_OK;
}

static HRESULT vkd3d_cache_root_finish_reservation(struct vkd3d_shader_cache_disk *disk,
        uint32_t reservation_index, bool committed)
{
    struct vkd3d_cache_root_reservation *reservation;
    struct vkd3d_cache_root_state state;
    struct vkd3d_cache_root_namespace *ns;
    HRESULT hr, unlock_hr;
    int namespace_index;

    if (reservation_index >= VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS)
        return E_INVALIDARG;
    if (FAILED(hr = vkd3d_cache_read_root_state(disk, &state)))
        goto unlock;
    reservation = &state.reservations[reservation_index];
    if (reservation->flags != VKD3D_CACHE_ROOT_RESERVATION_ACTIVE)
    {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        goto unlock;
    }
    if (committed)
    {
        if ((namespace_index = vkd3d_cache_root_find_namespace(&state,
                reservation->namespace_hash)) < 0)
        {
            hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            goto unlock;
        }
        ns = &state.namespaces[namespace_index];
        if (state.physical_size > UINT64_MAX - reservation->physical_size
                || ns->physical_size > UINT64_MAX - reservation->physical_size)
        {
            hr = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            goto unlock;
        }
        state.physical_size += reservation->physical_size;
        ns->physical_size += reservation->physical_size;
        if (state.entry_count == UINT32_MAX || ns->entry_count == UINT32_MAX)
        {
            hr = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            goto unlock;
        }
        ++state.entry_count;
        ++ns->entry_count;
    }
    memset(reservation, 0, sizeof(*reservation));
    hr = vkd3d_cache_write_root_state(disk, &state);

unlock:
    unlock_hr = vkd3d_cache_unlock_range(disk->root_lock_handle,
            VKD3D_CACHE_ROOT_RESERVATION_LOCK_OFFSET(reservation_index));
    if (FAILED(unlock_hr) && SUCCEEDED(hr))
        hr = unlock_hr;
    return hr;
}

static HRESULT vkd3d_cache_disk_lock(struct vkd3d_shader_cache_disk *disk)
{
    return vkd3d_cache_lock_range(disk->lock_handle, VKD3D_CACHE_OPERATION_LOCK_OFFSET, true, false);
}

static HRESULT vkd3d_cache_disk_lock_shared(struct vkd3d_shader_cache_disk *disk)
{
    return vkd3d_cache_lock_range(disk->lock_handle, VKD3D_CACHE_OPERATION_LOCK_OFFSET, false, false);
}

static HRESULT vkd3d_cache_disk_unlock(struct vkd3d_shader_cache_disk *disk, HRESULT operation_hr)
{
    HRESULT hr;

    if (FAILED(hr = vkd3d_cache_unlock_range(disk->lock_handle, VKD3D_CACHE_OPERATION_LOCK_OFFSET)))
    {
        ERR("Failed to release shader cache operation lock, hr %#lx.\n", hr);
        if (SUCCEEDED(operation_hr))
            operation_hr = hr;
    }
    return operation_hr;
}

static void vkd3d_cache_build_header(const struct vkd3d_shader_cache *cache,
        const uint8_t key_hash[VKD3D_CACHE_SHA256_SIZE], const void *key, size_t key_size,
        const void *value, size_t value_size, uint8_t header[VKD3D_CACHE_HEADER_SIZE])
{
    struct vkd3d_sha256_context ctx;

    memset(header, 0, VKD3D_CACHE_HEADER_SIZE);
    memcpy(header, vkd3d_cache_magic, sizeof(vkd3d_cache_magic));
    vkd3d_cache_write_u32(&header[VKD3D_CACHE_HEADER_FORMAT_OFFSET], VKD3D_CACHE_FORMAT_VERSION);
    vkd3d_cache_write_u32(&header[VKD3D_CACHE_HEADER_SIZE_OFFSET], VKD3D_CACHE_HEADER_SIZE);
    vkd3d_cache_write_u32(&header[VKD3D_CACHE_HEADER_ABI_OFFSET], VKD3D_CACHE_ABI_VERSION);
    vkd3d_cache_write_u64(&header[VKD3D_CACHE_HEADER_KEY_SIZE_OFFSET], key_size);
    vkd3d_cache_write_u64(&header[VKD3D_CACHE_HEADER_VALUE_SIZE_OFFSET], value_size);
    vkd3d_cache_write_u64(&header[VKD3D_CACHE_HEADER_SESSION_VERSION_OFFSET], cache->version);
    memcpy(&header[VKD3D_CACHE_HEADER_NAMESPACE_OFFSET], cache->disk.namespace_hash,
            VKD3D_CACHE_SHA256_SIZE);
    memcpy(&header[VKD3D_CACHE_HEADER_KEY_HASH_OFFSET], key_hash, VKD3D_CACHE_SHA256_SIZE);

    vkd3d_cache_sha256_init(&ctx);
    vkd3d_cache_sha256_update(&ctx, key, key_size);
    vkd3d_cache_sha256_update(&ctx, value, value_size);
    vkd3d_cache_sha256_final(&ctx, &header[VKD3D_CACHE_HEADER_PAYLOAD_HASH_OFFSET]);
    vkd3d_cache_sha256(header, VKD3D_CACHE_HEADER_HASH_OFFSET,
            &header[VKD3D_CACHE_HEADER_HASH_OFFSET]);
}

static HRESULT vkd3d_cache_validate_header(const struct vkd3d_shader_cache *cache,
        const uint8_t header[VKD3D_CACHE_HEADER_SIZE], uint64_t file_size,
        uint64_t *key_size, uint64_t *value_size)
{
    uint8_t hash[VKD3D_CACHE_SHA256_SIZE];
    uint64_t expected_size;

    if (memcmp(header, vkd3d_cache_magic, sizeof(vkd3d_cache_magic))
            || vkd3d_cache_read_u32(&header[VKD3D_CACHE_HEADER_FORMAT_OFFSET]) != VKD3D_CACHE_FORMAT_VERSION
            || vkd3d_cache_read_u32(&header[VKD3D_CACHE_HEADER_SIZE_OFFSET]) != VKD3D_CACHE_HEADER_SIZE
            || vkd3d_cache_read_u32(&header[VKD3D_CACHE_HEADER_ABI_OFFSET]) != VKD3D_CACHE_ABI_VERSION
            || vkd3d_cache_read_u32(&header[VKD3D_CACHE_HEADER_FLAGS_OFFSET])
            || vkd3d_cache_read_u64(&header[VKD3D_CACHE_HEADER_SESSION_VERSION_OFFSET]) != cache->version
            || memcmp(&header[VKD3D_CACHE_HEADER_NAMESPACE_OFFSET], cache->disk.namespace_hash,
                    VKD3D_CACHE_SHA256_SIZE))
        return HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);

    vkd3d_cache_sha256(header, VKD3D_CACHE_HEADER_HASH_OFFSET, hash);
    if (memcmp(hash, &header[VKD3D_CACHE_HEADER_HASH_OFFSET], sizeof(hash)))
        return HRESULT_FROM_WIN32(ERROR_CRC);

    *key_size = vkd3d_cache_read_u64(&header[VKD3D_CACHE_HEADER_KEY_SIZE_OFFSET]);
    *value_size = vkd3d_cache_read_u64(&header[VKD3D_CACHE_HEADER_VALUE_SIZE_OFFSET]);
    if (!*key_size || !*value_size || *key_size > VKD3D_CACHE_MAXIMUM_KEY_SIZE
            || *value_size > cache->maximum_value_size
            || *key_size > UINT64_MAX - *value_size
            || *key_size + *value_size > UINT64_MAX - VKD3D_CACHE_HEADER_SIZE)
        return HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
    expected_size = VKD3D_CACHE_HEADER_SIZE + *key_size + *value_size;
    if (file_size != expected_size)
        return HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
    return S_OK;
}

static HRESULT vkd3d_cache_open_entry(const WCHAR *path, HANDLE *file, uint64_t *file_size,
        FILETIME *last_write_time)
{
    BY_HANDLE_FILE_INFORMATION info;
    ULARGE_INTEGER size;
    HANDLE h;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return vkd3d_cache_last_error();
    if (!GetFileInformationByHandle(h, &info))
    {
        HRESULT hr = vkd3d_cache_last_error();
        CloseHandle(h);
        return hr;
    }
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
    {
        CloseHandle(h);
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }

    size.u.LowPart = info.nFileSizeLow;
    size.u.HighPart = info.nFileSizeHigh;
    *file_size = size.QuadPart;
    if (last_write_time)
        *last_write_time = info.ftLastWriteTime;
    *file = h;
    return S_OK;
}

static HRESULT vkd3d_cache_read_entry_header(const struct vkd3d_shader_cache *cache,
        const WCHAR *path, uint8_t header[VKD3D_CACHE_HEADER_SIZE], uint64_t *key_size,
        uint64_t *value_size, FILETIME *last_write_time, HANDLE *file)
{
    LARGE_INTEGER position;
    uint64_t file_size = 0;
    HRESULT hr;

    *key_size = 0;
    *value_size = 0;
    if (FAILED(hr = vkd3d_cache_open_entry(path, file, &file_size, last_write_time)))
        return hr;
    position.QuadPart = 0;
    if (!SetFilePointerEx(*file, position, NULL, FILE_BEGIN))
        hr = vkd3d_cache_last_error();
    else if (file_size < VKD3D_CACHE_HEADER_SIZE)
        hr = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
    else if (FAILED(hr = vkd3d_cache_file_read(*file, header, VKD3D_CACHE_HEADER_SIZE)))
        ;
    else
        hr = vkd3d_cache_validate_header(cache, header, file_size, key_size, value_size);
    if (FAILED(hr))
    {
        CloseHandle(*file);
        *file = INVALID_HANDLE_VALUE;
    }
    return hr;
}

static HRESULT vkd3d_cache_disk_entry_path(const struct vkd3d_shader_cache_disk *disk,
        const uint8_t hash[VKD3D_CACHE_SHA256_SIZE], WCHAR **path)
{
    WCHAR hex[VKD3D_CACHE_HEX_SIZE + 1];

    vkd3d_cache_hash_to_hex(hash, hex);
    return vkd3d_cache_path_join(disk->entries_path, hex, path);
}

static bool vkd3d_cache_is_corrupt_hresult(HRESULT hr)
{
    return hr == HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT)
            || hr == HRESULT_FROM_WIN32(ERROR_CRC)
            || hr == HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)
            || hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
}

static HRESULT vkd3d_cache_write_namespace_metadata(
        const struct vkd3d_shader_cache_disk *disk,
        const uint8_t seed_hash[VKD3D_CACHE_SHA256_SIZE], const WCHAR *target_path,
        const void *data, size_t size)
{
    WCHAR name[VKD3D_CACHE_HEX_SIZE + 1];
    WCHAR *temp_path = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION info;
    HRESULT hr = S_OK;
    DWORD attempt, error;

    for (attempt = 0; attempt < 16; ++attempt)
    {
        vkd3d_cache_make_temp_name(seed_hash, attempt, name);
        if (FAILED(hr = vkd3d_cache_path_join(disk->temp_path, name, &temp_path)))
            goto done;
        file = CreateFileW(temp_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT
                | FILE_FLAG_WRITE_THROUGH, NULL);
        if (file != INVALID_HANDLE_VALUE)
            break;
        error = GetLastError();
        vkd3d_free(temp_path);
        temp_path = NULL;
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
        {
            hr = HRESULT_FROM_WIN32(error);
            goto done;
        }
    }
    if (file == INVALID_HANDLE_VALUE)
    {
        hr = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
        goto done;
    }
    if (!GetFileInformationByHandle(file, &info))
        hr = vkd3d_cache_last_error();
    else if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)
            || info.nNumberOfLinks != 1)
        hr = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    else if (FAILED(hr = vkd3d_cache_file_write(file, data, size)))
        ;
    else if (!FlushFileBuffers(file))
        hr = vkd3d_cache_last_error();
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    if (SUCCEEDED(hr) && !MoveFileExW(temp_path, target_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        hr = vkd3d_cache_last_error();

done:
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    if (FAILED(hr) && temp_path && !DeleteFileW(temp_path)
            && GetLastError() != ERROR_FILE_NOT_FOUND)
        WARN("Failed to remove shader cache metadata temporary file %s, error %lu.\n",
                debugstr_w(temp_path, sizeof(WCHAR)), GetLastError());
    vkd3d_free(temp_path);
    return hr;
}

static HRESULT vkd3d_cache_write_quota_state(struct vkd3d_shader_cache *cache,
        const struct vkd3d_cache_quota_state *state, bool dirty)
{
    uint8_t data[VKD3D_CACHE_QUOTA_STATE_SIZE];

    memset(data, 0, sizeof(data));
    memcpy(data, vkd3d_cache_quota_magic, sizeof(vkd3d_cache_quota_magic));
    vkd3d_cache_write_u32(&data[8], VKD3D_CACHE_FORMAT_VERSION);
    vkd3d_cache_write_u32(&data[12], VKD3D_CACHE_QUOTA_STATE_SIZE);
    vkd3d_cache_write_u32(&data[16], dirty ? VKD3D_CACHE_QUOTA_DIRTY : 0);
    vkd3d_cache_write_u32(&data[20], state->entry_count);
    vkd3d_cache_write_u64(&data[24], state->value_size);
    vkd3d_cache_write_u64(&data[32], state->physical_size);
    memcpy(&data[40], cache->disk.namespace_hash, VKD3D_CACHE_SHA256_SIZE);
    vkd3d_cache_sha256(data, VKD3D_CACHE_QUOTA_HASH_OFFSET,
            &data[VKD3D_CACHE_QUOTA_HASH_OFFSET]);

    return vkd3d_cache_write_namespace_metadata(&cache->disk,
            cache->disk.namespace_hash, cache->disk.quota_path, data, sizeof(data));
}

static HRESULT vkd3d_cache_read_quota_state(struct vkd3d_shader_cache *cache,
        struct vkd3d_cache_quota_state *state, bool *dirty)
{
    BY_HANDLE_FILE_INFORMATION info;
    uint8_t data[VKD3D_CACHE_QUOTA_STATE_SIZE];
    uint8_t hash[VKD3D_CACHE_SHA256_SIZE];
    HANDLE file;
    HRESULT hr;
    uint32_t flags;

    file = CreateFileW(cache->disk.quota_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return vkd3d_cache_last_error();
    if (!GetFileInformationByHandle(file, &info))
    {
        hr = vkd3d_cache_last_error();
        goto done;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            || info.nFileSizeHigh || info.nFileSizeLow != VKD3D_CACHE_QUOTA_STATE_SIZE)
    {
        hr = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
        goto done;
    }
    if (FAILED(hr = vkd3d_cache_file_read(file, data, sizeof(data))))
        goto done;
    vkd3d_cache_sha256(data, VKD3D_CACHE_QUOTA_HASH_OFFSET, hash);
    flags = vkd3d_cache_read_u32(&data[16]);
    if (memcmp(data, vkd3d_cache_quota_magic, sizeof(vkd3d_cache_quota_magic))
            || vkd3d_cache_read_u32(&data[8]) != VKD3D_CACHE_FORMAT_VERSION
            || vkd3d_cache_read_u32(&data[12]) != VKD3D_CACHE_QUOTA_STATE_SIZE
            || (flags & ~VKD3D_CACHE_QUOTA_DIRTY)
            || memcmp(&data[40], cache->disk.namespace_hash, VKD3D_CACHE_SHA256_SIZE)
            || memcmp(&data[VKD3D_CACHE_QUOTA_HASH_OFFSET], hash, sizeof(hash)))
    {
        hr = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
        goto done;
    }
    state->entry_count = vkd3d_cache_read_u32(&data[20]);
    state->value_size = vkd3d_cache_read_u64(&data[24]);
    state->physical_size = vkd3d_cache_read_u64(&data[32]);
    if (state->entry_count > cache->disk.maximum_entry_count
            || state->value_size > cache->disk.maximum_size
            || state->physical_size > cache->disk.maximum_physical_size)
    {
        hr = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
        goto done;
    }
    *dirty = !!(flags & VKD3D_CACHE_QUOTA_DIRTY);
    hr = S_OK;

done:
    CloseHandle(file);
    return hr;
}

static HRESULT vkd3d_cache_ensure_quota_state(struct vkd3d_shader_cache *cache,
        struct vkd3d_cache_quota_state *state);

static HRESULT vkd3d_cache_disk_scan(struct vkd3d_shader_cache *cache,
        struct vkd3d_cache_disk_candidate **candidates, size_t *candidate_count,
        uint64_t *total_value_size, uint64_t *total_physical_size)
{
    struct vkd3d_cache_disk_candidate *array = NULL;
    uint8_t header[VKD3D_CACHE_HEADER_SIZE];
    WCHAR expected_name[VKD3D_CACHE_HEX_SIZE + 1];
    WIN32_FIND_DATAW data;
    WCHAR *search_path = NULL, *path = NULL;
    size_t count = 0, capacity = 0, slot;
    uint32_t object_count = 0;
    uint64_t key_size = 0, value_size = 0, file_size;
    HANDLE search, file;
    HRESULT hr = S_OK;
    DWORD error;

    *total_value_size = 0;
    *total_physical_size = 0;
    if (FAILED(hr = vkd3d_cache_path_join(cache->disk.entries_path, L"*", &search_path)))
        return hr;
    search = FindFirstFileW(search_path, &data);
    vkd3d_free(search_path);
    if (search == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    }

    do
    {
        if (++object_count > VKD3D_CACHE_MAXIMUM_DISK_ENTRIES + 2u)
        {
            hr = DXGI_ERROR_CACHE_FULL;
            break;
        }
        if (!vkd3d_cache_is_hex_name(data.cFileName))
            continue;
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        {
            hr = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
            break;
        }
        if (FAILED(hr = vkd3d_cache_path_join(cache->disk.entries_path, data.cFileName, &path)))
            break;
        file = INVALID_HANDLE_VALUE;
        if (FAILED(hr = vkd3d_cache_read_entry_header(cache, path, header,
                &key_size, &value_size, NULL, &file)))
        {
            if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                hr = S_OK;
            else if (vkd3d_cache_is_corrupt_hresult(hr))
            {
                WARN("Invalid shader cache entry %s, hr %#lx; removing it.\n",
                        debugstr_w(path, sizeof(WCHAR)), hr);
                if (!DeleteFileW(path) && GetLastError() != ERROR_FILE_NOT_FOUND)
                    hr = vkd3d_cache_last_error();
                else
                    hr = S_OK;
            }
            vkd3d_free(path);
            path = NULL;
            if (FAILED(hr))
                break;
            continue;
        }
        CloseHandle(file);
        vkd3d_cache_hash_to_hex(&header[VKD3D_CACHE_HEADER_KEY_HASH_OFFSET], expected_name);
        if (lstrcmpW(data.cFileName, expected_name))
        {
            WARN("Shader cache filename does not match its key digest; removing %s.\n",
                    debugstr_w(path, sizeof(WCHAR)));
            if (!DeleteFileW(path) && GetLastError() != ERROR_FILE_NOT_FOUND)
            {
                hr = vkd3d_cache_last_error();
                break;
            }
            vkd3d_free(path);
            path = NULL;
            continue;
        }
        file_size = VKD3D_CACHE_HEADER_SIZE + key_size + value_size;

        if (count >= cache->disk.maximum_entry_count)
        {
            WARN("Shader cache entry hard limit reached; removing excess entry %s.\n",
                    debugstr_w(path, sizeof(WCHAR)));
            if (!DeleteFileW(path))
            {
                hr = vkd3d_cache_last_error();
                break;
            }
            vkd3d_free(path);
            path = NULL;
            continue;
        }
        if (!vkd3d_array_reserve((void **)&array, &capacity, count + 1, sizeof(*array)))
        {
            hr = E_OUTOFMEMORY;
            break;
        }
        slot = count++;
        if (*total_value_size > UINT64_MAX - value_size
                || *total_physical_size > UINT64_MAX - file_size)
        {
            hr = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            break;
        }
        memcpy(array[slot].hash, &header[VKD3D_CACHE_HEADER_KEY_HASH_OFFSET],
                sizeof(array[slot].hash));
        array[slot].last_write_time = data.ftLastWriteTime;
        array[slot].value_size = value_size;
        array[slot].file_size = file_size;
        *total_value_size += value_size;
        *total_physical_size += file_size;
        vkd3d_free(path);
        path = NULL;
    }
    while (FindNextFileW(search, &data));

    error = GetLastError();
    if (SUCCEEDED(hr) && error != ERROR_NO_MORE_FILES)
        hr = HRESULT_FROM_WIN32(error);
    FindClose(search);
    vkd3d_free(path);
    if (FAILED(hr))
    {
        vkd3d_free(array);
        return hr;
    }
    *candidates = array;
    *candidate_count = count;
    return S_OK;
}

static int vkd3d_cache_compare_candidates(const void *a, const void *b)
{
    const struct vkd3d_cache_disk_candidate *candidate_a = a;
    const struct vkd3d_cache_disk_candidate *candidate_b = b;

    return CompareFileTime(&candidate_a->last_write_time, &candidate_b->last_write_time);
}

static HRESULT vkd3d_cache_ensure_quota_state(struct vkd3d_shader_cache *cache,
        struct vkd3d_cache_quota_state *state)
{
    struct vkd3d_cache_disk_candidate *candidates = NULL;
    size_t candidate_count = 0;
    bool dirty;
    HRESULT hr;

    hr = vkd3d_cache_read_quota_state(cache, state, &dirty);
    if (SUCCEEDED(hr) && !dirty)
        return S_OK;
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            && !vkd3d_cache_is_corrupt_hresult(hr))
        return hr;

    if (FAILED(hr))
        WARN("Recovering invalid shader cache quota state, hr %#lx.\n", hr);
    else
        TRACE("Recovering shader cache quota state after interrupted mutation.\n");
    if (FAILED(hr = vkd3d_cache_disk_scan(cache, &candidates, &candidate_count,
            &state->value_size, &state->physical_size)))
        goto done;
    state->entry_count = candidate_count;
    hr = vkd3d_cache_write_quota_state(cache, state, false);

done:
    vkd3d_free(candidates);
    return hr;
}

static HRESULT vkd3d_cache_disk_make_space(struct vkd3d_shader_cache *cache,
        uint64_t key_size, uint64_t value_size, struct vkd3d_cache_quota_state *state)
{
    struct vkd3d_cache_disk_candidate *candidates = NULL;
    size_t candidate_count = 0, eviction_index = 0;
    uint64_t total_value_size, total_physical_size, file_size;
    WCHAR *path = NULL;
    HRESULT hr;

    file_size = VKD3D_CACHE_HEADER_SIZE + key_size + value_size;
    if (value_size > cache->disk.maximum_size || file_size > cache->disk.maximum_physical_size)
        return DXGI_ERROR_CACHE_FULL;
    if (FAILED(hr = vkd3d_cache_ensure_quota_state(cache, state)))
        return hr;
    if (state->entry_count < cache->disk.maximum_entry_count
            && state->value_size <= cache->disk.maximum_size - value_size
            && state->physical_size <= cache->disk.maximum_physical_size - file_size)
        return S_OK;

    if (FAILED(hr = vkd3d_cache_disk_scan(cache, &candidates, &candidate_count,
            &total_value_size, &total_physical_size)))
        return hr;
    if (candidate_count > 1)
        qsort(candidates, candidate_count, sizeof(*candidates), vkd3d_cache_compare_candidates);

    while (candidate_count && (candidate_count >= cache->disk.maximum_entry_count
            || total_value_size > cache->disk.maximum_size - value_size
            || total_physical_size > cache->disk.maximum_physical_size - file_size))
    {
        if (FAILED(hr = vkd3d_cache_disk_entry_path(&cache->disk,
                candidates[eviction_index].hash, &path)))
            break;
        if (!DeleteFileW(path))
        {
            hr = vkd3d_cache_last_error();
            break;
        }
        TRACE("Evicted disk shader cache entry %s.\n", debugstr_w(path, sizeof(WCHAR)));
        total_value_size -= candidates[eviction_index].value_size;
        total_physical_size -= candidates[eviction_index].file_size;
        --candidate_count;
        ++eviction_index;
        vkd3d_free(path);
        path = NULL;
    }
    if (SUCCEEDED(hr) && (candidate_count >= cache->disk.maximum_entry_count
            || total_value_size > cache->disk.maximum_size - value_size
            || total_physical_size > cache->disk.maximum_physical_size - file_size))
        hr = DXGI_ERROR_CACHE_FULL;
    if (SUCCEEDED(hr))
    {
        state->entry_count = candidate_count;
        state->value_size = total_value_size;
        state->physical_size = total_physical_size;
        hr = vkd3d_cache_write_quota_state(cache, state, false);
    }
    vkd3d_free(path);
    vkd3d_free(candidates);
    return hr;
}

static void vkd3d_cache_make_temp_name(const uint8_t key_hash[VKD3D_CACHE_SHA256_SIZE],
        uint32_t attempt, WCHAR name[VKD3D_CACHE_HEX_SIZE + 1])
{
    static LONG counter;
    struct vkd3d_sha256_context ctx;
    LARGE_INTEGER performance_counter;
    uint8_t hash[VKD3D_CACHE_SHA256_SIZE];
    DWORD process_id = GetCurrentProcessId();
    DWORD thread_id = GetCurrentThreadId();
    LONG serial = InterlockedIncrement(&counter);

    QueryPerformanceCounter(&performance_counter);
    vkd3d_cache_sha256_init(&ctx);
    vkd3d_cache_sha256_update(&ctx, key_hash, VKD3D_CACHE_SHA256_SIZE);
    vkd3d_cache_sha256_update(&ctx, &process_id, sizeof(process_id));
    vkd3d_cache_sha256_update(&ctx, &thread_id, sizeof(thread_id));
    vkd3d_cache_sha256_update(&ctx, &performance_counter, sizeof(performance_counter));
    vkd3d_cache_sha256_update(&ctx, &serial, sizeof(serial));
    vkd3d_cache_sha256_update(&ctx, &attempt, sizeof(attempt));
    vkd3d_cache_sha256_final(&ctx, hash);
    vkd3d_cache_hash_to_hex(hash, name);
}

static HRESULT vkd3d_cache_disk_write_entry(struct vkd3d_shader_cache *cache,
        const uint8_t key_hash[VKD3D_CACHE_SHA256_SIZE], const void *key, size_t key_size,
        const void *value, size_t value_size,
        const WCHAR temp_name[VKD3D_CACHE_HEX_SIZE + 1])
{
    uint8_t header[VKD3D_CACHE_HEADER_SIZE];
    WCHAR *temp_path = NULL, *entry_path = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    HRESULT hr = S_OK;
    DWORD error;

    vkd3d_cache_build_header(cache, key_hash, key, key_size, value, value_size, header);
    if (FAILED(hr = vkd3d_cache_disk_entry_path(&cache->disk, key_hash, &entry_path)))
        goto done;

    if (FAILED(hr = vkd3d_cache_path_join(cache->disk.temp_path, temp_name, &temp_path)))
        goto done;
    file = CreateFileW(temp_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT
            | FILE_FLAG_WRITE_THROUGH, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        hr = vkd3d_cache_last_error();
        goto done;
    }

    if (FAILED(hr = vkd3d_cache_file_write(file, header, sizeof(header)))
            || FAILED(hr = vkd3d_cache_file_write(file, key, key_size))
            || FAILED(hr = vkd3d_cache_file_write(file, value, value_size)))
        goto done;
    if (!FlushFileBuffers(file))
    {
        hr = vkd3d_cache_last_error();
        goto done;
    }
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;

    if (!MoveFileExW(temp_path, entry_path, MOVEFILE_WRITE_THROUGH))
    {
        error = GetLastError();
        hr = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
                ? DXGI_ERROR_ALREADY_EXISTS : HRESULT_FROM_WIN32(error);
    }

done:
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    if (FAILED(hr) && temp_path && !DeleteFileW(temp_path) && GetLastError() != ERROR_FILE_NOT_FOUND)
        WARN("Failed to remove shader cache temporary file %s, error %lu.\n",
                debugstr_w(temp_path, sizeof(WCHAR)), GetLastError());
    vkd3d_free(entry_path);
    vkd3d_free(temp_path);
    return hr;
}

static HRESULT vkd3d_cache_disk_load_entry(struct vkd3d_shader_cache *cache,
        const uint8_t key_hash[VKD3D_CACHE_SHA256_SIZE], const void *key, size_t key_size,
        void *value, size_t *value_size, uint8_t **cache_payload, bool storing)
{
    struct vkd3d_sha256_context ctx;
    uint8_t header[VKD3D_CACHE_HEADER_SIZE];
    uint8_t payload_hash[VKD3D_CACHE_SHA256_SIZE];
    uint8_t *payload = NULL, *buffer = NULL;
    WCHAR *path = NULL;
    uint64_t stored_key_size = 0, stored_value_size = 0, payload_size, offset, remaining;
    size_t count;
    size_t value_size_in = *value_size;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool key_matches;
    HRESULT hr;

    *cache_payload = NULL;
    if (FAILED(hr = vkd3d_cache_disk_entry_path(&cache->disk, key_hash, &path)))
        goto done;
    if (FAILED(hr = vkd3d_cache_read_entry_header(cache, path, header,
            &stored_key_size, &stored_value_size, NULL, &file)))
        goto done;
    if (memcmp(key_hash, &header[VKD3D_CACHE_HEADER_KEY_HASH_OFFSET], VKD3D_CACHE_SHA256_SIZE))
    {
        hr = HRESULT_FROM_WIN32(ERROR_CRC);
        goto done;
    }
    payload_size = stored_key_size + stored_value_size;
    if (payload_size > SIZE_MAX || stored_value_size > SIZE_MAX)
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!(buffer = vkd3d_malloc(VKD3D_CACHE_IO_BUFFER_SIZE)))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!storing && cache->maximum_entry_count && payload_size <= cache->maximum_memory_size
            && !(payload = vkd3d_malloc(payload_size)))
        WARN("Could not allocate bounded shader cache memory-tier payload.\n");

    vkd3d_cache_sha256_init(&ctx);
    key_matches = stored_key_size == key_size;
    remaining = stored_key_size;
    offset = 0;
    while (remaining)
    {
        count = min(remaining, VKD3D_CACHE_IO_BUFFER_SIZE);
        if (FAILED(hr = vkd3d_cache_file_read(file, buffer, count)))
            goto done;
        vkd3d_cache_sha256_update(&ctx, buffer, count);
        if (key_matches && memcmp(buffer, (const uint8_t *)key + (size_t)offset, count))
            key_matches = false;
        if (payload)
            memcpy(payload + (size_t)offset, buffer, count);
        remaining -= count;
        offset += count;
    }
    /* A size-only lookup validates the header and key but deliberately avoids
     * reading a potentially large value. Full retrieval and Store duplicate
     * detection both verify the complete payload digest. */
    if (!storing && !value && !payload && key_matches)
    {
        *value_size = stored_value_size;
        hr = S_OK;
        goto done;
    }
    remaining = stored_value_size;
    offset = 0;
    while (remaining)
    {
        count = min(remaining, VKD3D_CACHE_IO_BUFFER_SIZE);
        if (FAILED(hr = vkd3d_cache_file_read(file, buffer, count)))
            goto done;
        vkd3d_cache_sha256_update(&ctx, buffer, count);
        if (payload)
            memcpy(payload + (size_t)stored_key_size + (size_t)offset, buffer, count);
        remaining -= count;
        offset += count;
    }
    vkd3d_cache_sha256_final(&ctx, payload_hash);
    if (memcmp(payload_hash, &header[VKD3D_CACHE_HEADER_PAYLOAD_HASH_OFFSET], sizeof(payload_hash)))
    {
        hr = HRESULT_FROM_WIN32(ERROR_CRC);
        goto done;
    }
    if (!key_matches)
    {
        hr = DXGI_ERROR_CACHE_HASH_COLLISION;
        goto done;
    }
    if (storing)
    {
        hr = DXGI_ERROR_ALREADY_EXISTS;
        goto done;
    }
    *value_size = stored_value_size;
    if (value && value_size_in < stored_value_size)
    {
        hr = DXGI_ERROR_MORE_DATA;
    }
    else
    {
        if (value && payload)
            memcpy(value, payload + stored_key_size, stored_value_size);
        else if (value)
        {
            LARGE_INTEGER position;

            position.QuadPart = VKD3D_CACHE_HEADER_SIZE + stored_key_size;
            if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN))
            {
                hr = vkd3d_cache_last_error();
                goto done;
            }
            if (FAILED(hr = vkd3d_cache_file_read(file, value, stored_value_size)))
                goto done;
        }
        hr = S_OK;
    }

    *cache_payload = payload;
    payload = NULL;

done:
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    vkd3d_free(buffer);
    vkd3d_free(payload);
    vkd3d_free(path);
    return hr;
}

static HRESULT vkd3d_cache_disk_find_existing(struct vkd3d_shader_cache *cache,
        const uint8_t key_hash[VKD3D_CACHE_SHA256_SIZE], const void *key, size_t key_size)
{
    uint8_t *payload;
    size_t value_size = 0;
    HRESULT hr;

    hr = vkd3d_cache_disk_load_entry(cache, key_hash, key, key_size,
            NULL, &value_size, &payload, true);
    vkd3d_free(payload);
    return hr;
}

static HRESULT vkd3d_cache_disk_normalize_lookup_failure(struct vkd3d_shader_cache *cache,
        const uint8_t key_hash[VKD3D_CACHE_SHA256_SIZE], HRESULT hr, HRESULT missing_result)
{
    WCHAR *path;
    HRESULT delete_hr;

    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        return missing_result;
    if (!vkd3d_cache_is_corrupt_hresult(hr))
        return hr;

    if (FAILED(delete_hr = vkd3d_cache_disk_entry_path(&cache->disk, key_hash, &path)))
        return delete_hr;
    WARN("Quarantining corrupt shader cache entry %s, hr %#lx.\n",
            debugstr_w(path, sizeof(WCHAR)), hr);
    if (!DeleteFileW(path) && GetLastError() != ERROR_FILE_NOT_FOUND)
        delete_hr = vkd3d_cache_last_error();
    else
    {
        struct vkd3d_cache_quota_state state = {0};
        bool dirty;

        delete_hr = vkd3d_cache_read_quota_state(cache, &state, &dirty);
        if (vkd3d_cache_is_corrupt_hresult(delete_hr)
                || delete_hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            delete_hr = S_OK;
        if (SUCCEEDED(delete_hr))
            delete_hr = vkd3d_cache_write_quota_state(cache, &state, true);
        if (SUCCEEDED(delete_hr))
            delete_hr = missing_result;
    }
    vkd3d_free(path);
    return delete_hr;
}

static void vkd3d_cache_build_manifest(const struct vkd3d_shader_cache *cache,
        uint8_t manifest[VKD3D_CACHE_MANIFEST_SIZE])
{
    static const char package_version[] = PACKAGE_VERSION;

    memset(manifest, 0, VKD3D_CACHE_MANIFEST_SIZE);
    memcpy(manifest, vkd3d_cache_manifest_magic, sizeof(vkd3d_cache_manifest_magic));
    vkd3d_cache_write_u32(&manifest[8], VKD3D_CACHE_FORMAT_VERSION);
    vkd3d_cache_write_u32(&manifest[12], VKD3D_CACHE_MANIFEST_SIZE);
    vkd3d_cache_write_u32(&manifest[16], VKD3D_CACHE_ABI_VERSION);
    vkd3d_cache_write_u64(&manifest[24], cache->version);
    vkd3d_cache_sha256(package_version, sizeof(package_version), &manifest[32]);
    memcpy(&manifest[64], cache->disk.namespace_hash, VKD3D_CACHE_SHA256_SIZE);
    vkd3d_cache_sha256(manifest, VKD3D_CACHE_MANIFEST_HASH_OFFSET,
            &manifest[VKD3D_CACHE_MANIFEST_HASH_OFFSET]);
}

static HRESULT vkd3d_cache_read_manifest(struct vkd3d_shader_cache *cache, bool *matches)
{
    static const char package_version[] = PACKAGE_VERSION;
    BY_HANDLE_FILE_INFORMATION info;
    uint8_t manifest[VKD3D_CACHE_MANIFEST_SIZE];
    uint8_t hash[VKD3D_CACHE_SHA256_SIZE], package_hash[VKD3D_CACHE_SHA256_SIZE];
    HANDLE file;
    HRESULT hr;

    *matches = false;
    file = CreateFileW(cache->disk.manifest_path, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return vkd3d_cache_last_error();
    if (!GetFileInformationByHandle(file, &info))
    {
        hr = vkd3d_cache_last_error();
        goto done;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            || info.nFileSizeHigh || info.nFileSizeLow != VKD3D_CACHE_MANIFEST_SIZE)
    {
        hr = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
        goto done;
    }
    if (FAILED(hr = vkd3d_cache_file_read(file, manifest, sizeof(manifest))))
        goto done;
    vkd3d_cache_sha256(manifest, VKD3D_CACHE_MANIFEST_HASH_OFFSET, hash);
    if (memcmp(hash, &manifest[VKD3D_CACHE_MANIFEST_HASH_OFFSET], sizeof(hash)))
    {
        hr = HRESULT_FROM_WIN32(ERROR_CRC);
        goto done;
    }
    vkd3d_cache_sha256(package_version, sizeof(package_version), package_hash);
    if (!memcmp(manifest, vkd3d_cache_manifest_magic, sizeof(vkd3d_cache_manifest_magic))
            && vkd3d_cache_read_u32(&manifest[8]) == VKD3D_CACHE_FORMAT_VERSION
            && vkd3d_cache_read_u32(&manifest[12]) == VKD3D_CACHE_MANIFEST_SIZE
            && vkd3d_cache_read_u32(&manifest[16]) == VKD3D_CACHE_ABI_VERSION
            && !vkd3d_cache_read_u32(&manifest[20])
            && vkd3d_cache_read_u64(&manifest[24]) == cache->version
            && !memcmp(&manifest[32], package_hash, sizeof(package_hash))
            && !memcmp(&manifest[64], cache->disk.namespace_hash, VKD3D_CACHE_SHA256_SIZE))
        *matches = true;
    hr = S_OK;

done:
    CloseHandle(file);
    return hr;
}

static HRESULT vkd3d_cache_write_manifest(struct vkd3d_shader_cache *cache)
{
    uint8_t manifest[VKD3D_CACHE_MANIFEST_SIZE];

    vkd3d_cache_build_manifest(cache, manifest);
    return vkd3d_cache_write_namespace_metadata(&cache->disk,
            cache->disk.namespace_hash, cache->disk.manifest_path,
            manifest, sizeof(manifest));
}

static HRESULT vkd3d_cache_disk_reset(struct vkd3d_shader_cache *cache)
{
    const struct vkd3d_cache_quota_state empty_state = {0};
    HRESULT first_error, hr;

    first_error = vkd3d_cache_delete_hex_files(cache->disk.entries_path,
            VKD3D_CACHE_MAXIMUM_DISK_ENTRIES + 2u);
    if (FAILED(hr = vkd3d_cache_delete_hex_files(cache->disk.temp_path,
            VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS + 2u)) && SUCCEEDED(first_error))
        first_error = hr;
    if (FAILED(hr = vkd3d_cache_write_quota_state(cache, &empty_state, false))
            && SUCCEEDED(first_error))
        first_error = hr;
    return first_error;
}

static HRESULT vkd3d_cache_prepare_manifest(struct vkd3d_shader_cache *cache, bool exclusive_lifecycle)
{
    bool matches, marker_exists;
    HRESULT hr;

    if (exclusive_lifecycle)
    {
        if (FAILED(hr = vkd3d_cache_regular_file_exists(cache->disk.marker_path, &marker_exists)))
            return hr;
        if (marker_exists)
        {
            if (FAILED(hr = vkd3d_cache_disk_reset(cache)))
                return hr;
            if (!DeleteFileW(cache->disk.marker_path) && GetLastError() != ERROR_FILE_NOT_FOUND)
                return vkd3d_cache_last_error();
        }
    }

    hr = vkd3d_cache_read_manifest(cache, &matches);
    if (SUCCEEDED(hr) && matches)
        return S_OK;
    if (!exclusive_lifecycle)
    {
        if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            return DXGI_ERROR_ALREADY_EXISTS;
        return hr;
    }

    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            && hr != HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT)
            && hr != HRESULT_FROM_WIN32(ERROR_CRC))
        return hr;
    if (FAILED(hr))
        WARN("Replacing invalid shader cache manifest, hr %#lx.\n", hr);
    else
        TRACE("Invalidating shader cache for a version or runtime change.\n");
    if (FAILED(hr = vkd3d_cache_disk_reset(cache)))
        return hr;
    return vkd3d_cache_write_manifest(cache);
}

static HRESULT vkd3d_cache_disk_init(struct vkd3d_shader_cache *cache,
        const struct vkd3d_shader_cache_desc *desc)
{
    struct vkd3d_shader_cache_disk *disk = &cache->disk;
    struct vkd3d_cache_root_state root_state;
    struct vkd3d_cache_quota_state quota_state;
    WCHAR namespace_name[VKD3D_CACHE_HEX_SIZE + 1];
    FILETIME now;
    int namespace_index;
    HRESULT hr, lock_hr;
    uint32_t root_object_count;
    bool exclusive_lifecycle = false, inventory_matches, root_locked = false;

    disk->cache_root_handle = INVALID_HANDLE_VALUE;
    disk->root_lock_handle = INVALID_HANDLE_VALUE;
    disk->root_handle = INVALID_HANDLE_VALUE;
    disk->entries_handle = INVALID_HANDLE_VALUE;
    disk->temp_handle = INVALID_HANDLE_VALUE;
    disk->lock_handle = INVALID_HANDLE_VALUE;
    disk->maximum_size = VKD3D_CACHE_ROOT_MAXIMUM_PHYSICAL_SIZE;
    disk->maximum_entry_count = VKD3D_CACHE_MAXIMUM_DISK_ENTRIES;
    disk->maximum_physical_size = VKD3D_CACHE_ROOT_MAXIMUM_PHYSICAL_SIZE;
    if (FAILED(hr = vkd3d_cache_compute_namespace_hash(desc, disk->namespace_hash)))
        goto error;
    vkd3d_cache_hash_to_hex(disk->namespace_hash, namespace_name);

    if (desc->flags & D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR)
        hr = vkd3d_cache_get_working_root(&disk->cache_root_path);
    else
        hr = vkd3d_cache_get_application_root(&disk->cache_root_path);
    if (FAILED(hr))
        goto error;
    if (FAILED(hr = vkd3d_cache_open_directory(disk->cache_root_path,
            &disk->cache_root_handle)))
        goto error;
    if (FAILED(hr = vkd3d_cache_path_join(disk->cache_root_path,
            vkd3d_cache_root_lock_name, &disk->root_lock_path))
            || FAILED(hr = vkd3d_cache_path_join(disk->cache_root_path,
                    vkd3d_cache_root_state_name, &disk->root_state_path))
            || FAILED(hr = vkd3d_cache_path_join(disk->cache_root_path,
                    vkd3d_cache_root_state_temp_name, &disk->root_state_temp_path))
            || FAILED(hr = vkd3d_cache_open_lock_file(disk->root_lock_path,
                    OPEN_ALWAYS, &disk->root_lock_handle)))
        goto error;
    if (FAILED(hr = vkd3d_cache_root_lock(disk)))
        goto error;
    root_locked = true;
    if (FAILED(hr = vkd3d_cache_ensure_root_state(disk, &root_state)))
        goto error;
    if (vkd3d_cache_root_reservation_count(&root_state))
    {
        hr = vkd3d_cache_root_rebuild_if_idle(disk, &root_state);
        if (hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
            hr = S_OK;
        if (FAILED(hr))
            goto error;
    }
    if (FAILED(hr = vkd3d_cache_root_check_inventory(disk, &root_state,
            &inventory_matches, &root_object_count)))
        goto error;
    if (!inventory_matches)
    {
        hr = vkd3d_cache_root_rebuild_if_idle(disk, &root_state);
        if (hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
            hr = DXGI_ERROR_CACHE_FULL;
        if (FAILED(hr))
            goto error;
        if (FAILED(hr = vkd3d_cache_root_check_inventory(disk, &root_state,
                &inventory_matches, &root_object_count)))
            goto error;
        if (!inventory_matches)
        {
            hr = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
            goto error;
        }
    }
    /* Every successful open updates root state, so keep one object slot for
     * its crash-recovery temporary file. */
    if (root_object_count >= VKD3D_CACHE_ROOT_MAXIMUM_SCAN_OBJECTS)
    {
        hr = DXGI_ERROR_CACHE_FULL;
        goto error;
    }
    if ((namespace_index = vkd3d_cache_root_find_namespace(&root_state,
            disk->namespace_hash)) < 0)
    {
        /* Keep room for the namespace directory and the root-state temporary
         * file, including after an interrupted state replacement. */
        if (root_object_count > VKD3D_CACHE_ROOT_MAXIMUM_SCAN_OBJECTS - 2u)
        {
            hr = DXGI_ERROR_CACHE_FULL;
            goto error;
        }
        if (FAILED(hr = vkd3d_cache_root_make_space(disk, &root_state, 0, 0, true)))
            goto error;
        namespace_index = root_state.namespace_count++;
        memcpy(root_state.namespaces[namespace_index].hash, disk->namespace_hash,
                VKD3D_CACHE_SHA256_SIZE);
    }
    GetSystemTimeAsFileTime(&now);
    root_state.namespaces[namespace_index].last_used = vkd3d_cache_filetime_to_u64(&now);

    if (FAILED(hr = vkd3d_cache_path_join(disk->cache_root_path,
            namespace_name, &disk->root_path)))
        goto error;
    if (FAILED(hr = vkd3d_cache_ensure_directory(disk->root_path, &disk->root_handle)))
        goto error;

    if (FAILED(hr = vkd3d_cache_path_join(disk->root_path,
            vkd3d_cache_entries_name, &disk->entries_path)))
        goto error;
    if (FAILED(hr = vkd3d_cache_ensure_directory(disk->entries_path, &disk->entries_handle)))
        goto error;
    if (FAILED(hr = vkd3d_cache_path_join(disk->root_path,
            vkd3d_cache_temp_name, &disk->temp_path)))
        goto error;
    if (FAILED(hr = vkd3d_cache_ensure_directory(disk->temp_path, &disk->temp_handle)))
        goto error;
    if (FAILED(hr = vkd3d_cache_path_join(disk->root_path,
            vkd3d_cache_lock_name, &disk->lock_path)))
        goto error;
    if (FAILED(hr = vkd3d_cache_path_join(disk->root_path,
            vkd3d_cache_marker_name, &disk->marker_path)))
        goto error;
    if (FAILED(hr = vkd3d_cache_path_join(disk->root_path,
            vkd3d_cache_manifest_name, &disk->manifest_path)))
        goto error;
    if (FAILED(hr = vkd3d_cache_path_join(disk->root_path,
            vkd3d_cache_quota_name, &disk->quota_path)))
        goto error;
    if (FAILED(hr = vkd3d_cache_open_lock_file(disk->lock_path, OPEN_ALWAYS, &disk->lock_handle)))
        goto error;

    lock_hr = vkd3d_cache_lock_range(disk->lock_handle,
            VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET, true, true);
    if (SUCCEEDED(lock_hr))
        exclusive_lifecycle = true;
    else if (lock_hr != HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
    {
        hr = lock_hr;
        goto error;
    }
    else if (FAILED(hr = vkd3d_cache_lock_range(disk->lock_handle,
            VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET, false, false)))
        goto error;
    else
        disk->lifecycle_locked = true;

    if (FAILED(hr = vkd3d_cache_disk_lock(disk)))
        goto error;
    hr = vkd3d_cache_prepare_manifest(cache, exclusive_lifecycle);
    if (SUCCEEDED(hr))
        hr = vkd3d_cache_delete_hex_files(disk->temp_path,
                VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS + 2u);
    if (SUCCEEDED(hr))
        hr = vkd3d_cache_ensure_quota_state(cache, &quota_state);
    if (SUCCEEDED(hr))
        hr = vkd3d_cache_root_set_namespace_physical(&root_state,
                disk->namespace_hash, quota_state.physical_size, quota_state.entry_count);
    hr = vkd3d_cache_disk_unlock(disk, hr);
    if (FAILED(hr))
        goto error;

    if (exclusive_lifecycle)
    {
        if (FAILED(hr = vkd3d_cache_unlock_range(disk->lock_handle,
                VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET)))
            goto error;
        exclusive_lifecycle = false;
        if (FAILED(hr = vkd3d_cache_lock_range(disk->lock_handle,
                VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET, false, false)))
            goto error;
        disk->lifecycle_locked = true;
        if (FAILED(hr = vkd3d_cache_disk_lock(disk)))
            goto error;
        hr = vkd3d_cache_prepare_manifest(cache, false);
        hr = vkd3d_cache_disk_unlock(disk, hr);
        if (FAILED(hr))
            goto error;
    }
    if (FAILED(hr = vkd3d_cache_root_make_space(disk, &root_state, 0, 0, false)))
        goto error;
    if (FAILED(hr = vkd3d_cache_write_root_state(disk, &root_state)))
        goto error;
    hr = vkd3d_cache_root_unlock(disk, S_OK);
    root_locked = false;
    if (FAILED(hr))
        goto error;
    return S_OK;

error:
    if (exclusive_lifecycle)
    {
        HRESULT unlock_hr = vkd3d_cache_unlock_range(disk->lock_handle,
                VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET);
        if (FAILED(unlock_hr))
            ERR("Failed to release exclusive shader cache lifecycle lock, hr %#lx.\n", unlock_hr);
    }
    if (disk->lifecycle_locked)
    {
        HRESULT unlock_hr = vkd3d_cache_unlock_range(disk->lock_handle,
                VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET);
        if (FAILED(unlock_hr))
            ERR("Failed to release shader cache lifecycle lock, hr %#lx.\n", unlock_hr);
        disk->lifecycle_locked = false;
    }
    if (root_locked)
        hr = vkd3d_cache_root_unlock(disk, hr);
    vkd3d_cache_disk_cleanup(disk);
    return hr;
}

static int vkd3d_shader_cache_compare_memory_key(const void *key, const struct rb_entry *entry)
{
    const struct shader_cache_entry *e = RB_ENTRY_VALUE(entry, struct shader_cache_entry, entry);
    const struct shader_cache_key *k = key;
    uint64_t entry_prefix, key_prefix;

    memcpy(&key_prefix, k->hash, sizeof(key_prefix));
    memcpy(&entry_prefix, e->h.hash, sizeof(entry_prefix));
    return vkd3d_u64_compare(key_prefix, entry_prefix);
}

static int vkd3d_shader_cache_compare_disk_key(const void *key, const struct rb_entry *entry)
{
    const struct shader_cache_entry *e = RB_ENTRY_VALUE(entry, struct shader_cache_entry, entry);
    const struct shader_cache_key *k = key;

    return memcmp(k->hash, e->h.hash, sizeof(k->hash));
}

static void vkd3d_shader_cache_init_tree(struct vkd3d_shader_cache *cache)
{
    rb_init(&cache->tree, cache->mode == D3D12_SHADER_CACHE_MODE_MEMORY
            ? vkd3d_shader_cache_compare_memory_key : vkd3d_shader_cache_compare_disk_key);
}

static void vkd3d_shader_cache_add_entry(struct vkd3d_shader_cache *cache,
        struct shader_cache_entry *entry)
{
    struct shader_cache_key key;

    memcpy(key.hash, entry->h.hash, cache->mode == D3D12_SHADER_CACHE_MODE_DISK
            ? sizeof(key.hash) : sizeof(uint64_t));
    rb_put(&cache->tree, &key, &entry->entry);
    list_add_tail(&cache->lru_list, &entry->lru_entry);
}

static void vkd3d_shader_cache_destroy_entry(struct rb_entry *entry, void *context)
{
    struct shader_cache_entry *e = RB_ENTRY_VALUE(entry, struct shader_cache_entry, entry);

    (void)context;
    vkd3d_free(e->payload);
    vkd3d_free(e);
}

static void vkd3d_shader_cache_clear_memory(struct vkd3d_shader_cache *cache)
{
    rb_destroy(&cache->tree, vkd3d_shader_cache_destroy_entry, NULL);
    vkd3d_shader_cache_init_tree(cache);
    list_init(&cache->lru_list);
    cache->memory_size = 0;
    cache->entry_count = 0;
}

static void vkd3d_shader_cache_remove_entry(struct vkd3d_shader_cache *cache,
        struct shader_cache_entry *entry)
{
    uint64_t storage_size = entry->h.key_size + entry->h.value_size;

    rb_remove(&cache->tree, &entry->entry);
    list_remove(&entry->lru_entry);
    VKD3D_ASSERT(cache->entry_count && cache->memory_size >= storage_size);
    --cache->entry_count;
    cache->memory_size -= storage_size;
    vkd3d_free(entry->payload);
    vkd3d_free(entry);
}

static HRESULT vkd3d_shader_cache_memory_lookup(struct vkd3d_shader_cache *cache,
        const uint8_t hash[VKD3D_CACHE_SHA256_SIZE], const void *key, size_t key_size,
        struct shader_cache_entry **entry)
{
    struct shader_cache_key cache_key;
    struct rb_entry *rb_entry;
    struct shader_cache_entry *e;

    memcpy(cache_key.hash, hash,
            cache->mode == D3D12_SHADER_CACHE_MODE_DISK
            ? sizeof(cache_key.hash) : sizeof(uint64_t));
    if (!(rb_entry = rb_get(&cache->tree, &cache_key)))
    {
        *entry = NULL;
        return DXGI_ERROR_NOT_FOUND;
    }
    e = RB_ENTRY_VALUE(rb_entry, struct shader_cache_entry, entry);
    if (e->h.key_size != key_size || memcmp(e->payload, key, key_size))
    {
        *entry = NULL;
        return DXGI_ERROR_CACHE_HASH_COLLISION;
    }
    *entry = e;
    return S_OK;
}

static HRESULT vkd3d_shader_cache_memory_insert(struct vkd3d_shader_cache *cache,
        const uint8_t hash[VKD3D_CACHE_SHA256_SIZE], const void *key, size_t key_size,
        const void *value, size_t value_size, bool replace_existing)
{
    struct shader_cache_entry *entry;
    uint64_t storage_size, allocation_size;
    HRESULT hr;

    if (SUCCEEDED(hr = vkd3d_shader_cache_memory_lookup(cache, hash, key, key_size, &entry)))
    {
        if (!replace_existing)
            return DXGI_ERROR_ALREADY_EXISTS;
        list_remove(&entry->lru_entry);
        list_add_tail(&cache->lru_list, &entry->lru_entry);
        return S_OK;
    }
    if (hr == DXGI_ERROR_CACHE_HASH_COLLISION)
        return hr;

    if (key_size > UINT64_MAX - value_size)
        return E_OUTOFMEMORY;
    storage_size = (uint64_t)key_size + value_size;
    if (!cache->maximum_entry_count || storage_size > cache->maximum_memory_size)
        return DXGI_ERROR_CACHE_FULL;
    allocation_size = sizeof(*entry) + storage_size;
    if (allocation_size > SIZE_MAX)
        return E_OUTOFMEMORY;

    while (cache->entry_count && (cache->entry_count >= cache->maximum_entry_count
            || cache->memory_size > cache->maximum_memory_size
            || storage_size > cache->maximum_memory_size - cache->memory_size))
    {
        entry = LIST_ENTRY(list_head(&cache->lru_list), struct shader_cache_entry, lru_entry);
        vkd3d_shader_cache_remove_entry(cache, entry);
    }

    if (!(entry = vkd3d_malloc(sizeof(*entry))))
        return E_OUTOFMEMORY;
    if (!(entry->payload = vkd3d_malloc(storage_size)))
    {
        vkd3d_free(entry);
        return E_OUTOFMEMORY;
    }

    memcpy(entry->h.hash, hash, cache->mode == D3D12_SHADER_CACHE_MODE_DISK
            ? sizeof(entry->h.hash) : sizeof(uint64_t));
    entry->h.key_size = key_size;
    entry->h.value_size = value_size;
    memcpy(entry->payload, key, key_size);
    memcpy(entry->payload + key_size, value, value_size);
    vkd3d_shader_cache_add_entry(cache, entry);
    cache->memory_size += storage_size;
    ++cache->entry_count;
    return S_OK;
}

static HRESULT vkd3d_shader_cache_memory_get(struct vkd3d_shader_cache *cache,
        const uint8_t hash[VKD3D_CACHE_SHA256_SIZE], const void *key, size_t key_size,
        void *value, size_t *value_size)
{
    struct shader_cache_entry *entry;
    size_t value_size_in = *value_size;
    HRESULT hr;

    if (FAILED(hr = vkd3d_shader_cache_memory_lookup(cache, hash, key, key_size, &entry)))
        return hr;
    list_remove(&entry->lru_entry);
    list_add_tail(&cache->lru_list, &entry->lru_entry);

    if (entry->h.value_size > SIZE_MAX)
        return E_OUTOFMEMORY;
    *value_size = entry->h.value_size;
    if (value && value_size_in < entry->h.value_size)
        return DXGI_ERROR_MORE_DATA;
    if (value)
        memcpy(value, entry->payload + entry->h.key_size, entry->h.value_size);
    return S_OK;
}

HRESULT vkd3d_shader_open_cache(struct vkd3d_shader_cache **cache,
        const struct vkd3d_shader_cache_desc *desc)
{
    static const D3D12_SHADER_CACHE_FLAGS valid_flags = D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED
            | D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR;
    struct vkd3d_shader_cache *object;
    HRESULT hr;

    TRACE("cache %p, desc %p.\n", cache, desc);

    if (!cache || !desc || (desc->mode != D3D12_SHADER_CACHE_MODE_MEMORY
            && desc->mode != D3D12_SHADER_CACHE_MODE_DISK) || (desc->flags & ~valid_flags))
        return E_INVALIDARG;
    if ((desc->flags & D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED)
            && (!desc->driver_identity || !desc->driver_identity_size))
        return E_INVALIDARG;
    if (desc->mode == D3D12_SHADER_CACHE_MODE_DISK
            && desc->maximum_value_file_size > VKD3D_CACHE_MAXIMUM_VALUE_SIZE)
        return E_INVALIDARG;
    *cache = NULL;

    if (!(object = vkd3d_calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;
    object->refcount = 1;
    object->mode = desc->mode;
    object->flags = desc->flags;
    object->version = desc->version;
    object->maximum_value_size = desc->maximum_value_file_size
            ? desc->maximum_value_file_size : VKD3D_CACHE_DEFAULT_VALUE_SIZE;
    object->maximum_memory_size = desc->maximum_memory_size
            ? desc->maximum_memory_size : VKD3D_CACHE_DEFAULT_MEMORY_SIZE;
    object->maximum_entry_count = desc->maximum_memory_entry_count
            ? desc->maximum_memory_entry_count : VKD3D_CACHE_DEFAULT_MEMORY_ENTRIES;
    object->disk.root_handle = INVALID_HANDLE_VALUE;
    object->disk.entries_handle = INVALID_HANDLE_VALUE;
    object->disk.temp_handle = INVALID_HANDLE_VALUE;
    object->disk.lock_handle = INVALID_HANDLE_VALUE;
    vkd3d_shader_cache_init_tree(object);
    list_init(&object->lru_list);
    vkd3d_mutex_init(&object->lock);

    if (object->mode == D3D12_SHADER_CACHE_MODE_DISK
            && FAILED(hr = vkd3d_cache_disk_init(object, desc)))
    {
        WARN("Failed to initialize disk shader cache, hr %#lx.\n", hr);
        vkd3d_mutex_destroy(&object->lock);
        vkd3d_free(object);
        return hr;
    }

    *cache = object;
    return S_OK;
}

unsigned int vkd3d_shader_cache_incref(struct vkd3d_shader_cache *cache)
{
    unsigned int refcount = vkd3d_atomic_increment_u32(&cache->refcount);

    TRACE("cache %p refcount %u.\n", cache, refcount);
    return refcount;
}

HRESULT vkd3d_shader_cache_clear(struct vkd3d_shader_cache *cache)
{
    struct vkd3d_cache_root_state root_state;
    HRESULT hr = S_OK, lock_hr;
    unsigned int i;

    TRACE("cache %p.\n", cache);
    if (!cache)
        return E_INVALIDARG;

    if (cache->mode == D3D12_SHADER_CACHE_MODE_MEMORY)
    {
        vkd3d_mutex_lock(&cache->lock);
        vkd3d_shader_cache_clear_memory(cache);
        vkd3d_mutex_unlock(&cache->lock);
        return S_OK;
    }

retry_operation_lock:
    if (FAILED(hr = vkd3d_cache_root_lock(&cache->disk)))
        return hr;
    if (FAILED(hr = vkd3d_cache_ensure_root_state(&cache->disk, &root_state)))
        return vkd3d_cache_root_unlock(&cache->disk, hr);
    lock_hr = vkd3d_cache_lock_range(cache->disk.lock_handle,
            VKD3D_CACHE_OPERATION_LOCK_OFFSET, true, true);
    if (lock_hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
    {
        if (FAILED(hr = vkd3d_cache_root_unlock(&cache->disk, S_OK)))
            return hr;
        if (FAILED(hr = vkd3d_cache_disk_lock(&cache->disk)))
            return hr;
        if (FAILED(hr = vkd3d_cache_disk_unlock(&cache->disk, S_OK)))
            return hr;
        goto retry_operation_lock;
    }
    if (FAILED(lock_hr))
        return vkd3d_cache_root_unlock(&cache->disk, lock_hr);

    hr = vkd3d_cache_root_discard_namespace_reservations(&cache->disk, &root_state,
            cache->disk.namespace_hash);
    if (hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
    {
        for (i = 0; i < VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS; ++i)
        {
            if (root_state.reservations[i].flags
                    && !memcmp(root_state.reservations[i].namespace_hash,
                            cache->disk.namespace_hash, VKD3D_CACHE_SHA256_SIZE))
                break;
        }
        hr = vkd3d_cache_disk_unlock(&cache->disk, S_OK);
        hr = vkd3d_cache_root_unlock(&cache->disk, hr);
        if (FAILED(hr))
            return hr;
        if (i == VKD3D_CACHE_ROOT_MAXIMUM_RESERVATIONS)
            return E_UNEXPECTED;
        if (FAILED(hr = vkd3d_cache_lock_range(cache->disk.root_lock_handle,
                VKD3D_CACHE_ROOT_RESERVATION_LOCK_OFFSET(i), true, false)))
            return hr;
        if (FAILED(hr = vkd3d_cache_unlock_range(cache->disk.root_lock_handle,
                VKD3D_CACHE_ROOT_RESERVATION_LOCK_OFFSET(i))))
            return hr;
        goto retry_operation_lock;
    }
    if (FAILED(hr))
    {
        hr = vkd3d_cache_disk_unlock(&cache->disk, hr);
        return vkd3d_cache_root_unlock(&cache->disk, hr);
    }
    if (SUCCEEDED(hr = vkd3d_cache_disk_reset(cache)))
        hr = vkd3d_cache_root_set_namespace_physical(&root_state,
                cache->disk.namespace_hash, 0, 0);
    if (SUCCEEDED(hr))
        hr = vkd3d_cache_write_root_state(&cache->disk, &root_state);
    vkd3d_mutex_lock(&cache->lock);
    vkd3d_shader_cache_clear_memory(cache);
    vkd3d_mutex_unlock(&cache->lock);
    hr = vkd3d_cache_disk_unlock(&cache->disk, hr);
    return vkd3d_cache_root_unlock(&cache->disk, hr);
}

HRESULT vkd3d_shader_cache_put(struct vkd3d_shader_cache *cache,
        const void *key, size_t key_size, const void *value, size_t value_size)
{
    struct vkd3d_cache_root_state root_state;
    struct vkd3d_cache_quota_state quota_state;
    uint8_t key_hash[VKD3D_CACHE_SHA256_SIZE], temp_hash[VKD3D_CACHE_SHA256_SIZE];
    WCHAR temp_name[VKD3D_CACHE_HEX_SIZE + 1];
    uint64_t file_size;
    uint32_t reservation_index = UINT32_MAX;
    HRESULT hr, lock_hr, memory_hr, root_hr;
    bool operation_locked = false, root_locked = false, committed = false;
    bool repair_corrupt = false;

    TRACE("cache %p, key %p, key_size %#zx, value %p, value_size %#zx.\n",
            cache, key, key_size, value, value_size);
    if (!cache || !key || !key_size || !value || !value_size)
        return E_INVALIDARG;
    if (key_size > UINT64_MAX - value_size)
        return E_OUTOFMEMORY;
    if (cache->mode == D3D12_SHADER_CACHE_MODE_MEMORY)
    {
        uint64_t memory_hash;

        if (!cache->maximum_entry_count
                || key_size + (uint64_t)value_size > cache->maximum_memory_size)
            return DXGI_ERROR_CACHE_FULL;
        memory_hash = vkd3d_cache_hash_memory_key(key, key_size);
        memcpy(key_hash, &memory_hash, sizeof(memory_hash));
        vkd3d_mutex_lock(&cache->lock);
        hr = vkd3d_shader_cache_memory_insert(cache, key_hash,
                key, key_size, value, value_size, false);
        vkd3d_mutex_unlock(&cache->lock);
        return hr;
    }
    if (value_size > cache->maximum_value_size || key_size > VKD3D_CACHE_MAXIMUM_KEY_SIZE)
        return DXGI_ERROR_CACHE_FULL;
    vkd3d_cache_sha256(key, key_size, key_hash);

    /* The first probe is read-only and deliberately root-lock-free. */
    if (FAILED(hr = vkd3d_cache_disk_lock_shared(&cache->disk)))
        return hr;
    hr = vkd3d_cache_disk_find_existing(cache, key_hash, key, key_size);
    hr = vkd3d_cache_disk_unlock(&cache->disk, hr);
    if (hr == DXGI_ERROR_ALREADY_EXISTS || hr == DXGI_ERROR_CACHE_HASH_COLLISION)
        return hr;
    if (hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            && hr != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
            && !vkd3d_cache_is_corrupt_hresult(hr))
        return hr;
    repair_corrupt = vkd3d_cache_is_corrupt_hresult(hr);

    file_size = VKD3D_CACHE_HEADER_SIZE + (uint64_t)key_size + value_size;
    vkd3d_cache_make_temp_name(key_hash, 0, temp_name);
    if (!vkd3d_cache_hex_to_hash(temp_name, temp_hash))
        return E_UNEXPECTED;

retry_operation_lock:
    if (FAILED(hr = vkd3d_cache_root_lock(&cache->disk)))
        return hr;
    root_locked = true;
    if (FAILED(hr = vkd3d_cache_ensure_root_state(&cache->disk, &root_state)))
        goto done;
    lock_hr = vkd3d_cache_lock_range(cache->disk.lock_handle,
            VKD3D_CACHE_OPERATION_LOCK_OFFSET, true, true);
    if (lock_hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
    {
        hr = vkd3d_cache_root_unlock(&cache->disk, S_OK);
        root_locked = false;
        if (FAILED(hr))
            return hr;
        if (FAILED(hr = vkd3d_cache_disk_lock(&cache->disk)))
            return hr;
        if (FAILED(hr = vkd3d_cache_disk_unlock(&cache->disk, S_OK)))
            return hr;
        goto retry_operation_lock;
    }
    if (FAILED(lock_hr))
    {
        hr = lock_hr;
        goto done;
    }
    operation_locked = true;
    if (repair_corrupt)
    {
        hr = vkd3d_cache_disk_find_existing(cache, key_hash, key, key_size);
        hr = vkd3d_cache_disk_normalize_lookup_failure(cache, key_hash, hr,
                HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        if (SUCCEEDED(lock_hr = vkd3d_cache_ensure_quota_state(cache, &quota_state))
                && SUCCEEDED(lock_hr = vkd3d_cache_root_set_namespace_physical(&root_state,
                        cache->disk.namespace_hash, quota_state.physical_size,
                        quota_state.entry_count)))
            lock_hr = vkd3d_cache_write_root_state(&cache->disk, &root_state);
        if (FAILED(lock_hr))
        {
            hr = lock_hr;
            goto done;
        }
        if (hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            goto done;
        repair_corrupt = false;
    }
    if (FAILED(hr = vkd3d_cache_root_reserve(&cache->disk, &root_state,
            cache->disk.namespace_hash, key_hash, temp_hash, file_size,
            &reservation_index)))
        goto done;
    if (FAILED(hr = vkd3d_cache_disk_make_space(cache,
            key_size, value_size, &quota_state)))
        goto done;
    if (FAILED(hr = vkd3d_cache_root_set_namespace_physical(&root_state,
            cache->disk.namespace_hash, quota_state.physical_size, quota_state.entry_count))
            || FAILED(hr = vkd3d_cache_write_root_state(&cache->disk, &root_state)))
        goto done;

    /* The durable reservation remains charged while the root lock is released
     * for the potentially large key/value write. The namespace operation lock
     * prevents a same-namespace rename or quota-state race. */
    if (FAILED(hr = vkd3d_cache_root_unlock(&cache->disk, S_OK)))
    {
        root_locked = false;
        goto done;
    }
    root_locked = false;

    hr = vkd3d_cache_disk_find_existing(cache, key_hash, key, key_size);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
    {
        /* Publish the namespace-local dirty bit before the atomic rename. */
        if (SUCCEEDED(hr = vkd3d_cache_write_quota_state(cache, &quota_state, true))
                && SUCCEEDED(hr = vkd3d_cache_disk_write_entry(cache,
                        key_hash, key, key_size, value, value_size, temp_name)))
        {
            committed = true;
            ++quota_state.entry_count;
            quota_state.value_size += value_size;
            quota_state.physical_size += file_size;
            hr = vkd3d_cache_write_quota_state(cache, &quota_state, false);
        }
    }
    else if (vkd3d_cache_is_corrupt_hresult(hr))
        WARN("A corrupt shader cache entry appeared during Store, hr %#lx.\n", hr);

done:
    if (operation_locked)
    {
        hr = vkd3d_cache_disk_unlock(&cache->disk, hr);
        operation_locked = false;
    }
    if (!root_locked && reservation_index != UINT32_MAX)
    {
        if (FAILED(lock_hr = vkd3d_cache_root_lock(&cache->disk)))
        {
            vkd3d_cache_unlock_range(cache->disk.root_lock_handle,
                    VKD3D_CACHE_ROOT_RESERVATION_LOCK_OFFSET(reservation_index));
            if (SUCCEEDED(hr))
                hr = lock_hr;
            reservation_index = UINT32_MAX;
        }
        else
            root_locked = true;
    }
    if (root_locked && reservation_index != UINT32_MAX)
    {
        root_hr = vkd3d_cache_root_finish_reservation(&cache->disk,
                reservation_index, committed);
        reservation_index = UINT32_MAX;
        if (FAILED(root_hr) && SUCCEEDED(hr))
            hr = root_hr;
    }
    if (root_locked)
    {
        hr = vkd3d_cache_root_unlock(&cache->disk, hr);
        root_locked = false;
    }
    if (SUCCEEDED(hr))
    {
        vkd3d_mutex_lock(&cache->lock);
        memory_hr = vkd3d_shader_cache_memory_insert(cache, key_hash,
                key, key_size, value, value_size, true);
        vkd3d_mutex_unlock(&cache->lock);
        if (FAILED(memory_hr) && memory_hr != DXGI_ERROR_CACHE_FULL)
            WARN("Disk cache store succeeded but memory tier insertion failed, hr %#lx.\n", memory_hr);
    }
    return hr;
}

HRESULT vkd3d_shader_cache_get(struct vkd3d_shader_cache *cache,
        const void *key, size_t key_size, void *value, size_t *value_size)
{
    struct vkd3d_cache_root_state root_state;
    struct vkd3d_cache_quota_state quota_state;
    uint8_t key_hash[VKD3D_CACHE_SHA256_SIZE], *payload = NULL;
    size_t result_size, memory_size, value_size_in;
    HRESULT hr, memory_hr;

    TRACE("cache %p, key %p, key_size %#zx, value %p, value_size %p.\n",
            cache, key, key_size, value, value_size);
    if (!cache || !key || !key_size || !value_size)
        return E_INVALIDARG;
    if (cache->mode == D3D12_SHADER_CACHE_MODE_DISK
            && key_size > VKD3D_CACHE_MAXIMUM_KEY_SIZE)
        return DXGI_ERROR_NOT_FOUND;
    if (cache->mode == D3D12_SHADER_CACHE_MODE_MEMORY
            && (!cache->maximum_entry_count || key_size > cache->maximum_memory_size))
        return DXGI_ERROR_NOT_FOUND;
    value_size_in = *value_size;
    if (cache->mode == D3D12_SHADER_CACHE_MODE_MEMORY)
    {
        uint64_t memory_hash = vkd3d_cache_hash_memory_key(key, key_size);
        memcpy(key_hash, &memory_hash, sizeof(memory_hash));
    }
    else
        vkd3d_cache_sha256(key, key_size, key_hash);

    vkd3d_mutex_lock(&cache->lock);
    hr = vkd3d_shader_cache_memory_get(cache, key_hash, key, key_size, value, value_size);
    vkd3d_mutex_unlock(&cache->lock);
    if (hr != DXGI_ERROR_NOT_FOUND || cache->mode == D3D12_SHADER_CACHE_MODE_MEMORY)
        return hr;
    if (FAILED(hr = vkd3d_cache_disk_lock_shared(&cache->disk)))
        return hr;
    result_size = *value_size;
    hr = vkd3d_cache_disk_load_entry(cache, key_hash, key, key_size,
            value, &result_size, &payload, false);
    *value_size = result_size;
    if ((SUCCEEDED(hr) || hr == DXGI_ERROR_MORE_DATA) && payload)
    {
        memory_size = result_size;
        vkd3d_mutex_lock(&cache->lock);
        memory_hr = vkd3d_shader_cache_memory_insert(cache, key_hash, payload, key_size,
                payload + key_size, memory_size, true);
        vkd3d_mutex_unlock(&cache->lock);
        if (FAILED(memory_hr) && memory_hr != DXGI_ERROR_CACHE_FULL)
            WARN("Disk cache read succeeded but memory tier insertion failed, hr %#lx.\n", memory_hr);
    }
    hr = vkd3d_cache_disk_unlock(&cache->disk, hr);
    vkd3d_free(payload);
    if (vkd3d_cache_is_corrupt_hresult(hr))
    {
        if (FAILED(memory_hr = vkd3d_cache_root_lock(&cache->disk)))
            return memory_hr;
        if (FAILED(memory_hr = vkd3d_cache_ensure_root_state(&cache->disk, &root_state)))
            return vkd3d_cache_root_unlock(&cache->disk, memory_hr);
        if (FAILED(memory_hr = vkd3d_cache_disk_lock(&cache->disk)))
            return vkd3d_cache_root_unlock(&cache->disk, memory_hr);
        result_size = value_size_in;
        hr = vkd3d_cache_disk_load_entry(cache, key_hash, key, key_size,
                value, &result_size, &payload, false);
        *value_size = result_size;
        hr = vkd3d_cache_disk_normalize_lookup_failure(cache, key_hash, hr, DXGI_ERROR_NOT_FOUND);
        if (SUCCEEDED(memory_hr = vkd3d_cache_ensure_quota_state(cache, &quota_state))
                && SUCCEEDED(memory_hr = vkd3d_cache_root_set_namespace_physical(&root_state,
                        cache->disk.namespace_hash, quota_state.physical_size,
                        quota_state.entry_count)))
            memory_hr = vkd3d_cache_write_root_state(&cache->disk, &root_state);
        if (FAILED(memory_hr) && (SUCCEEDED(hr) || hr == DXGI_ERROR_NOT_FOUND))
            hr = memory_hr;
        hr = vkd3d_cache_disk_unlock(&cache->disk, hr);
        hr = vkd3d_cache_root_unlock(&cache->disk, hr);
        vkd3d_free(payload);
    }
    else
    {
        hr = vkd3d_cache_disk_normalize_lookup_failure(cache, key_hash, hr, DXGI_ERROR_NOT_FOUND);
    }
    return hr;
}

HRESULT vkd3d_shader_cache_set_delete_on_destroy(struct vkd3d_shader_cache *cache)
{
    HRESULT hr, lock_hr;

    TRACE("cache %p.\n", cache);
    if (!cache)
        return E_INVALIDARG;
    if (cache->mode == D3D12_SHADER_CACHE_MODE_MEMORY)
        return S_OK;

retry_operation_lock:
    if (FAILED(hr = vkd3d_cache_root_lock(&cache->disk)))
        return hr;
    lock_hr = vkd3d_cache_lock_range(cache->disk.lock_handle,
            VKD3D_CACHE_OPERATION_LOCK_OFFSET, true, true);
    if (lock_hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
    {
        if (FAILED(hr = vkd3d_cache_root_unlock(&cache->disk, S_OK)))
            return hr;
        if (FAILED(hr = vkd3d_cache_disk_lock(&cache->disk)))
            return hr;
        if (FAILED(hr = vkd3d_cache_disk_unlock(&cache->disk, S_OK)))
            return hr;
        goto retry_operation_lock;
    }
    if (FAILED(lock_hr))
        return vkd3d_cache_root_unlock(&cache->disk, lock_hr);
    hr = vkd3d_cache_write_namespace_metadata(&cache->disk,
            cache->disk.namespace_hash, cache->disk.marker_path, NULL, 0);
    hr = vkd3d_cache_disk_unlock(&cache->disk, hr);
    return vkd3d_cache_root_unlock(&cache->disk, hr);
}

static void vkd3d_shader_cache_disk_destroy(struct vkd3d_shader_cache *cache)
{
    struct vkd3d_shader_cache_disk *disk = &cache->disk;
    struct vkd3d_cache_root_state root_state;
    HRESULT hr, operation_hr = S_OK;
    bool marker_exists, root_locked = false;

    if (disk->lifecycle_locked)
    {
        if (FAILED(hr = vkd3d_cache_unlock_range(disk->lock_handle,
                VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET)))
            ERR("Failed to release shader cache lifecycle lock, hr %#lx.\n", hr);
        disk->lifecycle_locked = false;
    }

    if (FAILED(hr = vkd3d_cache_root_lock(disk)))
    {
        ERR("Failed to acquire shader cache root lock during destruction, hr %#lx.\n", hr);
        vkd3d_cache_disk_cleanup(disk);
        return;
    }
    root_locked = true;
    if (FAILED(hr = vkd3d_cache_ensure_root_state(disk, &root_state)))
    {
        ERR("Failed to load shader cache root state during destruction, hr %#lx.\n", hr);
        goto unlock_root;
    }

    hr = vkd3d_cache_lock_range(disk->lock_handle,
            VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET, true, true);
    if (hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
        goto unlock_root;
    if (FAILED(hr))
    {
        ERR("Failed to acquire final shader cache lifecycle lock, hr %#lx.\n", hr);
        goto unlock_root;
    }

    if (FAILED(hr = vkd3d_cache_disk_lock(disk)))
    {
        ERR("Failed to acquire shader cache operation lock during destruction, hr %#lx.\n", hr);
        goto unlock_lifecycle;
    }
    if (FAILED(operation_hr = vkd3d_cache_regular_file_exists(disk->marker_path, &marker_exists)))
        ;
    else if (marker_exists)
    {
        operation_hr = vkd3d_cache_disk_reset(cache);
        if (SUCCEEDED(operation_hr))
            operation_hr = vkd3d_cache_root_set_namespace_physical(&root_state,
                    disk->namespace_hash, 0, 0);
        if (SUCCEEDED(operation_hr))
            operation_hr = vkd3d_cache_write_root_state(disk, &root_state);
        if (!DeleteFileW(disk->marker_path) && GetLastError() != ERROR_FILE_NOT_FOUND
                && SUCCEEDED(operation_hr))
            operation_hr = vkd3d_cache_last_error();
    }
    operation_hr = vkd3d_cache_disk_unlock(disk, operation_hr);
    if (FAILED(operation_hr))
        ERR("Failed to process shader cache delete-on-destroy marker, hr %#lx.\n", operation_hr);

unlock_lifecycle:
    if (FAILED(hr = vkd3d_cache_unlock_range(disk->lock_handle,
            VKD3D_CACHE_LIFECYCLE_LOCK_OFFSET)))
        ERR("Failed to release final shader cache lifecycle lock, hr %#lx.\n", hr);
unlock_root:
    if (root_locked && FAILED(hr = vkd3d_cache_root_unlock(disk, S_OK)))
        ERR("Failed to release shader cache root lock during destruction, hr %#lx.\n", hr);
    vkd3d_cache_disk_cleanup(disk);
}

unsigned int vkd3d_shader_cache_decref(struct vkd3d_shader_cache *cache)
{
    unsigned int refcount = vkd3d_atomic_decrement_u32(&cache->refcount);

    TRACE("cache %p refcount %u.\n", cache, refcount);
    if (refcount)
        return refcount;

    vkd3d_shader_cache_clear_memory(cache);
    if (cache->mode == D3D12_SHADER_CACHE_MODE_DISK)
        vkd3d_shader_cache_disk_destroy(cache);
    vkd3d_mutex_destroy(&cache->lock);
    vkd3d_free(cache);
    return 0;
}

HRESULT vkd3d_shader_cache_clear_application_disk_caches(void)
{
    struct vkd3d_cache_root_state state;
    struct vkd3d_shader_cache_disk disk;
    bool removed, root_locked = false;
    uint32_t index = 0;
    HRESULT hr, clear_hr, first_error = S_OK;

    memset(&disk, 0, sizeof(disk));
    disk.cache_root_handle = INVALID_HANDLE_VALUE;
    disk.root_lock_handle = INVALID_HANDLE_VALUE;
    disk.root_handle = INVALID_HANDLE_VALUE;
    disk.entries_handle = INVALID_HANDLE_VALUE;
    disk.temp_handle = INVALID_HANDLE_VALUE;
    disk.lock_handle = INVALID_HANDLE_VALUE;

    if (FAILED(hr = vkd3d_cache_get_application_root(&disk.cache_root_path)))
        goto done;
    if (FAILED(hr = vkd3d_cache_open_directory(disk.cache_root_path,
            &disk.cache_root_handle))
            || FAILED(hr = vkd3d_cache_path_join(disk.cache_root_path,
                    vkd3d_cache_root_lock_name, &disk.root_lock_path))
            || FAILED(hr = vkd3d_cache_path_join(disk.cache_root_path,
                    vkd3d_cache_root_state_name, &disk.root_state_path))
            || FAILED(hr = vkd3d_cache_path_join(disk.cache_root_path,
                    vkd3d_cache_root_state_temp_name, &disk.root_state_temp_path))
            || FAILED(hr = vkd3d_cache_open_lock_file(disk.root_lock_path,
                    OPEN_ALWAYS, &disk.root_lock_handle))
            || FAILED(hr = vkd3d_cache_root_lock(&disk)))
        goto done;
    root_locked = true;
    if (FAILED(hr = vkd3d_cache_ensure_root_state(&disk, &state)))
        goto done;

    while (index < state.namespace_count)
    {
        clear_hr = vkd3d_cache_root_clear_namespace(&disk,
                state.namespaces[index].hash, &removed);
        if (clear_hr == S_FALSE)
        {
            ++index;
            continue;
        }
        if (FAILED(clear_hr))
        {
            if (SUCCEEDED(first_error))
                first_error = clear_hr;
            ++index;
            continue;
        }
        clear_hr = vkd3d_cache_root_discard_namespace_reservations(&disk, &state,
                state.namespaces[index].hash);
        if (FAILED(clear_hr))
        {
            if (SUCCEEDED(first_error))
                first_error = clear_hr;
            ++index;
            continue;
        }
        if (removed)
            vkd3d_cache_root_remove_namespace(&state, index);
        else
        {
            VKD3D_ASSERT(state.physical_size >= state.namespaces[index].physical_size);
            VKD3D_ASSERT(state.entry_count >= state.namespaces[index].entry_count);
            state.physical_size -= state.namespaces[index].physical_size;
            state.entry_count -= state.namespaces[index].entry_count;
            state.namespaces[index].physical_size = 0;
            state.namespaces[index].entry_count = 0;
            ++index;
        }
    }
    if (FAILED(hr = vkd3d_cache_write_root_state(&disk, &state)) && SUCCEEDED(first_error))
        first_error = hr;
    hr = first_error;

done:
    if (root_locked)
        hr = vkd3d_cache_root_unlock(&disk, hr);
    vkd3d_cache_disk_cleanup(&disk);
    return hr;
}
