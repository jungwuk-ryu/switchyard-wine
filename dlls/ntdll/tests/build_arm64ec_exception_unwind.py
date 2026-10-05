#!/usr/bin/env python3
"""Build a PE regression from the actual ARM64EC dispatcher and unwind selector.

Run the resulting PE under the intended ARM64EC Wine runtime. The optional
negative controls misinterpret the prepared context or omit return-PC adjustment.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--wine-build", required=True, type=Path)
parser.add_argument("--clang", required=True, type=Path)
parser.add_argument("--output", required=True, type=Path)
negative = parser.add_mutually_exclusive_group()
negative.add_argument("--negative-old-context", action="store_true")
negative.add_argument("--negative-return-pc", action="store_true")
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
source_path = repo / "dlls/ntdll/signal_arm64ec.c"
fixture = Path(__file__).with_name("arm64ec_exception_unwind_pe.c")
source = source_path.read_text(encoding="utf-8")


def region(start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise SystemExit("extraction anchors changed")
    return source[source.index(start):source.index(end)]


selector = region("static NTSTATUS virtual_unwind( ULONG type,",
                  "\n\n/**********************************************************************\n *           unwind_exception_handler")
selector = selector.replace("virtual_unwind(", "boundary_virtual_unwind(", 1)
if args.negative_return_pc:
    adjustment = "if (dispatch->ControlPcIsUnwound && RtlIsEcCode( pc )) pc -= 4;"
    if selector.count(adjustment) != 1:
        raise SystemExit("return-PC adjustment anchor changed")
    selector = selector.replace(adjustment, "/* Deliberately omit return-PC adjustment. */")
dispatcher = region("void __attribute__((naked)) KiUserExceptionDispatcher(",
                    "\n\n/*******************************************************************\n *\t\tKiUserApcDispatcher")
dispatcher = dispatcher.replace("KiUserExceptionDispatcher", "boundary_exception_dispatcher")
dispatcher = dispatcher.replace("prepare_exception_arm64ec", "boundary_prepare")
dispatcher = dispatcher.replace("#dispatch_exception", "#boundary_dispatch")
dispatcher = dispatcher.replace('asm( ', 'asm( ".globl boundary_dispatch_start\\n" '
                                '"boundary_dispatch_start:\\n" ', 1)
dispatcher = dispatcher.replace('"dispatch_prepared_exception:\\n\\t"',
    '".globl dispatch_prepared_exception\\n"\n         "dispatch_prepared_exception:\\n\\t"', 1)
if dispatcher.count('".seh_ec_context\\n\\t"') != 1:
    raise SystemExit("prepared EC-context range missing or ambiguous")
if args.negative_old_context:
    dispatcher = dispatcher.replace('".seh_ec_context\\n\\t"',
        '".seh_context\\n\\t"', 1)

output = args.output.resolve()
output.mkdir()  # Fail closed on replay/overwrite, including symlinks.
generated = output / "exception_unwind_source.inc"
generated.write_text(selector + "\n" + dispatcher, encoding="utf-8")
build = args.wine_build.resolve(strict=True)
clang = args.clang.resolve(strict=True)
binary = output / "exception-phase.exe"
command = [str(build / "tools/winegcc/winegcc"), "-o", str(binary), "--wine-objdir", ".",
    "--cc-cmd=" + str(clang), "-b", "arm64ec-windows", "-nostdlib", "-nodefaultlibs",
    "-Wl,--entry,mainCRTStartup", "-Wl,--file-alignment,4096", "-Wl,--section-alignment,65536",
    "-I" + str(repo / "include"), "-I" + str(repo / "include/msvcrt"), "-Iinclude",
    "-I" + str(output), "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-D__WINE_PE_BUILD",
    str(fixture), "-lwinecrt0", "-lkernel32", "-lntdll"]
with (output / "build.log").open("xb") as log:
    subprocess.run(command, cwd=build, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=60)
paths = [source_path, fixture, generated, binary, Path(__file__), clang, build / "tools/winegcc/winegcc"]
manifest = {"command": command, "cwd": str(build), "negative_old_context": args.negative_old_context,
    "negative_return_pc": args.negative_return_pc,
    "sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
with (output / "build.json").open("x", encoding="utf-8") as stream:
    json.dump(manifest, stream, indent=2, sort_keys=True)
    stream.write("\n")
print(binary)
