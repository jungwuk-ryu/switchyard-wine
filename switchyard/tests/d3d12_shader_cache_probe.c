#define COBJMACROS

#include <windows.h>
#include <initguid.h>

#include <dxgi1_6.h>
#include <d3d12.h>

#include <psapi.h>
#include <wincrypt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHADER_CACHE_KEY_BASE_DATA1 0x8b1a56f2
#define SHADER_CACHE_KEY_BASE_DATA2 0xe984
#define SHADER_CACHE_KEY_BASE_DATA3 0x49d7
#define SHADER_CACHE_KEY_BASE_DATA4 0x82
#define SHADER_CACHE_HEADER_SIZE 176u
#define SHADER_CACHE_HEADER_HASH_OFFSET 144u
#define SHADER_CACHE_PAYLOAD_HASH_OFFSET 112u
#define SHADER_CACHE_KEY_SIZE_OFFSET 24u
#define SHADER_CACHE_ENTRIES_DIRECTORY "\\0000000000000000000000000000000000000000000000000000000000000001\\"
#define SHADER_CACHE_TEMP_DIRECTORY "\\0000000000000000000000000000000000000000000000000000000000000002\\"

struct cache_file_entry
{
    char path[MAX_PATH];
    ULONGLONG size;
    struct cache_file_entry *next;
};

struct workdir_ctx
{
    char old_cwd[MAX_PATH];
    char root[MAX_PATH];
    BOOL active;
};

struct scenario_metrics
{
    const char *name;
    double latency_ms;
    unsigned long long hits;
    unsigned long long misses;
    unsigned long long operations;
    ULONGLONG rss_peak;
    ULONGLONG disk_usage;
};

typedef enum
{
    CHILD_OP_NONE,
    CHILD_OP_STORE,
    CHILD_OP_FIND,
    CHILD_OP_BIG_STORE,
} child_op_t;

struct child_opts
{
    child_op_t op;
    BOOL use_disk;
    BOOL expect_hit;
    BOOL expect_miss;
    BOOL allow_already_exists;
    BOOL with_working_dir_flag;
    BOOL big_store;
    char working_dir[MAX_PATH];
    unsigned int identifier;
    unsigned int entries;
    unsigned int value_base;
    unsigned int payload_size;
    unsigned long long payload_size64;
    unsigned int wait_mark_ms;
    char key_prefix[128];
    char key[128];
};

static unsigned long long get_time_us(void)
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER now;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (unsigned long long)now.QuadPart * 1000000ULL / (unsigned long long)frequency.QuadPart;
}

static ULONGLONG read_working_set_peak(void)
{
    PROCESS_MEMORY_COUNTERS_EX counters;

    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof(counters)))
        return 0;
    return counters.PeakWorkingSetSize;
}

static const char *hresult_name(HRESULT hr)
{
    static char buf[64];

    if (hr == S_OK) return "S_OK";
    if (hr == S_FALSE) return "S_FALSE";
    if (hr == E_FAIL) return "E_FAIL";
    if (hr == E_NOINTERFACE) return "E_NOINTERFACE";
    if (hr == E_INVALIDARG) return "E_INVALIDARG";
    if (hr == DXGI_ERROR_NOT_FOUND) return "DXGI_ERROR_NOT_FOUND";
    if (hr == DXGI_ERROR_ALREADY_EXISTS) return "DXGI_ERROR_ALREADY_EXISTS";
    if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
    if (hr == DXGI_ERROR_CACHE_FULL) return "DXGI_ERROR_CACHE_FULL";
    if (hr == DXGI_ERROR_MORE_DATA) return "DXGI_ERROR_MORE_DATA";
    if (hr == DXGI_ERROR_ACCESS_DENIED) return "DXGI_ERROR_ACCESS_DENIED";
    if (hr == DXGI_ERROR_CACHE_HASH_COLLISION) return "DXGI_ERROR_CACHE_HASH_COLLISION";

    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)hr);
    return buf;
}

