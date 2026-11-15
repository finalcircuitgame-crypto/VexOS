#!/usr/bin/env python3
"""Apply VexOS's non-invasive DoomGeneric input-loop fix to a build copy."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys


OLD = """            event.data2 = 0;

            if (event.data1 != 0)
            {
                D_PostEvent(&event);
            }
            break;
        }
"""

NEW = """            event.data2 = 0;

            if (event.data1 != 0)
            {
                D_PostEvent(&event);
            }
        }
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    try:
        source = args.source.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"patch_doom_input: cannot read {args.source}: {exc}", file=sys.stderr)
        return 1

    matches = source.count(OLD)
    if matches != 1:
        print(
            "patch_doom_input: expected exactly one key-up early-exit block; "
            f"found {matches}. Refusing to patch an unknown source version.",
            file=sys.stderr,
        )
        return 1

    patched = source.replace(OLD, NEW, 1)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        args.output.write_text(patched, encoding="utf-8", newline="\n")
    except OSError as exc:
        print(f"patch_doom_input: cannot write {args.output}: {exc}", file=sys.stderr)
        return 1

    print("[DOOM] Patched input loop: drain key-up and key-down events")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
