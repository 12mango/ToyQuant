#!/usr/bin/env python3
"""Regenerate the two path figures as SVG, and refuse a file on disk that is not this script's output.

Why a script and an SVG rather than Mermaid, which is how these two started. Mermaid draws in the browser, and in
the site's dark palette the `classDef` text colour lost to the page's own CSS, so every label came out pale on a
pale box -- the figure's whole point, unreadable. The second figure had a layout problem too: two subgraphs with
`direction LR` and no edge between them are laid out side by side, which turned a two-row diagram into a row. An
SVG has neither problem: it is drawn here once, its colours are fixed, and it reads the same on a white page, on
Material's dark scheme and as a plain image in README.

An SVG included on its own is not a Markdown block. Python-Markdown closes the paragraph after the opening tag, so
the figure reaches the browser as `<p><svg></p>` followed by a paragraph per `<text>` element -- a figure that is
in the source, green in the build, and absent from the page (that is how `charge.svg` was being rendered). Every
include therefore wraps the file in `<figure>`, which is block level and passes through whole; `lint_prose.py`
fails a bare `--8<--` of an `.svg`.

    python3 tools/study/figure_path.py --emit     # write docs/_figures/path-front.svg and path.svg
    python3 tools/study/figure_path.py --check    # 0 when both files are this script's output
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FRONT = ROOT / "docs/_figures/path-front.svg"
PATH = ROOT / "docs/_figures/path.svg"

# Fixed colours rather than `currentColor`: the same file is inlined on the site, which has two schemes, and used
# as an image in README, where there is no page colour to inherit. A pale fill under dark ink reads on white and on
# `#1e1e1e`; the connector grey clears 3.5:1 against both.
INK = "#10233a"
BOX = ("#e8f1f8", "#2f6fa8", INK)
GATE = ("#fdf3e3", "#b07d1c", "#3b2a06")
LEDGER = ("#eaf6ef", "#2c7d54", "#0d2f21")
LINE = "#7d8b9a"
FONT = ("font-family=\"ui-sans-serif, system-ui, -apple-system, 'Segoe UI', Roboto, 'Helvetica Neue', "
        "Arial, sans-serif\"")

WIDTH = 860
BOX_H = 44
GAP = 36
MARGIN = 30
# A rough advance width per character, used only to refuse a label that cannot fit its box. It is deliberately
# generous: a label that passes here still has room, which is what a layout nobody can render locally needs.
ADVANCE = 0.62

FRONT_ROW = [("feed", 88, BOX, "box"), ("L2 book", 112, BOX, "box"), ("strategy", 112, BOX, "box"),
             ("risk gate", 120, GATE, "diamond"), ("venue", 96, BOX, "box"), ("ledger", 96, LEDGER, "box")]
LOOP_ROW = [("record", 112, BOX, "box"), ("replay", 112, BOX, "box"), ("[RECON] parity", 168, LEDGER, "box")]

DEFS = (f'<defs><marker id="head" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
        f'orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{LINE}"/></marker></defs>')


def fits(label, size, width):
    return len(label) * size * ADVANCE <= width - 16


def text(x, y, label, size, colour, anchor="middle"):
    return (f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}" font-size="{size}" fill="{colour}" '
            f'{FONT}>{label}</text>')


def box(x, y, width, height, label, palette, size=15):
    fill, stroke, ink = palette
    if not fits(label, size, width):
        raise ValueError(f"label {label!r} does not fit a {width}px box at {size}px")
    return [f'<rect x="{x:.1f}" y="{y:.1f}" width="{width}" height="{height}" rx="6" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="1.5"/>',
            text(x + width / 2, y + height / 2 + size * 0.35, label, size, ink)]


def diamond(cx, cy, half_width, half_height, label, palette, size=13):
    fill, stroke, ink = palette
    if not fits(label, size, half_width * 2):
        raise ValueError(f"label {label!r} does not fit a {half_width * 2}px diamond at {size}px")
    points = f"{cx:.1f},{cy - half_height:.1f} {cx + half_width:.1f},{cy:.1f} " \
             f"{cx:.1f},{cy + half_height:.1f} {cx - half_width:.1f},{cy:.1f}"
    return [f'<polygon points="{points}" fill="{fill}" stroke="{stroke}" stroke-width="1.5"/>',
            text(cx, cy + size * 0.35, label, size, ink)]


def arrow(x_from, x_to, y):
    return [f'<line x1="{x_from:.1f}" y1="{y:.1f}" x2="{x_to - 2:.1f}" y2="{y:.1f}" stroke="{LINE}" '
            f'stroke-width="1.5" marker-end="url(#head)"/>']


def row(out, y, items, start):
    """Draw one left-to-right row and return the x it ends at."""
    x = start
    previous = None
    for label, width, palette, kind in items:
        if previous is not None:
            out.extend(arrow(previous, x, y))
        if kind == "diamond":
            out.extend(diamond(x + width / 2, y, width / 2, 26, label, palette))
        else:
            out.extend(box(x, y - BOX_H / 2, width, BOX_H, label, palette))
        previous = x + width
        x = previous + GAP
    return previous


def panel(x, y, width, height, label):
    return [f'<rect x="{x}" y="{y}" width="{width}" height="{height}" rx="10" fill="none" stroke="{LINE}" '
            f'stroke-opacity="0.45"/>',
            text(x + 14, y + 21, label, 12, LINE, anchor="start")]


def svg(width, height, label, body):
    # The `width`/`height` attributes are the intrinsic size an `<img>` needs (README's copy goes through one), and the
    # style is what lets the inline copy shrink with the page. Without the attributes a renderer that is not a browser
    # has no height to work with.
    return "\n".join([f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
                      f'viewBox="0 0 {width} {height}" role="img" style="max-width:100%;height:auto" '
                      f'aria-label="{label}">',
                      '<!-- Generated by tools/study/figure_path.py. Wrap the include in <figure>; see the module '
                      'docstring. -->',
                      DEFS] + body + ["</svg>"]) + "\n"


def render_front():
    out = []
    row(out, 50, FRONT_ROW, MARGIN)
    return svg(WIDTH, 100, "The path a market event takes: feed, L2 book, strategy, risk gate, venue, ledger", out)


def render_path():
    out = []
    out.extend(panel(16, 16, WIDTH - 32, 104, "the path"))
    row(out, 84, FRONT_ROW, MARGIN)
    out.extend(panel(16, 144, WIDTH - 32, 100, "the loop"))
    loop_width = sum(width for _, width, _, _ in LOOP_ROW) + GAP * (len(LOOP_ROW) - 1)
    row(out, 194, LOOP_ROW, (WIDTH - loop_width) // 2)
    return svg(WIDTH, 268,
               "Two rows: the path a market event takes, and the record, replay and reconciliation loop", out)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emit", action="store_true", help="write both SVGs")
    parser.add_argument("--check", action="store_true", help="compare the SVGs on disk with this script")
    args = parser.parse_args()
    if args.emit == args.check:
        parser.error("choose one of --emit or --check")
    figures = ((FRONT, render_front()), (PATH, render_path()))
    status = 0
    for path, content in figures:
        if args.emit:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            print(f"wrote {path.relative_to(ROOT)} ({len(content)} bytes, {content.count(chr(10))} lines)")
            continue
        if not path.is_file():
            print(f"FAIL {path.relative_to(ROOT)} does not exist")
            status = 1
            continue
        on_disk = path.read_text(encoding="utf-8")
        if on_disk == content:
            print(f"{path.name} is this script's output")
            continue
        status = 1
        print(f"FAIL {path.name} differs from this script's output")
        for number, (a, b) in enumerate(zip(on_disk.splitlines(), content.splitlines()), start=1):
            if a != b:
                print(f"  line {number}: on disk {a[:90]!r}")
                print(f"  line {number}: emitted {b[:90]!r}")
                break
    return status


if __name__ == "__main__":
    sys.exit(main())
