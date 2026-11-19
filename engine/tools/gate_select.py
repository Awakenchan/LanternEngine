# SPDX-License-Identifier: GPL-3.0-or-later
import fnmatch
import os
import re
import subprocess
import sys

ENGINE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(ENGINE)
SEARCH = ["", "src", "gpu", "gpu/shim", "gpu/shim/engine", "gpu/shadps4", "gpu/bbnet",
          "gpu/bbnet/vendor/bbhost_shim", "gpu/bbnet/vendor", "tests"]
SOURCE = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc")
SCANNED = SOURCE + (".cs", ".py", ".sh")
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
LITERAL = re.compile(r'"([A-Za-z0-9_.\-/]+\.[A-Za-z0-9_]+)"')
WHOLE = ["engine/build.sh", "engine/tests/gate-groups.txt", "engine/tools/gate_select.py"]
INERT = ["notes/*", "faq/*", "*.md", "package-template/*", "server/*", "tools/*",
         ".github/*", "build.ps1", "Build.exe", "NOTICE", ".gitignore", ".gitattributes"]
GUARDED = ("engine/", "launcher/")
RESOLVED = {}
EDGES = {}


def git(*args):
    return subprocess.run(["git", "-c", "core.quotepath=off", "-C", ROOT, *args],
                          capture_output=True, check=True).stdout.decode("utf-8")


def name_status(*args):
    fields = git("diff", "--name-status", "--no-renames", "-z", *args).split("\0")
    pairs = []
    i = 0
    while i + 1 < len(fields) and fields[i]:
        status = fields[i]
        paths = fields[i + 1:i + 3] if status[0] in "RC" else fields[i + 1:i + 2]
        pairs.extend((status[0], p) for p in paths)
        i += 1 + len(paths)
    return pairs


def changed_files():
    listed = os.environ.get("BB_GATE_CHANGED")
    if listed:
        with open(listed, encoding="utf-8") as handle:
            return "listed in " + listed, [("M", line.strip()) for line in handle if line.strip()]
    base = os.environ.get("BB_GATE_BASE")
    if not base:
        head = git("rev-parse", "HEAD").strip()
        trunk = os.environ.get("BB_GATE_TRUNK", "beta-candidate")
        base = git("merge-base", "HEAD", trunk).strip()
        if base == head:
            raise RuntimeError("on trunk with no BB_GATE_BASE")
    base = git("rev-parse", "--verify", base + "^{commit}").strip()
    entries = name_status(base, "HEAD") + name_status("HEAD")
    entries += [("A", p) for p in git("ls-files", "--others", "--exclude-standard", "-z").split("\0") if p]
    return "base " + base[:10], entries


def existing(name, here):
    found = []
    for directory in [here, ENGINE, ROOT] + [os.path.join(ENGINE, d) for d in SEARCH]:
        path = os.path.normpath(os.path.join(directory, name))
        if os.path.isfile(path):
            found.append(path)
    return found


def edges(path):
    if path in EDGES:
        return EDGES[path]
    found = []
    if path.endswith(SCANNED):
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        here = os.path.dirname(path)
        names = INCLUDE.findall(text) if path.endswith(SOURCE) else []
        for name in names + LITERAL.findall(text):
            key = (name, here)
            if key not in RESOLVED:
                RESOLVED[key] = existing(name, here)
            found.extend(RESOLVED[key])
    EDGES[path] = found
    return found


def closure(roots):
    seen = set()
    stack = list(roots)
    while stack:
        path = stack.pop()
        if path in seen or not os.path.isfile(path):
            continue
        seen.add(path)
        stack.extend(edges(path))
    return {os.path.relpath(p, ROOT).replace(os.sep, "/") for p in seen}


def everything(where, reason):
    print("ALL")
    print("gate fast: %s, %s, every group runs" % (where, reason), file=sys.stderr)


def main():
    groups = {}
    always = set()
    with open(os.path.join(ENGINE, "tests", "gate-groups.txt"), encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            name, _, rest = line.partition(":")
            tokens = rest.split()
            if "@always" in tokens:
                always.add(name)
            roots, patterns = [], []
            for token in tokens:
                if token == "@always":
                    continue
                rooted = token.startswith("/")
                relative = token[1:] if rooted else token
                if any(c in token for c in "*?["):
                    patterns.append(relative if rooted else "engine/" + relative)
                else:
                    roots.append(os.path.normpath(os.path.join(ROOT if rooted else ENGINE, relative)))
            groups[name] = (closure(roots), patterns)
    try:
        where, changed = changed_files()
    except (subprocess.CalledProcessError, OSError, RuntimeError) as error:
        everything("no change list", str(error).strip().splitlines()[0] if str(error).strip() else "git failed")
        return
    run = set(always)
    unmapped = []
    for status, path in changed:
        if path in WHOLE:
            everything(where, path + " changed")
            return
        if status in "DR" and path.startswith(GUARDED):
            everything(where, path + " deleted or renamed")
            return
        hit = False
        for name, (files, patterns) in groups.items():
            if path in files or any(fnmatch.fnmatchcase(path, p) for p in patterns):
                run.add(name)
                hit = True
        if not hit and not any(fnmatch.fnmatchcase(path, p) for p in INERT) and path.startswith(GUARDED):
            unmapped.append(path)
    if unmapped:
        everything(where, "no group maps " + " ".join(unmapped[:5]))
        return
    print(" ".join(sorted(run)))
    print("gate fast: %s, %d change(s), %d of %d groups run" % (where, len(changed), len(run), len(groups)), file=sys.stderr)


main()
