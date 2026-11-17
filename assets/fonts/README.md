# VexOS built-in fonts

VexOS uses **real upstream font families**, not a newly invented typeface or
custom-drawn glyphs:

1. **Inter** — default UI font. SIL Open Font License 1.1.
   https://github.com/rsms/inter/blob/master/LICENSE.txt
2. **Lato** — alternative sans-serif. SIL Open Font License 1.1.
   https://github.com/googlefonts/lato/blob/main/OFL.txt
3. **DejaVu Sans** — alternative sans-serif and fallback. Bitstream Vera /
   DejaVu font license.
   https://github.com/dejavu-fonts/dejavu-fonts/blob/master/LICENSE
4. **DejaVu Sans Mono** — monospace face for terminal-style text. Bitstream Vera
   / DejaVu font license.
   https://github.com/dejavu-fonts/dejavu-fonts/blob/master/LICENSE

The build rasterizes the original regular and bold font faces into a compact
framebuffer atlas. The glyph outlines are not redesigned; only rasterization,
antialiasing, and the existing compact cell metrics are adapted for VexOS.
Coverage is stored at 4 bits per pixel (16 alpha levels), and bold uses the
font's actual bold face instead of artificially thickening regular glyphs.

The atlas is generated manually by `tools/prepare_font_atlas.py`.
**The Makefile intentionally does not run this script.** It only embeds the
prepared `assets/fonts/font_atlas.bin`, so normal builds do not require
Pillow or access to the original TTF/OTF files. Generate the atlas before the
first build and whenever the source fonts change.

The kernel embeds the atlas; it does not load TTF/OTF files at runtime or
depend on a runtime font engine. The original CGA bitmap font remains available
as a fallback for bytes outside the built-in ASCII atlas.

## Recommended folder layout

Keep only the faces used by the atlas generator and each family's license:

```text
assets/fonts/
  README.md
  font_atlas.bin                 # generated locally; ignored by Git
  Inter/
    Inter-Regular.ttf
    Inter-Bold.ttf
    OFL.txt
  Lato/
    Lato-Regular.ttf
    Lato-Bold.ttf
    OFL.txt
  DejaVu/
    DejaVuSans.ttf
    DejaVuSans-Bold.ttf
    DejaVuSansMono.ttf
    DejaVuSansMono-Bold.ttf
    LICENSE
```

`font_atlas.bin` is generated output, not a source font. The generator
recursively searches the family folders, so this layout works without copying
font files into the top-level `assets/fonts/` directory.

## Preparing the source fonts

Download the static **regular and bold** font files for all four families
listed above, and place them anywhere underneath `assets/fonts/`. The
generator searches this folder recursively, so keeping each family in its
own subfolder is fine. Keep the upstream license files with the font
downloads.

Install Pillow and the matching font families for your host.

Windows PowerShell:

```powershell
py -m pip install Pillow
```

Ubuntu/WSL:

```bash
sudo apt install python3-pil fonts-inter fonts-lato fonts-dejavu-core
```

## Generate the atlas manually

Run this from the repository root.

Windows PowerShell:

```powershell
py tools\prepare_font_atlas.py --output assets\fonts\
```

Ubuntu/WSL:

```bash
python3 tools/prepare_font_atlas.py --font-dir /usr/share/fonts --output assets/fonts/
```

Both commands generate the ignored file `assets/fonts/font_atlas.bin`.
Check that it is exactly 32,768 bytes, then run `make`.
