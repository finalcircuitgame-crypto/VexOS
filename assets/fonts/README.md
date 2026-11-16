# VexOS built-in fonts

The desktop renderer builds a compact 2-bit antialiased ASCII atlas from these
system font families:

1. **Inter** — default UI font. Licensed under the SIL Open Font License 1.1.
   https://github.com/rsms/inter/blob/master/LICENSE.txt
2. **Lato** — alternative sans-serif. Licensed under the SIL Open Font License 1.1.
   https://github.com/googlefonts/lato/blob/main/OFL.txt
3. **DejaVu Sans** — broad-coverage sans-serif fallback. Distributed under the
   Bitstream Vera / DejaVu font license.
   https://github.com/dejavu-fonts/dejavu-fonts/blob/master/LICENSE
4. **DejaVu Sans Mono** — fixed-width font for terminal-style text. Distributed
   under the Bitstream Vera / DejaVu font license.
   https://github.com/dejavu-fonts/dejavu-fonts/blob/master/LICENSE

The atlas is generated during the build by `tools/prepare_font_atlas.py`.
Install Pillow and the matching system font packages before building. The
kernel embeds the resulting atlas; it does not load fonts from disk and does
not depend on a runtime font engine. The original CGA bitmap font remains
linked as a fallback for bytes outside the built-in ASCII atlas.
