/*
 * ra_helper.c - RetroAchievements lookup, ROM hashing, and NextUI offline cache reader
 *
 * Designed for minui-random-game-pak-plus:
 *   - Uses official rcheevos (rc_hash) + libchdr + miniz for 100% parity with NextUI ROM hashing
 *   - Reads NextUI settings and .ra/offline/ cache & ledger strictly READ-ONLY
 *   - Queries retroachievements.org/dorequest.php via read-only endpoints only
 *     (r=gameid, r=patch, r=unlocks&h=0) — never calls startsession
 *   - Maintains a separate pak-local cache under --cache-dir
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <rc_hash.h>
#include <miniz.h>

#include "chd_reader.h"
#include "ra_consoles.h"

#define RA_DEFAULT_API_URL "https://retroachievements.org/dorequest.php"
#define RA_USER_AGENT "minui-random-game-pak-plus/1.0"

/* Cache TTLs (seconds) */
#define RA_META_TTL_FOUND_SECS    (7 * 24 * 3600)   /* 7 days for recognized game metadata */
#define RA_META_TTL_MISSING_SECS  (3 * 24 * 3600)   /* 3 days for unsupported / no-set games */
#define RA_UNLOCKS_TTL_SECS       120               /* 2 minutes during active re-rolling */
#define RA_MAX_ZIP_ROM_BYTES      (64U * 1024U * 1024U)
#define RA_MAX_CACHE_FILE_BYTES   (8U * 1024U * 1024U)
#define RA_SHA256_LEN             32

/* NextUI ledger record types (from LoveRetro/NextUI ra_offline.h) */
#define RA_LEDGER_SESSION_START      0x01
#define RA_LEDGER_ACHIEVEMENT_UNLOCK 0x02
#define RA_LEDGER_SESSION_END        0x03
#define RA_LEDGER_SYNC_ACK           0x04

#pragma pack(push, 1)
typedef struct {
    uint8_t  type;
    uint32_t timestamp;
    uint32_t game_id;
    uint32_t achievement_id;
    uint8_t  hardcore;
    char     game_hash[33];
    uint8_t  prev_hash[32];
    uint8_t  record_hash[32];
} NextUILedgerRecord;
#pragma pack(pop)

_Static_assert(sizeof(NextUILedgerRecord) == 111, "NextUILedgerRecord must be 111 bytes");

/* =========================================================================
 * Dynamic uint32_t ID set
 * ========================================================================= */

typedef struct {
    uint32_t *items;
    size_t count;
    size_t cap;
} IdSet;

static void idset_init(IdSet *s) {
    s->items = NULL;
    s->count = 0;
    s->cap = 0;
}

static void idset_free(IdSet *s) {
    free(s->items);
    s->items = NULL;
    s->count = 0;
    s->cap = 0;
}

static bool idset_contains(const IdSet *s, uint32_t id) {
    for (size_t i = 0; i < s->count; i++) {
        if (s->items[i] == id) return true;
    }
    return false;
}

static bool idset_add(IdSet *s, uint32_t id) {
    if (id == 0 || idset_contains(s, id)) return true;
    if (s->count == s->cap) {
        size_t new_cap = s->cap ? s->cap * 2 : 32;
        uint32_t *p = (uint32_t *)realloc(s->items, new_cap * sizeof(uint32_t));
        if (!p) return false;
        s->items = p;
        s->cap = new_cap;
    }
    s->items[s->count++] = id;
    return true;
}

static void idset_remove(IdSet *s, uint32_t id) {
    for (size_t i = 0; i < s->count; i++) {
        if (s->items[i] == id) {
            s->items[i] = s->items[s->count - 1];
            s->count--;
            return;
        }
    }
}

static void idset_union(IdSet *dst, const IdSet *src) {
    for (size_t i = 0; i < src->count; i++) {
        idset_add(dst, src->items[i]);
    }
}

static size_t idset_count_intersection(const IdSet *a, const IdSet *b) {
    size_t n = 0;
    for (size_t i = 0; i < a->count; i++) {
        if (idset_contains(b, a->items[i])) n++;
    }
    return n;
}

static void idset_parse_csv(IdSet *dst, const char *csv) {
    if (!csv || !*csv) return;
    const char *p = csv;
    while (*p) {
        while (*p && (*p == ',' || isspace((unsigned char)*p))) p++;
        if (!*p) break;
        char *end = NULL;
        unsigned long val = strtoul(p, &end, 10);
        if (end == p) {
            while (*p && *p != ',') p++;
            continue;
        }
        if (val > 0 && val <= UINT32_MAX) {
            idset_add(dst, (uint32_t)val);
        }
        p = end;
    }
}

static void idset_write_csv(const IdSet *s, FILE *f) {
    for (size_t i = 0; i < s->count; i++) {
        if (i > 0) fputc(',', f);
        fprintf(f, "%u", s->items[i]);
    }
}

/* =========================================================================
 * Filesystem & string helpers
 * ========================================================================= */

static int mkdirs(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static bool file_exists_nonempty(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static time_t file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        return st.st_mtime;
    }
    return 0;
}

static void strip_eol(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static bool has_ext_ci(const char *path, const char *ext) {
    if (!path || !ext) return false;
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

static bool url_encode(const char *in, char *out, size_t out_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (j + 1 >= out_size) return false;
            out[j++] = (char)c;
        } else {
            if (j + 3 >= out_size) return false;
            out[j++] = '%';
            out[j++] = hex[(c >> 4) & 0xF];
            out[j++] = hex[c & 0xF];
        }
    }
    out[j] = '\0';
    return true;
}

/* =========================================================================
 * Minimal recursive-descent JSON reader (zero external deps)
 * ========================================================================= */

