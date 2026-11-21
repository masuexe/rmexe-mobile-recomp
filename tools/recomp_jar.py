#!/usr/bin/env python3
"""
Static Recompiler (JVM .class -> C++20) for rmexe-mobile-recomp.

Translates all classes in a DoJa .jar file (PoN.jar or LoN.jar) into a
standalone C++20 compilation unit that links against runtime/doja_runtime.cpp.
"""

from __future__ import annotations

import argparse
import re
import struct
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from survey_doja import (
    CONSTANT_Class,
    CONSTANT_Double,
    CONSTANT_Fieldref,
    CONSTANT_Float,
    CONSTANT_Integer,
    CONSTANT_InterfaceMethodref,
    CONSTANT_Long,
    CONSTANT_Methodref,
    CONSTANT_NameAndType,
    CONSTANT_String,
    CONSTANT_Utf8,
    ClassReader,
)


@dataclass
class ExceptionEntry:
    start_pc: int
    end_pc: int
    handler_pc: int
    catch_type: str


@dataclass
class MethodIR:
    access_flags: int
    name: str
    descriptor: str
    max_stack: int = 0
    max_locals: int = 0
    code: bytes = b""
    exceptions: list[ExceptionEntry] = field(default_factory=list)


@dataclass
class FieldIR:
    access_flags: int
    name: str
    descriptor: str
    const_value: Any = None


@dataclass
class ClassIR:
    class_id: int
    name: str
    super_name: str
    access_flags: int
    interfaces: list[str]
    fields: list[FieldIR]
    methods: list[MethodIR]
    cp: list[Any]


def parse_class_ir(data: bytes, class_id: int) -> ClassIR:
    r = ClassReader(data)
    magic = r.u4()
    if magic != 0xCAFEBABE:
        raise ValueError("Invalid class magic")
    _minor = r.u2()
    _major = r.u2()

    cp_count = r.u2()
    cp: list[Any] = [None] * cp_count
    i = 1
    while i < cp_count:
        tag = r.u1()
        if tag == CONSTANT_Utf8:
            cp[i] = ("Utf8", r.read_bytes(r.u2()).decode("utf-8", errors="replace"))
        elif tag == CONSTANT_Integer:
            val = struct.unpack(">i", r.read_bytes(4))[0]
            cp[i] = ("Integer", val)
        elif tag == CONSTANT_Float:
            val = struct.unpack(">f", r.read_bytes(4))[0]
            cp[i] = ("Float", val)
        elif tag == CONSTANT_Long:
            val = struct.unpack(">q", r.read_bytes(8))[0]
            cp[i] = ("Long", val)
            i += 1
        elif tag == CONSTANT_Double:
            val = struct.unpack(">d", r.read_bytes(8))[0]
            cp[i] = ("Double", val)
            i += 1
        elif tag == CONSTANT_Class:
            cp[i] = ("Class", r.u2())
        elif tag == CONSTANT_String:
            cp[i] = ("String", r.u2())
        elif tag in (CONSTANT_Fieldref, CONSTANT_Methodref, CONSTANT_InterfaceMethodref):
            cp[i] = ("Ref", r.u2(), r.u2())
        elif tag == CONSTANT_NameAndType:
            cp[i] = ("NameAndType", r.u2(), r.u2())
        else:
            raise ValueError(f"Unknown CP tag {tag}")
        i += 1

    def utf8(idx: int) -> str:
        return cp[idx][1] if idx and cp[idx] else ""

    def cls_name(idx: int) -> str:
        return utf8(cp[idx][1]) if idx and cp[idx] else ""

    access_flags = r.u2()
    this_name = cls_name(r.u2())
    super_name = cls_name(r.u2())
    interfaces = [cls_name(r.u2()) for _ in range(r.u2())]

    fields: list[FieldIR] = []
    for _ in range(r.u2()):
        f_flags = r.u2()
        f_name = utf8(r.u2())
        f_desc = utf8(r.u2())
        const_val = None
        for _ in range(r.u2()):
            a_name = utf8(r.u2())
            a_bytes = r.read_bytes(r.u4())
            if a_name == "ConstantValue" and len(a_bytes) == 2:
                c_idx = struct.unpack(">H", a_bytes)[0]
                if cp[c_idx]:
                    const_val = cp[c_idx][1]
        fields.append(FieldIR(f_flags, f_name, f_desc, const_val))

    methods: list[MethodIR] = []
    for _ in range(r.u2()):
        m_flags = r.u2()
        m_name = utf8(r.u2())
        m_desc = utf8(r.u2())
        m_ir = MethodIR(m_flags, m_name, m_desc)
        for _ in range(r.u2()):
            a_name = utf8(r.u2())
            a_bytes = r.read_bytes(r.u4())
            if a_name == "Code":
                cr = ClassReader(a_bytes)
                m_ir.max_stack = cr.u2()
                m_ir.max_locals = cr.u2()
                code_len = cr.u4()
                m_ir.code = cr.read_bytes(code_len)
                exc_len = cr.u2()
                for _ in range(exc_len):
                    s_pc = cr.u2()
                    e_pc = cr.u2()
                    h_pc = cr.u2()
                    c_type = cls_name(cr.u2())
                    m_ir.exceptions.append(ExceptionEntry(s_pc, e_pc, h_pc, c_type))
        methods.append(m_ir)

    return ClassIR(class_id, this_name, super_name, access_flags, interfaces, fields, methods, cp)


def mangle_ident(s: str) -> str:
    return (
        s.replace("/", "_")
        .replace("<init>", "_init_")
        .replace("<clinit>", "_clinit_")
        .replace("(", "__")
        .replace(")", "__")
        .replace("[", "A")
        .replace(";", "_")
        .replace("$", "_")
    )


def sanitize_ident(s: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_]", "_", s)
    if not cleaned or cleaned[0].isdigit():
        cleaned = "_" + cleaned
    return cleaned


def parse_method_descriptor(desc: str) -> tuple[list[str], str]:
    assert desc.startswith("(")
    idx = 1
    args: list[str] = []
    while desc[idx] != ")":
        start = idx
        while desc[idx] == "[":
            idx += 1
        if desc[idx] == "L":
            idx = desc.index(";", idx) + 1
        else:
            idx += 1
        args.append(desc[start:idx])
    ret = desc[idx + 1 :]
    return args, ret


def cpp_escape_string(s: str) -> str:
    out = ['"']
    for b in s.encode("utf-8"):
        if b == 0x22:
            out.append('\\"')
        elif b == 0x5C:
            out.append("\\\\")
        elif b == 0x0A:
            out.append("\\n")
        elif b == 0x0D:
            out.append("\\r")
        elif 32 <= b < 127 and chr(b) not in "0123456789abcdefABCDEF":
            out.append(chr(b))
        else:
            out.append(f'\\x{b:02x}""')
    out.append('"')
    return "".join(out)


