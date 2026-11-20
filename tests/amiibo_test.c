/* tests/test_amiibo.py builds and runs this: host/amiibo.h on dumps of this file's own making (no figure's data, no key). */
#include "../host/amiibo.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static int failures;
#define CHECK(what, ...) do { if (!(what)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t kUid[7] = {0x04, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6};
static const uint8_t kModel[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x02};

/* a tag's first sixteen bytes and its figure's eight, at the places a tag's dump has them */
static void tag_dump(uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)(0xa0 + i * 7);   /* (what a tag encrypts: noise here) */
    d[0] = kUid[0], d[1] = kUid[1], d[2] = kUid[2], d[3] = (uint8_t)(0x88 ^ kUid[0] ^ kUid[1] ^ kUid[2]);
    memcpy(d + 4, kUid + 3, 4);
    d[8] = (uint8_t)(kUid[3] ^ kUid[4] ^ kUid[5] ^ kUid[6]), d[9] = 0x48, d[10] = 0x0f, d[11] = 0xe0;
    d[12] = 0xf1, d[13] = 0x10, d[14] = 0xff, d[15] = 0xee;
    memcpy(d + 0x54, kModel, 8);
}
/* the same tag as a decrypting tool writes it: the tag's bytes 8 to 15 first, 0 to 7 at 0x1d4, 0x54 on at 0x1dc */
static void decrypted_dump(uint8_t *d, size_t n) {
    uint8_t tag[572];
    tag_dump(tag, sizeof tag);
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)(0x11 + i * 3);
    memcpy(d, tag + 8, 8);
    memcpy(d + 0x1d4, tag, 8);
    memcpy(d + 0x1dc, tag + 0x54, 8);
    d[0x54] = 0x7f, d[0x5b] = 0x7f;   /* (nothing of a figure where a tag's dump has it) */
}

/* n bytes that end at a page nobody may read: a read past the dump's end stops the test */
static uint8_t *guarded(size_t n) {
    const size_t page = 4096, room = (n + page - 1) / page * page + page;
#ifdef _WIN32
    uint8_t *base = (uint8_t *)VirtualAlloc(NULL, room + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    if (!base || !VirtualProtect(base + room, page, PAGE_NOACCESS, &old)) exit(3);
#else
    uint8_t *base = (uint8_t *)mmap(NULL, room + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED || mprotect(base + room, page, PROT_NONE)) exit(3);
#endif
    return base + room - n;
}

static void test_parse(void) {
    static const size_t good[] = {532, 540, 572};
    static const size_t bad[] = {0, 1, 16, 0x5c, 520, 531, 533, 539, 541, 571, 573, 1024, 1025};
    for (unsigned i = 0; i < 3; i++) {
        uint8_t *d = guarded(good[i]);
        tag_dump(d, good[i]);
        AmiiboTag t;
        memset(&t, 0xee, sizeof t);
        CHECK(amiibo_parse(d, good[i], &t) == AMIIBO_OK, "a tag's dump of %u bytes is not taken", (unsigned)good[i]);
        CHECK(!memcmp(t.uid, kUid, 7) && !memcmp(t.model, kModel, 8) && !t.decrypted, "a tag's dump: wrong serial number or figure");
        decrypted_dump(d, good[i]);
        memset(&t, 0xee, sizeof t);
        CHECK(amiibo_parse(d, good[i], &t) == AMIIBO_OK, "a decrypted dump of %u bytes is not taken", (unsigned)good[i]);
        CHECK(!memcmp(t.uid, kUid, 7) && !memcmp(t.model, kModel, 8) && t.decrypted == 1, "a decrypted dump: wrong serial number or figure");
    }
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t *d = guarded(bad[i]);
        if (bad[i] >= 0x5c) tag_dump(d, bad[i]);   /* (everything a figure's dump has, but not its size) */
        AmiiboTag t, same;
        memset(&t, 0xee, sizeof t), memset(&same, 0xee, sizeof same);
        CHECK(amiibo_parse(d, bad[i], &t) == AMIIBO_BAD_SIZE, "a file of %u bytes is taken for a dump", (unsigned)bad[i]);
        CHECK(!memcmp(&t, &same, sizeof t), "a refused file of %u bytes wrote to the result", (unsigned)bad[i]);
    }
    {
        AmiiboTag t;
        CHECK(amiibo_parse(NULL, 540, &t) == AMIIBO_BAD_SIZE, "no data is taken for a dump");
    }
    /* one mark wrong at a time: a check byte of the serial number, the capability bytes, the figure's last byte */
    static const struct { size_t at; const char *what; } marks[] = {
        {0, "the serial number's first check byte"}, {3, "the serial number's first check byte"},
        {8, "the serial number's second check byte"}, {6, "the serial number's second check byte"},
        {12, "the capability bytes"}, {15, "the capability bytes"}, {0x5b, "the figure's last byte"}};
    for (unsigned i = 0; i < sizeof marks / sizeof marks[0]; i++) {
        uint8_t *d = guarded(540);
        tag_dump(d, 540);
        d[marks[i].at] ^= 0x40;
        AmiiboTag t, same;
        memset(&t, 0xee, sizeof t), memset(&same, 0xee, sizeof same);
        CHECK(amiibo_parse(d, 540, &t) == AMIIBO_NOT_A_FIGURE, "a dump with %s wrong is taken", marks[i].what);
        CHECK(!memcmp(&t, &same, sizeof t), "a refused dump wrote to the result");
    }
    {   /* (all zeros, all ones, noise: sizes of a dump, nothing of a figure) */
        uint8_t *d = guarded(572);
        AmiiboTag t;
        memset(d, 0, 572);
        CHECK(amiibo_parse(d, 572, &t) == AMIIBO_NOT_A_FIGURE, "572 zero bytes are taken for a figure");
        memset(d, 0xff, 572);
        CHECK(amiibo_parse(d, 572, &t) == AMIIBO_NOT_A_FIGURE, "572 0xff bytes are taken for a figure");
        for (unsigned i = 0; i < 572; i++) d[i] = (uint8_t)(i * 131 + 17);
        CHECK(amiibo_parse(d, 572, &t) == AMIIBO_NOT_A_FIGURE, "noise is taken for a figure");
    }
}