static const char *json_skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *json_skip_string(const char *p, const char *end) {
    if (p >= end || *p != '"') return NULL;
    p++;
    while (p < end) {
        if (*p == '\\') {
            p += 2;
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return NULL;
}

static const char *json_skip_value(const char *p, const char *end) {
    p = json_skip_ws(p, end);
    if (p >= end) return NULL;

    if (*p == '"') {
        return json_skip_string(p, end);
    } else if (*p == '{' || *p == '[') {
        char open_c = *p;
        char close_c = (open_c == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (p < end && depth > 0) {
            if (*p == '"') {
                p = json_skip_string(p, end);
                if (!p) return NULL;
                continue;
            }
            if (*p == open_c) depth++;
            else if (*p == close_c) depth--;
            p++;
        }
        return depth == 0 ? p : NULL;
    } else {
        while (p < end && *p != ',' && *p != '}' && *p != ']' &&
               *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            p++;
        }
        return p;
    }
}

/* Decode JSON string starting at opening '"' into out buffer. Returns pointer after closing '"'. */
static const char *json_read_string(const char *p, const char *end, char *out, size_t out_size) {
    p = json_skip_ws(p, end);
    if (p >= end || *p != '"') return NULL;
    p++;
    size_t j = 0;
    while (p < end && *p != '"') {
        char c = *p++;
        if (c == '\\' && p < end) {
            char esc = *p++;
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: c = esc; break;
            }
        }
        if (out && out_size > 0 && j + 1 < out_size) {
            out[j++] = c;
        }
    }
    if (out && out_size > 0) out[j] = '\0';
    if (p >= end || *p != '"') return NULL;
    return p + 1;
}

/*
 * Find top-level key inside JSON object [obj_start, obj_end).
 * obj_start should point to '{' (or whitespace before '{').
 * Writes value bounds [val_start, val_end) on match.
 */
static bool json_find_key(const char *obj_start, const char *obj_end, const char *key,
                          const char **val_start, const char **val_end) {
    const char *p = json_skip_ws(obj_start, obj_end);
    if (p >= obj_end || *p != '{') return false;
    p++;

    while (p < obj_end) {
        p = json_skip_ws(p, obj_end);
        if (p >= obj_end || *p == '}') return false;
        if (*p == ',') {
            p++;
            continue;
        }

        char cur_key[128];
        const char *after_key = json_read_string(p, obj_end, cur_key, sizeof(cur_key));
        if (!after_key) return false;

        p = json_skip_ws(after_key, obj_end);
        if (p >= obj_end || *p != ':') return false;
        p = json_skip_ws(p + 1, obj_end);

        const char *v_end = json_skip_value(p, obj_end);
        if (!v_end) return false;

        if (strcmp(cur_key, key) == 0) {
            if (val_start) *val_start = p;
            if (val_end) *val_end = v_end;
            return true;
        }
        p = v_end;
    }
    return false;
}

static int json_get_bool(const char *obj_start, const char *obj_end, const char *key) {
    const char *vs = NULL, *ve = NULL;
    if (!json_find_key(obj_start, obj_end, key, &vs, &ve)) return -1;
    size_t len = (size_t)(ve - vs);
    if (len == 4 && memcmp(vs, "true", 4) == 0) return 1;
    if (len == 5 && memcmp(vs, "false", 5) == 0) return 0;
    return -1;
}

static bool json_get_uint(const char *obj_start, const char *obj_end, const char *key, uint32_t *out) {
    const char *vs = NULL, *ve = NULL;
    if (!json_find_key(obj_start, obj_end, key, &vs, &ve)) return false;
    if (vs >= ve) return false;
    if (*vs == '"') {
        char buf[32];
        if (!json_read_string(vs, ve, buf, sizeof(buf))) return false;
        if (!isdigit((unsigned char)buf[0])) return false;
        *out = (uint32_t)strtoul(buf, NULL, 10);
        return true;
    }
    if (!isdigit((unsigned char)*vs)) return false;
    *out = (uint32_t)strtoul(vs, NULL, 10);
    return true;
}

static bool json_get_string(const char *obj_start, const char *obj_end, const char *key,
                            char *out, size_t out_size) {
    const char *vs = NULL, *ve = NULL;
    if (!json_find_key(obj_start, obj_end, key, &vs, &ve)) return false;
    return json_read_string(vs, ve, out, out_size) != NULL;
}

/* Parse an array of achievement objects [{ "ID": 123, "Flags": 3, ... }] and collect core (Flags == 3) IDs. */
static void parse_achievements_array(const char *arr_start, const char *arr_end, IdSet *core_ids) {
    const char *p = json_skip_ws(arr_start, arr_end);
    if (p >= arr_end || *p != '[') return;
    p++;

    while (p < arr_end) {
        p = json_skip_ws(p, arr_end);
        if (p >= arr_end || *p == ']') break;
        if (*p == ',') {
            p++;
            continue;
        }
        const char *elem_end = json_skip_value(p, arr_end);
        if (!elem_end) break;

        if (*p == '{') {
            uint32_t id = 0;
            uint32_t flags = 3; /* default to core if Flags omitted */
            if (json_get_uint(p, elem_end, "ID", &id) && id > 0) {
                json_get_uint(p, elem_end, "Flags", &flags);
                if (flags == 3) {
                    idset_add(core_ids, id);
                }
            }
        }
        p = elem_end;
    }
}

/* Parse startsession unlock array [{ "ID": 123, "When": ... }] */
static void parse_unlock_objects_array(const char *arr_start, const char *arr_end, IdSet *unlocks) {
    const char *p = json_skip_ws(arr_start, arr_end);
    if (p >= arr_end || *p != '[') return;
    p++;

    while (p < arr_end) {
        p = json_skip_ws(p, arr_end);
        if (p >= arr_end || *p == ']') break;
        if (*p == ',') {
            p++;
            continue;
        }
        const char *elem_end = json_skip_value(p, arr_end);
        if (!elem_end) break;

        if (*p == '{') {
            uint32_t id = 0;
            if (json_get_uint(p, elem_end, "ID", &id) && id > 0) {
                idset_add(unlocks, id);
            }
        } else if (isdigit((unsigned char)*p)) {
            uint32_t id = (uint32_t)strtoul(p, NULL, 10);
            if (id > 0) idset_add(unlocks, id);
        }
        p = elem_end;
    }
}

/* Parse UserUnlocks integer array [101, 102, ...] */
static void parse_uint_array(const char *arr_start, const char *arr_end, IdSet *ids) {
    const char *p = json_skip_ws(arr_start, arr_end);
    if (p >= arr_end || *p != '[') return;
    p++;

    while (p < arr_end) {
        p = json_skip_ws(p, arr_end);
        if (p >= arr_end || *p == ']') break;
        if (*p == ',') {
            p++;
            continue;
        }
        const char *elem_end = json_skip_value(p, arr_end);
        if (!elem_end) break;
        if (isdigit((unsigned char)*p)) {
            uint32_t id = (uint32_t)strtoul(p, NULL, 10);
            if (id > 0) idset_add(ids, id);
        } else if (*p == '"') {
            char buf[32];
            if (json_read_string(p, elem_end, buf, sizeof(buf)) && isdigit((unsigned char)buf[0])) {
                uint32_t id = (uint32_t)strtoul(buf, NULL, 10);
                if (id > 0) idset_add(ids, id);
            }
        }
        p = elem_end;
    }
}

/* Parse either r=achievementsets or r=patch JSON body */
static bool parse_game_metadata_json(const char *body, size_t len, uint32_t *out_game_id, IdSet *core_ids) {
    const char *end = body + len;
    int success = json_get_bool(body, end, "Success");
    if (success == 0) return false;

    uint32_t gid = 0;
    const char *sets_s = NULL, *sets_e = NULL;
    if (json_find_key(body, end, "Sets", &sets_s, &sets_e)) {
        json_get_uint(body, end, "GameId", &gid);
        if (gid == 0) json_get_uint(body, end, "GameID", &gid);

        const char *p = json_skip_ws(sets_s, sets_e);
        if (p < sets_e && *p == '[') {
            p++;
            while (p < sets_e) {
                p = json_skip_ws(p, sets_e);
                if (p >= sets_e || *p == ']') break;
                if (*p == ',') {
                    p++;
                    continue;
                }
                const char *elem_end = json_skip_value(p, sets_e);
                if (!elem_end) break;

                if (*p == '{') {
                    char set_type[32] = {0};
                    json_get_string(p, elem_end, "Type", set_type, sizeof(set_type));
                    if (set_type[0] == '\0' || strcasecmp(set_type, "core") == 0) {
                        const char *ach_s = NULL, *ach_e = NULL;
                        if (json_find_key(p, elem_end, "Achievements", &ach_s, &ach_e)) {
                            parse_achievements_array(ach_s, ach_e, core_ids);
                        }
                    }
                }
                p = elem_end;
            }
        }
        if (out_game_id && gid > 0) *out_game_id = gid;
        return true;
    }

    const char *pd_s = NULL, *pd_e = NULL;
    if (json_find_key(body, end, "PatchData", &pd_s, &pd_e)) {
        json_get_uint(pd_s, pd_e, "ID", &gid);
        const char *ach_s = NULL, *ach_e = NULL;
        if (json_find_key(pd_s, pd_e, "Achievements", &ach_s, &ach_e)) {
            parse_achievements_array(ach_s, ach_e, core_ids);
        }
        if (out_game_id && gid > 0) *out_game_id = gid;
        return true;
    }

    return false;
}

/* =========================================================================
 * NextUI settings reader (minuisettings.txt)
 * ========================================================================= */

typedef struct {
    bool enabled;
    bool authenticated;
    char username[128];
    char token[128];
} NextUIRAConfig;

static bool read_nextui_ra_config(const char *sdcard, NextUIRAConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    char path[1024];
    snprintf(path, sizeof(path), "%s/.userdata/shared/minuisettings.txt", sdcard);

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        strip_eol(line);
        if (strncmp(line, "raEnable=", 9) == 0) {
            const char *v = line + 9;
            cfg->enabled = (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0);
        } else if (strncmp(line, "raAuthenticated=", 16) == 0) {
            const char *v = line + 16;
            cfg->authenticated = (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0);
        } else if (strncmp(line, "raUsername=", 11) == 0) {
            snprintf(cfg->username, sizeof(cfg->username), "%.127s", line + 11);
        } else if (strncmp(line, "raToken=", 8) == 0) {
            snprintf(cfg->token, sizeof(cfg->token), "%.127s", line + 8);
        }
    }
    fclose(f);

    return cfg->enabled && cfg->authenticated && cfg->username[0] != '\0' && cfg->token[0] != '\0';
}

/* =========================================================================
 * NextUI offline cache & ledger reader (strictly READ-ONLY)
 * ========================================================================= */

/*
 * NextUI cache file format:
 *   [uint32_t little-endian body_len][body bytes][32-byte SHA-256]
 */
static bool read_nextui_cache_bin(const char *path, char **out_body, size_t *out_len) {
    *out_body = NULL;
    *out_len = 0;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (st.st_size < (off_t)(4 + RA_SHA256_LEN)) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t hdr[4];
    if (fread(hdr, 1, 4, f) != 4) {
        fclose(f);
        return false;
    }
    uint32_t len32 = (uint32_t)hdr[0] |
                     ((uint32_t)hdr[1] << 8) |
                     ((uint32_t)hdr[2] << 16) |
                     ((uint32_t)hdr[3] << 24);
    if (len32 == 0 || len32 > RA_MAX_CACHE_FILE_BYTES) {
        fclose(f);
        return false;
    }
    if ((off_t)(4 + (uint64_t)len32 + RA_SHA256_LEN) != st.st_size) {
        fclose(f);
        return false;
    }

    char *body = (char *)malloc((size_t)len32 + 1);
    if (!body) {
        fclose(f);
        return false;
    }
    if (fread(body, 1, len32, f) != len32) {
        free(body);
        fclose(f);
        return false;
    }
    body[len32] = '\0';
    fclose(f);

    *out_body = body;
    *out_len = (size_t)len32;
    return true;
}

typedef struct {
    bool offline_ready;
    bool has_metadata;
    bool has_session;
    uint32_t game_id;
    IdSet core_ids;
    IdSet session_unlocks;
    IdSet pending_unlocks;
    time_t newest_mtime;
} NextUIOfflineData;

static void nextui_offline_init(NextUIOfflineData *d) {
    memset(d, 0, sizeof(*d));
    idset_init(&d->core_ids);
    idset_init(&d->session_unlocks);
    idset_init(&d->pending_unlocks);
}

static void nextui_offline_free(NextUIOfflineData *d) {
    idset_free(&d->core_ids);
    idset_free(&d->session_unlocks);
    idset_free(&d->pending_unlocks);
}

static void read_nextui_ledger_pending(const char *ledger_path, const char *game_hash,
                                       uint32_t game_id, IdSet *pending_out) {
    FILE *f = fopen(ledger_path, "rb");
    if (!f) return;

    NextUILedgerRecord rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        rec.game_hash[32] = '\0';
        bool match = false;
        if (rec.game_hash[0] != '\0' && strcasecmp(rec.game_hash, game_hash) == 0) {
            match = true;
        } else if (game_id > 0 && rec.game_id == game_id) {
            match = true;
        }

        if (rec.type == RA_LEDGER_ACHIEVEMENT_UNLOCK && match && rec.achievement_id > 0) {
            idset_add(pending_out, rec.achievement_id);
        } else if (rec.type == RA_LEDGER_SYNC_ACK && rec.achievement_id > 0) {
            if (game_id == 0 || rec.game_id == 0 || rec.game_id == game_id) {
                idset_remove(pending_out, rec.achievement_id);
            }
        }
    }
    fclose(f);
}

