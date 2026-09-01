#!/usr/bin/env python3
"""从 CK-GAL 字形图集和汉化脚本重建 raw-code -> Unicode 映射。

这是离线维护工具，不参与游戏运行。它把同一个字形在多条完整中文句子中交给
RapidOCR 识别，只接受长度一致且多次结果达成共识的映射；不确定项留给运行时
fon.pak 位图回退，绝不猜字。

用法:
  python tools/rebuild_ckgal_map.py ck-gal.pak 本体80.png src/wa2/ckgal_map.inc
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import sys
from pathlib import Path

import cv2
import numpy as np
from rapidocr_onnxruntime import RapidOCR

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pak  # noqa: E402


RANGES = [
    (0x0020, 0x007D), (0x00B1, 0x00DD),
    (0x8140, 0x81AC), (0x81B8, 0x81BF), (0x81C8, 0x81CE), (0x81DA, 0x81FC),
    (0x824F, 0x8258), (0x8260, 0x8279), (0x8281, 0x829A), (0x829F, 0x82F1),
    (0x8340, 0x8396), (0x839F, 0x83B6), (0x83BF, 0x83D6),
    (0x8440, 0x8460), (0x8470, 0x8491), (0x849F, 0x84BE),
    (0x8740, 0x875D), (0x875F, 0x8775), (0x877E, 0x878F), (0x889F, 0x88FC),
    (0x8940, 0x89FC), (0x8A40, 0x8AFC), (0x8B40, 0x8BFC), (0x8C40, 0x8CFC),
    (0x8D40, 0x8DFC), (0x8E40, 0x8EFC), (0x8F40, 0x8FFC), (0x9040, 0x90FC),
    (0x9140, 0x91FC), (0x9240, 0x92FC), (0x9340, 0x93FC), (0x9440, 0x94FC),
    (0x9540, 0x95FC), (0x9640, 0x96FC), (0x9740, 0x97FC), (0x9840, 0x9872),
    (0x989F, 0x98FC), (0x9940, 0x99FC), (0x9A40, 0x9AFC), (0x9B40, 0x9BFC),
    (0x9C40, 0x9CFC), (0x9D40, 0x9DFC), (0x9E40, 0x9EFC), (0x9F40, 0x9FFC),
    (0xE040, 0xE0FC), (0xE140, 0xE1FC), (0xE240, 0xE2FC), (0xE340, 0xE3FC),
    (0xE440, 0xE4FC), (0xE540, 0xE5FC), (0xE640, 0xE6FC), (0xE740, 0xE7FC),
    (0xE840, 0xE8FC), (0xE940, 0xE9FC), (0xEA40, 0xEAA4),
]


def font_slot(code: int) -> int:
    slot = 0
    for start, end in RANGES:
        if start <= code <= end:
            return slot + code - start
        slot += end - start + 1
    return -1


def raw_tokens(data: bytes) -> list[int]:
    result: list[int] = []
    i = 0
    while i < len(data):
        first = data[i]
        if ((0x81 <= first <= 0x9F) or (0xE0 <= first <= 0xFE)) and i + 1 < len(data):
            result.append((first << 8) | data[i + 1])
            i += 2
        else:
            result.append(first)
            i += 1
    return result


def visible_chunks(data: bytes, max_chars: int = 36) -> list[tuple[int, ...]]:
    """丢弃排版标记，把换行/等待键当作可靠的 OCR 分句边界。"""
    tokens = raw_tokens(data)
    chunks: list[list[int]] = [[]]
    i = 0
    while i < len(tokens):
        code = tokens[i]
        if code == ord("<"):
            # Ruby/样式标签会令 OCR 字符数与源槽位不一致，整段跳过更保守。
            return []
        if code == ord("\\") and i + 1 < len(tokens):
            control = tokens[i + 1]
            if control in (ord("n"), ord("k")):
                if chunks[-1]:
                    chunks.append([])
                i += 2
                continue
            if control in map(ord, "<>\\^|~"):
                chunks[-1].append(control)
                i += 2
                continue
            # 其他反斜杠命令不是可见字。
            i += 2
            continue
        if code in (ord("^"), ord("`"), ord("~")):
            i += 1
            continue
        if font_slot(code) >= 0:
            chunks[-1].append(code)
        i += 1

    output: list[tuple[int, ...]] = []
    for chunk in chunks:
        for start in range(0, len(chunk), max_chars):
            part = tuple(chunk[start:start + max_chars])
            if len(part) >= 3:
                output.append(part)
    return output


def load_segments(pack_path: Path) -> tuple[list[tuple[int, ...]], collections.Counter[int]]:
    buf, entries = pak.read_pack(str(pack_path))
    unique: dict[tuple[int, ...], None] = {}
    frequency: collections.Counter[int] = collections.Counter()
    for name, offset, size, compressed in entries:
        if not name.endswith(".txt") or not size:
            continue
        data = buf[offset:offset + size]
        if compressed:
            data = pak.lzss_decompress(data)
        for field in data.split(b","):
            for segment in visible_chunks(field):
                unique.setdefault(segment, None)
                frequency.update(code for code in segment if code >= 0x80)
    return list(unique), frequency


def choose_segments(segments: list[tuple[int, ...]], votes_per_code: int) -> list[tuple[int, ...]]:
    occurrences: dict[int, list[int]] = collections.defaultdict(list)
    for index, segment in enumerate(segments):
        for code in set(segment):
            if code >= 0x80:
                occurrences[code].append(index)
    selected: set[int] = set()
    for indices in occurrences.values():
        if len(indices) <= votes_per_code:
            selected.update(indices)
            continue
        for n in range(votes_per_code):
            selected.add(indices[round(n * (len(indices) - 1) / (votes_per_code - 1))])
    return [segments[i] for i in sorted(selected)]


def atlas_gray(path: Path) -> np.ndarray:
    image = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    if image is None or image.shape[:2] != (3840, 3200):
        raise ValueError(f"invalid atlas: {path}")
    if image.ndim == 3 and image.shape[2] == 4:
        return (image[:, :, 0].astype(np.float32) * image[:, :, 3] / 255.0).astype(np.uint8)
    return image[:, :, 0] if image.ndim == 3 else image


def render_segment(atlas: np.ndarray, segment: tuple[int, ...]) -> np.ndarray:
    image = np.zeros((40, 40 * len(segment)), dtype=np.uint8)
    for index, code in enumerate(segment):
        slot = font_slot(code)
        x, y = slot % 80 * 40, slot // 80 * 40
        image[:, index * 40:(index + 1) * 40] = atlas[y:y + 40, x:x + 40]
    return cv2.resize(image, None, fx=2, fy=2, interpolation=cv2.INTER_CUBIC)


def is_alignment_safe(segment: tuple[int, ...], text: str) -> bool:
    if len(text) != len(segment):
        return False
    # ASCII 字母/数字是强锚点；不一致通常意味着 OCR 合并或错位。
    for code, char in zip(segment, text):
        if code < 0x80 and chr(code).isalnum() and chr(code) != char:
            return False
    return True


def write_include(path: Path, mapping: dict[int, int], stats: dict) -> None:
    lines = [
        "// Generated by tools/rebuild_ckgal_map.py; do not edit by hand.",
        f"// accepted={len(mapping)} weighted_coverage={stats['weighted_coverage']:.6f}",
        "static const CkgalMapEntry kCkgalMap[] = {",
    ]
    for raw, codepoint in sorted(mapping.items()):
        lines.append(f"    {{0x{raw:04X}, 0x{codepoint:04X}}},")
    lines.append("};")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("pack", type=Path)
    parser.add_argument("atlas", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--votes", type=int, default=4)
    parser.add_argument("--min-confidence", type=float, default=0.92)
    args = parser.parse_args()

    segments, frequency = load_segments(args.pack)
    selected = choose_segments(segments, max(2, args.votes))
    print(f"unique segments={len(segments)} selected={len(selected)} raw codes={len(frequency)}")

    atlas = atlas_gray(args.atlas)
    ocr = RapidOCR()
    votes: dict[int, collections.Counter[int]] = collections.defaultdict(collections.Counter)
    accepted_lines = 0
    for index, segment in enumerate(selected, 1):
        result, _ = ocr(render_segment(atlas, segment), use_det=False, use_cls=False, use_rec=True)
        if result:
            text, confidence = result[0][0], float(result[0][1])
            text = text.strip()
            if confidence >= args.min_confidence and is_alignment_safe(segment, text):
                accepted_lines += 1
                weight = max(1, round(confidence * 100))
                for raw, char in zip(segment, text):
                    if raw >= 0x80:
                        votes[raw][ord(char)] += weight
        if index % 1000 == 0:
            print(f"ocr {index}/{len(selected)} accepted={accepted_lines}", flush=True)

    mapping: dict[int, int] = {}
    audit = {}
    for raw, choices in votes.items():
        ranked = choices.most_common()
        total = sum(choices.values())
        best, score = ranked[0]
        ratio = score / total
        # 两条高置信上下文达成多数，或单条近满分且没有冲突。
        if ratio >= 0.72 and (score >= 180 or (len(ranked) == 1 and score >= 99)):
            mapping[raw] = best
        audit[f"{raw:04X}"] = {
            "accepted": raw in mapping,
            "ratio": ratio,
            "choices": [[f"U+{cp:04X}", value] for cp, value in ranked[:5]],
            "frequency": frequency[raw],
        }

    covered = sum(count for raw, count in frequency.items() if raw in mapping)
    total = sum(frequency.values())
    stats = {
        "raw_codes": len(frequency),
        "mapped_codes": len(mapping),
        "accepted_lines": accepted_lines,
        "selected_lines": len(selected),
        "weighted_coverage": covered / total if total else 0.0,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_include(args.output, mapping, stats)
    args.output.with_suffix(".audit.json").write_text(
        json.dumps({"stats": stats, "codes": audit}, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(json.dumps(stats, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