static void test_infos(void) {
    AmiiboTag t;
    memcpy(t.uid, kUid, 7), memcpy(t.model, kModel, 8), t.decrypted = 0;
    uint8_t tag[NFP_TAG_INFO_SIZE + 2], model[NFP_MODEL_INFO_SIZE + 2];
    memset(tag, 0x5a, sizeof tag), memset(model, 0x5a, sizeof model);
    amiibo_tag_info(&t, tag + 1);
    amiibo_model_info(&t, model + 1);
    CHECK(tag[0] == 0x5a && tag[NFP_TAG_INFO_SIZE + 1] == 0x5a && model[0] == 0x5a && model[NFP_MODEL_INFO_SIZE + 1] == 0x5a,
          "an info is written outside its 0x58 or 0x40 bytes");
    CHECK(NFP_TAG_INFO_SIZE == 0x58 && NFP_MODEL_INFO_SIZE == 0x40, "the SDK's structures' sizes");
    CHECK(!memcmp(tag + 1, kUid, 7) && tag[1 + 7] == 0 && tag[1 + 10] == 7, "TagInfo: the serial number and its length");
    CHECK(tag[1 + 0x20] == 1 && tag[1 + 0x24] == 2, "TagInfo: the protocol and the tag's type");
    for (int i = 11; i < NFP_TAG_INFO_SIZE; i++)
        if (i != 0x20 && i != 0x24) CHECK(tag[1 + i] == 0, "TagInfo byte %#x is not 0", i);
    /* the character's three bytes, the series, the number low byte first, the kind */
    static const uint8_t want[7] = {0x01, 0x02, 0x03, 0x07, 0x06, 0x05, 0x04};
    CHECK(!memcmp(model + 1, want, 7), "ModelInfo: the character, series, number and kind are not where the game reads them");
    for (int i = 7; i < NFP_MODEL_INFO_SIZE; i++) CHECK(model[1 + i] == 0, "ModelInfo byte %#x is not 0", i);
    CHECK((uint16_t)(model[1 + 4] | model[1 + 5] << 8) == 0x0506, "ModelInfo: the figure's number as the game's 16-bit read has it");
}