static void fail(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void print_metrics(const struct scenario_metrics *metric, const char *note)
{
    if (!metric)
        return;
    printf("[metrics] scenario=%s latency_ms=%.3f hits=%llu misses=%llu operations=%llu hit_rate=%.3f rss_peak=%llu disk_bytes=%llu %s\n",
           metric->name, metric->latency_ms, (unsigned long long)metric->hits,
           (unsigned long long)metric->misses, (unsigned long long)metric->operations,
           metric->operations ? (double)metric->hits / (double)metric->operations : 0.0,
           (unsigned long long)metric->rss_peak, (unsigned long long)metric->disk_usage,
           note ? note : "");
}

static int workdir_init(struct workdir_ctx *ctx)
{
    char temp_path[MAX_PATH];
    DWORD len;

    ctx->active = FALSE;
    len = GetCurrentDirectoryA(sizeof(ctx->old_cwd), ctx->old_cwd);
    if (!len || len >= sizeof(ctx->old_cwd))
        return 0;
    if (!GetTempPathA(sizeof(temp_path), temp_path) || !temp_path[0])
        return 0;
    if (!GetTempFileNameA(temp_path, "sd3", 0, ctx->root))
        return 0;
    DeleteFileA(ctx->root);
    if (!CreateDirectoryA(ctx->root, NULL))
        return 0;
    if (!SetCurrentDirectoryA(ctx->root))
    {
        RemoveDirectoryA(ctx->root);
        return 0;
    }
    ctx->active = TRUE;
    return 1;
}

static void workdir_cleanup(struct workdir_ctx *ctx)
{
    if (!ctx->active) return;
    SetCurrentDirectoryA(ctx->old_cwd);
    ctx->active = FALSE;
}

static void free_cache_file_list(struct cache_file_entry *list)
{
    while (list)
    {
        struct cache_file_entry *next = list->next;
        free(list);
        list = next;
    }
}

static void add_cache_file_node(struct cache_file_entry **list, const char *path, ULONGLONG size)
{
    struct cache_file_entry *node;

    node = malloc(sizeof(*node));
    if (!node) return;
    strcpy(node->path, path);
    node->size = size;
    node->next = *list;
    *list = node;
}

static void discover_cache_files(const char *root, struct cache_file_entry **list, ULONGLONG *size_total)
{
    WIN32_FIND_DATAA data;
    HANDLE find;
    char search[MAX_PATH + 64];
    char child[MAX_PATH + 64];

    if (!size_total) return;

    snprintf(search, sizeof(search), "%s\\*", root);
    find = FindFirstFileA(search, &data);
    if (find == INVALID_HANDLE_VALUE) return;

    do
    {
        if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, ".."))
            continue;

        snprintf(child, sizeof(child), "%s\\%s", root, data.cFileName);

        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            discover_cache_files(child, list, size_total);
            continue;
        }

        *size_total += (((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow);
        add_cache_file_node(list, child, (((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow));
    }
    while (FindNextFileA(find, &data));
    FindClose(find);
}

static BOOL cache_file_list_has_temp(const struct cache_file_entry *list, ULONGLONG minimum_size)
{
    for (; list; list = list->next)
    {
        if (strstr(list->path, SHADER_CACHE_TEMP_DIRECTORY) && list->size >= minimum_size)
            return TRUE;
    }
    return FALSE;
}

static void init_cache_desc(D3D12_SHADER_CACHE_SESSION_DESC *desc,
                           unsigned int identifier, D3D12_SHADER_CACHE_MODE mode,
                           UINT64 version, BOOL use_working_dir)
{
    static const GUID base =
    {
        SHADER_CACHE_KEY_BASE_DATA1, SHADER_CACHE_KEY_BASE_DATA2, SHADER_CACHE_KEY_BASE_DATA3,
        {SHADER_CACHE_KEY_BASE_DATA4, 0xe7, 0x59, 0xd7, 0xe9, 0xda, 0x31, 0x41}
    };

    memset(desc, 0, sizeof(*desc));
    desc->Identifier = base;
    desc->Identifier.Data1 += identifier;
    desc->Mode = mode;
    desc->Version = version;
    desc->Flags = use_working_dir ? D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR : 0;
}

static int create_device9(ID3D12Device **device, ID3D12Device9 **device9)
{
    HRESULT hr;

    *device = NULL;
    *device9 = NULL;
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                           &IID_ID3D12Device, (void **)device);
    if (FAILED(hr))
    {
        printf("[skip] D3D12CreateDevice failed: %s\n", hresult_name(hr));
        return 0;
    }

    hr = ID3D12Device_QueryInterface(*device, &IID_ID3D12Device9, (void **)device9);
    if (hr == E_NOINTERFACE)
    {
        ID3D12Device_Release(*device);
        *device = NULL;
        printf("[skip] ID3D12Device9 unavailable on this runtime\n");
        return 0;
    }
    if (FAILED(hr))
    {
        ID3D12Device_Release(*device);
        *device = NULL;
        printf("[skip] ID3D12Device9 query failed: %s\n", hresult_name(hr));
        return 0;
    }
    return 1;
}

static void key_for_index(const char *prefix, unsigned int index, char *buffer, size_t size)
{
    snprintf(buffer, size, "%s-%u", prefix, index);
}

static HRESULT create_cache_session(ID3D12Device9 *device9,
                                   const D3D12_SHADER_CACHE_SESSION_DESC *desc,
                                   ID3D12ShaderCacheSession **session)
{
    *session = NULL;
    return ID3D12Device9_CreateShaderCacheSession(device9, desc,
            &IID_ID3D12ShaderCacheSession, (void **)session);
}

static HRESULT store_entry(ID3D12ShaderCacheSession *session,
                          const char *key, unsigned int payload_size,
                          unsigned int index, unsigned int value_base)
{
    unsigned int size = payload_size ? payload_size : sizeof(DWORD);
    BYTE *value;
    HRESULT hr;

    if (size < sizeof(DWORD))
        return E_INVALIDARG;
    value = malloc(size);
    if (!value) return E_OUTOFMEMORY;
    memset(value, (unsigned char)((index + 0x5a) & 0xff), size);
    *(unsigned int *)value = value_base + index;

    hr = ID3D12ShaderCacheSession_StoreValue(session, key, (UINT)strlen(key) + 1,
            value, size);
    free(value);
    return hr;
}

static HRESULT find_entry(ID3D12ShaderCacheSession *session, const char *key,
                         unsigned int payload_size, unsigned int expected, BOOL expect_found,
                         unsigned int *observed)
{
    UINT value_size = 0;
    HRESULT hr;
    BYTE *value;

    hr = ID3D12ShaderCacheSession_FindValue(session, key, (UINT)strlen(key) + 1,
                                           NULL, &value_size);
    if (!expect_found)
    {
        if (observed) *observed = 0;
        return hr;
    }
    if (FAILED(hr))
        return hr;
    if (value_size < sizeof(DWORD))
        return E_INVALIDARG;
    if (payload_size && value_size != payload_size)
        return DXGI_ERROR_MORE_DATA;

    value = malloc(value_size);
    if (!value)
        return E_OUTOFMEMORY;

    hr = ID3D12ShaderCacheSession_FindValue(session, key, (UINT)strlen(key) + 1,
                                           value, &value_size);
    if (SUCCEEDED(hr))
    {
        unsigned int observed_value = *(unsigned int *)value;
        if (observed) *observed = observed_value;
        if (observed_value != expected)
            hr = E_FAIL;
    }
    free(value);
    return hr;
}

static int run_child_store(const struct child_opts *opts)
{
    ID3D12Device *device = NULL;
    ID3D12Device9 *device9 = NULL;
    ID3D12ShaderCacheSession *session = NULL;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    unsigned int i;
    BOOL stored = FALSE, existed = FALSE;
    HRESULT hr;

    if (!create_device9(&device, &device9))
        return 77;

    if (opts->working_dir[0] && opts->use_disk)
    {
        if (!SetCurrentDirectoryA(opts->working_dir))
        {
            fail("Child failed to switch working directory: %lu", GetLastError());
            ID3D12Device9_Release(device9);
            ID3D12Device_Release(device);
            return 1;
        }
    }

    init_cache_desc(&desc, opts->identifier, opts->use_disk ? D3D12_SHADER_CACHE_MODE_DISK : D3D12_SHADER_CACHE_MODE_MEMORY,
                    1, opts->with_working_dir_flag && opts->use_disk);
    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
    {
        fail("Child session creation failed: %s", hresult_name(hr));
        ID3D12Device9_Release(device9);
        ID3D12Device_Release(device);
        return 1;
    }

    for (i = 0; i < opts->entries; ++i)
    {
        char key[128];
        UINT size = opts->payload_size ? opts->payload_size : sizeof(DWORD);

        key_for_index(opts->key_prefix[0] ? opts->key_prefix : opts->key, i, key, sizeof(key));
        hr = store_entry(session, key, size, i, opts->value_base);
        if (!opts->allow_already_exists)
        {
            if (FAILED(hr))
            {
                fail("Child store failed key %u: %s", i, hresult_name(hr));
                ID3D12ShaderCacheSession_Release(session);
                ID3D12Device9_Release(device9);
                ID3D12Device_Release(device);
                return 1;
            }
        }
        else if (hr != S_OK && hr != DXGI_ERROR_ALREADY_EXISTS)
        {
            fail("Child duplicate-writer returned %s for %s", hresult_name(hr), key);
            ID3D12ShaderCacheSession_Release(session);
            ID3D12Device9_Release(device9);
            ID3D12Device_Release(device);
            return 1;
        }
        else if (hr == S_OK)
            stored = TRUE;
        else
            existed = TRUE;
    }

    ID3D12ShaderCacheSession_Release(session);
    ID3D12Device9_Release(device9);
    ID3D12Device_Release(device);

    if (!opts->allow_already_exists || stored)
        return 0;
    return existed ? 3 : 1;
}

static int run_child_find(const struct child_opts *opts)
{
    ID3D12Device *device = NULL;
    ID3D12Device9 *device9 = NULL;
    ID3D12ShaderCacheSession *session = NULL;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    unsigned int i;
    HRESULT hr;

    if (!create_device9(&device, &device9))
        return 77;

    if (opts->working_dir[0] && opts->use_disk)
    {
        if (!SetCurrentDirectoryA(opts->working_dir))
        {
            fail("Child failed to switch working directory: %lu", GetLastError());
            ID3D12Device9_Release(device9);
            ID3D12Device_Release(device);
            return 1;
        }
    }

    init_cache_desc(&desc, opts->identifier, opts->use_disk ? D3D12_SHADER_CACHE_MODE_DISK : D3D12_SHADER_CACHE_MODE_MEMORY,
                    1, opts->with_working_dir_flag && opts->use_disk);
    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
    {
        fail("Child find session creation failed: %s", hresult_name(hr));
        ID3D12Device9_Release(device9);
        ID3D12Device_Release(device);
        return 1;
    }

    for (i = 0; i < opts->entries; ++i)
    {
        char key[128];
        unsigned int observed = 0;
        BOOL expect_found = opts->expect_hit && !opts->expect_miss;
        UINT size = opts->payload_size ? opts->payload_size : sizeof(DWORD);

        key_for_index(opts->key_prefix[0] ? opts->key_prefix : opts->key, i, key, sizeof(key));

        hr = find_entry(session, key, size, opts->value_base + i, expect_found, &observed);
        if (opts->expect_miss)
        {
            if (hr != DXGI_ERROR_NOT_FOUND)
            {
                fail("Child find-miss failed for %s: %s", key, hresult_name(hr));
                ID3D12ShaderCacheSession_Release(session);
                ID3D12Device9_Release(device9);
                ID3D12Device_Release(device);
                return 1;
            }
            continue;
        }

        if (!expect_found)
            continue;
        if (hr != S_OK)
        {
            fail("Child find hit failed for %s: %s", key, hresult_name(hr));
            ID3D12ShaderCacheSession_Release(session);
            ID3D12Device9_Release(device9);
            ID3D12Device_Release(device);
            return 1;
        }
        if (observed != opts->value_base + i)
        {
            fail("Child value mismatch for %s: got %u expected %u", key,
                 observed, opts->value_base + i);
            ID3D12ShaderCacheSession_Release(session);
            ID3D12Device9_Release(device9);
            ID3D12Device_Release(device);
            return 1;
        }
    }

    ID3D12ShaderCacheSession_Release(session);
    ID3D12Device9_Release(device9);
    ID3D12Device_Release(device);
    return 0;
}

static int run_child_big_store(const struct child_opts *opts)
{
    ID3D12Device *device = NULL;
    ID3D12Device9 *device9 = NULL;
    ID3D12ShaderCacheSession *session = NULL;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    HRESULT hr;
    HANDLE ready;
    BYTE *value;
    UINT value_size;

    if (!create_device9(&device, &device9))
        return 77;

    if (!SetCurrentDirectoryA(opts->working_dir))
    {
        fail("Child could not switch working directory: %lu", GetLastError());
        ID3D12Device9_Release(device9);
        ID3D12Device_Release(device);
        return 1;
    }

    init_cache_desc(&desc, opts->identifier, D3D12_SHADER_CACHE_MODE_DISK,
                    1, TRUE);
    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
    {
        fail("Child big store session failed: %s", hresult_name(hr));
        ID3D12Device9_Release(device9);
        ID3D12Device_Release(device);
        return 1;
    }

    if (!opts->payload_size64 || opts->payload_size64 > UINT_MAX)
    {
        ID3D12ShaderCacheSession_Release(session);
        ID3D12Device9_Release(device9);
        ID3D12Device_Release(device);
        return 1;
    }
    value_size = (UINT)opts->payload_size64;
    if (!(value = malloc(value_size)))
    {
        ID3D12ShaderCacheSession_Release(session);
        ID3D12Device9_Release(device9);
        ID3D12Device_Release(device);
        return 1;
    }
    memset(value, 0xa5, value_size);
    memcpy(value, &opts->value_base, sizeof(opts->value_base));

    ready = CreateFileA("switchyard_big_writer_ready.flag", GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (ready != INVALID_HANDLE_VALUE)
    {
        CloseHandle(ready);
    }

    hr = ID3D12ShaderCacheSession_StoreValue(session,
            opts->key_prefix[0] ? opts->key_prefix : "force-termination",
            (UINT)strlen(opts->key_prefix[0] ? opts->key_prefix : "force-termination") + 1,
            value, value_size);
    free(value);
    if (FAILED(hr))
    {
        fail("Child big store failed: %s", hresult_name(hr));
        ID3D12ShaderCacheSession_Release(session);
        ID3D12Device9_Release(device9);
        ID3D12Device_Release(device);
        return 1;
    }

    ID3D12ShaderCacheSession_Release(session);
    ID3D12Device9_Release(device9);
    ID3D12Device_Release(device);
    return 0;
}

static int run_child(const struct child_opts *opts)
{
    if (opts->op == CHILD_OP_STORE)
        return run_child_store(opts);
    if (opts->op == CHILD_OP_FIND)
        return run_child_find(opts);
    if (opts->op == CHILD_OP_BIG_STORE)
        return run_child_big_store(opts);

    fail("unknown child mode\n");
    return 1;
}

static int spawn_child(const char *exe, const struct child_opts *opts,
                      unsigned int *child_exit)
{
    char command[2048];
    STARTUPINFOA si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    char op_arg[32] = "";
    char mode_arg[16] = "";
    char working_dir_arg[2 * MAX_PATH + 32] = "";
    char identifier_arg[64] = "";
    char entries_arg[64] = "";
    char value_base_arg[64] = "";
    char payload_arg[64] = "";
    char wait_arg[64] = "";
    PROCESS_MEMORY_COUNTERS_EX counters = {0};
    unsigned long long start;

    snprintf(op_arg, sizeof(op_arg), "--op=%s",
             opts->op == CHILD_OP_STORE ? "store" :
             opts->op == CHILD_OP_FIND ? "find" :
             "big-store");
    snprintf(mode_arg, sizeof(mode_arg), "--mode=%s", opts->use_disk ? "disk" : "mem");
    if (opts->use_disk)
        snprintf(working_dir_arg, sizeof(working_dir_arg), " --working-dir=\"%s\"", opts->working_dir);
    snprintf(identifier_arg, sizeof(identifier_arg), " --identifier=%u", opts->identifier);
    snprintf(entries_arg, sizeof(entries_arg), " --entries=%u", opts->entries);
    snprintf(value_base_arg, sizeof(value_base_arg), " --value-base=%u", opts->value_base);
    if (opts->payload_size)
        snprintf(payload_arg, sizeof(payload_arg), " --payload=%u", opts->payload_size);
    if (opts->payload_size64)
        snprintf(payload_arg, sizeof(payload_arg), " --payload64=%llu", opts->payload_size64);
    if (opts->wait_mark_ms)
        snprintf(wait_arg, sizeof(wait_arg), " --wait=%u", opts->wait_mark_ms);

    snprintf(command, sizeof(command),
             "\"%s\" --child %s %s%s%s%s%s%s%s --key-prefix=%s",
             exe,
             op_arg,
             mode_arg,
             working_dir_arg,
             identifier_arg,
             entries_arg,
             value_base_arg,
             payload_arg,
             wait_arg,
             opts->key_prefix[0] ? opts->key_prefix : opts->key);

    if (opts->expect_miss)
        strcat(command, " --expect-miss");
    if (opts->expect_hit)
        strcat(command, " --expect-hit");
    if (opts->with_working_dir_flag)
        strcat(command, " --use-working-dir");
    if (opts->allow_already_exists)
        strcat(command, " --allow-exists");

    start = get_time_us();
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        fail("Failed to start child process: %lu", GetLastError());
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    counters.cb = sizeof(counters);
    GetProcessMemoryInfo(pi.hProcess, (PROCESS_MEMORY_COUNTERS *)&counters, sizeof(counters));
    if (!GetExitCodeProcess(pi.hProcess, (LPDWORD)child_exit))
        *child_exit = 1;

    printf("[child-metrics] op=%s latency_ms=%.3f rss_peak=%llu exit=%u\n",
            opts->op == CHILD_OP_STORE ? "store" :
            opts->op == CHILD_OP_FIND ? "find" : "big-store",
            (double)(get_time_us() - start) / 1000.0,
            (unsigned long long)counters.PeakWorkingSetSize, *child_exit);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}

struct spawn_child_thread_ctx
{
    const char *exe;
    const struct child_opts *opts;
    unsigned int exit_code;
    int spawn_result;
};

static DWORD WINAPI spawn_child_worker(void *arg)
{
    struct spawn_child_thread_ctx *ctx = arg;

    ctx->spawn_result = spawn_child(ctx->exe, ctx->opts, &ctx->exit_code);
    return 0;
}

static const struct cache_file_entry *first_data_entry(const struct cache_file_entry *files)
{
    while (files)
    {
        if (strstr(files->path, SHADER_CACHE_ENTRIES_DIRECTORY))
            return files;
        files = files->next;
    }
    return NULL;
}

static int sha256_bytes(const BYTE *data, DWORD size, BYTE digest[32])
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    DWORD digest_size = 32;
    int ret = 0;

    if (!CryptAcquireContextA(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        goto done;
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
        goto done;
    if (size && !CryptHashData(hash, data, size, 0))
        goto done;
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_size, 0) || digest_size != 32)
        goto done;
    ret = 1;

done:
    if (hash)
        CryptDestroyHash(hash);
    if (provider)
        CryptReleaseContext(provider, 0);
    return ret;
}

static int rewrite_entry_key(const struct cache_file_entry *entry,
        const char *old_key, const char *new_key)
{
    LARGE_INTEGER size, zero = {0};
    ULONGLONG stored_key_size;
    BYTE digest[32];
    BYTE *data = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD count;
    int ret = 0;

    if (!entry || strlen(old_key) != strlen(new_key) || entry->size > 1024 * 1024)
        return 0;
    file = CreateFileA(entry->path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size)
            || size.QuadPart < SHADER_CACHE_HEADER_SIZE || size.QuadPart > 1024 * 1024)
        goto done;
    if (!(data = malloc((size_t)size.QuadPart)))
        goto done;
    if (!ReadFile(file, data, (DWORD)size.QuadPart, &count, NULL) || count != size.QuadPart)
        goto done;
    if (memcmp(data, "VKD3DC01", 8))
        goto done;
    memcpy(&stored_key_size, data + SHADER_CACHE_KEY_SIZE_OFFSET, sizeof(stored_key_size));
    if (stored_key_size != strlen(old_key) + 1
            || SHADER_CACHE_HEADER_SIZE + stored_key_size > (ULONGLONG)size.QuadPart
            || memcmp(data + SHADER_CACHE_HEADER_SIZE, old_key, stored_key_size))
        goto done;

    memcpy(data + SHADER_CACHE_HEADER_SIZE, new_key, stored_key_size);
    if (!sha256_bytes(data + SHADER_CACHE_HEADER_SIZE,
            (DWORD)size.QuadPart - SHADER_CACHE_HEADER_SIZE, digest))
        goto done;
    memcpy(data + SHADER_CACHE_PAYLOAD_HASH_OFFSET, digest, sizeof(digest));
    if (!sha256_bytes(data, SHADER_CACHE_HEADER_HASH_OFFSET, digest))
        goto done;
    memcpy(data + SHADER_CACHE_HEADER_HASH_OFFSET, digest, sizeof(digest));
    if (!SetFilePointerEx(file, zero, NULL, FILE_BEGIN)
            || !WriteFile(file, data, (DWORD)size.QuadPart, &count, NULL)
            || count != size.QuadPart || !FlushFileBuffers(file))
        goto done;
    ret = 1;

done:
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    free(data);
    return ret;
}

