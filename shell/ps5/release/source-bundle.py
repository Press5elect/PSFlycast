#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 the PSFlyCast contributors
"""PSFlyCast - the corresponding source of a staged title, one archive per part.

    python3 shell/ps5/release/source-bundle.py build-ps5/dist/PPSA99247 OUT --tag v1.0.0

Reads the title's licenses/components.json (stage-notices.py) and writes, at
the revision recorded there for each part:

  PSFlyCast-<tag>-source.tar.xz   this repository (Flycast is merged into it),
                                  with every submodule the build checked out
  PS5_Mesa-<rev>.tar.xz           RADV's source as built
  PS5_Vulkan-<rev>.tar.xz         the link recipe and the packaging tool
  PS5_PayloadSDK-<rev>.tar.xz     the SDK fork: the revision the title was
                                  compiled with, and the one RADV was
  libsmb2-<rev>.tar.xz
  libretro-database-cht-dreamcast-<rev>.tar.xz   the cheat files, as data

plus SHA256SUMS and SOURCES.txt, which say what each archive is. They are
attached to the release beside the title's ZIP, so the source is offered from
the same place as the binary. A repository with uncommitted changes is
refused: its committed revision is not what was built. With --max-size an
archive larger than that is written as parts, each an archive of its own.
"""

import argparse
import hashlib
import io
import json
import lzma
import os
import subprocess
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent


class BundleError(Exception):
    pass


def git(path, *args, binary=False):
    result = subprocess.run(["git", "-C", str(path), *args], capture_output=True)
    if result.returncode != 0:
        raise BundleError(f"git {' '.join(args)} in {path}: {result.stderr.decode().strip()}")
    return result.stdout if binary else result.stdout.decode().strip()


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def clean(path):
    if git(path, "status", "--porcelain", "--untracked-files=no"):
        raise BundleError(f"{path} has uncommitted changes")


def archive_members(path, revision, prefix, pathspec=()):
    """The files of a revision, as tar members under prefix."""
    data = git(path, "archive", "--format=tar", f"--prefix={prefix}", revision, *pathspec, binary=True)
    return tarfile.open(fileobj=io.BytesIO(data))


def write_archive(target, entries):
    """One .tar.xz of (git archive, member) entries."""
    with lzma.open(target, "wb", preset=6) as packed:
        with tarfile.open(fileobj=packed, mode="w", format=tarfile.PAX_FORMAT) as out:
            for source, member in entries:
                if member.pax_headers:
                    member.pax_headers = {}
                data = source.extractfile(member) if member.isfile() else None
                out.addfile(member, data)


def write_archives(out, name, sources, limit):
    """The archive NAME.tar.xz of several git archives (a repository and its
    submodules). Where it would be larger than limit bytes it is written as
    parts instead, NAME.part1ofN.tar.xz and so on: each is an archive of its
    own, of files in the order git lists them, and all unpack into the same
    folder. Returns the files written."""
    entries = [(source, member) for source in sources for member in source]
    whole = out / f"{name}.tar.xz"
    write_archive(whole, entries)
    if not limit or whole.stat().st_size <= limit:
        return [whole.name]
    total = sum(member.size for _, member in entries)
    parts = whole.stat().st_size // limit + 1
    whole.unlink()
    while True:
        # Cut where the files before reach each share of the bytes.
        written, begin, done = [], 0, 0
        for index in range(parts):
            end, share = begin, total * (index + 1) / parts
            while end < len(entries) and (done < share or index == parts - 1):
                done += entries[end][1].size
                end += 1
            target = out / f"{name}.part{index + 1}of{parts}.tar.xz"
            write_archive(target, entries[begin:end])
            written.append(target)
            begin = end
        if all(target.stat().st_size <= limit for target in written):
            return [target.name for target in written]
        for target in written:
            target.unlink()
        parts += 1