static void read_nextui_offline_data(const char *sdcard, const char *hash, NextUIOfflineData *out) {
    char cache_dir[1024];
    char ledger_path[1024];
    char p_login[1100], p_gameid[1100], p_sets[1100], p_patch[1100], p_session[1100];

    snprintf(cache_dir, sizeof(cache_dir), "%s/.userdata/shared/.ra/offline/cache", sdcard);
    snprintf(ledger_path, sizeof(ledger_path), "%s/.userdata/shared/.ra/offline/ledger.bin", sdcard);
    snprintf(p_login, sizeof(p_login), "%s/login.bin", cache_dir);
    snprintf(p_gameid, sizeof(p_gameid), "%s/gameid_%s.bin", cache_dir, hash);
    snprintf(p_sets, sizeof(p_sets), "%s/achievementsets_%s.bin", cache_dir, hash);
    snprintf(p_patch, sizeof(p_patch), "%s/patch_%s.bin", cache_dir, hash);
    snprintf(p_session, sizeof(p_session), "%s/startsession_%s.bin", cache_dir, hash);

    bool has_login = file_exists_nonempty(p_login);

    /* gameid_<hash>.bin */
    char *body = NULL;
    size_t blen = 0;
    if (read_nextui_cache_bin(p_gameid, &body, &blen)) {
        uint32_t gid = 0;
        if (json_get_uint(body, body + blen, "GameID", &gid) && gid > 0) {
            out->game_id = gid;
        }
        free(body);
    }

    /* achievementsets_<hash>.bin or patch_<hash>.bin */
    bool has_sets_file = false;
    if (read_nextui_cache_bin(p_sets, &body, &blen)) {
        has_sets_file = true;
        if (parse_game_metadata_json(body, blen, &out->game_id, &out->core_ids)) {
            out->has_metadata = true;
        }
        free(body);
    } else if (read_nextui_cache_bin(p_patch, &body, &blen)) {
        has_sets_file = true;
        if (parse_game_metadata_json(body, blen, &out->game_id, &out->core_ids)) {
            out->has_metadata = true;
        }
        free(body);
    }

    /* startsession_<hash>.bin */
    if (read_nextui_cache_bin(p_session, &body, &blen)) {
        out->has_session = true;
        const char *end = body + blen;
        const char *us = NULL, *ue = NULL;
        if (json_find_key(body, end, "Unlocks", &us, &ue)) {
            parse_unlock_objects_array(us, ue, &out->session_unlocks);
        }
        if (json_find_key(body, end, "HardcoreUnlocks", &us, &ue)) {
            parse_unlock_objects_array(us, ue, &out->session_unlocks);
        }
        time_t mt = file_mtime(p_session);
        if (mt > out->newest_mtime) out->newest_mtime = mt;
        free(body);
    }

    /* ledger.bin pending unlocks */
    time_t lmt = file_mtime(ledger_path);
    if (lmt > out->newest_mtime) out->newest_mtime = lmt;
    read_nextui_ledger_pending(ledger_path, hash, out->game_id, &out->pending_unlocks);

    out->offline_ready = has_login && has_sets_file && out->has_session;
}