static void test_names(void) {
    static const char *const good[] = {"link.bin", "Link (Ocarina of Time).bin", "a", "\xe3\x83\xaa\xe3\x83\xb3\xe3\x82\xaf.bin"};
    static const char *const bad[] = {"", ".hidden.bin", "..", "a/b.bin", "a\\b.bin", "C:link.bin", "../link.bin", "a..b", " x.bin",
                                      "a\nb", "a*b", "a?b", "a|b", "a<b", "a>b", "a\"b"};
    for (unsigned i = 0; i < sizeof good / sizeof good[0]; i++) CHECK(amiibo_name_ok(good[i]), "the name \"%s\" is refused", good[i]);
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(!amiibo_name_ok(bad[i]), "the name \"%s\" is taken", bad[i]);
    char longest[202];
    memset(longest, 'a', 201), longest[201] = 0;
    CHECK(!amiibo_name_ok(longest), "a name of 201 bytes is taken");
    longest[200] = 0;
    CHECK(amiibo_name_ok(longest), "a name of 200 bytes is refused");
}

static AmiiboTag figure(void) {
    AmiiboTag t;
    memcpy(t.uid, kUid, 7), memcpy(t.model, kModel, 8), t.decrypted = 0;
    return t;
}

static void test_readers(void) {
    const int64_t ms = 1000000;
    uint8_t tag[NFP_TAG_INFO_SIZE], model[NFP_MODEL_INFO_SIZE];
    uint64_t handles[4] = {1, 2, 3, 4};
    Nfp n;
    nfp_reset(&n);
    /* nothing chosen: no reader, no handle is ours (the SDK's functions then answer as the stubs did) */
    CHECK(nfp_list(&n, handles, 4) == 0 && handles[0] == 1, "readers are listed before a figure was chosen");
    CHECK(nfp_reader(&n, nfp_handle(0)) < 0 && nfp_reader(&n, 0) < 0, "a handle is taken before a figure was chosen");
    CHECK(n.said == NFP_SAID_NONE && n.holder < 0, "a new state is not empty");
    const AmiiboTag t = figure();
    CHECK(nfp_choose(&n, &t) == 1, "the first figure chosen does not say the readers changed");
    CHECK(nfp_list(&n, handles, 4) == 2 && handles[0] == nfp_handle(0) && handles[1] == nfp_handle(1) && handles[2] == 3, "the two readers are not listed");
    CHECK(nfp_list(&n, handles, 1) == 1 && nfp_list(&n, handles, 0) == 0, "more readers are written than the game has room for");
    CHECK(nfp_reader(&n, nfp_handle(0)) == 0 && nfp_reader(&n, nfp_handle(1)) == 1 && nfp_reader(&n, 0) < 0 && nfp_reader(&n, 0x20) < 0,
          "the handles do not name the readers");
    CHECK(nfp_reader_npad(0) == 0 && nfp_reader_npad(1) == 0x20, "the readers are not the first controller's and the console's");
    CHECK(n.said == NFP_SAID_WAITING, "a figure chosen does not wait");
    /* not on a reader yet: it does not look for a tag */
    CHECK(nfp_state(&n, 0, 1 * ms) == NFP_INIT, "a reader looks before detection started");
    CHECK(nfp_tag_info(&n, 0, 1 * ms, tag) == NFP_WRONG_STATE && nfp_mount(&n, 0, 1 * ms) == NFP_WRONG_STATE &&
          nfp_model_info(&n, 0, 1 * ms, model) == NFP_WRONG_STATE, "a reader with no tag answers for one");
    /* present: the second reader looks first, and gets it */
    CHECK(nfp_start(&n, 1, 2 * ms) == 0 && nfp_state(&n, 1, 2 * ms) == NFP_ACTIVE && n.said == NFP_SAID_ON_READER, "the figure is not found by the reader that looks");
    CHECK(nfp_start(&n, 1, 2 * ms) == NFP_WRONG_STATE, "detection starts twice");
    CHECK(nfp_start(&n, 0, 3 * ms) == 0 && nfp_state(&n, 0, 3 * ms) == NFP_SEARCH, "one figure is on two readers");
    CHECK(nfp_tag_info(&n, 0, 3 * ms, tag) == NFP_WRONG_STATE, "the other reader answers for the figure");
    CHECK(nfp_model_info(&n, 1, 3 * ms, model) == NFP_WRONG_STATE, "ModelInfo is read before the tag is mounted");
    CHECK(nfp_tag_info(&n, 1, 3 * ms, tag) == 0 && !memcmp(tag, kUid, 7), "TagInfo of the figure found");
    CHECK(nfp_mount(&n, 1, 4 * ms) == 0 && nfp_state(&n, 1, 4 * ms) == NFP_MOUNT, "the tag found is not mounted");
    CHECK(nfp_state(&n, 1, 4 * ms + 100 * NFP_HOLD_NS) == NFP_MOUNT, "a figure is lifted off before the game read it");
    CHECK(nfp_model_info(&n, 1, 5 * ms, model) == 0 && model[0] == kModel[0] && model[3] == kModel[6], "ModelInfo of the mounted figure");
    CHECK(nfp_state(&n, 1, 5 * ms + NFP_HOLD_NS - 1) == NFP_MOUNT, "the figure is lifted off too early");
    /* removed: after the hold */
    CHECK(nfp_state(&n, 1, 5 * ms + NFP_HOLD_NS) == NFP_DEACTIVE && n.said == NFP_SAID_READ && n.holder < 0, "the figure read is not lifted off");
    CHECK(nfp_tag_info(&n, 1, 6 * ms + NFP_HOLD_NS, tag) == NFP_WRONG_STATE, "a figure lifted off still answers");
    CHECK(nfp_stop(&n, 1) == 0 && nfp_state(&n, 1, 7 * ms + NFP_HOLD_NS) == NFP_INIT, "StopDetection does not make the reader new");
    /* one choice is one touch */
    CHECK(nfp_start(&n, 1, 8 * ms + NFP_HOLD_NS) == 0 && nfp_state(&n, 1, 9 * ms + 9 * NFP_HOLD_NS) == NFP_SEARCH, "a figure read once is found again");
    CHECK(nfp_choose(&n, &t) == 0, "a second figure says the readers changed again");
    CHECK(nfp_state(&n, 0, 10 * NFP_HOLD_NS) == NFP_ACTIVE, "a figure chosen while a reader looks is not found");
    /* taken away by the player while the game has it */
    nfp_take_away(&n);
    CHECK(nfp_state(&n, 0, 10 * NFP_HOLD_NS) == NFP_DEACTIVE && n.said == NFP_SAID_NONE && n.holder < 0, "a figure taken away is still there");
    CHECK(nfp_list(&n, handles, 4) == 2, "the readers go away with the figure");
    /* the game stops looking with the figure on the reader: that was its touch */
    CHECK(nfp_stop(&n, 0) == 0 && nfp_choose(&n, &t) == 0 && nfp_start(&n, 0, 11 * NFP_HOLD_NS) == 0 && nfp_state(&n, 0, 11 * NFP_HOLD_NS) == NFP_ACTIVE,
          "a figure chosen again is not found");
    CHECK(nfp_stop(&n, 0) == 0 && n.holder < 0 && !n.waiting && nfp_state(&n, 0, 12 * NFP_HOLD_NS) == NFP_INIT, "StopDetection leaves the figure on the reader");
    CHECK(nfp_stop(&n, 0) == 0, "StopDetection on a new reader is an error (the game's task makes that call)");
    /* Initialize: every reader new, what was chosen still waits */
    CHECK(nfp_choose(&n, &t) == 0 && nfp_start(&n, 0, 13 * NFP_HOLD_NS) == 0 && nfp_mount(&n, 0, 13 * NFP_HOLD_NS) == 0, "mount");
    nfp_restart(&n);
    CHECK(n.state[0] == NFP_INIT && n.state[1] == NFP_INIT && n.holder < 0, "Initialize leaves a reader as it was");
    CHECK(nfp_choose(&n, &t) == 0 && n.waiting, "choose after Initialize");
    nfp_restart(&n);
    CHECK(n.waiting && nfp_start(&n, 1, 14 * NFP_HOLD_NS) == 0 && nfp_state(&n, 1, 14 * NFP_HOLD_NS) == NFP_ACTIVE, "a figure chosen before Initialize is forgotten");
}

