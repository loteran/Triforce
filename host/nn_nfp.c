/* (Triforce) nn::nfp: an amiibo from a file.

   The game's amiibo sequence asks its NfpMgr for a reader and waits while there is none; with one, it starts detection,
   reads the figure's TagInfo and ModelInfo and waits for the figure to be lifted off (host/amiibo.h says what it reads
   of them, and has the readers' states). Until now every nn::nfp function was the lift's stub, which answers 0 and
   writes nothing: no reader, so the sequence waited for ever.

   The player puts dumps of their own figures into a folder "amiibo" in the game folder and chooses one on the Debug
   tab's Scan amiibo page (gpu/vk/vk_menu.cpp). The page sends the file's name (HOS_OPT_AMIIBO_ASK, a byte a read, a line
   feed at its end; a line of its own for "none"), read here at the game's pad read. From the first figure chosen on, two
   readers are listed and the game is told that the readers changed (the event of AttachAvailabilityChangeEvent, which
   its NfpMgr thread asks about while it has none); the figure goes onto the reader the game looks for one with. Until
   a figure is chosen every function here answers as the stub did, so a player who chooses none gets the game as it was.
   The page is told what became of the figure (HOS_OPT_AMIIBO_SAID, NFP_SAID_*).

   Only what a tag keeps in the clear is read from a dump (the serial number and which figure it is), so no key is
   needed, none is looked for and none is read. The game's own code asks for nothing else (host/amiibo.h); the SDK's
   functions for a figure's owner, nickname and saved data stay the stubs they were. */
#include "hos.h"
#include "amiibo.h"
#include "../gpu/vk/vk_api.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>   /* (SRWLOCK; linux: host/posix's) */

int rx_option(int id, int otherwise);            /* rx_backend.c */
void rx_option_set(int id, int value);
void hos_system_event_signal(uint64_t event);    /* nn_os_sync.c: by the event's address in the game */
int hos_share_game_dir(char *out, size_t cap);   /* save_share.c: the game folder, no separator at its end */

/* (what the page is told is what host/amiibo.h says became of the figure) */
_Static_assert(NFP_SAID_NONE == HOS_AMIIBO_NONE && NFP_SAID_WAITING == HOS_AMIIBO_WAITING && NFP_SAID_ON_READER == HOS_AMIIBO_ON_READER &&
               NFP_SAID_READ == HOS_AMIIBO_READ && NFP_SAID_REFUSED == HOS_AMIIBO_REFUSED && NFP_SAID_NO_FILE == HOS_AMIIBO_NO_FILE,
               "HOS_AMIIBO_* (gpu/vk/vk_api.h) and NFP_SAID_* (host/amiibo.h)");

static SRWLOCK nfp_lock = SRWLOCK_INIT;
static Nfp nfp = {.holder = -1};
static uint64_t availability_event;   /* the SystemEventType the game gave AttachAvailabilityChangeEvent, 0 none */

/* the reader a call names (its DeviceHandle is the argument's first eight bytes), -1 when it is none of ours: the
   caller then answers as the stub did */
static int reader_of(uint64_t handle_ptr) { return handle_ptr ? nfp_reader(&nfp, G64(handle_ptr)) : -1; }

HLE(_ZN2nn3nfp10InitializeEv) {
    AcquireSRWLockExclusive(&nfp_lock);
    nfp_restart(&nfp);
    ReleaseSRWLockExclusive(&nfp_lock);
    RET(0);
}
HLE(_ZN2nn3nfp8FinalizeEv) {
    AcquireSRWLockExclusive(&nfp_lock);
    nfp_restart(&nfp);
    availability_event = 0;   /* (the game's event goes with its NfpMgr: never signalled after this) */
    ReleaseSRWLockExclusive(&nfp_lock);
    RET(0);
}
/* AttachAvailabilityChangeEvent(os::SystemEventType*): only kept. The game clears and asks it as before (nn_os_sync.c
   makes an event of an address at its first use); it is signalled when the first figure is chosen */
