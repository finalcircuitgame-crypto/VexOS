#!/usr/bin/env python3
"""Generate a diagnostic copy of Doom's WAD directory loader.

The upstream DoomGeneric submodule stays untouched. The generated C file is
written under dist/obj and is compiled in place of the submodule's w_wad.c.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


ASSIGN_MARKER = "/* VEXOS_WAD_DIAG_ASSIGN */"
AFTER_FREE_MARKER = "/* VEXOS_WAD_DIAG_AFTER_FREE */"
READ_MARKER = "/* VEXOS_WAD_DIAG_READ */"


def inject(source: str) -> str:
    if all(marker in source for marker in (
        ASSIGN_MARKER, AFTER_FREE_MARKER, READ_MARKER
    )):
        return source

    assign_anchor = "        strncpy(lump_p->name, filerover->name, 8);"
    if assign_anchor not in source:
        raise ValueError("could not find W_AddFile lump assignment anchor")

    assign_diag = f"""
        {ASSIGN_MARKER}
        if (i == 410u) {{
            printf("[WADDBG] ASSIGN lump=%u entry=%p wad=%p class=%p pos=%d size=%d\\\\n",
                   i, (void *)lump_p, (void *)lump_p->wad_file,
                   (void *)(lump_p->wad_file ? lump_p->wad_file->file_class : NULL),
                   lump_p->position, lump_p->size);
        }}
"""
    source = source.replace(assign_anchor, assign_anchor + assign_diag, 1)

    free_anchor = "    Z_Free(fileinfo);"
    if free_anchor not in source:
        raise ValueError("could not find W_AddFile Z_Free(fileinfo) anchor")

    after_free_diag = f"""
    {free_anchor}

    {AFTER_FREE_MARKER}
    if (numlumps > 410u) {{
        wad_file_t *debug_wad = lumpinfo[410].wad_file;
        printf("[WADDBG] AFTER_FREE lump=410 entry=%p wad=%p class=%p pos=%d size=%d\\\\n",
               (void *)&lumpinfo[410], (void *)debug_wad,
               (void *)(debug_wad ? debug_wad->file_class : NULL),
               lumpinfo[410].position, lumpinfo[410].size);
    }}
"""
    source = source.replace(free_anchor, after_free_diag.rstrip("\n"), 1)

    read_anchor = "    l = lumpinfo+lump;"
    if read_anchor not in source:
        raise ValueError("could not find W_ReadLump directory lookup anchor")

    read_diag = f"""
    {read_anchor}

    {READ_MARKER}
    if (lump == 410u) {{
        wad_file_t *debug_wad = l->wad_file;
        printf("[WADDBG] READ lump=%u directory=%p entry=%p wad=%p class=%p pos=%d size=%d\\\\n",
               lump, (void *)lumpinfo, (void *)l, (void *)debug_wad,
               (void *)(debug_wad ? debug_wad->file_class : NULL),
               l->position, l->size);
    }}
"""
    source = source.replace(read_anchor, read_diag.rstrip("\n"), 1)

    return source


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    try:
        original = args.source.read_text(encoding="utf-8")
        instrumented = inject(original)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(instrumented, encoding="utf-8", newline="\n")
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"[WADDBG] instrumentation failed: {exc}", file=sys.stderr)
        return 1

    print(f"[WADDBG] instrumented source: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
