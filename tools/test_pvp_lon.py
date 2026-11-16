#!/usr/bin/env python3
"""
End-to-end LoN Online PvP verification suite (Stage 4):
  Part 1:  Headless --pvp-self-test (Suites A-L) on lon_jp_recomp and lon_en_recomp
  Part 2a: rmexe_server bidirectional PoN<->LoN isolation (HTTP 404 + CLI --pvp-join)
           and LoN UDP auth / v2 PvpFighterBuild & PvpCustomCommit wire boundary fuzzing
  Part 2b: Six live rmexe_server + UDP NetBattle matches:
           2.1 LoN JP vs LoN JP (Single)
           2.2 LoN EN vs LoN EN (Single)
           2.3 LoN JP vs LoN EN (Cross-Region Single)
           2.4 LoN EN vs LoN JP (Reverse Cross-Region Single)
           2.5 LoN JP vs LoN EN (BO3 Triple Battle, seeded Trance + asymmetric dell_time)
           2.6 LoN JP vs LoN EN (Real-Time 16Hz Clock-Sync + mid-match forged UDP guard)
"""

import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(ROOT, "build")

SERVER_BIN = os.path.join(BUILD_DIR, "rmexe_server")
LON_JP_BIN = os.path.join(BUILD_DIR, "lon_jp_recomp")
LON_EN_BIN = os.path.join(BUILD_DIR, "lon_en_recomp")
PON_JP_BIN = os.path.join(BUILD_DIR, "pon_jp_recomp")

LON_JP_SP = os.path.join(ROOT, "assets", "LoN_jp", "sp", "LoN.sp")
LON_EN_SP = os.path.join(ROOT, "assets", "LoN", "sp", "LoN.sp")

RESULT_RE = re.compile(
    r"\[PvP BOT RESULT\]\s+"
    r"local_idx=(-?\d+)\s+"
    r"winner=(-?\d+)\s+"
    r"turns=(\d+)\s+"
    r"rounds=(\d+)\s+"
    r"hp0=(\d+)\s+"
    r"hp1=(\d+)\s+"
    r"desyncs=(\d+)"
    r"(?:\s+bo3=(\d+)\s+games=(\d+)\s+w0=(\d+)\s+w1=(\d+))?"
)

# PvpPacketType values from common/pvp_protocol.hpp
PKT_BIND = 1
PKT_BIND_ACK = 2
PKT_MATCH_START = 3
PKT_PING = 4
PKT_PONG = 5
PKT_FRAME_INPUT = 6
PKT_DISCONNECT = 8
PKT_BUILD_SYNC = 9
PKT_BUILD_ACK = 10
PKT_CUSTOM_COMMIT = 11
PKT_CUSTOM_ACK = 12

LON_SP_HEADER_SIZE = 64
LON_SAV_SV_BASE = 400
LON_IDX_MAX_HP = 3
LON_IDX_GET_ARM0 = 529
LON_IDX_ARM_FLG = 537
LON_IDX_DELL_TIME0 = 538


def alloc_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def http_json(method: str, url: str) -> tuple[int, dict]:
    req = urllib.request.Request(url, method=method, data=(b"" if method == "POST" else None))
    try:
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            return int(resp.status), json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", errors="replace")
        try:
            body = json.loads(raw)
        except Exception:
            body = {"raw": raw}
        return int(e.code), body


def pack_rmxp(
    pkt_type: int,
    room_id: int,
    token: int,
    *,
    version: int = 1,
    slot: int = 0,
    flags: int = 0,
    frame_or_seq: int = 0,
    keypad_mask: int = 0,
    payload: bytes = b"",
    magic: bytes = b"RMXP",
    checksum16: int = 0,
    declared_len: int | None = None,
) -> bytes:
    plen = len(payload) if declared_len is None else declared_len
    hdr = struct.pack(
        "!4sBBBBIIIiHH",
        magic,
        version,
        pkt_type,
        slot,
        flags,
        room_id,
        token,
        frame_or_seq,
        keypad_mask,
        plen,
        checksum16,
    )
    return hdr + payload


def drain_udp(sock: socket.socket) -> list[bytes]:
    out = []
    while True:
        try:
            out.append(sock.recv(4096))
        except socket.timeout:
            break
    return out


