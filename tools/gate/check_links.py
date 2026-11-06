#!/usr/bin/env python3
"""Check that every markdown link in the tree resolves, and that every anchor exists.

The documents are cross-referenced heavily and the references are how a reader moves between tiers, so a link that
points at a moved file is a dead end nobody notices: nothing in the build read a link until this did. Three things are
checked, in the order a reader would hit them:

  * the file exists, resolved relative to the document that links it;
  * an absolute GitHub URL that names this repository is mapped back to the path it names and checked the same way --
    those links exist because `mkdocs build --strict` refuses a relative link that resolves outside `docs_dir`, and
    skipping them left a handful of links that nothing followed;
  * the `#anchor`, if there is one, matches a heading in that file -- with GitHub's own slug rules, which is what a
    reader's browser will look for.

A link to a repository *other* than this one is reported as a problem rather than ignored, because in this tree it is
a stale name and not an external reference: the published site carried `ToyQuant-v1` in its header for two weeks after
the rename, and nothing compared the name in a link against the name of the repository it was written in.

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
# `https://github.com/<owner>/<repo>/(blob|tree)/<ref>/<path>[#anchor]`, which is the shape every out-of-tree link in
# these documents takes. The ref is not checked: it is `main` in all of them, and a link to another branch is a
# deliberate reference to something else.
GITHUB = re.compile(
    r"^https://github\.com/(?P<owner>[^/]+)/(?P<repo>[^/]+)/(?:blob|tree)/(?P<ref>[^/]+)/(?P<path>[^#]+)"
    r"(?:#(?P<anchor>.*))?$")
REPOSITORY = "12mango/ToyQuant"


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
        github = GITHUB.match(raw)
        if github is not None:
            name = f"{github.group('owner')}/{github.group('repo')}"
            if name != REPOSITORY:
                # A `None` target carrying the name it found: the caller reports the mismatch rather than a missing file.
                yield None, name
            elif github.group("path").endswith(".md"):
                yield (ROOT / github.group("path")).resolve(), github.group("anchor") or ""
            continue
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


def display(path):
    """How a path is named in a message: relative to the tree when it is inside it, absolute when it is not.

    Both cases are legitimate -- the whole-tree run names your own documents, and a document outside the tree can be
    checked on purpose -- so neither should raise. `document.relative_to(ROOT)` used to, which is why `check_links.py
    docs/*.md` (the form its own usage line advertises) failed on every file the shell had not made absolute.
    """
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("documents", nargs="*", type=Path,
                        help="documents to start from; the whole tree when omitted")
    args = parser.parse_args()

    if args.documents:
        documents = [path.resolve() for path in args.documents]
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
            problems.append(f"{display(document)}: {malformed} link(s) are malformed -- an unclosed"
                            " parenthesis or a space inside the target")
        for target, anchor in targets_of(document):
            checked += 1
            where = display(document)
            if target is None:
                problems.append(f"{where}: links to {anchor}, which is not this repository -- every absolute URL in"
                                f" these documents has to name {REPOSITORY}")
                continue
            if not target.exists():
                problems.append(f"{where}: links to {display(target)}, which does not exist")
                continue
            if not anchor:
                continue
            if target not in cache:
                cache[target] = headings_of(target)
            if anchor not in cache[target]:
                problems.append(f"{where}: links to #{anchor} in {display(target)}, which has no such heading")

    for problem in problems:
        print(f"FAIL {problem}")
    print(f"checked {checked} link(s) in {len(documents)} document(s), {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
