#!/usr/bin/env python3
"""Verifies that a host executable exports every Node-API function.

Addons resolve napi_* from the host process; a missing export makes an addon
fail to load (Linux/macOS) or crash on first call (Windows delay-load).

Usage: tools/check_exports.py <executable> [--tool nm|objdump]
"""

import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from napi_decls import parse_declarations  # noqa: E402


def read_head(path, n=4):
    with open(path, "rb") as f:
        return f.read(n)


def exported_symbols(path):
    head = read_head(path)
    if head[:2] == b"MZ":
        # PE: parse the export table printed by (llvm-)objdump -p.
        tool = shutil.which("llvm-objdump") or shutil.which("objdump") or \
            shutil.which("x86_64-w64-mingw32-objdump")
        if not tool:
            raise SystemExit("objdump not found: cannot inspect a PE file")
        out = subprocess.run([tool, "-p", path], capture_output=True,
                             text=True, check=True).stdout
        names = set()
        in_table = False
        for line in out.splitlines():
            if "Name Pointer" in line or "Export Table" in line:
                in_table = True
                continue
            if in_table:
                m = re.search(r"\b((?:napi|node_api)_\w+)\b", line)
                if m:
                    names.add(m.group(1))
        return names
    if head[:4] in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe"):
        cmd = ["nm", "-gU", path]  # Mach-O
    else:
        cmd = ["nm", "-D", "--defined-only", path]  # ELF
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    names = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ("T", "t", "W"):
            names.add(parts[2].lstrip("_") if head[:2] != b"\x7fE" else parts[2])
    return names


def main(argv):
    if len(argv) < 1:
        print(__doc__)
        return 2
    exe = argv[0]
    expected = {d.name for d in parse_declarations()}
    exported = exported_symbols(exe)
    missing = sorted(expected - exported)
    if missing:
        print("%s does not export %d Node-API functions:" % (exe, len(missing)))
        for name in missing:
            print("  " + name)
        return 1
    print("%s exports all %d Node-API functions" % (exe, len(expected)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
