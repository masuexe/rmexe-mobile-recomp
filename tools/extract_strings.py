#!/usr/bin/env python3
"""
Bilingual JP + EN -> ZH String Extraction & Validation Tool for rmexe-mobile-recomp.

Extracts all translatable strings from DoJa game assets (.jar and .sp) into
clean JSON translation overlays (pon_zh.json / lon_zh.json) without storing any
proprietary game data in the repository itself.
"""

from __future__ import annotations

import argparse
import difflib
import json
import os
import struct
import sys
import zipfile
import zlib
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from survey_doja import parse_class_bytes


def fnv1a_64(data: bytes) -> str:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"0x{h:016x}"


def inflate_pk_stream(data: bytes) -> bytes:
    if data[:4] != b"PK\x03\x04":
        return data
    if len(data) < 30:
        return b""
    comp = struct.unpack_from("<H", data, 8)[0]
    usz = struct.unpack_from("<I", data, 22)[0]
    fnl, exl = struct.unpack_from("<HH", data, 26)
    start = 30 + fnl + exl
    if comp == 0:
        return data[start : start + usz]
    if comp == 8:
        return zlib.decompressobj(-15).decompress(data[start:])
    raise ValueError(f"Unsupported PK compression method {comp}")


def read_sp_toc(sp_path: Path) -> dict[int, bytes]:
    raw = sp_path.read_bytes()
    hdr = 64 if (len(raw) % 1024 != 0) else 0
    toc = struct.unpack_from(">60i", raw, hdr)
    pos = raw.find(b"PK\x03\x04")
    if pos < 0:
        raise ValueError(f"Could not find first PK\\x03\\x04 stream in {sp_path}")
    entries: dict[int, bytes] = {}
    for idx, sz in enumerate(toc):
        if sz <= 0:
            continue
        chunk = raw[pos : pos + sz]
        pos += sz
        if chunk[:4] == b"PK\x03\x04":
            entries[idx] = inflate_pk_stream(chunk)
        else:
            entries[idx] = chunk
    return entries


def decode_cp932_slot(slot_bytes: bytes) -> str:
    nul = slot_bytes.find(0)
    if nul >= 0:
        slot_bytes = slot_bytes[:nul]
    return slot_bytes.decode("cp932", errors="replace")


def decode_fixed_lines(payload: bytes, line_bytes: int) -> list[str]:
    if len(payload) % line_bytes != 0:
        raise ValueError(
            f"Stream length {len(payload)} is not a multiple of line_bytes={line_bytes}"
        )
    count = len(payload) // line_bytes
    return [
        decode_cp932_slot(payload[i * line_bytes : (i + 1) * line_bytes])
        for i in range(count)
    ]


def decode_crlf_lines(payload: bytes) -> tuple[list[str], bool]:
    text = payload.decode("cp932", errors="replace")
    trailing_crlf = text.endswith("\r\n")
    if trailing_crlf:
        text = text[:-2]
    lines = text.split("\r\n") if text else []
    return lines, trailing_crlf


def has_non_ascii(s: str) -> bool:
    return any(ord(c) > 0x7F for c in s)


def align_class_strings(
    jp_list: list[str], en_list: list[str] | None
) -> dict[int, str]:
    if not en_list:
        return {}
    if len(jp_list) == len(en_list):
        return {i: en_list[i] for i in range(len(jp_list))}
    a1 = [s if not has_non_ascii(s) else "<CJK>" for s in jp_list]
    a2 = [s if not has_non_ascii(s) else "<CJK>" for s in en_list]
    sm = difflib.SequenceMatcher(None, a1, a2, autojunk=False)
    mapped: dict[int, str] = {}
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag in ("equal", "replace") and (i2 - i1) == (j2 - j1):
            for k in range(i2 - i1):
                mapped[i1 + k] = en_list[j1 + k]
    return mapped


