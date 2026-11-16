#!/usr/bin/env python3
"""Build VexOS's compact, 2-bit antialiased ASCII font atlas.

The output stores four families in this order: Inter, Lato, DejaVu Sans,
and DejaVu Sans Mono. Each family has 128 glyphs, each glyph is 8x8 pixels.
Four 2-bit coverage values are packed into each byte.
"""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONTS = (
    "/usr/share/fonts/opentype/inter/Inter-Regular.otf",
    "/usr/share/fonts/truetype/lato/Lato-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
)

def render_glyph(font, codepoint):
    image = Image.new("L", (8, 8), 0)
    if codepoint < 32:
        return image.tobytes()
    character = chr(codepoint)
    bounds = font.getbbox(character)
    if not bounds:
        return image.tobytes()
    glyph = Image.new(
        "L", (max(1, bounds[2] - bounds[0]), max(1, bounds[3] - bounds[1])), 0
    )
    ImageDraw.Draw(glyph).text(
        (-bounds[0], -bounds[1]), character, font=font, fill=255
    )
    glyph.thumbnail((8, 8), Image.Resampling.LANCZOS)
    image.paste(glyph, ((8 - glyph.width) // 2, (8 - glyph.height) // 2))
    return image.tobytes()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    packed = bytearray()
    for font_path in FONTS:
        if not Path(font_path).is_file():
            raise SystemExit(f"Required font is missing: {font_path}")
        font = ImageFont.truetype(font_path, 10)
        for codepoint in range(128):
            pixels = render_glyph(font, codepoint)
            levels = [min(3, (value + 42) // 85) for value in pixels]
            for index in range(0, 64, 4):
                packed.append(
                    (levels[index] << 6)
                    | (levels[index + 1] << 4)
                    | (levels[index + 2] << 2)
                    | levels[index + 3]
                )
    Path(args.output).write_bytes(packed)
    if len(packed) != 8192:
        raise SystemExit(f"Unexpected font atlas size: {len(packed)}")

if __name__ == "__main__":
    main()
