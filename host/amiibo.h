/* (Triforce) An amiibo from a file, and the NFC reader the game is shown (host/nn_nfp.c has the SDK's functions; no SDK,
   no game and no file here: tests/test_amiibo.py compiles this header and runs it on bytes of its own making).

   What the game asks of a figure (read in its own code, build e0f3ccfa: the NfpMgr thread's task at 0x83b84230 and the
   amiibo sequence's callback at 0x8328b480): it lists the readers, starts detection on one, waits for a tag by asking for
   the reader's state, reads the tag's TagInfo, mounts it with the mount target 1 (the tag's read-only part), reads its
   ModelInfo, hands both to the game and then waits for the figure to be lifted off. Of the TagInfo it keeps the serial
   number; of the ModelInfo the figure's character (bytes 0 to 2), its series (byte 3) and its number (bytes 4 and 5, which
   it looks up in its own list). The game has no other request: the owner, the nickname and a game's own data on a
   figure are never opened.

   Both are in the clear in a dump of a figure's NTAG215 tag, so no key is needed and none is read: the serial number in
   pages 0 and 1 (bytes 0 to 2 and 4 to 7; bytes 3 and 8 are its check bytes) and the eight bytes that say which figure it
   is in pages 21 and 22 (0x54 to 0x5b: the character, its variant, the kind of figure, the number high byte first, the
   series, and 2). A dump that a tool has already decrypted keeps the same bytes in other places (amiitool's layout: the
   tag's bytes 8 to 15 first, the serial number's page at 0x1d4, the figure's at 0x1dc). Everything a tag keeps encrypted
   is left unread. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { AMIIBO_FILE_MAX = 1024 };   /* (the most of a file that is read: a dump is 532, 540 or 572 bytes) */
enum { AMIIBO_OK = 0, AMIIBO_BAD_SIZE, AMIIBO_NOT_A_FIGURE };

typedef struct AmiiboTag {
    uint8_t uid[7];     /* the tag's serial number */
    uint8_t model[8];   /* the tag's bytes 0x54 to 0x5b */
    int decrypted;      /* 1: read from a decrypted dump's layout */
} AmiiboTag;

/* a dump's size: an NTAG215's 135 pages (540), the same without its last two (532), or with the tag's signature (572) */
static inline int amiibo_size_ok(size_t n) { return n == 532 || n == 540 || n == 572; }

/* the serial number's two check bytes, the tag's capability bytes (an amiibo's are f1 10 ff ee) and the 2 that ends the
   figure's eight bytes: uid8 the tag's bytes 0 to 7, rest8 its bytes 8 to 15, model its bytes 0x54 to 0x5b */
static inline int amiibo_marks_ok(const uint8_t *uid8, const uint8_t *rest8, const uint8_t *model) {
    static const uint8_t cc[4] = {0xf1, 0x10, 0xff, 0xee};
    if ((uint8_t)(0x88 ^ uid8[0] ^ uid8[1] ^ uid8[2]) != uid8[3]) return 0;
    if ((uint8_t)(uid8[4] ^ uid8[5] ^ uid8[6] ^ uid8[7]) != rest8[0]) return 0;
    return !memcmp(rest8 + 4, cc, 4) && model[7] == 2;
}

/* A dump's serial number and figure: AMIIBO_OK, or why not. Nothing past data[n - 1] is read, and nothing is written
   to *out unless the dump is taken */
static inline int amiibo_parse(const uint8_t *data, size_t n, AmiiboTag *out) {
    if (!data || !amiibo_size_ok(n)) return AMIIBO_BAD_SIZE;
    const uint8_t *uid8 = NULL, *model = NULL;
    int decrypted = 0;
    if (amiibo_marks_ok(data, data + 8, data + 0x54)) uid8 = data, model = data + 0x54;   /* (as the tag has them) */
    else if (amiibo_marks_ok(data + 0x1d4, data, data + 0x1dc)) uid8 = data + 0x1d4, model = data + 0x1dc, decrypted = 1;
    if (!uid8) return AMIIBO_NOT_A_FIGURE;
    memcpy(out->uid, uid8, 3);
    memcpy(out->uid + 3, uid8 + 4, 4);
    memcpy(out->model, model, 8);
    out->decrypted = decrypted;
    return AMIIBO_OK;
}

/* nn::nfp::TagInfo (0x58 bytes): the serial number in its first ten bytes, its length at 10, the protocol at 0x20 (1,
   NFC-A) and the tag's type at 0x24 (2, a type 2 tag) */
