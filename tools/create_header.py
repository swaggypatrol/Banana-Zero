"""Embeds a compiled shader in the DLL: writes a C++ header holding the file's bytes as an array.

    python tools/create_header.py <shader.cso> <header.h> <array name>

The build runs it for every src/*.hlsl (src/dlssnr.vcxproj), e.g. nr.cso -> build/Release/obj/nr_shader.h, array
nr_cso: twelve bytes per line, each line ending in ", ", written with the platform's line endings. A header whose text
is already right is not rewritten, only touched, so that it is newer than its .hlsl and the next build leaves it be.
Exit code 1 if a file cannot be read or written.
"""

import os
import sys

PER_LINE = 12
INDENT = "    "


def header(data: bytes, name: str) -> str:
    rows = [", ".join(f"0x{b:02x}" for b in data[i : i + PER_LINE]) for i in range(0, len(data), PER_LINE)]
    body = (", \n" + INDENT).join(rows)
    if data and len(data) % PER_LINE == 0:
        body += "\n" + INDENT  # a full last row still breaks the line
    return f"#pragma once\n\ninline static const unsigned char {name}[] = {{\n{INDENT}{body}\n}};\n"


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print("usage: create_header.py <shader.cso> <header.h> <array name>", file=sys.stderr)
        return 1
    source, target, name = argv[1:]
    try:
        with open(source, "rb") as f:
            data = f.read()
        text = header(data, name)
        current = None
        if os.path.exists(target):
            with open(target, "r") as f:  # text mode both ways, so line endings compare equal
                current = f.read()
        if current == text:
            os.utime(target, None)  # newer than the .hlsl again, so the build does not redo this every time
        else:
            with open(target, "w") as f:  # text mode: the platform's line endings
                f.write(text)
    except OSError as error:
        print(f"create_header.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
