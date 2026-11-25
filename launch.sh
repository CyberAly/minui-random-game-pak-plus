#!/bin/sh
PAK_DIR="$(dirname "$0")"
PAK_NAME="$(basename "$PAK_DIR")"
PAK_NAME="${PAK_NAME%.*}"
set -x

rm -f "$LOGS_PATH/$PAK_NAME.txt"
exec >>"$LOGS_PATH/$PAK_NAME.txt"
exec 2>&1

echo "$0" "$@"
cd "$PAK_DIR" || exit 1
mkdir -p "$USERDATA_PATH/$PAK_NAME"

architecture=arm
if uname -m | grep -q '64'; then
    architecture=arm64
fi

export HOME="$USERDATA_PATH/$PAK_NAME"
export LD_LIBRARY_PATH="$PAK_DIR/lib:$LD_LIBRARY_PATH"
export PATH="$PAK_DIR/bin/$architecture:$PAK_DIR/bin/$PLATFORM:$PAK_DIR/bin:$PATH"

if [ "$DEVICE" = "rgsp" ]; then
    export RGXX_MODEL="RG34xx"
fi

ROM_LIST="/tmp/random_game_roms_$$.txt"
SHUFFLE_LIST="/tmp/random_game_shuffle_$$.txt"
MENU_INPUT="/tmp/random_game_menu_in_$$.txt"
MENU_OUTPUT="/tmp/random_game_menu_out_$$.txt"
PRESENTER_PID=""
PRESENTER_BIN="minui-presenter"
LIST_BIN="minui-list"
HAS_LIST_BIN=0
RA_ENABLED=0
RA_CACHE_DIR="$USERDATA_PATH/$PAK_NAME/ra_cache"
RA_DISPLAY=""
RA_STATUS=""
RA_TOTAL=0
RA_HAS_ACHIEVEMENTS=0
FILTER_RA_ONLY=0

is_nextui() {
    [ -f "$SDCARD_PATH/.userdata/shared/minuisettings.txt" ]
}

select_presenter() {
    PRESENTER_BIN="minui-presenter"
    if is_nextui && command -v minui-presenter-nextui >/dev/null 2>&1; then
        PRESENTER_BIN="minui-presenter-nextui"
    fi
}

select_list() {
    LIST_BIN="minui-list"
    if is_nextui && command -v minui-list-nextui >/dev/null 2>&1; then
        LIST_BIN="minui-list-nextui"
    fi
}

init_retroachievements() {
    RA_ENABLED=0
    if ! is_nextui; then
        return 0
    fi
    if ! command -v ra-helper >/dev/null 2>&1; then
        echo "ra-helper not found; skipping RetroAchievements display." 1>&2
        return 0
    fi
    if ra-helper check-config --sdcard "$SDCARD_PATH" >/dev/null 2>&1; then
        RA_ENABLED=1
    fi
}

lookup_retroachievements() {
    ROM_FILE="$1"
    EMU_TAG="$2"
    OFFLINE_ONLY="${3:-0}"
    RA_DISPLAY=""
    RA_STATUS=""
    RA_TOTAL=0
    RA_HAS_ACHIEVEMENTS=0

    if [ "$RA_ENABLED" -ne 1 ]; then
        return 0
    fi

    if [ "$OFFLINE_ONLY" -eq 1 ]; then
        ra_out="$(ra-helper lookup \
            --sdcard "$SDCARD_PATH" \
            --cache-dir "$RA_CACHE_DIR" \
            --rom "$ROM_FILE" \
            --emu "$EMU_TAG" \
            --offline-only 2>/dev/null || true)"
    else
        ra_out="$(ra-helper lookup \
            --sdcard "$SDCARD_PATH" \
            --cache-dir "$RA_CACHE_DIR" \
            --rom "$ROM_FILE" \
            --emu "$EMU_TAG" 2>/dev/null || true)"
    fi

    if [ -z "$ra_out" ]; then
        return 0
    fi

    ra_status=""
    ra_earned="0"
    ra_total="0"
    ra_pending="0"
    ra_offline_ready="0"

    while IFS='=' read -r key val; do
        case "$key" in
            status) ra_status="$val" ;;
            earned) ra_earned="$val" ;;
            total) ra_total="$val" ;;
            pending) ra_pending="$val" ;;
            offline_ready) ra_offline_ready="$val" ;;
        esac
    done <<EOF
