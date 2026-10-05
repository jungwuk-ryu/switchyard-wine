#!/usr/bin/env python3
"""Compile the real cpu.c resolver in a native, UBSan contract harness."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


def extract(source, name):
    match = re.search(r"(?m)^static [\w\s*]+\b" + re.escape(name) + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(f"missing implementation: {name}")
    start = source.index("{", match.start())
    depth = 0
    for end in range(start, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if not depth:
            return source[match.start():end + 1]
    raise ValueError(f"unterminated implementation: {name}")


def main():
    source = Path(sys.argv[1]).read_text()
    names = ["decode_ec_entry_thunk", "ec_entry_cache_index"]
    if "static void touch_ec_entry_cache(" in source:
        names.append("touch_ec_entry_cache")
    names.append("resolve_ec_entry_thunk")
    with tempfile.TemporaryDirectory(prefix="xtajit64-ec-cache-") as directory:
        root = Path(directory)
        # This is generated compiler input, never a replacement for cpu.c.
        (root / "ec_entry_cache_impl.h").write_text("\n\n".join(extract(source, name) for name in names))
        command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=undefined", "-fno-sanitize-recover=all", "-pthread", "-I" + str(root),
                   str(Path(__file__).with_name("ec_entry_cache.c")), "-o", str(root / "test")]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(root / "test")], check=True, timeout=20)


if __name__ == "__main__":
    main()
