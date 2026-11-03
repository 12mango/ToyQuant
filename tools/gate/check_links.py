#!/usr/bin/env python3
"""Check that every relative markdown link in the tree points at a file, and that every anchor exists.

The documents are cross-referenced heavily and the references are how a reader moves between tiers, so a link that
points at a moved file is a dead end nobody notices: nothing in the build read a link until this did. Two things are
checked, in the order a reader would hit them:

  * the file exists, resolved relative to the document that links it;
  * the `#anchor`, if there is one, matches a heading in that file -- with GitHub's own slug rules, which is what a
    reader's browser will look for.

Anchors are the half that actually rots. A file can be renamed by a `git mv` and a careful author will update the
references to it; a `## Section` that is reworded inside the file breaks every deep link to it, in files the author was
not looking at.

Usage:
    python3 tools/gate/check_links.py            # the whole tree
    python3 tools/gate/check_links.py docs/*.md  # only the documents given, still following their links
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LINK = re.compile(r"\]\(([^)\s]+)\)")
HEADING = re.compile(r"^#{1,6}\s+(.*)$", re.MULTILINE)


def slug(title):
    """GitHub's anchor for a heading: lowercase, drop what is not a letter, digit, space, hyphen or underscore."""
    text = title.strip().lower()
    text = re.sub(r"`", "", text)
    text = re.sub(r"[^a-z0-9 \-_]", "", text)
    return re.sub(r" +", "-", text).strip("-")


def headings_of(path):
    return {slug(match) for match in HEADING.findall(path.read_text())}


def targets_of(path):
    for raw in LINK.findall(path.read_text()):
        if raw.startswith(("http://", "https://", "mailto:")):
            continue
        if raw.startswith("#"):
            yield path, raw[1:]
            continue
        target, _, anchor = raw.partition("#")
        if not target.endswith(".md"):
            continue
        yield (path.parent / target).resolve(), anchor


def malformed_count(path):
    """`](` occurrences the link pattern could not read: an unbalanced paren or a space inside the target.

    A broken link is invisible to a pattern that insists on well-formed one, which is exactly how a repair that dropped
    a closing parenthesis went unnoticed here: every remaining link resolved, and the damaged ones had simply vanished
    from the check. Counting the openings is the cheapest way to notice that.
    """
    text = path.read_text()
    return text.count("](") - len(LINK.findall(text))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("documents", nargs="*", type=Path,
                        help="documents to start from; the whole tree when omitted")
    args = parser.parse_args()

    if args.documents:
        documents = [path for path in args.documents]
    else:
        documents = [ROOT / "README.md", ROOT / "CHANGELOG.md", ROOT / "data" / "README.md",
                     ROOT / "tools" / "README.md"]
        documents += sorted((ROOT / "docs").rglob("*.md"))

    cache = {}
    problems = []
    checked = 0
    for document in documents:
        if not document.exists():
            continue
        malformed = malformed_count(document)
        if malformed:
            problems.append(f"{document.relative_to(ROOT)}: {malformed} link(s) are malformed -- an unclosed"
                            " parenthesis or a space inside the target")
        for target, anchor in targets_of(document):
            checked += 1
            where = f"{document.relative_to(ROOT)}"
            if not target.exists():
                problems.append(f"{where}: links to {target.relative_to(ROOT) if target.is_relative_to(ROOT) else target},"
                                " which does not exist")
                continue
            if not anchor:
                continue
            if target not in cache:
                cache[target] = headings_of(target)
            if anchor not in cache[target]:
                problems.append(f"{where}: links to #{anchor} in "
                                f"{target.relative_to(ROOT) if target.is_relative_to(ROOT) else target}, which has no such"
                                " heading")

    for problem in problems:
        print(f"FAIL {problem}")
    print(f"checked {checked} link(s) in {len(documents)} document(s), {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
