#!/usr/bin/env python3
"""Prints src/ui/font5x7.c as ASCII art, so a human can look at it.

    python3 tools/show_font.py            # every glyph
    python3 tools/show_font.py 0123456789 # just these

README used to defer showing note detail on the grounds that a
hand-transcribed bitmap font could not be visually verified without hardware.
This is the answer to that: the table is parsed out of the C source and drawn,
so a wrong byte is visible immediately and on any machine. A board is still
needed to judge whether it is legible at arm's length, which is a different
question -- see font5x7.h's FONT5X7_MIN_READABLE_SCALE.
"""

import pathlib
import re
import sys

SRC = pathlib.Path(__file__).resolve().parent.parent / "src" / "ui" / "font5x7.c"

ROW = re.compile(
    r"\{((?:0x[0-9A-Fa-f]{2},\s*){4}0x[0-9A-Fa-f]{2})\},\s*/\*\s*(\d+)"
)


def load():
    text = SRC.read_text()
    glyphs = {}
    for cols, code in ROW.findall(text):
        code = int(code)
        glyphs[chr(code)] = [int(b, 16) for b in cols.replace(" ", "").split(",") if b]
    return glyphs


def render(ch, cols):
    lines = []
    for row in range(7):
        lines.append("".join("##" if (cols[c] >> row) & 1 else ".." for c in range(5)))
    return lines


def main():
    glyphs = load()
    missing = [
        chr(c) for c in range(32, 127) if chr(c) not in glyphs
    ]
    if missing:
        print(f"MISSING glyphs for: {missing!r}", file=sys.stderr)
        return 1
    print(f"parsed {len(glyphs)} glyphs from {SRC.name}\n")

    wanted = sys.argv[1] if len(sys.argv) > 1 else "".join(
        chr(c) for c in range(32, 127)
    )

    # Eight per row, so a terminal can show them side by side.
    for start in range(0, len(wanted), 8):
        chunk = [c for c in wanted[start : start + 8] if c in glyphs]
        if not chunk:
            continue
        blocks = [render(c, glyphs[c]) for c in chunk]
        print("   ".join(f"{repr(c):^10}" for c in chunk))
        for row in range(7):
            print("   ".join(b[row] for b in blocks))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