static int corrupt_entry_file(const struct cache_file_entry *files)
{
    const struct cache_file_entry *entry;
    LARGE_INTEGER offset;
    HANDLE file;
    BYTE byte;
    DWORD count;

    for (entry = files; entry; entry = entry->next)
    {
        /* The v1 entry header is 176 bytes. Manifest and quota records are
         * smaller; select only a data entry in this controlled directory. */
        if (entry->size <= SHADER_CACHE_HEADER_SIZE
                || !strstr(entry->path, SHADER_CACHE_ENTRIES_DIRECTORY))
            continue;
        file = CreateFileA(entry->path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (file == INVALID_HANDLE_VALUE)
            continue;
        offset.QuadPart = -1;
        if (!SetFilePointerEx(file, offset, NULL, FILE_END)
                || !ReadFile(file, &byte, 1, &count, NULL) || count != 1)
        {
            CloseHandle(file);
            continue;
        }
        byte ^= 0x5a;
        offset.QuadPart = -1;
        if (!SetFilePointerEx(file, offset, NULL, FILE_END)
                || !WriteFile(file, &byte, 1, &count, NULL) || count != 1
                || !FlushFileBuffers(file))
        {
            CloseHandle(file);
            return 0;
        }
        CloseHandle(file);
        return 1;
    }
    return 0;
}

static int run_hash_collision_fixture(ID3D12Device9 *device9)
{
    static const char old_key[] = "collision-a";
    static const char new_key[] = "collision-b";
    struct workdir_ctx work;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session = NULL;
    struct cache_file_entry *files = NULL;
    ULONGLONG total_size = 0;
    BYTE value[32] = {0};
    UINT value_size = 0;
    HRESULT find_hr, store_hr, hr;
    int ret = 1;

    if (!workdir_init(&work))
        return 1;
    init_cache_desc(&desc, 0x7e, D3D12_SHADER_CACHE_MODE_DISK, 1, TRUE);
    if (FAILED(hr = create_cache_session(device9, &desc, &session)))
        goto done;
    if (FAILED(hr = ID3D12ShaderCacheSession_StoreValue(session,
            old_key, sizeof(old_key), value, sizeof(value))))
        goto done;
    ID3D12ShaderCacheSession_Release(session);
    session = NULL;

    discover_cache_files(work.root, &files, &total_size);
    if (!rewrite_entry_key(first_data_entry(files), old_key, new_key))
        goto done;
    if (FAILED(hr = create_cache_session(device9, &desc, &session)))
        goto done;

    find_hr = ID3D12ShaderCacheSession_FindValue(session,
            old_key, sizeof(old_key), NULL, &value_size);
    store_hr = ID3D12ShaderCacheSession_StoreValue(session,
            old_key, sizeof(old_key), value, sizeof(value));
    printf("[scenario] valid-hash-collision find=%08lx store=%08lx\n",
            (unsigned long)find_hr, (unsigned long)store_hr);
    ret = find_hr == DXGI_ERROR_CACHE_HASH_COLLISION
            && store_hr == DXGI_ERROR_CACHE_HASH_COLLISION ? 0 : 1;

done:
    if (session)
    {
        ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
        ID3D12ShaderCacheSession_Release(session);
    }
    free_cache_file_list(files);
    workdir_cleanup(&work);
    return ret;
}

static int verify_corrupted_isolation(ID3D12Device9 *device9)
{
    struct workdir_ctx work;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session = NULL;
    struct cache_file_entry *files = NULL;
    ULONGLONG total_size = 0;
    HRESULT hr;
    char key[128];
    unsigned int i;
    const char *base = "corrupt";
    unsigned int hit_count = 0;
    unsigned int key_count = 128;
    int ret = 1;

    if (!workdir_init(&work))
        return 1;
    init_cache_desc(&desc, 0x7f, D3D12_SHADER_CACHE_MODE_DISK, 1, TRUE);
    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
        goto done;

    for (i = 0; i < key_count; ++i)
    {
        key_for_index(base, i, key, sizeof(key));
        hr = store_entry(session, key, 24, i, 0x1000 + i);
        if (FAILED(hr))
            goto done;
    }
    ID3D12ShaderCacheSession_Release(session);
    session = NULL;

    discover_cache_files(work.root, &files, &total_size);
    if (!files || !corrupt_entry_file(files))
        goto done;

    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
        goto done;

    for (i = 0; i < key_count; ++i)
    {
        unsigned int observed = 0;
        key_for_index(base, i, key, sizeof(key));
        hr = find_entry(session, key, 24, 0x1000 + i, TRUE, &observed);
        if (hr == S_OK && observed == 0x1000 + i)
            ++hit_count;
    }

    printf("[check] malformed-entry-isolation hits=%u missing=%u of %u\n",
            hit_count, key_count - hit_count, key_count);
    ret = hit_count == key_count - 1 ? 0 : 1;

done:
    if (session)
    {
        ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
        ID3D12ShaderCacheSession_Release(session);
    }
    free_cache_file_list(files);
    workdir_cleanup(&work);
    return ret;
}

static int verify_finalized_entry_write_exclusion(ID3D12Device9 *device9)
{
    static const char key[] = "write-exclusion";
    struct workdir_ctx work;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session = NULL;
    struct cache_file_entry *files = NULL;
    const struct cache_file_entry *entry;
    ULONGLONG total_size = 0;
    HANDLE writer = INVALID_HANDLE_VALUE;
    unsigned int observed = 0;
    UINT value_size = 0;
    HRESULT blocked_hr, hr;
    int ret = 1;

    if (!workdir_init(&work))
        return 1;
    init_cache_desc(&desc, 0x80, D3D12_SHADER_CACHE_MODE_DISK, 1, TRUE);
    desc.MaximumInMemoryCacheSizeBytes = 1;
    desc.MaximumInMemoryCacheEntries = 1;
    desc.MaximumValueFileSizeBytes = 2 * 1024 * 1024;
    if (FAILED(hr = create_cache_session(device9, &desc, &session)))
        goto done;
    if (FAILED(hr = store_entry(session, key, 1024 * 1024, 0, 0x9000)))
        goto done;
    ID3D12ShaderCacheSession_Release(session);
    session = NULL;

    discover_cache_files(work.root, &files, &total_size);
    if (!(entry = first_data_entry(files)))
        goto done;
    writer = CreateFileA(entry->path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (writer == INVALID_HANDLE_VALUE)
        goto done;
    if (FAILED(hr = create_cache_session(device9, &desc, &session)))
        goto done;

    blocked_hr = ID3D12ShaderCacheSession_FindValue(session,
            key, sizeof(key), NULL, &value_size);
    if (blocked_hr != HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))
        goto done;
    CloseHandle(writer);
    writer = INVALID_HANDLE_VALUE;
    hr = find_entry(session, key, 1024 * 1024, 0x9000, TRUE, &observed);
    printf("[scenario] finalized-entry-write-exclusion blocked=%s recovered=%s\n",
            hresult_name(blocked_hr), hresult_name(hr));
    ret = hr == S_OK && observed == 0x9000 ? 0 : 1;

done:
    if (writer != INVALID_HANDLE_VALUE)
        CloseHandle(writer);
    if (session)
    {
        ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
        ID3D12ShaderCacheSession_Release(session);
    }
    free_cache_file_list(files);
    workdir_cleanup(&work);
    return ret;
}

static int run_memory_mode_check(ID3D12Device9 *device9, struct scenario_metrics *metric)
{
    ID3D12ShaderCacheSession *session = NULL;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    HRESULT hr;
    const unsigned int entries = 256;
    unsigned int i;
    char key[128];
    unsigned long long start;
    unsigned int observed;

    init_cache_desc(&desc, 0x01, D3D12_SHADER_CACHE_MODE_MEMORY, 1, FALSE);
    desc.MaximumInMemoryCacheSizeBytes = 1024 * 1024;
    desc.MaximumInMemoryCacheEntries = entries;
    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
        return 1;

    metric->name = "memory-cold-and-warm";
    metric->operations = 0;
    metric->hits = 0;
    metric->misses = 0;

    start = get_time_us();
    for (i = 0; i < entries; ++i)
    {
        key_for_index("mem", i, key, sizeof(key));
        hr = store_entry(session, key, sizeof(UINT), i, 0x2000 + i);
        if (FAILED(hr))
        {
            ID3D12ShaderCacheSession_Release(session);
            return 1;
        }
    }
    for (i = 0; i < entries; ++i)
    {
        key_for_index("mem", i, key, sizeof(key));
        hr = find_entry(session, key, sizeof(UINT), 0x2000 + i, TRUE, &observed);
        if (hr == S_OK && observed == 0x2000 + i)
            ++metric->hits;
        else
            ++metric->misses;
    }
    metric->operations = metric->hits + metric->misses;
    metric->latency_ms = (double)(get_time_us() - start) / 1000.0;
    metric->rss_peak = read_working_set_peak();
    metric->disk_usage = 0;

    ID3D12ShaderCacheSession_Release(session);
    return 0;
}

struct throughput_ctx
{
    ID3D12ShaderCacheSession *session;
    unsigned int entries;
    unsigned int loops_per_thread;
    volatile unsigned long long *hits;
    volatile unsigned long long *misses;
};

static DWORD WINAPI hit_worker(void *arg)
{
    struct throughput_ctx *ctx = arg;
    char key[128];
    unsigned int thread_index;
    unsigned int j;
    HRESULT hr;

    thread_index = (unsigned int)(uintptr_t)GetCurrentThreadId();
    for (j = 0; j < ctx->loops_per_thread; ++j)
    {
        unsigned int i = (thread_index + j) % ctx->entries;
        unsigned int observed;
        key_for_index("thr", i, key, sizeof(key));
        hr = find_entry(ctx->session, key, sizeof(UINT), 0x3000 + i, TRUE, &observed);
        if (hr == S_OK && observed == 0x3000 + i)
            InterlockedIncrement64((LONGLONG *)ctx->hits);
        else
            InterlockedIncrement64((LONGLONG *)ctx->misses);
    }
    return 0;
}

static int run_throughput(ID3D12Device9 *device9, struct scenario_metrics *metric)
{
    ID3D12ShaderCacheSession *session = NULL;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    const unsigned int entry_count = 2048;
    const unsigned int thread_count = 8;
    const unsigned int loops_per_thread = 500;
    unsigned int i;
    unsigned long long start;
    HANDLE threads[8];
    struct throughput_ctx contexts[8];
    volatile unsigned long long hits = 0;
    volatile unsigned long long misses = 0;
    char key[128];
    DWORD wait_result;
    HRESULT hr;

    init_cache_desc(&desc, 0x02, D3D12_SHADER_CACHE_MODE_MEMORY, 1, FALSE);
    desc.MaximumInMemoryCacheSizeBytes = 1024 * 1024;
    desc.MaximumInMemoryCacheEntries = entry_count;
    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
        return 1;

    for (i = 0; i < entry_count; ++i)
    {
        key_for_index("thr", i, key, sizeof(key));
        hr = store_entry(session, key, sizeof(UINT), i, 0x3000 + i);
        if (FAILED(hr))
        {
            ID3D12ShaderCacheSession_Release(session);
            return 1;
        }
    }

    start = get_time_us();
    for (i = 0; i < thread_count; ++i)
    {
        contexts[i].session = session;
        contexts[i].entries = entry_count;
        contexts[i].loops_per_thread = loops_per_thread;
        contexts[i].hits = &hits;
        contexts[i].misses = &misses;

        threads[i] = CreateThread(NULL, 0, hit_worker, &contexts[i], 0, NULL);
        if (!threads[i])
        {
            if (i)
                WaitForMultipleObjects(i, threads, TRUE, INFINITE);
            while (i)
                CloseHandle(threads[--i]);
            ID3D12ShaderCacheSession_Release(session);
            return 1;
        }
    }
    wait_result = WaitForMultipleObjects(thread_count, threads, TRUE, INFINITE);
    if (wait_result != WAIT_OBJECT_0)
    {
        /* Keep the session alive until every stack-backed context is no
         * longer reachable, even if the aggregate wait itself failed. */
        for (i = 0; i < thread_count; ++i)
        {
            WaitForSingleObject(threads[i], INFINITE);
            CloseHandle(threads[i]);
        }
        ID3D12ShaderCacheSession_Release(session);
        return 1;
    }

    metric->name = "throughput-8-thread-hit";
    metric->hits = hits;
    metric->misses = misses;
    metric->operations = hits + misses;
    metric->latency_ms = (double)(get_time_us() - start) / 1000.0;
    metric->rss_peak = read_working_set_peak();
    metric->disk_usage = 0;

    for (i = 0; i < thread_count; ++i)
    {
        CloseHandle(threads[i]);
    }

    ID3D12ShaderCacheSession_Release(session);
    return metric->misses ? 1 : 0;
}

static int run_ten_k_entries(ID3D12Device9 *device9)
{
    struct workdir_ctx work;
    ID3D12ShaderCacheSession *session = NULL;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    const unsigned int count = 10000;
    unsigned int i;
    char key[128];
    HRESULT hr;
    ULONGLONG before = 0, after = 0;
    struct cache_file_entry *files = NULL;

    if (!workdir_init(&work))
        return 1;
    init_cache_desc(&desc, 0x03, D3D12_SHADER_CACHE_MODE_DISK, 1, TRUE);
    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
    {
        workdir_cleanup(&work);
        return 1;
    }

    discover_cache_files(work.root, &files, &before);
    for (i = 0; i < count; ++i)
    {
        key_for_index("stress", i, key, sizeof(key));
        hr = store_entry(session, key, 64, i, 0x4000 + i);
        if (FAILED(hr))
        {
            ID3D12ShaderCacheSession_Release(session);
            workdir_cleanup(&work);
            free_cache_file_list(files);
            return 1;
        }
    }

    for (i = 0; i < count; ++i)
    {
        unsigned int observed;
        key_for_index("stress", i, key, sizeof(key));
        hr = find_entry(session, key, 64, 0x4000 + i, TRUE, &observed);
        if (FAILED(hr) || observed != 0x4000 + i)
        {
            ID3D12ShaderCacheSession_Release(session);
            workdir_cleanup(&work);
            free_cache_file_list(files);
            return 1;
        }
    }
    discover_cache_files(work.root, &files, &after);

    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);
    printf("[scenario] ten-thousand-entry-stress count=%u disk_delta=%llu\n", count, after > before ? after - before : 0);
    workdir_cleanup(&work);
    free_cache_file_list(files);
    return 0;
}

