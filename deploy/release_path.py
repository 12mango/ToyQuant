#!/usr/bin/env python3
"""Decide whether a change can travel the config-only release path, or needs a binary.

The deploy card's Cut 2. A diff that touches only the six paths below ships through `rsync` and `activate.sh` -- no build,
no tests, no tar, no rule audit -- because none of them is read by the program. Everything else takes the full path, which
is unchanged.

**Why the allowlist is one constant and why it is this narrow.** The failure mode a fast path invites is a change that
*needs* a binary classified as light: a unit file that starts the program with a flag the running build does not read, or a
config key `live_config.py` emits and `app_config.cpp` refuses. So `tools/live/*.py` -- the reader that turns the operator's
file into that command line -- is **not** light, `deploy/*.sh` is **not** light (a changed `activate.sh` must travel with
the audit that reads it), and an empty change list is full: a classifier that answers "light" when it was told nothing is
the one failure that would ship nothing and call it a deploy.

    python3 deploy/release_path.py --paths config/live.example.yaml,deploy/toyquant-report.timer
    git diff --name-only <base>..HEAD | python3 deploy/release_path.py --stdin
"""
import argparse
import re
import sys

# The six paths the card names, in the three shapes they come in: a directory, a unit pair, and an env template.
LIGHT_PREFIXES = ("config/", "deploy/journald/", "deploy/logrotate/")
LIGHT_UNIT = re.compile(r"^deploy/[A-Za-z0-9_.@-]+\.(service|timer)$")
LIGHT_ENV = re.compile(r"^deploy/[A-Za-z0-9_.@-]+\.env\.example$")
# A path that is light by suffix but names a program is the defect this classifier exists to refuse.
PROGRAM = re.compile(r"(^|/)(toy_quant|toy_quant\.prev|[^/]+\.so|[^/]+\.py|[^/]+\.sh)$")


def light(path):
    """True when one path may travel without a binary; the regexes are the allowlist, not a heuristic."""
    if not path.strip() or path.endswith("/"):
        return False
    if PROGRAM.search(path):
        return False
    return path.startswith(LIGHT_PREFIXES) or bool(LIGHT_UNIT.match(path)) or bool(LIGHT_ENV.match(path))


def classify(paths):
    """("light" | "full", the path that decided it). Every path must be light, and there must be at least one."""
    paths = [path.strip() for path in paths if path.strip()]
    if not paths:
        return "full", "no path was named"
    for path in paths:
        if not light(path):
            return "full", path
    return "light", ", ".join(paths)


def self_test():
    cases = {
        "config/live.example.yaml": "light",
        "deploy/toyquant-live.service": "light",
        "deploy/toyquant-report.timer": "light",
        "deploy/notify.env.example": "light",
        "deploy/journald/toyquant.conf": "light",
        "deploy/logrotate/toyquant": "light",
        "src/app/app_config.cpp": "full",
        "tools/live/live_config.py": "full",
        "deploy/activate.sh": "full",
        "deploy/release_path.py": "full",
        "out/build/linux-debug/toy_quant": "full",
        "config/": "full",
    }
    for path, expected in cases.items():
        got, why = classify([path])
        assert got == expected, (path, got, why)
    # One full path makes the whole change full: the audit reads the tree, not a prefix of it.
    assert classify(["config/live.example.yaml", "src/main.cpp"])[0] == "full"
    # And nothing named is not light.
    assert classify([])[0] == "full"
    print(f"release_path self-test OK ({len(cases)} paths, one mixed list, one empty list)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--paths", help="comma separated changed paths")
    parser.add_argument("--stdin", action="store_true", help="read the changed paths, one a line, from stdin")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.stdin:
        paths = [line.rstrip("\n") for line in sys.stdin]
    elif args.paths:
        paths = args.paths.split(",")
    else:
        parser.error("name --paths, --stdin or --self-test")
    verdict, why = classify(paths)
    print(verdict)
    print(f"release_path: {verdict} -- {why}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
