#!/usr/bin/env python3
"""
Automated End-to-End Verification Suite for Rockman EXE PoN (JP & EN) PvP NetBattle.

Runs:
  1. Headless C++ Self-Tests (--pvp-self-test) on both `pon_jp_recomp` and `pon_en_recomp`:
     - All 213 BattleChip / PA / Charge Shot IDs (0..212) solo P0<->P1 mirror symmetry,
       simultaneous clash stability, orientation-invariant state hashing, and read-only render checks.
     - 15 dedicated mechanics & regression assertions (EN Buster MAX guard, Muramasa, Atk+10 isolation,
       FstGauge/FullCust, Barrier/NrthWind, Guard + Breaking shatter, CrsShld, AntiDmg,
       AntiFire chip + Charge Shot, AntiNavi, dual RockCube + cover block + Breaking shatter + Mine,
       Wind/Fan push/pull, Satelit1 targeting, AreaGrab natural revert symmetry,
       and mid-battle Round 2+ Custom Screen isolation).
     - 300-frame chaotic 2P GGPO rollback determinism torture test (1..20 frame rollbacks).
  2. Live Dual-Process UDP NetBattle Matches via `rmexe_server`:
     - Match 1: PoN JP vs PoN JP
     - Match 2: PoN EN vs PoN EN
     - Match 3: PoN JP vs PoN EN (Cross-Region NetBattle!)
     Verifies clean match completion, matching winner/turn counts, and ZERO `[PvP DESYNC]` warnings.
"""

from __future__ import annotations

import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT_DIR / "build"

RESULT_RE = re.compile(
    r"\[PvP BOT RESULT\]\s+local_idx=(-?\d+)\s+winner=(-?\d+)\s+turns=(\d+)\s+"
    r"rounds=(\d+)\s+hp0=(-?\d+)\s+hp1=(-?\d+)\s+desyncs=(\d+)"
)


def pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s_tcp:
        s_tcp.bind(("127.0.0.1", 0))
        port = s_tcp.getsockname()[1]
    return port


def http_get_json(url: str, timeout: float = 1.0) -> dict:
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def headless_env() -> dict[str, str]:
    env = os.environ.copy()
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    return env


def run_self_test(bin_path: Path, label: str) -> None:
    print(f"=== [1] Running --pvp-self-test on {label} ({bin_path.name}) ===", flush=True)
    t0 = time.monotonic()
    proc = subprocess.run(
        [str(bin_path), "--pvp-self-test"],
        cwd=str(ROOT_DIR),
        env=headless_env(),
        capture_output=True,
        text=True,
        timeout=120,
    )
    dt = time.monotonic() - t0
    if proc.stdout:
        for line in proc.stdout.strip().splitlines():
            print(f"  {line}")
    if proc.returncode != 0 or "[PvP SELF-TEST PASS]" not in proc.stdout:
        print(f"ERROR: {label} --pvp-self-test failed (rc={proc.returncode}):", file=sys.stderr)
        if proc.stderr:
            print(proc.stderr, file=sys.stderr)
        sys.exit(1)
    print(f"  -> {label} self-test PASSED in {dt:.2f}s!\n", flush=True)


