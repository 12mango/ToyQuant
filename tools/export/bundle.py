#!/usr/bin/env python3
"""Bundle the repository for a reader that cannot clone it, and check a bundle against the tree.

A codebase dump handed to an assistant or pasted into a chat is a **copy**, and a copy cannot say how old it is: the
snapshot this tool exists for was ten days behind and described a README three times the length of the real one. So the
dump and its manifest are one artifact, the manifest carries the commit and a sha256 per file, and `--check`
recomputes them -- the same rule the rest of this repository applies to a number: if it cannot be re-derived, it is not
a result.

Two bundles rather than one, because the two halves answer different questions: `code.txt` is what a change touches,
`docs.txt` is what a claim lives in, and a reader that needs one rarely needs the other.

    python3 tools/export/bundle.py --emit --out /tmp/tq-export      # code.txt, docs.txt, manifest.txt
    python3 tools/export/bundle.py --check --out /tmp/tq-export     # 0 when the bundle is this tree
    TOYQUANT_DRIVE_DIR=... python3 tools/export/bundle.py --sync    # the same, plus diff.patch since the last sync

The sync is the workflow step: it writes the bundle into the folder a Drive (or any) client uploads from, adds
`diff.patch` -- the change since the commit the previous bundle carried, over the bundled paths only -- and then checks
what it wrote. That diff is what a read-only reviewer is given: a review needs the change, not the tree.

The file list is `git ls-files`, so it is the tree's own answer to "what is in this repository": ignored paths
(`data/v2`, `secrets/`, `out/`) are absent, and the commit in the manifest is the one those bytes came from.
"""
import argparse
import hashlib
import io
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

CODE_PREFIXES = ("src/", "tests/", "tools/", "CMakeLists.txt", "CMakePresets.json")
DOC_PREFIXES = ("docs/", "mkdocs.yml", "README.md", "CHANGELOG.md", ".clinerules", "LICENSE")
# A bundle is text a reader reads: a file over this size is data or a generated table, and belongs to neither half.
MAX_BYTES = 2_000_000
BANNER = "===== {} ({} lines) sha256={} ====="
# The folder a Drive (or any) client syncs. A sync that cannot find it is an error that names it, never a quiet skip:
# a snapshot nobody refreshed is exactly the failure this tool exists for.
DRIVE_ENV = "TOYQUANT_DRIVE_DIR"


def tracked_files():
    """Every file git tracks, in path order: the tree's own list, not a walk that re-derives the ignore rules."""
    out = subprocess.run(["git", "ls-files"], check=True, capture_output=True, text=True).stdout
    return [line for line in out.splitlines() if line.strip()]


def classify(path):
    if path.startswith(CODE_PREFIXES):
        return "code"
    if path.startswith(DOC_PREFIXES):
        return "docs"
    return None


def read_text(path):
    data = Path(path).read_bytes()
    if len(data) > MAX_BYTES:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def commit_id():
    return subprocess.run(["git", "rev-parse", "HEAD"], check=True, capture_output=True,
                          text=True).stdout.strip()


def emit(out_dir, since=None):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    halves = {"code": io.StringIO(), "docs": io.StringIO()}
    manifest = [f"commit {commit_id()}"]
    if since:
        manifest.append(f"since {since}")
    manifest += [f"generated {datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}",
                 "sha256\tlines\tpath"]
    counts = {"code": [0, 0], "docs": [0, 0]}
    for path in tracked_files():
        half = classify(path)
        if half is None:
            continue
        text = read_text(path)
        if text is None:
            continue
        digest = hashlib.sha256(text.encode()).hexdigest()
        lines = text.count("\n") + (0 if text.endswith("\n") or not text else 1)
        halves[half].write(BANNER.format(path, lines, digest) + "\n")
        halves[half].write(text if text.endswith("\n") else text + "\n")
        manifest.append(f"{digest}\t{lines}\t{path}")
        counts[half][0] += 1
        counts[half][1] += lines
    for half, buffer in halves.items():
        (out / f"{half}.txt").write_text(buffer.getvalue(), encoding="utf-8")
    manifest.append(f"# code: {counts['code'][0]} files, {counts['code'][1]} lines")
    manifest.append(f"# docs: {counts['docs'][0]} files, {counts['docs'][1]} lines")
    (out / "manifest.txt").write_text("\n".join(manifest) + "\n", encoding="utf-8")
    if since:
        (out / "diff.patch").write_text(diff_since(since), encoding="utf-8")
    print(f"wrote {out}/code.txt, {out}/docs.txt, {out}/manifest.txt"
          + (f", {out}/diff.patch" if since else "")
          + f" ({counts['code'][0]} code files, {counts['docs'][0]} docs files)")
    return 0


def diff_since(ref):
    """The change since the last sync, over the bundled paths only: what a reviewer has to read, and nothing else."""
    paths = [path for path in tracked_files() if classify(path) is not None]
    if not paths:
        return ""
    result = subprocess.run(["git", "diff", "--no-color", "--no-ext-diff", ref, "HEAD", "--", *paths],
                            check=True, capture_output=True, text=True)
    return result.stdout


def previous_commit(out_dir):
    """The commit the last bundle in this directory was made from, if there is one."""
    manifest = Path(out_dir) / "manifest.txt"
    if not manifest.is_file():
        return None
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if line.startswith("commit "):
            return line.split(" ", 1)[1].strip()
    return None


