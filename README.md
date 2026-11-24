# minui-random-game.pak

A MinUI app that starts a random game from the ROMs folder.

## Requirements

This pak is designed and tested on the following MinUI Platforms and devices:

- `h700`: Anbernic RG28XX, RG34XX, RG34XX SP, RG35XX Plus, RG35XX 2024, RG35XX H, RG35XX Pro, RG35XX SP, RG40XX H, RG40XX V, RG CubeXX and RG SP, running NextUI on BaseOS
- `miyoomini`: Miyoo Mini and Miyoo Mini Plus
- `my282`: Miyoo A30
- `my355`: Miyoo Flip
- `rg35xxplus`: RG-35XX Plus, RG-34XX, RG-35XX H, RG-35XX SP
- `tg5040`: Trimui Brick (formerly `tg3040`), Trimui Smart Pro
- `tg5050`: Trimui Smart Pro S

Use the correct platform for your device.

## Installation

1. Mount your MinUI SD card.
2. Download the latest release from Github. It will be named `Random.Game.pak.zip`.
3. Copy the zip file to `/Tools/$PLATFORM/Random Game.pak.zip`. Please ensure the new zip file name is `Random Game.pak.zip`, without a dot (`.`) between the words `Random` and `Game`.
4. Extract the zip in place, then delete the zip file.
5. Confirm that there is a `/Tools/$PLATFORM/Random Game.pak/launch.sh` file on your SD card.
6. Unmount your SD Card and insert it into your Device.

## Usage

> [!IMPORTANT]
> If the zip file was not extracted correctly, the pak may show up under `Tools > Random Game`. Rename the folder to `Random Game.pak` to fix this.

Browse to `Tools > Random Game` and press `A` to roll a random game. The selected game's title, system, and cover art (if available) will be shown:

- `A`: Launch the game
- `Y`: Re-roll for another random game
- `B`: Return to the main menu

### Debug Logging

Debug logs are written to the `$SDCARD_PATH/.userdata/$PLATFORM/logs/` folder.

## Development & Testing Deployment

Use `scripts/deploy.sh` to deploy directly from your local working tree (including uncommitted edits) without mutating `pak.json`. On the first run it automatically downloads and caches the `minui-presenter` binary for your target platform under `bin/<platform>/`.

### One-time configuration (optional)

Copy `.deploy.env.example` to `.deploy.env` (gitignored) so you don't need to pass flags on every run:

```sh
cp .deploy.env.example .deploy.env
```

CLI flags always override `.deploy.env`. Tip: pass `--name "Random Game Dev"` to install side-by-side without overwriting an existing `Random Game.pak`.

### Option 1: USB via ADB (Recommended)

NextUI on the TrimUI Brick / Brick Hammer (`tg5040`) enables ADB out of the box over the **bottom USB-C port** (not the top/back OTG port).

1. Install Android Platform Tools on macOS:
   ```sh
   brew install android-platform-tools
   ```
2. Plug a data-capable USB-C cable into the **bottom** USB-C port of your powered-on handheld and verify with `adb devices`.
3. Deploy:
   ```sh
   ./scripts/deploy.sh --device brick-hammer
   ```
   *(If ADB ever stops responding after unplugging, reboot the handheld or install the **Restart ADB** pak from the Pak Store.)*

### Option 2: Wi-Fi via SSH

1. On your handheld, open **Pak Store** and install **SSH Server** (Dropbear), then turn it on under `Tools > SSH Server`.
2. Find your device's IP address in NextUI's Wi-Fi settings.
3. Deploy (default credentials: `root` / `tina`, or `trimui` / `trimui`):
   ```sh
   ./scripts/deploy.sh --method ssh --ip <DEVICE_IP>
   ```
   *(Optional: to avoid typing the password on every deploy, copy your public key once:)*
   ```sh
   cat ~/.ssh/id_ed25519.pub | ssh root@<DEVICE_IP> 'mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 700 ~/.ssh && chmod 600 ~/.ssh/authorized_keys'
   ```

### Option 3: Local SD Card

With your microSD card mounted on macOS (e.g., volume name `NEXTUI`):

```sh
./scripts/deploy.sh --method local --storage NEXTUI
```

### Viewing Debug Logs

After launching `Tools > Random Game` on your device:

```sh
# Print the latest log (ADB or SSH)
./scripts/deploy.sh --logs-only

# Stream logs live while testing on the device
./scripts/deploy.sh --follow-logs
```

