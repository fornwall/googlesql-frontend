"""Renders an ICU common-data package as portable C source.

This is a small reimplementation of ICU genccode's C output. It lets the
native Bazel build embed the release's prebuilt data without first building
and running ICU's host tools.

Usage: dat_to_c.py <input.dat> <entry-name> <output.c>
"""

import sys


def main() -> None:
    dat_path, entry_name, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(dat_path, "rb") as source:
        data = source.read()

    with open(out_path, "w", encoding="ascii") as output:
        output.write(
            "#ifndef IN_GENERATED_CCODE\n"
            "#define IN_GENERATED_CCODE\n"
            "#define U_DISABLE_RENAMING 1\n"
            '#include "unicode/umachine.h"\n'
            "#endif\n"
            "U_CDECL_BEGIN\n"
            "const struct {\n"
            "    double bogus;\n"
            f"    uint8_t bytes[{len(data)}];\n"
            f"}} {entry_name}_dat={{ 0.0, {{\n"
        )
        for offset in range(0, len(data), 16):
            output.write(",".join(str(value) for value in data[offset : offset + 16]))
            output.write(",\n" if offset + 16 < len(data) else "\n")
        output.write("}\n};\nU_CDECL_END\n")


if __name__ == "__main__":
    main()
