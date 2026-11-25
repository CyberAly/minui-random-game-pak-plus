#!/usr/bin/env python3
"""End-to-end tests for ra-helper and launch.sh RetroAchievements integration."""

import hashlib
import http.server
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import urllib.parse
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def write_nextui_cache_bin(path: Path, body_obj: dict) -> None:
    body = json.dumps(body_obj).encode("utf-8")
    digest = hashlib.sha256(body).digest()
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(struct.pack("<I", len(body)))
        f.write(body)
        f.write(digest)


def pack_ledger_record(rec_type: int, timestamp: int, game_id: int,
                       ach_id: int, hardcore: int, game_hash: str) -> bytes:
    hash_bytes = game_hash.encode("ascii")[:32].ljust(33, b"\x00")
    prev_hash = b"\x00" * 32
    rec_hash = b"\x00" * 32
    rec = struct.pack("<BIIIB33s32s32s",
                      rec_type, timestamp, game_id, ach_id, hardcore,
                      hash_bytes, prev_hash, rec_hash)
    assert len(rec) == 111
    return rec


def parse_kv(output: str) -> dict:
    res = {}
    for line in output.strip().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            res[k.strip()] = v.strip()
    return res


class MockRAHandler(http.server.BaseHTTPRequestHandler):
    routes = {}
    requests_seen = []

    def log_message(self, format, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length).decode("utf-8")
        params = urllib.parse.parse_qs(raw, keep_blank_values=True)
        flat = {k: v[0] for k, v in params.items()}
        self.__class__.requests_seen.append(flat)

        req_type = flat.get("r", "")
        assert req_type != "startsession", "ra-helper must NEVER call r=startsession!"

        handler = self.__class__.routes.get(req_type)
        if not handler:
            self.send_response(404)
            self.end_headers()
            return

        status, payload = handler(flat)
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def run_helper(cmd_prefix, args, env=None):
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    proc = subprocess.run(
        cmd_prefix + args,
        capture_output=True,
        text=True,
        env=merged_env,
        timeout=15,
    )
    return proc.returncode, parse_kv(proc.stdout), proc.stdout, proc.stderr