/* =========================================================================
 * rcheevos CD reader wrapper (CHD first, default CUE/BIN/ISO fallback)
 * ========================================================================= */

#define RA_CDHANDLE_MAGIC 0x43484448U

typedef struct {
    uint32_t magic;
    bool is_chd;
    void *inner;
} WrappedCDHandle;

static rc_hash_cdreader_t g_default_cdreader;

static void *wrap_cd_handle(void *inner, bool is_chd) {
    if (!inner) return NULL;
    WrappedCDHandle *h = (WrappedCDHandle *)malloc(sizeof(WrappedCDHandle));
    if (!h) {
        if (is_chd) chd_close_track(inner);
        else if (g_default_cdreader.close_track) g_default_cdreader.close_track(inner);
        return NULL;
    }
    h->magic = RA_CDHANDLE_MAGIC;
    h->is_chd = is_chd;
    h->inner = inner;
    return h;
}

static void *cdreader_open_track(const char *path, uint32_t track) {
    void *h = chd_open_track(path, track);
    if (h) return wrap_cd_handle(h, true);
    if (g_default_cdreader.open_track) {
        h = g_default_cdreader.open_track(path, track);
        if (h) return wrap_cd_handle(h, false);
    }
    return NULL;
}

static void *cdreader_open_track_iterator(const char *path, uint32_t track,
                                          const struct rc_hash_iterator *iterator) {
    void *h = chd_open_track_iterator(path, track, iterator);
    if (h) return wrap_cd_handle(h, true);
    if (g_default_cdreader.open_track_iterator) {
        h = g_default_cdreader.open_track_iterator(path, track, iterator);
        if (h) return wrap_cd_handle(h, false);
    } else if (g_default_cdreader.open_track) {
        h = g_default_cdreader.open_track(path, track);
        if (h) return wrap_cd_handle(h, false);
    }
    return NULL;
}

static size_t cdreader_read_sector(void *track_handle, uint32_t sector, void *buffer, size_t requested_bytes) {
    WrappedCDHandle *h = (WrappedCDHandle *)track_handle;
    if (!h || h->magic != RA_CDHANDLE_MAGIC) return 0;
    if (h->is_chd) return chd_read_sector(h->inner, sector, buffer, requested_bytes);
    if (g_default_cdreader.read_sector) return g_default_cdreader.read_sector(h->inner, sector, buffer, requested_bytes);
    return 0;
}

static void cdreader_close_track(void *track_handle) {
    WrappedCDHandle *h = (WrappedCDHandle *)track_handle;
    if (!h || h->magic != RA_CDHANDLE_MAGIC) return;
    if (h->is_chd) chd_close_track(h->inner);
    else if (g_default_cdreader.close_track) g_default_cdreader.close_track(h->inner);
    free(h);
}

static uint32_t cdreader_first_track_sector(void *track_handle) {
    WrappedCDHandle *h = (WrappedCDHandle *)track_handle;
    if (!h || h->magic != RA_CDHANDLE_MAGIC) return 0;
    if (h->is_chd) return chd_first_track_sector(h->inner);
    if (g_default_cdreader.first_track_sector) return g_default_cdreader.first_track_sector(h->inner);
    return 0;
}

static void init_cdreader(void) {
    rc_hash_get_default_cdreader(&g_default_cdreader);
    static rc_hash_cdreader_t custom_reader;
    memset(&custom_reader, 0, sizeof(custom_reader));
    custom_reader.open_track = cdreader_open_track;
    custom_reader.open_track_iterator = cdreader_open_track_iterator;
    custom_reader.read_sector = cdreader_read_sector;
    custom_reader.close_track = cdreader_close_track;
    custom_reader.first_track_sector = cdreader_first_track_sector;
    rc_hash_init_custom_cdreader(&custom_reader);
}

