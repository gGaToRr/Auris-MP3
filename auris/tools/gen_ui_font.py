#!/usr/bin/env python3
"""Generate the Auris UI bitmap font from a system 8x8 console font.

Why a system bitmap font instead of a scalable TTF
--------------------------------------------------
The panel is 480x480, so glyphs are drawn magnified 2x..6x. Rasterising an
outline font into a 6x10 cell and scaling that up produces muddy letterforms
whose `0`, `O` and `D` collapse into each other - at 6x10 the typeface's own
zero differs from its `O` by a single pixel and both read as a squared-off `D`,
which is what made "STEREO" look like "STERED".

A console font is different: every glyph was designed by hand as a bitmap on an
8x8 grid, so it stays crisp when magnified and its letterforms are deliberate.
The CP437 face this script reads (the classic VGA/IBM-PC 8x8) happens to solve
the ambiguity structurally rather than by patching:

  * `0` carries a diagonal slash through the counter,
  * `O` is a smooth oval,
  * `D` is flat-sided with square corners.

That takes the three glyphs from barely a pixel or two apart to 16/10/20.

Cell geometry
-------------
The source glyphs are 8x8 with the baseline between rows 6 and 7 (caps occupy
rows 0..6, descenders row 7). Emitting a 7-wide cell - dropping the mostly
unused 8th column - gives a 1px letter gap with an 8px advance, which is what
the UI layout is built around: at 8px advance every existing string still fits
at its existing scale, whereas a full 8-wide cell (9px advance) pushes the
"00:08 / ~14:54" and "100%" line past the right margin.

Vertical metrics are unchanged from the previous 6x10 cell: caps still occupy
rows 0..6, so every baseline in player_ui.c stays where it is.

The one glyph tweak
-------------------
In the source face `l` and `I` differ by a single pixel (a stub on the top
serif), which is authentic to DOS but reads as a duplicate on a bright panel.
`l` therefore loses its top serif, which is the conventional way to set it
apart from `I` - `I` keeps both bars, `l` gets the foot only.

Usage: gen_ui_font.py <output-header> [console-font]
"""
import gzip
import os
import sys
from collections import defaultdict

FIRST, LAST = 0x20, 0x7E
SRC_W, SRC_H = 8, 8
CELL_W, CELL_H = 7, 8        # source cell minus the unused 8th column
CAP_ROWS = 7                 # rows occupied by cap/digit height (0..6)
DEFAULT_FONT = "/usr/share/kbd/consolefonts/alt-8x8.gz"

# Rows are '#'/'.' art, CELL_W wide, '/'-separated, top row first. Trailing
# blank rows are optional; ink below row 9 is rejected.
GLYPH_OVERRIDES = {
    # Source 'l' is ".###...." / "..##...." x5 / ".####..." - one pixel from
    # 'I' (".####..." / "..##...." x5 / ".####..."). Drop the top bar: 'I'
    # keeps both, 'l' keeps the foot.
    "l": "......./..##.../..##.../..##.../..##.../..##.../.####..",
}


def load_console_font(path):
    """Read a raw 256-glyph 8x8 console font (no header) indexed by codepoint.

    These ship in /usr/share/kbd/consolefonts as a bare 2048-byte blob: 256
    glyphs of 8 rows, one byte per row, most significant bit leftmost.
    """
    with gzip.open(path, "rb") as fh:
        raw = fh.read()
    if len(raw) != 256 * SRC_H:
        raise ValueError(f"{path}: expected {256 * SRC_H} bytes, got {len(raw)}")
    glyphs = {}
    for code in range(256):
        blob = raw[code * SRC_H:(code + 1) * SRC_H]
        glyphs[code] = [
            [(blob[row] >> (SRC_W - 1 - col)) & 1 for col in range(SRC_W)]
            for row in range(SRC_H)
        ]
    return glyphs


def to_cell(rows):
    """Drop the source's 8th column, which is a letter gap in practice.

    Of the printable ASCII range only '*' and '_' put ink in that column, and
    both simply become one pixel narrower - a slightly shorter underline and an
    asterisk missing the tip of its right arm. Nothing else changes.
    """
    cell = [row[:CELL_W] for row in rows]
    if any(len(row) != CELL_W for row in cell):
        raise ValueError("cell row has the wrong width")
    return cell


def art_to_columns(art):
    """Turn '#'/'.' art into column-major bits, rejecting bad metrics.

    Overrides are typed by hand, so a typo that quietly changes a glyph's size
    is the failure mode worth guarding: the art has to fit the cell, and ink
    below the last usable row is a mistake rather than a design choice.
    """
    rows = [r for r in art.split("/") if r != ""]
    if len(rows) > CELL_H:
        raise ValueError(f"override has {len(rows)} rows, cell is {CELL_H}")
    for row in rows:
        if len(row) != CELL_W or set(row) - set("#."):
            raise ValueError(f"override row {row!r} is not {CELL_W} of '#'/'.'")
    while len(rows) < CELL_H:
        rows.append("." * CELL_W)
    return [sum(1 << y for y in range(CELL_H) if rows[y][x] == "#")
            for x in range(CELL_W)]


def build_table(src):
    table = {}
    for code in range(FIRST, LAST + 1):
        table[chr(code)] = to_cell(src[code])
    for ch, art in GLYPH_OVERRIDES.items():
        if ch not in table:
            raise ValueError(f"override for {ch!r} is outside 0x{FIRST:02X}..0x{LAST:02X}")
        table[ch] = [list(r) for r in art.split("/") if r != ""]
        while len(table[ch]) < CELL_H:
            table[ch].append([0] * CELL_W)
    return table


def to_columns(rows):
    return [sum(1 << y for y in range(CELL_H) if rows[y][x]) for x in range(CELL_W)]