def extract_class_strings(
    z_jp: zipfile.ZipFile, z_en: zipfile.ZipFile | None
) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    seen_jp: set[str] = set()
    en_names = set(z_en.namelist()) if z_en else set()

    for fn in sorted(z_jp.namelist()):
        if not fn.endswith(".class"):
            continue
        cls_jp = parse_class_bytes(z_jp.read(fn))
        cls_en = (
            parse_class_bytes(z_en.read(fn)) if (z_en and fn in en_names) else None
        )
        en_map = align_class_strings(
            cls_jp.constant_strings,
            cls_en.constant_strings if cls_en else None,
        )
        short_cls = fn[:-6].replace("/", ".")
        for idx, s_jp in enumerate(cls_jp.constant_strings):
            if not has_non_ascii(s_jp):
                continue
            if s_jp in seen_jp:
                continue
            seen_jp.add(s_jp)
            results.append(
                {
                    "id": f"{short_cls}#{idx}",
                    "jp": s_jp,
                    "en": en_map.get(idx, ""),
                    "zh": "",
                }
            )
    return results


def build_fixed_stream(
    stream_id: str,
    description: str,
    jp_bytes: bytes,
    en_bytes: bytes | None,
    line_bytes: int,
    lines_per_record: int,
) -> dict[str, Any]:
    jp_lines = decode_fixed_lines(jp_bytes, line_bytes)
    en_lines = (
        decode_fixed_lines(en_bytes, line_bytes)
        if (en_bytes is not None and len(en_bytes) > 0 and len(en_bytes) % line_bytes == 0)
        else []
    )

    entries: list[dict[str, Any]] = []
    rec_idx = 0
    for pos in range(0, len(jp_lines), lines_per_record):
        j_slice = jp_lines[pos : pos + lines_per_record]
        e_slice = en_lines[pos : pos + lines_per_record] if pos < len(en_lines) else []
        if lines_per_record == 1:
            entries.append(
                {
                    "index": rec_idx,
                    "jp": j_slice[0],
                    "en": e_slice[0] if e_slice else "",
                    "zh": "",
                }
            )
        else:
            if e_slice and len(e_slice) < len(j_slice):
                e_slice = e_slice + [""] * (len(j_slice) - len(e_slice))
            entries.append(
                {
                    "index": rec_idx,
                    "jp": j_slice,
                    "en": e_slice,
                    "zh": [],
                }
            )
        rec_idx += 1

    obj: dict[str, Any] = {
        "id": stream_id,
        "description": description,
        "format": "fixed",
        "line_bytes": line_bytes,
        "lines_per_record": lines_per_record,
        "orig_len": len(jp_bytes),
        "jp_fnv1a": fnv1a_64(jp_bytes),
    }
    if en_bytes is not None and len(en_bytes) > 0 and en_bytes != jp_bytes:
        obj["en_orig_len"] = len(en_bytes)
        obj["en_fnv1a"] = fnv1a_64(en_bytes)
    obj["entries"] = entries
    return obj


def build_crlf_stream(
    stream_id: str,
    description: str,
    jp_bytes: bytes,
    en_bytes: bytes | None,
) -> dict[str, Any]:
    jp_lines, trailing_crlf = decode_crlf_lines(jp_bytes)
    en_lines, _ = decode_crlf_lines(en_bytes) if en_bytes else ([], False)

    entries: list[dict[str, Any]] = []
    for i, j_line in enumerate(jp_lines):
        entries.append(
            {
                "index": i,
                "jp": j_line,
                "en": en_lines[i] if i < len(en_lines) else "",
                "zh": "",
            }
        )

    obj: dict[str, Any] = {
        "id": stream_id,
        "description": description,
        "format": "crlf",
        "trailing_crlf": trailing_crlf,
        "orig_len": len(jp_bytes),
        "jp_fnv1a": fnv1a_64(jp_bytes),
    }
    if en_bytes is not None and len(en_bytes) > 0 and en_bytes != jp_bytes:
        obj["en_orig_len"] = len(en_bytes)
        obj["en_fnv1a"] = fnv1a_64(en_bytes)
    obj["entries"] = entries
    return obj


