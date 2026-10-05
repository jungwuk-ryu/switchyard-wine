/* SPDX-License-Identifier: MIT */
#include "switchyard_fex.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check(value) do { if (!(value)) { fprintf(stderr, "window C contract line %u\n", __LINE__); exit(97); } } while (0)
#define WORKERS 8u
#define ITERATIONS 10000u

static struct switchyard_fex_thread *shared_thread;
static unsigned char expected[SWITCHYARD_FEX_REGISTER_WINDOW_DATA_SIZE_V1];
static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
static unsigned int ready;
static int started;
static atomic_uint completed, busy;

static struct switchyard_fex_register_window window_for(void *data)
{
    struct switchyard_fex_register_window window = {
        .size = sizeof(window), .version = SWITCHYARD_FEX_REGISTER_WINDOW_VERSION,
        .data = (uintptr_t)data, .data_size = SWITCHYARD_FEX_REGISTER_WINDOW_DATA_SIZE_V1,
    };
    return window;
}

static void *worker(void *opaque)
{
    unsigned char input[sizeof(expected)], output[sizeof(expected)];
    struct switchyard_fex_register_window in = window_for(input), out = window_for(output);
    unsigned int index;
    (void)opaque;
    memcpy(input, expected, sizeof(input));
    check(!pthread_mutex_lock(&start_mutex));
    ++ready;
    check(!pthread_cond_broadcast(&start_cond));
    while (!started) check(!pthread_cond_wait(&start_cond, &start_mutex));
    check(!pthread_mutex_unlock(&start_mutex));
    for (index = 0; index < ITERATIONS; ++index)
    {
        enum switchyard_fex_result result = switchyard_fex_thread_import_register_window(shared_thread, &in);
        check(result == SWITCHYARD_FEX_OK || result == SWITCHYARD_FEX_ERROR_BUSY);
        if (result == SWITCHYARD_FEX_ERROR_BUSY) atomic_fetch_add(&busy, 1);
        memset(output, 0xa5, sizeof(output));
        result = switchyard_fex_thread_export_register_window(shared_thread, &out);
        check(result == SWITCHYARD_FEX_OK || result == SWITCHYARD_FEX_ERROR_BUSY);
        if (result == SWITCHYARD_FEX_OK) check(!memcmp(expected, output, sizeof(output)));
        else
        {
            for (size_t byte = 0; byte < sizeof(output); ++byte) check(output[byte] == 0xa5);
            atomic_fetch_add(&busy, 1);
        }
        atomic_fetch_add(&completed, 1);
    }
    return NULL;
}