/* The game's task as its code has it (the NfpMgr thread's loop at 0x83b84230, request 2 with the flag that waits for the
   figure to be lifted off): step 1 starts detection unless the reader is in a state 1 to 4; step 2 stops and starts over
   in the states 0, 4 and 5, waits in 1 and 3, and in 2 reads the TagInfo and mounts; step 4 reads the ModelInfo and
   reports; step 3 waits while the state is 2 or 4, then stops detection in a state 1 to 4. Returns what it reported
   (1 the figure, 3 an error of the reader's) or 0 when it still waits after `ticks` of 10 ms; *calls counts the
   SDK calls made */
static int game_task(Nfp *n, int reader, int64_t *now, int ticks, int *calls, uint8_t *tag, uint8_t *model) {
    int step = 1, reported = 0;
    for (int tick = 0; tick < ticks && step; tick++) {
        uint32_t result = 0;
        const int st = (++*calls, nfp_state(n, reader, *now));
        if (step == 1) {
            if (!(st >= 1 && st <= 4)) result = (++*calls, nfp_start(n, reader, *now));
            step = 2;
        } else if (step == 2) {
            if (st == 0 || st == 4 || st == 5) result = (++*calls, nfp_stop(n, reader)), step = 1;
            else if (st == 2) {
                result = (++*calls, nfp_tag_info(n, reader, *now, tag));
                if (!result) result = (++*calls, nfp_mount(n, reader, *now));
                if (!result) step = 4;
            } else *now += 10000000;
        } else if (step == 4) {
            result = (++*calls, nfp_model_info(n, reader, *now, model));
            if (!result) reported = 1, step = 3;
        } else if (step == 3) {
            if (st == 2 || st == 4) *now += 10000000;
            else {
                if (st >= 1 && st <= 4) ++*calls, nfp_stop(n, reader);
                step = 0;
            }
        }
        if (result) return 3;
    }
    return step ? 0 : reported;
}

