#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 the PSFlyCast contributors
"""PSFlyCast - the notices of a staged title folder.

    python3 shell/ps5/release/stage-notices.py build-ps5/dist/PPSA99247 [--release TAG] [--check]

Reads shell/ps5/release/components.json and writes into the title folder:

  LEGAL.txt                 no piracy, the licence of the whole, the trademarks
  licenses/README.txt       every part: licence, copyright, texts, source, revision
  licenses/components.json  the same, with the sha256 of every file of the title
  licenses/<part>/          the licence texts each part's licence asks to travel

and then checks the folder: every file in it belongs to a part or is the
user's to fill (games, bios, covers), nothing that may never ship is there
(games, BIOS files, the console's settings), and no source it was built from
has uncommitted changes (--release refuses those; without it they are
recorded as "dirty"). --check only checks a folder already staged.

shell/ps5/build.sh runs it at the end of every build.
"""

import argparse
import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent

# What the user fills, and what a title folder holds besides its parts.
USER_FOLDERS = ("games", "bios", "covers", "patches", "data", "logs", "cheats")
# Never in a package: games, BIOS files, what a console wrote.
FORBIDDEN = ("*.chd", "*.gdi", "*.cdi", "*.cue", "*.iso", "*.7z", "*.zip", "dc_boot.bin", "dc_flash.bin",
             "*boot.bin", "*flash.bin", "*.cfg", "*.log", "logo.png", "*.state", "*.sav", "*.vmu",
             "patches/patches.txt", ".env")
ALLOWED_DESPITE = ("eboot.bin",)