$ra_out
EOF

    RA_STATUS="$ra_status"
    if [ "$ra_total" -ge 0 ] 2>/dev/null; then
        RA_TOTAL="$ra_total"
    fi

    case "$ra_status" in
        ok)
            if [ "$RA_TOTAL" -ge 1 ] 2>/dev/null; then
                RA_HAS_ACHIEVEMENTS=1
            fi
            if [ "$ra_offline_ready" = "1" ]; then
                if [ "$ra_pending" -gt 0 ] 2>/dev/null; then
                    RA_DISPLAY="RA: ${ra_earned}/${ra_total}\nRA Offline Ready (${ra_pending} Unsynced)"
                else
                    RA_DISPLAY="RA: ${ra_earned}/${ra_total}\nRA Offline Ready"
                fi
            else
                RA_DISPLAY="RA: ${ra_earned}/${ra_total}\nWiFi Required for RA"
            fi
            ;;
        none)
            RA_DISPLAY="RA None"
            ;;
        incompatible)
            RA_DISPLAY="RA Incompatible ROM"
            ;;
        unsupported)
            RA_DISPLAY="RA Unsupported"
            ;;
        wifi_required)
            if [ "$RA_TOTAL" -ge 1 ] 2>/dev/null; then
                RA_HAS_ACHIEVEMENTS=1
                RA_DISPLAY="RA: ${ra_total} Achievements\nWiFi Required for RA"
            else
                RA_DISPLAY="WiFi Required for RA"
            fi
            ;;
        auth_invalid)
            echo "RetroAchievements credentials invalid; disabling RA display for this session." 1>&2
            RA_ENABLED=0
            RA_DISPLAY=""
            ;;
        disabled | "")
            RA_DISPLAY=""
            ;;
    esac
}