def sync(out_dir, since=None):
    """Emit, then check what was emitted, then say which commit it is and how much the reviewer has to read."""
    base = since or previous_commit(out_dir)
    status = emit(out_dir, since=base)
    if status != 0:
        return status
    patch = Path(out_dir) / "diff.patch"
    changed = 0
    if base and patch.is_file():
        changed = sum(1 for line in patch.read_text(encoding="utf-8").splitlines()
                      if line.startswith("diff --git "))
    status = check(out_dir)
    print(f"synced {commit_id()[:7]}"
          + (f" (from {base[:7]}: {changed} file(s) in diff.patch)" if base else "")
          + f" -> {out_dir}")
    return status


def check(out_dir):
    """The bundle against the tree: the files it names, their hashes, and whether the commit has moved."""
    manifest = Path(out_dir) / "manifest.txt"
    if not manifest.is_file():
        print(f"FAIL {manifest} is not a manifest this tool wrote")
        return 1
    stated = {}
    snapshot_commit = ""
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if line.startswith("commit "):
            snapshot_commit = line.split(" ", 1)[1]
        elif line.startswith(("#", "sha256", "generated ", "since ")) or not line.strip():
            continue
        else:
            digest, _lines, path = line.split("\t", 2)
            stated[path.strip()] = digest
    problems = []
    current = {}
    for path in tracked_files():
        if classify(path) is None:
            continue
        text = read_text(path)
        if text is None:
            continue
        current[path] = hashlib.sha256(text.encode()).hexdigest()
    for path, digest in sorted(stated.items()):
        if path not in current:
            problems.append(f"{path} is in the bundle and not in the tree")
        elif current[path] != digest:
            problems.append(f"{path} changed since the bundle")
    for path in sorted(set(current) - set(stated)):
        problems.append(f"{path} is in the tree and not in the bundle")
    head = commit_id()
    if snapshot_commit and snapshot_commit != head:
        print(f"note: the bundle is from {snapshot_commit[:7]} and HEAD is {head[:7]}")
    for problem in problems[:20]:
        print(f"FAIL {problem}")
    if len(problems) > 20:
        print(f"FAIL ... and {len(problems) - 20} more")
    print(f"checked {len(stated)} bundled file(s) against {len(current)} tracked, {len(problems)} difference(s)")
    return 1 if problems else 0


def self_test():
    """The checker has to catch a changed hash and a dropped file, or it is a promise rather than a check."""
    import shutil
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "bundle"
        assert emit(out) == 0
        assert check(out) == 0, "a fresh bundle does not match the tree it was made from"
        manifest = out / "manifest.txt"
        original = manifest.read_text(encoding="utf-8")
        lines = original.splitlines()
        # Corrupt one hash: the checker must notice without the tree being touched.
        for index, line in enumerate(lines):
            if line and not line.startswith(("commit ", "generated ", "sha256 ", "#")):
                _digest, rest = line.split("\t", 1)
                lines[index] = ("0" * 64) + "\t" + rest
                break
        manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
        assert check(out) == 1, "a changed hash was accepted"
        manifest.write_text(original, encoding="utf-8")
        assert check(out) == 0
        # A file the bundle does not mention.
        kept = [line for line in original.splitlines()
                if not (line and not line.startswith(("commit ", "generated ", "sha256 ", "#")))]
        manifest.write_text("\n".join(kept[:-2]) + "\n", encoding="utf-8")
        assert check(out) == 1, "a missing file was accepted"
        # A sync from a known base: the diff is the artifact a reviewer reads instead of the tree.
        assert emit(out, since="HEAD~1") == 0
        patch = out / "diff.patch"
        assert patch.is_file() and "diff --git " in patch.read_text(encoding="utf-8"), "no diff for a known base"
        assert check(out) == 0, "a manifest with a `since` line does not check"
        # The next sync finds the base in the manifest it just wrote, so a second sync is a no-op diff.
        assert sync(out) == 0
        assert previous_commit(out) == commit_id(), "sync did not record the commit it was made from"
        shutil.rmtree(out)
    print("self-test OK")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emit", action="store_true", help="write code.txt, docs.txt and manifest.txt")
    parser.add_argument("--check", action="store_true", help="compare a bundle with this tree")
    parser.add_argument("--sync", action="store_true",
                        help=f"emit into the folder a client syncs (${DRIVE_ENV}), with a diff since the last sync, "
                             "then check it")
    parser.add_argument("--since", help="the commit the diff starts at (default: the last bundle's commit)")
    parser.add_argument("--out", help="where the bundle lives (default out/export, and the sync folder for --sync)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if sum((args.emit, args.check, args.sync)) != 1:
        parser.error("choose one of --emit, --check or --sync")
    if args.sync:
        target = args.out or os.environ.get(DRIVE_ENV, "")
        if not target:
            parser.error(f"--sync needs a folder: pass --out, or set {DRIVE_ENV} to the directory your client syncs")
        if not Path(target).is_dir() and not Path(target).parent.is_dir():
            parser.error(f"{target} does not exist and neither does its parent: create the synced folder first")
        return sync(target, since=args.since)
    out = args.out or "out/export"
    if args.emit:
        return emit(out, since=args.since)
    return check(out)


if __name__ == "__main__":
    sys.exit(main())