def find_proc_udp_ports(pid: int) -> list[int]:
    inodes: set[str] = set()
    fd_dir = f"/proc/{pid}/fd"
    try:
        for fd in os.listdir(fd_dir):
            try:
                target = os.readlink(os.path.join(fd_dir, fd))
            except OSError:
                continue
            if target.startswith("socket:[") and target.endswith("]"):
                inodes.add(target[8:-1])
    except OSError:
        return []
    if not inodes:
        return []
    ports: list[int] = []
    for tbl in (f"/proc/{pid}/net/udp", f"/proc/{pid}/net/udp6"):
        try:
            with open(tbl, "r", encoding="utf-8") as f:
                next(f, None)
                for line in f:
                    cols = line.split()
                    if len(cols) >= 10 and cols[9] in inodes:
                        hex_port = cols[1].split(":")[-1]
                        p = int(hex_port, 16)
                        if p > 0 and p not in ports:
                            ports.append(p)
        except OSError:
            continue
    return ports


def seed_lon_sav(sp_path: str, out_sav_path: str, *, dell_time_vals: list[int], max_hp: int | None = None) -> None:
    raw = open(sp_path, "rb").read()
    assert len(raw) > LON_SP_HEADER_SIZE + 3000, f"Unexpected LoN.sp size: {len(raw)}"
    data = bytearray(raw[LON_SP_HEADER_SIZE:])
    if max_hp is not None:
        struct.pack_into(">i", data, LON_SAV_SV_BASE + LON_IDX_MAX_HP * 4, int(max_hp))
    for i in range(6):
        struct.pack_into(">i", data, LON_SAV_SV_BASE + (LON_IDX_GET_ARM0 + i) * 4, 1)
    struct.pack_into(">i", data, LON_SAV_SV_BASE + LON_IDX_ARM_FLG * 4, 1)
    assert len(dell_time_vals) == 10
    for i, dv in enumerate(dell_time_vals):
        struct.pack_into(">i", data, LON_SAV_SV_BASE + (LON_IDX_DELL_TIME0 + i) * 4, int(dv))
    with open(out_sav_path, "wb") as f:
        f.write(data)


def start_server(port: int, data_dir: str, log_path: str) -> subprocess.Popen:
    log_f = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(
        [SERVER_BIN, "--bind", "127.0.0.1", "--port", str(port), "--data-dir", data_dir],
        cwd=ROOT,
        stdout=log_f,
        stderr=subprocess.STDOUT,
    )
    proc._test_log_f = log_f  # type: ignore[attr-defined]
    for _ in range(80):
        if proc.poll() is not None:
            raise RuntimeError(f"rmexe_server exited prematurely ({proc.returncode})")
        try:
            st, body = http_json("GET", f"http://127.0.0.1:{port}/health")
            if st == 200 and body.get("ok"):
                return proc
        except Exception:
            time.sleep(0.05)
    stop_server(proc)
    raise RuntimeError("Timed out waiting for rmexe_server /health")


def stop_server(proc: subprocess.Popen) -> None:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    log_f = getattr(proc, "_test_log_f", None)
    if log_f is not None:
        log_f.close()