enum { NFP_TAG_INFO_SIZE = 0x58, NFP_MODEL_INFO_SIZE = 0x40 };
static inline void amiibo_tag_info(const AmiiboTag *t, uint8_t out[NFP_TAG_INFO_SIZE]) {
    memset(out, 0, NFP_TAG_INFO_SIZE);
    memcpy(out, t->uid, 7);
    out[10] = 7;
    out[0x20] = 1;
    out[0x24] = 2;
}
/* nn::nfp::ModelInfo (0x40 bytes), as the game reads it: the character's three bytes as the tag has them, the series,
   the figure's number as a 16-bit number of this processor (the tag has its high byte first), the kind of figure */
static inline void amiibo_model_info(const AmiiboTag *t, uint8_t out[NFP_MODEL_INFO_SIZE]) {
    memset(out, 0, NFP_MODEL_INFO_SIZE);
    memcpy(out, t->model, 3);
    out[3] = t->model[6];
    out[4] = t->model[5];
    out[5] = t->model[4];
    out[6] = t->model[3];
}

/* ---- the readers ----
   Two are listed once a figure was chosen (never before: the game is then told what it was always told, no reader): the
   first controller's and the console's own, since the game asks for one or the other by how it is played. A reader's
   state is nn::nfp::DeviceState's: 0 just initialized, 1 looking for a tag, 2 a tag found, 3 the tag taken away,
   4 the tag mounted. The figure chosen is put on the reader that looks for one first, stays there while the game reads it,
   and is lifted off NFP_HOLD_NS after the game read its ModelInfo (the game waits for that before it goes on), or when
   the game stops looking. One choice is one touch. */
enum { NFP_INIT = 0, NFP_SEARCH, NFP_ACTIVE, NFP_DEACTIVE, NFP_MOUNT };
enum { NFP_READERS = 2 };
#define NFP_HANDLE_BASE 0x7466000000000000ULL   /* (a DeviceHandle of ours: this, and the controller's number) */
#define NFP_HOLD_NS 1000000000LL
/* nn::Result 2115-0073, "the reader is not in the state for that": the game's task ends as for a reader that went away */
#define NFP_WRONG_STATE 0x9273u
/* what the Scan amiibo row is told (HOS_OPT_AMIIBO_SAID) */
enum { NFP_SAID_NONE = 0, NFP_SAID_WAITING, NFP_SAID_ON_READER, NFP_SAID_READ, NFP_SAID_REFUSED, NFP_SAID_NO_FILE };

typedef struct Nfp {
    int listed;                 /* the readers are shown to the game (from the first figure chosen, for the rest of the run) */
    int waiting;                /* a figure waits for a reader that looks for one */
    int holder;                 /* the reader the figure is on, -1 none */
    int64_t read_at;            /* when the game read the ModelInfo of the figure on the reader, 0 not yet */
    int said;                   /* NFP_SAID_* */
    int state[NFP_READERS];
    AmiiboTag tag;
} Nfp;

static inline void nfp_reset(Nfp *n) {
    memset(n, 0, sizeof *n);
    n->holder = -1;
}
static inline uint32_t nfp_reader_npad(int reader) { return reader ? 0x20u : 0u; }   /* (No. 1, and handheld) */
static inline uint64_t nfp_handle(int reader) { return NFP_HANDLE_BASE | nfp_reader_npad(reader); }
/* the reader a DeviceHandle names, -1 when it is not one of ours (or none is listed) */
static inline int nfp_reader(const Nfp *n, uint64_t handle) {
    if (!n->listed) return -1;
    for (int i = 0; i < NFP_READERS; i++)
        if (handle == nfp_handle(i)) return i;
    return -1;
}
/* the figure on a reader is lifted off: that reader says so until the game stops or starts detection */
static inline void nfp_lift(Nfp *n, int said) {
    if (n->holder >= 0) n->state[n->holder] = NFP_DEACTIVE;
    n->holder = -1;
    n->read_at = 0;
    n->said = said;
}
/* A figure chosen: 1 when the readers were not listed before (the game is then told the readers changed). A figure
   still on a reader is lifted off first */
