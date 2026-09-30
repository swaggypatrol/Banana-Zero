"""The dxgi.dll export forwarding, generated from the real System32\\dxgi.dll's own export table.

    python tools/gen_dxgi_exports.py [<dxgi.dll>]
        Writes src/dxgi.def, src/dxgi_stubs.asm and src/dxgi_exports.h: every export of that DLL (default
        C:\\Windows\\System32\\dxgi.dll), with its name and ordinal, forwarded by one jump stub each.

    python tools/gen_dxgi_exports.py --check <built dxgi.dll> [<dxgi.dll>]
        Compares the built DLL's export table with the real one: the same ordinals with the same names, every
        export in code. Exit code 1 and a list of the differences if they are not.

The list must come from the DLL itself, never from memory or a web page: one missing export and some game will not
start. Reads the PE file directly, so it needs nothing beyond Python.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
DEFAULT_DLL = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "dxgi.dll")

IMAGE_SCN_MEM_EXECUTE = 0x20000000


class Export:
    def __init__(self, ordinal, name, rva, forwarder, in_code):
        self.ordinal = ordinal
        self.name = name  # None when exported by ordinal only
        self.rva = rva
        self.forwarder = forwarder  # "OTHER.Function" when the entry forwards elsewhere, else None
        self.in_code = in_code


def read_exports(path):
    """Every export of a 64-bit PE file, sorted by ordinal."""
    with open(path, "rb") as f:
        data = f.read()

    def u16(offset):
        return struct.unpack_from("<H", data, offset)[0]

    def u32(offset):
        return struct.unpack_from("<I", data, offset)[0]

    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE file")
    nt = u32(0x3C)
    if data[nt : nt + 4] != b"PE\0\0":
        raise ValueError(f"{path}: not a PE file")
    section_count = u16(nt + 6)
    optional_size = u16(nt + 20)
    optional = nt + 24
    if u16(optional) != 0x20B:
        raise ValueError(f"{path}: not a 64-bit DLL")

    # PE32+: the data directories start 112 bytes into the optional header; the export table is the first.
    export_rva, export_size = struct.unpack_from("<II", data, optional + 112)
    if export_rva == 0:
        return []

    sections = []
    for i in range(section_count):
        header = optional + optional_size + 40 * i
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, header + 8)
        characteristics = u32(header + 36)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, characteristics))

    def section_of(rva):
        for section in sections:
            if section[0] <= rva < section[0] + section[1]:
                return section
        raise ValueError(f"{path}: RVA {rva:#x} is in no section")

    def offset(rva):
        section = section_of(rva)
        return rva - section[0] + section[2]

    def string(rva):
        start = offset(rva)
        return data[start : data.index(b"\0", start)].decode("ascii")

    directory = offset(export_rva)
    base, function_count, name_count, functions, names, name_ordinals = struct.unpack_from(
        "<IIIIII", data, directory + 16
    )

    names_by_index = {}
    for i in range(name_count):
        index = u16(offset(name_ordinals) + 2 * i)
        names_by_index[index] = string(u32(offset(names) + 4 * i))

    exports = []
    for index in range(function_count):
        rva = u32(offset(functions) + 4 * index)
        if rva == 0:
            continue  # a gap in the ordinal range
        forwarder = string(rva) if export_rva <= rva < export_rva + export_size else None
        in_code = forwarder is not None or bool(section_of(rva)[3] & IMAGE_SCN_MEM_EXECUTE)
        exports.append(Export(base + index, names_by_index.get(index), rva, forwarder, in_code))
    return exports


def file_version(path):
    """The version resource's file version, or None off Windows."""
    try:
        import ctypes
        from ctypes import wintypes

        version = ctypes.windll.version
    except (ImportError, AttributeError):
        return None
    size = version.GetFileVersionInfoSizeW(path, None)
    if size == 0:
        return None
    buffer = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(path, 0, size, buffer):
        return None
    info = ctypes.c_void_p()
    length = wintypes.UINT()
    if not version.VerQueryValueW(buffer, "\\", ctypes.byref(info), ctypes.byref(length)):
        return None
    ms, ls = struct.unpack_from("<II", ctypes.string_at(info.value, length.value), 8)
    return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"


