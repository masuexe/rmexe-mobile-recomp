#!/usr/bin/env python3
"""
Unified build, contract, and test gate for rmexe-mobile-recomp.

Usage:
  python3 tools/gate.py               # Contract checks + CMake build + full PoN/LoN PvP suites
  python3 tools/gate.py --quick       # Contract checks + CMake build + headless --pvp-self-test
  python3 tools/gate.py --check-only  # Fast static & contract checks only
"""

from __future__ import annotations

import argparse
import os
import py_compile
import re
import subprocess
import sys
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT_DIR / "build"

TARGET_BINARIES = [
    "pon_jp_recomp",
    "pon_en_recomp",
    "lon_jp_recomp",
    "lon_en_recomp",
]


def run_step(title: str, cmd: list[str], cwd: Path = ROOT_DIR) -> None:
    print(f"\n==> [{title}] {' '.join(cmd)}")
    proc = subprocess.run(cmd, cwd=cwd)
    if proc.returncode != 0:
        raise RuntimeError(f"Step '{title}' failed with exit code {proc.returncode}")


def check_contracts() -> None:
    print("==> [Contract Checks] Checking docs, Python tools, and PvP safety invariants...")
    errors: list[str] = []

    for name in ("AGENTS.md", "CLAUDE.md", "LESSONS.md"):
        if not (ROOT_DIR / name).is_file():
            errors.append(f"Missing required governance file: {name}")

    claude_path = ROOT_DIR / "CLAUDE.md"
    if claude_path.is_file():
        claude_text = claude_path.read_text(encoding="utf-8")
        for ref in ("@AGENTS.md", "@LESSONS.md"):
            if ref not in claude_text:
                errors.append(f"CLAUDE.md must import {ref}")

    lessons_path = ROOT_DIR / "LESSONS.md"
    if lessons_path.is_file():
        first_line = lessons_path.read_text(encoding="utf-8").splitlines()[0] if lessons_path.stat().st_size > 0 else ""
        if not re.match(r"^<!-- reviewed-through: [0-9a-f]{7,} \(\d{4}-\d{2}-\d{2}\) -->$", first_line):
            errors.append(
                "LESSONS.md line 1 must match '<!-- reviewed-through: <hash> (YYYY-MM-DD) -->'"
            )

    # Regression guard from ad06ac4: ensure PvP exit resets core and LoN Ene.Draw gates on is_arena_ready()
    pvp_common = ROOT_DIR / "runtime" / "pvp_common.hpp"
    if pvp_common.is_file():
        text = pvp_common.read_text(encoding="utf-8")
        if "eng.core = CoreT{};" not in text:
            errors.append("runtime/pvp_common.hpp: reset_common_exit_fields must reset 'eng.core = CoreT{};'")

    pvp_lon = ROOT_DIR / "runtime" / "pvp_lon_engine.inl"
    if pvp_lon.is_file():
        text = pvp_lon.read_text(encoding="utf-8")
        m = re.search(r"pvp_lon_intercept_ene_draw\([^)]*\)\s*\{[^}]{0,300}", text, re.DOTALL)
        if not m or "is_arena_ready()" not in m.group(0):
            errors.append(
                "runtime/pvp_lon_engine.inl: pvp_lon_intercept_ene_draw must guard single-player with is_arena_ready()"
            )

    # Regression guard for MesRead 0x81 0x8F sentinel collision: forbid trail byte 0x81 in 2-byte localization encoding
    loc_hpp = ROOT_DIR / "common" / "localization.hpp"
    if loc_hpp.is_file():
        loc_text = loc_hpp.read_text(encoding="utf-8")
        if "tb1 != 0x81u" not in loc_text or "kNumTrails = 189" not in loc_text:
            errors.append(
                "common/localization.hpp: must forbid trail byte 0x81 (tb1 != 0x81u and kNumTrails = 189)"
            )

    # Regression guard for single-threaded busy-wait key loops (e.g., CpCanvas.Machi / Keiji / kidou)
    rt_hpp = ROOT_DIR / "runtime" / "doja_runtime.hpp"
    recomp_py = ROOT_DIR / "tools" / "recomp_jar.py"
    if rt_hpp.is_file() and recomp_py.is_file():
        if "void yield_spin_wait();" not in rt_hpp.read_text(encoding="utf-8"):
            errors.append("runtime/doja_runtime.hpp: must declare void yield_spin_wait();")
        if "rt.yield_spin_wait();" not in recomp_py.read_text(encoding="utf-8"):
            errors.append("tools/recomp_jar.py: must emit rt.yield_spin_wait(); for pure spin-wait loops")

    # Syntax check all Python scripts in tools/
    for py_file in sorted((ROOT_DIR / "tools").glob("*.py")):
        try:
            py_compile.compile(str(py_file), doraise=True)
        except py_compile.PyCompileError as exc:
            errors.append(f"Python syntax error in {py_file.name}: {exc}")

    if errors:
        for err in errors:
            print(f"ERROR: {err}", file=sys.stderr)
        raise RuntimeError(f"Contract checks failed with {len(errors)} error(s)")

    print("OK: All contract & syntax checks passed.")


def build_cmake(reconfigure: bool = False) -> None:
    if reconfigure or not (BUILD_DIR / "CMakeCache.txt").is_file():
        run_step("CMake Configure", ["cmake", "-B", str(BUILD_DIR)])
    jobs = str(os.cpu_count() or 4)
    run_step("CMake Build", ["cmake", "--build", str(BUILD_DIR), f"-j{jobs}"])


def run_quick_self_tests() -> None:
    ran = 0
    for name in TARGET_BINARIES:
        bin_path = BUILD_DIR / name
        if bin_path.is_file():
            run_step(f"Self-Test {name}", [str(bin_path), "--pvp-self-test"])
            ran += 1
        else:
            print(f"SKIP: {bin_path} not built (assets missing?)")
    if ran == 0:
        raise RuntimeError("No target binaries found in build/ for --pvp-self-test")


def run_full_suites() -> None:
    for script in ("test_pvp_pon.py", "test_pvp_lon.py"):
        run_step(script, [sys.executable, str(ROOT_DIR / "tools" / script)])


def main() -> int:
    parser = argparse.ArgumentParser(description="Unified build and test gate for rmexe-mobile-recomp")
    parser.add_argument("--check-only", action="store_true", help="Run static/contract checks only")
    parser.add_argument("--quick", action="store_true", help="Build and run --pvp-self-test on built targets")
    parser.add_argument("--reconfigure", action="store_true", help="Force cmake -B build before building")
    args = parser.parse_args()

    try:
        check_contracts()
        if args.check_only:
            return 0
        build_cmake(reconfigure=args.reconfigure)
        if args.quick:
            run_quick_self_tests()
        else:
            run_full_suites()
    except RuntimeError as exc:
        print(f"\n[GATE FAILED] {exc}", file=sys.stderr)
        return 1

    print("\n[GATE PASSED] All checks succeeded.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
