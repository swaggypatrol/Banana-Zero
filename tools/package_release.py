"""The release package: the two DLLs of one build, the README and the licences, zipped, with a SHA-256 list.

    python tools/package_release.py <tag> [<build dir>]
        Checks that the repository is exactly at the annotated tag <tag> with a clean tree (`git describe`, the
        command the build stamps the DLLs with, prints <tag> and nothing more) and that dxgi.dll and
        banana.nvngx.dll in <build dir> (default build/Release) are 64-bit DLLs stamped <tag>. Then writes
        dist/<tag>/Banana-Zero-<tag>.zip, a folder Banana-Zero-<tag>/ holding the files in CONTENTS, and
        dist/<tag>/SHA256SUMS.txt, every file of the zip in sha256sum's format, and verifies the result as below.

    python tools/package_release.py --verify <tag>
        Checks dist/<tag>/: the zip holds exactly the files the list names, with those hashes, and both DLLs carry
        the stamp. The release workflow (.github/workflows/release.yml) runs this before it publishes.

Both print a Markdown table of the files (name, bytes, SHA-256) on stdout; the release notes carry it. Text files go
into the zip with CRLF line endings, plus a UTF-8 byte order mark when they are not plain ASCII, so that Notepad on
any Windows shows them right, and every entry carries the tag commit's time, so the same files make the same zip.
Exit code 1 on any failed check. Needs nothing beyond Python and git.
"""

import hashlib
import io
import os
import re
import struct
import subprocess
import sys
import time
import zipfile
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

DLLS = ["dxgi.dll", "banana.nvngx.dll"]

# Name in the zip -> file in the repository. The DLLs come from the build directory.
CONTENTS = {
    "README.md": "README.md",
    "LICENSE.txt": "LICENSE",
    "THIRD_PARTY_NOTICES.txt": "THIRD_PARTY_NOTICES.txt",
}

TAG_PATTERN = re.compile(r"^v\d+\.\d+\.\d+$")


class Failed(Exception):
    pass