static int run_cross_process_duplicate(void)
{
    struct workdir_ctx work;
    struct child_opts opts;
    struct spawn_child_thread_ctx children[8];
    HANDLE threads[8] = {0};
    char exe[MAX_PATH];
    unsigned int i;
    unsigned int success = 0, already_exists = 0;

    if (!GetModuleFileNameA(NULL, exe, sizeof(exe)))
        return 1;

    if (!workdir_init(&work))
        return 1;

    memset(&opts, 0, sizeof(opts));
    opts.op = CHILD_OP_STORE;
    opts.use_disk = 1;
    opts.entries = 1;
    opts.value_base = 0x1111;
    opts.identifier = 0x55;
    opts.payload_size = 16;
    opts.with_working_dir_flag = TRUE;
    opts.allow_already_exists = TRUE;
    strcpy(opts.working_dir, work.root);
    strcpy(opts.key_prefix, "dup");

    for (i = 0; i < 8; ++i)
    {
        children[i].exe = exe;
        children[i].opts = &opts;
        children[i].exit_code = 1;
        children[i].spawn_result = 1;
        threads[i] = CreateThread(NULL, 0, spawn_child_worker, &children[i], 0, NULL);
        if (!threads[i])
        {
            if (i)
                WaitForMultipleObjects(i, threads, TRUE, INFINITE);
            while (i)
                CloseHandle(threads[--i]);
            workdir_cleanup(&work);
            return 1;
        }
    }
    WaitForMultipleObjects(8, threads, TRUE, INFINITE);

    for (i = 0; i < 8; ++i)
    {
        CloseHandle(threads[i]);
        if (children[i].spawn_result)
        {
            workdir_cleanup(&work);
            return 1;
        }
        if (children[i].exit_code == 0)
            ++success;
        else if (children[i].exit_code == 3)
            ++already_exists;
    }

    if (!success || (success + already_exists) != 8)
    {
        workdir_cleanup(&work);
        return 1;
    }
    printf("[scenario] cross-process-duplicate-writers success=%u already_exists=%u\n", success, already_exists);

    for (i = 0; i < 8; ++i)
        printf("[child] crossdup code=%u\n", children[i].exit_code);

    workdir_cleanup(&work);
    return 0;
}