build_rom_list() {
    ROMS_DIR="$1"
    OUT_FILE="$2"
    : >"$OUT_FILE"

    for emu_dir in "$ROMS_DIR"/*; do
        [ -d "$emu_dir" ] || continue
        emu_folder="$(basename "$emu_dir")"
        case "$emu_folder" in
            *.disabled) continue ;;
        esac

        emu_name="$(get_emu_name "$emu_folder")"
        [ -n "$emu_name" ] || continue
        get_emu_path "$emu_name" >/dev/null || continue

        find "$emu_dir" -type f \
            ! -path '*/.*' \
            ! -path '*/Imgs/*' \
            ! -name '*.txt' \
            ! -name '*.log' \
            ! -name '*.png' \
            ! -name '*.jpg' \
            ! -name '*.jpeg' \
            ! -name '*.bmp' \
            ! -name '*.webp' \
            ! -name '*.xml' \
            ! -name '*.json' \
            ! -name '*.db' \
            ! -name '*.sav' \
            ! -name '*.srm' \
            >>"$OUT_FILE"
    done
}

get_random_number() {
    roll_seed="${1:-0}"
    num=""
    if [ -r /dev/urandom ]; then
        bytes=""
        if command -v od >/dev/null 2>&1; then
            bytes="$(dd if=/dev/urandom bs=3 count=1 2>/dev/null | od -An -tu1 2>/dev/null)"
        fi
        if [ -z "$bytes" ] && command -v hexdump >/dev/null 2>&1; then
            bytes="$(dd if=/dev/urandom bs=3 count=1 2>/dev/null | hexdump -v -e '3/1 "%u "' 2>/dev/null)"
        fi
        if [ -n "$bytes" ]; then
            # shellcheck disable=SC2086
            set -- $bytes
            if [ $# -eq 3 ]; then
                num="$((($1 * 65536) + ($2 * 256) + $3))"
            fi
        fi
    fi

    if [ -z "$num" ]; then
        num="$((($(date +%s) * 1103515245 + $$ + roll_seed * 12345) & 2147483647))"
    fi

    echo "$num"
}

pick_random_index() {
    total="$1"
    prev_index="$2"
    roll_seed="$3"

    r="$(($(get_random_number "$roll_seed") % total + 1))"
    if [ "$total" -gt 1 ] && [ "$r" = "$prev_index" ]; then
        r="$((r % total + 1))"
    fi
    echo "$r"
}

get_rom_at_index() {
    list_file="$1"
    index="$2"
    sed -n "${index}p;${index}q" "$list_file"
}

add_game_to_recents() {
    FILEPATH="$1" GAME_ALIAS="$2"

    FILEPATH="${FILEPATH#"$SDCARD_PATH/"}"
    RECENTS="$SDCARD_PATH/.userdata/shared/.minui/recent.txt"
    if [ -f "$RECENTS" ]; then
        sed -i "#/$FILEPATH\t$GAME_ALIAS#d" "$RECENTS"
    fi

    rm -f "/tmp/recent.txt"
    touch "/tmp/recent.txt"
    printf "%s\t%s\n" "/$FILEPATH" "$GAME_ALIAS" >"/tmp/recent.txt"
    if [ -f "$RECENTS" ]; then
        cat "$RECENTS" >>"/tmp/recent.txt"
    fi
    mv "/tmp/recent.txt" "$RECENTS"
}

get_rom_alias() {
    FILEPATH="$1"
    rom_dir="$(dirname "$FILEPATH")"
    filename="$(basename "$FILEPATH")"

    if [ -f "$rom_dir/map.txt" ]; then
        mapped="$(awk -F '\t' -v f="$filename" '$1 == f { print $2; exit }' "$rom_dir/map.txt" | tr -d '\r')"
        if [ -n "$mapped" ]; then
            echo "$mapped"
            return
        fi
    fi

    filename="${filename%.*}"
    filename="$(echo "$filename" | sed 's/([^)]*)//g' | sed 's/\[[^]]*\]//g' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    echo "$filename"
}

get_emu_folder() {
    FILEPATH="$1"
    ROMS="$SDCARD_PATH/Roms"

    echo "${FILEPATH#"$ROMS/"}" | cut -d'/' -f1
}

get_emu_name() {
    EMU_FOLDER="$1"

    echo "$EMU_FOLDER" | sed 's/.*(\([^)]*\)).*/\1/'
}

get_system_name() {
    EMU_FOLDER="$1"
    name="$(echo "$EMU_FOLDER" | sed 's/^[0-9][0-9]*)[[:space:]]*//' | sed 's/[[:space:]]*([^)]*)//g' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    if [ -z "$name" ]; then
        get_emu_name "$EMU_FOLDER"
    else
        echo "$name"
    fi
}

get_emu_path() {
    EMU_NAME="$1"
    platform_emu="$SDCARD_PATH/Emus/$PLATFORM/${EMU_NAME}.pak/launch.sh"
    if [ -f "$platform_emu" ]; then
        echo "$platform_emu"
        return
    fi

    pak_emu="$SDCARD_PATH/.system/$PLATFORM/paks/Emus/${EMU_NAME}.pak/launch.sh"
    if [ -f "$pak_emu" ]; then
        echo "$pak_emu"
        return
    fi

    return 1
}

