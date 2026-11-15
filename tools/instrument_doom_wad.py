#!/usr/bin/env python3
"""Generate a diagnostic copy of Doom's WAD directory loader.

The upstream DoomGeneric submodule remains untouched. The generated C source
lives under dist/obj and is compiled in place of the submodule's w_wad.c.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


ASSIGN_MARKER = "/* VEXOS_WAD_DIAG_ASSIGN */"
AFTER_FREE_MARKER = "/* VEXOS_WAD_DIAG_AFTER_FREE */"
READ_MARKER = "/* VEXOS_WAD_DIAG_READ */"


def insert_after(source: str, marker: str, pattern: str, code_builder, label: str) -> str:
    if marker in source:
        return source

    match = re.search(pattern, source, flags=re.MULTILINE)
    if match is None:
        raise ValueError(f"could not find {label} anchor in upstream w_wad.c")

    indent = match.group("indent")
    code = code_builder(indent)
    return source[:match.end()] + "\n" + code.rstrip("\n") + source[match.end():]


def inject(source: str) -> str:
    # Match source indentation flexibly: upstream DoomGeneric uses tabs in
    # some places and spaces in others.
    source = insert_after(
        source,
        ASSIGN_MARKER,
        r"^(?P<indent>[ \t]*)strncpy\s*\(\s*lump_p->name\s*,\s*filerover->name\s*,\s*8\s*\)\s*;[ \t]*$",
        lambda indent: (
            f"{indent}{ASSIGN_MARKER}\n"
            f"{indent}if (i == 410u) {{\n"
            f'{indent}    printf("[WADDBG] ASSIGN lump=%u entry=%p wad=%p class=%p pos=%d size=%d\\n",\n'
            f"{indent}           i, (void *)lump_p, (void *)lump_p->wad_file,\n"
            f"{indent}           (void *)(lump_p->wad_file ? lump_p->wad_file->file_class : NULL),\n"
            f"{indent}           lump_p->position, lump_p->size);\n"
            f"{indent}}}\n"
        ),
        "W_AddFile lump assignment",
    )

    source = insert_after(
        source,
        AFTER_FREE_MARKER,
        r"^(?P<indent>[ \t]*)Z_Free\s*\(\s*fileinfo\s*\)\s*;[ \t]*$",
        lambda indent: (
            f"{indent}{AFTER_FREE_MARKER}\n"
            f"{indent}if (numlumps > 410u) {{\n"
            f"{indent}    wad_file_t *debug_wad = lumpinfo[410].wad_file;\n"
            f'{indent}    printf("[WADDBG] AFTER_FREE lump=410 entry=%p wad=%p class=%p pos=%d size=%d\\n",\n'
            f"{indent}           (void *)&lumpinfo[410], (void *)debug_wad,\n"
            f"{indent}           (void *)(debug_wad ? debug_wad->file_class : NULL),\n"
            f"{indent}           lumpinfo[410].position, lumpinfo[410].size);\n"
            f"{indent}}}\n"
        ),
        "W_AddFile Z_Free(fileinfo)",
    )

    source = insert_after(
        source,
        READ_MARKER,
        r"^(?P<indent>[ \t]*)l\s*=\s*lumpinfo\s*\+\s*lump\s*;[ \t]*$",
        lambda indent: (
            f"{indent}{READ_MARKER}\n"
            f"{indent}if (lump == 410u) {{\n"
            f"{indent}    wad_file_t *debug_wad = l->wad_file;\n"
            f'{indent}    printf("[WADDBG] READ lump=%u directory=%p entry=%p wad=%p class=%p pos=%d size=%d\\n",\n'
            f"{indent}           lump, (void *)lumpinfo, (void *)l, (void *)debug_wad,\n"
            f"{indent}           (void *)(debug_wad ? debug_wad->file_class : NULL),\n"
            f"{indent}           l->position, l->size);\n"
            f"{indent}}}\n"
        ),
        "W_ReadLump directory lookup",
    )

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