/* =========================================================================
 * ROM hashing (with .zip extraction & hash cache)
 * ========================================================================= */

static bool is_ignored_zip_entry(const char *name) {
    if (!name || !*name) return true;
    size_t n = strlen(name);
    if (name[n - 1] == '/') return true;
    if (strstr(name, "__MACOSX/") != NULL) return true;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (base[0] == '.') return true;
    return has_ext_ci(name, ".txt") || has_ext_ci(name, ".nfo") ||
           has_ext_ci(name, ".diz") || has_ext_ci(name, ".xml") ||
           has_ext_ci(name, ".json") || has_ext_ci(name, ".png") ||
           has_ext_ci(name, ".jpg") || has_ext_ci(name, ".jpeg") ||
           has_ext_ci(name, ".sav") || has_ext_ci(name, ".srm") ||
           has_ext_ci(name, ".md");
}

static bool hash_zip_rom(const char *zip_path, int console_id, char hash_out[33]) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        return false;
    }

    mz_uint num_files = mz_zip_reader_get_num_files(&zip);
    int best_idx = -1;
    mz_uint64 best_size = 0;
    char best_name[512] = {0};

    for (mz_uint i = 0; i < num_files; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (is_ignored_zip_entry(st.m_filename)) continue;
        if (st.m_uncomp_size == 0 || st.m_uncomp_size > RA_MAX_ZIP_ROM_BYTES) continue;
        if (st.m_uncomp_size > best_size) {
            best_size = st.m_uncomp_size;
            best_idx = (int)i;
            snprintf(best_name, sizeof(best_name), "%s", st.m_filename);
        }
    }

    if (best_idx < 0) {
        mz_zip_reader_end(&zip);
        return false;
    }

    size_t extracted_size = 0;
    void *buf = mz_zip_reader_extract_to_heap(&zip, (mz_uint)best_idx, &extracted_size, 0);
    mz_zip_reader_end(&zip);
    if (!buf || extracted_size == 0) {
        free(buf);
        return false;
    }

    bool ok = false;
    if (console_id != RC_CONSOLE_UNKNOWN) {
        ok = rc_hash_generate_from_buffer(hash_out, (uint32_t)console_id, (const uint8_t *)buf, extracted_size) != 0;
    } else {
        struct rc_hash_iterator iter;
        rc_hash_initialize_iterator(&iter, best_name, (const uint8_t *)buf, extracted_size);
        ok = rc_hash_iterate(hash_out, &iter) != 0;
        rc_hash_destroy_iterator(&iter);
    }

    free(buf);
    return ok;
}

static bool compute_rom_hash(const char *rom_path, const char *emu_tag, char hash_out[33]) {
    int console_id = ra_get_console_id(emu_tag, rom_path);

    if (has_ext_ci(rom_path, ".zip") &&
        console_id != RC_CONSOLE_ARCADE &&
        console_id != RC_CONSOLE_MS_DOS &&
        console_id != RC_CONSOLE_ARDUBOY) {
        if (hash_zip_rom(rom_path, console_id, hash_out)) {
            return true;
        }
    }

    if (console_id != RC_CONSOLE_UNKNOWN) {
        return rc_hash_generate_from_file(hash_out, (uint32_t)console_id, rom_path) != 0;
    }

    struct rc_hash_iterator iter;
    rc_hash_initialize_iterator(&iter, rom_path, NULL, 0);
    int ok = rc_hash_iterate(hash_out, &iter);
    rc_hash_destroy_iterator(&iter);
    return ok != 0;
}

static bool lookup_cached_hash(const char *cache_dir, const char *rom_path, const char *emu_tag,
                               off_t size, time_t mtime, char hash_out[33]) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/hashes.tsv", cache_dir);
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[2048];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        strip_eol(line);
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1++ = '\0';
        char *t2 = strchr(t1, '\t');
        if (!t2) continue;
        *t2++ = '\0';
        char *t3 = strchr(t2, '\t');
        if (!t3) continue;
        *t3++ = '\0';
        char *t4 = strchr(t3, '\t');
        if (!t4) continue;
        *t4++ = '\0';

        if (strcmp(line, rom_path) == 0 &&
            strcmp(t3, emu_tag) == 0 &&
            (off_t)strtoll(t1, NULL, 10) == size &&
            (time_t)strtoll(t2, NULL, 10) == mtime &&
            strlen(t4) == 32) {
            memcpy(hash_out, t4, 33);
            found = true;
            /* keep scanning so latest entry wins */
        }
    }
    fclose(f);
    return found;
}

static void save_cached_hash(const char *cache_dir, const char *rom_path, const char *emu_tag,
                             off_t size, time_t mtime, const char *hash) {
    if (mkdirs(cache_dir) != 0) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/hashes.tsv", cache_dir);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\t%lld\t%lld\t%s\t%s\n",
            rom_path, (long long)size, (long long)mtime, emu_tag, hash);
    fclose(f);
}

/* =========================================================================
 * Pak-local game cache (<cache_dir>/games/<hash>.kv)
 * ========================================================================= */

typedef struct {
    bool present;
    uint32_t game_id;
    uint32_t total;
    uint32_t earned;
    time_t meta_ts;
    time_t unlocks_ts;
    IdSet core_ids;
    IdSet unlock_ids;
} PakGameCache;

static void pak_cache_init(PakGameCache *c) {
    memset(c, 0, sizeof(*c));
    idset_init(&c->core_ids);
    idset_init(&c->unlock_ids);
}

static void pak_cache_free(PakGameCache *c) {
    idset_free(&c->core_ids);
    idset_free(&c->unlock_ids);
}