HLE(_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE) {
    AcquireSRWLockExclusive(&nfp_lock);
    availability_event = ARG(0);
    ReleaseSRWLockExclusive(&nfp_lock);
    RET(0);
}
/* ListDevices(DeviceHandle* out, int* count, int max): eight bytes a handle. Nothing written while none is listed */
HLE(_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii) {
    uint64_t handles[NFP_READERS];
    AcquireSRWLockExclusive(&nfp_lock);
    const int max = (int)(int32_t)ARG(2);
    const int n = ARG(0) && ARG(1) ? nfp_list(&nfp, handles, max < NFP_READERS ? max : NFP_READERS) : 0;
    ReleaseSRWLockExclusive(&nfp_lock);
    for (int i = 0; i < n; i++) G64(ARG(0) + 8ULL * i) = handles[i];
    if (n > 0) G32(ARG(1)) = (uint32_t)n;
    RET(0);
}
/* GetNpadId(unsigned* out, DeviceHandle const&) */
HLE(_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE) {
    AcquireSRWLockExclusive(&nfp_lock);
    const int reader = reader_of(ARG(1));
    ReleaseSRWLockExclusive(&nfp_lock);
    if (reader >= 0 && ARG(0)) G32(ARG(0)) = nfp_reader_npad(reader);
    RET(0);
}
/* GetDeviceState(DeviceHandle const&) -> DeviceState */
HLE(_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE) {
    AcquireSRWLockExclusive(&nfp_lock);
    const int reader = reader_of(ARG(0));
    const int state = reader >= 0 ? nfp_state(&nfp, reader, hos_now_ns()) : 0;
    ReleaseSRWLockExclusive(&nfp_lock);
    RET(state);
}
HLE(_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE) {
    AcquireSRWLockExclusive(&nfp_lock);
    const int reader = reader_of(ARG(0));
    const uint32_t result = reader >= 0 ? nfp_start(&nfp, reader, hos_now_ns()) : 0;
    ReleaseSRWLockExclusive(&nfp_lock);
    RET(result);
}
HLE(_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE) {
    AcquireSRWLockExclusive(&nfp_lock);
    const int reader = reader_of(ARG(0));
    const uint32_t result = reader >= 0 ? nfp_stop(&nfp, reader) : 0;
    ReleaseSRWLockExclusive(&nfp_lock);
    RET(result);
}
/* GetTagInfo(TagInfo* out, DeviceHandle const&) */
HLE(_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE) {
    uint8_t info[NFP_TAG_INFO_SIZE];
    AcquireSRWLockExclusive(&nfp_lock);
    const int reader = reader_of(ARG(1));
    const uint32_t result = reader >= 0 ? nfp_tag_info(&nfp, reader, hos_now_ns(), info) : 0;
    ReleaseSRWLockExclusive(&nfp_lock);
    if (reader >= 0 && !result && ARG(0)) memcpy(GP(ARG(0)), info, sizeof info);
    RET(result);
}
/* Mount(DeviceHandle const&, ModelType, MountTarget) */
HLE(_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE) {
    AcquireSRWLockExclusive(&nfp_lock);
    const int reader = reader_of(ARG(0));
    const uint32_t result = reader >= 0 ? nfp_mount(&nfp, reader, hos_now_ns()) : 0;
    ReleaseSRWLockExclusive(&nfp_lock);
    RET(result);
}
/* GetModelInfo(ModelInfo* out, DeviceHandle const&) */
HLE(_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE) {
    uint8_t info[NFP_MODEL_INFO_SIZE];
    AcquireSRWLockExclusive(&nfp_lock);
    const int reader = reader_of(ARG(1));
    const uint32_t result = reader >= 0 ? nfp_model_info(&nfp, reader, hos_now_ns(), info) : 0;
    ReleaseSRWLockExclusive(&nfp_lock);
    if (reader >= 0 && !result && ARG(0)) memcpy(GP(ARG(0)), info, sizeof info);
    if (reader >= 0 && !result) hos_log("nfp: the game read the figure's ModelInfo");
    RET(result);
}

/* The dump "name" of the game folder's amiibo folder: NFP_SAID_WAITING when it is a figure's (*tag filled), else
   NFP_SAID_NO_FILE or NFP_SAID_REFUSED. At most AMIIBO_FILE_MAX bytes of it are read */
static int load_dump(const char *name, AmiiboTag *tag) {
    char dir[600], path[900];
    uint8_t data[AMIIBO_FILE_MAX + 1];
    if (!amiibo_name_ok(name) || !hos_share_game_dir(dir, sizeof dir)) return NFP_SAID_NO_FILE;
    snprintf(path, sizeof path, "%s/amiibo/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NFP_SAID_NO_FILE;
    const size_t n = fread(data, 1, sizeof data, f);   /* (one byte more than a dump may have: a longer file is refused) */
    fclose(f);
    const int why = amiibo_parse(data, n, tag);
    if (why != AMIIBO_OK) {
        hos_log("nfp: amiibo/%s is not taken: %s", name, why == AMIIBO_BAD_SIZE ? "not the size of a figure's dump (532, 540 or 572 bytes)"
                                                                                   : "it does not hold a figure's serial number and number");
        return NFP_SAID_REFUSED;
    }
    return NFP_SAID_WAITING;
}

/* The Scan amiibo page's choice, taken at the game's pad read (host/nn_hid.c; every 50 ms at most): a file's name, or
   an empty line for none. And the page told what became of the figure when that changed */
void hos_amiibo_poll(void) {
    static int64_t next;
    static int told = -1;
    static char line[256];
    static size_t used;
    static SRWLOCK one = SRWLOCK_INIT;   /* (the pad is read by more than one of the game's threads) */
    const int64_t now = hos_now_ns();
    if (now < __atomic_load_n(&next, __ATOMIC_RELAXED) || !TryAcquireSRWLockExclusive(&one)) return;
    __atomic_store_n(&next, now + 50000000LL, __ATOMIC_RELAXED);
    int byte;
    while ((byte = rx_option(HOS_OPT_AMIIBO_ASK, 0)) > 0) {
        if (byte != '\n') {
            if (used + 1 < sizeof line) line[used++] = (char)byte;
            continue;
        }
        line[used] = 0, used = 0;
        AmiiboTag tag;
        const int loaded = line[0] ? load_dump(line, &tag) : NFP_SAID_NONE;
        uint64_t event = 0;
        AcquireSRWLockExclusive(&nfp_lock);
        if (loaded == NFP_SAID_WAITING) {
            if (nfp_choose(&nfp, &tag)) event = availability_event;
        } else {
            nfp_take_away(&nfp);
            nfp.said = loaded;
        }
        ReleaseSRWLockExclusive(&nfp_lock);
        if (loaded == NFP_SAID_WAITING)
            hos_log("nfp: amiibo/%s chosen (%s dump): on the reader when the game looks for a figure%s", line,
                    tag.decrypted ? "a decrypted" : "a tag's", event ? "; the game is told a reader is there now" : "");
        if (event) hos_system_event_signal(event);
    }
    AcquireSRWLockExclusive(&nfp_lock);
    for (int i = 0; nfp.listed && i < NFP_READERS; i++) nfp_step(&nfp, i, now);   /* (a figure lifted off when its time is up) */
    const int said = nfp.said;
    ReleaseSRWLockExclusive(&nfp_lock);
    if (said != told) rx_option_set(HOS_OPT_AMIIBO_SAID, told = said);
    ReleaseSRWLockExclusive(&one);
}