def parse_sdat_chunks(sdat_bytes: bytes) -> list[bytes]:
    first_pk = sdat_bytes.find(b"PK\x03\x04")
    if first_pk < 0 or first_pk % 4 != 0:
        return []
    cnt = first_pk // 4
    sizes = struct.unpack_from(f">{cnt}I", sdat_bytes, 0)
    off = first_pk
    chunks: list[bytes] = []
    for sz in sizes:
        chunk = sdat_bytes[off : off + sz]
        off += sz
        chunks.append(inflate_pk_stream(chunk))
    return chunks


def extract_game_json(
    game: str,
    jp_jar_path: Path,
    jp_sp_path: Path,
    en_jar_path: Path | None,
    en_sp_path: Path | None,
) -> dict[str, Any]:
    if not jp_jar_path.is_file():
        raise FileNotFoundError(f"JP JAR not found: {jp_jar_path}")
    if not jp_sp_path.is_file():
        raise FileNotFoundError(f"JP SP not found: {jp_sp_path}")

    z_jp = zipfile.ZipFile(jp_jar_path)
    z_en = zipfile.ZipFile(en_jar_path) if (en_jar_path and en_jar_path.is_file()) else None

    sp_jp = read_sp_toc(jp_sp_path)
    sp_en = read_sp_toc(en_sp_path) if (en_sp_path and en_sp_path.is_file()) else {}

    class_strings = extract_class_strings(z_jp, z_en)
    streams: list[dict[str, Any]] = []

    # 1. ScratchPad (.sp) text tables
    sp_fixed_specs = [
        (22, "battle_chips", 16, 1),
        (12, "chip_descriptions", 30, 2),
        (19, "navicust_names", 16, 1),
        (13, "navicust_descriptions", 30, 2),
        (29, "enemy_names", 16, 1),
        (11, "system_messages", 30, 3),
        (35, "menu_help_messages", 30, 3),
    ]
    if game == "pon":
        sp_fixed_specs.extend(
            [
                (9, "mail_titles", 18, 1),
                (10, "mail_bodies", 30, 3),
            ]
        )

    for idx, desc, lb, lpr in sp_fixed_specs:
        if idx in sp_jp and len(sp_jp[idx]) > 0:
            streams.append(
                build_fixed_stream(
                    f"sp:{idx}", desc, sp_jp[idx], sp_en.get(idx), lb, lpr
                )
            )

    if game == "lon":
        for idx, desc in [
            (37, "area_names"),
            (38, "map_names"),
            (40, "ui_labels"),
        ]:
            if idx in sp_jp and len(sp_jp[idx]) > 0:
                streams.append(
                    build_crlf_stream(f"sp:{idx}", desc, sp_jp[idx], sp_en.get(idx))
                )

    # 2. JAR data/new/ tables
    jp_names = set(z_jp.namelist())
    en_names = set(z_en.namelist()) if z_en else set()

    new_dat_specs = [
        ("data/new/sys_mes2.dat", "network_system_messages", 30, 3),
        ("data/new/sina_mes2.dat", "network_scenario_messages", 30, 3),
    ]
    if game == "pon":
        new_dat_specs.append(("data/new/10mail_mes.dat", "network_mail_messages", 30, 3))

    for rel, desc, lb, lpr in new_dat_specs:
        if rel in jp_names:
            b_jp = z_jp.read(rel)
            b_en = z_en.read(rel) if (z_en and rel in en_names) else None
            streams.append(build_fixed_stream(f"jar:{rel}", desc, b_jp, b_en, lb, lpr))

    # 3. JAR data/quest/ tables
    if "data/quest/quest.dat" in jp_names:
        b_jp = z_jp.read("data/quest/quest.dat")
        b_en = z_en.read("data/quest/quest.dat") if (z_en and "data/quest/quest.dat" in en_names) else None
        streams.append(
            build_fixed_stream("jar:data/quest/quest.dat", "quest_board_summaries", b_jp, b_en, 30, 1)
        )

    quest_dirs: list[int] = []
    for n in jp_names:
        parts = n.split("/")
        if len(parts) == 4 and parts[0] == "data" and parts[1] == "quest" and parts[2].isdigit():
            qnum = int(parts[2])
            if qnum not in quest_dirs:
                quest_dirs.append(qnum)
    quest_dirs.sort()

    for qnum in quest_dirs:
        s_jar = f"data/quest/{qnum}/s.jar"
        if s_jar in jp_names:
            u_jp = inflate_pk_stream(z_jp.read(s_jar))
            u_en = inflate_pk_stream(z_en.read(s_jar)) if (z_en and s_jar in en_names) else None
            if len(u_jp) > 0:
                streams.append(
                    build_fixed_stream(f"jar:{s_jar}", f"quest_{qnum}_dialogue", u_jp, u_en, 30, 3)
                )
        t_dat = f"data/quest/{qnum}/t.dat"
        if t_dat in jp_names:
            u_jp = z_jp.read(t_dat)
            u_en = z_en.read(t_dat) if (z_en and t_dat in en_names) else None
            if len(u_jp) > 0:
                streams.append(
                    build_fixed_stream(f"jar:{t_dat}", f"quest_{qnum}_title", u_jp, u_en, 30, 1)
                )

    # 4. JAR data/scenario/<ch>/s.dat (Chapters 1..9)
    scen_dirs: list[int] = []
    for n in jp_names:
        parts = n.split("/")
        if len(parts) == 4 and parts[0] == "data" and parts[1] == "scenario" and parts[2].isdigit():
            cnum = int(parts[2])
            if cnum not in scen_dirs:
                scen_dirs.append(cnum)
    scen_dirs.sort()

    for cnum in scen_dirs:
        s_dat = f"data/scenario/{cnum}/s.dat"
        if s_dat not in jp_names:
            continue
        jp_chunks = parse_sdat_chunks(z_jp.read(s_dat))
        en_chunks = parse_sdat_chunks(z_en.read(s_dat)) if (z_en and s_dat in en_names) else []
        for k, u_jp in enumerate(jp_chunks):
            if len(u_jp) == 0:
                continue
            u_en = en_chunks[k] if k < len(en_chunks) else None
            streams.append(
                build_fixed_stream(
                    f"jar:{s_dat}#{k}",
                    f"chapter_{cnum}_dialogue_part_{k}",
                    u_jp,
                    u_en,
                    30,
                    3,
                )
            )

    return {
        "schema_version": 1,
        "game": game,
        "encoding_notes": {
            "byte_budget_rule": "ASCII and half-width katakana (U+FF61..U+FF9F) count as 1 byte; full-width characters and CJK hanzi count as 2 bytes.",
            "control_markers": "Preserve U+FFE5 (￥, parameter substitution), U+FF04 (＄, prompt/sentinel), and U+FF03 (＃, # key) when present in jp.",
        },
        "class_strings": class_strings,
        "streams": streams,
    }


