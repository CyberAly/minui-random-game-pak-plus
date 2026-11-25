#!/usr/bin/env bash
# Deploy this MinUI/NextUI pak directly from the working tree to a handheld.
# Supports USB (ADB), Wi-Fi (SSH), and local SD card copy.
# Compatible with macOS default bash 3.2+ and Linux bash.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

log() {
    printf '[deploy] %s\n' "$*"
}

warn() {
    printf '[deploy] WARNING: %s\n' "$*" >&2
}

die() {
    printf '[deploy] ERROR: %s\n' "$*" >&2
    exit 1
}

to_lower() {
    printf '%s' "$1" | tr '[:upper:]' '[:lower:]'
}

shell_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

read_pak_field() {
    field="$1"
    pak_json="$REPO_ROOT/pak.json"
    if [ ! -f "$pak_json" ]; then
        return 1
    fi
    if command -v jq >/dev/null 2>&1; then
        jq -r --arg f "$field" '.[$f] // empty' "$pak_json"
    elif command -v python3 >/dev/null 2>&1; then
        python3 - "$pak_json" "$field" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    data = json.load(fh)
val = data.get(sys.argv[2], "")
if isinstance(val, str):
    print(val)
PY
    else
        sed -n "s/^[[:space:]]*\"${field}\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$pak_json" | head -n 1
    fi
}

read_presenter_version() {
    makefile="$REPO_ROOT/Makefile"
    if [ -f "$makefile" ]; then
        ver="$(sed -n 's/^MINUI_PRESENTER_VERSION[[:space:]]*:=[[:space:]]*//p' "$makefile" | head -n 1 | tr -d '[:space:]')"
        if [ -n "$ver" ]; then
            printf '%s\n' "$ver"
            return 0
        fi
    fi
    printf '0.13.4\n'
}

resolve_platform() {
    raw="$(to_lower "$1")"
    case "$raw" in
        tg5040 | tg3040 | brick | brick-hammer | brick_hammer | "brick hammer" | trimui-brick | trimui-brick-hammer | smart-pro | smartpro | tsp)
            printf 'tg5040\n'
            ;;
        tg5050 | smart-pro-s | smartpros | tsps)
            printf 'tg5050\n'
            ;;
        h700 | anbernic | anbernic-h700)
            printf 'h700\n'
            ;;
        miyoomini | miyoo-mini | miyoo-mini-plus | mmp)
            printf 'miyoomini\n'
            ;;
        my282 | miyoo-a30 | a30)
            printf 'my282\n'
            ;;
        my355 | miyoo-flip | flip)
            printf 'my355\n'
            ;;
        rg35xxplus | rg35xx-plus | rg35xx | rg34xx | rg40xx)
            printf 'rg35xxplus\n'
            ;;
        *)
            die "Unknown device/platform '$1'. Supported platforms: tg5040 (TrimUI Brick / Brick Hammer / Smart Pro), tg5050 (Smart Pro S), h700, miyoomini, my282, my355, rg35xxplus."
            ;;
    esac
}

presenter_download_url() {
    platform="$1"
    version="$2"
    base="https://github.com/josegonzalez/minui-presenter/releases/download/${version}"
    case "$platform" in
        h700)
            printf '%s/minui-presenter-h700-nextui\n' "$base"
            ;;
        tg5050)
            printf '%s/minui-presenter-tg5050-nextui\n' "$base"
            ;;
        *)
            printf '%s/minui-presenter-%s\n' "$base" "$platform"
            ;;
    esac
}

presenter_nextui_download_url() {
    platform="$1"
    version="$2"
    base="https://github.com/josegonzalez/minui-presenter/releases/download/${version}"
    case "$platform" in
        tg5040 | my355)
            printf '%s/minui-presenter-%s-nextui\n' "$base" "$platform"
            ;;
        *)
            return 1
            ;;
    esac
}

platform_arch() {
    case "$1" in
        tg5040 | tg5050 | h700 | my355)
            printf 'arm64\n'
            ;;
        *)
            printf 'arm\n'
            ;;
    esac
}

