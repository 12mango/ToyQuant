#!/usr/bin/env python3
"""Verify that the numbers a document quotes are still what the code produces.

A document that quotes replay output is a claim about the code, and this project's claims are its
product. Hand-copied numbers drift: one already did, a "thirty times per fill" that a re-measurement
turned into ten. The comparison tables are therefore emitted by `tools/compare_runs.sh --markdown`,
which wraps them in a comment block carrying the exact command and the values it produced:

    <!-- toyquant:check
    run: l2_replay data/... 0 active_l2 1 conservative --fast-validation
    then: submitted_orders=406 trade_reports=1 captured_edge_per_unit_ticks=2.5
    -->

This runs each `run:` and fails when any `then:` value has moved. Only marked blocks are checked, so
historical prose is left alone and a table is checked exactly when someone decided it should be.

A block may also name the binary it runs, which is how a benchmark that is not the main binary is checked
at all:

    <!-- toyquant:check
    binary: ring_bench
    run: 200000 1
    then: messages=200000 checksum=15483742669132917760
    -->

The name is resolved relative to the binary the gate passes in, so it follows `--binary` and the preset
that binary came from. A block whose command exits non-zero fails, because a binary that crashed printed
none of the values the block is about.

A block may also name a `tool:` instead of a `run:`, which is how a number a Python tool derives is checked
rather than trusted:

    <!-- toyquant:check
    tool: python3 tools/pnl_interval.py --window btc-months
    then: n=5 mean=-1.87368 includes_zero=0
    -->

The line is executed with this interpreter, and the binary the gate passed in is appended to it as
`--binary <path>`, so a tool that replays windows follows the preset like a `run:` does. The interpreter and
the script's path are checked before the run, because a tool whose name moved should say so rather than
report a missing field.

Usage:
    python3 tools/check_docs.py docs/PERFORMANCE_HISTORY.md [more.md ...]
"""

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

MARKER = "<!-- toyquant:check"
END = "-->"
FIELD_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")


def check_blocks(text):
    """Yields (line_number, [(command, {field: value}), ...]) for every marked block.

    Parsed line by line rather than with one regex over the whole file: the block spans lines, the
    obvious pattern was not matching what it looked like it should, and a marker line is easier to
    read than a pattern that has to be debugged.
    """
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        if lines[index].strip() != MARKER:
            index += 1
            continue
        body = []
        cursor = index + 1
        while cursor < len(lines) and lines[cursor].strip() != END:
            body.append(lines[cursor])
            cursor += 1
        yield index + 1, parse_block(body)
        index = cursor + 1


def parse_block(body):
    """(kind, command, {field: value}, binary) in the order the block lists them.

    `binary:` is sticky for the runs that follow it, because a block describes one thing: a benchmark
    named once at the top rather than repeated on every line of it.

    `kind` is "run" for the binary and "tool" for a `tool:` line, which is executed by this interpreter
    instead. It is carried in the tuple rather than inferred later because the two are launched differently
    and a block is allowed to hold both.
    """
    runs = []
    command = None
    binary = None
    kind = "run"
    for raw_line in body:
        line = raw_line.strip()
        if line.startswith("binary:"):
            binary = line[len("binary:") :].strip()
        elif line.startswith("tool:"):
            if command is not None:
                runs.append((kind, command, {}, binary))
            kind = "tool"
            command = line[len("tool:") :].strip()
        elif line.startswith("run:"):
            if command is not None:
                runs.append((kind, command, {}, binary))
            kind = "run"
            command = line[len("run:") :].strip()
        elif line.startswith("then:") and command is not None:
            runs.append((kind, command, dict_of(line[len("then:") :]), binary))
            command = None
    if command is not None:
        runs.append((kind, command, {}, binary))
    return runs


def dict_of(text):
    """First occurrence wins, matching how compare_runs.sh reads a line."""
    values = {}
    for field, value in FIELD_RE.findall(text):
        values.setdefault(field, value)
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("documents", type=Path, nargs="+")
    parser.add_argument("--binary", default="out/build/linux-debug/toy_quant")
    args = parser.parse_args()

    checked = 0
    failures = []
    main_binary = Path(args.binary)
    for document in args.documents:
        text = document.read_text()
        for line_number, runs in check_blocks(text):
            for kind, command, expected, binary in runs:
                checked += 1
                if kind == "tool":
                    argv = shlex.split(command)
                    # A tool line is written as `python3 tools/x.py ...`; the interpreter is this one and the
                    # binary the gate passed in follows as a flag, so a tool that replays windows follows the
                    # preset the way a `run:` does rather than replaying with a path written into a document.
                    if argv and argv[0] in ("python3", "python"):
                        argv = argv[1:]
                    if not argv or not Path(argv[0]).exists():
                        failures.append(
                            f"{document}:{line_number}: {argv[0] if argv else command} does not exist, so "
                            f"the block cannot be run"
                        )
                        continue
                    argv = [sys.executable] + argv
                    if "--binary" not in argv:
                        argv += ["--binary", str(main_binary)]
                    completed = subprocess.run(argv, capture_output=True, text=True)
                    label = " ".join(argv[1:])
                else:
                    # A block that names a binary gets the one beside the binary the gate passed in, so the
                    # benchmark follows the preset the run is checking rather than a path written into a doc.
                    executable = main_binary if binary is None else main_binary.parent / binary
                    if not executable.exists():
                        failures.append(
                            f"{document}:{line_number}: {executable} is not built, so the block cannot be run"
                        )
                        continue
                    completed = subprocess.run(
                        [str(executable)] + shlex.split(command), capture_output=True, text=True
                    )
                    label = f"{executable} {command}"
                if completed.returncode != 0:
                    failures.append(
                        f"{document}:{line_number}: {label} exited "
                        f"{completed.returncode}: {completed.stderr.strip()[:200]}"
                    )
                    continue
                observed = dict_of(completed.stdout + completed.stderr)
                for field, want in expected.items():
                    have = observed.get(field)
                    if have != want:
                        failures.append(
                            f"{document}:{line_number}: {field} is {have!r}, "
                            f"the document says {want!r}"
                        )

    for failure in failures:
        print(f"FAIL {failure}")
    print(
        f"checked {checked} recorded run(s) in {len(args.documents)} document(s), "
        f"{len(failures)} mismatch(es)"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