static int run_cold_warm_new_process(void)
{
    struct workdir_ctx work;
    char exe[MAX_PATH];
    struct child_opts opts;
    unsigned int exit_code = 0;
    struct cache_file_entry *files = NULL;
    ULONGLONG disk_usage = 0;
    unsigned long long start;
    int ret = 1;

    if (!GetModuleFileNameA(NULL, exe, sizeof(exe)))
        return 1;
    if (!workdir_init(&work))
        return 1;

    memset(&opts, 0, sizeof(opts));
    opts.op = CHILD_OP_STORE;
    opts.use_disk = 1;
    opts.entries = 1;
    opts.value_base = 0x5000;
    opts.identifier = 0x12;
    opts.payload_size = 16;
    opts.with_working_dir_flag = TRUE;
    strcpy(opts.working_dir, work.root);
    strcpy(opts.key_prefix, "cold-warm");

    start = get_time_us();
    if (spawn_child(exe, &opts, &exit_code) || exit_code)
        goto done;
    printf("[metrics] scenario=disk-cold-store latency_ms=%.3f hits=0 misses=1 operations=1 ",
            (double)(get_time_us() - start) / 1000.0);
    discover_cache_files(work.root, &files, &disk_usage);
    printf("rss_peak=%llu disk_bytes=%llu\n",
            (unsigned long long)read_working_set_peak(), (unsigned long long)disk_usage);

    memset(&opts, 0, sizeof(opts));
    opts.op = CHILD_OP_FIND;
    opts.use_disk = 1;
    opts.entries = 1;
    opts.value_base = 0x5000;
    opts.identifier = 0x12;
    opts.payload_size = 16;
    opts.with_working_dir_flag = TRUE;
    opts.expect_hit = TRUE;
    strcpy(opts.working_dir, work.root);
    strcpy(opts.key_prefix, "cold-warm");

    start = get_time_us();
    if (spawn_child(exe, &opts, &exit_code) || exit_code)
        goto done;
    printf("[metrics] scenario=disk-warm-hit latency_ms=%.3f hits=1 misses=0 operations=1 ",
            (double)(get_time_us() - start) / 1000.0);
    printf("rss_peak=%llu disk_bytes=%llu\n",
            (unsigned long long)read_working_set_peak(), (unsigned long long)disk_usage);

    opts.op = CHILD_OP_FIND;
    opts.expect_miss = TRUE;
    strcpy(opts.key_prefix, "cold-warm-missing");

    start = get_time_us();
    if (spawn_child(exe, &opts, &exit_code) || exit_code)
        goto done;

    printf("[metrics] scenario=disk-warm-miss latency_ms=%.3f hits=0 misses=1 operations=1 ",
            (double)(get_time_us() - start) / 1000.0);
    printf("rss_peak=%llu disk_bytes=%llu\n",
            (unsigned long long)read_working_set_peak(), (unsigned long long)disk_usage);

    printf("[scenario] cold-store-and-warm-new-process verified\n");
    ret = 0;

done:
    free_cache_file_list(files);
    workdir_cleanup(&work);
    return ret;
}