get_cover_art() {
    FILEPATH="$1"
    rom_dir="$(dirname "$FILEPATH")"
    rom_file="$(basename "$FILEPATH")"
    rom_base="${rom_file%.*}"
    parent_dir="$(dirname "$rom_dir")"

    for candidate in \
        "$rom_dir/.media/${rom_base}.png" \
        "$rom_dir/.media/${rom_base}.jpg" \
        "$rom_dir/.media/${rom_base}.jpeg" \
        "$rom_dir/.media/${rom_file}.png" \
        "$rom_dir/Imgs/${rom_base}.png" \
        "$rom_dir/Imgs/${rom_base}.jpg" \
        "$parent_dir/.media/${rom_base}.png" \
        "$parent_dir/.media/${rom_base}.jpg"; do
        if [ -f "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done

    return 1
}

stop_presenter() {
    if [ -n "$PRESENTER_PID" ]; then
        kill "$PRESENTER_PID" >/dev/null 2>&1 || true
        wait "$PRESENTER_PID" 2>/dev/null || true
        PRESENTER_PID=""
    fi
    killall minui-presenter minui-presenter-nextui >/dev/null 2>&1 || true
}

show_message() {
    message="$1"
    seconds="$2"

    if [ -z "$seconds" ]; then
        seconds="forever"
    fi

    stop_presenter
    echo "$message" 1>&2
    if [ "$seconds" = "forever" ]; then
        "$PRESENTER_BIN" --message "$message" --timeout -1 &
        PRESENTER_PID=$!
    else
        "$PRESENTER_BIN" --message "$message" --timeout "$seconds"
    fi
}

active_filter_count() {
    count=0
    if [ "$FILTER_RA_ONLY" -eq 1 ]; then
        count=$((count + 1))
    fi
    echo "$count"
}

has_active_filters() {
    [ "$(active_filter_count)" -gt 0 ]
}

clear_all_filters() {
    FILTER_RA_ONLY=0
}

# Hook for future static/metadata filters (console, genre, hidden games, etc.)
# evaluated before running heavier lookups such as RetroAchievements hashing/API calls.
rom_matches_static_filters() {
    _cand_file="$1"
    _cand_emu_folder="$2"
    _cand_emu_name="$3"
    return 0
}

build_shuffled_rom_list() {
    src_file="$1"
    prev_file="$2"
    seed="$3"
    out_file="$4"

    awk -v prev="$prev_file" -v seed="$seed" '
    {
        lines[++n] = $0
    }
    END {
        srand(seed)
        for (i = n; i > 1; i--) {
            j = int(rand() * i) + 1
            tmp = lines[i]; lines[i] = lines[j]; lines[j] = tmp
        }
        has_prev = 0
        for (i = 1; i <= n; i++) {
            if (prev != "" && lines[i] == prev && n > 1) {
                has_prev = 1
            } else {
                print lines[i]
            }
        }
        if (has_prev) {
            print prev
        }
    }' "$src_file" >"$out_file"
}

show_preview() {
    TITLE="$1"
    SYSTEM="$2"
    COVER_ART="$3"
    RA_INFO="$4"

    stop_presenter

    MESSAGE="${TITLE}\n${SYSTEM}"
    if [ -n "$RA_INFO" ]; then
        MESSAGE="${MESSAGE}\n${RA_INFO}"
    fi

    if [ -n "$COVER_ART" ]; then
        if [ "$RA_ENABLED" -eq 1 ]; then
            # Offset top-aligned message by one row so it clears the top-right hardware bar
            set -- \
                --message "\n${MESSAGE}" \
                --message-alignment top \
                --background-image "$COVER_ART" \
                --show-pill \
                --show-hardware-group
        else
            set -- \
                --message "$MESSAGE" \
                --message-alignment top \
                --background-image "$COVER_ART" \
                --show-pill
        fi
    else
        if [ "$RA_ENABLED" -eq 1 ]; then
            set -- \
                --message "$MESSAGE" \
                --message-alignment middle \
                --show-hardware-group
        else
            set -- \
                --message "$MESSAGE" \
                --message-alignment middle
        fi
    fi

    if [ "$HAS_LIST_BIN" -eq 1 ]; then
        set -- "$@" --inaction-button X --inaction-text "OPTIONS" --inaction-show
    fi

    "$PRESENTER_BIN" "$@" \
        --confirm-button A --confirm-text "PLAY" --confirm-show \
        --action-button Y --action-text "REROLL" --action-show \
        --cancel-button B --cancel-text "QUIT" --cancel-show \
        --timeout 0
}

run_menu() {
    menu_title="$1"
    menu_file="$2"
    out_file="$3"

    : >"$out_file"
    stop_presenter

    "$LIST_BIN" \
        --file "$menu_file" \
        --format text \
        --title "$menu_title" \
        --write-location "$out_file" \
        --confirm-button A --confirm-text "SELECT" \
        --cancel-button B --cancel-text "BACK" \
        --scroll-method pong \
        --disable-auto-sleep
}

show_filters_menu() {
    while true; do
        : >"$MENU_INPUT"
        if [ "$FILTER_RA_ONLY" -eq 1 ]; then
            printf '%s\n' "Games with RetroAchievements: On" >>"$MENU_INPUT"
        else
            printf '%s\n' "Games with RetroAchievements: Off" >>"$MENU_INPUT"
        fi
        # Future filters (console, genre, etc.) can be appended here.
        if has_active_filters; then
            printf '%s\n' "Clear All Filters" >>"$MENU_INPUT"
        fi

        run_menu "Filters" "$MENU_INPUT" "$MENU_OUTPUT"
        menu_rc=$?

        case "$menu_rc" in
            0)
                selection="$(tr -d '\r\n' <"$MENU_OUTPUT")"
                case "$selection" in
                    "Games with RetroAchievements: Off")
                        if [ "$RA_ENABLED" -ne 1 ]; then
                            if ! is_nextui; then
                                show_message "RetroAchievements requires NextUI." 2
                            elif ! command -v ra-helper >/dev/null 2>&1; then
                                show_message "ra-helper not found." 2
                            else
                                show_message "RetroAchievements is not enabled\nor credentials are invalid." 2
                            fi
                            continue
                        fi
                        FILTER_RA_ONLY=1
                        return 0
                        ;;
                    "Games with RetroAchievements: On")
                        FILTER_RA_ONLY=0
                        return 0
                        ;;
                    "Clear All Filters")
                        clear_all_filters
                        return 0
                        ;;
                esac
                ;;
            2)
                return 2
                ;;
            3 | 130 | 143)
                return "$menu_rc"
                ;;
            *)
                return 2
                ;;
        esac
    done
}

