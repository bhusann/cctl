#!/usr/bin/env python3
"""Generate a C byte array from SKILL.md for `cctl --am`."""
import sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
with open(dst, "w", encoding="ascii") as out:
    out.write("/* Generated from SKILL.md by tools/gen_skill_inc.py; do not edit. */\n")
    out.write("static const unsigned char cctl_skill_md[] = {\n")
    for start in range(0, len(data), 16):
        chunk = data[start:start + 16]
        out.write("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",\n")
    out.write("    0x00\n};\n")
