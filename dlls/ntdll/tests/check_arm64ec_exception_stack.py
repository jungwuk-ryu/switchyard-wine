#!/usr/bin/env python3
"""Compile the actual ARM64EC exception-stack selector against boundary inputs."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("static BOOL get_active_x64_exception_stack(")
end = source.index("\nstatic void route_active_x64_exception", start)
selector = source[start:end]
fixture = r'''
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#define check(expr) do { if (!(expr)) { \
    fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
typedef int BOOL;
typedef unsigned char BOOLEAN;
typedef uintptr_t ULONG_PTR;
#define TRUE 1
#define FALSE 0
#define STATUS_ACCESS_VIOLATION 0xc0000005u
#define EXCEPTION_READ_FAULT 0
#define EXCEPTION_WRITE_FAULT 1
#define EXCEPTION_EXECUTE_FAULT 8
#define TRACE(...) ((void)0)
typedef struct { BOOLEAN InSimulation; } CHPE_V2_CPU_AREA_INFO;
struct teb { CHPE_V2_CPU_AREA_INFO *ChpeV2CpuAreaInfo;
             struct { void *StackLimit, *StackBase; } Tib; };
struct thread_data { struct teb *teb; void *jmp_buf; };
typedef struct { ULONG_PTR sp, pc; } ucontext_t;
typedef struct { unsigned ExceptionCode, NumberParameters;
                 ULONG_PTR ExceptionInformation[2]; } EXCEPTION_RECORD;
struct wine_arm64ec_jit_host_context_v1 { ULONG_PTR pc; };
struct wine_arm64ec_jit_signal_observer_v3 {
    void *context;
    int (*query_exception_stack)(void *, const struct wine_arm64ec_jit_host_context_v1 *,
                                uint32_t, uint64_t, uint64_t *);
};
static int arm64ec = 1, arm64ec_jit_signal_observer_registered = 1, calls;
static uint64_t queried_stack = 0x5008;
static BOOL is_arm64ec(void) { return arm64ec; }
static BOOL is_inside_syscall(struct thread_data *data, ULONG_PTR sp)
{ (void)data; return sp >= 0x9000 && sp < 0xa000; }
#define SP_sig(context) ((context)->sp)
static void capture_arm64ec_jit_host_context(struct wine_arm64ec_jit_host_context_v1 *host,
                                            ucontext_t *context) { host->pc = context->pc; }
static int query(void *opaque, const struct wine_arm64ec_jit_host_context_v1 *host,
                 uint32_t access, uint64_t address, uint64_t *out)
{
    (void)opaque; (void)access; (void)address; ++calls;
    if (host->pc != 0x1000) return 1; /* observer-authenticated JIT only */
    *out = queried_stack; return 0;
}
static struct wine_arm64ec_jit_signal_observer_v3 arm64ec_jit_signal_observer = {NULL, query};
'''
tests = r'''
int main(void)
{
    CHPE_V2_CPU_AREA_INFO cpu = {1};
    struct teb teb = {&cpu, {(void *)0x4000, (void *)0x6000}};
    struct thread_data data = {&teb, NULL};
    ucontext_t context = {0x9800, 0x1000};
    EXCEPTION_RECORD rec = {STATUS_ACCESS_VIOLATION, 2, {0, 0x2000}};
    ULONG_PTR out = 0;
    check(get_active_x64_exception_stack(&data, &context, &rec, &out) && out == 0x5008);
    context.sp = 0xb800; /* detached control stack, not the guest/syscall stack */
    out = 0;
    check(get_active_x64_exception_stack(&data, &context, &rec, &out) && out == 0x5008);
    context.pc = 0x2000; /* host compiler/helper is not reconstructible JIT */
    out = 0x777;
    check(!get_active_x64_exception_stack(&data, &context, &rec, &out) && out == 0x777);
    context.pc = 0x1000;
    calls = 0; data.jmp_buf = &data;
    check(!get_active_x64_exception_stack(&data, &context, &rec, &out) && !calls);
    data.jmp_buf = NULL;
    arm64ec_jit_signal_observer_registered = 0;
    check(!get_active_x64_exception_stack(&data, &context, &rec, &out) && !calls);
    arm64ec_jit_signal_observer_registered = 1;
    cpu.InSimulation = 0;
    check(!get_active_x64_exception_stack(&data, &context, &rec, &out) && !calls);
    cpu.InSimulation = 1;
    rec.NumberParameters = 1;
    check(!get_active_x64_exception_stack(&data, &context, &rec, &out) && !calls);
    rec.NumberParameters = 2; rec.ExceptionInformation[0] = 7;
    check(!get_active_x64_exception_stack(&data, &context, &rec, &out) && !calls);
    rec.ExceptionInformation[0] = 8;
    const uint64_t bad[] = {0, 0x3ff8, 0x5001, 0x6008, 0x9800};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        queried_stack = bad[i];
        check(!get_active_x64_exception_stack(&data, &context, &rec, &out) && out == 0x777);
    }
    queried_stack = 0x9800; teb.Tib.StackBase = (void *)0xa000;
    check(!get_active_x64_exception_stack(&data, &context, &rec, &out));
    check(!get_active_x64_exception_stack(NULL, &context, &rec, &out));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="arm64ec-exception-stack-") as directory:
    root = Path(directory)
    cc = shlex.split(os.environ.get("CC", "cc"))
    for name, implementation, success in (
        ("current", selector, True),
        ("syscall-only-negative", selector.replace("if (data->jmp_buf ||",
            "if (data->jmp_buf || !is_inside_syscall( data, SP_sig(context) ) ||", 1), False),
    ):
        path, binary = root / (name + ".c"), root / name
        path.write_text(fixture + implementation + tests, encoding="utf-8")
        subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode != (0 if success else 1):
            raise SystemExit(f"{name}: unexpected exit {result.returncode}: {result.stderr.decode()}")
print("ARM64EC exception-stack selector: detached/syscall provenance and negative control passed")