show_more_options_menu() {
    while true; do
        : >"$MENU_INPUT"
        active_count="$(active_filter_count)"
        if [ "$active_count" -gt 0 ]; then
            printf 'Filters (%s Active)\n' "$active_count" >>"$MENU_INPUT"
        else
            printf '%s\n' "Filters" >>"$MENU_INPUT"
        fi
        # Future More Options items (e.g. "Hide Game") can be appended here.

        run_menu "More Options" "$MENU_INPUT" "$MENU_OUTPUT"
        menu_rc=$?

        case "$menu_rc" in
            0)
                selection="$(tr -d '\r\n' <"$MENU_OUTPUT")"
                case "$selection" in
                    Filters | "Filters ("*)
                        show_filters_menu
                        sub_rc=$?
                        case "$sub_rc" in
                            0)
                                return 0
                                ;;
                            2)
                                continue
                                ;;
                            3 | 130 | 143)
                                return "$sub_rc"
                                ;;
                        esac
                        ;;
                esac
                ;;
            2)
                return 2
                ;;
            3 | 130 | 143)
                return "$menu_rc"
                ;;
            *)
                return 2
                ;;
        esac
    done
}

roll_random_game() {
    if ! has_active_filters; then
        ROLL_COUNTER=$((ROLL_COUNTER + 1))
        INDEX="$(pick_random_index "$TOTAL" "$PREV_INDEX" "$ROLL_COUNTER")"
        CAND_FILE="$(get_rom_at_index "$ROM_LIST" "$INDEX")"
        if [ "$TOTAL" -gt 1 ] && [ "$CAND_FILE" = "$FILE" ]; then
            INDEX="$((INDEX % TOTAL + 1))"
            CAND_FILE="$(get_rom_at_index "$ROM_LIST" "$INDEX")"
        fi

        CAND_EMU_FOLDER="$(get_emu_folder "$CAND_FILE")"
        CAND_EMU_NAME="$(get_emu_name "$CAND_EMU_FOLDER")"
        CAND_EMU_PATH="$(get_emu_path "$CAND_EMU_NAME")"
        if [ -z "$CAND_EMU_PATH" ]; then
            show_message "Could not find an emulator for this game." 2
            return 1
        fi

        PREV_INDEX="$INDEX"
        FILE="$CAND_FILE"
        EMU_FOLDER="$CAND_EMU_FOLDER"
        EMU_NAME="$CAND_EMU_NAME"
        EMU_PATH="$CAND_EMU_PATH"
        ROM_ALIAS="$(get_rom_alias "$FILE")"
        SYSTEM_NAME="$(get_system_name "$EMU_FOLDER")"
        COVER_ART="$(get_cover_art "$FILE" || true)"
        lookup_retroachievements "$FILE" "$EMU_NAME"
        return 0
    fi

    SAVED_RA_DISPLAY="$RA_DISPLAY"
    if [ "$FILTER_RA_ONLY" -eq 1 ]; then
        show_message "Finding a game with RetroAchievements..."
    else
        show_message "Finding a matching game..."
    fi

    ROLL_COUNTER=$((ROLL_COUNTER + 1))
    SHUFFLE_SEED="$(get_random_number "$ROLL_COUNTER")"
    build_shuffled_rom_list "$ROM_LIST" "$FILE" "$SHUFFLE_SEED" "$SHUFFLE_LIST"

    FOUND=0
    CONSECUTIVE_WIFI_FAILS=0
    FORCE_OFFLINE=0
    SAW_WIFI_REQUIRED=0
    AUTH_FAILED=0

    while IFS= read -r CAND_FILE; do
        [ -n "$CAND_FILE" ] || continue

        CAND_EMU_FOLDER="$(get_emu_folder "$CAND_FILE")"
        CAND_EMU_NAME="$(get_emu_name "$CAND_EMU_FOLDER")"
        CAND_EMU_PATH="$(get_emu_path "$CAND_EMU_NAME" || true)"
        [ -n "$CAND_EMU_PATH" ] || continue

        rom_matches_static_filters "$CAND_FILE" "$CAND_EMU_FOLDER" "$CAND_EMU_NAME" || continue

        if [ "$FILTER_RA_ONLY" -eq 1 ]; then
            lookup_retroachievements "$CAND_FILE" "$CAND_EMU_NAME" "$FORCE_OFFLINE"
            if [ "$RA_ENABLED" -ne 1 ]; then
                AUTH_FAILED=1
                break
            fi
            if [ "$RA_HAS_ACHIEVEMENTS" -ne 1 ]; then
                if [ "$RA_STATUS" = "wifi_required" ]; then
                    SAW_WIFI_REQUIRED=1
                    CONSECUTIVE_WIFI_FAILS=$((CONSECUTIVE_WIFI_FAILS + 1))
                    if [ "$CONSECUTIVE_WIFI_FAILS" -ge 2 ]; then
                        FORCE_OFFLINE=1
                    fi
                else
                    CONSECUTIVE_WIFI_FAILS=0
                fi
                continue
            fi
        else
            lookup_retroachievements "$CAND_FILE" "$CAND_EMU_NAME"
        fi

        FOUND=1
        PREV_INDEX=""
        FILE="$CAND_FILE"
        EMU_FOLDER="$CAND_EMU_FOLDER"
        EMU_NAME="$CAND_EMU_NAME"
        EMU_PATH="$CAND_EMU_PATH"
        ROM_ALIAS="$(get_rom_alias "$FILE")"
        SYSTEM_NAME="$(get_system_name "$EMU_FOLDER")"
        COVER_ART="$(get_cover_art "$FILE" || true)"
        break
    done <"$SHUFFLE_LIST"

    if [ "$FOUND" -eq 1 ]; then
        return 0
    fi

    RA_DISPLAY="$SAVED_RA_DISPLAY"
    if [ "$AUTH_FAILED" -eq 1 ]; then
        FILTER_RA_ONLY=0
        RA_DISPLAY=""
        show_message "RetroAchievements login invalid.\nFilter disabled." 3
    elif [ "$FILTER_RA_ONLY" -eq 1 ] && [ "$SAW_WIFI_REQUIRED" -eq 1 ]; then
        FILTER_RA_ONLY=0
        show_message "No cached games with RetroAchievements found.\nConnect to Wi-Fi and try again." 3
    elif [ "$FILTER_RA_ONLY" -eq 1 ]; then
        FILTER_RA_ONLY=0
        show_message "No games with RetroAchievements found." 3
    else
        clear_all_filters
        show_message "No games matched your active filters." 3
    fi
    return 2
}