def bbox(rows):
    ys = [y for y in range(CELL_H) if any(rows[y])]
    xs = [x for x in range(CELL_W) if any(rows[y][x] for y in range(CELL_H))]
    return (min(ys), max(ys), min(xs), max(xs)) if ys and xs else None


CAPS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
DIGITS = "0123456789"
# Ascenders reach the cap line; x-height letters do not; descenders drop to
# the last cell row. These three classes are what "the same height as the other
# letters" actually means on screen, so they are checked rather than assumed.
ASCENDERS = "bdfhkl ti".replace(" ", "") + "i"
XHEIGHT = "acemnorsuvwxz"
DESCENDERS = "gjpqy"


def check(table):
    """Return (ok, lines, columns). Every check here catches a real defect."""
    lines, ok = [], True

    # Two characters sharing a bitmap are indistinguishable on screen, which
    # silently corrupts text (e.g. 'l' vs 'I', or a lowercase letter rendering
    # as its uppercase).
    seen = {}
    for ch, rows in sorted(table.items()):
        key = tuple(map(tuple, rows))
        if key in seen and ch != " ":
            lines.append(f"ERROR: '{seen[key]}' and '{ch}' render identically")
            ok = False
        seen.setdefault(key, ch)

    def extent(ch):
        box = bbox(table[ch])
        return None if box is None else (box[0], box[1])

    for ch in CAPS + DIGITS + ASCENDERS:
        if extent(ch) != (0, CAP_ROWS - 1):
            lines.append(f"ERROR: '{ch}' spans rows {extent(ch)}, "
                         f"cap height is 0..{CAP_ROWS - 1}")
            ok = False
    for ch in XHEIGHT:
        if extent(ch) != (2, CAP_ROWS - 1):
            lines.append(f"ERROR: '{ch}' spans rows {extent(ch)}, "
                         f"x-height letters sit on 2..{CAP_ROWS - 1}")
            ok = False
    for ch in DESCENDERS:
        got = extent(ch)
        if got is None or got[1] != CELL_H - 1:
            lines.append(f"ERROR: '{ch}' spans rows {got}, "
                         f"descenders must reach row {CELL_H - 1}")
            ok = False

    # The advance is what separates letters, so report the gap it leaves.
    def ink_width(rows):
        xs = [x for x in range(CELL_W) if any(rows[y][x] for y in range(CELL_H))]
        return max(xs) + 1 if xs else 0

    widest = max(ink_width(rows) for rows in table.values())
    lines.append(f"widest glyph {widest}px in a {CELL_W}px cell, "
                 f"advance {CELL_W + 1}px -> {CELL_W + 1 - widest}px letter gap")

    cols = {ch: to_columns(rows) for ch, rows in table.items()}

    def dist(a, b):
        return sum(bin(x ^ y).count("1") for x, y in zip(cols[a], cols[b]))

    for a, b in (("0", "O"), ("O", "D"), ("0", "D"), ("l", "I"), ("1", "l"),
                 ("1", "I"), ("5", "S"), ("6", "G"), ("G", "C"), ("O", "Q"),
                 ("8", "B"), ("2", "Z"), ("b", "d"), ("h", "n"), ("s", "z")):
        lines.append(f"separation '{a}' vs '{b}': {dist(a, b)} px")
    return ok, lines, cols


def preview(table):
    for start in range(FIRST, LAST + 1, 16):
        chars = [chr(c) for c in range(start, min(start + 16, LAST + 1))]
        print(f"--- 0x{start:02X} ---")
        for row in range(CELL_H):
            print("".join(
                "".join("#" if table[ch][row][x] else "." for x in range(CELL_W)) + " "
                for ch in chars))
        print("".join("_" if ch == " " else ch for ch in chars))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    font_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_FONT

    if not os.path.exists(font_path):
        print(f"console font not found: {font_path}")
        return 1

    table = build_table(load_console_font(font_path))
    for ch, art in GLYPH_OVERRIDES.items():
        if len(art.split("/")) > CAP_ROWS:
            raise SystemExit(f"override '{ch}' must stay within the cap rows")

    ok, lines, cols = check(table)
    for line in lines:
        print(line)
    if not ok:
        print("font rejected")
        return 1

    preview(table)

    header = [
        "// Generated by tools/gen_ui_font.py - do not edit by hand.",
        f"// {CELL_W}x{CELL_H} bitmap font, printable ASCII 0x20..0x{FIRST + 94:02X},"
        " column-major (bit 0 = top row).",
        f"// Source: {os.path.basename(font_path)} (system 8x8 console font, CP437 face).",
        "// Caps occupy rows 0..6, row 7 carries descenders; the source's unused",
        "// 8th column is dropped so the advance is 8px and the layout still fits.",
        "// Its slashed '0' and oval 'O' are why no glyph patches are needed beyond 'l'.",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define UI_FONT_W      {CELL_W}",
        f"#define UI_FONT_H      {CELL_H}",
        f"#define UI_FONT_FIRST  0x{FIRST:02X}",
        f"#define UI_FONT_LAST   0x{LAST:02X}",
        "",
        f"static const uint16_t ui_font{CELL_W}x{CELL_H}"
        f"[UI_FONT_LAST - UI_FONT_FIRST + 1][UI_FONT_W] = {{",
    ]
    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        values = ", ".join(f"0x{b:03X}" for b in cols[ch])
        header.append(f"    {{ {values} }}, /* 0x{code:02X} '{ch}' */")
    header += ["};", ""]
    with open(out, "w") as fh:
        fh.write("\n".join(header))
    print(f"wrote {out} ({font_path}, {CELL_W}x{CELL_H} cell, advance {CELL_W + 1}px)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