def has_nonempty_zh(val: Any) -> bool:
    if isinstance(val, str):
        return len(val) > 0
    if isinstance(val, list):
        return any(isinstance(x, str) and len(x) > 0 for x in val)
    return False


def merge_existing_translations(new_doc: dict[str, Any], old_path: Path) -> int:
    if not old_path.is_file():
        return 0
    try:
        old_doc = json.loads(old_path.read_text(encoding="utf-8"))
    except Exception:
        return 0

    merged_count = 0
    old_by_id: dict[str, tuple[Any, Any]] = {}
    old_by_jp: dict[str, Any] = {}
    for entry in old_doc.get("class_strings", []):
        zh = entry.get("zh", "")
        if has_nonempty_zh(zh):
            if "id" in entry:
                old_by_id[entry["id"]] = (entry.get("jp"), zh)
            if "jp" in entry:
                old_by_jp[entry["jp"]] = zh

    for entry in new_doc.get("class_strings", []):
        cid = entry.get("id", "")
        jp = entry.get("jp", "")
        if cid in old_by_id and old_by_id[cid][0] == jp:
            entry["zh"] = old_by_id[cid][1]
            merged_count += 1
        elif jp in old_by_jp:
            entry["zh"] = old_by_jp[jp]
            merged_count += 1

    old_streams: dict[str, dict[int, tuple[Any, Any]]] = {}
    for st in old_doc.get("streams", []):
        sid = st.get("id", "")
        smap: dict[int, tuple[Any, Any]] = {}
        for e in st.get("entries", []):
            if has_nonempty_zh(e.get("zh")):
                smap[int(e["index"])] = (e.get("jp"), e.get("zh"))
        if smap:
            old_streams[sid] = smap

    for st in new_doc.get("streams", []):
        smap = old_streams.get(st.get("id", ""))
        if not smap:
            continue
        for e in st.get("entries", []):
            idx = int(e["index"])
            if idx in smap and smap[idx][0] == e.get("jp"):
                e["zh"] = smap[idx][1]
                merged_count += 1

    return merged_count


