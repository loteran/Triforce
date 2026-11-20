/* tests/test_debug_link.py builds this with the game side's functions cut out of host/hos_main.c (HOST_FUNCTIONS) and runs
   it: gpu/vk/oot_debug_link.h, and those functions against a stand-in for the game's memory and functions. Nothing of the
   game is here: the names and the layout below are the test's own, laid out as host/hos_main.c's notes say GameData is. */
#include "../gpu/vk/oot_debug_link.h"
#include <stdarg.h>
#include <stdlib.h>

HOST_CONSTANTS

static int failures;
#define CHECK(what, ...) do { if (!(what)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the stand-in: a piece of guest memory, and counters of every read, write and call ---- */
typedef struct Ctx { uint64_t x[32]; } Ctx;
typedef void (*A64Fn)(Ctx *);
#define MEM_BASE 0x84c00000ULL
#define MEM_SIZE 0x200000ULL
static uint8_t mem[MEM_SIZE];
static long loads, stores, calls;
static uint8_t *at(uint64_t a, size_t n) {
    if (a < MEM_BASE || a + n > MEM_BASE + MEM_SIZE) {
        printf("FAIL: a read or write outside the stand-in's memory at %#llx\n", (unsigned long long)a);
        exit(2);
    }
    return mem + (a - MEM_BASE);
}
#define A64_PTR(a) at((a), 1)
static uint8_t recomp_load8(Ctx *c, uint64_t a) { (void)c, loads++; return *at(a, 1); }
static uint32_t recomp_load32(Ctx *c, uint64_t a) { (void)c, loads++; uint32_t v; memcpy(&v, at(a, 4), 4); return v; }
static uint64_t recomp_load64(Ctx *c, uint64_t a) { (void)c, loads++; uint64_t v; memcpy(&v, at(a, 8), 8); return v; }
static void recomp_store64(Ctx *c, uint64_t a, uint64_t v) { (void)c, stores++; memcpy(at(a, 8), &v, 8); }
static void put32(uint64_t a, uint32_t v) { memcpy(at(a, 4), &v, 4); }
static void put64(uint64_t a, uint64_t v) { memcpy(at(a, 8), &v, 8); }

static int64_t clock_ns;
static int64_t hos_now_ns(void) { return clock_ns; }
static void hos_log(const char *fmt, ...) { (void)fmt; }
static struct { int64_t requested_at; } teleport;
static const char *const oot_item_names[] = {"Stand_In"};
static int save_loaded;
static int oot_pouch_count(Ctx *c, const char *name) { (void)c, (void)name; return save_loaded ? 3 : -1; }
static int map_loading;
static int oot_gamedata_bool(Ctx *c, uint32_t hash, int *value) { (void)c, (void)hash; *value = map_loading; return 1; }

/* what the pages send, a byte a read, and what they are told */
static char asked[4096];
static size_t asked_at;
static char said_text[512], said_lines[8][512];
static int said_kinds[8], said_n;
static size_t said_used;
static void ask(const char *line) {
    const size_t used = strlen(asked);
    snprintf(asked + used, sizeof asked - used, "%s\n", line);
}
static int ids_bad;
int rx_option(int id, int otherwise) {
    (void)otherwise;
    if (id != HOS_OPT_DEBUG_ASK) return ids_bad++, 0;
    if (!asked[asked_at]) return asked_at = 0, asked[0] = 0, 0;
    return (unsigned char)asked[asked_at++];
}
void rx_option_set(int id, int value) {
    if (id == HOS_OPT_DEBUG_TEXT && said_used + 1 < sizeof said_text) said_text[said_used++] = (char)value;
    else if (id == HOS_OPT_DEBUG_SAID && said_n < 8) {
        said_text[said_used] = 0, said_used = 0;
        said_kinds[said_n] = value;
        snprintf(said_lines[said_n++], sizeof said_lines[0], "%s", said_text);
    } else ids_bad++;
}
static void said_clear(void) { said_n = 0, said_used = 0; }

/* GameData as host/hos_main.c reads it: the manager behind two pointers at OOT_GAMEDATA_SLOT; +16 the Bools' count, +24
   their 24-byte entries (the value at +22), +32 the (hash, index) table probed from hash % size, +44 its size */