def git(*args):
    result = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise Failed(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def package_dir(tag):
    return os.path.join(ROOT, "dist", tag)


def zip_name(tag):
    return f"Banana-Zero-{tag}.zip"


def check_dll(name, data, tag):
    """A 64-bit PE DLL whose build stamp (the BUILD_HASH string both DLLs carry) is exactly <tag>."""
    if data[:2] != b"MZ" or len(data) < 0x40:
        raise Failed(f"{name}: not a PE file")
    nt = struct.unpack_from("<I", data, 0x3C)[0]
    if data[nt : nt + 4] != b"PE\0\0" or struct.unpack_from("<H", data, nt + 4)[0] != 0x8664:
        raise Failed(f"{name}: not a 64-bit PE file")
    if struct.unpack_from("<H", data, nt + 22)[0] & 0x2000 == 0:
        raise Failed(f"{name}: an executable, not a DLL")
    # The stamp is a C string of its own: a NUL right after it (so not "<tag>-dirty" or "<tag>-3-g1234567"), and
    # no letter, digit or ._-/ right before it (so not the tail of a longer name).
    stamp = tag.encode("ascii") + b"\0"
    name_bytes = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._-/"
    start = data.find(stamp)
    while start != -1:
        if start == 0 or data[start - 1] not in name_bytes:
            return
        start = data.find(stamp, start + 1)
    raise Failed(f"{name}: not stamped {tag} (built from another commit, or from a dirty tree)")


def text_for_zip(data):
    """CRLF line endings; a UTF-8 byte order mark when the text is not plain ASCII (old Notepad needs one)."""
    text = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    if text.startswith(b"\xef\xbb\xbf"):
        return text
    try:
        text.decode("ascii")
        return text
    except UnicodeDecodeError:
        return b"\xef\xbb\xbf" + text


def table(files):
    """The Markdown table the release notes carry: name, bytes, SHA-256."""
    lines = ["| File | Bytes | SHA-256 |", "|---|---:|---|"]
    for name, data in files:
        lines.append(f"| `{name}` | {len(data):,} | `{hashlib.sha256(data).hexdigest()}` |")
    return "\n".join(lines)


def read_sums(path):
    """SHA256SUMS.txt: '<64 hex digits>  <name>' per line (' *<name>' also taken). Name -> hash, in file order."""
    sums = {}
    with open(path, "r", encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip("\r\n")
            if not line:
                continue
            match = re.fullmatch(r"([0-9a-f]{64}) [ *](\S.*)", line)
            if match is None:
                raise Failed(f"{path} line {number}: not '<sha256>  <name>': {line!r}")
            name = match.group(2)
            if name in sums:
                raise Failed(f"{path}: {name} is listed twice")
            sums[name] = match.group(1)
    if not sums:
        raise Failed(f"{path}: empty")
    return sums


def verify(tag):
    """Checks dist/<tag>/ as the docstring says. Returns the files of the zip as (name, bytes), in the list's order."""
    folder = package_dir(tag)
    archive = os.path.join(folder, zip_name(tag))
    sums_path = os.path.join(folder, "SHA256SUMS.txt")
    for path in (archive, sums_path):
        if not os.path.isfile(path):
            raise Failed(f"{os.path.relpath(path, ROOT)} is missing")
    sums = read_sums(sums_path)
    prefix = f"Banana-Zero-{tag}/"
    members = {}
    with zipfile.ZipFile(archive) as z:
        bad = z.testzip()
        if bad is not None:
            raise Failed(f"{zip_name(tag)}: {bad} is damaged")
        for info in z.infolist():
            if info.is_dir():
                continue
            if not info.filename.startswith(prefix):
                raise Failed(f"{zip_name(tag)}: {info.filename} is outside {prefix}")
            members[info.filename[len(prefix) :]] = z.read(info)
    if set(members) != set(sums):
        extra = sorted(set(members) - set(sums))
        missing = sorted(set(sums) - set(members))
        raise Failed(f"the zip and SHA256SUMS.txt differ: only in the zip {extra}, only in the list {missing}")
    for name, digest in sums.items():
        if hashlib.sha256(members[name]).hexdigest() != digest:
            raise Failed(f"{name}: its SHA-256 is not the one SHA256SUMS.txt lists")
    for name in DLLS:
        if name not in members:
            raise Failed(f"the zip has no {name}")
        check_dll(name, members[name], tag)
    return [(name, members[name]) for name in sums]


def package(tag, build_dir):
    described = git("describe", "--always", "--dirty", "--abbrev=7")
    if described != tag:
        raise Failed(f"git describe says {described}, not {tag}: check out the annotated tag {tag} with a clean tree")
    commit_time = int(git("log", "-1", "--format=%ct", f"{tag}^{{commit}}"))
    stamp = time.gmtime(commit_time)[:6]

    files = []
    for name in DLLS:
        with open(os.path.join(build_dir, name), "rb") as f:
            data = f.read()
        check_dll(name, data, tag)
        files.append((name, data))
    for name, source in CONTENTS.items():
        with open(os.path.join(ROOT, source), "rb") as f:
            files.append((name, text_for_zip(f.read())))

    folder = package_dir(tag)
    os.makedirs(folder, exist_ok=True)
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as z:
        for name, data in files:
            info = zipfile.ZipInfo(f"Banana-Zero-{tag}/{name}", date_time=stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 0  # MS-DOS, the same on every machine that packs it
            info.external_attr = 0x20  # MS-DOS "archive", a plain file (Python replaces a 0 with Unix modes)
            z.writestr(info, data)
    with open(os.path.join(folder, zip_name(tag)), "wb") as f:
        f.write(buffer.getvalue())
    with open(os.path.join(folder, "SHA256SUMS.txt"), "w", encoding="utf-8", newline="\n") as f:
        for name, data in files:
            f.write(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
    return verify(tag)


def main(argv):
    sys.stdout.reconfigure(encoding="utf-8")  # a redirected stdout on Windows is not UTF-8
    args = argv[1:]
    verifying = bool(args) and args[0] == "--verify"
    if verifying:
        args = args[1:]
    if len(args) not in (1, 2) or (verifying and len(args) != 1) or not TAG_PATTERN.match(args[0]):
        print("usage: package_release.py <tag vX.Y.Z> [<build dir>]\n"
              "       package_release.py --verify <tag vX.Y.Z>", file=sys.stderr)
        return 1
    tag = args[0]
    try:
        if verifying:
            files = verify(tag)
        else:
            build_dir = args[1] if len(args) == 2 else os.path.join(ROOT, "build", "Release")
            files = package(tag, build_dir)
    except (Failed, OSError, zipfile.BadZipFile, zlib.error) as error:
        print(f"package_release.py: {error}", file=sys.stderr)
        return 1
    folder = os.path.relpath(package_dir(tag), ROOT)
    print(f"package_release.py: {folder} is good: {len(files)} files, both DLLs stamped {tag}", file=sys.stderr)
    print(table(files))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