def doja_byte_len(text: str) -> int:
    total = 0
    for ch in text:
        cp = ord(ch)
        if cp < 0x80 or (0xFF61 <= cp <= 0xFF9F):
            total += 1
        else:
            total += 2
    return total


def validate_translation_json(json_path: Path) -> bool:
    doc = json.loads(json_path.read_text(encoding="utf-8"))
    errors: list[str] = []
    warnings: list[str] = []
    translated_cls = 0
    translated_rec = 0
    total_cls = len(doc.get("class_strings", []))
    total_rec = sum(len(st.get("entries", [])) for st in doc.get("streams", []))

    for entry in doc.get("class_strings", []):
        zh = entry.get("zh", "")
        if not zh:
            continue
        translated_cls += 1
        jp = entry.get("jp", "")
        for marker in ("\uFFE5", "\uFF04", "\uFF03"):
            if jp.count(marker) != zh.count(marker):
                warnings.append(
                    f"[class:{entry.get('id')}] marker '{marker}' count mismatch: jp={jp.count(marker)} zh={zh.count(marker)}"
                )

    for st in doc.get("streams", []):
        sid = st.get("id", "?")
        fmt = st.get("format", "fixed")
        lb = int(st.get("line_bytes", 30))
        lpr = int(st.get("lines_per_record", 1))
        for e in st.get("entries", []):
            zh = e.get("zh")
            if not has_nonempty_zh(zh):
                continue
            translated_rec += 1
            idx = e.get("index")
            jp = e.get("jp")
            if fmt == "fixed":
                jp_lines = [jp] if isinstance(jp, str) else list(jp or [])
                zh_lines = [zh] if isinstance(zh, str) else list(zh or [])
                if len(zh_lines) > len(jp_lines):
                    errors.append(
                        f"[{sid}#{idx}] zh has {len(zh_lines)} lines, exceeds slot capacity {len(jp_lines)}"
                    )
                for li, zline in enumerate(zh_lines):
                    blen = doja_byte_len(zline)
                    if blen > lb:
                        errors.append(
                            f"[{sid}#{idx} line {li}] {blen} bytes > limit {lb}: {zline!r}"
                        )
                jp_joined = "\n".join(jp_lines)
                zh_joined = "\n".join(zh_lines)
            else:
                jp_joined = jp if isinstance(jp, str) else ""
                zh_joined = zh if isinstance(zh, str) else ""
            for marker in ("\uFFE5", "\uFF04", "\uFF03"):
                if jp_joined.count(marker) != zh_joined.count(marker):
                    errors.append(
                        f"[{sid}#{idx}] marker '{marker}' count mismatch: jp={jp_joined.count(marker)} zh={zh_joined.count(marker)}"
                    )

    print(
        f"[validate] {json_path}: class_strings={translated_cls}/{total_cls}, "
        f"stream_records={translated_rec}/{total_rec}, errors={len(errors)}, warnings={len(warnings)}"
    )
    for w in warnings:
        print("  WARN:", w)
    for err in errors:
        print("  ERROR:", err)
    return len(errors) == 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Extract and validate JP/EN -> ZH translation overlays.")
    parser.add_argument("--game", choices=["pon", "lon", "all"], default="all")
    parser.add_argument("--jp-jar", type=Path, help="Override JP .jar path (when --game pon or lon)")
    parser.add_argument("--jp-sp", type=Path, help="Override JP .sp path (when --game pon or lon)")
    parser.add_argument("--en-jar", type=Path, help="Optional EN .jar path")
    parser.add_argument("--en-sp", type=Path, help="Optional EN .sp path")
    parser.add_argument("-o", "--out-dir", type=Path, default=Path("assets/translations"))
    parser.add_argument("--no-merge", action="store_true", help="Do not preserve existing zh strings")
    parser.add_argument("--validate", type=Path, help="Validate an existing translation JSON file")
    args = parser.parse_args()

    if args.validate:
        ok = validate_translation_json(args.validate)
        return 0 if ok else 1

    if args.game == "all" and (args.jp_jar or args.jp_sp or args.en_jar or args.en_sp):
        parser.error("Path overrides require --game pon or --game lon")

    defaults = {
        "pon": (
            Path("assets/PoN_jp/bin/PoN.jar"),
            Path("assets/PoN_jp/sp/PoN.orig.sp"),
            Path("assets/PoN_jp/sp/PoN.sp"),
            Path("assets/PoN/bin/PoN.jar"),
            Path("assets/PoN/sp/PoN.orig.sp"),
            Path("assets/PoN/sp/PoN.sp"),
        ),
        "lon": (
            Path("assets/LoN_jp/bin/LoN.jar"),
            Path("assets/LoN_jp/sp/LoN.orig.sp"),
            Path("assets/LoN_jp/sp/LoN.sp"),
            Path("assets/LoN/bin/LoN.jar"),
            Path("assets/LoN/sp/LoN.orig.sp"),
            Path("assets/LoN/sp/LoN.sp"),
        ),
    }

    args.out_dir.mkdir(parents=True, exist_ok=True)
    games = ["pon", "lon"] if args.game == "all" else [args.game]

    for g in games:
        jp_j_def, jp_sp_orig, jp_sp_fallback, en_j_def, en_sp_orig, en_sp_fallback = defaults[g]
        jp_jar = args.jp_jar or jp_j_def
        jp_sp = args.jp_sp or (jp_sp_orig if jp_sp_orig.is_file() else jp_sp_fallback)
        en_jar = args.en_jar or (en_j_def if en_j_def.is_file() else None)
        en_sp = args.en_sp or (
            en_sp_orig if en_sp_orig.is_file() else (en_sp_fallback if en_sp_fallback.is_file() else None)
        )

        doc = extract_game_json(g, jp_jar, jp_sp, en_jar, en_sp)
        out_path = args.out_dir / f"{g}_zh.json"
        merged = 0 if args.no_merge else merge_existing_translations(doc, out_path)
        out_path.write_text(json.dumps(doc, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

        total_records = sum(len(s["entries"]) for s in doc["streams"])
        print(
            f"[extract] {g}: wrote {out_path} "
            f"(class_strings={len(doc['class_strings'])}, streams={len(doc['streams'])}, "
            f"stream_records={total_records}, preserved_zh={merged})"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