cleanup() {
    rm -f /tmp/stay_awake "$ROM_LIST" "$SHUFFLE_LIST" "$MENU_INPUT" "$MENU_OUTPUT"
    stop_presenter
    killall minui-list minui-list-nextui >/dev/null 2>&1 || true
}

main() {
    echo "1" >/tmp/stay_awake
    trap "cleanup" EXIT INT TERM HUP QUIT

    if [ "$PLATFORM" = "tg3040" ] && [ -z "$DEVICE" ]; then
        export DEVICE="brick"
        export PLATFORM="tg5040"
    fi

    allowed_platforms="h700 miyoomini my282 my355 rg35xxplus tg5040 tg5050"
    if ! echo "$allowed_platforms" | grep -q "$PLATFORM"; then
        show_message "$PLATFORM is not a supported platform" 2
        return 1
    fi

    select_presenter
    if ! command -v "$PRESENTER_BIN" >/dev/null 2>&1; then
        show_message "$PRESENTER_BIN not found" 2
        return 1
    fi

    select_list
    HAS_LIST_BIN=0
    if command -v "$LIST_BIN" >/dev/null 2>&1; then
        HAS_LIST_BIN=1
    fi

    init_retroachievements

    show_message "Finding a random game..."
    build_rom_list "$SDCARD_PATH/Roms" "$ROM_LIST"

    TOTAL="$(wc -l <"$ROM_LIST" | tr -d '[:space:]')"
    if [ -z "$TOTAL" ] || [ "$TOTAL" -eq 0 ]; then
        show_message "Could not find any games." 2
        return 1
    fi

    FILE=""
    EMU_FOLDER=""
    EMU_NAME=""
    EMU_PATH=""
    ROM_ALIAS=""
    SYSTEM_NAME=""
    COVER_ART=""
    PREV_INDEX=""
    ROLL_COUNTER=0
    NEED_ROLL=1

    while true; do
        if [ "$NEED_ROLL" -eq 1 ]; then
            roll_random_game
            ROLL_RC=$?
            if [ "$ROLL_RC" -eq 1 ]; then
                return 1
            fi
            if [ "$ROLL_RC" -ne 0 ] && [ -z "$FILE" ]; then
                return 1
            fi
            NEED_ROLL=0
        fi

        show_preview "$ROM_ALIAS" "$SYSTEM_NAME" "$COVER_ART" "$RA_DISPLAY"
        EXIT_CODE=$?

        case "$EXIT_CODE" in
            0)
                break
                ;;
            4)
                NEED_ROLL=1
                ;;
            5)
                if [ "$HAS_LIST_BIN" -eq 1 ]; then
                    show_more_options_menu
                    MENU_EXIT=$?
                    case "$MENU_EXIT" in
                        0)
                            NEED_ROLL=1
                            ;;
                        3 | 130 | 143)
                            return 0
                            ;;
                    esac
                fi
                ;;
            2 | 130 | 143)
                return 0
                ;;
            6)
                # Ignore START button; stay on current game
                ;;
            *)
                return 1
                ;;
        esac
    done

    rm -f /tmp/stay_awake "$ROM_LIST" "$SHUFFLE_LIST" "$MENU_INPUT" "$MENU_OUTPUT"

    add_game_to_recents "$FILE" "$ROM_ALIAS"
    stop_presenter
    exec "$EMU_PATH" "$FILE"
}

main "$@"
