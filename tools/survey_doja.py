#!/usr/bin/env python3
"""
Phase 1 DoJa Bytecode & ScratchPad Survey Tool for rmexe-mobile-recomp.

Analyzes DoJa (.jar, .sp, .jam) game files to extract:
1. Class hierarchy, entry points (IApplication, Canvas, Runnable), fields, and methods.
2. Complete DoJa / CLDC external API call census (com.nttdocomo.*, javax.microedition.*).
3. Frame rate timer / main loop candidates (Thread.sleep, currentTimeMillis, ShortTimer).
4. ScratchPad (.sp) structure and embedded resource signatures (MFi/MLD 'melo', GIF, etc.).
5. Initial TOML symbol mapping file for the static recompiler.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import zipfile
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


# JVM Constant Pool Tags
CONSTANT_Utf8 = 1
CONSTANT_Integer = 3
CONSTANT_Float = 4
CONSTANT_Long = 5
CONSTANT_Double = 6
CONSTANT_Class = 7
CONSTANT_String = 8
CONSTANT_Fieldref = 9
CONSTANT_Methodref = 10
CONSTANT_InterfaceMethodref = 11
CONSTANT_NameAndType = 12


@dataclass
class MemberInfo:
    access_flags: int
    name: str
    descriptor: str
    code_size: int = 0
    max_stack: int = 0
    max_locals: int = 0
    called_methods: list[tuple[str, str, str]] = field(default_factory=list)
    accessed_fields: list[tuple[str, str, str]] = field(default_factory=list)
    string_literals: list[str] = field(default_factory=list)


@dataclass
class ClassFileInfo:
    name: str
    super_name: str
    access_flags: int
    interfaces: list[str]
    fields: list[MemberInfo]
    methods: list[MemberInfo]
    constant_strings: list[str]
    external_method_refs: list[tuple[str, str, str]]
    external_field_refs: list[tuple[str, str, str]]


class ClassReader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0

    def u1(self) -> int:
        val = self.data[self.pos]
        self.pos += 1
        return val

    def u2(self) -> int:
        val = struct.unpack_from(">H", self.data, self.pos)[0]
        self.pos += 2
        return val

    def u4(self) -> int:
        val = struct.unpack_from(">I", self.data, self.pos)[0]
        self.pos += 4
        return val

    def read_bytes(self, length: int) -> bytes:
        res = self.data[self.pos : self.pos + length]
        self.pos += length
        return res


def parse_class_bytes(data: bytes) -> ClassFileInfo:
    r = ClassReader(data)
    magic = r.u4()
    if magic != 0xCAFEBABE:
        raise ValueError(f"Invalid class magic: 0x{magic:08X}")
    _minor = r.u2()
    _major = r.u2()

    cp_count = r.u2()
    cp: list[Any] = [None] * cp_count
    i = 1
    while i < cp_count:
        tag = r.u1()
        if tag == CONSTANT_Utf8:
            length = r.u2()
            raw = r.read_bytes(length)
            cp[i] = ("Utf8", raw.decode("utf-8", errors="replace"))
        elif tag in (CONSTANT_Integer, CONSTANT_Float):
            cp[i] = (tag, r.u4())
        elif tag in (CONSTANT_Long, CONSTANT_Double):
            cp[i] = (tag, r.read_bytes(8))
            i += 1
        elif tag == CONSTANT_Class:
            cp[i] = ("Class", r.u2())
        elif tag == CONSTANT_String:
            cp[i] = ("String", r.u2())
        elif tag in (CONSTANT_Fieldref, CONSTANT_Methodref, CONSTANT_InterfaceMethodref):
            cp[i] = (tag, r.u2(), r.u2())
        elif tag == CONSTANT_NameAndType:
            cp[i] = ("NameAndType", r.u2(), r.u2())
        else:
            raise ValueError(f"Unsupported constant pool tag {tag} at index {i}")
        i += 1

    def get_utf8(idx: int) -> str:
        entry = cp[idx]
        return entry[1] if entry and entry[0] == "Utf8" else f"<cp#{idx}>"

    def get_class_name(idx: int) -> str:
        if idx == 0:
            return ""
        entry = cp[idx]
        if entry and entry[0] == "Class":
            return get_utf8(entry[1])
        return f"<class#{idx}>"

    def resolve_ref(idx: int) -> tuple[str, str, str] | None:
        if idx <= 0 or idx >= len(cp) or cp[idx] is None:
            return None
        entry = cp[idx]
        if entry[0] in (CONSTANT_Fieldref, CONSTANT_Methodref, CONSTANT_InterfaceMethodref):
            cls_name = get_class_name(entry[1])
            nt = cp[entry[2]]
            if nt and nt[0] == "NameAndType":
                return (cls_name, get_utf8(nt[1]), get_utf8(nt[2]))
        return None

    access_flags = r.u2()
    this_class = get_class_name(r.u2())
    super_class = get_class_name(r.u2())

    interfaces_count = r.u2()
    interfaces = [get_class_name(r.u2()) for _ in range(interfaces_count)]

    fields_count = r.u2()
    fields: list[MemberInfo] = []
    for _ in range(fields_count):
        f_flags = r.u2()
        f_name = get_utf8(r.u2())
        f_desc = get_utf8(r.u2())
        attr_count = r.u2()
        for _ in range(attr_count):
            _attr_name = r.u2()
            attr_len = r.u4()
            r.read_bytes(attr_len)
        fields.append(MemberInfo(access_flags=f_flags, name=f_name, descriptor=f_desc))

    methods_count = r.u2()
    methods: list[MemberInfo] = []
    for _ in range(methods_count):
        m_flags = r.u2()
        m_name = get_utf8(r.u2())
        m_desc = get_utf8(r.u2())
        attr_count = r.u2()
        m_info = MemberInfo(access_flags=m_flags, name=m_name, descriptor=m_desc)
        for _ in range(attr_count):
            attr_name = get_utf8(r.u2())
            attr_len = r.u4()
            attr_bytes = r.read_bytes(attr_len)
            if attr_name == "Code":
                cr = ClassReader(attr_bytes)
                m_info.max_stack = cr.u2()
                m_info.max_locals = cr.u2()
                m_info.code_size = cr.u4()
                code_bytes = cr.read_bytes(m_info.code_size)
                scan_bytecode_refs(code_bytes, cp, get_utf8, resolve_ref, m_info)
        methods.append(m_info)

    constant_strings: list[str] = []
    ext_methods: list[tuple[str, str, str]] = []
    ext_fields: list[tuple[str, str, str]] = []

    for idx in range(1, cp_count):
        entry = cp[idx]
        if not entry:
            continue
        if entry[0] == "String":
            constant_strings.append(get_utf8(entry[1]))
        elif entry[0] in (CONSTANT_Methodref, CONSTANT_InterfaceMethodref):
            ref = resolve_ref(idx)
            if ref:
                ext_methods.append(ref)
        elif entry[0] == CONSTANT_Fieldref:
            ref = resolve_ref(idx)
            if ref:
                ext_fields.append(ref)

    return ClassFileInfo(
        name=this_class,
        super_name=super_class,
        access_flags=access_flags,
        interfaces=interfaces,
        fields=fields,
        methods=methods,
        constant_strings=constant_strings,
        external_method_refs=ext_methods,
        external_field_refs=ext_fields,
    )


def scan_bytecode_refs(
    code: bytes,
    cp: list[Any],
    get_utf8: Any,
    resolve_ref: Any,
    m_info: MemberInfo,
) -> None:
    """Scan JVM bytecode instructions for method calls, field accesses, and ldc strings."""
    # Fixed instruction lengths (excluding opcode) for opcodes that need operand skipping
    FIXED_LEN = {
        0x10: 1,  # bipush
        0x11: 2,  # sipush
        0x12: 1,  # ldc
        0x13: 2,  # ldc_w
        0x14: 2,  # ldc2_w
        0x15: 1, 0x16: 1, 0x17: 1, 0x18: 1, 0x19: 1,  # *load
        0x36: 1, 0x37: 1, 0x38: 1, 0x39: 1, 0x3A: 1,  # *store
        0x84: 2,  # iinc
        0x99: 2, 0x9A: 2, 0x9B: 2, 0x9C: 2, 0x9D: 2, 0x9E: 2,  # if*
        0x9F: 2, 0xA0: 2, 0xA1: 2, 0xA2: 2, 0xA3: 2, 0xA4: 2, 0xA5: 2, 0xA6: 2,  # if_icmp*, if_acmp*
        0xA7: 2, 0xA8: 2,  # goto, jsr
        0xA9: 1,  # ret
        0xB2: 2, 0xB3: 2, 0xB4: 2, 0xB5: 2,  # get/put static/field
        0xB6: 2, 0xB7: 2, 0xB8: 2,  # invokevirtual, invokespecial, invokestatic
        0xB9: 4,  # invokeinterface
        0xBB: 2,  # new
        0xBC: 1,  # newarray
        0xBD: 2,  # anewarray
        0xC0: 2, 0xC1: 2,  # checkcast, instanceof
        0xC5: 3,  # multianewarray
        0xC6: 2, 0xC7: 2,  # ifnull, ifnonnull
        0xC8: 4, 0xC9: 4,  # goto_w, jsr_w
    }

    pc = 0
    n = len(code)
    while pc < n:
        op = code[pc]
        if op == 0xAA:  # tableswitch
            pad = (4 - ((pc + 1) % 4)) % 4
            base = pc + 1 + pad
            if base + 12 > n:
                break
            _default, low, high = struct.unpack_from(">iii", code, base)
            pc = base + 12 + (high - low + 1) * 4
            continue
        if op == 0xAB:  # lookupswitch
            pad = (4 - ((pc + 1) % 4)) % 4
            base = pc + 1 + pad
            if base + 8 > n:
                break
            _default, npairs = struct.unpack_from(">ii", code, base)
            pc = base + 8 + npairs * 8
            continue
        if op == 0xC4:  # wide
            if pc + 1 >= n:
                break
            sub_op = code[pc + 1]
            pc += 6 if sub_op == 0x84 else 4
            continue

        if op in (0x12, 0x13):  # ldc, ldc_w
            idx = code[pc + 1] if op == 0x12 else struct.unpack_from(">H", code, pc + 1)[0]
            if 0 < idx < len(cp) and cp[idx] and cp[idx][0] == "String":
                m_info.string_literals.append(get_utf8(cp[idx][1]))
        elif op in (0xB2, 0xB3, 0xB4, 0xB5):  # field access
            idx = struct.unpack_from(">H", code, pc + 1)[0]
            ref = resolve_ref(idx)
            if ref:
                m_info.accessed_fields.append(ref)
        elif op in (0xB6, 0xB7, 0xB8, 0xB9):  # method invocation
            idx = struct.unpack_from(">H", code, pc + 1)[0]
            ref = resolve_ref(idx)
            if ref:
                m_info.called_methods.append(ref)

        pc += 1 + FIXED_LEN.get(op, 0)


def scan_scratchpad(sp_path: Path) -> dict[str, Any]:
    data = sp_path.read_bytes()
    sha256 = hashlib.sha256(data).hexdigest()

    # Search for embedded MFi/MLD melodies ('melo' + 4-byte length) and GIF/PNG headers
    mld_hits: list[tuple[int, int]] = []
    gif_hits: list[int] = []

    pos = 0
    while True:
        idx = data.find(b"melo", pos)
        if idx == -1:
            break
        if idx + 8 <= len(data):
            mld_len = struct.unpack_from(">I", data, idx + 4)[0]
            if 16 <= mld_len <= len(data) - idx:
                mld_hits.append((idx, mld_len + 8))
        pos = idx + 4

    pos = 0
    while True:
        idx = data.find(b"GIF8", pos)
        if idx == -1:
            break
        if idx + 6 <= len(data) and data[idx : idx + 6] in (b"GIF87a", b"GIF89a"):
            gif_hits.append(idx)
        pos = idx + 4

    return {
        "size": len(data),
        "sha256": sha256,
        "mld_count": len(mld_hits),
        "mld_samples": mld_hits[:10],
        "gif_count": len(gif_hits),
        "gif_samples": gif_hits[:10],
    }


def parse_jam_file(jam_path: Path) -> dict[str, str]:
    raw = jam_path.read_bytes()
    for enc in ("cp932", "utf-8", "shift_jis"):
        try:
            text = raw.decode(enc)
            break
        except UnicodeDecodeError:
            continue
    else:
        text = raw.decode("utf-8", errors="replace")

    props: dict[str, str] = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            props[k.strip()] = v.strip()
    return props


def survey_game(
    jar_path: Path,
    sp_path: Path | None,
    jam_path: Path | None,
    report_out: Path,
    symbols_out: Path,
) -> None:
    jar_bytes = jar_path.read_bytes()
    jar_sha256 = hashlib.sha256(jar_bytes).hexdigest()

    classes: dict[str, ClassFileInfo] = {}
    other_files: list[tuple[str, int]] = []

    with zipfile.ZipFile(jar_path, "r") as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            raw = zf.read(info.filename)
            if info.filename.endswith(".class"):
                cls_info = parse_class_bytes(raw)
                classes[cls_info.name] = cls_info
            else:
                other_files.append((info.filename, len(raw)))

    internal_names = set(classes.keys())
    doja_api_calls: Counter[str] = Counter()
    cldc_api_calls: Counter[str] = Counter()
    loop_candidates: list[tuple[str, str, str, list[str]]] = []

    for cls in classes.values():
        for m in cls.methods:
            reasons: list[str] = []
            for target_cls, target_m, target_desc in m.called_methods:
                sig = f"{target_cls}.{target_m}{target_desc}"
                if target_cls not in internal_names:
                    if target_cls.startswith("com/nttdocomo/"):
                        doja_api_calls[sig] += 1
                    else:
                        cldc_api_calls[sig] += 1
                if target_m in ("sleep", "currentTimeMillis", "paint", "repaint", "lock", "unlock"):
                    reasons.append(f"{target_cls}.{target_m}")
            if reasons:
                loop_candidates.append((cls.name, m.name, m.descriptor, sorted(set(reasons))))

    # Write Markdown survey report
    lines: list[str] = [
        f"# DoJa Bytecode Survey Report: `{jar_path.name}`",
        "",
        f"- **JAR File:** `{jar_path.name}` ({len(jar_bytes):,} bytes, SHA-256: `{jar_sha256[:16]}...`)",
        f"- **Total Classes:** {len(classes)}",
        f"- **Non-Class Bundled Resources:** {len(other_files)}",
    ]

    if jam_path and jam_path.exists():
        jam_props = parse_jam_file(jam_path)
        lines.extend(["", "## 1. JAM Descriptor Properties", ""])
        for k, v in jam_props.items():
            lines.append(f"- `{k}` = `{v}`")

    if sp_path and sp_path.exists():
        sp_info = scan_scratchpad(sp_path)
        lines.extend(
            [
                "",
                "## 2. ScratchPad (`.sp`) Analysis",
                "",
                f"- **Size:** {sp_info['size']:,} bytes (SHA-256: `{sp_info['sha256'][:16]}...`)",
                f"- **Embedded MFi/MLD (`melo`) Audio Tracks Found:** {sp_info['mld_count']}",
                f"- **Embedded GIF Images Found:** {sp_info['gif_count']}",
            ]
        )

    lines.extend(["", "## 3. Class Hierarchy & Entry Points", ""])
    for name, cls in sorted(classes.items()):
        total_code = sum(m.code_size for m in cls.methods)
        ifaces = f" implements {', '.join(cls.interfaces)}" if cls.interfaces else ""
        lines.append(
            f"- **`{name}`** extends `{cls.super_name}`{ifaces} — "
            f"{len(cls.fields)} fields, {len(cls.methods)} methods, {total_code:,} bytes of bytecode"
        )

    lines.extend(["", "## 4. Main Loop & Frame Timing Candidates", ""])
    for cls_name, m_name, m_desc, reasons in loop_candidates:
        lines.append(f"- `{cls_name}.{m_name}{m_desc}` -> calls `{', '.join(reasons)}`")

    lines.extend(["", "## 5. DoJa API Surface Area (`com.nttdocomo.*`)", ""])
    for sig, count in doja_api_calls.most_common():
        lines.append(f"- `{sig}` ({count} call sites)")

    lines.extend(["", "## 6. CLDC / Standard Java API Surface Area", ""])
    for sig, count in cldc_api_calls.most_common(40):
        lines.append(f"- `{sig}` ({count} call sites)")

    report_out.parent.mkdir(parents=True, exist_ok=True)
    report_out.write_text("\n".join(lines) + "\n", encoding="utf-8")

    # Emit initial symbols.toml
    toml_lines: list[str] = [
        "# Auto-generated Symbol Map for rmexe-mobile-recomp",
        f'jar_name = "{jar_path.name}"',
        f'jar_sha256 = "{jar_sha256}"',
        "",
    ]
    for name, cls in sorted(classes.items()):
        role = f"Class_{name.replace('/', '_')}"
        if "IApplication" in cls.super_name:
            role = "AppMain"
        elif "Canvas" in cls.super_name:
            role = "GameCanvas"
        toml_lines.extend(
            [
                "[[classes]]",
                f'obfuscated = "{name}"',
                f'name = "{role}"',
                f'super = "{cls.super_name}"',
                "",
            ]
        )
    symbols_out.parent.mkdir(parents=True, exist_ok=True)
    symbols_out.write_text("\n".join(toml_lines) + "\n", encoding="utf-8")
    print(f"Survey complete: {report_out} and {symbols_out}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Survey DoJa .jar/.sp/.jam files")
    parser.add_argument("jar", type=Path, help="Path to PoN.jar or LoN.jar")
    parser.add_argument("--sp", type=Path, default=None, help="Path to .sp ScratchPad file")
    parser.add_argument("--jam", type=Path, default=None, help="Path to .jam descriptor file")
    parser.add_argument(
        "--report",
        type=Path,
        default=Path("out/survey_report.md"),
        help="Output Markdown report path",
    )
    parser.add_argument(
        "--symbols",
        type=Path,
        default=Path("out/symbols.toml"),
        help="Output TOML symbol map path",
    )
    args = parser.parse_args()

    sp_path = args.sp or args.jar.with_suffix(".sp")
    jam_path = args.jam or args.jar.with_suffix(".jam")
    survey_game(
        jar_path=args.jar,
        sp_path=sp_path if sp_path.exists() else None,
        jam_path=jam_path if jam_path.exists() else None,
        report_out=args.report,
        symbols_out=args.symbols,
    )


if __name__ == "__main__":
    main()
