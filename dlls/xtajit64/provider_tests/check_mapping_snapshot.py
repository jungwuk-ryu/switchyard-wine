#!/usr/bin/env python3
"""Compile actual snapshot collection/restart code with ordered view removal."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("static NTSTATUS collect_existing_mappings(")
end = source.index("static NTSTATUS synchronize_mapping_window(", start)
actual = source[start:end]
fixture = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t NTSTATUS, ULONG;
typedef uintptr_t ULONG_PTR;
typedef size_t SIZE_T;
#define STATUS_SUCCESS 0
#define STATUS_NOT_MAPPED_VIEW 0xc0000019u
#define STATUS_INVALID_ADDRESS 0xc0000141u
#define STATUS_INVALID_PARAMETER 0xc000000du
#define STATUS_ACCESS_DENIED 0xc0000022u
#define STATUS_NO_MEMORY 0xc0000017u
#define STATUS_RETRY 0xc000022du
#define MEM_COMMIT 0x1000
#define MEM_FREE 0x10000
#define MemoryBasicInformation 0
#define SystemBasicInformation 0
#define XTAJIT64_GUEST_PAGE_SIZE 4096
#define XTAJIT64_MAX_HOST_PAGE_SIZE 65536
#define XTAJIT64_GUEST_KUSER 0x7ffe0000
#define XTAJIT64_MAX_RESYNC_ATTEMPTS 8
#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define TRACE(...) do {} while (0)
#define check(e) do { if (!(e)) { fprintf(stderr, "snapshot check line %d: %s\n", __LINE__, #e); exit(1); } } while (0)
typedef struct { void *BaseAddress, *AllocationBase; SIZE_T RegionSize; ULONG State, Protect; } MEMORY_BASIC_INFORMATION;
typedef struct { ULONG PageSize; void *LowestUserAddress, *HighestUserAddress; } SYSTEM_BASIC_INFORMATION;
struct xtajit64_memory_params { uint64_t guest, host, size, allocation_base; ULONG protect, flags; };
struct xtajit64_memory_resync_params { uint64_t ranges, generation; ULONG count, reserved; };
struct xtajit64_memory_resync_begin_params { uint64_t generation; };
struct mapping_snapshot { struct xtajit64_memory_params *ranges; ULONG count, capacity; };
static struct xtajit64_memory_params storage[8];
static struct mapping_snapshot resync_snapshot = { storage, 0, 8 };
static int resync_snapshot_lock, held, attempts, commits, removes, skipped;
static NTSTATUS query_failure;
static void *GetCurrentProcess(void) { return NULL; }
static void RtlAcquireSRWLockExclusive(int *lock) { (void)lock; check(!held); held = 1; }
static void RtlReleaseSRWLockExclusive(int *lock) { (void)lock; check(held); held = 0; }
static NTSTATUS NtQuerySystemInformation(int kind, SYSTEM_BASIC_INFORMATION *out, SIZE_T size, void *length)
{ (void)kind; (void)size; (void)length; *out = (SYSTEM_BASIC_INFORMATION){4096, (void *)0x10000, (void *)0x12fff}; return 0; }
static NTSTATUS NtQueryVirtualMemory(void *process, void *cursor, int kind, MEMORY_BASIC_INFORMATION *out, SIZE_T size, void *length)
{
    (void)process; (void)kind; (void)size; (void)length;
    check(held);
    *out = (MEMORY_BASIC_INFORMATION){cursor, cursor, 4096, MEM_COMMIT, 4};
    /* The second range was committed in attempt1, but got unmapped before
     * describe_host_mapping. The next authoritative query sees it as free. */
    if ((uintptr_t)cursor == 0x11000 && attempts > 1 && removes == 1)
        out->State = MEM_FREE;
    return 0;
}
static NTSTATUS describe_host_mapping(ULONG_PTR start, SIZE_T size, ULONG_PTR base, ULONG protect,
                                     struct xtajit64_memory_params *out)
{
    if (start == 0x11000)
    {
        if (query_failure) return query_failure;
        if (removes) return STATUS_NOT_MAPPED_VIEW;
        if (skipped) return STATUS_ACCESS_DENIED;
    }
    *out = (struct xtajit64_memory_params){start, start, size, base, protect, 0};
    return 0;
}
static NTSTATUS append_mapping_snapshot(struct mapping_snapshot *snapshot, const struct xtajit64_memory_params *params)
{ check(snapshot->count < snapshot->capacity); snapshot->ranges[snapshot->count++] = *params; return 0; }
static NTSTATUS memory_resync_begin(struct xtajit64_memory_resync_begin_params *params)
{ params->generation = ++attempts; return 0; }
static NTSTATUS memory_resync(struct xtajit64_memory_resync_params *params)
{
    struct xtajit64_memory_params *ranges = (void *)(uintptr_t)params->ranges;
    ++commits;
    check(params->generation == (uint64_t)attempts);
    check(params->count == (removes || skipped ? 2u : 3u));
    check(ranges[0].guest == 0x10000 && ranges[params->count - 1].guest == 0x12000);
    return 0;
}
#define XTAJIT64_CALL(name,args) name(args)
'''
tests = r'''
static void reset(void) { check(!held && !resync_snapshot.count); attempts = commits = removes = skipped = query_failure = 0; }
int main(void)
{
    check(!resync_existing_mappings() && attempts == 1 && commits == 1);
    reset(); removes = 1;
    check(!resync_existing_mappings() && attempts == 2 && commits == 1);
    reset(); removes = 2;
    check(resync_existing_mappings() == STATUS_RETRY && attempts == 8 && !commits);
    reset(); query_failure = STATUS_NO_MEMORY;
    check(resync_existing_mappings() == STATUS_NO_MEMORY && attempts == 1 && !commits);
    reset(); query_failure = STATUS_INVALID_ADDRESS;
    check(resync_existing_mappings() == STATUS_INVALID_ADDRESS && attempts == 1 && !commits);
    reset(); skipped = 1;
    check(!resync_existing_mappings() && attempts == 1 && commits == 1);
    reset();
    puts("MAPPING_SNAPSHOT_PASS ordered-unmap partial-discard bounded-restart stable-errors low-owner");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="xtajit64-mapping-snapshot-") as directory:
    root = Path(directory)
    compiler = shlex.split(os.environ.get("CC", "cc"))
    for name, implementation, expected in (
        ("current", actual, 0),
        ("old-fatal-view-loss", actual.replace(
            "if (status == STATUS_NOT_MAPPED_VIEW) return STATUS_RETRY;", ""), 1),
        ("stale-partial-snapshot", actual.replace(
            "        snapshot->count = 0;\n        /* A concurrent", "        /* A concurrent"), 1),
    ):
        path, binary = root / (name + ".c"), root / name
        path.write_text(fixture + implementation + tests, encoding="utf-8")
        subprocess.run(compiler + ["-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=undefined", "-fno-sanitize-recover=all", str(path), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        if result.returncode != expected:
            raise SystemExit(name + ": " + result.stdout + result.stderr)
        print(name + ": " + (result.stdout.strip() or "expected rejection"))