def run_self_test(label: str, bin_path: str) -> None:
    print(f"=== [1] Running --pvp-self-test on {label} ({os.path.basename(bin_path)}) ===", flush=True)
    with tempfile.TemporaryDirectory(prefix="lon_st_") as td:
        t0 = time.monotonic()
        res = subprocess.run(
            [
                bin_path,
                "--headless",
                "--fast-forward",
                "--pvp-self-test",
                "--sav",
                os.path.join(td, "st.sav"),
                "--config",
                os.path.join(td, "st.cfg"),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=90,
        )
        dt = time.monotonic() - t0
        combined = res.stdout + res.stderr
        for line in combined.splitlines():
            if "[PvP" in line:
                print(f"  {line}")
        assert res.returncode == 0, f"{label} --pvp-self-test failed (rc={res.returncode})\n{combined}"
        assert "[PvP SELF-TEST PASS]" in combined, f"{label} missing [PvP SELF-TEST PASS]\n{combined}"
        assert "[PvP SELF-TEST FAIL]" not in combined, f"{label} reported failures\n{combined}"
        assert "JavaException" not in combined and "[PvP Exception]" not in combined, (
            f"{label} threw JavaException\n{combined}"
        )
        for suite in "ABCDEFGHIJKLMNO":
            expected = f"Suite {suite} complete (failures=0)"
            assert expected in combined, f"{label} missing '{expected}'\n{combined}"
        print(f"  -> {label} self-test PASSED in {dt:.2f}s!", flush=True)


def run_isolation_and_security_tests() -> None:
    print("\n=== [2a] Testing PoN<->LoN Isolation & LoN UDP Security / Wire Fuzzing ===", flush=True)
    port = alloc_free_port()
    with tempfile.TemporaryDirectory(prefix="lon_sec_") as td:
        srv_log = os.path.join(td, "srv.log")
        srv = start_server(port, td, srv_log)
        try:
            # 1. Bidirectional HTTP 404 isolation
            st, lon_rm = http_json("POST", f"http://127.0.0.1:{port}/pvp/create?uid=9101&name=LonHost&game=lon&format=single")
            assert st == 200 and lon_rm.get("ok"), f"LoN create failed: {st} {lon_rm}"
            lon_rid = int(lon_rm["room_id"])
            lon_tok = int(lon_rm["token"])

            st, lon_list = http_json("GET", f"http://127.0.0.1:{port}/pvp/rooms?game=lon")
            st2, pon_list = http_json("GET", f"http://127.0.0.1:{port}/pvp/rooms?game=pon")
            assert st == 200 and any(r["room_id"] == lon_rid for r in lon_list.get("rooms", []))
            assert st2 == 200 and not any(r["room_id"] == lon_rid for r in pon_list.get("rooms", []))

            st, cross1 = http_json("POST", f"http://127.0.0.1:{port}/pvp/join?room={lon_rid}&uid=9102&name=PonJoin&game=pon")
            assert st == 404 and cross1.get("error") == "no_open_room", f"Expected HTTP 404 PoN->LoN, got {st} {cross1}"

            st, same_uid = http_json("POST", f"http://127.0.0.1:{port}/pvp/join?room={lon_rid}&uid=9101&name=SameUid&game=lon")
            assert st == 409, f"Expected HTTP 409 same_uid, got {st} {same_uid}"

            st, pon_rm = http_json("POST", f"http://127.0.0.1:{port}/pvp/create?uid=9103&name=PonHost&game=pon&format=single")
            assert st == 200 and pon_rm.get("ok"), f"PoN create failed: {st} {pon_rm}"
            pon_rid = int(pon_rm["room_id"])
            pon_tok = int(pon_rm["token"])

            st, pon_list2 = http_json("GET", f"http://127.0.0.1:{port}/pvp/rooms?game=pon")
            st2, lon_list2 = http_json("GET", f"http://127.0.0.1:{port}/pvp/rooms?game=lon")
            assert st == 200 and any(r["room_id"] == pon_rid for r in pon_list2.get("rooms", []))
            assert st2 == 200 and not any(r["room_id"] == pon_rid for r in lon_list2.get("rooms", []))

            st, cross2 = http_json("POST", f"http://127.0.0.1:{port}/pvp/join?room={pon_rid}&uid=9104&name=LonJoin&game=lon")
            assert st == 404 and cross2.get("error") == "no_open_room", f"Expected HTTP 404 LoN->PoN, got {st} {cross2}"

            # 2. CLI cross-game --pvp-join rejection (both directions)
            for label, bin_path, target_rid, target_game, bad_game, uid in [
                ("pon_jp->lon_room", PON_JP_BIN, lon_rid, "lon", "pon", 9105),
                ("lon_jp->pon_room", LON_JP_BIN, pon_rid, "pon", "lon", 9106),
            ]:
                p = subprocess.Popen(
                    [
                        bin_path,
                        "--headless",
                        "--fast-forward",
                        "--config",
                        os.path.join(td, f"{label}.cfg"),
                        "--sav",
                        os.path.join(td, f"{label}.sav"),
                        "--server",
                        f"127.0.0.1:{port}",
                        "--uid",
                        str(uid),
                        "--name",
                        "CrossCli",
                        "--pvp-auto-bot",
                        "--pvp-join",
                        str(target_rid),
                    ],
                    cwd=ROOT,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                time.sleep(1.0)
                p.terminate()
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
                # Original room must still be untouched and open
                st, cur_rooms = http_json("GET", f"http://127.0.0.1:{port}/pvp/rooms?game={target_game}")
                assert st == 200 and any(r["room_id"] == target_rid for r in cur_rooms.get("rooms", [])), (
                    f"{label} unexpectedly mutated room #{target_rid}: {cur_rooms}"
                )
                srv_text = open(srv_log, "r", encoding="utf-8", errors="replace").read()
                expected_404 = f"game={bad_game}&room={target_rid} -> 404"
                assert expected_404 in srv_text, f"{label} did not trigger HTTP 404 on server log"

            http_json("POST", f"http://127.0.0.1:{port}/pvp/leave?room={lon_rid}&token={lon_tok}")
            http_json("POST", f"http://127.0.0.1:{port}/pvp/leave?room={pon_rid}&token={pon_tok}")
            print("  -> Bidirectional PoN<->LoN HTTP 404 & CLI --pvp-join isolation PASSED!", flush=True)

            # 3. LoN UDP security & v2 wire boundary fuzzing
            st, c_res = http_json("POST", f"http://127.0.0.1:{port}/pvp/create?uid=9201&name=UdpP0&game=lon&format=single")
            assert st == 200 and c_res.get("ok")
            rid = int(c_res["room_id"])
            slot0, tok0 = int(c_res["slot"]), int(c_res["token"])

            st, j_res = http_json("POST", f"http://127.0.0.1:{port}/pvp/join?room={rid}&uid=9202&name=UdpP1&game=lon")
            assert st == 200 and j_res.get("ok")
            slot1, tok1 = int(j_res["slot"]), int(j_res["token"])

            u0 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            u1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            u0.settimeout(0.15)
            u1.settimeout(0.15)
            addr = ("127.0.0.1", port)
            try:
                # Ping room=0 -> Pong
                u0.sendto(pack_rmxp(PKT_PING, 0, 0, frame_or_seq=42), addr)
                pong0 = u0.recv(2048)
                assert len(pong0) == 28 and pong0[5] == PKT_PONG, f"Expected PKT_PONG, got {pong0!r}"

                # Bind both sockets -> BIND_ACK + MATCH_START
                u0.sendto(pack_rmxp(PKT_BIND, rid, tok0, slot=slot0, frame_or_seq=1), addr)
                u1.sendto(pack_rmxp(PKT_BIND, rid, tok1, slot=slot1, frame_or_seq=1), addr)
                time.sleep(0.05)
                b0 = [p[5] for p in drain_udp(u0)]
                b1 = [p[5] for p in drain_udp(u1)]
                assert PKT_BIND_ACK in b0 and PKT_MATCH_START in b0, f"u0 missing bind/start: {b0}"
                assert PKT_BIND_ACK in b1 and PKT_MATCH_START in b1, f"u1 missing bind/start: {b1}"

                # Fuzz packets that MUST be silently dropped
                fuzz_pkts = [
                    ("bad_magic", pack_rmxp(PKT_PING, rid, tok0, slot=slot0, magic=b"XXXX")),
                    ("ver_2", pack_rmxp(PKT_PING, rid, tok0, version=2, slot=slot0)),
                    ("ver_0", pack_rmxp(PKT_PING, rid, tok0, version=0, slot=slot0)),
                    ("wrong_token", pack_rmxp(PKT_PING, rid, tok0 ^ 0xDEADBEEF, slot=slot0)),
                    ("short_hdr_12", b"RMXP\x01\x04\x00\x00\x00\x00\x00\x01"),
                    ("trunc_payload", pack_rmxp(PKT_BUILD_SYNC, rid, tok0, slot=slot0, declared_len=64)),
                    ("build_short_100", pack_rmxp(PKT_BUILD_SYNC, rid, tok0, slot=slot0, payload=b"\x00" * 100)),
                    ("build_over_400", pack_rmxp(PKT_BUILD_SYNC, rid, tok0, slot=slot0, payload=b"\x00" * 400)),
                    ("commit_short_20", pack_rmxp(PKT_CUSTOM_COMMIT, rid, tok0, slot=slot0, payload=b"\x00" * 20)),
                    ("commit_over_80", pack_rmxp(PKT_CUSTOM_COMMIT, rid, tok0, slot=slot0, payload=b"\x00" * 80)),
                ]
                for fname, fpkt in fuzz_pkts:
                    u0.sendto(fpkt, addr)
                time.sleep(0.08)
                assert drain_udp(u0) == [] and drain_udp(u1) == [], "Malformed UDP fuzz packet elicited server traffic!"

                # Valid LoN v2 PvpFighterBuild (332 bytes: 261 + 24 + 47 with 'L','N',2 trailer)
                build_v2 = bytearray(332)
                build_v2[285:288] = b"LN\x02"
                u0.sendto(pack_rmxp(PKT_BUILD_SYNC, rid, tok0, slot=slot0, frame_or_seq=1, payload=bytes(build_v2)), addr)
                time.sleep(0.05)
                r0 = [(p[5], len(p) - 28) for p in drain_udp(u0)]
                r1 = [(p[5], len(p) - 28) for p in drain_udp(u1)]
                assert (PKT_BUILD_ACK, 0) in r0 and (PKT_BUILD_SYNC, 332) in r1, f"Valid v2 BUILD_SYNC failed: u0={r0} u1={r1}"

                # Valid LoN v2 PvpCustomCommit (56 bytes: 30 + 26 with 'L','N',2 trailer)
                commit_v2 = bytearray(56)
                commit_v2[30:33] = b"LN\x02"
                u0.sendto(pack_rmxp(PKT_CUSTOM_COMMIT, rid, tok0, slot=slot0, frame_or_seq=2, payload=bytes(commit_v2)), addr)
                time.sleep(0.05)
                r0 = [(p[5], len(p) - 28) for p in drain_udp(u0)]
                r1 = [(p[5], len(p) - 28) for p in drain_udp(u1)]
                assert (PKT_CUSTOM_ACK, 0) in r0 and (PKT_CUSTOM_COMMIT, 56) in r1, f"Valid v2 CUSTOM_COMMIT failed: u0={r0} u1={r1}"

                # Server still alive & healthy after fuzzing
                u1.sendto(pack_rmxp(PKT_PING, rid, tok1, slot=slot1, frame_or_seq=99), addr)
                pong1 = u1.recv(2048)
                assert len(pong1) == 28 and pong1[5] == PKT_PONG, f"Post-fuzz PING failed: {pong1!r}"

                u0.sendto(pack_rmxp(PKT_DISCONNECT, rid, tok0, slot=slot0), addr)
                u1.sendto(pack_rmxp(PKT_DISCONNECT, rid, tok1, slot=slot1), addr)
                print("  -> LoN UDP auth, magic/version/token/truncation fuzz & v2 wire bounds PASSED!", flush=True)
            finally:
                u0.close()
                u1.close()

            # 4. Shared controls.cfg (identical player_uid, NO --uid flag) regressions
            shared_cfg = os.path.join(td, "shared.cfg")
            with open(shared_cfg, "w", encoding="utf-8") as f:
                f.write(
                    "online_mode=1\n"
                    f"server_url=127.0.0.1:{port}\n"
                    "player_uid=NV100B2A\n"
                    "player_name=ROCKMAN\n"
                )

            # 4a. Dual --pvp-create sharing shared.cfg -> must yield 2 distinct open rooms
            env_sh = os.environ.copy()
            env_sh["SDL_VIDEODRIVER"] = "dummy"
            env_sh["SDL_AUDIODRIVER"] = "dummy"
            c_procs = []
            try:
                for idx in range(2):
                    p = subprocess.Popen(
                        [
                            LON_JP_BIN, "--headless", "--fast-forward",
                            "--config", shared_cfg,
                            "--sav", os.path.join(td, f"shared_c{idx}.sav"),
                            "--pvp-create",
                        ],
                        cwd=ROOT,
                        env=env_sh,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                    c_procs.append(p)
                open_ids = []
                for _ in range(160):
                    for p in c_procs:
                        assert p.poll() is None, f"Shared-cfg creator exited unexpectedly (rc={p.returncode})"
                    st, rdata = http_json("GET", f"http://127.0.0.1:{port}/pvp/rooms?game=lon")
                    cur_ids = sorted(int(r["room_id"]) for r in rdata.get("rooms", [])) if st == 200 else []
                    if len(cur_ids) >= 2:
                        open_ids = cur_ids
                        break
                    time.sleep(0.05)
                assert len(open_ids) == 2 and len(set(open_ids)) == 2, (
                    f"Expected 2 distinct open rooms from shared controls.cfg, got {open_ids}"
                )
            finally:
                for p in c_procs:
                    if p.poll() is None:
                        p.terminate()
                        try:
                            p.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            p.kill()
                            p.wait()
            for _ in range(60):
                st, rdata = http_json("GET", f"http://127.0.0.1:{port}/pvp/rooms?game=lon")
                if st == 200 and not rdata.get("rooms"):
                    break
                time.sleep(0.05)
            st, rdata = http_json("GET", f"http://127.0.0.1:{port}/pvp/rooms?game=lon")
            assert st == 200 and not rdata.get("rooms"), f"Shared-cfg rooms lingered after exit: {rdata}"

            # 4b. Shared controls.cfg --pvp-create + --pvp-join 0 full live match (NO --uid)
            p0_log_p = os.path.join(td, "shared_p0.log")
            p1_log_p = os.path.join(td, "shared_p1.log")
            with open(p0_log_p, "w", encoding="utf-8") as lf0, open(p1_log_p, "w", encoding="utf-8") as lf1:
                sp0 = subprocess.Popen(
                    [
                        LON_JP_BIN, "--headless", "--fast-forward",
                        "--config", shared_cfg,
                        "--sav", os.path.join(td, "shared_m0.sav"),
                        "--pvp-auto-bot", "--pvp-create",
                    ],
                    cwd=ROOT, env=env_sh, stdout=lf0, stderr=subprocess.STDOUT,
                )
                created_ok = False
                for _ in range(160):
                    assert sp0.poll() is None, f"Shared-cfg match host exited prematurely (rc={sp0.returncode})"
                    st, rdata = http_json("GET", f"http://127.0.0.1:{port}/pvp/rooms?game=lon")
                    if st == 200 and rdata.get("rooms"):
                        created_ok = True
                        break
                    time.sleep(0.05)
                assert created_ok, "Timed out waiting for shared-cfg host room"

                sp1 = subprocess.Popen(
                    [
                        LON_EN_BIN, "--headless", "--fast-forward",
                        "--config", shared_cfg,
                        "--sav", os.path.join(td, "shared_m1.sav"),
                        "--pvp-auto-bot", "--pvp-join", "0",
                    ],
                    cwd=ROOT, env=env_sh, stdout=lf1, stderr=subprocess.STDOUT,
                )
                try:
                    t_end = time.monotonic() + 90.0
                    while sp0.poll() is None or sp1.poll() is None:
                        if time.monotonic() > t_end:
                            raise TimeoutError("Shared controls.cfg LoN match timed out")
                        time.sleep(0.05)
                    assert sp0.returncode == 0 and sp1.returncode == 0, (
                        f"Shared controls.cfg match failed rc0={sp0.returncode} rc1={sp1.returncode}"
                    )
                finally:
                    for p in (sp0, sp1):
                        if p.poll() is None:
                            p.kill()
                            p.wait()
            txt0 = open(p0_log_p, "r", encoding="utf-8", errors="replace").read()
            txt1 = open(p1_log_p, "r", encoding="utf-8", errors="replace").read()
            sr0 = parse_result(txt0, "shared-cfg P0")
            sr1 = parse_result(txt1, "shared-cfg P1")
            assert sr0["desyncs"] == 0 and sr1["desyncs"] == 0, f"Shared-cfg desync: {sr0} vs {sr1}"
            assert sr0["winner"] == sr1["winner"] and sr0["turns"] == sr1["turns"], (
                f"Shared-cfg result mismatch: {sr0} vs {sr1}"
            )
            print(
                f"  -> Shared controls.cfg dual-create ({open_ids}) & full live match "
                f"(winner={sr0['winner']}, turns={sr0['turns']}, desyncs=0) PASSED!",
                flush=True,
            )
        finally:
            stop_server(srv)


def parse_result(text: str, label: str) -> dict:
    m = RESULT_RE.search(text)
    assert m is not None, f"{label} missing [PvP BOT RESULT] line:\n{text}"
    return {
        "local_idx": int(m.group(1)),
        "winner": int(m.group(2)),
        "turns": int(m.group(3)),
        "rounds": int(m.group(4)),
        "hp0": int(m.group(5)),
        "hp1": int(m.group(6)),
        "desyncs": int(m.group(7)),
        "bo3": int(m.group(8)) if m.group(8) is not None else 0,
        "games": int(m.group(9)) if m.group(9) is not None else 1,
        "w0": int(m.group(10)) if m.group(10) is not None else 0,
        "w1": int(m.group(11)) if m.group(11) is not None else 0,
    }


def run_live_udp_match(
    tag: str,
    title: str,
    bin0: str,
    bin1: str,
    *,
    bo3: bool = False,
    realtime: bool = False,
    seed_sav_p0: list[int] | None = None,
    seed_sav_p1: list[int] | None = None,
    inject_forged_udp: bool = False,
    timeout_s: float = 90.0,
) -> None:
    print(f"\n=== [{tag}] Live UDP Match: {title} ===", flush=True)
    port = alloc_free_port()
    with tempfile.TemporaryDirectory(prefix=f"lon_m_{tag}_") as td:
        srv_log = os.path.join(td, "srv.log")
        srv = start_server(port, td, srv_log)
        p0 = p1 = None
        p0_log_f = p1_log_f = None
        try:
            sav0 = os.path.join(td, "p0.sav")
            sav1 = os.path.join(td, "p1.sav")
            if seed_sav_p0 is not None:
                sp0 = LON_EN_SP if bin0 == LON_EN_BIN else LON_JP_SP
                seed_lon_sav(sp0, sav0, dell_time_vals=seed_sav_p0)
            if seed_sav_p1 is not None:
                sp1 = LON_EN_SP if bin1 == LON_EN_BIN else LON_JP_SP
                seed_lon_sav(sp1, sav1, dell_time_vals=seed_sav_p1)

            env = os.environ.copy()
            if realtime:
                env["SDL_VIDEODRIVER"] = "dummy"
                env["SDL_AUDIODRIVER"] = "dummy"

            common_mode = ["--scale", "1"] if realtime else ["--headless", "--fast-forward"]
            cmd0 = [
                bin0, *common_mode,
                "--config", os.path.join(td, "p0.cfg"),
                "--sav", sav0,
                "--server", f"127.0.0.1:{port}",
                "--uid", "1001", "--name", "LonP0",
                "--pvp-auto-bot", "--pvp-create",
            ]
            if bo3:
                cmd0.append("--pvp-bo3")

            p0_log_path = os.path.join(td, "p0.log")
            p1_log_path = os.path.join(td, "p1.log")
            p0_log_f = open(p0_log_path, "w", encoding="utf-8")
            p0 = subprocess.Popen(cmd0, cwd=ROOT, env=env, stdout=p0_log_f, stderr=subprocess.STDOUT)

            room_id = None
            t_start = time.monotonic()
            for _ in range(160):
                if p0.poll() is not None:
                    raise RuntimeError(f"P0 exited prematurely ({p0.returncode})")
                st, data = http_json("GET", f"http://127.0.0.1:{port}/pvp/rooms?game=lon")
                rooms = data.get("rooms", []) if st == 200 else []
                if rooms:
                    room_id = int(rooms[0]["room_id"])
                    break
                time.sleep(0.05)
            assert room_id is not None, f"[{tag}] Timed out waiting for LoN room creation"

            cmd1 = [
                bin1, *common_mode,
                "--config", os.path.join(td, "p1.cfg"),
                "--sav", sav1,
                "--server", f"127.0.0.1:{port}",
                "--uid", "1002", "--name", "LonP1",
                "--pvp-auto-bot", "--pvp-join", str(room_id),
            ]
            if bo3:
                cmd1.append("--pvp-bo3")

            p1_log_f = open(p1_log_path, "w", encoding="utf-8")
            p1 = subprocess.Popen(cmd1, cwd=ROOT, env=env, stdout=p1_log_f, stderr=subprocess.STDOUT)

            t_end0 = t_end1 = None
            fuzz_sock = None
            srv_fuzz_cnt = cli_fuzz_cnt = 0
            p0_ports: list[int] = []
            p1_ports: list[int] = []
            next_fuzz_t = time.monotonic() + 0.35

            if inject_forged_udp:
                fuzz_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

            try:
                while t_end0 is None or t_end1 is None:
                    now = time.monotonic()
                    if now - t_start > timeout_s:
                        raise TimeoutError(f"[{tag}] Match timed out after {timeout_s:.1f}s")
                    if t_end0 is None and p0.poll() is not None:
                        t_end0 = now
                    if t_end1 is None and p1.poll() is not None:
                        t_end1 = now

                    if inject_forged_udp and fuzz_sock is not None and now >= next_fuzz_t and (t_end0 is None or t_end1 is None):
                        if not p0_ports and p0.poll() is None:
                            p0_ports = find_proc_udp_ports(p0.pid)
                        if not p1_ports and p1.poll() is None:
                            p1_ports = find_proc_udp_ports(p1.pid)
                        forged_pkts = [
                            pack_rmxp(PKT_FRAME_INPUT, room_id, 0xBAD0F00D, slot=1, frame_or_seq=5, keypad_mask=0xFFFF),
                            pack_rmxp(PKT_FRAME_INPUT, room_id, 1, version=2, slot=1, frame_or_seq=5, keypad_mask=0xFFFF),
                            pack_rmxp(PKT_DISCONNECT, room_id, 0xCAFEBABE, magic=b"FAKE", slot=1),
                        ]
                        for fp in forged_pkts:
                            fuzz_sock.sendto(fp, ("127.0.0.1", port))
                            srv_fuzz_cnt += 1
                            for cp in p0_ports:
                                fuzz_sock.sendto(fp, ("127.0.0.1", cp))
                                cli_fuzz_cnt += 1
                            for cp in p1_ports:
                                fuzz_sock.sendto(fp, ("127.0.0.1", cp))
                                cli_fuzz_cnt += 1
                        next_fuzz_t = now + 0.15

                    time.sleep(0.01)
            finally:
                if fuzz_sock is not None:
                    fuzz_sock.close()

            p0_log_f.close(); p0_log_f = None
            p1_log_f.close(); p1_log_f = None
            out0 = open(p0_log_path, "r", encoding="utf-8", errors="replace").read()
            out1 = open(p1_log_path, "r", encoding="utf-8", errors="replace").read()
            dt = max(t_end0, t_end1) - t_start
            skew = abs(t_end0 - t_end1)

            if inject_forged_udp:
                print(f"  Forged UDP injected: srv_pkts={srv_fuzz_cnt}, cli_pkts={cli_fuzz_cnt}, p0_ports={p0_ports}, p1_ports={p1_ports}")
            for line in out0.splitlines():
                if "[PvP BOT RESULT]" in line:
                    print(f"  P0: {line}")
            for line in out1.splitlines():
                if "[PvP BOT RESULT]" in line:
                    print(f"  P1: {line}")

            assert p0.returncode == 0 and p1.returncode == 0, (
                f"[{tag}] Non-zero exit rc0={p0.returncode} rc1={p1.returncode}\n--- P0 ---\n{out0}\n--- P1 ---\n{out1}"
            )
            for who, txt in [("P0", out0), ("P1", out1)]:
                assert "[PvP DESYNC]" not in txt, f"[{tag}] {who} logged [PvP DESYNC]:\n{txt}"
                assert "JavaException" not in txt and "[PvP Exception]" not in txt, (
                    f"[{tag}] {who} logged exception:\n{txt}"
                )

            r0 = parse_result(out0, f"{tag} P0")
            r1 = parse_result(out1, f"{tag} P1")
            assert r0["local_idx"] == 0 and r1["local_idx"] == 1
            assert r0["desyncs"] == 0 and r1["desyncs"] == 0, f"[{tag}] Non-zero desyncs: {r0} vs {r1}"
            keys = ("winner", "turns", "rounds", "hp0", "hp1", "bo3", "games", "w0", "w1")
            for k in keys:
                assert r0[k] == r1[k], f"[{tag}] Result mismatch on {k}: P0={r0} vs P1={r1}"

            if bo3:
                assert r0["bo3"] == 1 and r0["games"] in (2, 3), f"[{tag}] Unexpected BO3 metadata: {r0}"
                if r0["games"] == 2:
                    assert max(r0["w0"], r0["w1"]) == 2
            else:
                assert r0["bo3"] == 0 and r0["games"] == 1

            if realtime:
                assert skew <= 0.75, f"[{tag}] Real-time clock-sync exit skew too large: {skew:.3f}s"
            if inject_forged_udp:
                assert srv_fuzz_cnt > 0 and cli_fuzz_cnt > 0, f"[{tag}] Forged UDP packets failed to inject"

            print(
                f"  -> {title} PASSED in {dt:.2f}s "
                f"(room #{room_id}, winner={r0['winner']}, turns={r0['turns']}, rounds={r0['rounds']}, "
                f"bo3={r0['bo3']}, games={r0['games']}, score={r0['w0']}-{r0['w1']}, exit_skew={skew:.3f}s, desyncs=0)!",
                flush=True,
            )
        finally:
            if p0_log_f is not None:
                p0_log_f.close()
            if p1_log_f is not None:
                p1_log_f.close()
            for proc in (p0, p1):
                if proc is not None and proc.poll() is None:
                    proc.kill()
                    proc.wait()
            stop_server(srv)


def main() -> int:
    run_self_test("LoN JP", LON_JP_BIN)
    run_self_test("LoN EN", LON_EN_BIN)
    run_isolation_and_security_tests()

    run_live_udp_match("2.1", "LoN JP vs LoN JP (Single)", LON_JP_BIN, LON_JP_BIN)
    run_live_udp_match("2.2", "LoN EN vs LoN EN (Single)", LON_EN_BIN, LON_EN_BIN)
    run_live_udp_match("2.3", "LoN JP vs LoN EN (Cross-Region Single)", LON_JP_BIN, LON_EN_BIN)
    run_live_udp_match("2.4", "LoN EN vs LoN JP (Reverse Cross-Region Single)", LON_EN_BIN, LON_JP_BIN)
    run_live_udp_match(
        "2.5",
        "LoN JP vs LoN EN (BO3 Triple Battle + Trance + Asymmetric dell_time)",
        LON_JP_BIN,
        LON_EN_BIN,
        bo3=True,
        seed_sav_p0=[5] * 10,
        seed_sav_p1=[0] * 10,
        timeout_s=120.0,
    )
    run_live_udp_match(
        "2.6",
        "LoN JP vs LoN EN (Real-Time 16Hz Clock-Sync + Forged UDP Guard)",
        LON_JP_BIN,
        LON_EN_BIN,
        realtime=True,
        inject_forged_udp=True,
        timeout_s=240.0,
    )

    print("\n" + "=" * 68)
    print("ALL LoN PvP SELF-TESTS & LIVE CROSS-REGION UDP MATCHES PASSED (0 DESYNCS)!")
    print("=" * 68)
    return 0


if __name__ == "__main__":
    sys.exit(main())
