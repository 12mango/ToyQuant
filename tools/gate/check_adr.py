#!/usr/bin/env python3
"""Check that the decision records and their index agree.

`docs/adr/` is append-only: a record's status line is the only part that changes, and `index.md` carries the same state in
a table so that a reader can see the whole set at once. Two places holding one fact is exactly the drift this repository
spends its gates on, so the agreement is checked rather than promised:

  * every record has a row in the index, and every row names a record that exists;
  * the id in the row is the id in the file name;
  * the state column and the record's `Status:` line are the same words;
  * a `superseded by NNNN` status points at a record that exists and whose `Supersedes:` names it back, because a
    one-sided supersede is how a chain of decisions quietly loses its middle.

The last one is the check that pays: the other three catch a typo, and that one catches a decision that was replaced and
never said so.

Usage:
    python3 tools/gate/check_adr.py                 # docs/adr relative to the repository root
    python3 tools/gate/check_adr.py docs/adr        # or a directory
    python3 tools/gate/check_adr.py --self-test     # the four cases, on a temporary directory
"""

import argparse
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DIR = ROOT / "docs/adr"
RECORD = re.compile(r"^(\d{4})-[a-z0-9-]+\.md$")
ROW = re.compile(r"^\|\s*\[(\d{4})\]\(([^)]+)\)\s*\|(.+)$")
STATUS = re.compile(r"^-\s*\*\*Status\*\*:\s*(.+?)\s*$", re.MULTILINE)
SUPERSEDES = re.compile(r"^-\s*\*\*Supersedes\*\*:\s*(.+?)\s*$", re.MULTILINE)
SUPERSEDED_BY = re.compile(r"superseded by\s+(\d{4})", re.IGNORECASE)


def normalise(text):
    """Lower case, single spaces: the table and the line differ by whitespace and by nothing else."""
    return " ".join(text.split()).lower()


def read_records(directory):
    """{id: {"path": Path, "status": str, "supersedes": str}} for every record file."""
    records = {}
    for path in sorted(directory.iterdir()):
        match = RECORD.match(path.name)
        if not match:
            continue
        text = path.read_text(encoding="utf-8")
        status = STATUS.search(text)
        supersedes = SUPERSEDES.search(text)
        records[match.group(1)] = {
            "path": path,
            "status": normalise(status.group(1)) if status else "",
            "supersedes": normalise(supersedes.group(1)) if supersedes else "",
        }
    return records


def read_index(directory):
    """[(id, file_name, state)] for every row of the table in `index.md`, or None when it is missing."""
    index = directory / "index.md"
    if not index.exists():
        return None
    rows = []
    for line in index.read_text(encoding="utf-8").splitlines():
        match = ROW.match(line.strip())
        if not match:
            continue
        cells = [cell.strip() for cell in match.group(3).split("|")]
        # `| [id](file) | date | decision | state |` leaves date, decision, state and a trailing empty cell.
        state = cells[-2] if len(cells) >= 3 else ""
        rows.append((match.group(1), match.group(2), normalise(state)))
    return rows

