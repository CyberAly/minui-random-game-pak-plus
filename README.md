# minui-random-game-pak-plus

An enhanced fork of Jose Diaz-Gonzalez's [`minui-random-game-pak`](https://github.com/josegonzalez/minui-random-game-pak) for MinUI and NextUI handhelds—adds an interactive game preview screen with cover art, instant re-rolling, smarter ROM filtering, and a one-command build & deploy script.

> [!NOTE]
> **AI-Coded Enhancement Notice**
>
> This repository is an **AI-coded enhancement** built on top of [`josegonzalez/minui-random-game-pak`](https://github.com/josegonzalez/minui-random-game-pak) (upstream `v0.7.0`). All new features, scripts, and documentation in this fork were created with AI coding assistance and tested on real hardware. Full credit for the original pak concept, platform support, and `minui-presenter` goes to [Jose Diaz-Gonzalez](https://github.com/josegonzalez).

## Why Use This Version Over the Original?

The original `minui-random-game.pak` picks a random file from `/Roms` and immediately launches it. This version keeps the same lightweight shell-script footprint while adding quality-of-life improvements:

- **Interactive Game Preview with Cover Art**: Before launching, displays the rolled game's title, system name, and box/cover art (if available) with a translucent pill overlay for readability.
- **Optional NextUI RetroAchievements Preview**: Automatically activates when RetroAchievements is enabled and authenticated in NextUI Settings (no separate login required, invisible no-op on MinUI or when RA is disabled/invalid):
  - Displays softcore progress (e.g. `RA: 34/50`), whether the game is cached in NextUI for offline play (`RA Offline Ready`, plus pending unsynced count from NextUI's offline ledger) or needs Wi-Fi (`WiFi Required for RA`), as well as `RA None` (no official set) and `RA Unsupported` (unmatched ROM/hash).
  - Shows NextUI's native top-right hardware bar (Wi-Fi status, clock, and battery) so you can immediately tell if you're connected.
  - Uses a small static C helper (`ra-helper`, built with `rcheevos` + `libchdr` + `miniz`) for 100% ROM hashing parity with NextUI across cartridge, archive (`.zip`), arcade, and disc (`.chd`/`.cue`/`.iso`/`.pbp`) systems, with lazy per-roll caching and strictly read-only inspection of NextUI's `.ra/offline/` cache and ledger.
- **Instant Re-roll & Cancel Controls**: Not in the mood for the picked game? Press `Y` to roll another game immediately or `B` to return to the menu—no more launching a game just to quit and re-open the pak.
- **Faster, Smarter ROM Scanning**:
  - Scans your `Roms/` directory **once** per session so re-rolling is near-instant (no repeated `find` scans or artificial startup delay).
  - Skips `*.disabled` folders and systems that don't have an installed emulator pak (`Emus/<PLATFORM>/<EMU>.pak` or `.system/<PLATFORM>/paks/Emus/<EMU>.pak`).
  - Filters out non-game files commonly stored alongside ROMs: artwork folders and images (`Imgs/*`, `.png`, `.jpg`, `.jpeg`, `.bmp`, `.webp`), metadata/databases (`.xml`, `.json`, `.db`), save files (`.sav`, `.srm`), hidden files, `.txt`, and `.log`.
- **Clean Titles & `map.txt` Support**: Reads MinUI `map.txt` files to display proper game names (especially helpful for arcade/MAME sets), falls back to stripping region/revision tags like `(USA)` and `[!]`, and cleans system folder names (e.g., `01) Game Boy Advance (GBA)` → `Game Boy Advance`).
- **Better Randomness & No Back-to-Back Repeats**: Uses `/dev/urandom` (with a deterministic fallback) instead of `(timestamp + PID) % total`, and avoids picking the exact same game twice in a row when re-rolling.
- **Built-in Multi-Transport Deploy Script (`scripts/deploy.sh`)**: Automatically fetches the matching `minui-presenter` binary for your platform, builds `ra-helper` (via local cross-compiler or Docker if available), and deploys directly from your computer via USB (ADB), Wi-Fi (SSH), or a mounted microSD card—plus built-in remote debug log viewing.

## Supported Platforms

| Platform ID | Device Aliases (`deploy.sh`) | Devices |
| --- | --- | --- |
| `tg5040` | `brick`, `brick-hammer`, `smart-pro`, `tsp` | TrimUI Brick, TrimUI Brick Hammer (formerly `tg3040`), TrimUI Smart Pro |
| `tg5050` | `smart-pro-s`, `tsps` | TrimUI Smart Pro S |
| `h700` | `anbernic`, `anbernic-h700` | Anbernic RG28XX, RG34XX, RG34XX SP, RG35XX Plus/2024/H/Pro/SP, RG40XX H/V, RG CubeXX, RG SP (NextUI on BaseOS) |
| `miyoomini` | `miyoo-mini`, `miyoo-mini-plus`, `mmp` | Miyoo Mini, Miyoo Mini Plus |
| `my282` | `miyoo-a30`, `a30` | Miyoo A30 |
| `my355` | `miyoo-flip`, `flip` | Miyoo Flip |
| `rg35xxplus` | `rg35xx-plus`, `rg35xx`, `rg34xx`, `rg40xx` | Anbernic RG-35XX Plus, RG-34XX, RG-35XX H, RG-35XX SP |

## Usage & Controls

On your handheld, open `Tools > Random Game` (or whatever custom name you deployed it under). After a brief scan, a random game preview will appear:

| Button | Action |
| --- | --- |
| `A` | **PLAY** — Launch the selected game and add it to MinUI Recents |
| `Y` | **REROLL** — Pick another random game |
| `B` | **QUIT** — Exit back to the main menu |

**Cover art lookup:** The pak automatically checks `<rom_dir>/.media/<rom_name>.{png,jpg,jpeg}`, `<rom_dir>/.media/<rom_filename>.png`, `<rom_dir>/Imgs/<rom_name>.{png,jpg}`, and `<parent_dir>/.media/<rom_name>.{png,jpg}`. If no art is found, it falls back to a clean centered text layout.

---

## Building & Deploying to Your Unit

The core pak is a POSIX shell script (`launch.sh`) paired with prebuilt [`minui-presenter`](https://github.com/josegonzalez/minui-presenter) binaries and an optional static C helper (`src/ra-helper`) for NextUI RetroAchievements support. If `ra-helper` isn't built or you're on MinUI, the pak still runs cleanly without RetroAchievements.

### Prerequisites

- **All methods:** `git`, `curl`, and `bash` (built into macOS and Linux; use WSL or Git Bash on Windows)
- **RetroAchievements helper (`ra-helper`):**
  - **macOS (easiest, via Docker):** `brew install docker colima && colima start` (or OrbStack / Docker Desktop) — `scripts/deploy.sh` uses it automatically
  - **macOS (native, no Docker):** `brew install cmake messense/macos-cross-toolchains/aarch64-unknown-linux-gnu`
  - **Linux:** `sudo apt install build-essential cmake gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf` (or Docker)
- **USB (ADB) deploy:** Android Platform Tools (`adb`)
- **Wi-Fi (SSH) deploy:** `ssh` (built into macOS/Linux) and NextUI Pak Store's **SSH Server**
- **Building a standalone `.pak.zip` via `make`:** `make`, `jq`, `zip`, `cmake`, and ARM cross-compilers

### Step 1: Clone the Repository

```sh
git clone https://github.com/CyberAly/minui-random-game-pak-plus.git
cd minui-random-game-pak-plus
```

### Step 2: Deploy Directly with `scripts/deploy.sh` (Recommended)

`scripts/deploy.sh` is the fastest way to build and install the pak onto your handheld. On its first run, it automatically downloads and caches the correct `minui-presenter` binary under `bin/<platform>/`, stages only the needed files (`launch.sh`, `pak.json`, and your platform's binary), and copies them to `/Tools/<PLATFORM>/Random Game.pak/` on your SD card.

> [!TIP]
> Want to try this alongside the original `Random Game` pak without overwriting it? Pass `--name "Random Game Plus"` to any command below to install it side-by-side under `Tools > Random Game Plus`.

#### Option A: USB via ADB (Easiest for TrimUI Brick / Smart Pro on NextUI)

NextUI on `tg5040` devices enables ADB out of the box over the **bottom USB-C port** (not the top/back OTG port).

1. Install Android Platform Tools:
   - **macOS:** `brew install android-platform-tools`
   - **Ubuntu/Debian:** `sudo apt install adb`
2. Connect a data-capable USB-C cable to the **bottom** USB-C port of your powered-on handheld and verify it appears:
   ```sh
   adb devices
   ```
3. Build & deploy (replace `brick-hammer` with your device alias or platform ID from the table above):
   ```sh
   ./scripts/deploy.sh --device brick-hammer
   ```

*(If ADB stops responding after unplugging, reboot the handheld or run the **Restart ADB** pak from the Pak Store.)*

#### Option B: Wi-Fi via SSH

1. On your handheld, open **Pak Store**, install **SSH Server** (Dropbear), and enable it under `Tools > SSH Server`.
2. Check your handheld's IP address in NextUI's Wi-Fi settings.
3. Build & deploy (default NextUI credentials are `root` / `tina`, or `trimui` / `trimui`):
   ```sh
   ./scripts/deploy.sh --method ssh --ip <DEVICE_IP> --platform <PLATFORM>
   ```
4. *(Optional)* Copy your SSH public key once to skip password prompts on future deploys:
   ```sh
   cat ~/.ssh/id_ed25519.pub | ssh root@<DEVICE_IP> 'mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 700 ~/.ssh && chmod 600 ~/.ssh/authorized_keys'
   ```

#### Option C: Mounted microSD Card

1. Insert your MinUI/NextUI microSD card into your computer.
2. Build & deploy directly to the mounted card:
   - **macOS** (using the volume name shown in `/Volumes`, e.g., `NEXTUI`):
     ```sh
     ./scripts/deploy.sh --method local --storage NEXTUI --platform <PLATFORM>
     ```
   - **Linux** (using the full mount path):
     ```sh
     ./scripts/deploy.sh --method local --storage /media/$USER/NEXTUI --platform <PLATFORM>
     ```
3. Safely eject the SD card and put it back into your handheld.

#### Optional: Save Your Deploy Settings

To avoid retyping flags on every run, copy `.deploy.env.example` to `.deploy.env` (gitignored) and fill in your device details:

```sh
cp .deploy.env.example .deploy.env
```

CLI flags always override values in `.deploy.env`. Other handy flags include `--clean` (wipe the target `.pak` folder before copying), `--dry-run` (preview actions without changing anything), and `--help` (full option reference).

---

### Alternative: Build a Standalone `.pak.zip` & Install Manually

If you prefer a portable multi-platform `.pak.zip` archive (for manual SD card installation or sharing), use the `Makefile`:

1. Download the `minui-presenter` binaries for all supported platforms (or a single platform with `make bin/<PLATFORM>/minui-presenter`):
   ```sh
   make build
   ```
2. Package the committed repository and binaries into `dist/Random Game.pak.zip`:
   ```sh
   RELEASE_VERSION=0.7.0-plus make release
   ```
   *(Note: `make release` archives `HEAD` and updates the `version` field in `pak.json`. Run `git checkout -- pak.json` afterwards if you want to keep your working tree clean.)*
3. Install onto your SD card manually:
   - Mount your MinUI/NextUI SD card on your computer.
   - Extract `dist/Random Game.pak.zip` into `/Tools/<PLATFORM>/Random Game.pak/` on the SD card.
   - Confirm that `/Tools/<PLATFORM>/Random Game.pak/launch.sh` and `/Tools/<PLATFORM>/Random Game.pak/bin/<PLATFORM>/minui-presenter` exist.
   - Safely eject the SD card, insert it into your handheld, and launch `Tools > Random Game`.

> [!IMPORTANT]
> The installed folder name must end in `.pak` (for example, `Random Game.pak`). If the pak appears as a nested folder under `Tools > Random Game`, rename the folder so it ends with `.pak`.

## Troubleshooting & Debug Logs

Every run writes a full trace log to your SD card at:

```text
$SDCARD_PATH/.userdata/$PLATFORM/logs/<Pak Name>.txt
```

If you deployed over ADB or SSH, you can inspect or stream logs directly from your computer:

```sh
# Print the latest debug log
./scripts/deploy.sh --logs-only

# Stream logs live while testing on the handheld
./scripts/deploy.sh --follow-logs
```

*(If you installed with a custom `--name`, pass the same `--name` flag when viewing logs.)*

## Credits & License

- Original project: [`josegonzalez/minui-random-game-pak`](https://github.com/josegonzalez/minui-random-game-pak) by Jose Diaz-Gonzalez
- UI presenter binary: [`josegonzalez/minui-presenter`](https://github.com/josegonzalez/minui-presenter)
- Licensed under the [MIT License](LICENSE).