def run_live_udp_match(
    match_label: str,
    p0_bin: Path,
    p1_bin: Path,
    server_addr: str,
    tmp_dir: Path,
    match_idx: int,
    realtime_clock: bool = False,
    inject_stalls: bool = False,
) -> None:
    print(
        f"=== [2.{match_idx}] Live UDP Match: {match_label} ({p0_bin.name} vs {p1_bin.name}) ===",
        flush=True,
    )
    sav0 = tmp_dir / f"m{match_idx}_p0.sav"
    sav1 = tmp_dir / f"m{match_idx}_p1.sav"
    cfg0 = tmp_dir / f"m{match_idx}_p0.cfg"
    cfg1 = tmp_dir / f"m{match_idx}_p1.cfg"

    mode_args0 = ["--scale", "1", "--config", str(cfg0)] if realtime_clock else ["--headless", "--fast-forward"]
    mode_args1 = ["--scale", "1", "--config", str(cfg1)] if realtime_clock else ["--headless", "--fast-forward"]

    cmd0 = [
        str(p0_bin),
        *mode_args0,
        "--server",
        server_addr,
        "--uid",
        f"test_uid_m{match_idx}_p0",
        "--name",
        f"BOT0_M{match_idx}",
        "--sav",
        str(sav0),
        "--pvp-auto-bot",
        "--pvp-create",
    ]
    t0 = time.monotonic()
    p0 = subprocess.Popen(
        cmd0,
        cwd=str(ROOT_DIR),
        env=headless_env(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    room_id = None
    deadline = time.monotonic() + 12.0
    while time.monotonic() < deadline:
        if p0.poll() is not None:
            out0, err0 = p0.communicate()
            raise RuntimeError(f"Host exited before creating room:\nSTDOUT:\n{out0}\nSTDERR:\n{err0}")
        try:
            data = http_get_json(f"http://{server_addr}/pvp/rooms?game=pon")
            rooms = data.get("rooms", [])
            if rooms:
                room_id = int(rooms[0]["room_id"])
                break
        except Exception:
            pass
        time.sleep(0.05)

    if room_id is None:
        p0.kill()
        raise RuntimeError(f"Timed out waiting for {p0_bin.name} to open a PoN room on {server_addr}")

    cmd1 = [
        str(p1_bin),
        *mode_args1,
        "--server",
        server_addr,
        "--uid",
        f"test_uid_m{match_idx}_p1",
        "--name",
        f"BOT1_M{match_idx}",
        "--sav",
        str(sav1),
        "--pvp-auto-bot",
        "--pvp-join",
        str(room_id),
    ]
    p1 = subprocess.Popen(
        cmd1,
        cwd=str(ROOT_DIR),
        env=headless_env(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    res_box: list[dict] = [{}, {}]

    def wait_proc(proc: subprocess.Popen, slot: int, timeout_s: float) -> None:
        try:
            out, err = proc.communicate(timeout=timeout_s)
            res_box[slot] = {"out": out, "err": err, "t_end": time.monotonic(), "timeout": False}
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate()
            res_box[slot] = {"out": out, "err": err, "t_end": time.monotonic(), "timeout": True}

    max_wait = 90.0 if realtime_clock else 45.0
    th0 = threading.Thread(target=wait_proc, args=(p0, 0, max_wait), daemon=True)
    th1 = threading.Thread(target=wait_proc, args=(p1, 1, max_wait), daemon=True)
    th0.start()
    th1.start()

    if inject_stalls and hasattr(signal, "SIGSTOP"):
        # Inject asymmetric OS freezes mid-battle while both bots Buster Charge (蓄力集气)
        for target_proc, delay_s, freeze_s in ((p1, 2.2, 0.42), (p0, 2.0, 0.42)):
            time.sleep(delay_s)
            if p0.poll() is None and p1.poll() is None:
                try:
                    os.kill(target_proc.pid, signal.SIGSTOP)
                    time.sleep(freeze_s)
                finally:
                    try:
                        os.kill(target_proc.pid, signal.SIGCONT)
                    except OSError:
                        pass

    th0.join()
    th1.join()

    if res_box[0].get("timeout") or res_box[1].get("timeout"):
        raise RuntimeError(
            f"Live match {match_label} timed out!\n"
            f"--- P0 stdout/stderr ---\n{res_box[0].get('out', '')}\n{res_box[0].get('err', '')}\n"
            f"--- P1 stdout/stderr ---\n{res_box[1].get('out', '')}\n{res_box[1].get('err', '')}"
        )

    out0, err0 = res_box[0]["out"], res_box[0]["err"]
    out1, err1 = res_box[1]["out"], res_box[1]["err"]
    exit_skew_s = abs(res_box[0]["t_end"] - res_box[1]["t_end"])
    dt = time.monotonic() - t0

    if "[PvP DESYNC]" in err0 or "[PvP DESYNC]" in err1 or "[PvP Exception]" in err0 or "[PvP Exception]" in err1:
        raise RuntimeError(
            f"Desync or exception detected in {match_label}!\n"
            f"P0 stderr:\n{err0}\nP1 stderr:\n{err1}"
        )
    if p0.returncode != 0 or p1.returncode != 0:
        raise RuntimeError(
            f"Non-zero exit in {match_label} (rc0={p0.returncode}, rc1={p1.returncode}):\n"
            f"P0 out/err:\n{out0}\n{err0}\nP1 out/err:\n{out1}\n{err1}"
        )

    m0 = RESULT_RE.search(out0)
    m1 = RESULT_RE.search(out1)
    if not m0 or not m1:
        raise RuntimeError(f"Missing [PvP BOT RESULT] line:\nP0:\n{out0}\nP1:\n{out1}")

    r0 = tuple(int(x) for x in m0.groups())
    r1 = tuple(int(x) for x in m1.groups())
    # (local_idx, winner, turns, rounds, hp0, hp1, desyncs)
    print(f"  P0: {m0.group(0)}")
    print(f"  P1: {m1.group(0)}")

    if r0[6] != 0 or r1[6] != 0:
        raise RuntimeError(f"Non-zero desync counter in {match_label}: P0={r0[6]} P1={r1[6]}")
    if r0[1] not in (-1, 0, 1) or r0[1] != r1[1]:
        raise RuntimeError(f"Winner mismatch in {match_label}: P0 saw {r0[1]}, P1 saw {r1[1]}")
    if r0[2] != r1[2] or r0[3] != r1[3] or r0[4] != r1[4] or r0[5] != r1[5]:
        raise RuntimeError(
            f"End-of-match state mismatch in {match_label}: P0={r0} vs P1={r1}"
        )
    if realtime_clock and exit_skew_s > 0.75:
        raise RuntimeError(
            f"Real-time clock drift too high in {match_label}: exit_skew={exit_skew_s:.3f}s (> 0.75s)"
        )
    print(
        f"  -> {match_label} PASSED in {dt:.2f}s "
        f"(room #{room_id}, winner=P{r0[1]}, turns={r0[2]}, rounds={r0[3]}, "
        f"exit_skew={exit_skew_s:.3f}s, desyncs=0)!\n",
        flush=True,
    )


def main() -> int:
    pon_jp = BUILD_DIR / "pon_jp_recomp"
    pon_en = BUILD_DIR / "pon_en_recomp"
    srv_bin = BUILD_DIR / "rmexe_server"

    for b in (pon_jp, pon_en, srv_bin):
        if not b.exists():
            print(f"ERROR: Required binary not found: {b}", file=sys.stderr)
            return 1

    run_self_test(pon_jp, "PoN JP")
    run_self_test(pon_en, "PoN EN")

    with tempfile.TemporaryDirectory(prefix="rmexe_pvp_test_") as tmp_s:
        tmp_dir = Path(tmp_s)
        port = pick_free_port()
        server_addr = f"127.0.0.1:{port}"
        srv_proc = subprocess.Popen(
            [
                str(srv_bin),
                "--bind",
                "127.0.0.1",
                "--port",
                str(port),
                "--data-dir",
                str(tmp_dir / "server_data"),
                "--quiet",
            ],
            cwd=str(ROOT_DIR),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            ready = False
            for _ in range(50):
                try:
                    js = http_get_json(f"http://{server_addr}/health", timeout=0.5)
                    if js.get("ok"):
                        ready = True
                        break
                except Exception:
                    time.sleep(0.05)
            if not ready:
                raise RuntimeError("rmexe_server failed to become healthy")

            run_live_udp_match("PoN JP vs PoN JP", pon_jp, pon_jp, server_addr, tmp_dir, 1)
            run_live_udp_match("PoN EN vs PoN EN", pon_en, pon_en, server_addr, tmp_dir, 2)
            run_live_udp_match("PoN JP vs PoN EN (Cross-Region)", pon_jp, pon_en, server_addr, tmp_dir, 3)
            run_live_udp_match(
                "PoN JP Real-Time 16Hz + Buster Charge + Asymmetric Freeze Sync",
                pon_jp,
                pon_jp,
                server_addr,
                tmp_dir,
                4,
                realtime_clock=True,
                inject_stalls=True,
            )
        finally:
            srv_proc.terminate()
            try:
                srv_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                srv_proc.kill()

    print("====================================================================")
    print("ALL PoN PvP SELF-TESTS & LIVE CROSS-REGION UDP MATCHES PASSED (0 DESYNCS)!")
    print("====================================================================")
    return 0


if __name__ == "__main__":
    sys.exit(main())
