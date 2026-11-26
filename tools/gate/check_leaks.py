#!/usr/bin/env python3
"""The publication boundary, as a check.

The public repository carries the instrument and the method; the money, the seats and the machine are not its
business. This is the check for that boundary:

* **structural, always blocking** -- a secret shape, a machine path or a private file that is *tracked*. These are
  facts about this checkout rather than opinions: a key in a tracked file is a key in the history, and a tracked
  document naming `/home/<someone>/` or a Windows drive is a public repository telling every reader about a machine.
* **policy, from `private/leak_denylist.txt`** (ignored) -- the trading facts the owner keeps behind the boundary:
  instruments, sizes, money. Its `[block]` section is enforced; its `[audit]` section is *counted and reported* while
  the migration is in flight, so the same tool measures the job and then enforces it. A token moves from `[audit]` to
  `[block]` when the documents that name it have been moved, and the count reaching zero is the progress bar.

**The file list is `git ls-files`, never the working tree.** That is the list a reviewer receives -- the bundle is
`git ls-files` (`.clinerules` section 6) -- so an ignored file is never a finding, and `private/` is exactly where an
untracked file belongs.

    python3 tools/gate/check_leaks.py              # blocking rules, plus the policy count
    python3 tools/gate/check_leaks.py --strict     # the [audit] section blocks too (after the migration)
    python3 tools/gate/check_leaks.py --audit      # report only, exit 0: what is left and where
    python3 tools/gate/check_leaks.py --self-test  # the rules fire on what they name and not on a clean line
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DENYLIST = REPO_ROOT / "private" / "leak_denylist.txt"
# This file names every shape it refuses, so it would refuse itself. Skipping it by path is narrower than weakening a
# rule, and it is the one place where a `-----BEGIN ... PRIVATE KEY-----` line is documentation rather than a key.
SELF = "tools/gate/check_leaks.py"

# A secret shape is a leak wherever it appears, in any file type.
STRUCTURAL = (
    ("a private-key block", re.compile(r"-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----")),
    ("a GitHub token", re.compile(r"\bgh[pousr]_[A-Za-z0-9]{20,}\b|\bgithub_pat_[A-Za-z0-9_]{20,}\b")),
    ("an AWS access key", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("a Stripe secret", re.compile(r"\bsk_live_[A-Za-z0-9]{16,}\b")),
    ("a Slack token", re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{10,}\b")),
    # `G:\Users\...` and `/home/someone/...` are the two shapes a machine path takes here. The placeholder spellings a
    # document is allowed to use -- `X:/...`, `<the folder>`, `/opt/...` -- do not match.
    ("a Windows drive path", re.compile(r"(?<![A-Za-z0-9])[A-Za-z]:\\")),
    ("a home directory path", re.compile(r"/(?:home|Users|root)/[A-Za-z0-9._-]+/")),
    ("a cloud-drive folder name", re.compile(r"我的云端硬盘")),
    # A credential is leaked by its *value*: `OKX_API_SECRET=replace-with-...` is a template line and passes, while
    # sixteen or more non-placeholder characters after the `=` is a key in the tree.
    ("a filled credential",
     re.compile(r"(?i)\b(?:okx_api_key|okx_api_secret|okx_passphrase|api[_-]?secret|api[_-]?key)\s*=\s*"
                r"(?!(?:replace|your|\$|<|$|the-))[A-Za-z0-9/+_-]{16,}")),
)

# A path that must never be tracked. The two templates are the exception the pattern itself names.
PRIVATE_PATHS = (
    ("the live configuration", re.compile(r"(?:^|/)config/live[A-Za-z0-9._-]*\.ya?ml$")),
    ("the credentials", re.compile(r"(?:^|/)\.env(?:\.[A-Za-z0-9_-]+)?$")),
    ("the secrets directory", re.compile(r"(?:^|/)secrets/")),
    ("the private tree", re.compile(r"(?:^|/)private/")),
    ("the local reports", re.compile(r"(?:^|/)local_reports/")),
)
PRIVATE_EXCEPTIONS = {".env.example", "config/live.example.yaml"}

TEXT_SUFFIXES = {".md", ".txt", ".py", ".sh", ".yml", ".yaml", ".json", ".cpp", ".h", ".toml",
                 ".cfg", ".ini", ".service", ""}


def tracked_files() -> list[str]:
    out = subprocess.run(["git", "-C", str(REPO_ROOT), "ls-files", "-z"],
                         capture_output=True, check=True).stdout
    return [name for name in out.decode("utf-8", "replace").split("\0") if name]


def read(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except (OSError, UnicodeError):
        return None


def load_denylist(path: Path) -> tuple[list[str], list[str]]:
    """`[block]` tokens are enforced, `[audit]` tokens are counted; a token before any section is a `[block]` one."""
    blocking: list[str] = []
    auditing: list[str] = []
    section = "[block]"
    if not path.exists():
        return blocking, auditing
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line in ("[block]", "[audit]"):
            section = line
            continue
        (blocking if section == "[block]" else auditing).append(line)
    return blocking, auditing


def scan(files: list[str], blocking: list[str], auditing: list[str], strict: bool,
         report_only: bool) -> int:
    problems: list[str] = []
    policy_hits: dict[str, list[str]] = {token: [] for token in blocking + auditing}
    for name in files:
        if name == SELF:
            continue
        for label, pattern in PRIVATE_PATHS:
            if pattern.search(name) and name not in PRIVATE_EXCEPTIONS:
                problems.append(f"{label} is tracked: {name}")
        if Path(name).suffix not in TEXT_SUFFIXES and not name.startswith(".env"):
            continue
        text = read(REPO_ROOT / name)
        if text is None:
            continue
        for label, pattern in STRUCTURAL:
            for match in pattern.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                problems.append(f"{label} in {name}:{line}")
        lowered = text.lower()
        for token in policy_hits:
            if token.lower() in lowered:
                policy_hits[token].append(name)
    for token, hits in policy_hits.items():
        if not hits:
            continue
        if token in blocking or strict:
            problems.append(f"deny-listed token {token!r} in {len(hits)} file(s): "
                            + ", ".join(sorted(hits)[:4]) + (" ..." if len(hits) > 4 else ""))
        else:
            print(f"audit: {token!r} in {len(hits)} file(s) -- stage 2 of the migration")
    if report_only:
        for problem in problems:
            print("leak (report only): " + problem)
        if not problems:
            print("leak: no structural finding")
        return 0
    if problems:
        for problem in problems:
            print("leak: " + problem, file=sys.stderr)
        print(f"leak: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    print(f"leak: {len(files)} tracked file(s), no secret, machine path or private file; "
          f"{len(blocking)} blocking and {len(auditing)} auditing token(s)")
    return 0


def self_test() -> int:
    """Every rule fires on its own sample and none of them fires on a line the tree is allowed to carry."""
    rules = dict(STRUCTURAL)
    fires = {
        "a private-key block": "-----BEGIN OPENSSH PRIVATE KEY-----\nabc\n",
        "a GitHub token": "token: ghp_" + "a" * 24,
        "an AWS access key": "AKIA" + "Q" * 16,
        "a Stripe secret": "sk_live_" + "1" * 20,
        "a Windows drive path": r"copy out to G:\My Drive\sync",
        "a home directory path": "python3 /home/operator/tq_snapshot.py",
        "a cloud-drive folder name": "copying to 我的云端硬盘",
        "a filled credential": "OKX_API_SECRET=9f8e7d6c5b4a39281706",
    }
    for label, sample in fires.items():
        assert rules[label].search(sample), f"{label} did not fire on its own sample"
    clean = [
        "OKX_API_SECRET=replace-with-the-demo-secret",
        "OKX_API_KEY=<the key from the venue>",
        "OKX_PASSPHRASE=${OKX_PASSPHRASE}",
        "the folder named by .drive-target, which is X:/sync on the client",
        "`/opt/toyquant/config/live.yaml` and `/opt/toyquant/logs`",
        "a path under /tmp/tq/ is scratch",
    ]
    for sample in clean:
        for label, pattern in STRUCTURAL:
            assert not pattern.search(sample), f"{label} fired on a clean line: {sample!r}"
    # A private path is a finding by its name, and the two templates are legal because they are named as exceptions --
    # which is a fact about the file list rather than about the pattern, so the test pins the list.
    paths = dict(PRIVATE_PATHS)
    assert paths["the live configuration"].search("config/live.yaml")
    assert paths["the live configuration"].search("config/live.prod.yaml")
    assert paths["the credentials"].search(".env")
    assert paths["the private tree"].search("private/docs/studies.md")
    assert {"config/live.example.yaml", ".env.example"} <= PRIVATE_EXCEPTIONS
    print("self-test OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--strict", action="store_true",
                        help="the deny-list's [audit] section blocks too, which is where it stays once stage 2 is done")
    parser.add_argument("--audit", action="store_true", help="report only; exit 0 whatever is found")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--denylist", type=Path, default=DENYLIST)
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    blocking, auditing = load_denylist(arguments.denylist)
    if not arguments.denylist.exists():
        print(f"leak: no deny-list at {arguments.denylist}; structural rules only. The policy list is a local file "
              f"because the tokens it carries are the thing being kept private.")
    return scan(tracked_files(), blocking, auditing, arguments.strict, arguments.audit)


if __name__ == "__main__":
    sys.exit(main())
