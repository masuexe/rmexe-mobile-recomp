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
     - Match 4: PoN JP Real-Time 16Hz + Buster Charge + Asymmetric Freeze Sync
     - Match 5: PoN JP vs PoN EN [BO3 Triple Battle]
     Verifies clean match completion, matching winner/turn counts, and ZERO `[PvP DESYNC]` warnings.
"""

from __future__ import annotations

import json
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT_DIR / "build"

RESULT_RE = re.compile(
    r"\[PvP BOT RESULT\]\s+local_idx=(-?\d+)\s+winner=(-?\d+)\s+turns=(\d+)\s+"
    r"rounds=(\d+)\s+hp0=(-?\d+)\s+hp1=(-?\d+)\s+desyncs=(\d+)"
    r"(?:\s+bo3=(\d+)\s+games=(\d+)\s+w0=(\d+)\s+w1=(\d+))?"
)


def pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s_tcp:
        s_tcp.bind(("127.0.0.1", 0))
        port = s_tcp.getsockname()[1]
    return port


def http_get_json(url: str, timeout: float = 1.0) -> dict:
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def http_post_json(url: str, timeout: float = 2.0) -> tuple[int, dict]:
    req = urllib.request.Request(url, data=b"", method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        try:
            return exc.code, json.loads(body)
        except Exception:
            return exc.code, {"ok": False, "raw": body}


def find_proc_udp_ports(pid: int) -> set[int]:
    inodes: set[str] = set()
    fd_dir = Path(f"/proc/{pid}/fd")
    try:
        for entry in fd_dir.iterdir():
            try:
                target = os.readlink(entry)
                if target.startswith("socket:[") and target.endswith("]"):
                    inodes.add(target[8:-1])
            except OSError:
                continue
    except OSError:
        return set()
    if not inodes:
        return set()

    ports: set[int] = set()
    for table in ("/proc/net/udp", "/proc/net/udp6"):
        try:
            with open(table, "r", encoding="utf-8") as f:
                next(f, None)
                for line in f:
                    cols = line.split()
                    if len(cols) > 9 and cols[9] in inodes:
                        _, port_hex = cols[1].rsplit(":", 1)
                        port = int(port_hex, 16)
                        if port > 0:
                            ports.add(port)
        except OSError:
            continue
    return ports


def pack_forged_rmxp(
    version: int,
    pkt_type: int,
    slot: int,
    room_id: int,
    token: int,
    frame_or_seq: int = 0,
    keypad_mask: int = 0,
    payload: bytes = b"",
) -> bytes:
    hdr = struct.pack(
        "!4sBBBBIIIiHH",
        b"RMXP",
        version & 0xFF,
        pkt_type & 0xFF,
        slot & 0xFF,
        0,
        room_id & 0xFFFFFFFF,
        token & 0xFFFFFFFF,
        frame_or_seq & 0xFFFFFFFF,
        keypad_mask,
        len(payload) & 0xFFFF,
        0,
    )
    return hdr + payload


def run_server_security_and_guard_tests(server_addr: str, lon_bin: Path, pon_bin: Path, tmp_dir: Path) -> None:
    print("=== [1.5] Testing rmexe_server /pvp/join guards & LoN/PoN room isolation ===", flush=True)

    # 1. Explicit same-UID self-join -> HTTP 409 same_uid
    st, d_create_a = http_post_json(
        f"http://{server_addr}/pvp/create?uid=sec_uid_a&name=HostA&game=pon&format=single"
    )
    if st != 200 or not d_create_a.get("ok"):
        raise RuntimeError(f"Security test create_a failed: status={st} body={d_create_a}")
    rid_a = int(d_create_a["room_id"])
    tok_a = int(d_create_a["token"])

    try:
        st, d_self = http_post_json(
            f"http://{server_addr}/pvp/join?uid=sec_uid_a&name=JoinA&game=pon&room={rid_a}"
        )
        if st != 409 or d_self.get("ok") is not False or d_self.get("error") != "same_uid":
            raise RuntimeError(f"Expected HTTP 409 same_uid for explicit self-join, got status={st} body={d_self}")

        # 2. Quick-match (room=0) skips host's own room -> 404 when alone
        st, d_quick_alone = http_post_json(
            f"http://{server_addr}/pvp/join?uid=sec_uid_a&name=JoinA&game=pon&room=0"
        )
        if st != 404 or d_quick_alone.get("ok") is not False or d_quick_alone.get("error") != "no_open_room":
            raise RuntimeError(
                f"Expected HTTP 404 no_open_room when room=0 only sees same_uid, got status={st} body={d_quick_alone}"
            )

        # 3. Quick-match (room=0) skips host's own room and joins another UID's open room
        st, d_create_b = http_post_json(
            f"http://{server_addr}/pvp/create?uid=sec_uid_b&name=HostB&game=pon&format=single"
        )
        if st != 200 or not d_create_b.get("ok"):
            raise RuntimeError(f"Security test create_b failed: status={st} body={d_create_b}")
        rid_b = int(d_create_b["room_id"])
        tok_b = int(d_create_b["token"])
        try:
            st, d_quick_other = http_post_json(
                f"http://{server_addr}/pvp/join?uid=sec_uid_a&name=JoinA&game=pon&room=0"
            )
            if st != 200 or not d_quick_other.get("ok") or int(d_quick_other.get("room_id", 0)) != rid_b:
                raise RuntimeError(
                    f"Expected room=0 to skip rid_a={rid_a} and join rid_b={rid_b}, got status={st} body={d_quick_other}"
                )
        finally:
            http_post_json(f"http://{server_addr}/pvp/leave?room={rid_b}&token={tok_b}")

        # 4. Cross-game join (game=lon joining pon room_id) -> HTTP 404 no_open_room
        st, d_cross = http_post_json(
            f"http://{server_addr}/pvp/join?uid=sec_uid_c&name=LonGuest&game=lon&room={rid_a}"
        )
        if st != 404 or d_cross.get("ok") is not False or d_cross.get("error") != "no_open_room":
            raise RuntimeError(
                f"Expected HTTP 404 no_open_room for cross-game LoN->PoN join, got status={st} body={d_cross}"
            )
    finally:
        http_post_json(f"http://{server_addr}/pvp/leave?room={rid_a}&token={tok_a}")

    rem_rooms = http_get_json(f"http://{server_addr}/pvp/rooms?game=all").get("rooms", [])
    if rem_rooms:
        raise RuntimeError(f"Expected 0 lingering rooms after /pvp/join guard tests, found: {rem_rooms}")
    print("  -> /pvp/join guards PASSED (409 same_uid, room=0 skip, 404 cross-game rejection)", flush=True)

    # 5. LoN room creation & PoN cross-game /pvp/join HTTP 404 isolation check
    lon_dir = tmp_dir / "lon_guard"
    pon_cross_dir = tmp_dir / "pon_cross_guard"
    lon_dir.mkdir(parents=True, exist_ok=True)
    pon_cross_dir.mkdir(parents=True, exist_ok=True)
    lon_proc = subprocess.Popen(
        [
            str(lon_bin),
            "--headless",
            "--fast-forward",
            "--sav",
            str(lon_dir / "lon.sav"),
            "--config",
            str(lon_dir / "lon.cfg"),
            "--server",
            server_addr,
            "--pvp-create",
        ],
        cwd=str(ROOT_DIR),
        env=headless_env(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        lon_rid = 0
        for _ in range(100):
            if lon_proc.poll() is not None:
                out, err = lon_proc.communicate()
                raise RuntimeError(
                    f"lon_jp_recomp --pvp-create exited unexpectedly rc={lon_proc.returncode}:\nSTDOUT:\n{out}\nSTDERR:\n{err}"
                )
            lon_rooms = http_get_json(f"http://{server_addr}/pvp/rooms?game=lon").get("rooms", [])
            if lon_rooms:
                lon_rid = int(lon_rooms[0]["room_id"])
                break
            time.sleep(0.05)
        if lon_rid <= 0:
            raise RuntimeError("Timed out waiting for lon_jp_recomp --pvp-create room in /pvp/rooms?game=lon")

        pon_rooms = http_get_json(f"http://{server_addr}/pvp/rooms?game=pon").get("rooms", [])
        if any(int(r.get("room_id", 0)) == lon_rid for r in pon_rooms):
            raise RuntimeError(f"Expected PoN /pvp/rooms to exclude LoN room #{lon_rid}, found: {pon_rooms}")

        # Direct HTTP /pvp/join with game=pon onto the LoN room must be rejected with HTTP 404
        st, d_cross_pon = http_post_json(
            f"http://{server_addr}/pvp/join?uid=pon_cross_uid&name=PonGuest&game=pon&room={lon_rid}"
        )
        if st != 404 or d_cross_pon.get("ok") is not False or d_cross_pon.get("error") != "no_open_room":
            raise RuntimeError(
                f"Expected HTTP 404 no_open_room for PoN->LoN join on #{lon_rid}, got status={st} body={d_cross_pon}"
            )

        # Launching pon_jp_recomp --pvp-join <lon_rid> must be rejected and leave the LoN room open
        pon_proc = subprocess.run(
            [
                str(pon_bin),
                "--headless",
                "--fast-forward",
                "--max-frames",
                "20",
                "--sav",
                str(pon_cross_dir / "pon.sav"),
                "--config",
                str(pon_cross_dir / "pon.cfg"),
                "--server",
                server_addr,
                "--pvp-join",
                str(lon_rid),
            ],
            cwd=str(ROOT_DIR),
            env=headless_env(),
            capture_output=True,
            text=True,
            timeout=30,
        )
        if pon_proc.returncode != 0:
            raise RuntimeError(
                f"pon_jp_recomp --pvp-join cross-game check exited rc={pon_proc.returncode}:\nSTDOUT:\n{pon_proc.stdout}\nSTDERR:\n{pon_proc.stderr}"
            )
        if "已加入" in pon_proc.stdout or "已加入" in pon_proc.stderr:
            raise RuntimeError(f"pon_jp_recomp unexpectedly joined LoN room #{lon_rid}")

        lon_rooms_after = http_get_json(f"http://{server_addr}/pvp/rooms?game=lon").get("rooms", [])
        if not any(int(r.get("room_id", 0)) == lon_rid for r in lon_rooms_after):
            raise RuntimeError(f"Expected LoN room #{lon_rid} to stay open after rejected PoN join, found: {lon_rooms_after}")
    finally:
        lon_proc.terminate()
        try:
            lon_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            lon_proc.kill()
            lon_proc.wait(timeout=5)
    print(f"  -> LoN --pvp-create + PoN /pvp/join HTTP 404 isolation PASSED (room #{lon_rid})\n", flush=True)


def headless_env() -> dict[str, str]:
    env = os.environ.copy()
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    return env


def run_self_test(bin_path: Path, label: str, tmp_dir: Path) -> None:
    print(f"=== [1] Running --pvp-self-test on {label} ({bin_path.name}) ===", flush=True)
    st_dir = tmp_dir / f"selftest_{bin_path.stem}"
    st_dir.mkdir(parents=True, exist_ok=True)
    t0 = time.monotonic()
    proc = subprocess.run(
        [
            str(bin_path),
            "--pvp-self-test",
            "--sav",
            str(st_dir / "test.sav"),
            "--config",
            str(st_dir / "controls.cfg"),
        ],
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
    bo3: bool = False,
    inject_forged_udp: bool = False,
) -> None:
    print(
        f"=== [2.{match_idx}] Live UDP Match: {match_label} ({p0_bin.name} vs {p1_bin.name}) ===",
        flush=True,
    )
    sav0 = tmp_dir / f"m{match_idx}_p0.sav"
    sav1 = tmp_dir / f"m{match_idx}_p1.sav"
    cfg0 = tmp_dir / f"m{match_idx}_p0.cfg"
    cfg1 = tmp_dir / f"m{match_idx}_p1.cfg"

    mode_args0 = ["--scale", "1", "--config", str(cfg0)] if realtime_clock else ["--headless", "--fast-forward", "--config", str(cfg0)]
    mode_args1 = ["--scale", "1", "--config", str(cfg1)] if realtime_clock else ["--headless", "--fast-forward", "--config", str(cfg1)]

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
    if bo3:
        cmd0.append("--pvp-bo3")
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
    room_fmt = None
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
                room_fmt = rooms[0].get("format")
                break
        except Exception:
            pass
        time.sleep(0.05)

    if room_id is None:
        p0.kill()
        raise RuntimeError(f"Timed out waiting for {p0_bin.name} to open a PoN room on {server_addr}")

    expected_fmt = "bo3" if bo3 else "single"
    if room_fmt != expected_fmt:
        p0.kill()
        raise RuntimeError(
            f"Room #{room_id} format mismatch in {match_label}: expected {expected_fmt!r}, got {room_fmt!r}"
        )

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

    max_wait = 90.0 if (realtime_clock or bo3) else 45.0
    th0 = threading.Thread(target=wait_proc, args=(p0, 0, max_wait), daemon=True)
    th1 = threading.Thread(target=wait_proc, args=(p1, 1, max_wait), daemon=True)
    th0.start()
    th1.start()

    forged_stats: dict = {
        "srv_pkts": 0,
        "cli_pkts": 0,
        "p0_ports": set(),
        "p1_ports": set(),
        "error": None,
    }
    forge_thread: threading.Thread | None = None
    if inject_forged_udp:
        srv_host, srv_port_s = server_addr.rsplit(":", 1)
        srv_port = int(srv_port_s)

        def run_forge() -> None:
            sock_a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock_b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                sock_a.bind(("127.0.0.1", 0))
                sock_b.bind(("127.0.0.1", 0))
                bad_pkts = [
                    # Wrong token (disconnect, frame input, match end)
                    pack_forged_rmxp(1, 8, 1, room_id, 0xDEADBEEF),
                    pack_forged_rmxp(1, 6, 1, room_id, 0xCAFEBABE, frame_or_seq=5, keypad_mask=0x7FFF),
                    pack_forged_rmxp(1, 15, 2, room_id, 0),
                    # Wrong protocol version (= 2)
                    pack_forged_rmxp(2, 8, 1, room_id, 1),
                    pack_forged_rmxp(2, 6, 2, room_id, 1, frame_or_seq=12, keypad_mask=0x10),
                    pack_forged_rmxp(2, 15, 1, room_id, 1),
                    # Spoofed source port (unauthorized ephemeral socket != server/client port)
                    pack_forged_rmxp(1, 1, 1, room_id, 1),
                    pack_forged_rmxp(1, 8, 2, room_id, room_id ^ 0x5555),
                    pack_forged_rmxp(1, 6, 1, room_id + 777, 1, frame_or_seq=20, keypad_mask=0x20),
                    # Truncated / non-RMXP garbage
                    b"BAD",
                    b"RMXP\x01\x08\x01\x00" + b"\xff" * 20,
                ]
                waves_both = 0
                deadline_f = time.monotonic() + 12.0
                while time.monotonic() < deadline_f and waves_both < 15:
                    if p0.poll() is not None or p1.poll() is not None:
                        break
                    p0_ps = find_proc_udp_ports(p0.pid)
                    p1_ps = find_proc_udp_ports(p1.pid)
                    forged_stats["p0_ports"].update(p0_ps)
                    forged_stats["p1_ports"].update(p1_ps)
                    client_targets = sorted(forged_stats["p0_ports"] | forged_stats["p1_ports"])
                    for idx, pkt in enumerate(bad_pkts):
                        sk = sock_a if (idx % 2 == 0) else sock_b
                        sk.sendto(pkt, (srv_host, srv_port))
                        forged_stats["srv_pkts"] += 1
                        for cp in client_targets:
                            sk.sendto(pkt, (srv_host, cp))
                            forged_stats["cli_pkts"] += 1
                    if forged_stats["p0_ports"] and forged_stats["p1_ports"]:
                        waves_both += 1
                    time.sleep(0.04)
            except Exception as exc:
                forged_stats["error"] = exc
            finally:
                sock_a.close()
                sock_b.close()

        forge_thread = threading.Thread(target=run_forge, daemon=True)
        forge_thread.start()

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
    if forge_thread is not None:
        forge_thread.join()
        if forged_stats["error"] is not None:
            raise RuntimeError(f"Forged UDP injector error in {match_label}: {forged_stats['error']}")
        if forged_stats["srv_pkts"] <= 0 or not forged_stats["p0_ports"] or not forged_stats["p1_ports"] or forged_stats["cli_pkts"] <= 0:
            raise RuntimeError(
                f"Forged UDP injector did not target server + both clients in {match_label}: {forged_stats}"
            )
        print(
            f"  Forged UDP injected: srv_pkts={forged_stats['srv_pkts']}, "
            f"cli_pkts={forged_stats['cli_pkts']}, "
            f"p0_ports={sorted(forged_stats['p0_ports'])}, p1_ports={sorted(forged_stats['p1_ports'])}"
        )

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

    r0 = tuple(int(x) for x in m0.groups()[:7])
    r1 = tuple(int(x) for x in m1.groups()[:7])
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

    bo3_info = ""
    if bo3:
        if m0.group(7) is None or m1.group(7) is None:
            raise RuntimeError(f"Missing BO3 fields (bo3/games/w0/w1) in [PvP BOT RESULT] for {match_label}")
        b0 = tuple(int(x) for x in m0.groups()[7:])
        b1 = tuple(int(x) for x in m1.groups()[7:])
        if b0 != b1:
            raise RuntimeError(f"BO3 summary mismatch in {match_label}: P0={b0} vs P1={b1}")
        bo3_flag, games_cnt, w0_cnt, w1_cnt = b0
        if bo3_flag != 1 or games_cnt < 2:
            raise RuntimeError(
                f"Expected BO3 match with games >= 2 in {match_label}: bo3={bo3_flag}, games={games_cnt}, w0={w0_cnt}, w1={w1_cnt}"
            )
        bo3_info = f", bo3=1, games={games_cnt}, score={w0_cnt}-{w1_cnt}"
    elif m0.group(7) is not None and m1.group(7) is not None:
        if int(m0.group(7)) != 0 or int(m1.group(7)) != 0:
            raise RuntimeError(f"Unexpected bo3=1 in single-battle match {match_label}")

    if realtime_clock and exit_skew_s > 0.75:
        raise RuntimeError(
            f"Real-time clock drift too high in {match_label}: exit_skew={exit_skew_s:.3f}s (> 0.75s)"
        )
    print(
        f"  -> {match_label} PASSED in {dt:.2f}s "
        f"(room #{room_id}, winner=P{r0[1]}, turns={r0[2]}, rounds={r0[3]}{bo3_info}, "
        f"exit_skew={exit_skew_s:.3f}s, desyncs=0)!\n",
        flush=True,
    )


def main() -> int:
    pon_jp = BUILD_DIR / "pon_jp_recomp"
    pon_en = BUILD_DIR / "pon_en_recomp"
    lon_jp = BUILD_DIR / "lon_jp_recomp"
    srv_bin = BUILD_DIR / "rmexe_server"

    for b in (pon_jp, pon_en, lon_jp, srv_bin):
        if not b.exists():
            print(f"ERROR: Required binary not found: {b}", file=sys.stderr)
            return 1

    with tempfile.TemporaryDirectory(prefix="rmexe_pvp_test_") as tmp_s:
        tmp_dir = Path(tmp_s)
        run_self_test(pon_jp, "PoN JP", tmp_dir)
        run_self_test(pon_en, "PoN EN", tmp_dir)

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

            run_server_security_and_guard_tests(server_addr, lon_jp, pon_jp, tmp_dir)

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
            run_live_udp_match(
                "PoN JP vs PoN EN [BO3 Triple Battle]",
                pon_jp,
                pon_en,
                server_addr,
                tmp_dir,
                5,
                bo3=True,
            )
            run_live_udp_match(
                "PoN JP vs PoN EN [Real-Time 16Hz + Forged UDP Auth Guard]",
                pon_jp,
                pon_en,
                server_addr,
                tmp_dir,
                6,
                realtime_clock=True,
                inject_forged_udp=True,
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