int main(void)
{
    struct switchyard_fex_config config = {
        .size = sizeof(config), .abi_version = SWITCHYARD_FEX_ABI_VERSION,
    };
    struct switchyard_fex_process *process = NULL;
    struct switchyard_fex_thread *thread = NULL;
    struct switchyard_fex_x64_state before, after;
    unsigned char data[SWITCHYARD_FEX_REGISTER_WINDOW_DATA_SIZE_V1 + 32], unchanged[sizeof(data)];
    struct switchyard_fex_register_window in, out, bad;
    _Alignas(struct switchyard_fex_register_window) unsigned char descriptor[sizeof(out) + 1];
    pthread_t workers[WORKERS];
    uint64_t flags = 0x202;
    uint32_t mxcsr = 0x1f80;
    unsigned int index, offset;

    check(switchyard_fex_abi_version() == SWITCHYARD_FEX_ABI_VERSION);
    check(switchyard_fex_process_create(&config, &process) == SWITCHYARD_FEX_OK);
    check(switchyard_fex_thread_create(process, &thread) == SWITCHYARD_FEX_OK);
    memset(expected, 0x73, sizeof(expected));
    memcpy(expected + SWITCHYARD_FEX_WINDOW_EFLAGS, &flags, sizeof(flags));
    memcpy(expected + SWITCHYARD_FEX_WINDOW_MXCSR, &mxcsr, sizeof(mxcsr));
    memset(expected + SWITCHYARD_FEX_WINDOW_RESERVED, 0, sizeof(uint32_t));
    in = window_for(expected);
    out = window_for(data + 1);
    memset(data, 0xa5, sizeof(data));
    memcpy(unchanged, data, sizeof(data));
    check(switchyard_fex_thread_export_register_window(thread, &out) == SWITCHYARD_FEX_ERROR_BUSY);
    check(!memcmp(data, unchanged, sizeof(data)));
    check(switchyard_fex_thread_import_register_window(thread, &in) == SWITCHYARD_FEX_OK);
    check(switchyard_fex_thread_export_register_window(thread, &out) == SWITCHYARD_FEX_OK);
    /* The reconstructed flag representation, not arbitrary raw EFLAGS bits. */
    memcpy(expected, data + 1, sizeof(expected));

    before.size = sizeof(before);
    before.version = SWITCHYARD_FEX_STATE_VERSION;
    check(switchyard_fex_thread_export_state(thread, &before) == SWITCHYARD_FEX_OK);
    for (index = 0; index < 10; ++index)
    {
        bad = out;
        switch (index)
        {
        case 0: bad.size--; break;
        case 1: bad.version++; break;
        case 2: bad.flags = 1; break;
        case 3: bad.reserved = 1; break;
        case 4: bad.data_size--; break;
        case 5: bad.data_size++; break;
        case 6: bad.data = 0; break;
        case 7: bad.data = UINT64_MAX - 406; break;
        case 8: bad.gs_base = 1; break;
        case 9: bad.data = (uintptr_t)thread; break;
        }
        memset(data, 0xa5, sizeof(data));
        memcpy(unchanged, data, sizeof(data));
        check(switchyard_fex_thread_export_register_window(thread, &bad) ==
              (index == 1 ? SWITCHYARD_FEX_ERROR_ABI_MISMATCH : SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT));
        check(!memcmp(data, unchanged, sizeof(data)));
        if (index != 8)
            check(switchyard_fex_thread_import_register_window(thread, &bad) ==
                  (index == 1 ? SWITCHYARD_FEX_ERROR_ABI_MISMATCH : SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT));
        after.size = sizeof(after);
        after.version = SWITCHYARD_FEX_STATE_VERSION;
        check(switchyard_fex_thread_export_state(thread, &after) == SWITCHYARD_FEX_OK);
        check(!memcmp(&before, &after, sizeof(before)));
    }
    bad = out;
    bad.data = (uintptr_t)&bad;
    check(switchyard_fex_thread_export_register_window(thread, &bad) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    check(switchyard_fex_thread_import_register_window(thread, &bad) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    check(switchyard_fex_thread_import_register_window(NULL, &in) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    check(switchyard_fex_thread_export_register_window(NULL, &out) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    check(switchyard_fex_thread_import_register_window(thread, NULL) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    check(switchyard_fex_thread_export_register_window(thread, NULL) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    memcpy(descriptor + 1, &out, sizeof(out));
    check(switchyard_fex_thread_import_register_window(thread,
          (const struct switchyard_fex_register_window *)(const void *)(descriptor + 1)) ==
          SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    check(switchyard_fex_thread_export_register_window(thread,
          (const struct switchyard_fex_register_window *)(const void *)(descriptor + 1)) ==
          SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);

    for (offset = 0; offset < 16; ++offset)
    {
        memset(data, 0xa5, sizeof(data));
        memcpy(data + offset, expected, sizeof(expected));
        in = window_for(data + offset);
        in.gs_base = UINT64_MAX;
        check(switchyard_fex_thread_import_register_window(thread, &in) == SWITCHYARD_FEX_OK);
        in.gs_base = 0;
        check(switchyard_fex_thread_export_register_window(thread, &in) == SWITCHYARD_FEX_OK);
        check(!memcmp(data + offset, expected, sizeof(expected)));
        for (index = 0; index < offset; ++index) check(data[index] == 0xa5);
        for (size_t byte = offset + sizeof(expected); byte < sizeof(data); ++byte) check(data[byte] == 0xa5);
    }
    shared_thread = thread;
    for (index = 0; index < WORKERS; ++index) check(!pthread_create(&workers[index], NULL, worker, NULL));
    check(!pthread_mutex_lock(&start_mutex));
    while (ready != WORKERS) check(!pthread_cond_wait(&start_cond, &start_mutex));
    started = 1;
    check(!pthread_cond_broadcast(&start_cond));
    check(!pthread_mutex_unlock(&start_mutex));
    for (index = 0; index < WORKERS; ++index) check(!pthread_join(workers[index], NULL));
    check(atomic_load(&completed) == WORKERS * ITERATIONS);
    check(switchyard_fex_thread_destroy(thread) == SWITCHYARD_FEX_OK);
    check(switchyard_fex_process_destroy(process) == SWITCHYARD_FEX_OK);
    printf("register_window_c result=pass geometry=10 alignment=16 concurrency=%u\n", atomic_load(&completed));
    return 0;
}
