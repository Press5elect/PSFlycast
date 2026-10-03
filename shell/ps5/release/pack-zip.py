#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 the PSFlyCast contributors
"""PSFlyCast - pack a staged title folder into the release's ZIP.

    python3 shell/ps5/release/pack-zip.py build-ps5/dist/PPSA99247 OUT/PSFlyCast-v1.0.0.zip --date 2026-10-03

The same way every time, so the same folder gives the same ZIP: entries in
name order, directory entries included (the empty games/, bios/ and covers/
are part of the title), deflate, Unix modes kept, every timestamp the
release's date. The ZIP is then read back and tested, and its sha256 printed.
"""

import argparse
import hashlib
import sys
import zipfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("title", type=Path)
    parser.add_argument("zip", type=Path)
    parser.add_argument("--date", required=True, help="YYYY-MM-DD")
    arguments = parser.parse_args()
    title = arguments.title.resolve()
    year, month, day = (int(part) for part in arguments.date.split("-"))
    stamp = (year, month, day, 12, 0, 0)
    entries = sorted(title.rglob("*"), key=lambda path: str(path.relative_to(title)))
    arguments.zip.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(arguments.zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as out:
        for path in [title] + entries:
            name = title.name + "/" + str(path.relative_to(title)).replace("\\", "/") if path != title else title.name
            if path.is_dir():
                info = zipfile.ZipInfo(name + "/", stamp)
                info.external_attr = (0o40755 << 16) | 0x10
                out.writestr(info, b"")
            else:
                info = zipfile.ZipInfo(name, stamp)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = ((0o100755 if path.stat().st_mode & 0o111 else 0o100644) << 16)
                out.writestr(info, path.read_bytes(), compresslevel=9)
    with zipfile.ZipFile(arguments.zip) as packed:
        bad = packed.testzip()
        if bad is not None:
            print(f"zip: {bad} does not read back", file=sys.stderr)
            return 1
        count = len(packed.namelist())
    digest = hashlib.sha256(arguments.zip.read_bytes()).hexdigest()
    print(f"zip: {arguments.zip.name}, {count} entries, {arguments.zip.stat().st_size} bytes")
    print(f"sha256: {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
