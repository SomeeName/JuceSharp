#!/usr/bin/env python3
"""Validate every relative Markdown link target in the JuceSharp docs.

Walks README.md, CONTRIBUTING.md, SECURITY.md, CHANGELOG.md, docs/**/*.md and
every src/*/README.md (the packed package readmes),
extracts each `](target)` link, skips absolute URLs / mailto / bare anchors,
strips any `#fragment`, and checks the resolved path exists on disk.

Usage:  python tools/check-doc-links.py        (from the repo root)
Exit:   0 = every link resolves, 1 = at least one is dead.
"""
import os
import re
import sys
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LINK = re.compile(r"\]\(\s*([^)\s]+?)\s*(?:\s+\"[^\"]*\")?\)")
FENCE = re.compile(r"^\s*```")
SKIP_PREFIX = ("http://", "https://", "mailto:", "#", "data:", "ftp://")


def markdown_files():
    for name in ("README.md", "CONTRIBUTING.md", "SECURITY.md", "CHANGELOG.md"):
        p = os.path.join(ROOT, name)
        if os.path.isfile(p):
            yield p
    src = os.path.join(ROOT, "src")
    if os.path.isdir(src):
        for proj in sorted(os.listdir(src)):
            p = os.path.join(src, proj, "README.md")
            if os.path.isfile(p):
                yield p
    for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, "docs")):
        dirnames[:] = [d for d in dirnames if d not in ("node_modules", "_book")]
        for fn in sorted(filenames):
            if fn.endswith(".md"):
                yield os.path.join(dirpath, fn)


def links_in(path):
    """Yield (line_no, target) for links outside fenced code blocks."""
    with open(path, encoding="utf-8") as fh:
        in_fence = False
        for i, line in enumerate(fh, 1):
            if FENCE.match(line):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            for m in LINK.finditer(line):
                yield i, m.group(1)


def main():
    checked = 0
    dead = []
    for path in markdown_files():
        base = os.path.dirname(path)
        for line_no, target in links_in(path):
            if target.startswith(SKIP_PREFIX):
                continue
            clean = urllib.parse.unquote(target.split("#", 1)[0])
            if not clean:
                continue
            checked += 1
            resolved = os.path.normpath(os.path.join(base, clean))
            if not os.path.exists(resolved):
                dead.append((os.path.relpath(path, ROOT).replace("\\", "/"),
                             line_no, target))

    print("checked %d relative links across %d files"
          % (checked, len(list(markdown_files()))))
    if dead:
        print("\nDEAD LINKS (%d):" % len(dead))
        for f, line, target in dead:
            print("  %s:%d  ->  %s" % (f, line, target))
        return 1
    print("all relative links resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
