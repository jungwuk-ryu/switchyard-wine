#!/usr/bin/env python3
"""Validate the source-level scope of the ARM64EC late-libcall contracts."""

from __future__ import annotations

import re
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit("ARM64EC libcall contract source test: " + message)


def require(text: str, pattern: str, message: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        fail(message)


def function(text: str, name: str) -> str:
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", text, re.DOTALL)
    if not match:
        fail("cannot find " + name)
    start = text.rfind("\n", 0, match.start()) + 1
    brace = text.find("{", match.start())
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if not depth:
                return text[start:index + 1]
    fail("unterminated " + name)


def validate(contracts: dict[str, str], winecrt: str, cryptbase: str, makedep: str) -> None:
    signatures = {"memcpy": r"void \*,\s*const void \*,\s*size_t",
                  "memmove": r"void \*,\s*const void \*,\s*size_t",
                  "memset": r"void \*,\s*int,\s*size_t"}
    for name, signature in signatures.items():
        contract = contracts[name]
        require(contract, r"#\s*ifdef\s+__arm64ec__", name + " contract is not ARM64EC-only")
        require(winecrt, rf"(?m)^\s*arm64ec_{name}_contract\.c\s*\\",
                "winecrt0 does not build the " + name + " contract")
        require(contract, rf'__asm__\(\s*"\.weak __imp_{name}"\s*\);',
                name + " contract can create an unused IAT import")
        require(contract, rf"__declspec\(dllimport\).*\b{name}\s*\(\s*{signature}\s*\)",
                name + " is not a typed CRT import")
        body = function(contract, "arm64ec_" + name + "_contract")
        require(contract, rf"static\s+void\s*\*\s*__attribute__\(\(used,\s*noinline,\s*"
                          rf"nodebug,\s*no_builtin\(\"{name}\"\)\)\)\s*"
                          rf"arm64ec_{name}_contract\s*\(",
                name + " metadata producer is not local/retained/debug-free/non-builtin")
        require(body, rf"return\s+{name}\s*\(", name + " metadata producer lacks its typed call")

    if "ARM64EC_LIBCALLS" in cryptbase:
        fail("cryptbase still carries a module-specific libcall workaround")
    add_imports = function(makedep, "add_import_libs")
    require(add_imports, r'static\s+const\s+char\s*\*\s*const\s+libcalls\[\]\s*=\s*\{\s*'
                         r'"memcpy"\s*,\s*"memmove"\s*,\s*"memset"\s*,\s*NULL\s*\}',
            "the complete fixed memory-libcall contract set is not linked")
    require(add_imports, r"type\s*==\s*IMPORT_TYPE_DEFAULT", "contract is not limited to default imports")
    require(add_imports, r"!strcmp\(\s*basename,\s*\"winecrt0\"\s*\)",
            "contract is not anchored to winecrt0")
    require(add_imports, r"get_cpu_from_name\(\s*archs\.str\[link_arch\]\s*\)\s*==\s*CPU_ARM64EC",
            "contract is not limited to the actual ARM64EC link architecture")
    require(add_imports, r"!\(make->module\s*&&\s*is_crt_module\(\s*make->module\s*\)\)",
            "contract does not exclude CRT implementation modules")
    require(add_imports, r"!\(make->testdll\s*&&\s*is_crt_module\(\s*make->testdll\s*\)\)",
            "contract does not exclude CRT implementation test DLLs")
    require(add_imports, r"%sarm64ec_%s_contract\.o.*arch_dirs\[link_arch\].*\*libcall",
            "contract object is not selected by helper and actual ARM64EC object directory")
    contract_pos = add_imports.find("strarray_add( &ret, contract )")
    library_pos = add_imports.find("strarray_add( &ret, lib )")
    if contract_pos < 0 or library_pos < 0 or contract_pos >= library_pos:
        fail("contract object must precede the CRT import libraries")
    if "arm64ec_libcall.o" in makedep:
        fail("makedep still injects the former all-helper contract")
    if "ARM64EC_LIBCALLS" in makedep:
        fail("makedep still requires module-specific libcall declarations")
    if "strarray_has_crt_module" in add_imports:
        fail("contract still excludes nodefault links without inspecting their actual target")
    if re.search(r"fno-builtin-(?:memcpy|memmove|memset)", makedep):
        fail("makedep globally disables optimized memory builtins")


def main() -> None:
    if len(sys.argv) != 7:
        fail("usage: test MEMCPY MEMMOVE MEMSET WINECRT_MAKEFILE CRYPTBASE_MAKEFILE MAKEDEP")
    paths = [Path(value).resolve(strict=True) for value in sys.argv[1:]]
    values = [path.read_text(encoding="utf-8") for path in paths]
    contracts = dict(zip(("memcpy", "memmove", "memset"), values[:3]))
    validate(contracts, *values[3:])

    mutations = (
        (0, ".weak __imp_memcpy", ".weak_anti_dep __imp_memcpy"),
        (0, "noinline, nodebug,", "noinline,"),
        (3, "\tarm64ec_memcpy_contract.c \\", ""),
        (4, "MODULE    = cryptbase.dll\n", "MODULE    = cryptbase.dll\nARM64EC_LIBCALLS = memcpy\n"),
        (5, '"memcpy", "memmove", "memset"', '"memcpy", "memset"'),
        (5, "type == IMPORT_TYPE_DEFAULT", "type == IMPORT_TYPE_DIRECT"),
        (5, "!(make->module && is_crt_module( make->module ))", "true"),
        (5, "!(make->testdll && is_crt_module( make->testdll ))", "true"),
        (5, 'strmake( "%sarm64ec_%s_contract.o", arch_dirs[link_arch], *libcall )',
            'strmake( "%sarm64ec_%s_contract.o", arch_dirs[arch], *libcall )'),
    )
    for selected, old, new in mutations:
        changed = list(values)
        if changed[selected].count(old) != 1:
            fail("mutation anchor is not unique: " + old)
        changed[selected] = changed[selected].replace(old, new, 1)
        changed_contracts = dict(zip(("memcpy", "memmove", "memset"), changed[:3]))
        try:
            validate(changed_contracts, *changed[3:])
        except SystemExit:
            continue
        fail("validator accepted mutation: " + old)
    print("ARM64EC libcall contract source test passed")


if __name__ == "__main__":
    main()
