#!/usr/bin/env python3
"""Embeds a static file into a C header as a byte array.

Usage: gen_web_header.py <input file> <output .h> <c identifier>
"""
import sys


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 1

    in_path, out_path, ident = sys.argv[1], sys.argv[2], sys.argv[3]

    with open(in_path, "rb") as f:
        data = f.read()

    with open(out_path, "w") as f:
        f.write("// Auto-generated from {} — do not edit by hand.\n".format(in_path))
        f.write("#pragma once\n\n")
        f.write("static const unsigned char {}[] = {{\n".format(ident))
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            f.write("    " + ",".join(str(b) for b in chunk) + ",\n")
        f.write("};\n")
        f.write("static const unsigned int {}_len = {};\n".format(ident, len(data)))

    return 0


if __name__ == "__main__":
    sys.exit(main())
