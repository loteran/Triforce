/* (Triforce, OoT debug tools) The Debug tab's Game flags and Cutscenes pages (gpu/vk/vk_menu.cpp, gpu/vk/debug_names.h) and
   the game's side (host/hos_main.c): the lines that go between them, a byte a call (HOS_OPT_DEBUG_ASK one way,
   HOS_OPT_DEBUG_TEXT and HOS_OPT_DEBUG_SAID the other: gpu/vk/vk_api.h). C and C++; tests/test_debug_link.py runs it.

   The game knows a flag (a Bool of its GameData) by the MurmurHash3 of its name alone, and an event by the name of its
   file. Neither list is in this repository: the pages read the names from the player's own game files, in memory.

   The pages ask:
     W <hash> <hash> ...   the flags the page shows now, at most OOT_WATCH_MAX, eight hex digits each: their states are
                           sent twice a second from then on (a W with none: no more)
     F <hash> <0|1>        the flag cleared or set, through the game's own setter
     E <name>              the event of that name started, as one of the game's actors starts it
   The game's side says:
     OOT_SAID_FLAGS   "<sum> <states>": the watch list's oot_watch_sum, then a character a flag in the list's order:
                      1 set, 0 not, - the save data has no such flag, ? no save is loaded
     OOT_SAID_STATUS  what became of an F or an E, in words, for the page's foot */
#ifndef OOT_DEBUG_LINK_H
#define OOT_DEBUG_LINK_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OOT_WATCH_MAX = 64, OOT_NAME_MAX = 95, OOT_LINE_MAX = 4 + 9 * OOT_WATCH_MAX };
enum { OOT_SAID_FLAGS = 1, OOT_SAID_STATUS = 2 };

/* MurmurHash3 (32-bit, seed 0) of a name's bytes, as the game's GameData getters and setters take it */
static inline uint32_t oot_name_hash(const char *text, size_t n) {
    uint32_t h = 0;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t k = (uint32_t)(uint8_t)text[i] | (uint32_t)(uint8_t)text[i + 1] << 8 | (uint32_t)(uint8_t)text[i + 2] << 16 |
                     (uint32_t)(uint8_t)text[i + 3] << 24;
        k *= 0xcc9e2d51u, k = k << 15 | k >> 17, k *= 0x1b873593u;
        h ^= k, h = h << 13 | h >> 19, h = h * 5 + 0xe6546b64u;
    }
    uint32_t k = 0;
    if ((n & 3) == 3) k ^= (uint32_t)(uint8_t)text[i + 2] << 16;
    if ((n & 3) >= 2) k ^= (uint32_t)(uint8_t)text[i + 1] << 8;
    if (n & 3) k ^= (uint8_t)text[i], k *= 0xcc9e2d51u, k = k << 15 | k >> 17, k *= 0x1b873593u, h ^= k;
    h ^= (uint32_t)n;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

/* a character of a flag's or an event's name: the game's names are letters, digits, _ . and - */
static inline int oot_name_char(int ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
}
/* a name of 1 to OOT_NAME_MAX such characters */
static inline int oot_name_ok(const char *name) {
    size_t n = 0;
    while (name[n] && n <= OOT_NAME_MAX && oot_name_char((unsigned char)name[n])) n++;
    return n > 0 && n <= OOT_NAME_MAX && !name[n];
}

/* what pairs a line of states with the list it answers: the flags' hashes in their order */
static inline uint32_t oot_watch_sum(const uint32_t *hashes, int n) {
    uint32_t sum = 0x9e3779b9u + (uint32_t)n;
    for (int i = 0; i < n; i++) sum = (sum << 5 | sum >> 27) ^ hashes[i];
    return sum;
}

/* eight hex digits at *at, which moves past them: 1 when there were eight */
static inline int oot_hex8(const char **at, uint32_t *value) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        const int ch = (unsigned char)(*at)[i];
        const int digit = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
        if (digit < 0) return 0;
        v = v << 4 | (uint32_t)digit;
    }
    *at += 8, *value = v;
    return 1;
}

typedef struct OotAsk {
    int kind;                         /* 'W', 'F' or 'E' */
    uint32_t hash;                    /* F: the flag */
    int value;                        /* F: 0 or 1 */
    int count;                        /* W: how many */
    uint32_t watch[OOT_WATCH_MAX];    /* W: the flags */
    char name[OOT_NAME_MAX + 1];      /* E: the event */
} OotAsk;

/* A line of the pages taken apart: 1 when it is one of the three, whole and well formed (anything else is dropped) */
static inline int oot_ask_parse(const char *line, OotAsk *ask) {
    memset(ask, 0, sizeof *ask);
    const char *at = line + 1;
    if (line[0] == 'W') {
        while (*at) {
            if (*at++ != ' ' || ask->count == OOT_WATCH_MAX || !oot_hex8(&at, &ask->watch[ask->count])) return 0;
            ask->count++;
        }
        ask->kind = 'W';
        return 1;
    }
    if (line[0] == 'F') {
        if (*at++ != ' ' || !oot_hex8(&at, &ask->hash) || !ask->hash || *at++ != ' ' || (*at != '0' && *at != '1') || at[1]) return 0;
        ask->value = *at == '1';
        ask->kind = 'F';
        return 1;
    }
    if (line[0] == 'E') {
        if (*at++ != ' ' || !oot_name_ok(at)) return 0;
        memcpy(ask->name, at, strlen(at) + 1);
        ask->kind = 'E';
        return 1;
    }
    return 0;
}

/* The pages' lines written (out of OOT_LINE_MAX at least): their length, 0 when the line cannot be made */
static inline int oot_ask_watch(char *out, size_t cap, const uint32_t *hashes, int n) {
    if (n < 0 || n > OOT_WATCH_MAX || cap < 2 + 9 * (size_t)n) return 0;
    int used = snprintf(out, cap, "W");
    for (int i = 0; i < n; i++) used += snprintf(out + used, cap - (size_t)used, " %08x", (unsigned)hashes[i]);
    return used;
}
static inline int oot_ask_flag(char *out, size_t cap, uint32_t hash, int value) {
    return hash && cap >= 13 ? snprintf(out, cap, "F %08x %d", (unsigned)hash, value ? 1 : 0) : 0;
}
static inline int oot_ask_event(char *out, size_t cap, const char *name) {
    return oot_name_ok(name) && cap >= strlen(name) + 3 ? snprintf(out, cap, "E %s", name) : 0;
}

/* A line of states read: 1 when it answers the list of that sum and has n states of the four characters; they are in
   states then */
static inline int oot_states_parse(const char *line, uint32_t sum, int n, char *states) {
    uint32_t said;
    const char *at = line;
    if (!oot_hex8(&at, &said) || said != sum || *at++ != ' ' || (int)strlen(at) != n) return 0;
    for (int i = 0; i < n; i++)
        if (at[i] != '0' && at[i] != '1' && at[i] != '-' && at[i] != '?') return 0;
    memcpy(states, at, (size_t)n);
    return 1;
}
#endif