def run_suite_for_binary(label: str, cmd_prefix: list) -> None:
    print(f"=== Testing {label}: {' '.join(cmd_prefix)} ===")

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockRAHandler)
    port = server.server_address[1]
    api_url = f"http://127.0.0.1:{port}/dorequest.php"
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    tmpdir = Path(tempfile.mkdtemp(prefix="ra_helper_test_"))
    try:
        sdcard = tmpdir / "SDCARD"
        cache_dir = tmpdir / "pak_cache"
        roms = sdcard / "Roms"
        settings_path = sdcard / ".userdata" / "shared" / "minuisettings.txt"
        settings_path.parent.mkdir(parents=True, exist_ok=True)

        # 1. check-config gating
        rc, kv, _, _ = run_helper(cmd_prefix, ["check-config", "--sdcard", str(sdcard)])
        assert rc == 1 and kv["enabled"] == "0", f"Missing settings failed: {rc} {kv}"

        settings_path.write_text("raEnable=0\nraAuthenticated=1\nraUsername=alice\nraToken=tok123\n")
        rc, kv, _, _ = run_helper(cmd_prefix, ["check-config", "--sdcard", str(sdcard)])
        assert rc == 1 and kv["enabled"] == "0", f"Disabled RA failed: {rc} {kv}"

        settings_path.write_text("raEnable=1\nraAuthenticated=0\nraUsername=alice\nraToken=tok123\n")
        rc, kv, _, _ = run_helper(cmd_prefix, ["check-config", "--sdcard", str(sdcard)])
        assert rc == 1 and kv["enabled"] == "0", f"Unauthenticated RA failed: {rc} {kv}"

        settings_path.write_text("raEnable=1\nraAuthenticated=1\nraUsername=alice\nraToken=\n")
        rc, kv, _, _ = run_helper(cmd_prefix, ["check-config", "--sdcard", str(sdcard)])
        assert rc == 1 and kv["enabled"] == "0", f"Empty token failed: {rc} {kv}"

        settings_path.write_text("raEnable=1\nraAuthenticated=1\nraUsername=alice\nraToken=tok123\n")
        rc, kv, _, _ = run_helper(cmd_prefix, ["check-config", "--sdcard", str(sdcard)])
        assert rc == 0 and kv["enabled"] == "1", f"Valid config failed: {rc} {kv}"

        # Create test ROMs
        gba_dir = roms / "Game Boy Advance (GBA)"
        nes_dir = roms / "Nintendo (FC)"
        fbn_dir = roms / "Arcade (FBN)"
        gba_dir.mkdir(parents=True, exist_ok=True)
        nes_dir.mkdir(parents=True, exist_ok=True)
        fbn_dir.mkdir(parents=True, exist_ok=True)

        gba_bytes = b"TEST_GBA_ROM_DATA" * 1024
        gba_hash = hashlib.md5(gba_bytes).hexdigest()
        gba_rom = gba_dir / "Cool Game (USA).gba"
        gba_rom.write_bytes(gba_bytes)

        # Zipped GBA ROM with an extra .txt file (should hash identically to gba_rom!)
        gba_zip = gba_dir / "Cool Game Zipped.zip"
        with zipfile.ZipFile(gba_zip, "w") as zf:
            zf.writestr("readme.txt", "hello world")
            zf.writestr("Cool Game.gba", gba_bytes)

        # NES ROM with 16-byte iNES header (rcheevos strips the 16-byte header)
        nes_payload = b"NES_PRG_CHR_PAYLOAD" * 1024
        nes_header = b"NES\x1a" + b"\x02\x01" + b"\x00" * 10
        nes_rom = nes_dir / "Super Nes Test (USA).nes"
        nes_rom.write_bytes(nes_header + nes_payload)
        expected_nes_hash = hashlib.md5(nes_payload).hexdigest()

        # Unsupported ROM (no RA match)
        unknown_rom = gba_dir / "Homebrew Hack.gba"
        unknown_bytes = b"UNKNOWN_HOMEBREW_DATA" * 512
        unknown_hash = hashlib.md5(unknown_bytes).hexdigest()
        unknown_rom.write_bytes(unknown_bytes)

        # Game on RA with 0 official achievements
        no_ach_rom = gba_dir / "No Achievements Yet.gba"
        no_ach_bytes = b"NO_ACHIEVEMENTS_GAME" * 512
        no_ach_hash = hashlib.md5(no_ach_bytes).hexdigest()
        no_ach_rom.write_bytes(no_ach_bytes)

        # Incompatible ROM (virtual game ID > 1000000000 from RA)
        incompat_rom = gba_dir / "Wrong Region (Japan).gba"
        incompat_bytes = b"INCOMPATIBLE_REGION_ROM" * 512
        incompat_hash = hashlib.md5(incompat_bytes).hexdigest()
        incompat_rom.write_bytes(incompat_bytes)

        # Arcade ROM (FBN hashes filename without extension: "mslug")
        fbn_rom = fbn_dir / "mslug.zip"
        fbn_rom.write_bytes(b"fake zip contents")
        expected_fbn_hash = hashlib.md5(b"mslug").hexdigest()

        # Configure mock RA server (including realistic CLIENT_WARNING_ID 101000001 injection)
        def handle_gameid(p):
            m = p.get("m", "")
            if m == gba_hash:
                return 200, {"Success": True, "GameID": 1001}
            if m == no_ach_hash:
                return 200, {"Success": True, "GameID": 2002}
            if m == expected_nes_hash:
                return 200, {"Success": True, "GameID": 3003}
            if m == expected_fbn_hash:
                return 200, {"Success": True, "GameID": 4004}
            if m == incompat_hash:
                return 200, {"Success": True, "GameID": 1000007777}
            return 200, {"Success": True, "GameID": 0}

        def handle_patch(p):
            if p.get("t") != "tok123":
                return 200, {"Success": False, "Code": "invalid_credentials", "Error": "Invalid User/Token"}
            g = int(p.get("g", "0"))
            assert g <= 1000000000, f"Should not fetch patch for virtual game ID {g}"
            if g == 1001:
                return 200, {
                    "Success": True,
                    "PatchData": {
                        "ID": 1001,
                        "Title": "Cool Game",
                        "Achievements": [
                            {"ID": 101, "Flags": 3, "Title": "A1"},
                            {"ID": 102, "Flags": 3, "Title": "A2"},
                            {"ID": 103, "Flags": 3, "Title": "A3"},
                            {"ID": 104, "Flags": 3, "Title": "A4"},
                            {"ID": 999, "Flags": 5, "Title": "Unofficial"},
                            {"ID": 101000001, "Flags": 3, "Title": "Warning: Unknown Emulator"},
                        ],
                    },
                }
            if g == 2002:
                return 200, {
                    "Success": True,
                    "PatchData": {
                        "ID": 2002,
                        "Title": "No Achievements Yet",
                        "Achievements": [
                            {"ID": 888, "Flags": 5, "Title": "Only Unofficial"},
                            {"ID": 101000001, "Flags": 3, "Title": "Warning: Unknown Emulator"},
                        ],
                    },
                }
            return 200, {
                "Success": True,
                "PatchData": {
                    "ID": g,
                    "Achievements": [
                        {"ID": 501, "Flags": 3},
                        {"ID": 101000001, "Flags": 3, "Title": "Warning: Unknown Emulator"},
                    ],
                },
            }

        def handle_unlocks(p):
            if p.get("t") != "tok123":
                return 401, {"Success": False, "Error": "Unauthorized"}
            assert p.get("h") == "0", f"Expected softcore h=0, got {p}"
            g = int(p.get("g", "0"))
            assert g <= 1000000000, f"Should not fetch unlocks for virtual game ID {g}"
            if g == 1001:
                # Realistic RAWeb softcore unlocks include CLIENT_WARNING_ID (101000001)
                return 200, {"Success": True, "UserUnlocks": [101, 102, 999, 101000001]}
            return 200, {"Success": True, "UserUnlocks": [101000001]}

        MockRAHandler.routes = {
            "gameid": handle_gameid,
            "patch": handle_patch,
            "unlocks": handle_unlocks,
        }
        MockRAHandler.requests_seen.clear()

        # 2. Online lookup: GBA ROM (not in NextUI cache -> offline_ready=0)
        rc, kv, _, err = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(cache_dir),
             "--rom", str(gba_rom), "--emu", "GBA"],
            env={"RA_API_URL": api_url},
        )
        assert rc == 0, err
        assert kv == {
            "status": "ok",
            "hash": gba_hash,
            "game_id": "1001",
            "earned": "2",
            "total": "4",
            "pending": "0",
            "offline_ready": "0",
            "source": "online",
        }, f"Unexpected GBA online result: {kv}"

        # Ensure NextUI .ra/offline was NOT created/written
        assert not (sdcard / ".userdata" / "shared" / ".ra").exists()

        # 3. Offline lookup from pak cache (still offline_ready=0 -> WiFi Required for RA)
        rc, kv, _, _ = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(cache_dir),
             "--rom", str(gba_rom), "--emu", "GBA", "--offline-only"],
            env={"RA_API_URL": "http://127.0.0.1:1/unreachable"},
        )
        assert rc == 0
        assert kv["status"] == "ok" and kv["earned"] == "2" and kv["total"] == "4"
        assert kv["offline_ready"] == "0" and kv["source"] == "pak_cache", kv

        # 4. Zipped GBA ROM hashes identically and hits pak cache!
        rc, kv, _, _ = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(cache_dir),
             "--rom", str(gba_zip), "--emu", "GBA", "--offline-only"],
        )
        assert kv["hash"] == gba_hash and kv["status"] == "ok" and kv["earned"] == "2", kv

        # 5. NES header stripping & Arcade filename hashing (earned must be 0 despite 101000001 in UserUnlocks!)
        rc, kv, _, _ = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(cache_dir),
             "--rom", str(nes_rom), "--emu", "FC"],
            env={"RA_API_URL": api_url},
        )
        assert kv["hash"] == expected_nes_hash and kv["game_id"] == "3003" and kv["earned"] == "0" and kv["total"] == "1", kv

        rc, kv, _, _ = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(cache_dir),
             "--rom", str(fbn_rom), "--emu", "FBN"],
            env={"RA_API_URL": api_url},
        )
        assert kv["hash"] == expected_fbn_hash and kv["game_id"] == "4004" and kv["earned"] == "0" and kv["total"] == "1", kv

        # 6. Game matched on RA with 0 official achievements (only unofficial + 101000001) -> status=none
        rc, kv, _, _ = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(cache_dir),
             "--rom", str(no_ach_rom), "--emu", "GBA"],
            env={"RA_API_URL": api_url},
        )
        assert kv["status"] == "none" and kv["game_id"] == "2002" and kv["total"] == "0", kv

        # 6b. Incompatible / wrong-region ROM (virtual game ID > 1000000000) -> status=incompatible (not 0/1!)
        rc, kv, _, _ = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(cache_dir),
             "--rom", str(incompat_rom), "--emu", "GBA"],
            env={"RA_API_URL": api_url},
        )
        assert kv["status"] == "incompatible" and kv["game_id"] == "1000007777" and kv["total"] == "0", kv

        # 6c. Stale pak cache containing 101000001 self-heals on read without needing cache wipe
        stale_kv = cache_dir / "games" / f"{gba_hash}.kv"
        stale_kv.write_text(
            "game_id=1001\ntotal=5\nearned=1\nmeta_ts=4102444800\nunlocks_ts=4102444800\n"
            "core_ids=101,102,103,104,101000001\nunlock_ids=101000001\n"
        )
        rc, kv, _, _ = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(cache_dir),
             "--rom", str(gba_rom), "--emu", "GBA", "--offline-only"],
        )
        assert kv["status"] == "ok" and kv["earned"] == "0" and kv["total"] == "4", kv

        # 7. Unmatched hash -> status=unsupported
        rc, kv, _, _ = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(cache_dir),
             "--rom", str(unknown_rom), "--emu", "GBA"],
            env={"RA_API_URL": api_url},
        )
        assert kv["status"] == "unsupported" and kv["game_id"] == "0", kv

        # 8. Explicitly unsupported emulator -> status=unsupported without network
        rc, kv, _, _ = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(cache_dir),
             "--rom", str(gba_rom), "--emu", "PRBOOM"],
            env={"RA_API_URL": "http://127.0.0.1:1/unreachable"},
        )
        assert kv["status"] == "unsupported", kv

        # 9. Uncached + offline (unreachable server) -> status=wifi_required
        fresh_rom = gba_dir / "Never Checked.gba"
        fresh_rom.write_bytes(b"BRAND_NEW_ROM_BYTES" * 512)
        rc, kv, _, _ = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(cache_dir),
             "--rom", str(fresh_rom), "--emu", "GBA"],
            env={"RA_API_URL": "http://127.0.0.1:1/unreachable"},
        )
        assert kv["status"] == "wifi_required" and kv["offline_ready"] == "0", kv

        # 10. NextUI offline cache + ledger.bin (strictly read-only, completely offline)
        ra_offline_cache = sdcard / ".userdata" / "shared" / ".ra" / "offline" / "cache"
        ra_ledger = sdcard / ".userdata" / "shared" / ".ra" / "offline" / "ledger.bin"
        fresh_hash = kv["hash"]

        write_nextui_cache_bin(ra_offline_cache / "login.bin", {"Success": True, "User": "alice"})
        write_nextui_cache_bin(ra_offline_cache / f"gameid_{fresh_hash}.bin", {"Success": True, "GameID": 5555})
        write_nextui_cache_bin(
            ra_offline_cache / f"achievementsets_{fresh_hash}.bin",
            {
                "Success": True,
                "GameId": 5555,
                "Sets": [
                    {
                        "AchievementSetId": 1,
                        "GameId": 5555,
                        "Type": "core",
                        "Achievements": [
                            {"ID": 10, "Flags": 3},
                            {"ID": 11, "Flags": 3},
                            {"ID": 12, "Flags": 3},
                            {"ID": 13, "Flags": 3},
                            {"ID": 14, "Flags": 5},  # unofficial
                            {"ID": 101000001, "Flags": 3},  # client warning, ignored
                        ],
                    },
                    {
                        "AchievementSetId": 2,
                        "GameId": 5555,
                        "Type": "bonus",
                        "Achievements": [{"ID": 99, "Flags": 3}],  # bonus subset, ignored
                    },
                ],
            },
        )
        write_nextui_cache_bin(
            ra_offline_cache / f"startsession_{fresh_hash}.bin",
            {
                "Success": True,
                "Unlocks": [{"ID": 10, "When": 1700000000}, {"ID": 101000001, "When": 1700000002}],
                "HardcoreUnlocks": [{"ID": 11, "When": 1700000001}],
            },
        )
        # Ledger: unlock 12 (pending), unlock 13 + sync_ack 13 (confirmed), unlock 10 (already in session)
        with open(ra_ledger, "wb") as lf:
            lf.write(pack_ledger_record(0x02, 1700000100, 5555, 12, 0, fresh_hash))
            lf.write(pack_ledger_record(0x02, 1700000101, 5555, 13, 0, fresh_hash))
            lf.write(pack_ledger_record(0x04, 1700000102, 5555, 13, 0, fresh_hash))

        rc, kv, _, _ = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(cache_dir),
             "--rom", str(fresh_rom), "--emu", "GBA", "--offline-only"],
        )
        assert kv == {
            "status": "ok",
            "hash": fresh_hash,
            "game_id": "5555",
            "earned": "3",  # 10 (softcore) + 11 (hardcore) + 12 (pending offline)
            "total": "4",   # 10, 11, 12, 13 (excludes unofficial 14, bonus 99, warning 101000001)
            "pending": "1", # only 12 is still unsynced (13 was acked)
            "offline_ready": "1",
            "source": "nextui_cache",
        }, f"Unexpected NextUI offline cache result: {kv}"

        # 11. Invalid credentials -> status=auth_invalid
        settings_path.write_text("raEnable=1\nraAuthenticated=1\nraUsername=alice\nraToken=BAD_TOKEN\n")
        bad_cache_dir = tmpdir / "bad_auth_cache"
        rc, kv, _, _ = run_helper(
            cmd_prefix,
            ["lookup", "--sdcard", str(sdcard), "--cache-dir", str(bad_cache_dir),
             "--rom", str(gba_rom), "--emu", "GBA"],
            env={"RA_API_URL": api_url},
        )
        assert kv["status"] == "auth_invalid", f"Expected auth_invalid, got {kv}"

        print(f"  PASS ({label})")
    finally:
        server.shutdown()
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_launch_sh_formatting() -> None:
    print("=== Testing launch.sh syntax & RA display formatting ===")
    subprocess.run(["sh", "-n", str(REPO_ROOT / "launch.sh")], check=True)
    subprocess.run(["bash", "-n", str(REPO_ROOT / "scripts/deploy.sh")], check=True)

    # Extract and exercise lookup_retroachievements formatting with mocked ra-helper
    tmpdir = Path(tempfile.mkdtemp(prefix="launch_sh_test_"))
    try:
        fake_bin = tmpdir / "bin"
        fake_bin.mkdir()
        cases = [
            ("status=ok\nearned=34\ntotal=50\npending=0\noffline_ready=1", r"RA: 34/50\nRA Offline Ready", "1"),
            ("status=ok\nearned=36\ntotal=50\npending=2\noffline_ready=1", r"RA: 36/50\nRA Offline Ready (2 Unsynced)", "1"),
            ("status=ok\nearned=12\ntotal=40\npending=0\noffline_ready=0", r"RA: 12/40\nWiFi Required for RA", "1"),
            ("status=none\ngame_id=2002\ntotal=0", "RA None", "1"),
            ("status=incompatible\ngame_id=1000007777\ntotal=0", "RA Incompatible ROM", "1"),
            ("status=unsupported\ngame_id=0", "RA Unsupported", "1"),
            ("status=wifi_required", "WiFi Required for RA", "1"),
            ("status=auth_invalid", "", "0"),
            ("status=disabled", "", "1"),
        ]

        # Build a tiny harness that sources only the functions from launch.sh
        launch_src = (REPO_ROOT / "launch.sh").read_text()
        # Strip top-level execution lines (set -x, exec redirect, cd, main "$@")
        func_start = launch_src.index("is_nextui()")
        func_end = launch_src.index("build_rom_list()")
        funcs = launch_src[func_start:func_end]

        for idx, (mock_out, expected_display, expected_enabled) in enumerate(cases):
            helper = fake_bin / "ra-helper"
            helper.write_text(f"#!/bin/sh\nprintf '%s\\n' '{mock_out}'\n")
            helper.chmod(0o755)

            script = (
                f"PATH='{fake_bin}:$PATH'\n"
                f"SDCARD_PATH='{tmpdir}'\n"
                f"RA_CACHE_DIR='{tmpdir}/cache'\n"
                "RA_ENABLED=1\n"
                "RA_DISPLAY=''\n"
                f"{funcs}\n"
                "lookup_retroachievements '/rom.gba' 'GBA'\n"
                "printf 'DISPLAY=%s\\nENABLED=%s\\n' \"$RA_DISPLAY\" \"$RA_ENABLED\"\n"
            )
            proc = subprocess.run(["sh", "-c", script], capture_output=True, text=True, check=True)
            lines = dict(line.split("=", 1) for line in proc.stdout.splitlines() if "=" in line)
            assert lines["DISPLAY"] == expected_display, f"Case {idx}: got {lines['DISPLAY']!r}, expected {expected_display!r}"
            assert lines["ENABLED"] == expected_enabled, f"Case {idx}: got ENABLED={lines['ENABLED']!r}, expected {expected_enabled!r}"
        print("  PASS (launch.sh formatting & auth disable)")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def main() -> int:
    run_suite_for_binary("host (x86_64)", [str(REPO_ROOT / "src/ra-helper/build/host/ra-helper")])

    arm64_bin = REPO_ROOT / "bin/arm64/ra-helper"
    if arm64_bin.exists() and shutil.which("qemu-aarch64"):
        run_suite_for_binary("arm64 (qemu-aarch64)", ["qemu-aarch64", str(arm64_bin)])

    arm_bin = REPO_ROOT / "bin/arm/ra-helper"
    if arm_bin.exists() and shutil.which("qemu-arm"):
        run_suite_for_binary("arm (qemu-arm)", ["qemu-arm", str(arm_bin)])

    test_launch_sh_formatting()
    print("\nALL TESTS PASSED!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