def generate_cpp(jar_path: Path, out_cpp: Path, game_namespace: str = "") -> None:
    classes: dict[str, ClassIR] = {}
    with zipfile.ZipFile(jar_path, "r") as zf:
        cid = 1
        for name in sorted(zf.namelist()):
            if name.endswith(".class"):
                cls_ir = parse_class_ir(zf.read(name), cid)
                classes[cls_ir.name] = cls_ir
                cid += 1

    # Assign field indices (static vs instance, 32-bit vs 64-bit)
    static_fields: dict[tuple[str, str, str], str] = {}
    instance_i32_idx: dict[tuple[str, str, str], int] = {}
    instance_i64_idx: dict[tuple[str, str, str], int] = {}
    class_num_i32: dict[str, int] = {}
    class_num_i64: dict[str, int] = {}

    for cls_name, cls in classes.items():
        i32_c = 0
        i64_c = 0
        for f in cls.fields:
            key = (cls_name, f.name, f.descriptor)
            is_static = bool(f.access_flags & 0x0008)
            if is_static:
                static_fields[key] = f"sf_{mangle_ident(cls_name)}_{mangle_ident(f.name)}"
            else:
                if f.descriptor in ("J", "D"):
                    instance_i64_idx[key] = i64_c
                    i64_c += 1
                else:
                    instance_i32_idx[key] = i32_c
                    i32_c += 1
        class_num_i32[cls_name] = i32_c
        class_num_i64[cls_name] = i64_c

    defined_methods: dict[tuple[str, str, str], tuple[str, bool]] = {}
    for cls_name, cls in classes.items():
        for m in cls.methods:
            is_static = bool(m.access_flags & 0x0008)
            fn_name = f"fn_{mangle_ident(cls_name)}_{mangle_ident(m.name)}_{mangle_ident(m.descriptor)}"
            defined_methods[(cls_name, m.name, m.descriptor)] = (fn_name, is_static)

    ns_open = f"namespace rmexe::{game_namespace} {{" if game_namespace else "namespace rmexe {"
    ns_close = f"}} // namespace rmexe::{game_namespace}" if game_namespace else "} // namespace rmexe"
    lines: list[str] = [
        "// Auto-generated by tools/recomp_jar.py — DO NOT EDIT MANUALLY",
        '#include "doja_runtime.hpp"',
    ]
    if game_namespace:
        lines.extend(
            [
                '#include "pvp_common.hpp"',
                f"#define RMEXE_GAME_NAMESPACE {game_namespace}",
            ]
        )
    lines.extend(
        [
            "",
            ns_open,
            "",
            "// Static fields across all classes",
        ]
    )

    for (cls_name, f_name, f_desc), c_var in sorted(static_fields.items()):
        c_type = "int64_t" if f_desc in ("J", "D") else "int32_t"
        lines.append(f"static {c_type} {c_var} = 0;")
    if "sf_app_CpCanvas_bas_max_flg" in static_fields.values():
        lines.append("#define HAS_SF_app_CpCanvas_bas_max_flg 1")
    if "sf_app_CpCanvas_rank_flg" in static_fields.values():
        lines.append("#define HAS_SF_app_CpCanvas_rank_flg 1")
    if "sf_app_CpCanvas_bas_up" in static_fields.values():
        lines.append("#define HAS_SF_app_CpCanvas_bas_up 1")
    if "sf_app_CpCanvas_imgMap2" in static_fields.values():
        lines.append("#define HAS_SF_app_CpCanvas_imgMap2 1")
    if ("app/CpCanvas", "PalSetBG", "()V") in defined_methods:
        lines.append("#define HAS_FN_CpCanvas_PalSetBG 1")

    # Auto-export per-class metadata & instance field offsets as constexpr
    app_short_counts: dict[str, int] = {}
    short_counts: dict[str, int] = {}
    for cls_name in classes:
        s_cls = sanitize_ident(cls_name.split("/")[-1])
        short_counts[s_cls] = short_counts.get(s_cls, 0) + 1
        if cls_name.startswith("app/"):
            app_short_counts[s_cls] = app_short_counts.get(s_cls, 0) + 1
    assert all(cnt == 1 for cnt in app_short_counts.values()), (
        f"Non-unique short class names under app/: {app_short_counts}"
    )

    short_cls_map: dict[str, str] = {}
    used_short_cls: set[str] = set()
    for cls_name in sorted(classes.keys()):
        if cls_name.startswith("app/"):
            s_cls = sanitize_ident(cls_name.split("/")[-1])
            short_cls_map[cls_name] = s_cls
            used_short_cls.add(s_cls)
    for cls_name in sorted(classes.keys()):
        if cls_name in short_cls_map:
            continue
        s_cls = sanitize_ident(cls_name.split("/")[-1])
        if s_cls in used_short_cls or short_counts.get(s_cls, 0) > 1:
            s_cls = sanitize_ident(cls_name)
        while s_cls in used_short_cls:
            s_cls += "_dup"
        short_cls_map[cls_name] = s_cls
        used_short_cls.add(s_cls)

    lines.extend(["", "// Class metadata constants across all classes"])
    for cls_name in sorted(classes.keys()):
        cls = classes[cls_name]
        s_cls = short_cls_map[cls_name]
        lines.append(f"static constexpr int32_t kClassId_{s_cls} = {cls.class_id};")
        lines.append(f"static constexpr int32_t kNumI32_{s_cls} = {class_num_i32[cls_name]};")
        lines.append(f"static constexpr int32_t kNumI64_{s_cls} = {class_num_i64[cls_name]};")

    lines.extend(["", "// Instance field offset constants across all classes"])
    emitted_fld_consts: set[str] = set()

    def emit_field_consts(idx_map: dict[tuple[str, str, str], int], prefix: str) -> None:
        for (c_name, f_name, f_desc), f_idx in sorted(idx_map.items()):
            s_cls = short_cls_map[c_name]
            s_fld = sanitize_ident(f_name)
            const_name = f"{prefix}_{s_cls}_{s_fld}"
            if const_name in emitted_fld_consts:
                const_name = f"{const_name}_{sanitize_ident(f_desc)}"
            while const_name in emitted_fld_consts:
                const_name += "_dup"
            emitted_fld_consts.add(const_name)
            lines.append(f"static constexpr int32_t {const_name} = {f_idx};")

    emit_field_consts(instance_i32_idx, "kFld")
    emit_field_consts(instance_i64_idx, "kFld64")

    sorted_statics = [(f_desc, c_var) for (_, _, f_desc), c_var in sorted(static_fields.items())]
    lines.extend(
        [
            "",
            "static void generated_save_statics(std::vector<int64_t>& out) {",
            f"    out.resize({len(sorted_statics)});",
        ]
    )
    for idx, (_, c_var) in enumerate(sorted_statics):
        lines.append(f"    out[{idx}] = static_cast<int64_t>({c_var});")
    lines.extend(
        [
            "}",
            "",
            "static void generated_restore_statics(const std::vector<int64_t>& in) {",
            f"    if (in.size() < {len(sorted_statics)}) return;",
        ]
    )
    for idx, (f_desc, c_var) in enumerate(sorted_statics):
        c_type = "int64_t" if f_desc in ("J", "D") else "int32_t"
        lines.append(f"    {c_var} = static_cast<{c_type}>(in[{idx}]);")
    lines.append("}")

    is_pon_pvp = (
        "app/Rock" in classes
        and ("app/Ene", "Hit", "(III)Z") in defined_methods
        and ("app/Rock", "Hit", "(III)Z") in defined_methods
        and ("app/CpCanvas", "EnSet", "()V") in defined_methods
        and ("app/CpCanvas", "Load", "(I)V") in defined_methods
    )
    is_lon_pvp = (
        "app/Rock" in classes
        and ("app/Ene", "Hit", "(III)Z") in defined_methods
        and ("app/Rock", "Hit", "(III)Z") in defined_methods
        and ("app/CpCanvas", "EnSet", "()V") in defined_methods
        and ("app/CpCanvas", "Load", "(I)I") in defined_methods
        and ("app/Rock", "SetArm", "(I)V") in defined_methods
    )
    assert not (is_pon_pvp and is_lon_pvp), f"Ambiguous PvP target detected in {jar_path}"
    is_pvp = is_pon_pvp or is_lon_pvp
    pvp_prefix = "pvp_pon" if is_pon_pvp else "pvp_lon"
    pvp_inl = "pvp_pon_engine.inl" if is_pon_pvp else "pvp_lon_engine.inl"
    if is_pvp:
        game_macro = "#define RMEXE_IS_PON_GAME 1" if is_pon_pvp else "#define RMEXE_IS_LON_GAME 1"
        lines.extend(
            [
                "",
                game_macro,
                f"// Online PvP NetBattle hooks (implemented in {pvp_inl})",
                f"static bool {pvp_prefix}_tick_override(DojaRuntime& rt, Ref cp);",
                f"static int32_t {pvp_prefix}_intercept_ene_hit(DojaRuntime& rt, Ref ene_thiz, int32_t a0, int32_t a1, int32_t a2);",
                f"static bool {pvp_prefix}_intercept_ene_draw(DojaRuntime& rt, Ref ene_thiz, int32_t a0);",
                "static bool g_pvp_in_ene_hit_passthrough = false;",
                "static int32_t g_pvp_rock_flip_override = -1;",
                "",
                "// Assist / QoL hooks (implemented in assist_engine.inl)",
                "static void assist_on_frame_tick(DojaRuntime& rt);",
                "static bool assist_should_bypass_en_bas_max(DojaRuntime& rt);",
                "static void assist_adjust_buster_damage(DojaRuntime& rt, Ref attack_obj, int32_t attack_id, int32_t owner_side);",
                "static int32_t assist_scale_rock_damage(DojaRuntime& rt, int32_t dmg);",
                "static bool assist_eval_field_encounter(DojaRuntime& rt, int32_t raw_encount, int32_t move_cnt);",
                "static void assist_override_wild_boss_encounter(DojaRuntime& rt, int32_t& ghost_loc3, int32_t boss_roll_loc4);",
                "static int32_t assist_filter_ene_get(DojaRuntime& rt, int32_t mode, int32_t raw_val);",
                "static int32_t assist_scale_zenny_getitem(DojaRuntime& rt, int32_t item_type, int32_t op_mode, int32_t amount);",
                "static int32_t assist_move_check_intercept(DojaRuntime& rt, Ref cp, int32_t a0, int32_t a1);",
                "static void assist_on_battle_end_hp(DojaRuntime& rt);",
                "static bool assist_should_always_escape(DojaRuntime& rt);",
                "static int32_t assist_custom_gauge_step(DojaRuntime& rt, int32_t base_sp);",
                "static bool assist_should_always_drop_chip(DojaRuntime& rt);",
                "static bool assist_should_force_s_rank(DojaRuntime& rt);",
                "static bool assist_should_block_ranking_record(DojaRuntime& rt);",
                "static bool assist_should_block_ranking_upload(DojaRuntime& rt);",
                "static void assist_on_battle_tip_touch(DojaRuntime& rt);",
                "static void assist_run_self_test(DojaRuntime& rt, Ref cp);",
            ]
        )

    lines.extend(["", "// Forward declarations of recompiled methods"])
    for (cls_name, m_name, m_desc), (fn_name, is_static) in sorted(defined_methods.items()):
        arg_types, ret_type = parse_method_descriptor(m_desc)
        c_ret = "void" if ret_type == "V" else ("int64_t" if ret_type == "J" else "int32_t")
        c_params = ["DojaRuntime& rt"]
        if not is_static:
            c_params.append("Ref thiz")
        for idx, at in enumerate(arg_types):
            c_params.append(f"{'int64_t' if at == 'J' else 'int32_t'} a{idx}")
        lines.append(f"static {c_ret} {fn_name}({', '.join(c_params)});")

    lines.append("")

    matched_cp_exe = False
    matched_rock_draw = False
    matched_ene_hit = False
    matched_ene_draw = False
    assist_hooks = {
        "attack_set_buster": 0,
        "attack_pow_bas_max": 0,
        "rock_hit": 0,
        "field_enc_cmp": 0,
        "field_btset": 0,
        "move_chack": 0,
        "esc_ch": 0,
        "bas_lv": 0,
        "ene_get_drop": 0,
        "ene_get_ret": 0,
        "get_item": 0,
        "battle_cus_sp": 0,
        "battle_end_hp": 0,
        "rank_record": 0,
        "rank_up_entry": 0,
        "rank_up_call": 0,
    }

    # Emit each method implementation
    for cls_name, cls in classes.items():
        cp = cls.cp

        def utf8(idx: int) -> str:
            return cp[idx][1] if idx and cp[idx] else ""

        def resolve_ref(idx: int) -> tuple[str, str, str]:
            _, c_idx, nt_idx = cp[idx]
            target_cls = utf8(cp[c_idx][1])
            _, n_idx, d_idx = cp[nt_idx]
            return (target_cls, utf8(n_idx), utf8(d_idx))

        for m in cls.methods:
            fn_name, is_static = defined_methods[(cls_name, m.name, m.descriptor)]
            arg_types, ret_type = parse_method_descriptor(m.descriptor)
            c_ret = "void" if ret_type == "V" else ("int64_t" if ret_type == "J" else "int32_t")
            c_params = ["DojaRuntime& rt"]
            if not is_static:
                c_params.append("Ref thiz")
            for idx, at in enumerate(arg_types):
                c_params.append(f"{'int64_t' if at == 'J' else 'int32_t'} a{idx}")

            lines.append(f"static {c_ret} {fn_name}({', '.join(c_params)}) {{")
            max_locals = max(m.max_locals, len(arg_types) * 2 + 2)
            max_stack = max(m.max_stack * 2 + 8, 16)
            lines.append(f"    Slot loc[{max_locals}];")
            lines.append(f"    Slot stk[{max_stack}];")
            lines.append("    int sp = 0;")
            lines.append("    int pc = 0;")

            # Initialize locals from parameters
            loc_slot = 0
            if not is_static:
                lines.append("    loc[0].r = thiz;")
                loc_slot = 1
            for idx, at in enumerate(arg_types):
                if at == "J":
                    lines.append(f"    loc[{loc_slot}].j = a{idx};")
                    loc_slot += 2
                else:
                    lines.append(f"    loc[{loc_slot}].i = a{idx};")
                    loc_slot += 1

            lines.append("    auto push_i = [&](int32_t v) { stk[sp++].i = v; };")
            lines.append("    auto pop_i  = [&]() -> int32_t { return stk[--sp].i; };")
            lines.append("    auto push_j = [&](int64_t v) { stk[sp++].j = v; stk[sp++].j = 0; };")
            lines.append("    auto pop_j  = [&]() -> int64_t { sp -= 2; return stk[sp].j; };")

            if is_pvp and (cls_name, m.name, m.descriptor) == ("app/Ene", "Hit", "(III)Z"):
                matched_ene_hit = True
                lines.append("    if (!g_pvp_in_ene_hit_passthrough) {")
                lines.append(f"        int32_t pvp_hr = {pvp_prefix}_intercept_ene_hit(rt, thiz, a0, a1, a2);")
                lines.append("        if (pvp_hr >= 0) return pvp_hr;")
                lines.append("    }")
            if is_pvp and (cls_name, m.name, m.descriptor) == ("app/Ene", "Draw", "(I)V"):
                matched_ene_draw = True
                lines.append(f"    if ({pvp_prefix}_intercept_ene_draw(rt, thiz, a0)) return;")
            if is_pvp and (cls_name, m.name, m.descriptor) == ("app/Rock", "Hit", "(III)Z"):
                assist_hooks["rock_hit"] += 1
                lines.append("    loc[2].i = assist_scale_rock_damage(rt, loc[2].i);")
            if is_pvp and (cls_name, m.name, m.descriptor) == ("app/CpCanvas", "MoveChack", "(II)I"):
                assist_hooks["move_chack"] += 1
                lines.append("    { int32_t am = assist_move_check_intercept(rt, thiz, a0, a1); if (am >= 0) return am; }")
            if is_pvp and (cls_name, m.name, m.descriptor) == ("app/CpCanvas", "EscCh", "()Z"):
                assist_hooks["esc_ch"] += 1
                lines.append("    if (assist_should_always_escape(rt)) return 1;")
            if is_pvp and (cls_name, m.name, m.descriptor) == ("app/CpCanvas", "BasLv", "()I"):
                assist_hooks["bas_lv"] += 1
                lines.append("    if (assist_should_force_s_rank(rt)) return 11;")
            if is_pvp and (cls_name, m.name, m.descriptor) == ("app/CpCanvas", "GetItem", "(III)I"):
                assist_hooks["get_item"] += 1
                lines.append("    loc[3].i = assist_scale_zenny_getitem(rt, loc[1].i, loc[2].i, loc[3].i);")
            if is_pvp and (cls_name, m.name, m.descriptor) == ("app/CpCanvas", "RankUp", "()I"):
                assist_hooks["rank_up_entry"] += 1
                lines.append("    if (assist_should_block_ranking_upload(rt)) return -999;")
            if is_pvp and (cls_name, m.name, m.descriptor) == ("app/CpCanvas", "BattleTip", "()V"):
                lines.append("    assist_on_battle_tip_touch(rt);")

            is_cp_exe = is_pvp and (cls_name, m.name, m.descriptor) == ("app/CpCanvas", "exe", "()V")
            is_rock_draw = is_pvp and (cls_name, m.name, m.descriptor) == ("app/Rock", "Draw", "(I)V")
            is_attack_set = is_pvp and (cls_name, m.name, m.descriptor) == ("app/Attack", "Set", "(IIIII)I")
            is_pow_bas_max = is_pvp and cls_name == "app/Attack" and m.name == "powBasMax"
            is_field_main = is_pvp and (cls_name, m.name, m.descriptor) == ("app/CpCanvas", "FieldMain", "()V")
            is_ene_get = is_pvp and (cls_name, m.name, m.descriptor) == ("app/Ene", "Get", "(II)I")
            is_battle_main = is_pvp and (cls_name, m.name, m.descriptor) == ("app/CpCanvas", "BattleMain", "()V")
            is_battle_tip = is_pvp and (cls_name, m.name, m.descriptor) == ("app/CpCanvas", "BattleTip", "()V")
            is_ranking = is_pvp and (cls_name, m.name, m.descriptor) == ("app/CpCanvas", "Ranking", "()V")
            seen_exe_gfx_lock = False
            emitted_exe_join = False
            rock_flip_hook_count = 0
            last_push_static = None
            prev_static_1 = None
            prev_static_2 = None
            seen_skill_kouka_in_ene_get = False
            rank_dat54_cmp_count = 0

            has_exc = len(m.exceptions) > 0
            if has_exc:
                lines.append("    dispatch_handler:")
                lines.append("    try {")
                lines.append("        switch (pc) {")
                lines.append("            case 0: goto L_0;")
                for h_pc in sorted({ex.handler_pc for ex in m.exceptions if ex.handler_pc != 0}):
                    lines.append(f"            case {h_pc}: goto L_{h_pc};")
                lines.append("            default: break;")
                lines.append("        }")

            code = m.code
            pc = 0
            n = len(code)
            while pc < n:
                op = code[pc]
                if (
                    is_cp_exe
                    and seen_exe_gfx_lock
                    and not emitted_exe_join
                    and op == 0xB2
                    and resolve_ref(struct.unpack_from(">H", code, pc + 1)[0]) == ("app/CpCanvas", "play_flg", "I")
                ):
                    emitted_exe_join = True
                    lines.append("    pvp_exe_frame_join:;")
                lines.append(f"    L_{pc}: pc = {pc};")
                if op == 0x00:  # nop
                    pc += 1
                elif op == 0x01:  # aconst_null
                    lines.append("        push_i(0);")
                    pc += 1
                elif 0x02 <= op <= 0x08:  # iconst_m1 .. iconst_5
                    lines.append(f"        push_i({op - 3});")
                    pc += 1
                elif op in (0x09, 0x0A):  # lconst_0, lconst_1
                    lines.append(f"        push_j({op - 9}LL);")
                    pc += 1
                elif op == 0x10:  # bipush
                    val = struct.unpack_from(">b", code, pc + 1)[0]
                    lines.append(f"        push_i({val});")
                    pc += 2
                elif op == 0x11:  # sipush
                    val = struct.unpack_from(">h", code, pc + 1)[0]
                    lines.append(f"        push_i({val});")
                    pc += 3
                elif op in (0x12, 0x13):  # ldc, ldc_w
                    idx = code[pc + 1] if op == 0x12 else struct.unpack_from(">H", code, pc + 1)[0]
                    tag, cval = cp[idx][0], cp[idx][1]
                    if tag == "Integer":
                        lines.append(f"        push_i({cval});")
                    elif tag == "String":
                        s_lit = cpp_escape_string(utf8(cval))
                        lines.append(f"        push_i(rt.alloc_string({s_lit}));")
                    else:
                        lines.append("        push_i(0);")
                    pc += 2 if op == 0x12 else 3
                elif op == 0x14:  # ldc2_w
                    idx = struct.unpack_from(">H", code, pc + 1)[0]
                    cval = cp[idx][1]
                    lines.append(f"        push_j({cval}LL);")
                    pc += 3
                elif op in (0x15, 0x19):  # iload, aload
                    lines.append(f"        push_i(loc[{code[pc + 1]}].i);")
                    pc += 2
                elif op == 0x16:  # lload
                    lines.append(f"        push_j(loc[{code[pc + 1]}].j);")
                    pc += 2
                elif 0x1A <= op <= 0x1D:  # iload_0..3
                    lines.append(f"        push_i(loc[{op - 0x1A}].i);")
                    pc += 1
                elif 0x1E <= op <= 0x21:  # lload_0..3
                    lines.append(f"        push_j(loc[{op - 0x1E}].j);")
                    pc += 1
                elif 0x2A <= op <= 0x2D:  # aload_0..3
                    lines.append(f"        push_i(loc[{op - 0x2A}].i);")
                    pc += 1
                elif op in (0x2E, 0x32):  # iaload, aaload
                    lines.append("        { int32_t idx = pop_i(); Ref arr = pop_i(); push_i(rt.iaload(arr, idx)); }")
                    pc += 1
                elif op == 0x33:  # baload
                    lines.append("        { int32_t idx = pop_i(); Ref arr = pop_i(); push_i(rt.baload(arr, idx)); }")
                    pc += 1
                elif op in (0x36, 0x3A):  # istore, astore
                    lines.append(f"        loc[{code[pc + 1]}].i = pop_i();")
                    pc += 2
                elif op == 0x37:  # lstore
                    lines.append(f"        loc[{code[pc + 1]}].j = pop_j();")
                    pc += 2
                elif 0x3B <= op <= 0x3E:  # istore_0..3
                    lines.append(f"        loc[{op - 0x3B}].i = pop_i();")
                    pc += 1
                elif 0x3F <= op <= 0x42:  # lstore_0..3
                    lines.append(f"        loc[{op - 0x3F}].j = pop_j();")
                    pc += 1
                elif 0x4B <= op <= 0x4E:  # astore_0..3
                    lines.append(f"        loc[{op - 0x4B}].i = pop_i();")
                    pc += 1
                elif op in (0x4F, 0x53):  # iastore, aastore
                    lines.append("        { int32_t v = pop_i(); int32_t idx = pop_i(); Ref arr = pop_i(); rt.iaload(arr, idx) = v; }")
                    pc += 1
                elif op == 0x54:  # bastore
                    lines.append("        { int32_t v = pop_i(); int32_t idx = pop_i(); Ref arr = pop_i(); rt.bastore(arr, idx, v); }")
                    pc += 1
                elif op == 0x57:  # pop
                    lines.append("        --sp;")
                    pc += 1
                elif op == 0x58:  # pop2
                    lines.append("        sp -= 2;")
                    pc += 1
                elif op == 0x59:  # dup
                    lines.append("        stk[sp] = stk[sp - 1]; ++sp;")
                    pc += 1
                elif op == 0x5A:  # dup_x1
                    lines.append("        { Slot v1 = stk[--sp]; Slot v2 = stk[--sp]; stk[sp++] = v1; stk[sp++] = v2; stk[sp++] = v1; }")
                    pc += 1
                elif op == 0x5B:  # dup_x2
                    lines.append("        { Slot v1 = stk[--sp]; Slot v2 = stk[--sp]; Slot v3 = stk[--sp]; stk[sp++] = v1; stk[sp++] = v3; stk[sp++] = v2; stk[sp++] = v1; }")
                    pc += 1
                elif op == 0x5C:  # dup2
                    lines.append("        { Slot v1 = stk[sp - 1]; Slot v2 = stk[sp - 2]; stk[sp++] = v2; stk[sp++] = v1; }")
                    pc += 1
                elif op == 0x60:  # iadd
                    lines.append("        { int32_t b = pop_i(); int32_t a = pop_i(); push_i(a + b); }")
                    pc += 1
                elif op == 0x64:  # isub
                    lines.append("        { int32_t b = pop_i(); int32_t a = pop_i(); push_i(a - b); }")
                    pc += 1
                elif op == 0x65:  # lsub
                    lines.append("        { int64_t b = pop_j(); int64_t a = pop_j(); push_j(a - b); }")
                    pc += 1
                elif op == 0x68:  # imul
                    lines.append("        { int32_t b = pop_i(); int32_t a = pop_i(); push_i(a * b); }")
                    pc += 1
                elif op == 0x6C:  # idiv
                    lines.append("        { int32_t b = pop_i(); int32_t a = pop_i(); if (b == 0) throw JavaException(1); push_i(a / b); }")
                    pc += 1
                elif op == 0x6D:  # ldiv
                    lines.append("        { int64_t b = pop_j(); int64_t a = pop_j(); if (b == 0) throw JavaException(1); push_j(a / b); }")
                    pc += 1
                elif op == 0x70:  # irem
                    lines.append("        { int32_t b = pop_i(); int32_t a = pop_i(); if (b == 0) throw JavaException(1); push_i(a % b); }")
                    pc += 1
                elif op == 0x74:  # ineg
                    lines.append("        push_i(-pop_i());")
                    pc += 1
                elif op == 0x78:  # ishl
                    lines.append("        { int32_t b = pop_i(); int32_t a = pop_i(); push_i(a << (b & 31)); }")
                    pc += 1
                elif op == 0x7A:  # ishr
                    lines.append("        { int32_t b = pop_i(); int32_t a = pop_i(); push_i(a >> (b & 31)); }")
                    pc += 1
                elif op == 0x7C:  # iushr
                    lines.append("        { int32_t b = pop_i(); uint32_t a = static_cast<uint32_t>(pop_i()); push_i(static_cast<int32_t>(a >> (b & 31))); }")
                    pc += 1
                elif op == 0x7E:  # iand
                    lines.append("        { int32_t b = pop_i(); int32_t a = pop_i(); push_i(a & b); }")
                    pc += 1
                elif op == 0x80:  # ior
                    lines.append("        { int32_t b = pop_i(); int32_t a = pop_i(); push_i(a | b); }")
                    pc += 1
                elif op == 0x82:  # ixor
                    lines.append("        { int32_t b = pop_i(); int32_t a = pop_i(); push_i(a ^ b); }")
                    pc += 1
                elif op == 0x84:  # iinc
                    v_idx = code[pc + 1]
                    inc = struct.unpack_from(">b", code, pc + 2)[0]
                    lines.append(f"        loc[{v_idx}].i += {inc};")
                    pc += 3
                elif op == 0x85:  # i2l
                    lines.append("        push_j(static_cast<int64_t>(pop_i()));")
                    pc += 1
                elif op == 0x88:  # l2i
                    lines.append("        push_i(static_cast<int32_t>(pop_j()));")
                    pc += 1
                elif op == 0x91:  # i2b
                    lines.append("        push_i(static_cast<int8_t>(pop_i()));")
                    pc += 1
                elif op == 0x94:  # lcmp
                    lines.append("        { int64_t b = pop_j(); int64_t a = pop_j(); push_i(a > b ? 1 : (a == b ? 0 : -1)); }")
                    pc += 1
                elif 0x99 <= op <= 0x9E:  # ifeq .. ifle
                    offset = struct.unpack_from(">h", code, pc + 1)[0]
                    target = pc + offset
                    cmp_op = {0x99: "==", 0x9A: "!=", 0x9B: "<", 0x9C: ">=", 0x9D: ">", 0x9E: "<="}[op]
                    if is_ene_get and op == 0x99 and seen_skill_kouka_in_ene_get and assist_hooks["ene_get_drop"] == 0:
                        assist_hooks["ene_get_drop"] += 1
                        lines.append(f"        {{ int32_t sk = pop_i(); if (sk == 0 && !assist_should_always_drop_chip(rt)) goto L_{target}; }}")
                    else:
                        lines.append(f"        if (pop_i() {cmp_op} 0) goto L_{target};")
                    pc += 3
                elif 0x9F <= op <= 0xA6:  # if_icmp* / if_acmp*
                    offset = struct.unpack_from(">h", code, pc + 1)[0]
                    target = pc + offset
                    cmp_op = {
                        0x9F: "==", 0xA0: "!=", 0xA1: "<", 0xA2: ">=",
                        0xA3: ">", 0xA4: "<=", 0xA5: "==", 0xA6: "!=",
                    }[op]
                    if (
                        is_field_main
                        and op == 0xA2
                        and prev_static_2 == ("app/CpCanvas", "encount", "I")
                        and prev_static_1 == ("app/CpCanvas", "move_cnt", "I")
                    ):
                        assist_hooks["field_enc_cmp"] += 1
                        lines.append(
                            f"        {{ int32_t b = pop_i(); int32_t a = pop_i(); if (!assist_eval_field_encounter(rt, a, b)) goto L_{target}; }}"
                        )
                    elif (
                        is_ranking
                        and op == 0xA2
                        and prev_static_1 == ("app/CpCanvas", "score", "I")
                    ):
                        rank_dat54_cmp_count += 1
                        if rank_dat54_cmp_count == 2:
                            assist_hooks["rank_record"] += 1
                            lines.append(
                                f"        {{ int32_t b = pop_i(); int32_t a = pop_i(); if (a >= b || assist_should_block_ranking_record(rt)) goto L_{target}; }}"
                            )
                        else:
                            lines.append(f"        {{ int32_t b = pop_i(); int32_t a = pop_i(); if (a {cmp_op} b) goto L_{target}; }}")
                    else:
                        lines.append(f"        {{ int32_t b = pop_i(); int32_t a = pop_i(); if (a {cmp_op} b) goto L_{target}; }}")
                    pc += 3
                elif op == 0xA7:  # goto
                    offset = struct.unpack_from(">h", code, pc + 1)[0]
                    target = pc + offset
                    lines.append(f"        goto L_{target};")
                    pc += 3
                elif op in (0xAC, 0xB0):  # ireturn, areturn
                    if is_ene_get and op == 0xAC:
                        assist_hooks["ene_get_ret"] += 1
                        lines.append("        return assist_filter_ene_get(rt, loc[1].i, pop_i());")
                    else:
                        lines.append("        return pop_i();")
                    pc += 1
                elif op == 0xB1:  # return
                    lines.append("        return;")
                    pc += 1
                elif op == 0xB2:  # getstatic
                    ref = resolve_ref(struct.unpack_from(">H", code, pc + 1)[0])
                    if is_pow_bas_max and ref == ("app/CpCanvas", "bas_max_flg", "I"):
                        assist_hooks["attack_pow_bas_max"] += 1
                        lines.append("        if (assist_should_bypass_en_bas_max(rt)) return loc[1].i;")
                    if is_ene_get and ref == ("app/CpCanvas", "skill_kouka", "[I"):
                        seen_skill_kouka_in_ene_get = True
                    if ref in static_fields:
                        c_var = static_fields[ref]
                        if ref[2] in ("J", "D"):
                            lines.append(f"        push_j({c_var});")
                        else:
                            if is_battle_main and ref == ("app/CpCanvas", "cus_sp", "I"):
                                assist_hooks["battle_cus_sp"] += 1
                                lines.append(f"        push_i(assist_custom_gauge_step(rt, {c_var}));")
                            else:
                                lines.append(f"        push_i({c_var});")
                    else:
                        lines.append("        push_i(1);")  # System.out
                    prev_static_2, prev_static_1 = prev_static_1, ref
                    last_push_static = ref
                    pc += 3
                elif op == 0xB3:  # putstatic
                    ref = resolve_ref(struct.unpack_from(">H", code, pc + 1)[0])
                    c_var = static_fields[ref]
                    if ref[2] in ("J", "D"):
                        lines.append(f"        {c_var} = pop_j();")
                    else:
                        lines.append(f"        {c_var} = pop_i();")
                    if (
                        (is_battle_main or is_battle_tip)
                        and ref == ("app/CpCanvas", "now_hp", "I")
                        and last_push_static == ("app/Rock", "hp", "I")
                    ):
                        assist_hooks["battle_end_hp"] += 1
                        lines.append("        assist_on_battle_end_hp(rt);")
                    pc += 3
                elif op == 0xB4:  # getfield
                    ref = resolve_ref(struct.unpack_from(">H", code, pc + 1)[0])
                    if ref in instance_i64_idx:
                        fidx = instance_i64_idx[ref]
                        lines.append(f"        {{ Ref obj = pop_i(); push_j(rt.field_i64(obj, {fidx})); }}")
                    else:
                        fidx = instance_i32_idx[ref]
                        lines.append(f"        {{ Ref obj = pop_i(); push_i(rt.field_i32(obj, {fidx})); }}")
                    pc += 3
                elif op == 0xB5:  # putfield
                    ref = resolve_ref(struct.unpack_from(">H", code, pc + 1)[0])
                    if ref in instance_i64_idx:
                        fidx = instance_i64_idx[ref]
                        lines.append(f"        {{ int64_t v = pop_j(); Ref obj = pop_i(); rt.field_i64(obj, {fidx}) = v; }}")
                    else:
                        fidx = instance_i32_idx[ref]
                        lines.append(f"        {{ int32_t v = pop_i(); Ref obj = pop_i(); rt.field_i32(obj, {fidx}) = v; }}")
                    pc += 3
                elif op in (0xB6, 0xB7, 0xB8, 0xB9):  # invoke*
                    idx = struct.unpack_from(">H", code, pc + 1)[0]
                    ref = resolve_ref(idx)
                    t_cls, t_name, t_desc = ref
                    m_args, m_ret = parse_method_descriptor(t_desc)
                    if ref in defined_methods:
                        callee_fn, callee_static = defined_methods[ref]
                        if is_field_main and ref == ("app/CpCanvas", "BtSet", "(I)V"):
                            assist_hooks["field_btset"] += 1
                            lines.append("        assist_override_wild_boss_encounter(rt, loc[3].i, loc[4].i);")
                            lines.append("        stk[sp - 1].i = sf_app_CpCanvas_teki_pt;")
                        lines.append("        {")
                        for a_i in range(len(m_args) - 1, -1, -1):
                            if m_args[a_i] == "J":
                                lines.append(f"            int64_t call_a{a_i} = pop_j();")
                            else:
                                lines.append(f"            int32_t call_a{a_i} = pop_i();")
                        if is_rock_draw:
                            if is_pon_pvp and ref == ("app/CpCanvas", "Ani", "(IIIIIIII)I"):
                                rock_flip_hook_count += 1
                                lines.append("            if (g_pvp_rock_flip_override >= 0) call_a7 = g_pvp_rock_flip_override;")
                            elif is_lon_pvp and ref == ("app/CpCanvas", "DrawAni", "(IIIIII)I"):
                                rock_flip_hook_count += 1
                                lines.append("            if (g_pvp_rock_flip_override >= 0) call_a4 = g_pvp_rock_flip_override;")
                        call_args = ["rt"]
                        if not callee_static:
                            lines.append("            Ref call_thiz = pop_i();")
                            call_args.append("call_thiz")
                        if is_attack_set and ref == ("app/Attack", "WazaSet", "()V"):
                            assist_hooks["attack_set_buster"] += 1
                            lines.append("            assist_adjust_buster_damage(rt, call_thiz, loc[1].i, loc[5].i);")
                        for a_i in range(len(m_args)):
                            call_args.append(f"call_a{a_i}")
                        call_expr = f"{callee_fn}({', '.join(call_args)})"
                        if m_ret == "V":
                            lines.append(f"            {call_expr};")
                        elif m_ret == "J":
                            lines.append(f"            push_j({call_expr});")
                        else:
                            lines.append(f"            push_i({call_expr});")
                            if is_ranking and ref == ("app/CpCanvas", "RankUp", "()I"):
                                assist_hooks["rank_up_call"] += 1
                                lines.append("            if (stk[sp - 1].i == -999) { --sp; sf_app_CpCanvas_rank_no = 0; sf_app_CpCanvas_mes_flg = 0; sf_app_CpCanvas_yes_no_flg = 0; sf_app_CpCanvas_key = 0; return; }")
                        lines.append("        }")
                    else:
                        is_static_call = op == 0xB8
                        total_args = len(m_args) + (0 if is_static_call else 1)
                        lines.append("        {")
                        lines.append(f"            Slot ext_args[{max(total_args, 1)}];")
                        for a_i in range(len(m_args) - 1, -1, -1):
                            dst_idx = a_i + (0 if is_static_call else 1)
                            if m_args[a_i] == "J":
                                lines.append(f"            ext_args[{dst_idx}].j = pop_j();")
                            else:
                                lines.append(f"            ext_args[{dst_idx}].i = pop_i();")
                        if not is_static_call:
                            lines.append("            ext_args[0].r = pop_i();")
                        lines.append(
                            f'            Slot res = rt.invoke_external("{t_cls}", "{t_name}", "{t_desc}", ext_args, {total_args});'
                        )
                        if m_ret == "J":
                            lines.append("            push_j(res.j);")
                        elif m_ret != "V":
                            lines.append("            push_i(res.i);")
                        lines.append("        }")
                        if is_cp_exe and not seen_exe_gfx_lock and t_cls == "com/nttdocomo/ui/Graphics" and t_name == "lock":
                            seen_exe_gfx_lock = True
                            lines.append("        if (rt.assist_self_test()) assist_run_self_test(rt, loc[0].r);")
                            lines.append(f"        if ({pvp_prefix}_tick_override(rt, loc[0].r)) goto pvp_exe_frame_join;")
                            lines.append("        assist_on_frame_tick(rt);")
                    pc += 5 if op == 0xB9 else 3
                elif op == 0xBB:  # new
                    c_idx = struct.unpack_from(">H", code, pc + 1)[0]
                    t_cls = utf8(cp[c_idx][1])
                    if t_cls in classes:
                        cid = classes[t_cls].class_id
                        ni32 = class_num_i32[t_cls]
                        ni64 = class_num_i64[t_cls]
                        lines.append(f"        push_i(rt.alloc_object({cid}, {ni32}, {ni64}));")
                    else:
                        lines.append("        push_i(rt.alloc_object(0, 4, 0));")
                    pc += 3
                elif op == 0xBC:  # newarray
                    atype = code[pc + 1]
                    if atype in (4, 8):  # T_BOOLEAN, T_BYTE
                        lines.append("        push_i(rt.alloc_byte_array(pop_i()));")
                    else:
                        lines.append("        push_i(rt.alloc_int_array(pop_i()));")
                    pc += 2
                elif op == 0xBD:  # anewarray
                    lines.append("        push_i(rt.alloc_ref_array(pop_i()));")
                    pc += 3
                elif op == 0xBE:  # arraylength
                    lines.append("        push_i(rt.array_length(pop_i()));")
                    pc += 1
                elif op == 0xC0:  # checkcast
                    pc += 3
                elif op == 0xC4:  # wide
                    sub_op = code[pc + 1]
                    if sub_op == 0x84:
                        v_idx = struct.unpack_from(">H", code, pc + 2)[0]
                        inc = struct.unpack_from(">h", code, pc + 4)[0]
                        lines.append(f"        loc[{v_idx}].i += {inc};")
                        pc += 6
                    else:
                        raise ValueError(f"Unsupported wide sub-opcode 0x{sub_op:02x}")
                elif op == 0xC5:  # multianewarray
                    c_idx = struct.unpack_from(">H", code, pc + 1)[0]
                    t_arr = utf8(cp[c_idx][1])
                    dims = code[pc + 3]
                    if dims == 2:
                        if t_arr.endswith("B"):
                            lines.append(
                                "        { int32_t d2 = pop_i(); int32_t d1 = pop_i(); push_i(rt.alloc_multi_byte_array(d1, d2)); }"
                            )
                        else:
                            lines.append(
                                "        { int32_t d2 = pop_i(); int32_t d1 = pop_i(); push_i(rt.alloc_multi_int_array(d1, d2)); }"
                            )
                    elif dims == 3:
                        lines.append(
                            "        { int32_t d3 = pop_i(); int32_t d2 = pop_i(); int32_t d1 = pop_i(); push_i(rt.alloc_multi_int_array3(d1, d2, d3)); }"
                        )
                    else:
                        raise ValueError(f"Unsupported multianewarray dims={dims}")
                    pc += 4
                elif op == 0xC6:  # ifnull
                    offset = struct.unpack_from(">h", code, pc + 1)[0]
                    target = pc + offset
                    lines.append(f"        if (pop_i() == 0) goto L_{target};")
                    pc += 3
                elif op == 0xC7:  # ifnonnull
                    offset = struct.unpack_from(">h", code, pc + 1)[0]
                    target = pc + offset
                    lines.append(f"        if (pop_i() != 0) goto L_{target};")
                    pc += 3
                else:
                    raise ValueError(f"Unsupported opcode 0x{op:02x} in {cls_name}.{m.name}")

            if has_exc:
                lines.append("    } catch (const JavaException& ex) {")
                for ex in m.exceptions:
                    lines.append(
                        f"        if (pc >= {ex.start_pc} && pc < {ex.end_pc}) {{ "
                        f"sp = 0; stk[sp++].r = ex.exc_ref; pc = {ex.handler_pc}; goto dispatch_handler; }}"
                    )
                lines.append("        throw;")
                lines.append("    }")

            if is_cp_exe:
                assert seen_exe_gfx_lock and emitted_exe_join, (
                    f"CpCanvas.exe PvP hook mismatch in {jar_path}: "
                    f"seen_exe_gfx_lock={seen_exe_gfx_lock}, emitted_exe_join={emitted_exe_join}"
                )
                matched_cp_exe = True
            if is_rock_draw:
                assert rock_flip_hook_count > 0, (
                    f"Rock.Draw PvP flip hook mismatch in {jar_path}: no Ani/DrawAni call matched"
                )
                matched_rock_draw = True

            if ret_type != "V":
                lines.append("    return 0;")
            lines.append("}")
            lines.append("")

    if is_pvp:
        assert matched_cp_exe and matched_rock_draw and matched_ene_hit and matched_ene_draw, (
            f"Missing PvP hook method in {jar_path}: "
            f"cp_exe={matched_cp_exe}, rock_draw={matched_rock_draw}, "
            f"ene_hit={matched_ene_hit}, ene_draw={matched_ene_draw}"
        )
        assert assist_hooks["attack_set_buster"] >= 1, f"Assist hook attack_set_buster mismatch: {assist_hooks}"
        assert assist_hooks["rock_hit"] == 1, f"Assist hook rock_hit mismatch: {assist_hooks}"
        assert assist_hooks["field_enc_cmp"] == 1, f"Assist hook field_enc_cmp mismatch: {assist_hooks}"
        assert assist_hooks["field_btset"] == 1, f"Assist hook field_btset mismatch: {assist_hooks}"
        assert assist_hooks["move_chack"] == 1, f"Assist hook move_chack mismatch: {assist_hooks}"
        assert assist_hooks["esc_ch"] == 1, f"Assist hook esc_ch mismatch: {assist_hooks}"
        assert assist_hooks["bas_lv"] == 1, f"Assist hook bas_lv mismatch: {assist_hooks}"
        assert assist_hooks["ene_get_drop"] == 1, f"Assist hook ene_get_drop mismatch: {assist_hooks}"
        assert assist_hooks["ene_get_ret"] >= 1, f"Assist hook ene_get_ret mismatch: {assist_hooks}"
        assert assist_hooks["get_item"] == 1, f"Assist hook get_item mismatch: {assist_hooks}"
        assert assist_hooks["battle_cus_sp"] == 1, f"Assist hook battle_cus_sp mismatch: {assist_hooks}"
        assert assist_hooks["battle_end_hp"] == 4, f"Assist hook battle_end_hp mismatch: {assist_hooks}"
        if "sf_app_CpCanvas_bas_max_flg" in static_fields.values():
            assert assist_hooks["attack_pow_bas_max"] == 1, f"Assist hook attack_pow_bas_max mismatch: {assist_hooks}"
        if is_pon_pvp:
            assert assist_hooks["rank_record"] == 1, f"Assist hook rank_record mismatch: {assist_hooks}"
            assert assist_hooks["rank_up_entry"] == 1, f"Assist hook rank_up_entry mismatch: {assist_hooks}"
            assert assist_hooks["rank_up_call"] == 1, f"Assist hook rank_up_call mismatch: {assist_hooks}"
        lines.extend(
            [
                '#include "assist_engine.inl"',
                f'#include "{pvp_inl}"',
                "",
            ]
        )

    # Emit entry-point glue
    lines.extend(
        [
            "void generated_static_init(DojaRuntime& rt) {",
        ]
    )
    for cls_name in sorted(classes.keys()):
        clinit_key = (cls_name, "<clinit>", "()V")
        if clinit_key in defined_methods:
            fn_name, _ = defined_methods[clinit_key]
            lines.append(f"    {fn_name}(rt);")
    app_init_fn = defined_methods[("app/AppMain", "<init>", "()V")][0]
    app_start_fn = defined_methods[("app/AppMain", "start", "()V")][0]
    canvas_event_fn = defined_methods[("app/CpCanvas", "processEvent", "(II)V")][0]
    lines.extend(
        [
            "}",
            "",
            "Ref generated_create_app_main(DojaRuntime& rt) {",
            f"    Ref app = rt.alloc_object({classes['app/AppMain'].class_id}, {class_num_i32['app/AppMain']}, {class_num_i64['app/AppMain']});",
            f"    {app_init_fn}(rt, app);",
            "    return app;",
            "}",
            "",
            "void generated_start_app(DojaRuntime& rt, Ref app_ref) {",
            f"    {app_start_fn}(rt, app_ref);",
            "}",
            "",
            "void generated_process_event(DojaRuntime& rt, Ref canvas_ref, int32_t type, int32_t param) {",
            f"    {canvas_event_fn}(rt, canvas_ref, type, param);",
            "}",
        ]
    )
    if game_namespace:
        lines.extend(
            [
                "",
                "const GameModule& get_game_module() {",
                "    static const GameModule kMod{",
                "        generated_static_init,",
                "        generated_create_app_main,",
                "        generated_start_app,",
                "        generated_process_event",
                "    };",
                "    return kMod;",
                "}",
            ]
        )
    lines.extend(
        [
            "",
            ns_close,
        ]
    )

    out_cpp.parent.mkdir(parents=True, exist_ok=True)
    out_cpp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Generated {out_cpp} ({len(lines):,} lines of C++ from {jar_path})")


def main() -> None:
    parser = argparse.ArgumentParser(description="Recompile DoJa .jar into C++20 source")
    parser.add_argument("jar", type=Path, help="Input .jar file")
    parser.add_argument("-o", "--out", type=Path, required=True, help="Output .cpp file")
    parser.add_argument("--namespace", dest="game_namespace", default="", help="Optional nested namespace under rmexe:: for multi-game builds")
    args = parser.parse_args()
    generate_cpp(args.jar, args.out, game_namespace=args.game_namespace)


if __name__ == "__main__":
    main()
