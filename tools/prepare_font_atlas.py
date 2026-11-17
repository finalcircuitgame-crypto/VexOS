#!/usr/bin/env python3
"""Build VexOS's antialiased ASCII atlas from original installed TTF/OTF faces.

Families are stored in enum order: Inter, Lato, DejaVu Sans, DejaVu Sans
Mono. Each family stores its real regular face followed by its real bold face.
Each glyph is an 8x8 cell; two 4-bit coverage values are packed per byte.
"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT_FAMILIES = (
    (
        "Inter",
        "/usr/share/fonts/opentype/inter/Inter-Regular.otf",
        "/usr/share/fonts/opentype/inter/Inter-Bold.otf",
    ),
    (
        "Lato",
        "/usr/share/fonts/truetype/lato/Lato-Regular.ttf",
        "/usr/share/fonts/truetype/lato/Lato-Bold.ttf",
    ),
    (
        "DejaVu Sans",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    ),
    (
        "DejaVu Sans Mono",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    ),
)

CELL_WIDTH = 8
CELL_HEIGHT = 8
FONT_SIZE = 10
GLYPHS = 128


def render_glyph(font, codepoint):
    image = Image.new("L", (CELL_WIDTH, CELL_HEIGHT), 0)
    if codepoint < 32:
        return image.tobytes()

    character = chr(codepoint)
    bounds = font.getbbox(character)
    if not bounds:
        return image.tobytes()

    glyph = Image.new(
        "L",
        (max(1, bounds[2] - bounds[0]), max(1, bounds[3] - bounds[1])),
        0,
    )
    ImageDraw.Draw(glyph).text(
        (-bounds[0], -bounds[1]), character, font=font, fill=255
    )
    glyph.thumbnail((CELL_WIDTH, CELL_HEIGHT), Image.Resampling.LANCZOS)
    image.paste(
        glyph,
        ((CELL_WIDTH - glyph.width) // 2, (CELL_HEIGHT - glyph.height) // 2),
    )
    return image.tobytes()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    packed = bytearray()
    for family_name, regular_path, bold_path in FONT_FAMILIES:
        for font_path in (regular_path, bold_path):
            if not Path(font_path).is_file():
                raise SystemExit(
                    f"Required {family_name} font face is missing: {font_path}"
                )

            font = ImageFont.truetype(font_path, FONT_SIZE)
            for codepoint in range(GLYPHS):
                pixels = render_glyph(font, codepoint)
                levels = [min(15, (value + 8) // 17) for value in pixels]
                for index in range(0, CELL_WIDTH * CELL_HEIGHT, 2):
                    packed.append((levels[index] << 4) | levels[index + 1])

    Path(args.output).write_bytes(packed)
    expected_size = len(FONT_FAMILIES) * 2 * GLYPHS * 32
    if len(packed) != expected_size:
        raise SystemExit(
            f"Unexpected font atlas size: {len(packed)} (expected {expected_size})"
        )


if __name__ == "__main__":
    main()