usage() {
    cat <<'EOF'
Usage: scripts/deploy.sh [options]

Deploys the working tree of this pak (including uncommitted changes) to a
MinUI/NextUI handheld. Automatically downloads the target platform's
minui-presenter binary if not already cached in bin/<platform>/.

Config file:
  If .deploy.env exists in the repo root (copy from .deploy.env.example), it is
  loaded automatically. CLI flags always override values in .deploy.env.

Options:
  -m, --method <adb|ssh|local>  Transport method (default: adb)
  -d, --device, --model <name>  Friendly device name or platform ID
                                (e.g. brick-hammer, brick, tg5040, tg5050)
  -p, --platform <id>           Platform ID (default: tg5040)
  -s, --sdcard, --storage <p>   SD card path (default: /mnt/SDCARD for adb/ssh;
                                for local on macOS, a bare volume name like
                                NEXTUI resolves to /Volumes/NEXTUI)
  -n, --name <pak-name>         Target pak folder name without .pak
                                (default: from pak.json, e.g. "Random Game";
                                use e.g. "Random Game Dev" to test side-by-side)
  -H, --host, --ip <addr>       Device IP/hostname (required for --method ssh)
  -u, --user <user>             SSH username (default: root)
  -P, --port <port>             SSH port (default: 22)
  -k, --key <path>              SSH private key path (optional)
      --serial <id>             ADB device serial (optional, if multiple devices)
  -c, --config <path>           Path to config env file (default: .deploy.env)
      --clean                   Delete existing target .pak folder before deploy
      --force-download          Re-download minui-presenter binary even if cached
      --logs                    Show the pak debug log after deploying
      --logs-only               Skip deploy and just print the pak debug log
      --follow-logs             Skip deploy and tail -f the pak debug log (adb/ssh)
      --dry-run                 Print what would happen without modifying device
  -h, --help                    Show this help message

Examples:
  # Deploy to TrimUI Brick Hammer over USB (bottom USB-C port, ADB):
  ./scripts/deploy.sh

  # Deploy over Wi-Fi using NextUI Pak Store's "SSH Server":
  ./scripts/deploy.sh --method ssh --ip 192.168.1.42

  # Deploy side-by-side as "Random Game Dev" and tail logs:
  ./scripts/deploy.sh --name "Random Game Dev"
  ./scripts/deploy.sh --name "Random Game Dev" --follow-logs

  # Deploy to a mounted microSD card named NEXTUI on macOS:
  ./scripts/deploy.sh --method local --storage NEXTUI
EOF
}

# ---------------------------------------------------------------------------
# Parse config file first (pre-scan for --config / -c and --help / -h)
# ---------------------------------------------------------------------------

CONFIG_FILE="$REPO_ROOT/.deploy.env"
CONFIG_EXPLICIT=0

prev=""
for arg in "$@"; do
    if [ "$prev" = "-c" ] || [ "$prev" = "--config" ]; then
        CONFIG_FILE="$arg"
        CONFIG_EXPLICIT=1
        prev=""
        continue
    fi
    case "$arg" in
        -c | --config)
            prev="$arg"
            ;;
        --config=*)
            CONFIG_FILE="${arg#*=}"
            CONFIG_EXPLICIT=1
            ;;
        -h | --help)
            usage
            exit 0
            ;;
    esac
done

# Defaults
DEPLOY_METHOD="adb"
DEPLOY_PLATFORM="tg5040"
DEPLOY_SDCARD=""
DEPLOY_NAME=""
DEPLOY_HOST=""
DEPLOY_USER="root"
DEPLOY_PORT="22"
DEPLOY_SSH_KEY=""
DEPLOY_ADB_SERIAL=""
DEPLOY_CLEAN=0
FORCE_DOWNLOAD=0
SHOW_LOGS=0
LOGS_ONLY=0
FOLLOW_LOGS=0
DRY_RUN=0

if [ -f "$CONFIG_FILE" ]; then
    # shellcheck source=/dev/null
    . "$CONFIG_FILE"
elif [ "$CONFIG_EXPLICIT" -eq 1 ]; then
    die "Config file not found: $CONFIG_FILE"
fi

# ---------------------------------------------------------------------------
# Parse CLI flags (override .deploy.env)
# ---------------------------------------------------------------------------

