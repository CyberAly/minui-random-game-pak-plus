#!/usr/bin/env python3
"""End-to-end tests for the More Options menu, Filters menu, and RA filter shuffle in launch.sh."""

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def write_executable(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def make_fake_presenter(bin_path: Path, state_dir: Path) -> None:
    """Fake minui-presenter that logs every call and pops exit codes for interactive previews (--timeout 0)."""
    script = f"""#!/usr/bin/env python3
import json, sys
from pathlib import Path

state_dir = Path({str(state_dir)!r})
log_file = state_dir / "presenter_log.jsonl"
actions_file = state_dir / "presenter_actions.json"

args = sys.argv[1:]
msg = ""
timeout = None
inaction_btn = None
inaction_text = None
inaction_show = False

i = 0
while i < len(args):
    a = args[i]
    if a == "--message" and i + 1 < len(args):
        msg = args[i + 1]; i += 2
    elif a == "--timeout" and i + 1 < len(args):
        timeout = args[i + 1]; i += 2
    elif a == "--inaction-button" and i + 1 < len(args):
        inaction_btn = args[i + 1]; i += 2
    elif a == "--inaction-text" and i + 1 < len(args):
        inaction_text = args[i + 1]; i += 2
    elif a == "--inaction-show":
        inaction_show = True; i += 1
    else:
        i += 1

entry = {{
    "message": msg,
    "timeout": timeout,
    "inaction_button": inaction_btn,
    "inaction_text": inaction_text,
    "inaction_show": inaction_show,
}}
with open(log_file, "a", encoding="utf-8") as fh:
    fh.write(json.dumps(entry) + "\\n")

if timeout == "-1":
    # Background status message; exit cleanly
    sys.exit(0)
if timeout != "0":
    # Timed toast message; exit immediately in tests
    sys.exit(0)

actions = json.loads(actions_file.read_text(encoding="utf-8"))
if not actions:
    sys.exit(2)
rc = actions.pop(0)
actions_file.write_text(json.dumps(actions), encoding="utf-8")
sys.exit(int(rc))
"""
    write_executable(bin_path, script)


def make_fake_list(bin_path: Path, state_dir: Path) -> None:
    """Fake minui-list that logs title + items and pops scripted menu actions."""
    script = f"""#!/usr/bin/env python3
import json, sys
from pathlib import Path

state_dir = Path({str(state_dir)!r})
log_file = state_dir / "list_log.jsonl"
actions_file = state_dir / "list_actions.json"

args = sys.argv[1:]
title = ""
menu_file = ""
write_loc = ""
fmt = ""

i = 0
while i < len(args):
    a = args[i]
    if a == "--title" and i + 1 < len(args):
        title = args[i + 1]; i += 2
    elif a == "--file" and i + 1 < len(args):
        menu_file = args[i + 1]; i += 2
    elif a == "--write-location" and i + 1 < len(args):
        write_loc = args[i + 1]; i += 2
    elif a == "--format" and i + 1 < len(args):
        fmt = args[i + 1]; i += 2
    else:
        i += 1

items = [line.strip() for line in Path(menu_file).read_text(encoding="utf-8").splitlines() if line.strip()]
with open(log_file, "a", encoding="utf-8") as fh:
    fh.write(json.dumps({{"title": title, "items": items, "format": fmt}}) + "\\n")

actions = json.loads(actions_file.read_text(encoding="utf-8"))
if not actions:
    sys.exit(2)
action = actions.pop(0)
actions_file.write_text(json.dumps(actions), encoding="utf-8")

rc = int(action["rc"])
if rc == 0:
    sel = action["select"]
    if sel not in items:
        sys.stderr.write(f"Requested item {{sel!r}} not in {{items!r}}\\n")
        sys.exit(1)
    if write_loc:
        Path(write_loc).write_text(sel, encoding="utf-8")
sys.exit(rc)
"""
    write_executable(bin_path, script)


def make_fake_ra_helper(bin_path: Path, state_dir: Path, responses: dict, enabled: bool = True) -> None:
    """Fake ra-helper that logs lookups and returns scripted key=val responses by ROM filename."""
    script = f"""#!/usr/bin/env python3
import json, sys
from pathlib import Path

state_dir = Path({str(state_dir)!r})
log_file = state_dir / "ra_log.jsonl"
responses = {json.dumps(responses)}
enabled = {enabled!r}

cmd = sys.argv[1] if len(sys.argv) > 1 else ""
if cmd == "check-config":
    print("enabled=1" if enabled else "enabled=0")
    sys.exit(0 if enabled else 1)

if cmd == "lookup":
    args = sys.argv[2:]
    rom = ""
    emu = ""
    offline_only = False
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--rom" and i + 1 < len(args):
            rom = args[i + 1]; i += 2
        elif a == "--emu" and i + 1 < len(args):
            emu = args[i + 1]; i += 2
        elif a == "--offline-only":
            offline_only = True; i += 1
        else:
            i += 1

    name = Path(rom).name
    with open(log_file, "a", encoding="utf-8") as fh:
        fh.write(json.dumps({{"rom": name, "emu": emu, "offline_only": offline_only}}) + "\\n")

    spec = responses.get(name, {{"status": "unsupported", "total": 0}})
    if isinstance(spec, list):
        seq_file = state_dir / f"ra_seq_{{name}}.idx"
        idx = int(seq_file.read_text()) if seq_file.exists() else 0
        chosen = spec[min(idx, len(spec) - 1)]
        seq_file.write_text(str(idx + 1))
    elif isinstance(spec, dict) and "online" in spec:
        chosen = spec["offline"] if offline_only else spec["online"]
    else:
        chosen = spec

    for k, v in chosen.items():
        print(f"{{k}}={{v}}")
    sys.exit(0)

sys.exit(2)
"""
    write_executable(bin_path, script)


def run_launch_scenario(
    roms: list[str],
    ra_responses: dict,
    presenter_actions: list[int],
    list_actions: list[dict],
    nextui: bool = True,
    ra_enabled: bool = True,
) -> dict:
    tmpdir = Path(tempfile.mkdtemp(prefix="filters_test_"))
    try:
        pak_dir = tmpdir / "Random Game.pak"
        sdcard = tmpdir / "SDCARD"
        logs_dir = tmpdir / "logs"
        userdata = tmpdir / "userdata"
        state_dir = tmpdir / "state"

        for d in [pak_dir, sdcard / "Roms" / "Game Boy Advance (GBA)", logs_dir, userdata, state_dir]:
            d.mkdir(parents=True, exist_ok=True)

        # Copy launch.sh into fake pak dir
        shutil.copy2(REPO_ROOT / "launch.sh", pak_dir / "launch.sh")
        (pak_dir / "launch.sh").chmod(0o755)

        # Create fake NextUI settings if requested
        if nextui:
            settings = sdcard / ".userdata" / "shared" / "minuisettings.txt"
            settings.parent.mkdir(parents=True, exist_ok=True)
            settings.write_text("raEnable=1\nraAuthenticated=1\nraUsername=alice\nraToken=tok\n")

        # Create recents dir
        (sdcard / ".userdata" / "shared" / ".minui").mkdir(parents=True, exist_ok=True)

        # Create fake emulator launcher that records launched ROM
        emu_launch = sdcard / "Emus" / "tg5040" / "GBA.pak" / "launch.sh"
        write_executable(
            emu_launch,
            f"#!/bin/sh\nprintf '%s\\n' \"$1\" > {str(state_dir / 'launched.txt')!r}\nexit 0\n",
        )

        # Create fake ROM files
        rom_dir = sdcard / "Roms" / "Game Boy Advance (GBA)"
        for rom_name in roms:
            (rom_dir / rom_name).write_bytes(b"ROMDATA")

        # Put fake binaries in pak bin dir for platform tg5040 + both arm/arm64
        plat_bin = pak_dir / "bin" / "tg5040"
        make_fake_presenter(plat_bin / "minui-presenter", state_dir)
        make_fake_presenter(plat_bin / "minui-presenter-nextui", state_dir)
        make_fake_list(plat_bin / "minui-list", state_dir)
        make_fake_list(plat_bin / "minui-list-nextui", state_dir)
        for arch in ("arm", "arm64"):
            make_fake_ra_helper(pak_dir / "bin" / arch / "ra-helper", state_dir, ra_responses, enabled=ra_enabled)

        (state_dir / "presenter_actions.json").write_text(json.dumps(presenter_actions), encoding="utf-8")
        (state_dir / "list_actions.json").write_text(json.dumps(list_actions), encoding="utf-8")

        env = os.environ.copy()
        env.update(
            {
                "PLATFORM": "tg5040",
                "SDCARD_PATH": str(sdcard),
                "LOGS_PATH": str(logs_dir),
                "USERDATA_PATH": str(userdata),
            }
        )

        proc = subprocess.run(
            ["sh", str(pak_dir / "launch.sh")],
            env=env,
            capture_output=True,
            text=True,
            timeout=20,
        )

        def read_jsonl(p: Path) -> list[dict]:
            if not p.exists():
                return []
            return [json.loads(line) for line in p.read_text(encoding="utf-8").splitlines() if line.strip()]

        launched = ""
        if (state_dir / "launched.txt").exists():
            launched = (state_dir / "launched.txt").read_text(encoding="utf-8").strip()

        log_text = ""
        log_path = logs_dir / "Random Game.txt"
        if log_path.exists():
            log_text = log_path.read_text(encoding="utf-8")

        return {
            "returncode": proc.returncode,
            "presenter_log": read_jsonl(state_dir / "presenter_log.jsonl"),
            "list_log": read_jsonl(state_dir / "list_log.jsonl"),
            "ra_log": read_jsonl(state_dir / "ra_log.jsonl"),
            "launched": launched,
            "log_text": log_text,
        }
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_syntax_and_deploy_dry_run() -> None:
    print("=== Testing script syntax and deploy.sh --dry-run ===")
    subprocess.run(["sh", "-n", str(REPO_ROOT / "launch.sh")], check=True)
    subprocess.run(["bash", "-n", str(REPO_ROOT / "scripts/deploy.sh")], check=True)

    proc = subprocess.run(
        [
            "bash",
            str(REPO_ROOT / "scripts/deploy.sh"),
            "--method",
            "local",
            "--storage",
            "/tmp",
            "--platform",
            "tg5040",
            "--dry-run",
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    out = proc.stdout + proc.stderr
    assert "bin/tg5040/minui-presenter" in out, f"Missing minui-presenter in dry-run output:\n{out}"
    assert "bin/tg5040/minui-list" in out, f"Missing minui-list in dry-run output:\n{out}"
    assert "bin/tg5040/minui-list-nextui" in out, f"Missing minui-list-nextui in dry-run output:\n{out}"
    print("  PASS (syntax & deploy dry-run)")


def test_ra_filter_flow_and_reroll() -> None:
    print("=== Testing X -> More Options -> Filters -> Games with RetroAchievements + Y reroll ===")
    roms = [
        "NoAch1 (USA).gba",
        "NoAch2 (USA).gba",
        "BadDump (USA).gba",
        "AchAlpha (USA).gba",
        "AchBeta (USA).gba",
    ]
    ra_responses = {
        "NoAch1 (USA).gba": {"status": "none", "earned": 0, "total": 0, "pending": 0, "offline_ready": 0},
        "NoAch2 (USA).gba": {"status": "unsupported", "earned": 0, "total": 0, "pending": 0, "offline_ready": 0},
        "BadDump (USA).gba": {"status": "incompatible", "earned": 0, "total": 0, "pending": 0, "offline_ready": 0},
        "AchAlpha (USA).gba": {"status": "ok", "earned": 5, "total": 25, "pending": 0, "offline_ready": 1},
        "AchBeta (USA).gba": {"status": "ok", "earned": 1, "total": 10, "pending": 0, "offline_ready": 0},
    }

    # Preview actions:
    # 1) Initial preview -> press X (5)
    # 2) Filtered preview #1 -> press Y (4) to reroll with filter still active
    # 3) Filtered preview #2 -> press X (5), enter Filters, press B back to More Options, press B back to preview
    # 4) Same filtered preview #2 -> press X (5), disable filter -> roll -> press A (0) to launch
    presenter_actions = [5, 4, 5, 5, 0]
    list_actions = [
        # First X: More Options -> Filters -> Enable RA filter
        {"rc": 0, "select": "Filters"},
        {"rc": 0, "select": "Games with RetroAchievements: Off"},
        # Second X: check active badge, enter Filters, press B back, press B back
        {"rc": 0, "select": "Filters (1 Active)"},
        {"rc": 2},
        {"rc": 2},
        # Third X: disable RA filter
        {"rc": 0, "select": "Filters (1 Active)"},
        {"rc": 0, "select": "Games with RetroAchievements: On"},
    ]

    res = run_launch_scenario(roms, ra_responses, presenter_actions, list_actions)
    assert res["returncode"] == 0, f"launch.sh failed: {res['log_text']}"

    previews = [e for e in res["presenter_log"] if e["timeout"] == "0"]
    assert len(previews) == 5, f"Expected 5 previews, got {len(previews)}: {previews}"

    # Verify X button is wired to OPTIONS
    for p in previews:
        assert p["inaction_button"] == "X"
        assert p["inaction_text"] == "OPTIONS"
        assert p["inaction_show"] is True

    # Filtered preview #1 (index 1) must be AchAlpha or AchBeta
    p1 = previews[1]["message"]
    assert ("AchAlpha" in p1) or ("AchBeta" in p1), f"Expected RA game on filtered roll 1, got {p1!r}"

    # Filtered preview #2 (index 2) must be the OTHER RA game (no back-to-back repeat!)
    p2 = previews[2]["message"]
    assert ("AchAlpha" in p2) or ("AchBeta" in p2), f"Expected RA game on filtered roll 2, got {p2!r}"
    assert p2 != p1, f"Expected different RA game on reroll, got same: {p1!r}"

    # Backing out of menus with B must keep exact same preview (index 3 == index 2)
    assert previews[3]["message"] == p2, f"Expected B-cancel to keep preview {p2!r}, got {previews[3]['message']!r}"

    # Verify menu titles & items
    assert res["list_log"][0] == {"title": "More Options", "items": ["Filters"], "format": "text"}
    assert res["list_log"][1] == {
        "title": "Filters",
        "items": ["Games with RetroAchievements: Off"],
        "format": "text",
    }
    assert res["list_log"][2] == {"title": "More Options", "items": ["Filters (1 Active)"], "format": "text"}
    assert res["list_log"][3] == {
        "title": "Filters",
        "items": ["Games with RetroAchievements: On", "Clear All Filters"],
        "format": "text",
    }

    assert res["launched"] != "", "Expected game to be launched on A"
    print("  PASS (More Options -> Filters -> RA shuffle & reroll)")


def test_clear_all_filters() -> None:
    print("=== Testing Clear All Filters ===")
    roms = ["NoAch (USA).gba", "AchOne (USA).gba"]
    ra_responses = {
        "NoAch (USA).gba": {"status": "none", "earned": 0, "total": 0},
        "AchOne (USA).gba": {"status": "ok", "earned": 2, "total": 20, "offline_ready": 1},
    }
    presenter_actions = [5, 5, 2]
    list_actions = [
        {"rc": 0, "select": "Filters"},
        {"rc": 0, "select": "Games with RetroAchievements: Off"},
        {"rc": 0, "select": "Filters (1 Active)"},
        {"rc": 0, "select": "Clear All Filters"},
    ]
    res = run_launch_scenario(roms, ra_responses, presenter_actions, list_actions)
    assert res["returncode"] == 0, f"launch.sh failed: {res['log_text']}"
    previews = [e for e in res["presenter_log"] if e["timeout"] == "0"]
    assert "AchOne" in previews[1]["message"], f"Expected AchOne when filtered, got {previews[1]}"
    # After clearing filters, rolling picks NoAch (since AchOne was previous and TOTAL=2)
    assert "NoAch" in previews[2]["message"], f"Expected NoAch after clearing filter, got {previews[2]}"
    print("  PASS (Clear All Filters)")


def test_single_matching_game_reroll() -> None:
    print("=== Testing single matching game reroll ===")
    roms = ["No1 (USA).gba", "No2 (USA).gba", "OnlyRA (USA).gba"]
    ra_responses = {
        "No1 (USA).gba": {"status": "none", "total": 0},
        "No2 (USA).gba": {"status": "unsupported", "total": 0},
        "OnlyRA (USA).gba": {"status": "ok", "earned": 0, "total": 5, "offline_ready": 1},
    }
    # X -> enable filter -> finds OnlyRA -> Y reroll -> should still find OnlyRA -> B quit
    presenter_actions = [5, 4, 2]
    list_actions = [
        {"rc": 0, "select": "Filters"},
        {"rc": 0, "select": "Games with RetroAchievements: Off"},
    ]
    res = run_launch_scenario(roms, ra_responses, presenter_actions, list_actions)
    assert res["returncode"] == 0, f"launch.sh failed: {res['log_text']}"
    previews = [e for e in res["presenter_log"] if e["timeout"] == "0"]
    assert len(previews) == 3
    assert "OnlyRA" in previews[1]["message"]
    assert "OnlyRA" in previews[2]["message"]
    print("  PASS (single matching game reroll)")


def test_zero_matching_games() -> None:
    print("=== Testing zero matching games restores preview & disables filter ===")
    roms = ["No1 (USA).gba", "No2 (USA).gba", "No3 (USA).gba"]
    ra_responses = {
        "No1 (USA).gba": {"status": "none", "total": 0},
        "No2 (USA).gba": {"status": "incompatible", "total": 0},
        "No3 (USA).gba": {"status": "unsupported", "total": 0},
    }
    presenter_actions = [5, 5, 2]
    list_actions = [
        {"rc": 0, "select": "Filters"},
        {"rc": 0, "select": "Games with RetroAchievements: Off"},
        # Second X verifies filter was auto-disabled (no "(1 Active)")
        {"rc": 2},
    ]
    res = run_launch_scenario(roms, ra_responses, presenter_actions, list_actions)
    assert res["returncode"] == 0, f"launch.sh failed: {res['log_text']}"

    previews = [e for e in res["presenter_log"] if e["timeout"] == "0"]
    assert previews[1]["message"] == previews[0]["message"], "Previous game should be restored on 0 matches"

    toasts = [e["message"] for e in res["presenter_log"] if e["timeout"] not in ("0", "-1")]
    assert any("No games with RetroAchievements found." in m for m in toasts), f"Missing toast: {toasts}"

    # Every ROM should be checked at most once during the shuffle (plus 1 initial lookup = 4 total)
    assert len(res["ra_log"]) == 4, f"Expected 1 initial + 3 unique shuffle lookups, got {len(res['ra_log'])}"
    assert res["list_log"][2] == {"title": "More Options", "items": ["Filters"], "format": "text"}
    print("  PASS (zero matching games)")


def test_offline_fallback_finds_cached_game() -> None:
    print("=== Testing offline fallback switches to --offline-only and finds cached RA game ===")
    roms = [
        "Uncached1 (USA).gba",
        "Uncached2 (USA).gba",
        "Uncached3 (USA).gba",
        "Uncached4 (USA).gba",
        "CachedRA (USA).gba",
    ]
    ra_responses = {
        "Uncached1 (USA).gba": {"status": "wifi_required", "total": 0},
        "Uncached2 (USA).gba": {"status": "wifi_required", "total": 0},
        "Uncached3 (USA).gba": {"status": "wifi_required", "total": 0},
        "Uncached4 (USA).gba": {"status": "wifi_required", "total": 0},
        "CachedRA (USA).gba": {
            "online": {"status": "wifi_required", "total": 30, "earned": 0, "offline_ready": 0},
            "offline": {"status": "wifi_required", "total": 30, "earned": 0, "offline_ready": 0},
        },
    }
    presenter_actions = [5, 2]
    list_actions = [
        {"rc": 0, "select": "Filters"},
        {"rc": 0, "select": "Games with RetroAchievements: Off"},
    ]
    res = run_launch_scenario(roms, ra_responses, presenter_actions, list_actions)
    assert res["returncode"] == 0, f"launch.sh failed: {res['log_text']}"

    previews = [e for e in res["presenter_log"] if e["timeout"] == "0"]
    assert "CachedRA" in previews[1]["message"], f"Expected CachedRA, got {previews[1]}"
    assert "RA: 30 Achievements" in previews[1]["message"], f"Expected achievement count display, got {previews[1]}"

    # Check that online lookups during shuffle capped at <= 2 wifi_required failures before switching to offline_only
    shuffle_lookups = res["ra_log"][1:]
    online_wifi_fails = [x for x in shuffle_lookups if not x["offline_only"] and x["rom"] != "CachedRA (USA).gba"]
    assert len(online_wifi_fails) <= 2, f"Should switch to --offline-only after 2 wifi failures: {shuffle_lookups}"
    print("  PASS (offline fallback finds cached game)")


def test_offline_no_cached_games() -> None:
    print("=== Testing offline with no cached RA games shows Wi-Fi toast ===")
    roms = ["U1 (USA).gba", "U2 (USA).gba", "U3 (USA).gba", "U4 (USA).gba"]
    ra_responses = {r: {"status": "wifi_required", "total": 0} for r in roms}
    presenter_actions = [5, 2]
    list_actions = [
        {"rc": 0, "select": "Filters"},
        {"rc": 0, "select": "Games with RetroAchievements: Off"},
    ]
    res = run_launch_scenario(roms, ra_responses, presenter_actions, list_actions)
    assert res["returncode"] == 0, f"launch.sh failed: {res['log_text']}"

    toasts = [e["message"] for e in res["presenter_log"] if e["timeout"] not in ("0", "-1")]
    assert any("Connect to Wi-Fi" in m for m in toasts), f"Expected Wi-Fi toast, got {toasts}"

    shuffle_lookups = res["ra_log"][1:]
    assert len(shuffle_lookups) == 4
    assert sum(1 for x in shuffle_lookups if not x["offline_only"]) == 2
    assert sum(1 for x in shuffle_lookups if x["offline_only"]) == 2
    print("  PASS (offline with 0 cached games)")


def test_auth_invalid_during_shuffle() -> None:
    print("=== Testing auth_invalid mid-shuffle ===")
    # Single ROM returns status=none on initial roll (keeping RA_ENABLED=1),
    # then returns status=auth_invalid when the RA filter shuffles.
    presenter_actions = [5, 5, 2]
    list_actions = [
        {"rc": 0, "select": "Filters"},
        {"rc": 0, "select": "Games with RetroAchievements: Off"},
        # Second X: RA is now disabled, filter was turned off -> selecting it shows unavailable toast
        {"rc": 0, "select": "Filters"},
        {"rc": 0, "select": "Games with RetroAchievements: Off"},
        {"rc": 2},
        {"rc": 2},
    ]
    res = run_launch_scenario(
        ["G1 (USA).gba"],
        {"G1 (USA).gba": [{"status": "none", "total": 0}, {"status": "auth_invalid"}]},
        presenter_actions,
        list_actions,
    )
    assert res["returncode"] == 0, f"launch.sh failed: {res['log_text']}"
    toasts = [e["message"] for e in res["presenter_log"] if e["timeout"] not in ("0", "-1")]
    assert any("RetroAchievements login invalid." in m for m in toasts), f"Expected login invalid toast, got {toasts}"
    assert any("RetroAchievements is not enabled" in m for m in toasts), f"Expected disabled toast on retry, got {toasts}"
    assert res["list_log"][2] == {"title": "More Options", "items": ["Filters"], "format": "text"}
    print("  PASS (auth_invalid mid-shuffle & recovery)")


def test_minui_no_nextui_shows_helpful_message() -> None:
    print("=== Testing MinUI (non-NextUI) shows helpful message when selecting RA filter ===")
    roms = ["GameA (USA).gba", "GameB (USA).gba"]
    presenter_actions = [5, 2]
    list_actions = [
        {"rc": 0, "select": "Filters"},
        {"rc": 0, "select": "Games with RetroAchievements: Off"},
        {"rc": 2},
        {"rc": 2},
    ]
    res = run_launch_scenario(roms, {}, presenter_actions, list_actions, nextui=False, ra_enabled=False)
    assert res["returncode"] == 0, f"launch.sh failed: {res['log_text']}"
    toasts = [e["message"] for e in res["presenter_log"] if e["timeout"] not in ("0", "-1")]
    assert any("RetroAchievements requires NextUI." in m for m in toasts), f"Expected NextUI toast, got {toasts}"
    print("  PASS (MinUI non-NextUI helpful message)")


def main() -> int:
    test_syntax_and_deploy_dry_run()
    test_ra_filter_flow_and_reroll()
    test_clear_all_filters()
    test_single_matching_game_reroll()
    test_zero_matching_games()
    test_offline_fallback_finds_cached_game()
    test_offline_no_cached_games()
    test_auth_invalid_during_shuffle()
    test_minui_no_nextui_shows_helpful_message()
    print("\nALL FILTER TESTS PASSED!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