static void test_game(void) {
    uint8_t tag[NFP_TAG_INFO_SIZE], model[NFP_MODEL_INFO_SIZE];
    const AmiiboTag t = figure();
    for (int reader = 0; reader < NFP_READERS; reader++) {
        Nfp n;
        nfp_reset(&n);
        nfp_choose(&n, &t);
        nfp_take_away(&n);   /* (the readers listed, no figure) */
        int64_t now = 1000;
        int calls = 0;
        CHECK(game_task(&n, reader, &now, 500, &calls, tag, model) == 0, "the game's task ends with no figure");
        CHECK(calls <= 500 + 1 && n.state[reader] == NFP_SEARCH, "with no figure the task does not wait in the search state (%d calls)", calls);
        CHECK(now >= 4900000000LL, "with no figure the task spins instead of waiting");
        /* the figure chosen while the game waits: the same task goes on and ends */
        nfp_choose(&n, &t);
        memset(tag, 0, sizeof tag), memset(model, 0, sizeof model);
        calls = 0;
        const int64_t from = now;
        CHECK(game_task(&n, reader, &now, 500, &calls, tag, model) == 1, "the game's task does not read the figure");
        CHECK(!memcmp(tag, kUid, 7) && tag[10] == 7 && !memcmp(model, kModel, 3), "the game is handed another figure");
        CHECK(now - from >= NFP_HOLD_NS && now - from < NFP_HOLD_NS + 100000000, "the figure is not lifted off a second after it was read");
        CHECK(n.state[reader] == NFP_INIT && n.holder < 0 && !n.waiting && n.said == NFP_SAID_READ, "the task does not leave the reader new");
    }
}

int main(void) {
    test_parse();
    test_infos();
    test_names();
    test_readers();
    test_game();
    if (failures) return printf("%d failures\n", failures), 1;
    printf("ok\n");
    return 0;
}