def generate(dll):
    exports = read_exports(dll)
    if not exports:
        sys.exit(f"{dll} has no exports")
    data = [e for e in exports if not e.in_code]
    if data:
        # A jump stub can only stand in for code.
        sys.exit(f"{dll} exports data, which a jump stub cannot forward: " + ", ".join(label(e) for e in data))

    version = file_version(dll)
    source = f"{dll}" + (f" ({version})" if version else "")
    stamp = f"Generated by tools/gen_dxgi_exports.py from {source}, {len(exports)} exports. Regenerate, don't edit."

    lines = [
        f"; {stamp}",
        "; Every export of the real DLL, same name and ordinal, each a jump stub in dxgi_stubs.asm.",
        "LIBRARY dxgi",
        "EXPORTS",
    ]
    for i, e in enumerate(exports):
        if e.name is None:
            lines.append(f"    dxgi_stub_{i} @{e.ordinal} NONAME")
        else:
            lines.append(f"    {e.name}=dxgi_stub_{i} @{e.ordinal}")
    write(os.path.join(SRC, "dxgi.def"), lines)

    lines = [
        f"; {stamp}",
        "; Stub i jumps through g_dxgiReal[i], which main.cpp fills from the real System32\\dxgi.dll. Nothing is",
        "; pushed or changed, so the real function sees the caller's own call: arguments, stack and return address.",
        "; Each stub starts a 16-byte slot of its own, like a compiled function: hooking tools (security software on the",
        "; dev PC hooks CreateDXGIFactory*) overwrite the start of a function, some with a 14-byte jump, and that must",
        "; not run into the next stub.",
        "EXTERN g_dxgiReal:QWORD",
        ".CODE",
    ]
    for i, e in enumerate(exports):
        lines += [
            "ALIGN 16",
            f"dxgi_stub_{i} PROC ; {label(e)}",
            f"    jmp QWORD PTR [g_dxgiReal + {8 * i}]",
            f"dxgi_stub_{i} ENDP",
        ]
    lines.append("END")
    write(os.path.join(SRC, "dxgi_stubs.asm"), lines)

    lines = [
        "#pragma once",
        "",
        f"// {stamp}",
        "// kDxgiExports[i] is exported by dxgi_stub_i in dxgi_stubs.asm, which jumps through g_dxgiReal[i].",
        "",
        "struct DxgiExport",
        "{",
        "    unsigned short ordinal;",
        "    const char* name; // nullptr when exported by ordinal only",
        "};",
        "",
        "inline constexpr DxgiExport kDxgiExports[] = {",
    ]
    for e in exports:
        name = "nullptr" if e.name is None else f'"{e.name}"'
        lines.append(f"    {{ {e.ordinal}, {name} }},")
    lines.append("};")
    write(os.path.join(SRC, "dxgi_exports.h"), lines)

    print(f"{len(exports)} exports from {source}")
    for e in exports:
        print(f"  {label(e)}" + (f" -> {e.forwarder}" if e.forwarder else ""))


def check(built, dll):
    ours = {e.ordinal: e for e in read_exports(built)}
    real = {e.ordinal: e for e in read_exports(dll)}
    problems = []
    for ordinal, e in sorted(real.items()):
        if ordinal not in ours:
            problems.append(f"missing: {label(e)}")
        elif ours[ordinal].name != e.name:
            problems.append(f"ordinal {ordinal}: ours is {label(ours[ordinal])}, the real one is {label(e)}")
    for ordinal, e in sorted(ours.items()):
        if ordinal not in real:
            problems.append(f"extra: {label(e)}")
        if e.forwarder is not None or not e.in_code:
            problems.append(f"not a stub in our code: {label(e)}")
    if problems:
        print(f"{built} does not match {dll}:")
        for problem in problems:
            print(f"  {problem}")
        sys.exit(1)
    print(f"OK: {built} exports the same {len(real)} names and ordinals as {dll}")


def label(e):
    return f"{e.name or '(no name)'} @{e.ordinal}"


def write(path, lines):
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main(args):
    if args and args[0] == "--check":
        if len(args) not in (2, 3):
            sys.exit(__doc__)
        check(args[1], args[2] if len(args) == 3 else DEFAULT_DLL)
    elif len(args) <= 1 and not (args and args[0].startswith("-")):
        generate(args[0] if args else DEFAULT_DLL)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