static int run_forced_termination(ID3D12Device9 *device9)
{
    struct workdir_ctx work;
    char exe[MAX_PATH];
    struct child_opts opts;
    char flag_path[MAX_PATH * 2];
    struct cache_file_entry *files = NULL;
    ULONGLONG disk_before = 0;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session = NULL;
    unsigned long long start;
    STARTUPINFOA si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    char command[2048];
    HRESULT hr;
    int ret = 0;
    BOOL child_ready = FALSE, partial_seen = FALSE;

    if (!GetModuleFileNameA(NULL, exe, sizeof(exe)))
        return 1;
    if (!workdir_init(&work))
        return 1;

    memset(&opts, 0, sizeof(opts));
    opts.op = CHILD_OP_BIG_STORE;
    opts.use_disk = 1;
    opts.identifier = 0x77;
    opts.payload_size64 = 64 * 1024ULL * 1024ULL;
    opts.value_base = 0x6000;
    opts.with_working_dir_flag = TRUE;
    snprintf(opts.key_prefix, sizeof(opts.key_prefix), "%s", "termination");
    strcpy(opts.working_dir, work.root);

    snprintf(command, sizeof(command),
             "\"%s\" --child --op=big-store --mode=disk --working-dir=\"%s\" --identifier=%u --payload64=%llu --key-prefix=%s --use-working-dir",
             exe, opts.working_dir, opts.identifier, (unsigned long long)opts.payload_size64, opts.key_prefix);

    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        workdir_cleanup(&work);
        fail("Could not start termination child: %lu", GetLastError());
        return 1;
    }

    snprintf(flag_path, sizeof(flag_path), "%s\\switchyard_big_writer_ready.flag",
             opts.working_dir);
    for (start = GetTickCount64(); GetTickCount64() - start < 30000;)
    {
        if (GetFileAttributesA(flag_path) != INVALID_FILE_ATTRIBUTES)
        {
            child_ready = TRUE;
            break;
        }
        Sleep(100);
    }

    if (!child_ready)
        ret = 1;
    for (start = GetTickCount64(); child_ready && GetTickCount64() - start < 30000;)
    {
        DWORD child_status;

        free_cache_file_list(files);
        files = NULL;
        disk_before = 0;
        discover_cache_files(work.root, &files, &disk_before);
        if (cache_file_list_has_temp(files, 1024 * 1024))
        {
            partial_seen = TRUE;
            break;
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0
                || !GetExitCodeProcess(pi.hProcess, &child_status)
                || child_status != STILL_ACTIVE)
            break;
        Sleep(1);
    }
    if (!partial_seen)
        ret = 1;
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT)
    {
        if (!TerminateProcess(pi.hProcess, 1))
            ret = 1;
    }
    else
    {
        /* Completing before the interrupt is not evidence of crash recovery. */
        ret = 1;
    }
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    init_cache_desc(&desc, opts.identifier, D3D12_SHADER_CACHE_MODE_DISK, 1, TRUE);
    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
    {
        workdir_cleanup(&work);
        return 1;
    }

    start = get_time_us();
    {
        unsigned int observed = 0;

        hr = find_entry(session, opts.key_prefix, (unsigned int)opts.payload_size64,
                opts.value_base, TRUE, &observed);
        if (hr != DXGI_ERROR_NOT_FOUND)
            ret = 1;
    }
    printf("[scenario] termination-reopen hr=%s latency_ms=%.3f\n", hresult_name(hr),
           (double)(get_time_us() - start) / 1000.0);
    free_cache_file_list(files);
    files = NULL;
    disk_before = 0;
    discover_cache_files(work.root, &files, &disk_before);
    if (cache_file_list_has_temp(files, 0))
        ret = 1;

    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);

    free_cache_file_list(files);
    workdir_cleanup(&work);
    return ret;
}