require_arg() {
    if [ "$#" -lt 2 ] || [ -z "$2" ]; then
        die "Option $1 requires a value."
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -m | --method)
            require_arg "$1" "${2:-}"
            DEPLOY_METHOD="$2"
            shift 2
            ;;
        --method=*)
            DEPLOY_METHOD="${1#*=}"
            shift
            ;;
        -d | --device | --model | -p | --platform)
            require_arg "$1" "${2:-}"
            DEPLOY_PLATFORM="$2"
            shift 2
            ;;
        --device=* | --model=* | --platform=*)
            DEPLOY_PLATFORM="${1#*=}"
            shift
            ;;
        -s | --sdcard | --storage | --mount)
            require_arg "$1" "${2:-}"
            DEPLOY_SDCARD="$2"
            shift 2
            ;;
        --sdcard=* | --storage=* | --mount=*)
            DEPLOY_SDCARD="${1#*=}"
            shift
            ;;
        -n | --name)
            require_arg "$1" "${2:-}"
            DEPLOY_NAME="$2"
            shift 2
            ;;
        --name=*)
            DEPLOY_NAME="${1#*=}"
            shift
            ;;
        -H | --host | --ip)
            require_arg "$1" "${2:-}"
            DEPLOY_HOST="$2"
            shift 2
            ;;
        --host=* | --ip=*)
            DEPLOY_HOST="${1#*=}"
            shift
            ;;
        -u | --user)
            require_arg "$1" "${2:-}"
            DEPLOY_USER="$2"
            shift 2
            ;;
        --user=*)
            DEPLOY_USER="${1#*=}"
            shift
            ;;
        -P | --port)
            require_arg "$1" "${2:-}"
            DEPLOY_PORT="$2"
            shift 2
            ;;
        --port=*)
            DEPLOY_PORT="${1#*=}"
            shift
            ;;
        -k | --key)
            require_arg "$1" "${2:-}"
            DEPLOY_SSH_KEY="$2"
            shift 2
            ;;
        --key=*)
            DEPLOY_SSH_KEY="${1#*=}"
            shift
            ;;
        --serial)
            require_arg "$1" "${2:-}"
            DEPLOY_ADB_SERIAL="$2"
            shift 2
            ;;
        --serial=*)
            DEPLOY_ADB_SERIAL="${1#*=}"
            shift
            ;;
        -c | --config)
            require_arg "$1" "${2:-}"
            shift 2
            ;;
        --config=*)
            shift
            ;;
        --clean)
            DEPLOY_CLEAN=1
            shift
            ;;
        --force-download)
            FORCE_DOWNLOAD=1
            shift
            ;;
        --logs)
            SHOW_LOGS=1
            shift
            ;;
        --logs-only)
            LOGS_ONLY=1
            shift
            ;;
        --follow-logs)
            FOLLOW_LOGS=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            die "Unknown option: $1 (run with --help for usage)"
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Normalize and validate settings
# ---------------------------------------------------------------------------

DEPLOY_METHOD="$(to_lower "$DEPLOY_METHOD")"
case "$DEPLOY_METHOD" in
    adb | ssh | local) ;;
    *)
        die "Unsupported --method '$DEPLOY_METHOD'. Choose one of: adb, ssh, local."
        ;;
esac

PLATFORM="$(resolve_platform "$DEPLOY_PLATFORM")"

DEFAULT_PAK_NAME="$(read_pak_field name || true)"
if [ -z "$DEFAULT_PAK_NAME" ]; then
    DEFAULT_PAK_NAME="Random Game"
fi
PAK_NAME="${DEPLOY_NAME:-$DEFAULT_PAK_NAME}"
PAK_NAME="${PAK_NAME%.pak}"

PAK_TYPE="$(read_pak_field type || true)"
if [ -z "$PAK_TYPE" ]; then
    PAK_TYPE="TOOL"
fi
PAK_TYPE_FIRST="$(printf '%s' "$PAK_TYPE" | cut -c1 | tr '[:lower:]' '[:upper:]')"
PAK_TYPE_REST="$(printf '%s' "$PAK_TYPE" | cut -c2- | tr '[:upper:]' '[:lower:]')"
PAK_FOLDER="${PAK_TYPE_FIRST}${PAK_TYPE_REST}s"