#define MGR 0x84d00000ULL
#define ENTRIES 0x84d01000ULL
#define TABLE 0x84d02000ULL
#define EVENTS 0x84d03000ULL
#define STACK_TOP 0x84dff000ULL
#define NAME_BUF 0x84d04000ULL
enum { FLAGS = 5, TABLE_SIZE = 8 };
/* (three of them fall on the same cell of the table: 0x10, 0x18 and 0x20 are all 0 modulo 8) */
static const uint32_t flag_hash[FLAGS] = {0x00000010u, 0x00000018u, 0x00000020u, 0xcea220d3u, 0x00000007u};
static const uint32_t no_such_flag = 0x00000028u;   /* (the same cell again, and not in the table) */

/* the game's functions the tools call, by their addresses */
static struct { uint64_t mgr, hash; uint32_t value; long n; } set_bool, set_int;
static long made_args, requests, links, releases;
static uint64_t request_events, request_arg, link_at, link_actor;
static char requested_name[128];
static uint64_t request_result = 1;
static void spy_set_bool(Ctx *c) {
    calls++, set_bool.n++;
    set_bool.mgr = c->x[0], set_bool.value = (uint32_t)c->x[1], set_bool.hash = c->x[2];
    for (int i = 0; i < FLAGS; i++)   /* (the game's own drain, a frame later: the value written) */
        if (flag_hash[i] == (uint32_t)c->x[2]) mem[ENTRIES - MEM_BASE + 24 * i + 22] = (uint8_t)(c->x[1] & 1);
}
static void spy_set_int(Ctx *c) { (void)c, calls++, set_int.n++; }
static uint32_t name_room = 96;
static void spy_make_arg(Ctx *c) {   /* (the request's name buffer and its capacity, where oot_start_event looks for them) */
    calls++, made_args++;
    put64(c->x[0] + 6800, NAME_BUF);
    put32(c->x[0] + 6808, name_room);
}
static void spy_set_link(Ctx *c) { calls++, links++, link_at = c->x[0], link_actor = c->x[1]; }
static void spy_release(Ctx *c) { (void)c, calls++, releases++; }
static void spy_request(Ctx *c) {
    calls++, requests++;
    request_events = c->x[0], request_arg = c->x[1];
    snprintf(requested_name, sizeof requested_name, "%s", (const char *)at(NAME_BUF, 96));
    c->x[0] = request_result;
}
static A64Fn a64_lookup(uint64_t address) {
    switch (address) {
    case 0x806a2b20ULL: return spy_set_bool;
    case 0x80183b70ULL: return spy_set_int;
    case 0x80d99f10ULL: return spy_make_arg;
    case 0x80d9a820ULL: return spy_request;
    case 0x802dc440ULL: return spy_set_link;
    case 0x8022b6f0ULL: return spy_release;
    }
    return NULL;
}

HOST_FUNCTIONS

static void build_game(void) {
    memset(mem, 0, sizeof mem);
    put64(OOT_GAMEDATA_SLOT, MGR - 0x100), put64(MGR - 0x100, MGR);
    put64(OOT_EVENTMGR_SLOT, EVENTS - 0x100), put64(EVENTS - 0x100, EVENTS);
    put32(MGR + 16, FLAGS), put64(MGR + 24, ENTRIES), put64(MGR + 32, TABLE), put32(MGR + 44, TABLE_SIZE);
    for (uint32_t i = 0; i < FLAGS; i++) {
        uint32_t cell = flag_hash[i] % TABLE_SIZE;
        while (recomp_load32(NULL, TABLE + 8ULL * cell)) cell = (cell + 1) % TABLE_SIZE;
        put32(TABLE + 8ULL * cell, flag_hash[i]), put32(TABLE + 8ULL * cell + 4, i);
        mem[ENTRIES - MEM_BASE + 24 * i + 22] = (uint8_t)(i & 1);   /* (set: the second and the fourth) */
    }
}
static Ctx fresh_ctx(void) {
    Ctx c;
    for (int i = 0; i < 32; i++) c.x[i] = 0x1111000000000000ULL + (uint64_t)i;
    c.x[31] = STACK_TOP;
    return c;
}
static void counters_clear(void) {
    loads = stores = calls = 0;
    memset(&set_bool, 0, sizeof set_bool), memset(&set_int, 0, sizeof set_int);
    made_args = requests = links = releases = 0;
    requested_name[0] = 0;
    said_clear();
}

