# VexOS runtime TrueType fonts

VexOS uses **stb_truetype v1.28** (public domain) as its freestanding
TrueType/OpenType parser and grayscale glyph rasterizer. Font outlines are
parsed by the kernel at boot; glyph bitmaps are rasterized on demand and kept
in a small LRU cache.

The renderer uses real font metrics (advance widths, ascenders, descenders,
line gaps) and horizontal kerning. Desktop strings decode UTF-8 and are drawn
with variable-width advances instead of fixed 8x8 cells. The old CGA bitmap
font remains only as an early-boot fallback before the heap and font engine
have initialized.

## Required font faces

VexOS embeds these eight static TTF faces:

| Family | Regular | Bold |
|---|---|---|
| Inter | `Inter-Regular.ttf` | `Inter-Bold.ttf` |
| Lato | `Lato-Regular.ttf` | `Lato-Bold.ttf` |
| DejaVu Sans | `DejaVuSans.ttf` | `DejaVuSans-Bold.ttf` |
| DejaVu Sans Mono | `DejaVuSansMono.ttf` | `DejaVuSansMono-Bold.ttf` |

Recommended layout:

```text
assets/fonts/
  README.md
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

The Makefile searches recursively under `assets/fonts/`, so subfolders are
supported. It also searches `/usr/share/fonts/`, allowing CI and Linux
developers to use distro-installed fonts. If a face is missing, the build
prints the exact missing family/weight and stops.

## Licenses

- **Inter** — SIL Open Font License 1.1:
  https://github.com/rsms/inter/blob/master/LICENSE.txt
- **Lato** — SIL Open Font License 1.1:
  https://github.com/googlefonts/lato/blob/main/OFL.txt
- **DejaVu Sans / Mono** — Bitstream Vera / DejaVu font license:
  https://github.com/dejavu-fonts/dejavu-fonts/blob/master/LICENSE
- **stb_truetype** — public domain; upstream source and notice are vendored in
  `src/kernel/desktop/graphics/third_party/stb_truetype.h`.

Keep the upstream license/notice files alongside the TTF downloads.