class NoticeError(Exception):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def git(path, *args):
    result = subprocess.run(["git", "-C", str(path), *args], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def variables():
    vulkan = Path(os.environ.get("PS5_VULKAN_DIR", ROOT.parent / "PS5_Vulkan")).resolve()
    return {
        "VULKAN": vulkan,
        "MESA": Path(os.environ.get("PS5_MESA_FORK", vulkan.parent / "PS5_Mesa")).resolve(),
        "SDK": Path(os.environ.get("PS5_PAYLOAD_SDK", ROOT.parent / "PS5_RetroArch/.deps/native/ps5-payload-sdk")).resolve(),
        "SDK_FORK": Path(os.environ.get("PS5_PAYLOAD_SDK_FORK", ROOT.parent / "PS5_PayloadSDK")).resolve(),
        "SMB": Path(os.environ.get("LIBSMB2_DIR", ROOT.parent / "libsmb2")).resolve(),
        "CHEATS": Path(os.environ.get("CHEATS_REPO", ROOT.parent / "libretro-database")).resolve(),
    }


def resolve(text, names):
    def one(match):
        return str(names[match.group(1)])
    path = Path(re.sub(r"\$\{(\w+)\}", one, text))
    return path if path.is_absolute() else ROOT / path


def provenance(path, key):
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith(key + ":"):
            return line.split(":", 1)[1].strip()
    raise NoticeError(f"no '{key}:' in {path}")


def repository_state(path):
    """The revision a git checkout is at, and whether it has uncommitted changes."""
    revision = git(path, "rev-parse", "HEAD")
    if revision is None:
        return None, False
    dirty = bool(git(path, "status", "--porcelain", "--untracked-files=no"))
    return revision, dirty


def source_of(part, names):
    spec = part["source"]
    kind = spec["kind"]
    out = {"kind": kind}
    for key in ("url", "where", "base", "localnote"):
        if key in spec:
            out[key] = spec[key]
    if kind == "fixed":
        out["revision"] = spec["revision"]
    elif kind == "in-tree":
        revision, dirty = repository_state(ROOT)
        out["revision"] = revision or "the revision of the source archive this was built from"
        out["where"] = "in this title's repository (Flycast's core/deps, its submodules and fonts/), at this revision"
        out["dirty"] = dirty
    elif kind == "git":
        path = resolve(spec["path"], names)
        revision, dirty = repository_state(path)
        if revision is None:
            raise NoticeError(f"{part['id']}: {path} is not a git checkout")
        out["revision"] = revision
        out["dirty"] = dirty
    elif kind == "provenance":
        out["revision"] = provenance(resolve(spec["file"], names), "revision")
        out["archive_sha256"] = provenance(resolve(spec["file"], names), "archive sha256")
        path = resolve(spec["path"], names)
        if git(path, "cat-file", "-t", out["revision"]) != "commit":
            raise NoticeError(f"{part['id']}: {path} does not have {out['revision']}")
    elif kind == "stamp":
        out["revision"] = resolve(spec["file"], names).read_text(encoding="utf-8").strip()
        if spec.get("also") == "provenance-sdk":
            out["also"] = provenance(names["VULKAN"] / ".deps/native/radv-release/PROVENANCE.txt", "sdk")
    else:
        raise NoticeError(f"{part['id']}: source kind {kind}")
    return out


def copy_text(spec, destination, names):
    source = resolve(spec["from"], names)
    if not source.exists():
        if spec.get("optional"):
            return None
        raise NoticeError(f"licence text not found: {source}")
    target = destination / spec["to"]
    if source.is_dir():
        shutil.copytree(source, target)
        return spec["to"]
    target.parent.mkdir(parents=True, exist_ok=True)
    if "head" in spec or "tail" in spec:
        lines = source.read_text(encoding="utf-8", errors="replace").splitlines()
        lines = lines[: spec["head"]] if "head" in spec else lines[-spec["tail"]:]
        target.write_text(f"From {spec['from']} in Flycast's source:\n\n" + "\n".join(lines) + "\n", encoding="utf-8")
    else:
        shutil.copyfile(source, target)
    return spec["to"]


def files_of(title):
    return sorted(str(path.relative_to(title)).replace(os.sep, "/")
                  for path in title.rglob("*") if path.is_file())


def stage(title, release):
    names = variables()
    table = json.loads((HERE / "components.json").read_text(encoding="utf-8"))
    licenses = title / "licenses"
    if licenses.exists():
        shutil.rmtree(licenses)
    licenses.mkdir()
    shutil.copyfile(HERE / "LEGAL.txt", title / "LEGAL.txt")
    present = files_of(title)
    staged = []
    for part in table["components"]:
        matched = sorted({name for pattern in part["files"] for name in present if fnmatch.fnmatch(name, pattern)
                          and not (pattern == "licenses/*")})
        if not matched:
            continue        # a part not in this build (no cheat files staged, say)
        folder = licenses / part["id"]
        texts = []
        if part["texts"]:
            folder.mkdir()
            for spec in part["texts"]:
                copied = copy_text(spec, folder, names)
                if copied:
                    texts.append(f"{part['id']}/{copied}")
        if part["kind"] == "code" and part["source"]["kind"] != "in-tree" and not texts:
            raise NoticeError(f"{part['id']}: a code part with no licence text")
        entry = {key: part[key] for key in ("id", "name", "kind", "licence", "copyright", "modifications")}
        entry["texts"] = texts
        entry["source"] = source_of(part, names)
        if len(matched) > 40:
            entry["files"] = [f"{len(matched)} files matching " + ", ".join(part["files"])]
        else:
            entry["files"] = matched
        entry["sha256"] = {name: sha256(title / name) for name in matched
                           if name in ("eboot.bin", "sandbox-elevator.elf", "sce_module/libc.prx")}
        staged.append(entry)
    dirty = sorted({entry["id"] for entry in staged if entry["source"].get("dirty")})
    if dirty and release:
        raise NoticeError("uncommitted changes in the source of: " + ", ".join(dirty))
    result = {"schema": 1, "release": release or "development build", "components": staged}
    (licenses / "components.json").write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    (licenses / "README.txt").write_text(readme(result), encoding="utf-8")
    return result


def readme(result):
    legal = (HERE / "LEGAL.txt").read_text(encoding="utf-8")
    head, body = legal.split("\n", 2)[0], legal.split("\n", 2)[2]
    out = ["PSFlyCast - licences, notices and sources", "=" * 41, "", f"Release: {result['release']}", body.rstrip(), "",
           "This folder lists every part of this title, the licence each is under, the",
           "licence texts those licences require to travel with it, and the source",
           "revision each part was built from. components.json holds the same, with the",
           "sha256 of every executable file.", "",
           "Corresponding source: the release page this title was downloaded from carries",
           "the complete source of everything in it, one archive per part, with",
           "SOURCES.txt saying what each is (made by shell/ps5/release/source-bundle.py).",
           "Flycast and the libraries it carries are in PSFlyCast's own source archive.", "",
           "Not included: no games, BIOS files, console firmware or decryption keys.", "",
           "Parts", "-----", ""]
    for part in result["components"]:
        source = part["source"]
        out.append(part["name"])
        out.append(f"  kind:      {part['kind']}")
        out.append(f"  licence:   {part['licence']}")
        for line in part["copyright"]:
            out.append(f"  copyright: {line}")
        if part["texts"]:
            out.append("  texts:     " + ", ".join(part["texts"]))
        if "url" in source:
            out.append(f"  source:    {source['url']}")
        if "where" in source:
            out.append(f"  source:    {source['where']}")
        out.append(f"  revision:  {source['revision']}" + (" (with uncommitted changes)" if source.get("dirty") else ""))
        if "base" in source:
            out.append(f"  base:      {source['base']}")
        if "also" in source:
            out.append(f"  also:      {source['also']} (the revision RADV's archive was compiled with)")
        if "localnote" in source:
            out.append(f"  note:      {source['localnote']}")
        out.append(f"  changes:   {part['modifications']}")
        out.append("  files:     " + ", ".join(part["files"]))
        out.append("")
    return "\n".join(out)


def check(title, release):
    problems = []
    record = json.loads((title / "licenses/components.json").read_text(encoding="utf-8"))
    table = json.loads((HERE / "components.json").read_text(encoding="utf-8"))
    patterns = [pattern for part in table["components"] for pattern in part["files"]]
    for name in files_of(title):
        base = name.rsplit("/", 1)[-1]
        if name not in ALLOWED_DESPITE and any(fnmatch.fnmatch(name, p) or fnmatch.fnmatch(base, p) for p in FORBIDDEN):
            problems.append(f"may never ship: {name}")
            continue
        if not any(fnmatch.fnmatch(name, pattern) for pattern in patterns):
            problems.append(f"belongs to no part: {name}")
    for folder in USER_FOLDERS:
        path = title / folder
        if path.is_dir() and folder != "cheats" and any(path.iterdir()):
            problems.append(f"{folder}/ is not empty")
    for needed in ("LEGAL.txt", "licenses/README.txt", "README.md", "eboot.bin", "sce_sys/param.json", "sce_module/libc.prx"):
        if not (title / needed).is_file():
            problems.append(f"missing: {needed}")
    for part in record["components"]:
        for text in part["texts"]:
            if not (title / "licenses" / text).exists():
                problems.append(f"{part['id']}: its text {text} is missing")
        for name, digest in part.get("sha256", {}).items():
            if sha256(title / name) != digest:
                problems.append(f"{name} is not the file the notices were made for")
        if release and part["source"].get("dirty"):
            problems.append(f"{part['id']}: built from uncommitted source")
    if release and record["release"] != release:
        problems.append(f"the notices are for '{record['release']}', not {release}")
    for text in ("README.md", "LEGAL.txt", "licenses/README.txt"):
        body = (title / text).read_text(encoding="utf-8", errors="replace")
        # The examples' address is the only one a document may name.
        for address in set(re.findall(r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b", body)) - {"192.168.1.10"}:
            problems.append(f"{text} names a network address: {address}")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("title", type=Path)
    parser.add_argument("--release", help="the release's tag: refuses uncommitted source, and names the release in the notices")
    parser.add_argument("--check", action="store_true", help="only check a folder already staged")
    arguments = parser.parse_args()
    title = arguments.title.resolve()
    try:
        if not arguments.check:
            result = stage(title, arguments.release)
            print(f"notices: {len(result['components'])} parts in {title / 'licenses'}")
        problems = check(title, arguments.release)
    except NoticeError as error:
        print(f"notices: {error}", file=sys.stderr)
        return 1
    for problem in problems:
        print(f"notices: {problem}", file=sys.stderr)
    if not problems:
        print("notices: the folder passes the release check" if arguments.release else "notices: the folder passes the check")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
