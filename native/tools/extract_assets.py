#!/usr/bin/env python3
"""Extract the supplied DOS .VOL files for the native renderer.

The source format parser is the GPLv2 nog2vol tool vendored beside this file.
Run from any directory: python3 native/tools/extract_assets.py

SPDX-License-Identifier: GPL-2.0-only
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import zlib
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
ASSET_ROOT = ROOT / "native" / "assets"
sys.path.insert(0, str(Path(__file__).resolve().parent / "nog2vol"))

from assets import extract_release, rgba_png, vga_palette_to_rgb8  # noqa: E402
from nog2vol import build_inventory  # noqa: E402


FIGHTERS = ("KLUBBOR", "FETUS", "HENRY", "GURDIP", "ED", "SINAMMON", "BUDDY", "GONZOLES")
CINEMAS = ("LOGO", "NCRED", "END1", "END2", "END3", "END4", "END5", "END6", "END7", "END8")
SCENES = ("NOGTITLE", "NOGGINBG", "SELECT")
LAYERS = {"scene_layer1a": 0, "scene_layer1b": 1, "scene_layer2": 2, "scene_layer3": 3}


def relative(path: Path) -> str:
    return path.relative_to(ASSET_ROOT).as_posix()


def read_png_rgba(path: Path) -> tuple[int, int, bytes]:
    """Read the filter-0 indexed/RGBA PNGs produced by nog2vol."""
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError(f"invalid PNG: {path}")
    position = 8
    width = height = color_type = -1
    palette = b""
    alpha = b""
    compressed = bytearray()
    while position < len(data):
        if position + 12 > len(data):
            raise ValueError(f"truncated PNG: {path}")
        size = struct.unpack_from(">I", data, position)[0]
        kind = data[position + 4 : position + 8]
        start = position + 8
        end = start + size
        if end + 4 > len(data):
            raise ValueError(f"truncated PNG chunk: {path}")
        expected_crc = struct.unpack_from(">I", data, end)[0]
        actual_crc = zlib.crc32(kind + data[start:end]) & 0xFFFFFFFF
        if expected_crc != actual_crc:
            raise ValueError(f"PNG CRC mismatch: {path}")
        if kind == b"IHDR":
            width, height, depth, color_type, compression, filtering, interlace = struct.unpack(">IIBBBBB", data[start:end])
            if depth != 8 or color_type not in (3, 6) or compression or filtering or interlace:
                raise ValueError(f"unsupported PNG encoding: {path}")
        elif kind == b"PLTE":
            palette = data[start:end]
        elif kind == b"tRNS":
            alpha = data[start:end]
        elif kind == b"IDAT":
            compressed.extend(data[start:end])
        elif kind == b"IEND":
            break
        position = end + 4
    if width <= 0 or height <= 0:
        raise ValueError(f"PNG has no dimensions: {path}")
    channels = 1 if color_type == 3 else 4
    stride = width * channels
    raw = zlib.decompress(bytes(compressed))
    if len(raw) != height * (stride + 1):
        raise ValueError(f"PNG scanline length mismatch: {path}")
    output = bytearray(width * height * 4)
    for y in range(height):
        row = raw[y * (stride + 1) : (y + 1) * (stride + 1)]
        if row[0] != 0:
            raise ValueError(f"unsupported PNG filter: {path}")
        if color_type == 6:
            output[y * width * 4 : (y + 1) * width * 4] = row[1:]
        else:
            if len(palette) % 3:
                raise ValueError(f"invalid PNG palette: {path}")
            for x, index in enumerate(row[1:]):
                offset = index * 3
                if offset + 3 > len(palette):
                    raise ValueError(f"PNG palette index out of range: {path}")
                out = (y * width + x) * 4
                output[out : out + 3] = palette[offset : offset + 3]
                output[out + 3] = alpha[index] if index < len(alpha) else 255
    return width, height, bytes(output)


def write_rgba(path: Path, width: int, height: int, pixels: bytes) -> None:
    if len(pixels) != width * height * 4:
        raise ValueError(f"RGBA pixel length mismatch: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack("<II", width, height) + pixels)


def palette_rgba(palette: bytes) -> bytes:
    """Expand 256 VGA six-bit RGB triples to 256 RGBA8888 colors."""
    rgb = vga_palette_to_rgb8(palette)
    return b"".join(rgb[i : i + 3] + b"\xff" for i in range(0, len(rgb), 3))


def record_bytes(detail: dict[str, Any], role: str) -> bytes:
    matches = [item for item in detail["records"] if item["role"] == role]
    if len(matches) != 1:
        raise ValueError(f"expected one record {role} in {detail['source']['name']}")
    return (ROOT / str(matches[0]["output"]["path"])).read_bytes()


def write_palette(path: Path, payload: bytes) -> str:
    data = palette_rgba(payload)
    if len(data) != 1024:
        raise ValueError(f"palette length mismatch: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return relative(path)


def image_refs(vol: str, image: dict[str, Any]) -> list[tuple[int, int, int]]:
    name = str(image["name"])
    refs: list[tuple[int, int, int]] = []
    if vol in SCENES:
        bank = SCENES.index(vol)
        match = re.fullmatch(r"scene_image_blob_(\d{3})(_gray)?", name)
        if match:
            refs.append((1 if match.group(2) else 0, bank, int(match.group(1))))
        if name in LAYERS:
            refs.append((2, bank, LAYERS[name]))
    elif vol in FIGHTERS:
        match = re.fullmatch(r"character_image_blob_(\d{3})", name)
        if match:
            refs.append((6, FIGHTERS.index(vol), int(match.group(1))))
    elif vol == "HEAD":
        match = re.fullmatch(r"character_image_blob_(\d{3})", name)
        pain = re.fullmatch(r"character_pain_image_blob_(\d{3})", name)
        if match:
            refs.append((6, 8, int(match.group(1))))
        if pain:
            refs.append((9, 8, int(pain.group(1))))
    elif vol == "MISC":
        match = re.fullmatch(r"effects_image_blob_(\d{3})", name)
        if match:
            refs.append((6, 9, int(match.group(1))))
    elif vol in CINEMAS:
        match = re.fullmatch(r"cinema\.image\[(\d+)\]", name)
        if match:
            refs.append((4, CINEMAS.index(vol), int(match.group(1))))
    elif vol == "SELECT2":
        match = re.fullmatch(r"selector_mark\[(\d+)\]", name)
        if match:
            refs.append((3, 2, int(match.group(1))))
    elif vol in ("FONT", "FONT2", "FONT3"):
        match = re.fullmatch(r"glyph_(\d{3})(?:_(white|gold|icer|stump))?", name)
        if match:
            character = int(match.group(1))
            variant = match.group(2)
            if vol == "FONT":
                refs.append((7, 0, character))
            elif vol == "FONT2":
                refs.append((8, 1, character))
            elif variant == "white":
                refs.append((5, 2, character))
            elif variant == "icer":
                refs.append((10, 2, character))
            elif variant == "stump":
                refs.append((11, 2, character))
    return refs


def build_assets() -> dict[str, Any]:
    os.chdir(ROOT)
    inventory = build_inventory(Path("."))
    extracted = ASSET_ROOT / "extracted"
    extract_release(Path("."), Path("native/assets/extracted"))
    images: list[dict[str, Any]] = []
    audio: list[dict[str, Any]] = []
    palettes: list[dict[str, Any]] = []
    references: dict[tuple[int, int, int], dict[str, Any]] = {}
    png_lookup: dict[str, tuple[int, int, bytes]] = {}

    for file_info in sorted(inventory["files"], key=lambda entry: str(entry["name"])):
        vol = str(file_info["name"]).removesuffix(".VOL")
        detail = json.loads((extracted / vol / "manifest.json").read_text())
        if vol in SCENES:
            bank = SCENES.index(vol)
            palette_path = ASSET_ROOT / "palettes" / f"scene_{bank}.rgba"
            palettes.append({
                "kind": 0, "bank": bank, "index": 0,
                "source_vol": vol + ".VOL", "source_role": "scene.palette",
                "rgba": write_palette(palette_path, record_bytes(detail, "scene.palette")),
            })
        elif vol in CINEMAS:
            bank = CINEMAS.index(vol)
            base_role = "cinema.palette"
            base = record_bytes(detail, base_role)
            sentinel = ASSET_ROOT / "palettes" / f"cinema_{bank}_65535.rgba"
            palettes.append({
                "kind": 1, "bank": bank, "index": 65535,
                "source_vol": vol + ".VOL", "source_role": base_role,
                "rgba": write_palette(sentinel, base),
            })
            events = next(item["metadata"]["entries"] for item in detail["records"] if item["role"] == "cinema.events")
            backgrounds = sorted({int(event["data"]) for event in events if int(event["type"]) == 2})
            for index in backgrounds:
                role = f"cinema.image[{index}]"
                record = record_bytes(detail, role)
                if len(record) < 320 * 120 + 768:
                    raise ValueError(f"cinema background lacks palette: {vol}/{role}")
                palette_path = ASSET_ROOT / "palettes" / f"cinema_{bank}_{index}.rgba"
                palettes.append({
                    "kind": 1, "bank": bank, "index": index,
                    "source_vol": vol + ".VOL", "source_role": role + " appended palette",
                    "rgba": write_palette(palette_path, record[320 * 120 : 320 * 120 + 768]),
                })
        for item in detail["images"]:
            name = str(item["name"])
            png_path = ROOT / str(item["png"]["path"])
            width, height, pixels = read_png_rgba(png_path)
            if (width, height) != (int(item["width"]), int(item["height"])):
                raise ValueError(f"decoded dimensions differ from nog2vol manifest: {vol}/{name}")
            rgba_path = ASSET_ROOT / "rgba" / vol / (name + ".rgba")
            write_rgba(rgba_path, width, height, pixels)
            image_id = vol + "/" + name
            image_entry: dict[str, Any] = {
                "id": image_id,
                "source_vol": vol + ".VOL",
                "name": name,
                "width": width,
                "height": height,
                "transparent": bool(item["transparent"]),
                "palette_source": item["palette_source"],
                "source": item["source"],
                "png": relative(png_path),
                "rgba": relative(rgba_path),
            }
            images.append(image_entry)
            png_lookup[image_id] = (width, height, pixels)
            for reference in image_refs(vol, item):
                if reference in references:
                    raise ValueError(f"duplicate compositor reference: {reference}")
                references[reference] = {"kind": reference[0], "bank": reference[1], "index": reference[2], "image_id": image_id, "rgba": relative(rgba_path)}
        for item in detail["audio"]:
            if "wav" in item:
                audio.append({
                    "source_vol": vol + ".VOL",
                    "role": item["role"],
                    "sample_rate_hz": item["sample_rate_hz"],
                    "played_samples": item["played_samples"],
                    "wav": relative(ROOT / str(item["wav"]["path"])),
                })

    first = png_lookup["NOGGINBG/scene_layer1a"]
    second = png_lookup["NOGGINBG/scene_layer1b"]
    if first[:2] != (640, 100) or second[:2] != (640, 100):
        raise ValueError("game layer 1 composite has unexpected source dimensions")
    composite_pixels = first[2] + second[2]
    composite_rgba = ASSET_ROOT / "rgba" / "NOGGINBG" / "scene_layer1_composite.rgba"
    composite_png = ASSET_ROOT / "extracted" / "NOGGINBG" / "images" / "scene_layer1_composite.png"
    write_rgba(composite_rgba, 640, 200, composite_pixels)
    composite_png.write_bytes(rgba_png(640, 200, composite_pixels))
    composite_id = "NOGGINBG/scene_layer1_composite"
    images.append({
        "id": composite_id,
        "source_vol": "NOGGINBG.VOL",
        "name": "scene_layer1_composite",
        "width": 640,
        "height": 200,
        "transparent": True,
        "palette_source": "NOGGINBG.VOL",
        "source": {"derived_from": ["NOGGINBG/scene_layer1a", "NOGGINBG/scene_layer1b"]},
        "png": relative(composite_png),
        "rgba": relative(composite_rgba),
    })
    composite_ref = (2, 1, 4)
    references[composite_ref] = {"kind": 2, "bank": 1, "index": 4, "image_id": composite_id, "rgba": relative(composite_rgba)}

    ordered_refs = [references[key] for key in sorted(references)]
    manifest = {
        "format": "nog2-native-assets-v1",
        "rgba_format": "u32le width, u32le height, then width*height RGBA8888 bytes",
        "source_file_count": inventory["file_count"],
        "source_total_bytes": inventory["total_size"],
        "source_files": [
            {"name": item["name"], "size": item["size"], "sha256": item["sha256"]}
            for item in inventory["files"]
        ],
        "images": images,
        "audio": audio,
        "palettes": sorted(palettes, key=lambda item: (item["kind"], item["bank"], item["index"])),
        "compositor_images": ordered_refs,
    }
    ASSET_ROOT.mkdir(parents=True, exist_ok=True)
    (ASSET_ROOT / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    (ASSET_ROOT / "image_index.tsv").write_text(
        "".join(f'{entry["kind"]}\t{entry["bank"]}\t{entry["index"]}\t{entry["rgba"]}\n' for entry in ordered_refs)
    )
    (ASSET_ROOT / "palette_index.tsv").write_text(
        "".join(f'{entry["kind"]}\t{entry["bank"]}\t{entry["index"]}\t{entry["rgba"]}\n' for entry in manifest["palettes"])
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    manifest = build_assets()
    print(f'extracted {manifest["source_file_count"]} VOL files, {len(manifest["images"])} images, {len(manifest["audio"])} WAVs, {len(manifest["compositor_images"])} compositor mappings, {len(manifest["palettes"])} palettes')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
