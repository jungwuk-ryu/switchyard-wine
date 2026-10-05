#!/usr/bin/env python3
"""Run the actual Darwin native-call/x18 policy with ABI and signal canaries."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root / "dlls/ntdll/unix/signal_arm64.c").read_text()


def region(start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise SystemExit("native-call extraction anchors changed")
    return source[source.index(start):source.index(end)]


mode = region("#if defined(HAVE_OS_CUSTOM_X18_ABI) && \\\n",
              "\n#endif\n\n#define NTDLL_DWARF_H_NO_UNWINDER")
bridge = region("/* Unlike a general Unix call", "\n#endif\n\n__ASM_GLOBAL_FUNC( __wine_unix_call_dispatcher,")
signal = region("typedef void (*signal_handler_func)( int signal, siginfo_t *siginfo, void *sigcontext );",
                "\n#define DEFINE_SYSTEM_X18_SIGNAL_WRAPPER")
fixture = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ucontext.h>
#include <os/arch/arm64.h>
#include "wine/asm.h"
typedef int BOOL;
typedef uint8_t BOOLEAN;
typedef uint64_t UINT64;
typedef uintptr_t ULONG_PTR;
typedef uint32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *);
#define TRUE 1
#define FALSE 0
#define HAVE_OS_CUSTOM_X18_ABI 1
#define STATUS_INVALID_DEVICE_STATE 0xc0000184u
#define REGn_sig(n,c) ((c)->uc_mcontext->__ss.__x[n])
#define PC_sig(c) ((c)->uc_mcontext->__ss.__pc)
#define SP_sig(c) ((c)->uc_mcontext->__ss.__sp)
typedef struct { BOOLEAN InSimulation; void *SuspendDoorbell; } CHPE_V2_CPU_AREA_INFO;
struct teb { CHPE_V2_CPU_AREA_INFO *ChpeV2CpuAreaInfo; };
struct thread_data { struct teb *teb; };
struct syscall_frame { ULONG_PTR sp, pc, x[29]; };
static _Thread_local struct thread_data current;
static struct thread_data *get_thread_data(void) { return &current; }
static BOOL is_arm64ec(void) { return TRUE; }
static struct syscall_frame *get_syscall_frame(struct thread_data *data)
{ (void)data; return NULL; }
static BOOL recover_lost_custom_x18_fault(struct thread_data *data, ucontext_t *ctx, siginfo_t *info)
{ (void)data; (void)ctx; (void)info; _exit(98); }
static void usr2_handler(int sig, siginfo_t *info, void *ctx)
{ (void)sig; (void)info; (void)ctx; _exit(98); }
static void *pKiUserExceptionDispatcher, *pKiUserEmulationDispatcher;
static atomic_uint traps, calls;
'''
tests = r'''
static void trap(int sig, siginfo_t *info, void *opaque)
{
    (void)info;
    ucontext_t *ctx = opaque;
    if (sig != SIGTRAP || custom_x18_abi_enabled() ||
        *(const uint32_t *)PC_sig(ctx) != 0xd420fc00u) _exit(97);
    PC_sig(ctx) += 4;
    atomic_fetch_add_explicit(&traps, 1, memory_order_relaxed);
}
static void wrapped_trap(int sig, siginfo_t *info, void *ctx)
{ dispatch_signal_with_system_x18(trap, sig, info, ctx); }
static NTSTATUS callback(void *args)
{
    if (custom_x18_abi_enabled() || !pthread_self() || args != &current) _exit(96);
    atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed);
    __asm__ volatile("msr fpcr, xzr\n\tmsr fpsr, xzr\n\tbrk #0x7e0");
    return 0x12345678;
}
struct observed { uint64_t gpr[10], fpr[8], fpcr, fpsr, teb, sp_delta, custom; };
static NTSTATUS __attribute__((naked)) probe(unixlib_entry_t entry, void *args,
                                            UINT64 teb, struct observed *out)
{
    __asm__("sub sp, sp, #224\n\t"
            "stp x19, x20, [sp]\n\tstp x21, x22, [sp, #16]\n\t"
            "stp x23, x24, [sp, #32]\n\tstp x25, x26, [sp, #48]\n\t"
            "stp x27, x28, [sp, #64]\n\tstp x29, x30, [sp, #80]\n\t"
            "stp d8, d9, [sp, #96]\n\tstp d10, d11, [sp, #112]\n\t"
            "stp d12, d13, [sp, #128]\n\tstp d14, d15, [sp, #144]\n\t"
            "stp x0, x1, [sp, #160]\n\tstp x2, x3, [sp, #176]\n\t"
            "mrs x4, fpcr\n\tmrs x5, fpsr\n\tstp x4, x5, [sp, #192]\n\t"
            "bl _enter_windows_x18_abi\n\tldr x18, [sp, #176]\n\t"
            "mov x19, #0x119\n\tmov x20, #0x120\n\tmov x21, #0x121\n\t"
            "mov x22, #0x122\n\tmov x23, #0x123\n\tmov x24, #0x124\n\t"
            "mov x25, #0x125\n\tmov x26, #0x126\n\tmov x27, #0x127\n\t"
            "mov x28, #0x128\n\tmov x29, sp\n\t"
            "mov x4, #0xabc\n\tfmov d8, x4\n\tfmov d9, x4\n\t"
            "fmov d10, x4\n\tfmov d11, x4\n\tfmov d12, x4\n\t"
            "fmov d13, x4\n\tfmov d14, x4\n\tfmov d15, x4\n\t"
            "mov x4, #0x400000\n\tmsr fpcr, x4\n\t"
            "mov x4, #0x8000000\n\tmsr fpsr, x4\n\t"
            "ldp x0, x1, [sp, #160]\n\tldr x2, [sp, #176]\n\t"
            "bl ___wine_arm64ec_native_call_v1\n\tstr w0, [sp, #208]\n\t"
            "ldr x3, [sp, #184]\n\t"
            "stp x19, x20, [x3]\n\tstp x21, x22, [x3, #16]\n\t"
            "stp x23, x24, [x3, #32]\n\tstp x25, x26, [x3, #48]\n\t"
            "stp x27, x28, [x3, #64]\n\t"
            "stp d8, d9, [x3, #80]\n\tstp d10, d11, [x3, #96]\n\t"
            "stp d12, d13, [x3, #112]\n\tstp d14, d15, [x3, #128]\n\t"
            "mrs x4, fpcr\n\tmrs x5, fpsr\n\tstp x4, x5, [x3, #144]\n\t"
            "str x18, [x3, #160]\n\tmov x4, sp\n\tsub x4, x4, x29\n\t"
            "str x4, [x3, #168]\n\tmrs x4, TPIDR_EL0\n\t"
            "ubfx x4, x4, #48, #1\n\tstr x4, [x3, #176]\n\t"
            "bl _enter_system_x18_abi\n\t"
            "ldp x4, x5, [sp, #192]\n\tmsr fpcr, x4\n\tmsr fpsr, x5\n\t"
            "ldp x19, x20, [sp]\n\tldp x21, x22, [sp, #16]\n\t"
            "ldp x23, x24, [sp, #32]\n\tldp x25, x26, [sp, #48]\n\t"
            "ldp x27, x28, [sp, #64]\n\tldp x29, x30, [sp, #80]\n\t"
            "ldp d8, d9, [sp, #96]\n\tldp d10, d11, [sp, #112]\n\t"
            "ldp d12, d13, [sp, #128]\n\tldp d14, d15, [sp, #144]\n\t"
            "ldr w0, [sp, #208]\n\tadd sp, sp, #224\n\tret");
}
static int check_probe(unixlib_entry_t entry, void *args, uint64_t teb, NTSTATUS expected)
{
    struct observed out;
    NTSTATUS status = probe(entry, args, teb, &out);
    const uint64_t regs[] = {0x119,0x120,0x121,0x122,0x123,0x124,0x125,0x126,0x127,0x128};
    if (status != expected || out.teb != teb || out.sp_delta || out.custom != 1 ||
        out.fpcr != 0x400000 || out.fpsr != 0x8000000 || custom_x18_abi_enabled()) return 1;
    for (unsigned i = 0; i < 10; ++i) if (out.gpr[i] != regs[i]) return 1;
    for (unsigned i = 0; i < 8; ++i) if (out.fpr[i] != 0xabc) return 1;
    return 0;
}
static void *worker(void *unused)
{
    (void)unused;
    CHPE_V2_CPU_AREA_INFO cpu = {1, &cpu};
    struct teb teb = {&cpu};
    current.teb = &teb;
    for (unsigned i = 0; i < 50; ++i)
    {
        if (check_probe(callback, &current, (uintptr_t)&teb, 0x12345678)) return (void *)1;
        if (check_probe(callback, &current, (uintptr_t)&teb + 16, STATUS_INVALID_DEVICE_STATE)) return (void *)1;
        if (check_probe(NULL, &current, (uintptr_t)&teb, STATUS_INVALID_DEVICE_STATE)) return (void *)1;
        if (check_probe(callback, NULL, (uintptr_t)&teb, STATUS_INVALID_DEVICE_STATE)) return (void *)1;
        cpu.InSimulation = 0;
        if (check_probe(callback, &current, (uintptr_t)&teb, STATUS_INVALID_DEVICE_STATE)) return (void *)1;
        cpu.InSimulation = 1;
        cpu.SuspendDoorbell = NULL;
        if (check_probe(callback, &current, (uintptr_t)&teb, STATUS_INVALID_DEVICE_STATE)) return (void *)1;
        cpu.SuspendDoorbell = &cpu;
    }
    return NULL;
}
int main(void)
{
    alarm(20);
    if (!init_custom_x18_abi()) return 2;
    struct sigaction action = {.sa_sigaction = wrapped_trap, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTRAP, &action, NULL)) return 2;
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) if (pthread_create(&threads[i], NULL, worker, NULL)) return 2;
    for (unsigned i = 0; i < 4; ++i) {
        void *result;
        if (pthread_join(threads[i], &result) || result) return 1;
    }
    if (calls != 200 || traps < 200) return 1;
    printf("NATIVE_CALL_PASS calls=%u signals=%u\n", calls, traps);
    return 0;
}
'''

instrumented_lines = []
for line in bridge.splitlines():
    instrumented_lines.append(line)
    assembly = line.lstrip()
    # Include composed BL lines as well: the return from enter_windows_x18_abi
    # BEFORE reloading Windows x18 is a distinct, essential signal window.
    if (assembly.startswith('"') and assembly.endswith('"') and "\\n\\t" in assembly
            and not assembly.startswith(('"ret', '"1:'))):
        instrumented_lines.append('                   "brk #0x7e0\\n\\t"')
instrumented = "\n".join(instrumented_lines) + "\n"
if instrumented == bridge:
    raise SystemExit("no bridge instruction windows instrumented")
with tempfile.TemporaryDirectory(prefix="wine-native-call-") as directory:
    work = Path(directory).resolve()
    cc = shlex.split(os.environ.get("CC", "/usr/bin/clang"))
    for name, implementation, expected in (
        ("current", bridge, 0),
        ("signal-windows", instrumented, 0),
        ("fp-negative", bridge.replace('"msr fpcr, x9\\n\\t"', '"nop\\n\\t"'), 1),
        ("teb-negative", bridge.replace("(ULONG_PTR)data->teb != authenticated_teb", "(authenticated_teb && FALSE)"), 1),
        ("return-mode-negative", bridge.replace('"bl " __ASM_NAME("enter_windows_x18_abi") "\\n\\t"', '"nop\\n\\t"'), 1),
    ):
        path, binary = work / (name + ".c"), work / name
        path.write_text(fixture + mode + signal + implementation + tests)
        subprocess.run(cc + ["-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-mmacosx-version-min=26.5", "-I" + str(root / "include"), str(path), "-o", str(binary)], check=True)
        signing = r'''
set -euo pipefail
source "$1/switchyard/lib/macho_signing.sh"
test_entitlements_fd=
trap 'if [[ -n ${test_entitlements_fd:-} ]]; then close_validated_entitlements_snapshot "$test_entitlements_fd"; fi' EXIT
create_validated_entitlements_snapshot preview-native-arm64-fex "$1/switchyard/wine-runtime-native-arm64.entitlements" "$2" test_entitlements_fd
sign_engineering_macho_atomically /usr/bin/codesign preview-native-arm64-fex "$3" "$test_entitlements_fd"
verify_macho_entitlements_snapshot /usr/bin/codesign preview-native-arm64-fex "$3" "$test_entitlements_fd"
'''
        subprocess.run(["/bin/bash", "-c", signing, "native-call-sign", str(root), str(work), str(binary)],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=25)
        if result.returncode != expected:
            raise SystemExit(f"{name}: exit {result.returncode}, expected {expected}: {result.stdout}{result.stderr}")
        print(f"{name}: {result.stdout.strip() or 'expected failure'}")
