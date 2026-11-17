#!/usr/bin/env python3
"""Build VexOS's antialiased ASCII atlas from real installed TTF/OTF faces.

Families are stored in enum order: Inter, Lato, DejaVu Sans, DejaVu Sans
Mono. Each family stores its real regular face followed by its real bold face.
Each glyph is an 8x8 cell; two 4-bit coverage values are packed per byte.

Font files are discovered on Windows, macOS, and Linux. You can also pass one
or more --font-dir paths, or set VEXOS_FONT_DIR, to point at downloaded fonts.
"""
import argparse
import os
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT_FAMILIES = (
    (
        "Inter",
        ("Inter-Regular.otf", "Inter-Regular.ttf"),
        ("Inter-Bold.otf", "Inter-Bold.ttf"),
    ),
    (
        "Lato",
        ("Lato-Regular.ttf", "Lato-Regular.otf"),
        ("Lato-Bold.ttf", "Lato-Bold.otf"),
    ),
    (
        "DejaVu Sans",
        ("DejaVuSans.ttf",),
        ("DejaVuSans-Bold.ttf",),
    ),
    (
        "DejaVu Sans Mono",
        ("DejaVuSansMono.ttf",),
        ("DejaVuSansMono-Bold.ttf",),
    ),
)

CELL_WIDTH = 8
CELL_HEIGHT = 8
FONT_SIZE = 10
GLYPHS = 128
ATLAS_FILENAME = "font_atlas.bin"


def font_search_directories(extra_directories=()):
    """Return existing user, project, and OS font directories in priority order."""
    directories = []

    def add(path):
        if not path:
            return
        candidate = Path(path).expanduser()
        if candidate.is_dir():
            resolved = candidate.resolve()
            if resolved not in directories:
                directories.append(resolved)

    # Explicit CLI paths take priority, followed by the environment variable.
    for directory in extra_directories:
        add(directory)
    for directory in os.environ.get("VEXOS_FONT_DIR", "").split(os.pathsep):
        add(directory)

    # Keep downloaded fonts beside the VexOS source tree when convenient.
    project_root = Path(__file__).resolve().parents[1]
    add(project_root / "assets" / "fonts")

    # Common Windows system-wide and per-user font locations.
    windir = os.environ.get("WINDIR", r"C:\Windows")
    local_app_data = os.environ.get("LOCALAPPDATA")
    add(Path(windir) / "Fonts")
    if local_app_data:
        add(Path(local_app_data) / "Microsoft" / "Windows" / "Fonts")

    # Common macOS font locations.
    add("/System/Library/Fonts")
    add("/Library/Fonts")
    if os.environ.get("HOME"):
        add(Path(os.environ["HOME"]) / "Library" / "Fonts")

    # Common Linux distribution paths (including the paths used by CI).
    add("/usr/share/fonts/opentype/inter")
    add("/usr/share/fonts/truetype/lato")
    add("/usr/share/fonts/truetype/dejavu")
    add("/usr/local/share/fonts")
    add("/usr/share/fonts")

    return directories


def find_font_file(candidates, directories):
    """Find a font by filename, checking direct and nested font directories."""
    wanted = {name.casefold() for name in candidates}

    for directory in directories:
        # Most OS font folders and the suggested assets/fonts folder are flat.
        for candidate in candidates:
            direct = directory / candidate
            if direct.is_file():
                return direct

    # Also support downloaded font archives extracted into family subfolders.
    for directory in directories:
        try:
            for path in directory.rglob("*"):
                if path.is_file() and path.name.casefold() in wanted:
                    return path
        except OSError:
            continue

    return None


def resolve_font_families(extra_directories=()):
    directories = font_search_directories(extra_directories)
    resolved = []
    missing = []

    for family_name, regular_candidates, bold_candidates in FONT_FAMILIES:
        regular = find_font_file(regular_candidates, directories)
        bold = find_font_file(bold_candidates, directories)
        if regular is None:
            missing.append(f"{family_name} regular ({' or '.join(regular_candidates)})")
        if bold is None:
            missing.append(f"{family_name} bold ({' or '.join(bold_candidates)})")
        resolved.append((family_name, regular, bold))

    if missing:
        searched = "\n".join(f"  - {directory}" for directory in directories)
        missing_text = "\n".join(f"  - {face}" for face in missing)
        raise SystemExit(
            "Missing required font face(s):\n"
            f"{missing_text}\n\n"
            "Install the font families or place their regular/bold files in "
            "assets/fonts, then rerun. You can also pass --font-dir DIR "
            "(repeat it for multiple folders) or set VEXOS_FONT_DIR.\n\n"
            f"Directories searched:\n{searched or '  (none found)'}"
        )

    return tuple(resolved)


def resolve_output_path(output_argument):
    """Treat a trailing separator or existing directory as an output folder."""
    raw_output = str(output_argument)
    output = Path(raw_output).expanduser()
    if raw_output.endswith(("/", "\\")) or output.is_dir():
        output = output / ATLAS_FILENAME
    output.parent.mkdir(parents=True, exist_ok=True)
    return output


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
        glyph,
    )
    return image.tobytes()


def main():
    parser = argparse.ArgumentParser(
        description="Generate the VexOS font atlas from installed font files."
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Output file, or output directory (a trailing slash is optional "
             "when the directory already exists).",
    )
    parser.add_argument(
        "--font-dir",
        action="append",
        default=[],
        metavar="DIR",
        help="Search this font directory; may be supplied more than once.",
    )
    args = parser.parse_args()

    families = resolve_font_families(args.font_dir)
    output_path = resolve_output_path(args.output)

    packed = bytearray()
    for family_name, regular_path, bold_path in families:
        for font_path in (regular_path, bold_path):
            try:
                font = ImageFont.truetype(str(font_path), FONT_SIZE)
            except OSError as error:
                raise SystemExit(
                    f"Could not load {family_name} font face {font_path}: {error}"
                ) from error

            for codepoint in range(GLYPHS):
                pixels = render_glyph(font, codepoint)
                levels = [min(15, (value + 8) // 17) for value in pixels]
                for index in range(0, CELL_WIDTH * CELL_HEIGHT, 2):
                    packed.append((levels[index] << 4) | levels[index + 1])

    expected_size = len(FONT_FAMILIES) * 2 * GLYPHS * 32
    if len(packed) != expected_size:
        raise SystemExit(
            f"Unexpected font atlas size: {len(packed)} (expected {expected_size})"
        )

    output_path.write_bytes(packed)
    print(f"Generated {output_path} ({len(packed)} bytes)")


if __name__ == "__main__":
    main()