static bool read_pak_game_cache(const char *cache_dir, const char *hash, PakGameCache *out) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/games/%s.kv", cache_dir, hash);
    FILE *f = fopen(path, "r");
    if (!f) return false;

    size_t cap = 4096;
    char *line = (char *)malloc(cap);
    if (!line) {
        fclose(f);
        return false;
    }

    while (fgets(line, (int)cap, f)) {
        /* Handle arbitrarily long CSV lines */
        size_t len = strlen(line);
        while (len > 0 && line[len - 1] != '\n' && !feof(f)) {
            cap *= 2;
            char *nl = (char *)realloc(line, cap);
            if (!nl) {
                free(line);
                fclose(f);
                return false;
            }
            line = nl;
            if (!fgets(line + len, (int)(cap - len), f)) break;
            len = strlen(line);
        }
        strip_eol(line);
        if (strncmp(line, "game_id=", 8) == 0) {
            out->game_id = (uint32_t)strtoul(line + 8, NULL, 10);
        } else if (strncmp(line, "total=", 6) == 0) {
            out->total = (uint32_t)strtoul(line + 6, NULL, 10);
        } else if (strncmp(line, "earned=", 7) == 0) {
            out->earned = (uint32_t)strtoul(line + 7, NULL, 10);
        } else if (strncmp(line, "meta_ts=", 8) == 0) {
            out->meta_ts = (time_t)strtoll(line + 8, NULL, 10);
        } else if (strncmp(line, "unlocks_ts=", 11) == 0) {
            out->unlocks_ts = (time_t)strtoll(line + 11, NULL, 10);
        } else if (strncmp(line, "core_ids=", 9) == 0) {
            idset_parse_csv(&out->core_ids, line + 9);
        } else if (strncmp(line, "unlock_ids=", 11) == 0) {
            idset_parse_csv(&out->unlock_ids, line + 11);
        }
    }

    free(line);
    fclose(f);
    out->present = true;
    return true;
}

static void write_pak_game_cache(const char *cache_dir, const char *hash, const PakGameCache *c) {
    char games_dir[1024], tmp_path[1100], final_path[1100];
    snprintf(games_dir, sizeof(games_dir), "%s/games", cache_dir);
    if (mkdirs(games_dir) != 0) return;

    snprintf(tmp_path, sizeof(tmp_path), "%s/%s.kv.tmp.%d", games_dir, hash, (int)getpid());
    snprintf(final_path, sizeof(final_path), "%s/%s.kv", games_dir, hash);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;

    fprintf(f, "game_id=%u\n", c->game_id);
    fprintf(f, "total=%u\n", c->total);
    fprintf(f, "earned=%u\n", c->earned);
    fprintf(f, "meta_ts=%lld\n", (long long)c->meta_ts);
    fprintf(f, "unlocks_ts=%lld\n", (long long)c->unlocks_ts);
    fputs("core_ids=", f);
    idset_write_csv(&c->core_ids, f);
    fputc('\n', f);
    fputs("unlock_ids=", f);
    idset_write_csv(&c->unlock_ids, f);
    fputc('\n', f);

    if (fclose(f) == 0) {
        if (rename(tmp_path, final_path) != 0) {
            unlink(tmp_path);
        }
    } else {
        unlink(tmp_path);
    }
}

/* =========================================================================
 * Safe HTTP POST via curl (fork + execvp + stdin body, no shell)
 * ========================================================================= */

typedef enum {
    HTTP_OK = 0,
    HTTP_NET_ERROR,
    HTTP_AUTH_ERROR,
    HTTP_SERVER_ERROR
} HttpResult;

static HttpResult http_post_form(const char *url, const char *post_body,
                                 char **out_body, size_t *out_len) {
    *out_body = NULL;
    *out_len = 0;

    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0) return HTTP_NET_ERROR;
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return HTTP_NET_ERROR;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return HTTP_NET_ERROR;
    }

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        char *const argv[] = {
            "curl",
            "-s", "-k", "-L",
            "--connect-timeout", "3",
            "-m", "6",
            "-A", RA_USER_AGENT,
            "-H", "Content-Type: application/x-www-form-urlencoded",
            "--data-binary", "@-",
            "-w", "\n%{http_code}",
            (char *)url,
            NULL
        };
        execvp("curl", argv);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    /* Write POST body to curl stdin */
    size_t body_len = strlen(post_body);
    size_t written = 0;
    while (written < body_len) {
        ssize_t w = write(in_pipe[1], post_body + written, body_len - written);
        if (w <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        written += (size_t)w;
    }
    close(in_pipe[1]);

    /* Read curl stdout */
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        close(out_pipe[0]);
        waitpid(pid, NULL, 0);
        return HTTP_NET_ERROR;
    }

    for (;;) {
        if (len + 4096 >= cap) {
            if (cap >= RA_MAX_CACHE_FILE_BYTES) break;
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                close(out_pipe[0]);
                waitpid(pid, NULL, 0);
                return HTTP_NET_ERROR;
            }
            buf = nb;
        }
        ssize_t r = read(out_pipe[0], buf + len, cap - len - 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        len += (size_t)r;
    }
    close(out_pipe[0]);
    buf[len] = '\0';

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        free(buf);
        return HTTP_NET_ERROR;
    }

    /* Extract trailing \n<http_code> */
    char *last_nl = strrchr(buf, '\n');
    if (!last_nl) {
        free(buf);
        return HTTP_NET_ERROR;
    }
    int http_code = atoi(last_nl + 1);
    *last_nl = '\0';
    len = (size_t)(last_nl - buf);

    if (http_code == 401 || http_code == 403) {
        free(buf);
        return HTTP_AUTH_ERROR;
    }
    if (http_code == 0) {
        free(buf);
        return HTTP_NET_ERROR;
    }
    if (http_code != 200) {
        free(buf);
        return HTTP_SERVER_ERROR;
    }

    *out_body = buf;
    *out_len = len;
    return HTTP_OK;
}

static bool response_is_auth_failure(const char *body, size_t len) {
    const char *end = body + len;
    if (json_get_bool(body, end, "Success") != 0) return false;

    char code[64] = {0};
    char err[192] = {0};
    json_get_string(body, end, "Code", code, sizeof(code));
    json_get_string(body, end, "Error", err, sizeof(err));

    if (strcasecmp(code, "invalid_credentials") == 0 ||
        strcasecmp(code, "expired_token") == 0 ||
        strcasecmp(code, "access_denied") == 0) {
        return true;
    }
    if (strstr(err, "Invalid User") != NULL ||
        strstr(err, "Invalid user") != NULL ||
        strstr(err, "credentials") != NULL ||
        strstr(err, "Unauthorized") != NULL ||
        strstr(err, "Token") != NULL) {
        return true;
    }
    return false;
}

/* =========================================================================
 * Subcommands
 * ========================================================================= */

static int cmd_check_config(const char *sdcard) {
    NextUIRAConfig cfg;
    if (read_nextui_ra_config(sdcard, &cfg)) {
        printf("enabled=1\n");
        return 0;
    }
    printf("enabled=0\n");
    return 1;
}