def check(directory):
    """Returns the list of problems, each a sentence naming the file it is about."""
    records = read_records(directory)
    rows = read_index(directory)
    if rows is None:
        return ["index.md is missing, so there is nothing to keep the records honest"]
    problems = []
    if not records:
        problems.append(f"no decision record in {directory}: an index with no records is a table of promises")

    listed = {}
    for record_id, file_name, state in rows:
        listed[record_id] = state
        if not (directory / file_name).exists():
            problems.append(f"index.md lists {record_id} as {file_name}, which does not exist")
        elif not file_name.startswith(record_id):
            problems.append(f"index.md lists {record_id} against {file_name}, whose number is a different one")

    for record_id, record in records.items():
        name = record["path"].name
        if not record["status"]:
            problems.append(f"{name} has no `- **Status**:` line")
            continue
        if record_id not in listed:
            problems.append(f"{name} is not in index.md, so a reader browsing the set cannot see it")
            continue
        if listed[record_id] != record["status"]:
            problems.append(f"{name} says status {record['status']!r} and index.md says {listed[record_id]!r}")

    # A supersede has to be stated by both ends, which is the whole point of the record being append-only.
    for record_id, record in records.items():
        replaced = SUPERSEDED_BY.search(record["status"])
        if replaced:
            successor = replaced.group(1)
            if successor not in records:
                problems.append(f"{record['path'].name} is superseded by {successor}, which does not exist")
            elif record_id not in records[successor]["supersedes"]:
                problems.append(
                    f"{record['path'].name} is superseded by {successor}, but {records[successor]['path'].name} "
                    f"does not name {record_id} in its Supersedes line"
                )
        for older in re.findall(r"\d{4}", record["supersedes"]):
            if older not in records:
                problems.append(f"{record['path'].name} supersedes {older}, which does not exist")
                continue
            back = SUPERSEDED_BY.search(records[older]["status"])
            if not back or back.group(1) != record_id:
                problems.append(
                    f"{record['path'].name} supersedes {older}, but {records[older]['path'].name} does not say "
                    f"it was superseded by {record_id}"
                )
    return problems


INDEX_HEADER = "| # | date | supersedes | decision | state |\n|---|---|---|---|---|\n"


def bucket(rows):
    """A whole index from (id, file, state) rows, so the self-test writes one shape rather than three."""
    body = "".join(
        f"| [{record_id}]({file_name}) | 2026-01-01 | none | {record_id} | {state} |\n"
        for record_id, file_name, state in rows)
    return INDEX_HEADER + body


def self_test():
    """Four cases on a temporary directory: consistent, drifted status, orphan row, one-sided supersede."""
    with tempfile.TemporaryDirectory() as room:
        directory = Path(room)
        (directory / "0001-one.md").write_text("- **Status**: accepted\n- **Supersedes**: none\n")
        (directory / "0002-two.md").write_text("- **Status**: accepted\n- **Supersedes**: none\n")
        (directory / "index.md").write_text(
            bucket([("0001", "0001-one.md", "accepted"), ("0002", "0002-two.md", "accepted")]))
        assert check(directory) == [], check(directory)

        # The table drifts from the record: the case the tool exists for.
        (directory / "index.md").write_text(
            bucket([("0001", "0001-one.md", "accepted"), ("0002", "0002-two.md", "rejected")]))
        drifted = check(directory)
        assert len(drifted) == 1 and "0002-two.md" in drifted[0] and "rejected" in drifted[0], drifted

        # A row naming a file that is not there.
        (directory / "index.md").write_text(
            bucket([("0001", "0001-one.md", "accepted"), ("0002", "0002-two.md", "accepted"),
                    ("0003", "0003-three.md", "accepted")]))
        orphan = check(directory)
        assert any("0003-three.md" in problem and "does not exist" in problem for problem in orphan), orphan

        # A supersede stated at one end only, which is how a chain loses its middle.
        (directory / "index.md").write_text(
            bucket([("0001", "0001-one.md", "superseded by 0002"), ("0002", "0002-two.md", "accepted")]))
        (directory / "0001-one.md").write_text("- **Status**: superseded by 0002\n- **Supersedes**: none\n")
        one_sided = check(directory)
        assert any("does not name 0001" in problem for problem in one_sided), one_sided

        # ... and stated at both ends, which is the shape the record format asks for.
        (directory / "0002-two.md").write_text("- **Status**: accepted\n- **Supersedes**: 0001\n")
        assert check(directory) == [], check(directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", nargs="?", type=Path, default=DEFAULT_DIR,
                        help="the decision records, `docs/adr` by default")
    parser.add_argument("--self-test", action="store_true",
                        help="run the four cases on a temporary directory and exit")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("self-test OK: consistent, drifted status, orphan row and one-sided supersede all reported correctly")
        return 0
    problems = check(args.directory)
    for problem in problems:
        print(f"FAIL {problem}")
    records = len(read_records(args.directory))
    print(f"checked {records} decision record(s) against {args.directory / 'index.md'}, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