static inline int nfp_choose(Nfp *n, const AmiiboTag *tag) {
    const int was = n->listed;
    if (n->holder >= 0) nfp_lift(n, NFP_SAID_NONE);
    n->tag = *tag;
    n->listed = 1, n->waiting = 1;
    n->said = NFP_SAID_WAITING;
    return !was;
}
/* No figure any more: the one waiting is forgotten, the one on a reader lifted off. The readers stay listed */
static inline void nfp_take_away(Nfp *n) {
    n->waiting = 0;
    if (n->holder >= 0) nfp_lift(n, NFP_SAID_NONE);
    n->said = NFP_SAID_NONE;
}
/* nn::nfp::Initialize and Finalize: every reader as new, a figure on one lifted off; what was chosen still waits */
static inline void nfp_restart(Nfp *n) {
    if (n->holder >= 0) nfp_lift(n, n->read_at ? NFP_SAID_READ : NFP_SAID_NONE);
    for (int i = 0; i < NFP_READERS; i++) n->state[i] = NFP_INIT;
}
/* ListDevices: the handles (at most max of them), their number; 0 while none is listed */
static inline int nfp_list(const Nfp *n, uint64_t *handles, int max) {
    int count = 0;
    for (int i = 0; n->listed && i < NFP_READERS && count < max; i++) handles[count++] = nfp_handle(i);
    return count;
}
/* time passing on a reader: the figure that waits goes onto a reader that looks for one; the one the game has read is
   lifted off NFP_HOLD_NS later */
static inline void nfp_step(Nfp *n, int reader, int64_t now) {
    if (n->state[reader] == NFP_SEARCH && n->waiting && n->holder < 0) {
        n->waiting = 0, n->holder = reader, n->read_at = 0;
        n->state[reader] = NFP_ACTIVE;
        n->said = NFP_SAID_ON_READER;
    }
    if (n->holder == reader && n->read_at && now - n->read_at >= NFP_HOLD_NS) nfp_lift(n, NFP_SAID_READ);
}
static inline int nfp_state(Nfp *n, int reader, int64_t now) {
    nfp_step(n, reader, now);
    return n->state[reader];
}
/* StartDetection: a reader that is not looking yet looks for a tag */
static inline uint32_t nfp_start(Nfp *n, int reader, int64_t now) {
    if (n->state[reader] != NFP_INIT && n->state[reader] != NFP_DEACTIVE) return NFP_WRONG_STATE;
    n->state[reader] = NFP_SEARCH;
    nfp_step(n, reader, now);
    return 0;
}
/* StopDetection: the reader as new again. A figure on it is lifted off: that was its touch, read or not */
static inline uint32_t nfp_stop(Nfp *n, int reader) {
    if (n->holder == reader) nfp_lift(n, n->read_at ? NFP_SAID_READ : NFP_SAID_NONE);
    n->state[reader] = NFP_INIT;
    return 0;
}
/* GetTagInfo: of the figure on the reader */
static inline uint32_t nfp_tag_info(Nfp *n, int reader, int64_t now, uint8_t out[NFP_TAG_INFO_SIZE]) {
    nfp_step(n, reader, now);
    if (n->holder != reader || (n->state[reader] != NFP_ACTIVE && n->state[reader] != NFP_MOUNT)) return NFP_WRONG_STATE;
    amiibo_tag_info(&n->tag, out);
    return 0;
}
/* Mount: the tag found is mounted (whatever part of it was asked for: only the part in the clear is ever answered) */
static inline uint32_t nfp_mount(Nfp *n, int reader, int64_t now) {
    nfp_step(n, reader, now);
    if (n->holder != reader || (n->state[reader] != NFP_ACTIVE && n->state[reader] != NFP_MOUNT)) return NFP_WRONG_STATE;
    n->state[reader] = NFP_MOUNT;
    return 0;
}
/* GetModelInfo: of the mounted figure. From then on it is lifted off after NFP_HOLD_NS */
static inline uint32_t nfp_model_info(Nfp *n, int reader, int64_t now, uint8_t out[NFP_MODEL_INFO_SIZE]) {
    nfp_step(n, reader, now);
    if (n->holder != reader || n->state[reader] != NFP_MOUNT) return NFP_WRONG_STATE;
    amiibo_model_info(&n->tag, out);
    if (!n->read_at) n->read_at = now ? now : 1;
    return 0;
}

/* A dump's file name as the Scan amiibo page sends it: a name of the amiibo folder itself, never a path */
static inline int amiibo_name_ok(const char *name) {
    const size_t len = strlen(name);
    if (!len || len > 200 || name[0] == '.' || name[0] == ' ') return 0;
    for (size_t i = 0; i < len; i++) {
        const unsigned char ch = (unsigned char)name[i];
        if (ch < 0x20 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
            return 0;
    }
    return !strstr(name, "..");
}