static void test_link(void) {
    /* the hash, on the names and numbers host/hos_main.c's own tools already use */
    CHECK(oot_name_hash("Sequence_IsChildhood", 20) == 0xcea220d3u, "the hash of Sequence_IsChildhood");
    CHECK(oot_name_hash("Sequence_IsLoadingBanc", 22) == 0xef2b60c2u, "the hash of Sequence_IsLoadingBanc");
    CHECK(oot_name_hash("Sequence_CurrentBanc", 20) == 0x1d6189dau, "the hash of Sequence_CurrentBanc");
    CHECK(oot_name_hash("Sequence_ReservedNextBanc", 25) == 0xee27bf71u, "the hash of Sequence_ReservedNextBanc");
    CHECK(oot_name_hash("", 0) == 0 && oot_name_hash("a", 1) == 0x3c2569b2u && oot_name_hash("ab", 2) == 0x9bbfd75fu &&
          oot_name_hash("abc", 3) == 0xb3dd93fau && oot_name_hash("abcd", 4) == 0x43ed676au, "MurmurHash3's known answers");
    CHECK(oot_name_ok("Dm_SY_Some.Event-01") && oot_name_ok("a") && !oot_name_ok("") && !oot_name_ok("a b") && !oot_name_ok("a\n") &&
          !oot_name_ok("a/b") && !oot_name_ok("\xc3\xa9"), "which names are names");
    char name[OOT_NAME_MAX + 3];
    memset(name, 'n', sizeof name);
    name[OOT_NAME_MAX] = 0;
    CHECK(oot_name_ok(name), "a name of OOT_NAME_MAX characters is refused");
    name[OOT_NAME_MAX] = 'n', name[OOT_NAME_MAX + 1] = 0;
    CHECK(!oot_name_ok(name), "a longer name is taken");

    char line[OOT_LINE_MAX + 8], states[OOT_WATCH_MAX];
    uint32_t many[OOT_WATCH_MAX + 1];
    for (int i = 0; i <= OOT_WATCH_MAX; i++) many[i] = 0x9e3779b9u * (uint32_t)(i + 1);
    OotAsk a;
    CHECK(oot_ask_watch(line, sizeof line, many, OOT_WATCH_MAX) == 1 + 9 * OOT_WATCH_MAX && (int)strlen(line) <= OOT_LINE_MAX, "the longest W line's length");
    CHECK(oot_ask_parse(line, &a) && a.kind == 'W' && a.count == OOT_WATCH_MAX && !memcmp(a.watch, many, sizeof a.watch), "a W line read back");
    CHECK(!oot_ask_watch(line, sizeof line, many, OOT_WATCH_MAX + 1) && !oot_ask_watch(line, 10, many, 1), "a W line too long is made");
    CHECK(oot_ask_watch(line, sizeof line, many, 0) == 1 && oot_ask_parse(line, &a) && a.kind == 'W' && a.count == 0, "the empty W line");
    CHECK(oot_ask_flag(line, sizeof line, 0xcea220d3u, 1) && !strcmp(line, "F cea220d3 1") && oot_ask_parse(line, &a) && a.kind == 'F' &&
          a.hash == 0xcea220d3u && a.value == 1, "an F line");
    CHECK(oot_ask_flag(line, sizeof line, 0x0000000fu, 0) && !strcmp(line, "F 0000000f 0") && oot_ask_parse(line, &a) && a.hash == 0xf && a.value == 0, "an F line that clears");
    CHECK(!oot_ask_flag(line, sizeof line, 0, 1), "an F line for no flag");
    CHECK(oot_ask_event(line, sizeof line, "Dm_Stand_In") && !strcmp(line, "E Dm_Stand_In") && oot_ask_parse(line, &a) && a.kind == 'E' &&
          !strcmp(a.name, "Dm_Stand_In"), "an E line");
    CHECK(!oot_ask_event(line, sizeof line, "two words") && !oot_ask_event(line, 5, "Dm_Stand_In"), "an E line that should not be made");
    static const char *const bad[] = {"", "X", "W 1234567", "W 12345678 ", "W12345678", "W 1234567g", "W 12345678  12345678", "W 1234ABCD",
                                      "F", "F 12345678", "F 12345678 2", "F 12345678 1 ", "F 00000000 1", "F 1234567 1", "F  12345678 1",
                                      "E", "E ", "E a b", "E a/b", "e Dm_Stand_In", " E Dm_Stand_In"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(!oot_ask_parse(bad[i], &a), "the line \"%s\" is taken", bad[i]);
    {   /* (one more flag than a W line may have) */
        char longer[OOT_LINE_MAX + 16];
        int used = oot_ask_watch(longer, sizeof longer, many, OOT_WATCH_MAX);
        snprintf(longer + used, sizeof longer - (size_t)used, " %08x", 1u);
        CHECK(!oot_ask_parse(longer, &a), "a W line of more flags than OOT_WATCH_MAX is taken");
    }
    const uint32_t two[2] = {1, 2}, swapped[2] = {2, 1};
    CHECK(oot_watch_sum(two, 2) != oot_watch_sum(swapped, 2) && oot_watch_sum(two, 2) != oot_watch_sum(two, 1) && oot_watch_sum(two, 0) != 0,
          "the watch list's sum does not tell lists apart");
    snprintf(line, sizeof line, "%08x 10-?", (unsigned)oot_watch_sum(many, 4));
    CHECK(oot_states_parse(line, oot_watch_sum(many, 4), 4, states) && !memcmp(states, "10-?", 4), "a line of states");
    CHECK(!oot_states_parse(line, oot_watch_sum(many, 3), 4, states) && !oot_states_parse(line, oot_watch_sum(many, 4), 3, states) &&
          !oot_states_parse(line, oot_watch_sum(many, 4), 5, states), "states are taken for another list");
    snprintf(line, sizeof line, "%08x 10x?", (unsigned)oot_watch_sum(many, 4));
    CHECK(!oot_states_parse(line, oot_watch_sum(many, 4), 4, states), "a state that is none of the four is taken");
}

static void test_flags(void) {
    build_game();
    Ctx c = fresh_ctx();
    const Ctx before = c;
    /* nothing asked, nothing watched: nothing of the game is read, called or written */
    counters_clear();
    for (int i = 0; i < 100; i++) oot_link_tick(&c, 0x84d05000ULL, clock_ns += 16000000);
    CHECK(!loads && !stores && !calls && !said_n, "an idle tick reads the game, calls it or says something (%ld loads, %ld calls)", loads, calls);

    /* the states of four flags and one the save data does not have, with a save loaded */
    char line[OOT_LINE_MAX + 1], want[64];
    const uint32_t list[5] = {flag_hash[0], flag_hash[1], no_such_flag, flag_hash[3], flag_hash[2]};
    save_loaded = 1;
    oot_ask_watch(line, sizeof line, list, 5);
    ask(line);
    oot_link_tick(&c, 0, clock_ns += 16000000);
    CHECK(!said_n, "the states are sent before the line's own tick is over");
    oot_link_tick(&c, 0, clock_ns += 100000000);
    snprintf(want, sizeof want, "%08x 01-10", (unsigned)oot_watch_sum(list, 5));
    CHECK(said_n == 1 && said_kinds[0] == OOT_SAID_FLAGS && !strcmp(said_lines[0], want), "the watched flags' states: \"%s\", wanted \"%s\"", said_lines[0], want);
    CHECK(!stores && !calls && !set_bool.n, "reading the states wrote to the game");
    said_clear();
    oot_link_tick(&c, 0, clock_ns += 400000000);
    CHECK(!said_n, "the states come more often than twice a second");
    oot_link_tick(&c, 0, clock_ns += 100000000);
    CHECK(said_n == 1 && !strcmp(said_lines[0], want), "the states do not come twice a second");

    /* toggle: the third flag set, through the game's own setter and nothing else */
    counters_clear();
    oot_ask_flag(line, sizeof line, flag_hash[2], 1);
    ask(line);
    oot_link_tick(&c, 0, clock_ns += 16000000);
    CHECK(set_bool.n == 1 && set_bool.mgr == MGR && set_bool.hash == flag_hash[2] && set_bool.value == 1 && calls == 1,
          "a flag is not set through the game's SetBool (manager, value, hash): %ld calls", calls);
    CHECK(!stores && !set_int.n, "setting a flag wrote to the game's memory itself");
    CHECK(said_n == 1 && said_kinds[0] == OOT_SAID_STATUS && !strcmp(said_lines[0], "Flag set."), "the page is not told the flag was set");
    said_clear();
    oot_link_tick(&c, 0, clock_ns += 100000000);
    snprintf(want, sizeof want, "%08x 01-11", (unsigned)oot_watch_sum(list, 5));
    CHECK(said_n == 1 && !strcmp(said_lines[0], want), "the new state is not sent soon after: \"%s\"", said_lines[0]);
    counters_clear();
    oot_ask_flag(line, sizeof line, flag_hash[3], 0);
    ask(line);
    oot_link_tick(&c, 0, clock_ns += 16000000);
    CHECK(set_bool.n == 1 && set_bool.hash == flag_hash[3] && set_bool.value == 0 && !strcmp(said_lines[0], "Flag cleared."), "a flag is not cleared");

    /* the guards: a flag the save data does not have, and no save loaded */
    static uint8_t snapshot[sizeof mem];
    memcpy(snapshot, mem, sizeof mem);
    counters_clear();
    oot_ask_flag(line, sizeof line, no_such_flag, 1);
    ask(line);
    oot_link_tick(&c, 0, clock_ns += 16000000);
    CHECK(!calls && !stores && !memcmp(snapshot, mem, sizeof mem), "a flag the save data does not have is set");
    CHECK(said_n == 1 && !strcmp(said_lines[0], "The save data has no such flag."), "the page is not told why: \"%s\"", said_lines[0]);
    save_loaded = 0;
    counters_clear();
    for (int i = 0; i < FLAGS; i++) {
        oot_ask_flag(line, sizeof line, flag_hash[i], !(i & 1));
        ask(line);
    }
    oot_link_tick(&c, 0, clock_ns += 16000000);
    CHECK(!calls && !stores && !loads && !memcmp(snapshot, mem, sizeof mem), "with no save loaded a flag is set, or the game's data read (%ld calls, %ld loads)", calls, loads);
    CHECK(said_n == FLAGS && !strcmp(said_lines[0], "Load a save first.") && !strcmp(said_lines[FLAGS - 1], "Load a save first."), "the page is not told to load a save");
    said_clear();
    oot_link_tick(&c, 0, clock_ns += 600000000);
    snprintf(want, sizeof want, "%08x ?????", (unsigned)oot_watch_sum(list, 5));
    CHECK(said_n == 1 && !strcmp(said_lines[0], want) && !loads, "with no save loaded the states are not all unknown: \"%s\"", said_lines[0]);

    /* lines that are not the pages': dropped whole, the next line still taken */
    save_loaded = 1;
    counters_clear();
    ask("F 12345678"), ask("G 00000010 1"), ask("F 00000010 1 1"), ask("");
    {
        char big[OOT_LINE_MAX + 200];
        memset(big, 'F', sizeof big - 1), big[sizeof big - 1] = 0;
        ask(big);
    }
    oot_link_tick(&c, 0, clock_ns += 16000000);
    CHECK(!calls && !stores && !said_n, "a line that is not one of the pages' did something");
    oot_ask_flag(line, sizeof line, flag_hash[0], 1);
    ask(line);
    oot_link_tick(&c, 0, clock_ns += 16000000);
    CHECK(set_bool.n == 1 && set_bool.hash == flag_hash[0], "the line after a bad one is lost");
    /* no flag watched any more: nothing sent, nothing read */
    ask("W");
    oot_link_tick(&c, 0, clock_ns += 16000000);
    counters_clear();
    for (int i = 0; i < 100; i++) oot_link_tick(&c, 0, clock_ns += 16000000);
    CHECK(!loads && !calls && !said_n, "an empty watch list still reads the game");
    CHECK(!memcmp(&c, &before, sizeof c), "the game's registers are not as they were");
    CHECK(!ids_bad, "another option than the pages' three is read or set");
}

static void test_events(void) {
    build_game();
    Ctx c = fresh_ctx();
    const Ctx before = c;
    const uint64_t link = 0x84d05000ULL;
    char line[OOT_LINE_MAX + 1];
    static uint8_t snapshot[sizeof mem];
    memcpy(snapshot, mem, sizeof mem);
    oot_ask_event(line, sizeof line, "Dm_Stand_In");
    /* the guards: no save loaded, another event playing, a map loading, a teleport under way */
    static const char *const why[4] = {"Load a save first.", "Another event is playing.", "A map is loading.", "A teleport is under way."};
    for (int guard = 0; guard < 4; guard++) {
        save_loaded = guard != 0, map_loading = guard == 2;
        put64(EVENTS + 104, guard == 1 ? 0x84d06000ULL : 0);
        teleport.requested_at = guard == 3 ? clock_ns - 1000000000LL : 0;
        counters_clear();
        ask(line);
        oot_link_tick(&c, link, clock_ns += 16000000);
        CHECK(!calls && !stores && !made_args && !requests, "an event is asked for although: %s", why[guard]);
        CHECK(said_n == 1 && said_kinds[0] == OOT_SAID_STATUS && !strcmp(said_lines[0], why[guard]), "the page is told \"%s\", not \"%s\"", said_lines[0], why[guard]);
    }
    put64(EVENTS + 104, 0), map_loading = 0, teleport.requested_at = 0, save_loaded = 1;
    CHECK(!memcmp(snapshot, mem, sizeof mem), "a refused event wrote to the game");
    /* started: one request of the event manager, the name in the request's own buffer, Link its starter */
    counters_clear();
    ask(line);
    oot_link_tick(&c, link, clock_ns += 16000000);
    CHECK(made_args == 1 && requests == 1 && links == 1 && releases == 2, "the request is not made once and its links let go (%ld, %ld, %ld, %ld)",
          made_args, requests, links, releases);
    CHECK(request_events == EVENTS && !strcmp(requested_name, "Dm_Stand_In"), "the request is not the event manager's, or not by the event's name: \"%s\"", requested_name);
    CHECK(link_actor == link && link_at == request_arg + 0x1950, "Link is not the request's starter");
    CHECK(request_arg < STACK_TOP - OOT_EVENT_ARG_SIZE && !(request_arg & 15) && request_arg > STACK_TOP - 0x4000, "the request is not built under the game's stack");
    CHECK(said_n == 1 && !strcmp(said_lines[0], "Dm_Stand_In asked of the game."), "the page is told \"%s\"", said_lines[0]);
    CHECK(!memcmp(&c, &before, sizeof c), "the game's registers are not as they were after a request");
    request_result = 0;
    counters_clear();
    ask(line);
    oot_link_tick(&c, link, clock_ns += 16000000);
    CHECK(requests == 1 && !strcmp(said_lines[0], "The game did not take Dm_Stand_In."), "a refused request is not told: \"%s\"", said_lines[0]);
    request_result = 1;
    /* a name longer than the request's buffer: not sent */
    char longest[OOT_NAME_MAX + 1];
    memset(longest, 'n', OOT_NAME_MAX), longest[OOT_NAME_MAX] = 0;
    oot_ask_event(line, sizeof line, longest);   /* (95 characters and their 0: the stand-in's buffer has room for exactly 96) */
    counters_clear();
    ask(line);
    oot_link_tick(&c, link, clock_ns += 16000000);
    CHECK(requests == 1 && !strcmp(requested_name, longest), "the longest name does not fit a buffer of its size");
    name_room = 95;   /* (a buffer one byte too small for it: the request is not made, and its links are let go all the same) */
    counters_clear();
    ask(line);
    oot_link_tick(&c, link, clock_ns += 16000000);
    CHECK(made_args == 1 && !requests && !links && releases == 2 && !strcmp(said_lines[0], "The request could not be built."),
          "a name that does not fit the request's buffer is sent, or written past it: \"%s\"", said_lines[0]);
    name_room = 96;
    CHECK(!memcmp(&c, &before, sizeof c), "the game's registers are not as they were after a request that was not built");
}

int main(void) {
    test_link();
    test_flags();
    test_events();
    if (failures) return printf("%d failures\n", failures), 1;
    printf("ok\n");
    return 0;
}
