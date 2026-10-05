#!/usr/bin/env python3
"""Prove PE cleanup cannot outrun a failed native provider detach."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text()
start = source.index("void WINAPI ThreadTerm(")
end = source.index("\nvoid WINAPI UpdateProcessorInformation", start)
term = source[start:end]
guard_start = term.index("    if (__wine_unixlib_handle && (status = XTAJIT64_CALL( thread_term, NULL )))")
guard_end = term.index("    if (teb_allocation != native_stack_allocation", guard_start)
guard = term[guard_start:guard_end]
if guard_end > term.index("unregister_thread_teb_window("):
    raise SystemExit("native detach does not precede mapping cleanup")
fixture = r'''
#include <stddef.h>
#include <stdint.h>
typedef uint32_t NTSTATUS;
static int __wine_unixlib_handle, calls, poisoned, released;
static NTSTATUS result, poison_status;
static NTSTATUS call(void *args) { (void)args; ++calls; return result; }
#define XTAJIT64_CALL(name,args) call(args)
static void poison_provider(const char *where, NTSTATUS status)
{ (void)where; ++poisoned; poison_status = status; }
static void term(void)
{
    NTSTATUS status;
'''
tests = r'''
    ++released; /* first unregister/free after the production guard */
}
int main(void)
{
    __wine_unixlib_handle = 0; result = 0xc000009eu;
    term();
    if (calls || poisoned || released != 1) return 1;
    __wine_unixlib_handle = 1; result = 0;
    term();
    if (calls != 1 || poisoned || released != 2) return 1;
    result = 0xc000009eu;
    term();
    if (calls != 2 || poisoned != 1 || released != 2 || poison_status != result) return 1;
    result = 0xc0000184u;
    term();
    if (calls != 3 || poisoned != 2 || released != 2 || poison_status != result) return 1;
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="xtajit-detach-ownership-") as directory:
    work = Path(directory)
    for name, code, expected in (
        ("current", guard, 0),
        ("ignored-error-negative", guard.replace("        return;", "        /* ignored detach failure */"), 1),
    ):
        path, binary = work / (name + ".c"), work / name
        path.write_text(fixture + code + tests)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + ["-std=c11", "-Wall", "-Wextra", "-Werror",
            str(path), "-o", str(binary)], check=True)
        if subprocess.run([str(binary)], timeout=5).returncode != expected:
            raise SystemExit(name + ": unexpected result")
print("PE detach ownership: success, absent provider, retained failures and negative control passed")
