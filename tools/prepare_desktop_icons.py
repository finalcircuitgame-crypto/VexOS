#!/usr/bin/env python3
"""Convert VexOS desktop PNG icons into one embedded RGBA pixel blob.

Uses only Python's standard library so the normal kernel build does not need
Pillow or any other third-party image package.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
ICON_WIDTH = 32
ICON_HEIGHT = 32
EXPECTED_ICON_COUNT = 6


def paeth_predictor(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    distance_left = abs(estimate - left)
    distance_above = abs(estimate - above)
    distance_upper_left = abs(estimate - upper_left)

    if distance_left <= distance_above and distance_left <= distance_upper_left:
        return left
    if distance_above <= distance_upper_left:
        return above
    return upper_left


def decode_png(path: Path) -> bytes:
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError(f"{path}: invalid PNG signature")

    width = height = bit_depth = color_type = None
    compression = filter_method = interlace = None
    palette = None
    transparency = b""
    idat_chunks = []
    saw_iend = False
    position = len(PNG_SIGNATURE)

    while position < len(data):
        if len(data) - position < 12:
            raise ValueError(f"{path}: truncated PNG chunk")

        chunk_length = struct.unpack_from(">I", data, position)[0]
        chunk_type = data[position + 4:position + 8]
        chunk_start = position + 8
        chunk_end = chunk_start + chunk_length
        crc_offset = chunk_end
        if chunk_end < chunk_start or crc_offset + 4 > len(data):
            raise ValueError(f"{path}: PNG chunk extends past end of file")

        chunk_data = data[chunk_start:chunk_end]
        stored_crc = struct.unpack_from(">I", data, crc_offset)[0]
        actual_crc = zlib.crc32(chunk_type)
        actual_crc = zlib.crc32(chunk_data, actual_crc) & 0xFFFFFFFF
        if actual_crc != stored_crc:
            raise ValueError(
                f"{path}: CRC mismatch in {chunk_type.decode('ascii', 'replace')}"
            )

        if chunk_type == b"IHDR":
            if width is not None or chunk_length != 13:
                raise ValueError(f"{path}: invalid IHDR")
            (width, height, bit_depth, color_type, compression,
             filter_method, interlace) = struct.unpack(">IIBBBBB", chunk_data)
        elif chunk_type == b"PLTE":
            palette = chunk_data
        elif chunk_type == b"tRNS":
            transparency = chunk_data
        elif chunk_type == b"IDAT":
            idat_chunks.append(chunk_data)
        elif chunk_type == b"IEND":
            saw_iend = True
            position = crc_offset + 4
            break
        elif not (chunk_type[0] & 0x20):
            raise ValueError(f"{path}: unsupported critical PNG chunk {chunk_type!r}")

        position = crc_offset + 4

    if not saw_iend or width is None or height is None or not idat_chunks:
        raise ValueError(f"{path}: incomplete PNG")
    if (width, height) != (ICON_WIDTH, ICON_HEIGHT):
        raise ValueError(
            f"{path}: expected {ICON_WIDTH}x{ICON_HEIGHT}, got {width}x{height}"
        )
    if bit_depth != 8 or interlace != 0 or compression != 0 or filter_method != 0:
        raise ValueError(f"{path}: only non-interlaced 8-bit PNGs are supported")
    if color_type not in (2, 3, 6):
        raise ValueError(f"{path}: unsupported PNG color type {color_type}")

    channels = {2: 3, 3: 1, 6: 4}[color_type]
    bytes_per_pixel = channels
    stride = width * channels

    if color_type == 3:
        if not palette or len(palette) % 3 or len(palette) > 256 * 3:
            raise ValueError(f"{path}: missing or invalid indexed PNG palette")
        if len(transparency) > len(palette) // 3:
            raise ValueError(f"{path}: palette transparency table is too long")
    elif color_type == 2 and transparency and len(transparency) != 6:
        raise ValueError(f"{path}: invalid truecolor transparency table")

    try:
        raw = zlib.decompress(b"".join(idat_chunks))
    except zlib.error as error:
        raise ValueError(f"{path}: invalid PNG image data: {error}") from error

    expected_raw_size = height * (stride + 1)
    if len(raw) != expected_raw_size:
        raise ValueError(
            f"{path}: decoded scanline data has {len(raw)} bytes; "
            f"expected {expected_raw_size}"
        )

    decoded = bytearray(height * stride)
    previous = bytearray(stride)

    for row_index in range(height):
        raw_offset = row_index * (stride + 1)
        filter_type = raw[raw_offset]
        encoded = raw[raw_offset + 1:raw_offset + 1 + stride]
        row = bytearray(encoded)

        if filter_type > 4:
            raise ValueError(f"{path}: invalid PNG filter {filter_type}")

        for index, value in enumerate(row):
            left = row[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
            above = previous[index]
            upper_left = (
                previous[index - bytes_per_pixel]
                if index >= bytes_per_pixel else 0
            )

            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            else:
                predictor = paeth_predictor(left, above, upper_left)

            row[index] = (value + predictor) & 0xFF

        decoded[row_index * stride:(row_index + 1) * stride] = row
        previous = row

    rgba = bytearray(width * height * 4)
    transparent_rgb = None
    if color_type == 2 and transparency:
        transparent_rgb = struct.unpack(">HHH", transparency)

    for pixel_index in range(width * height):
        source_offset = pixel_index * channels
        target_offset = pixel_index * 4

        if color_type == 3:
            palette_index = decoded[source_offset]
            palette_offset = palette_index * 3
            if palette_offset + 2 >= len(palette):
                raise ValueError(
                    f"{path}: palette index {palette_index} is out of range"
                )
            red = palette[palette_offset]
            green = palette[palette_offset + 1]
            blue = palette[palette_offset + 2]
            alpha = (
                transparency[palette_index]
                if palette_index < len(transparency) else 255
            )
        elif color_type == 6:
            red, green, blue, alpha = decoded[source_offset:source_offset + 4]
        else:
            red, green, blue = decoded[source_offset:source_offset + 3]
            alpha = 0 if transparent_rgb == (red, green, blue) else 255

        rgba[target_offset:target_offset + 4] = bytes((red, green, blue, alpha))

    return bytes(rgba)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("icons", nargs="+", type=Path)
    args = parser.parse_args()

    if len(args.icons) != EXPECTED_ICON_COUNT:
        parser.error(f"expected exactly {EXPECTED_ICON_COUNT} icon PNGs")

    try:
        combined = b"".join(decode_png(path) for path in args.icons)
        expected_size = EXPECTED_ICON_COUNT * ICON_WIDTH * ICON_HEIGHT * 4
        if len(combined) != expected_size:
            raise ValueError(
                f"internal size mismatch: generated {len(combined)}, "
                f"expected {expected_size}"
            )

        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(combined)
    except (OSError, ValueError) as error:
        print(f"[ICONS] {error}", file=sys.stderr)
        return 1

    print(f"[ICONS] Embedded {len(args.icons)} transparent RGBA icons ({len(combined)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