static int run_permission_failure(ID3D12Device9 *device9)
{
    struct workdir_ctx work;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session = NULL;
    char blocker[MAX_PATH * 2];
    HANDLE file;
    HRESULT hr;

    if (!workdir_init(&work))
        return 1;

    snprintf(blocker, sizeof(blocker), "%s\\vkd3d-cache", work.root);
    file = CreateFileA(blocker, GENERIC_WRITE, 0, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        workdir_cleanup(&work);
        return 1;
    }
    CloseHandle(file);

    init_cache_desc(&desc, 0x66, D3D12_SHADER_CACHE_MODE_DISK, 1, TRUE);
    hr = create_cache_session(device9, &desc, &session);
    if (SUCCEEDED(hr))
    {
        ID3D12ShaderCacheSession_Release(session);
        DeleteFileA(blocker);
        workdir_cleanup(&work);
        return 1;
    }
    printf("[scenario] disk-permission/path-type failure=%s\n", hresult_name(hr));
    DeleteFileA(blocker);
    workdir_cleanup(&work);
    return 0;
}

static int run_delete_on_destroy(ID3D12Device9 *device9)
{
    struct workdir_ctx work;
    ID3D12ShaderCacheSession *session = NULL;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    HRESULT hr;
    struct cache_file_entry *files = NULL;
    ULONGLONG before = 0;
    ULONGLONG after = 0;
    UINT value_size;

    if (!workdir_init(&work))
        return 1;

    discover_cache_files(work.root, &files, &before);
    init_cache_desc(&desc, 0x88, D3D12_SHADER_CACHE_MODE_DISK, 1, TRUE);
    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
    {
        workdir_cleanup(&work);
        return 1;
    }
    hr = ID3D12ShaderCacheSession_StoreValue(session, "delete-me", 10, "x", 1);
    if (FAILED(hr))
    {
        ID3D12ShaderCacheSession_Release(session);
        workdir_cleanup(&work);
        return 1;
    }

    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);

    discover_cache_files(work.root, &files, &after);
    printf("[scenario] delete-on-destroy disk_before=%llu disk_after=%llu\n", before, after);

    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
    {
        workdir_cleanup(&work);
        free_cache_file_list(files);
        return 1;
    }
    value_size = 0;
    hr = ID3D12ShaderCacheSession_FindValue(session,
            "delete-me", 10, NULL, &value_size);
    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);

    workdir_cleanup(&work);
    free_cache_file_list(files);
    return hr == DXGI_ERROR_NOT_FOUND ? 0 : 1;
}