if [ "$DEPLOY_METHOD" = "local" ]; then
    if [ -z "$DEPLOY_SDCARD" ] || [ "$DEPLOY_SDCARD" = "/mnt/SDCARD" ]; then
        if [ ! -d "/mnt/SDCARD" ]; then
            if [ "$(uname -s)" = "Darwin" ] && [ -d "/Volumes" ]; then
                warn "Available volumes in /Volumes:"
                ls -1 /Volumes >&2 || true
            fi
            die "For --method local, pass your mounted SD card name or path via --storage (e.g. --storage NEXTUI or --sdcard /Volumes/NEXTUI)."
        fi
    fi
    # On macOS, expand a bare storage/volume name (no leading '/') to /Volumes/<name>
    case "$DEPLOY_SDCARD" in
        /* | ./* | ../*) ;;
        *)
            if [ "$(uname -s)" = "Darwin" ]; then
                DEPLOY_SDCARD="/Volumes/$DEPLOY_SDCARD"
            fi
            ;;
    esac
else
    if [ -z "$DEPLOY_SDCARD" ]; then
        DEPLOY_SDCARD="/mnt/SDCARD"
    fi
fi

# Strip trailing slash unless root
if [ "$DEPLOY_SDCARD" != "/" ]; then
    DEPLOY_SDCARD="${DEPLOY_SDCARD%/}"
fi

TARGET_PAK_DIR="${DEPLOY_SDCARD}/${PAK_FOLDER}/${PLATFORM}/${PAK_NAME}.pak"
LOG_FILE_PATH="${DEPLOY_SDCARD}/.userdata/${PLATFORM}/logs/${PAK_NAME}.txt"

# Build reusable ADB / SSH argument arrays (bash 3.2 safe)
ADB_ARGS=()
if [ -n "$DEPLOY_ADB_SERIAL" ]; then
    ADB_ARGS+=(-s "$DEPLOY_ADB_SERIAL")
fi

SSH_ARGS=(-p "$DEPLOY_PORT" -o ConnectTimeout=8)
if [ -n "$DEPLOY_SSH_KEY" ]; then
    # Expand leading tilde if provided in .deploy.env
    case "$DEPLOY_SSH_KEY" in
        "~"/*) DEPLOY_SSH_KEY="$HOME/${DEPLOY_SSH_KEY#~/}" ;;
    esac
    [ -f "$DEPLOY_SSH_KEY" ] || die "SSH key not found: $DEPLOY_SSH_KEY"
    SSH_ARGS+=(-i "$DEPLOY_SSH_KEY")
fi

# ---------------------------------------------------------------------------
# Transport preflight checks
# ---------------------------------------------------------------------------

check_adb() {
    if ! command -v adb >/dev/null 2>&1; then
        die "'adb' not found. On macOS, install it with: brew install android-platform-tools"
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
        return 0
    fi

    devices_out="$(adb devices | awk 'NR>1 && $2=="device" {print $1}')"
    if [ -z "$devices_out" ]; then
        cat >&2 <<'EOF'
[deploy] ERROR: No ADB device detected.
  Checklist for TrimUI Brick / Brick Hammer on NextUI:
  1. Plug a data-capable USB-C cable into the BOTTOM USB-C port (not the top/back port).
  2. Make sure the handheld is powered on and awake.
  3. If ADB stopped responding after unplugging earlier, reboot the Brick or run
     the "Restart ADB" pak from the Pak Store.
  4. Verify with: adb devices
EOF
        exit 1
    fi

    device_count="$(printf '%s\n' "$devices_out" | wc -l | tr -d '[:space:]')"
    if [ "$device_count" -gt 1 ] && [ -z "$DEPLOY_ADB_SERIAL" ]; then
        warn "Multiple ADB devices detected:"
        printf '%s\n' "$devices_out" >&2
        die "Specify which device to target using --serial <id> (or DEPLOY_ADB_SERIAL in .deploy.env)."
    fi
}

check_ssh() {
    if ! command -v ssh >/dev/null 2>&1; then
        die "'ssh' command not found."
    fi
    if [ -z "$DEPLOY_HOST" ]; then
        die "For --method ssh, specify the handheld's IP address via --ip <addr> (or DEPLOY_HOST in .deploy.env)."
    fi
}

check_local() {
    if [ "$DRY_RUN" -eq 1 ]; then
        return 0
    fi
    if [ ! -d "$DEPLOY_SDCARD" ]; then
        if [ "$(uname -s)" = "Darwin" ] && [ -d "/Volumes" ]; then
            warn "Available volumes in /Volumes:"
            ls -1 /Volumes >&2 || true
        fi
        die "SD card mount directory not found: $DEPLOY_SDCARD"
    fi
}

# ---------------------------------------------------------------------------
# Log helpers
# ---------------------------------------------------------------------------

show_logs() {
    log "Reading debug log: $LOG_FILE_PATH"
    if [ "$DRY_RUN" -eq 1 ]; then
        log "[dry-run] Would read $LOG_FILE_PATH via $DEPLOY_METHOD"
        return 0
    fi
    quoted_log="$(shell_quote "$LOG_FILE_PATH")"
    case "$DEPLOY_METHOD" in
        adb)
            adb ${ADB_ARGS[@]+"${ADB_ARGS[@]}"} shell "if [ -f $quoted_log ]; then cat $quoted_log; else echo 'Log file not found yet (launch the pak on your device first):' $quoted_log; fi"
            ;;
        ssh)
            ssh ${SSH_ARGS[@]+"${SSH_ARGS[@]}"} "${DEPLOY_USER}@${DEPLOY_HOST}" "if [ -f $quoted_log ]; then cat $quoted_log; else echo 'Log file not found yet (launch the pak on your device first):' $quoted_log; fi"
            ;;
        local)
            if [ -f "$LOG_FILE_PATH" ]; then
                cat "$LOG_FILE_PATH"
            else
                warn "Log file not found yet (launch the pak on your device first): $LOG_FILE_PATH"
            fi
            ;;
    esac
}

follow_logs() {
    log "Following debug log (Ctrl+C to stop): $LOG_FILE_PATH"
    if [ "$DRY_RUN" -eq 1 ]; then
        log "[dry-run] Would tail -f $LOG_FILE_PATH via $DEPLOY_METHOD"
        return 0
    fi
    quoted_log="$(shell_quote "$LOG_FILE_PATH")"
    quoted_log_dir="$(shell_quote "$(dirname "$LOG_FILE_PATH")")"
    tail_cmd="mkdir -p $quoted_log_dir && touch $quoted_log && tail -n 50 -f $quoted_log"
    case "$DEPLOY_METHOD" in
        adb)
            adb ${ADB_ARGS[@]+"${ADB_ARGS[@]}"} shell "$tail_cmd"
            ;;
        ssh)
            ssh ${SSH_ARGS[@]+"${SSH_ARGS[@]}"} "${DEPLOY_USER}@${DEPLOY_HOST}" "$tail_cmd"
            ;;
        local)
            die "--follow-logs is only supported for --method adb or --method ssh."
            ;;
    esac
}

# Short-circuit if only reading/following logs
if [ "$LOGS_ONLY" -eq 1 ] || [ "$FOLLOW_LOGS" -eq 1 ]; then
    case "$DEPLOY_METHOD" in
        adb) check_adb ;;
        ssh) check_ssh ;;
        local) check_local ;;
    esac
    if [ "$FOLLOW_LOGS" -eq 1 ]; then
        follow_logs
    else
        show_logs
    fi
    exit 0
fi

# ---------------------------------------------------------------------------
# Ensure minui-presenter binary for target platform is cached locally
# ---------------------------------------------------------------------------

ARCH="$(platform_arch "$PLATFORM")"

download_binary_if_needed() {
    bin_path="$1"
    url="$2"
    label="$3"

    if [ -x "$bin_path" ] && [ -s "$bin_path" ] && [ "$FORCE_DOWNLOAD" -eq 0 ]; then
        log "Using cached binary: $label"
        return 0
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
        log "[dry-run] Would download $url -> $label"
        return 0
    fi

    command -v curl >/dev/null 2>&1 || die "'curl' is required to download $label."
    log "Downloading $label..."
    mkdir -p "$(dirname "$bin_path")"
    tmp_bin="${bin_path}.tmp.$$"
    if ! curl -f -sSL -o "$tmp_bin" "$url"; then
        rm -f "$tmp_bin"
        die "Failed to download $label from $url"
    fi
    if [ ! -s "$tmp_bin" ]; then
        rm -f "$tmp_bin"
        die "Downloaded binary was empty: $url"
    fi
    chmod +x "$tmp_bin"
    mv "$tmp_bin" "$bin_path"
    log "Cached binary at $label"
}

ensure_presenter_binary() {
    version="$(read_presenter_version)"
    url="$(presenter_download_url "$PLATFORM" "$version")"
    download_binary_if_needed "$REPO_ROOT/bin/${PLATFORM}/minui-presenter" "$url" "bin/${PLATFORM}/minui-presenter"

    if nextui_url="$(presenter_nextui_download_url "$PLATFORM" "$version" 2>/dev/null)"; then
        download_binary_if_needed "$REPO_ROOT/bin/${PLATFORM}/minui-presenter-nextui" "$nextui_url" "bin/${PLATFORM}/minui-presenter-nextui"
    fi
}

ensure_ra_helper_binary() {
    helper_path="$REPO_ROOT/bin/${ARCH}/ra-helper"
    need_rebuild=0
    if [ ! -x "$helper_path" ] || [ ! -s "$helper_path" ] || [ "$FORCE_DOWNLOAD" -eq 1 ]; then
        need_rebuild=1
    else
        for src_file in "$REPO_ROOT"/src/ra-helper/*.c "$REPO_ROOT"/src/ra-helper/*.h "$REPO_ROOT"/src/ra-helper/Makefile; do
            if [ -f "$src_file" ] && [ "$src_file" -nt "$helper_path" ]; then
                need_rebuild=1
                break
            fi
        done
    fi

    if [ "$need_rebuild" -eq 0 ]; then
        log "Using cached binary: bin/${ARCH}/ra-helper"
        return 0
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
        log "[dry-run] Would build bin/${ARCH}/ra-helper"
        return 0
    fi

    if [ ! -f "$REPO_ROOT/src/ra-helper/Makefile" ]; then
        return 0
    fi

    local_cc=""
    if [ "$ARCH" = "arm64" ]; then
        if [ "$(uname -s)" = "Linux" ] && [ "$(uname -m)" = "aarch64" ]; then
            local_cc="gcc"
        elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
            local_cc="aarch64-linux-gnu-gcc"
        elif command -v aarch64-unknown-linux-gnu-gcc >/dev/null 2>&1; then
            local_cc="aarch64-unknown-linux-gnu-gcc"
        fi
    else
        if command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
            local_cc="arm-linux-gnueabihf-gcc"
        elif command -v arm-unknown-linux-gnueabihf-gcc >/dev/null 2>&1; then
            local_cc="arm-unknown-linux-gnueabihf-gcc"
        fi
    fi

    if [ -n "$local_cc" ] && command -v cmake >/dev/null 2>&1; then
        log "Building bin/${ARCH}/ra-helper locally (${local_cc})..."
        make -C "$REPO_ROOT/src/ra-helper" "$ARCH"
        return 0
    fi

    if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        log "Building bin/${ARCH}/ra-helper via Docker..."
        docker run --rm -v "$REPO_ROOT:/work" -w /work/src/ra-helper ubuntu:24.04 sh -c \
            "set -e; \
            extra_pkg=''; \
            if [ '${ARCH}' = 'arm64' ] && [ \"\$(uname -m)\" != 'aarch64' ]; then extra_pkg='gcc-aarch64-linux-gnu'; fi; \
            if [ '${ARCH}' = 'arm' ]; then extra_pkg='gcc-arm-linux-gnueabihf'; fi; \
            apt-get update -qq && apt-get install -y -qq build-essential cmake git \$extra_pkg && make ${ARCH}"
        return 0
    fi

    warn "Skipping bin/${ARCH}/ra-helper (install Docker/Colima, or cmake + aarch64-unknown-linux-gnu-gcc on macOS, to enable RetroAchievements support)."
}

# ---------------------------------------------------------------------------
# Stage pak contents cleanly (working tree + platform binary only)
# ---------------------------------------------------------------------------

STAGE_DIR=""
cleanup_stage() {
    if [ -n "$STAGE_DIR" ] && [ -d "$STAGE_DIR" ]; then
        rm -rf "$STAGE_DIR"
    fi
}
trap cleanup_stage EXIT INT TERM

stage_pak() {
    [ -f "$REPO_ROOT/launch.sh" ] || die "Missing launch.sh in $REPO_ROOT"
    [ -f "$REPO_ROOT/pak.json" ] || die "Missing pak.json in $REPO_ROOT"

    STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/deploy-pak.XXXXXX")"

    cp "$REPO_ROOT/launch.sh" "$STAGE_DIR/launch.sh"
    chmod +x "$STAGE_DIR/launch.sh"
    cp "$REPO_ROOT/pak.json" "$STAGE_DIR/pak.json"

    mkdir -p "$STAGE_DIR/bin/${PLATFORM}"
    if [ "$DRY_RUN" -eq 0 ]; then
        cp "$REPO_ROOT/bin/${PLATFORM}/minui-presenter" "$STAGE_DIR/bin/${PLATFORM}/minui-presenter"
        chmod +x "$STAGE_DIR/bin/${PLATFORM}/minui-presenter"

        if [ -f "$REPO_ROOT/bin/${PLATFORM}/minui-presenter-nextui" ]; then
            cp "$REPO_ROOT/bin/${PLATFORM}/minui-presenter-nextui" "$STAGE_DIR/bin/${PLATFORM}/minui-presenter-nextui"
            chmod +x "$STAGE_DIR/bin/${PLATFORM}/minui-presenter-nextui"
        fi

        if [ -f "$REPO_ROOT/bin/${ARCH}/ra-helper" ]; then
            mkdir -p "$STAGE_DIR/bin/${ARCH}"
            cp "$REPO_ROOT/bin/${ARCH}/ra-helper" "$STAGE_DIR/bin/${ARCH}/ra-helper"
            chmod +x "$STAGE_DIR/bin/${ARCH}/ra-helper"
        fi
    fi

    if [ -d "$REPO_ROOT/res" ]; then
        COPYFILE_DISABLE=1 cp -R "$REPO_ROOT/res" "$STAGE_DIR/res"
        find "$STAGE_DIR/res" \( -name '.gitkeep' -o -name '.DS_Store' -o -name '._*' \) -delete 2>/dev/null || true
    fi
}

# ---------------------------------------------------------------------------
# Deploy implementations
# ---------------------------------------------------------------------------

deploy_adb() {
    quoted_dir="$(shell_quote "$TARGET_PAK_DIR")"
    quoted_launch="$(shell_quote "$TARGET_PAK_DIR/launch.sh")"
    quoted_bin="$(shell_quote "$TARGET_PAK_DIR/bin/${PLATFORM}/minui-presenter")"
    quoted_nextui_bin="$(shell_quote "$TARGET_PAK_DIR/bin/${PLATFORM}/minui-presenter-nextui")"
    quoted_ra_bin="$(shell_quote "$TARGET_PAK_DIR/bin/${ARCH}/ra-helper")"

    if [ "$DRY_RUN" -eq 1 ]; then
        [ "$DEPLOY_CLEAN" -eq 1 ] && log "[dry-run] adb shell rm -rf $quoted_dir"
        log "[dry-run] adb shell mkdir -p $quoted_dir"
        log "[dry-run] adb push <staged-pak>/. $TARGET_PAK_DIR"
        log "[dry-run] adb shell chmod +x $quoted_launch $quoted_bin $quoted_nextui_bin $quoted_ra_bin"
        return 0
    fi

    if [ "$DEPLOY_CLEAN" -eq 1 ]; then
        log "Cleaning existing remote folder..."
        adb ${ADB_ARGS[@]+"${ADB_ARGS[@]}"} shell "rm -rf $quoted_dir"
    fi

    adb ${ADB_ARGS[@]+"${ADB_ARGS[@]}"} shell "mkdir -p $quoted_dir"
    adb ${ADB_ARGS[@]+"${ADB_ARGS[@]}"} push "$STAGE_DIR/." "$TARGET_PAK_DIR"
    adb ${ADB_ARGS[@]+"${ADB_ARGS[@]}"} shell "{ chmod +x $quoted_launch $quoted_bin $quoted_nextui_bin $quoted_ra_bin 2>/dev/null || true; } && test -f $quoted_launch"
}

deploy_ssh() {
    quoted_dir="$(shell_quote "$TARGET_PAK_DIR")"
    quoted_launch="$(shell_quote "$TARGET_PAK_DIR/launch.sh")"
    quoted_bin="$(shell_quote "$TARGET_PAK_DIR/bin/${PLATFORM}/minui-presenter")"
    quoted_nextui_bin="$(shell_quote "$TARGET_PAK_DIR/bin/${PLATFORM}/minui-presenter-nextui")"
    quoted_ra_bin="$(shell_quote "$TARGET_PAK_DIR/bin/${ARCH}/ra-helper")"

    remote_cmd=""
    if [ "$DEPLOY_CLEAN" -eq 1 ]; then
        remote_cmd="rm -rf $quoted_dir && "
    fi
    remote_cmd="${remote_cmd}mkdir -p $quoted_dir && tar -xf - -C $quoted_dir && { chmod +x $quoted_launch $quoted_bin $quoted_nextui_bin $quoted_ra_bin 2>/dev/null || true; } && test -f $quoted_launch"

    if [ "$DRY_RUN" -eq 1 ]; then
        log "[dry-run] tar --format=ustar -C <staged-pak> -cf - . | ssh ${DEPLOY_USER}@${DEPLOY_HOST} (port ${DEPLOY_PORT}) \"$remote_cmd\""
        return 0
    fi

    # Stream ustar tarball over a single SSH connection:
    # - avoids macOS OpenSSH scp->SFTP failures against Dropbear
    # - strips macOS resource forks / PAX extended headers
    # - prompts for password at most once
    COPYFILE_DISABLE=1 tar --format=ustar -C "$STAGE_DIR" -cf - . \
        | ssh ${SSH_ARGS[@]+"${SSH_ARGS[@]}"} "${DEPLOY_USER}@${DEPLOY_HOST}" "$remote_cmd"
}

deploy_local() {
    if [ "$DRY_RUN" -eq 1 ]; then
        [ "$DEPLOY_CLEAN" -eq 1 ] && log "[dry-run] rm -rf \"$TARGET_PAK_DIR\""
        log "[dry-run] mkdir -p \"$TARGET_PAK_DIR\""
        log "[dry-run] copy staged files -> \"$TARGET_PAK_DIR\""
        return 0
    fi

    if [ "$DEPLOY_CLEAN" -eq 1 ]; then
        rm -rf "$TARGET_PAK_DIR"
    fi
    mkdir -p "$TARGET_PAK_DIR"
    COPYFILE_DISABLE=1 cp -R "$STAGE_DIR/." "$TARGET_PAK_DIR/"
    chmod +x \
        "$TARGET_PAK_DIR/launch.sh" \
        "$TARGET_PAK_DIR/bin/${PLATFORM}/minui-presenter" \
        "$TARGET_PAK_DIR/bin/${PLATFORM}/minui-presenter-nextui" \
        "$TARGET_PAK_DIR/bin/${ARCH}/ra-helper" 2>/dev/null || true
    # Clean any stray macOS metadata files on FAT32/exFAT SD cards
    find "$TARGET_PAK_DIR" \( -name '._*' -o -name '.DS_Store' \) -delete 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

log "Method:   $DEPLOY_METHOD"
log "Platform: $PLATFORM ($ARCH)"
log "Target:   $TARGET_PAK_DIR"
if [ "$DEPLOY_METHOD" = "ssh" ]; then
    log "Host:     ${DEPLOY_USER}@${DEPLOY_HOST}:${DEPLOY_PORT}"
fi

case "$DEPLOY_METHOD" in
    adb) check_adb ;;
    ssh) check_ssh ;;
    local) check_local ;;
esac

ensure_presenter_binary
ensure_ra_helper_binary
stage_pak

case "$DEPLOY_METHOD" in
    adb) deploy_adb ;;
    ssh) deploy_ssh ;;
    local) deploy_local ;;
esac

if [ "$DRY_RUN" -eq 1 ]; then
    log "Dry run complete."
else
    log "Deployed successfully!"
    log "On your handheld, launch: Tools > ${PAK_NAME}"
    if [ "$DEPLOY_METHOD" = "local" ]; then
        log "Remember to safely eject ${DEPLOY_SDCARD} before removing the SD card."
    else
        log "View logs after running with: ./scripts/deploy.sh --logs-only"
    fi
fi

if [ "$SHOW_LOGS" -eq 1 ]; then
    show_logs
fi
