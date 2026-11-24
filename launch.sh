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
PRESENTER_PID=""

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
    killall minui-presenter >/dev/null 2>&1 || true
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
        minui-presenter --message "$message" --timeout -1 &
        PRESENTER_PID=$!
    else
        minui-presenter --message "$message" --timeout "$seconds"
    fi
}

show_preview() {
    TITLE="$1"
    SYSTEM="$2"
    COVER_ART="$3"

    stop_presenter

    MESSAGE="${TITLE}\n${SYSTEM}"
    if [ -n "$COVER_ART" ]; then
        minui-presenter \
            --message "$MESSAGE" \
            --message-alignment top \
            --background-image "$COVER_ART" \
            --show-pill \
            --confirm-button A --confirm-text "PLAY" --confirm-show \
            --action-button Y --action-text "REROLL" --action-show \
            --cancel-button B --cancel-text "QUIT" --cancel-show \
            --timeout 0
    else
        minui-presenter \
            --message "$MESSAGE" \
            --message-alignment middle \
            --confirm-button A --confirm-text "PLAY" --confirm-show \
            --action-button Y --action-text "REROLL" --action-show \
            --cancel-button B --cancel-text "QUIT" --cancel-show \
            --timeout 0
    fi
}

cleanup() {
    rm -f /tmp/stay_awake "$ROM_LIST"
    stop_presenter
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

    if ! command -v minui-presenter >/dev/null 2>&1; then
        show_message "minui-presenter not found" 2
        return 1
    fi

    show_message "Finding a random game..."
    build_rom_list "$SDCARD_PATH/Roms" "$ROM_LIST"

    TOTAL="$(wc -l <"$ROM_LIST" | tr -d '[:space:]')"
    if [ -z "$TOTAL" ] || [ "$TOTAL" -eq 0 ]; then
        show_message "Could not find any games." 2
        return 1
    fi

    PREV_INDEX=""
    ROLL_COUNTER=0
    NEED_ROLL=1

    while true; do
        if [ "$NEED_ROLL" -eq 1 ]; then
            ROLL_COUNTER=$((ROLL_COUNTER + 1))
            INDEX="$(pick_random_index "$TOTAL" "$PREV_INDEX" "$ROLL_COUNTER")"
            PREV_INDEX="$INDEX"
            FILE="$(get_rom_at_index "$ROM_LIST" "$INDEX")"

            EMU_FOLDER="$(get_emu_folder "$FILE")"
            EMU_NAME="$(get_emu_name "$EMU_FOLDER")"
            EMU_PATH="$(get_emu_path "$EMU_NAME")"
            if [ -z "$EMU_PATH" ]; then
                show_message "Could not find an emulator for this game." 2
                return 1
            fi

            ROM_ALIAS="$(get_rom_alias "$FILE")"
            SYSTEM_NAME="$(get_system_name "$EMU_FOLDER")"
            COVER_ART="$(get_cover_art "$FILE" || true)"
            NEED_ROLL=0
        fi

        show_preview "$ROM_ALIAS" "$SYSTEM_NAME" "$COVER_ART"
        EXIT_CODE=$?

        case "$EXIT_CODE" in
            0)
                break
                ;;
            4)
                NEED_ROLL=1
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

    rm -f /tmp/stay_awake "$ROM_LIST"

    add_game_to_recents "$FILE" "$ROM_ALIAS"
    stop_presenter
    exec "$EMU_PATH" "$FILE"
}

main "$@"
