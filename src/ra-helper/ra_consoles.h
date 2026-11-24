#ifndef RA_CONSOLES_H
#define RA_CONSOLES_H

#include <rc_consoles.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *emu_tag;
    int console_id;
} RA_ConsoleMapping;

/*
 * Maps NextUI/MinUI emulator tags (folder tag in parentheses, e.g. "(GBA)")
 * to rcheevos RC_CONSOLE_* IDs. Includes all NextUI mappings plus common
 * MinUI/NextUI aliases for future-proofing across platforms.
 */
static const RA_ConsoleMapping ra_console_table[] = {
    { "32X",     RC_CONSOLE_SEGA_32X },
    { "3DO",     RC_CONSOLE_3DO },
    { "A2600",   RC_CONSOLE_ATARI_2600 },
    { "A5200",   RC_CONSOLE_ATARI_5200 },
    { "A7800",   RC_CONSOLE_ATARI_7800 },
    { "AMIGA",   RC_CONSOLE_AMIGA },
    { "ARCADE",  RC_CONSOLE_ARCADE },
    { "C128",    RC_CONSOLE_COMMODORE_64 },
    { "C64",     RC_CONSOLE_COMMODORE_64 },
    { "COLECO",  RC_CONSOLE_COLECOVISION },
    { "CPC",     RC_CONSOLE_AMSTRAD_PC },
    { "CPS1",    RC_CONSOLE_ARCADE },
    { "CPS2",    RC_CONSOLE_ARCADE },
    { "CPS3",    RC_CONSOLE_ARCADE },
    { "DC",      RC_CONSOLE_DREAMCAST },
    { "FC",      RC_CONSOLE_NINTENDO },
    { "FBN",     RC_CONSOLE_ARCADE },
    { "FBNEO",   RC_CONSOLE_ARCADE },
    { "FDS",     RC_CONSOLE_FAMICOM_DISK_SYSTEM },
    { "GB",      RC_CONSOLE_GAMEBOY },
    { "GBA",     RC_CONSOLE_GAMEBOY_ADVANCE },
    { "GBC",     RC_CONSOLE_GAMEBOY_COLOR },
    { "GEN",     RC_CONSOLE_MEGA_DRIVE },
    { "GG",      RC_CONSOLE_GAME_GEAR },
    { "INTV",    RC_CONSOLE_INTELLIVISION },
    { "JAG",     RC_CONSOLE_ATARI_JAGUAR },
    { "LYNX",    RC_CONSOLE_ATARI_LYNX },
    { "MAME",    RC_CONSOLE_ARCADE },
    { "MD",      RC_CONSOLE_MEGA_DRIVE },
    { "MGBA",    RC_CONSOLE_GAMEBOY_ADVANCE },
    { "MSX",     RC_CONSOLE_MSX },
    { "N64",     RC_CONSOLE_NINTENDO_64 },
    { "NDS",     RC_CONSOLE_NINTENDO_DS },
    { "NEOGEO",  RC_CONSOLE_ARCADE },
    { "NES",     RC_CONSOLE_NINTENDO },
    { "NGP",     RC_CONSOLE_NEOGEO_POCKET },
    { "NGPC",    RC_CONSOLE_NEOGEO_POCKET },
    { "P8",      RC_CONSOLE_PICO },
    { "PCE",     RC_CONSOLE_PC_ENGINE },
    { "PCECD",   RC_CONSOLE_PC_ENGINE_CD },
    { "PET",     RC_CONSOLE_UNKNOWN },
    { "PKM",     RC_CONSOLE_POKEMON_MINI },
    { "PLUS4",   RC_CONSOLE_UNKNOWN },
    { "POKE",    RC_CONSOLE_POKEMON_MINI },
    { "PRBOOM",  RC_CONSOLE_UNKNOWN },
    { "PS",      RC_CONSOLE_PLAYSTATION },
    { "PSP",     RC_CONSOLE_PSP },
    { "PSX",     RC_CONSOLE_PLAYSTATION },
    { "PUAE",    RC_CONSOLE_AMIGA },
    { "SAT",     RC_CONSOLE_SATURN },
    { "SATURN",  RC_CONSOLE_SATURN },
    { "SEGACD",  RC_CONSOLE_SEGA_CD },
    { "SFC",     RC_CONSOLE_SUPER_NINTENDO },
    { "SG1000",  RC_CONSOLE_SG1000 },
    { "SGB",     RC_CONSOLE_GAMEBOY },
    { "SMS",     RC_CONSOLE_MASTER_SYSTEM },
    { "SNES",    RC_CONSOLE_SUPER_NINTENDO },
    { "SUPA",    RC_CONSOLE_SUPER_NINTENDO },
    { "VB",      RC_CONSOLE_VIRTUAL_BOY },
    { "VECTREX", RC_CONSOLE_VECTREX },
    { "VIC",     RC_CONSOLE_VIC20 },
    { "WS",      RC_CONSOLE_WONDERSWAN },
    { "WSC",     RC_CONSOLE_WONDERSWAN },
};

#define RA_CONSOLE_TABLE_SIZE (sizeof(ra_console_table) / sizeof(ra_console_table[0]))

static inline int ra_is_cd_extension(const char *path) {
    if (!path) return 0;
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    return strcasecmp(ext, ".chd") == 0 ||
           strcasecmp(ext, ".cue") == 0 ||
           strcasecmp(ext, ".iso") == 0 ||
           strcasecmp(ext, ".ccd") == 0 ||
           strcasecmp(ext, ".m3u") == 0;
}

static inline int ra_is_explicitly_unsupported_emu(const char *emu_tag) {
    if (!emu_tag || !*emu_tag) return 0;
    return strcasecmp(emu_tag, "PET") == 0 ||
           strcasecmp(emu_tag, "PLUS4") == 0 ||
           strcasecmp(emu_tag, "PRBOOM") == 0;
}

static inline int ra_get_console_id(const char *emu_tag, const char *rom_path) {
    if (!emu_tag || !*emu_tag) {
        return RC_CONSOLE_UNKNOWN;
    }

    int console_id = RC_CONSOLE_UNKNOWN;
    for (size_t i = 0; i < RA_CONSOLE_TABLE_SIZE; i++) {
        if (strcasecmp(emu_tag, ra_console_table[i].emu_tag) == 0) {
            console_id = ra_console_table[i].console_id;
            break;
        }
    }

    /* Match NextUI CD console overrides for multi-media tags */
    if (console_id == RC_CONSOLE_PC_ENGINE && ra_is_cd_extension(rom_path)) {
        console_id = RC_CONSOLE_PC_ENGINE_CD;
    } else if (console_id == RC_CONSOLE_MEGA_DRIVE && ra_is_cd_extension(rom_path)) {
        console_id = RC_CONSOLE_SEGA_CD;
    }

    return console_id;
}

#endif /* RA_CONSOLES_H */