def submodules(path):
    """The submodules checked out, each at the commit the repository records for it."""
    found = []
    for line in git(path, "submodule", "status", "--recursive").splitlines():
        if not line.strip():
            continue
        state, rest = line[0], line[1:].split()
        if state == "-":
            continue        # not checked out: the build does not use it
        if state != " ":
            raise BundleError(f"submodule {rest[1]} is not at the commit the repository records")
        found.append((rest[1], rest[0]))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("title", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--max-size", type=float, default=0,
                        help="the largest archive, in MiB: a larger one is written as parts (a host's upload limit)")
    arguments = parser.parse_args()
    sys.path.insert(0, str(HERE))
    names = __import__("stage-notices").variables()
    record = json.loads((arguments.title / "licenses/components.json").read_text(encoding="utf-8"))
    parts = {part["id"]: part for part in record["components"]}
    out = arguments.out
    out.mkdir(parents=True, exist_ok=True)
    made = []     # (file, what it is)

    def add(name, what, sources):
        stem = name[: -len(".tar.xz")]
        files = write_archives(out, stem, sources, int(arguments.max_size * 1024 * 1024))
        for index, file in enumerate(files):
            part = f" Part {index + 1} of {len(files)}: unpack them all into the same folder." if len(files) > 1 else ""
            made.append((file, what + part))
            print(f"source: {file} ({(out / file).stat().st_size / 1e6:.1f} MB)")

    try:
        # This repository, at the revision the title was built from.
        revision = parts["psflycast"]["source"]["revision"]
        clean(ROOT)
        if git(ROOT, "rev-parse", "HEAD") != revision:
            raise BundleError("the repository is not at the revision the title was built from")
        prefix = f"PSFlyCast-{arguments.tag}/"
        sources = [archive_members(ROOT, revision, prefix)]
        used = submodules(ROOT)
        for sub, commit in used:
            sources.append(archive_members(ROOT / sub, commit, prefix + sub + "/"))
        add(f"PSFlyCast-{arguments.tag}-source.tar.xz",
            f"PSFlyCast at {revision}: the port (shell/ps5/), Flycast at {parts['flycast']['source']['revision']} merged into it, "
            f"and the submodules the build uses, each at the commit the repository records: "
            + ", ".join(f"{sub} {commit[:12]}" for sub, commit in used) + ". The libraries Flycast compiles in and its fonts are in it.",
            sources)

        mesa = parts["radv"]["source"]
        add(f"PS5_Mesa-{mesa['revision'][:12]}.tar.xz",
            f"RADV and the parts of Mesa it links, as built: {mesa['url']} at {mesa['base']} with this repository's "
            f"shell/ps5/mesa/ patch applied (revision {mesa['revision']} of the build's checkout). zlib 1.3.1, which the "
            f"archive links, is Mesa's subproject (subprojects/zlib.wrap names its source).",
            [archive_members(names["MESA"], mesa["revision"], f"PS5_Mesa-{mesa['revision'][:12]}/")])

        vulkan = parts["ps5-vulkan"]["source"]
        clean(names["VULKAN"])
        add(f"PS5_Vulkan-{vulkan['revision'][:12]}.tar.xz",
            f"The RADV build and link recipe, the linker script, the tool that makes eboot.bin and sce_module/libc.prx: "
            f"{vulkan['url']} at {vulkan['base']}, {vulkan.get('localnote', '')} (revision {vulkan['revision']} of the build's checkout).",
            [archive_members(names["VULKAN"], vulkan["revision"], f"PS5_Vulkan-{vulkan['revision'][:12]}/")])

        sdk = parts["payload-sdk"]["source"]
        for revision, role in ((sdk["revision"], "the title was compiled with it"), (sdk.get("also"), "RADV's archive was compiled with it")):
            if revision:
                add(f"PS5_PayloadSDK-{revision[:12]}.tar.xz",
                    f"The payload SDK fork and its platform layer, {sdk['url']} at {revision}: {role}. It installs over the "
                    f"ps5-payload-dev SDK v0.42 release (https://github.com/ps5-payload-dev/sdk/releases/tag/v0.42), whose "
                    f"archives hold the LLVM runtime the title links.",
                    [archive_members(names["SDK_FORK"], revision, f"PS5_PayloadSDK-{revision[:12]}/")])

        smb = parts["libsmb2"]["source"]
        clean(names["SMB"])
        add(f"libsmb2-{smb['revision'][:12]}.tar.xz", f"libsmb2, {smb['url']} at {smb['revision']}, unmodified.",
            [archive_members(names["SMB"], smb["revision"], f"libsmb2-{smb['revision'][:12]}/")])

        if "cheats" in parts:
            cheats = parts["cheats"]["source"]
            add(f"libretro-database-cht-dreamcast-{cheats['revision'][:12]}.tar.xz",
                f"The cheat files in cheats/, as data: cht/Sega - Dreamcast and the LICENSE of {cheats['url']} at {cheats['revision']}.",
                [archive_members(names["CHEATS"], cheats["revision"], f"libretro-database-{cheats['revision'][:12]}/",
                                 ("cht/Sega - Dreamcast", "LICENSE"))])
    except BundleError as error:
        print(f"source: {error}", file=sys.stderr)
        return 1

    lines = [f"PSFlyCast {arguments.tag} - the source of everything in the title's ZIP", "",
             "Each archive is the source of one part at the revision the title was built from",
             "(licenses/README.txt in the title's folder lists the parts and their licences).", ""]
    for name, what in made:
        lines += [name, "  " + what, ""]
    lines += ["Not an archive here, because it is an unmodified upstream release:",
              "  the LLVM runtime (libc++, libc++abi, libunwind, compiler-rt builtins): the ps5-payload-dev SDK v0.42",
              "  release archives, https://github.com/ps5-payload-dev/sdk/releases/tag/v0.42", "",
              "Building: shell/ps5/README.md, \"Building\", in PSFlyCast's archive.", ""]
    (out / "SOURCES.txt").write_text("\n".join(lines), encoding="utf-8")
    with open(out / "SHA256SUMS", "w", encoding="utf-8") as sums:
        for name, _ in made:
            sums.write(f"{sha256(out / name)}  {name}\n")
    print(f"source: {len(made)} archives, SHA256SUMS and SOURCES.txt in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
