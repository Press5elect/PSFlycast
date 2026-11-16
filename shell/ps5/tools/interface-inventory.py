#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
"""PSFlyCast - nothing is lost in a change to the interface.

The interface (shell/ps5/bigpicture.cpp and its .inc files) says everything the player can do in
strings: a setting's name, a heading, a button, an entry of the quick menu, a message. This tool
keeps the list of those at the last release, and fails when one is gone from the code without being
acknowledged (renamed, moved to another word, or taken away on purpose, each with its reason).
It is the no-loss test of the revamp playbook, for an interface that is one file.

  interface-inventory.py check                 compare the tree with the baseline (exit 1 on a loss)
  interface-inventory.py make --from <rev>     write the baseline from a revision of the sources
  interface-inventory.py list                  every string of the tree that is in the baseline's form

The baseline is shell/ps5/docs/dev/interface-inventory.json: "strings" (the labels at the release
named in "from"), "acknowledged" (string -> why it is no longer there) and "added" is not kept.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASELINE = ROOT / "shell/ps5/docs/dev/interface-inventory.json"
FILES = ["shell/ps5/bigpicture.cpp", "shell/ps5/bigpicture.h"]
GLOBS = ["shell/ps5/bigpicture_*.inc"]


def literals(source):
    """The string literals of C++ source, comments left out."""
    out, i, n = [], 0, len(source)
    while i < n:
        c = source[i]
        if source.startswith("//", i):
            j = source.find("\n", i)
            i = n if j < 0 else j
        elif source.startswith("/*", i):
            j = source.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == "'":
            i += 1
            while i < n and source[i] != "'":
                i += 2 if source[i] == "\\" else 1
            i += 1
        elif c == '"':
            j = i + 1
            while j < n and source[j] != '"':
                j += 2 if source[j] == "\\" else 1
            out.append(source[i + 1:j])
            i = j + 1
        else:
            i += 1
    return out


def wanted(text):
    """A label the player reads: a short text that starts with a capital or a digit and has a letter."""
    if not 3 <= len(text) <= 80 or "%" in text or "\\n" in text:
        return False
    return text[0].isupper() or text[0].isdigit() if any(ch.isalpha() for ch in text) else False


def of_sources(read):
    seen = set()
    for name in read():
        for lit in literals(name[1]):
            if wanted(lit):
                seen.add(lit)
    return seen


def tree():
    names = [ROOT / f for f in FILES]
    for g in GLOBS:
        names += sorted(ROOT.glob(g))
    for p in names:
        if p.exists():
            yield p.name, p.read_text(encoding="utf-8", errors="replace")


def revision(rev):
    names = FILES + [f for f in subprocess.check_output(["git", "-C", str(ROOT), "ls-tree", "--name-only", rev, "shell/ps5/"], text=True).split()
                     if f.startswith("shell/ps5/bigpicture_") and f.endswith(".inc")]
    for f in names:
        try:
            yield f, subprocess.check_output(["git", "-C", str(ROOT), "show", f"{rev}:{f}"], text=True, errors="replace")
        except subprocess.CalledProcessError:
            continue


def load():
    return json.loads(BASELINE.read_text(encoding="utf-8"))


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "check"
    if cmd == "make":
        rev = argv[argv.index("--from") + 1]
        old = load() if BASELINE.exists() else {}
        data = {"about": "The labels of the interface at the release in 'from'; see shell/ps5/tools/interface-inventory.py.",
                "from": rev, "strings": sorted(of_sources(lambda: revision(rev))), "acknowledged": old.get("acknowledged", {})}
        BASELINE.write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"baseline: {len(data['strings'])} strings from {rev}")
        return 0
    now = of_sources(tree)
    if cmd == "list":
        print("\n".join(sorted(now)))
        return 0
    base = load()
    lost = [s for s in base["strings"] if s not in now and s not in base["acknowledged"]]
    stale = [s for s in base["acknowledged"] if s in now]
    kept = len(base["strings"]) - len(lost) - len([s for s in base["strings"] if s in base["acknowledged"] and s not in now])
    if lost:
        print(f"interface: {len(lost)} string(s) of {base['from']} are gone from the code and not acknowledged:")
        for s in lost:
            print("  -", s)
        print("Put each back, or add it to \"acknowledged\" in", BASELINE.relative_to(ROOT), "with the reason (renamed, moved to ...).")
        return 1
    print(f"interface: nothing lost ({kept} of {len(base['strings'])} strings of {base['from']} still there, "
          f"{len(base['acknowledged'])} acknowledged, {len(now)} in the tree)")
    if stale:
        print(f"({len(stale)} acknowledged string(s) are back in the code: they can be taken off the list)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