static int run_version_invalidation(ID3D12Device9 *device9)
{
    struct workdir_ctx work;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session = NULL;
    HRESULT hr;
    unsigned int observed;
    int ret = 1;

    if (!workdir_init(&work))
        return 1;
    init_cache_desc(&desc, 0x99, D3D12_SHADER_CACHE_MODE_DISK, 1, TRUE);
    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
        goto done;
    hr = store_entry(session, "version", 16, 0, 0x8000);
    if (FAILED(hr))
        goto done;
    ID3D12ShaderCacheSession_Release(session);
    session = NULL;

    init_cache_desc(&desc, 0x99, D3D12_SHADER_CACHE_MODE_DISK, 2, TRUE);
    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
        goto done;

    hr = find_entry(session, "version", 16, 0x8000, TRUE, &observed);
    ID3D12ShaderCacheSession_Release(session);
    session = NULL;
    if (hr == S_OK)
    {
        printf("[scenario] version-invalidation failed: stale value still present\n");
        goto done;
    }
    printf("[scenario] version-invalidation miss=%s\n", hresult_name(hr));

    init_cache_desc(&desc, 0x99, D3D12_SHADER_CACHE_MODE_DISK, 1, TRUE);
    hr = create_cache_session(device9, &desc, &session);
    if (FAILED(hr))
        goto done;
    hr = find_entry(session, "version", 16, 0x8000, TRUE, &observed);
    printf("[scenario] version-reopen-v1 %s\n", hresult_name(hr));
    ret = hr == DXGI_ERROR_NOT_FOUND ? 0 : 1;

done:
    if (session)
    {
        ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
        ID3D12ShaderCacheSession_Release(session);
    }
    workdir_cleanup(&work);
    return ret;
}

static int parse_args(int argc, char **argv, struct child_opts *opts)
{
    int i;

    for (i = 1; i < argc; ++i)
    {
        const char *arg = argv[i];

        if (!strcmp(arg, "--child"))
            continue;

        if (!strncmp(arg, "--op=", 5))
        {
            if (!strcmp(arg + 5, "store")) opts->op = CHILD_OP_STORE;
            else if (!strcmp(arg + 5, "find")) opts->op = CHILD_OP_FIND;
            else if (!strcmp(arg + 5, "big-store")) opts->op = CHILD_OP_BIG_STORE;
            else return 1;
        }
        else if (!strcmp(arg, "--expect-miss"))
            opts->expect_miss = TRUE;
        else if (!strcmp(arg, "--expect-hit"))
            opts->expect_hit = TRUE;
        else if (!strcmp(arg, "--allow-exists"))
            opts->allow_already_exists = TRUE;
        else if (!strcmp(arg, "--use-working-dir"))
            opts->with_working_dir_flag = TRUE;
        else if (!strncmp(arg, "--mode=", 7))
            opts->use_disk = !strcmp(arg + 7, "disk");
        else if (!strncmp(arg, "--working-dir=", 13))
            strncpy(opts->working_dir, arg + 13, sizeof(opts->working_dir) - 1);
        else if (!strncmp(arg, "--identifier=", 13))
            opts->identifier = strtoul(arg + 13, NULL, 0);
        else if (!strncmp(arg, "--entries=", 10))
            opts->entries = strtoul(arg + 10, NULL, 0);
        else if (!strncmp(arg, "--value-base=", 12))
            opts->value_base = strtoul(arg + 12, NULL, 0);
        else if (!strncmp(arg, "--payload=", 10))
            opts->payload_size = strtoul(arg + 10, NULL, 0);
        else if (!strncmp(arg, "--payload64=", 11))
            opts->payload_size64 = strtoull(arg + 11, NULL, 0);
        else if (!strncmp(arg, "--wait=", 7))
            opts->wait_mark_ms = strtoul(arg + 7, NULL, 0);
        else if (!strncmp(arg, "--key-prefix=", 12))
            strncpy(opts->key_prefix, arg + 12, sizeof(opts->key_prefix) - 1);
        else if (!strncmp(arg, "--key=", 6))
            strncpy(opts->key, arg + 6, sizeof(opts->key) - 1);
        else
            return 1;
    }

    if (opts->payload_size64 == 0)
        opts->payload_size = opts->payload_size ? opts->payload_size : 4;

    return 0;
}

int main(int argc, char **argv)
{
    struct child_opts child = {0};
    int i, ret = 0;
    ID3D12Device *device = NULL;
    ID3D12Device9 *device9 = NULL;
    struct scenario_metrics memory_metric = {0};
    struct scenario_metrics throughput_metric = {0};
    BOOL is_child = FALSE;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    for (i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--child"))
        {
            is_child = TRUE;
            break;
        }
    }

    if (is_child)
    {
        if (parse_args(argc, argv, &child))
            return 1;
        return run_child(&child);
    }

    if (!create_device9(&device, &device9))
        return 77;

    if (run_memory_mode_check(device9, &memory_metric))
        ret = 1;
    print_metrics(&memory_metric, "memory-mode");

    if (run_throughput(device9, &throughput_metric))
        ret = 1;
    print_metrics(&throughput_metric, "threads=8");

    if (run_ten_k_entries(device9))
        ret = 1;
    if (run_cold_warm_new_process())
        ret = 1;
    if (run_cross_process_duplicate())
        ret = 1;
    if (run_forced_termination(device9))
        ret = 1;
    if (verify_corrupted_isolation(device9))
        ret = 1;
    if (verify_finalized_entry_write_exclusion(device9))
        ret = 1;
    if (run_hash_collision_fixture(device9))
        ret = 1;
    if (run_delete_on_destroy(device9))
        ret = 1;
    if (run_version_invalidation(device9))
        ret = 1;
    if (run_permission_failure(device9))
        ret = 1;

    if (device9)
        ID3D12Device9_Release(device9);
    if (device)
        ID3D12Device_Release(device);

    printf("[summary] d3d12_shader_cache_probe ret=%d\n", ret);
    return ret;
}