static void print_result(const char *status, const char *hash, uint32_t game_id,
                         uint32_t earned, uint32_t total, uint32_t pending,
                         bool offline_ready, const char *source) {
    printf("status=%s\n", status);
    printf("hash=%s\n", hash ? hash : "");
    printf("game_id=%u\n", game_id);
    printf("earned=%u\n", earned);
    printf("total=%u\n", total);
    printf("pending=%u\n", pending);
    printf("offline_ready=%d\n", offline_ready ? 1 : 0);
    printf("source=%s\n", source ? source : "none");
}

static int cmd_lookup(const char *sdcard, const char *cache_dir,
                      const char *rom_path, const char *emu_tag, bool offline_only) {
    NextUIRAConfig cfg;
    if (!read_nextui_ra_config(sdcard, &cfg)) {
        print_result("disabled", "", 0, 0, 0, 0, false, "none");
        return 0;
    }

    if (ra_is_explicitly_unsupported_emu(emu_tag)) {
        print_result("unsupported", "", 0, 0, 0, 0, false, "none");
        return 0;
    }

    struct stat rom_st;
    if (stat(rom_path, &rom_st) != 0 || !S_ISREG(rom_st.st_mode)) {
        print_result("unsupported", "", 0, 0, 0, 0, false, "none");
        return 0;
    }

    char hash[33] = {0};
    if (!lookup_cached_hash(cache_dir, rom_path, emu_tag, rom_st.st_size, rom_st.st_mtime, hash)) {
        if (!compute_rom_hash(rom_path, emu_tag, hash)) {
            print_result("unsupported", "", 0, 0, 0, 0, false, "none");
            return 0;
        }
        save_cached_hash(cache_dir, rom_path, emu_tag, rom_st.st_size, rom_st.st_mtime, hash);
    }

    NextUIOfflineData nextui;
    nextui_offline_init(&nextui);
    read_nextui_offline_data(sdcard, hash, &nextui);

    PakGameCache pak;
    pak_cache_init(&pak);
    read_pak_game_cache(cache_dir, hash, &pak);

    time_t now = time(NULL);
    const char *api_url = getenv("RA_API_URL");
    if (!api_url || !*api_url) api_url = RA_DEFAULT_API_URL;

    bool have_meta = false;
    uint32_t game_id = 0;
    IdSet core_ids;
    idset_init(&core_ids);

    IdSet base_unlocks;
    idset_init(&base_unlocks);
    bool have_unlocks = false;
    const char *source = "none";

    /* Seed from NextUI offline cache first if available */
    if (nextui.has_metadata) {
        have_meta = true;
        game_id = nextui.game_id;
        idset_union(&core_ids, &nextui.core_ids);
        source = "nextui_cache";
    }
    if (nextui.has_session) {
        have_unlocks = true;
        idset_union(&base_unlocks, &nextui.session_unlocks);
    }

    /* Seed from pak cache if we didn't get metadata/unlocks from NextUI */
    if (pak.present) {
        if (!have_meta && pak.meta_ts > 0) {
            have_meta = true;
            game_id = pak.game_id;
            idset_union(&core_ids, &pak.core_ids);
            source = "pak_cache";
        }
        if (!have_unlocks && pak.unlocks_ts > 0) {
            have_unlocks = true;
            idset_union(&base_unlocks, &pak.unlock_ids);
            if (strcmp(source, "none") == 0) source = "pak_cache";
        }
    }

    /* Decide whether we should refresh metadata or unlocks online */
    bool meta_fresh = false;
    if (pak.present && pak.meta_ts > 0) {
        time_t ttl = (pak.game_id > 0 && pak.total > 0) ? RA_META_TTL_FOUND_SECS : RA_META_TTL_MISSING_SECS;
        if (now >= pak.meta_ts && (now - pak.meta_ts) < ttl) {
            meta_fresh = true;
        }
    } else if (nextui.has_metadata && nextui.game_id > 0) {
        meta_fresh = true;
    }

    bool unlocks_fresh = false;
    if (pak.present && pak.unlocks_ts > 0 && now >= pak.unlocks_ts &&
        (now - pak.unlocks_ts) < RA_UNLOCKS_TTL_SECS &&
        nextui.newest_mtime <= pak.unlocks_ts) {
        unlocks_fresh = true;
    }

    bool auth_invalid = false;
    bool network_failed = false;

    if (!offline_only && (!meta_fresh || (game_id > 0 && core_ids.count > 0 && !unlocks_fresh))) {
        char enc_user[384], enc_token[384];
        bool enc_ok = url_encode(cfg.username, enc_user, sizeof(enc_user)) &&
                      url_encode(cfg.token, enc_token, sizeof(enc_token));

        /* Step 1: resolve hash -> game_id if needed */
        if (enc_ok && !meta_fresh) {
            char post[256];
            snprintf(post, sizeof(post), "r=gameid&m=%s", hash);
            char *resp = NULL;
            size_t rlen = 0;
            HttpResult hr = http_post_form(api_url, post, &resp, &rlen);
            if (hr == HTTP_OK) {
                uint32_t gid = 0;
                int succ = json_get_bool(resp, resp + rlen, "Success");
                if (succ == 1 && json_get_uint(resp, resp + rlen, "GameID", &gid)) {
                    game_id = gid;
                    if (gid == 0) {
                        /* Hash not recognized on RA -> cache unsupported */
                        idset_free(&core_ids);
                        idset_init(&core_ids);
                        have_meta = true;
                        meta_fresh = true;
                        source = "online";

                        pak.present = true;
                        pak.game_id = 0;
                        pak.total = 0;
                        pak.earned = 0;
                        pak.meta_ts = now;
                        pak.unlocks_ts = 0;
                        idset_free(&pak.core_ids); idset_init(&pak.core_ids);
                        idset_free(&pak.unlock_ids); idset_init(&pak.unlock_ids);
                        write_pak_game_cache(cache_dir, hash, &pak);
                    }
                } else {
                    network_failed = true;
                }
                free(resp);
            } else if (hr == HTTP_AUTH_ERROR) {
                auth_invalid = true;
            } else {
                network_failed = true;
            }
        }

        /* Step 2: fetch game achievement set (r=patch) if game_id > 0 and metadata not fresh */
        if (enc_ok && !auth_invalid && !network_failed && !meta_fresh && game_id > 0) {
            char post[1024];
            snprintf(post, sizeof(post), "r=patch&u=%s&t=%s&g=%u", enc_user, enc_token, game_id);
            char *resp = NULL;
            size_t rlen = 0;
            HttpResult hr = http_post_form(api_url, post, &resp, &rlen);
            if (hr == HTTP_OK) {
                if (response_is_auth_failure(resp, rlen)) {
                    auth_invalid = true;
                } else {
                    IdSet fetched_core;
                    idset_init(&fetched_core);
                    uint32_t parsed_gid = game_id;
                    if (parse_game_metadata_json(resp, rlen, &parsed_gid, &fetched_core)) {
                        game_id = parsed_gid;
                        idset_free(&core_ids);
                        core_ids = fetched_core;
                        have_meta = true;
                        meta_fresh = true;
                        source = "online";

                        pak.present = true;
                        pak.game_id = game_id;
                        pak.total = (uint32_t)core_ids.count;
                        pak.meta_ts = now;
                        idset_free(&pak.core_ids); idset_init(&pak.core_ids);
                        idset_union(&pak.core_ids, &core_ids);
                        if (core_ids.count == 0) {
                            pak.earned = 0;
                            idset_free(&pak.unlock_ids); idset_init(&pak.unlock_ids);
                        }
                        write_pak_game_cache(cache_dir, hash, &pak);
                    } else {
                        idset_free(&fetched_core);
                        network_failed = true;
                    }
                }
                free(resp);
            } else if (hr == HTTP_AUTH_ERROR) {
                auth_invalid = true;
            } else {
                network_failed = true;
            }
        }

        /* Step 3: fetch softcore unlocks (r=unlocks&h=0) if game has official achievements */
        if (enc_ok && !auth_invalid && !network_failed && game_id > 0 && core_ids.count > 0 && !unlocks_fresh) {
            char post[1024];
            snprintf(post, sizeof(post), "r=unlocks&u=%s&t=%s&g=%u&h=0", enc_user, enc_token, game_id);
            char *resp = NULL;
            size_t rlen = 0;
            HttpResult hr = http_post_form(api_url, post, &resp, &rlen);
            if (hr == HTTP_OK) {
                if (response_is_auth_failure(resp, rlen)) {
                    auth_invalid = true;
                } else if (json_get_bool(resp, resp + rlen, "Success") == 1) {
                    const char *us = NULL, *ue = NULL;
                    if (json_find_key(resp, resp + rlen, "UserUnlocks", &us, &ue)) {
                        IdSet server_unlocks;
                        idset_init(&server_unlocks);
                        parse_uint_array(us, ue, &server_unlocks);

                        idset_free(&base_unlocks);
                        base_unlocks = server_unlocks;
                        have_unlocks = true;
                        source = "online";

                        pak.present = true;
                        pak.game_id = game_id;
                        pak.total = (uint32_t)core_ids.count;
                        if (pak.meta_ts == 0) pak.meta_ts = now;
                        pak.unlocks_ts = now;
                        idset_free(&pak.core_ids); idset_init(&pak.core_ids);
                        idset_union(&pak.core_ids, &core_ids);
                        idset_free(&pak.unlock_ids); idset_init(&pak.unlock_ids);
                        idset_union(&pak.unlock_ids, &base_unlocks);
                        pak.earned = (uint32_t)idset_count_intersection(&base_unlocks, &core_ids);
                        write_pak_game_cache(cache_dir, hash, &pak);
                    }
                }
                free(resp);
            } else if (hr == HTTP_AUTH_ERROR) {
                auth_invalid = true;
            }
        }
    }

    if (auth_invalid) {
        print_result("auth_invalid", hash, 0, 0, 0, 0, nextui.offline_ready, "online");
    } else if (!have_meta) {
        print_result("wifi_required", hash, 0, 0, 0, 0, nextui.offline_ready, "none");
    } else if (game_id == 0) {
        print_result("unsupported", hash, 0, 0, 0, 0, false, source);
    } else if (core_ids.count == 0) {
        print_result("none", hash, game_id, 0, 0, 0, nextui.offline_ready, source);
    } else if (!have_unlocks && !nextui.has_session) {
        print_result("wifi_required", hash, game_id, 0, (uint32_t)core_ids.count, 0, nextui.offline_ready, source);
    } else {
        /* Merge unlocks: authoritative server unlocks (if online) or local cache + NextUI session,
         * plus any pending offline unlocks from NextUI's ledger.bin */
        IdSet merged;
        idset_init(&merged);
        if (have_unlocks) {
            idset_union(&merged, &base_unlocks);
        }
        if (nextui.has_session && strcmp(source, "online") != 0) {
            idset_union(&merged, &nextui.session_unlocks);
        }

        /* Count pending unlocks belonging to this game's core set */
        uint32_t pending_core = 0;
        for (size_t i = 0; i < nextui.pending_unlocks.count; i++) {
            uint32_t aid = nextui.pending_unlocks.items[i];
            if (idset_contains(&core_ids, aid)) {
                pending_core++;
                idset_add(&merged, aid);
            }
        }

        uint32_t earned = (uint32_t)idset_count_intersection(&merged, &core_ids);
        uint32_t total = (uint32_t)core_ids.count;

        print_result("ok", hash, game_id, earned, total, pending_core, nextui.offline_ready, source);
        idset_free(&merged);
    }

    idset_free(&base_unlocks);
    idset_free(&core_ids);
    pak_cache_free(&pak);
    nextui_offline_free(&nextui);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s check-config --sdcard <path>\n"
            "  %s lookup --sdcard <path> --cache-dir <path> --rom <file> --emu <tag> [--offline-only]\n",
            prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    init_cdreader();

    if (strcmp(argv[1], "check-config") == 0) {
        const char *sdcard = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--sdcard") == 0 && i + 1 < argc) {
                sdcard = argv[++i];
            }
        }
        if (!sdcard) {
            usage(argv[0]);
            return 2;
        }
        return cmd_check_config(sdcard);
    }

    if (strcmp(argv[1], "lookup") == 0) {
        const char *sdcard = NULL;
        const char *cache_dir = NULL;
        const char *rom_path = NULL;
        const char *emu_tag = "";
        bool offline_only = false;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--sdcard") == 0 && i + 1 < argc) {
                sdcard = argv[++i];
            } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
                cache_dir = argv[++i];
            } else if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
                rom_path = argv[++i];
            } else if (strcmp(argv[i], "--emu") == 0 && i + 1 < argc) {
                emu_tag = argv[++i];
            } else if (strcmp(argv[i], "--offline-only") == 0) {
                offline_only = true;
            }
        }

        if (!sdcard || !cache_dir || !rom_path) {
            usage(argv[0]);
            return 2;
        }
        return cmd_lookup(sdcard, cache_dir, rom_path, emu_tag, offline_only);
    }

    usage(argv[0]);
    return 2;
}
