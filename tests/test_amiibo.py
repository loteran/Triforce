"""An amiibo from a file (host/amiibo.h, host/nn_nfp.c; no game, no figure's data, no key).

The game asks nn::nfp for a reader, a tag on it, the tag's TagInfo and ModelInfo, and waits for the figure to be lifted
off. A player chooses a dump of one of their own figures on the Debug tab's Scan amiibo page; until one is chosen the
game is told what it always was (no reader).

Compiles and runs tests/amiibo_test.c: the dump parser on bytes of the test's own making (a tag's layout and a decrypted
one, the three sizes, every other size and every wrong mark refused with nothing written, and the data ending at a page
that cannot be read, so a read past its end stops the test); the TagInfo and ModelInfo bytes where the game's code reads
them; the file names taken; the readers' states (none before a figure is chosen, the figure found by the reader that
looks, mounted, read, lifted off a second later, one touch a choice, taken away); and the game's own task, as its code
has it, run against them: it waits in the search state with no figure and ends with the figure's data with one.

Reads the source for what cannot be run without the game: every SDK function answers as the lift's stub did (0, nothing
written) for a handle that is not a listed reader's; the file is opened by a checked name inside the amiibo folder and
at most 1025 bytes of it are read; nothing names or opens a key; the choice is taken at the pad read; the option ids.

    python -I -B tests/test_amiibo.py [--cc gcc]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

# the eleven functions the game's own request makes (its task at 0x83b84230 and the NfpMgr around it)
FUNCTIONS = (
    "_ZN2nn3nfp10InitializeEv", "_ZN2nn3nfp8FinalizeEv", "_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE",
    "_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", "_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE",
    "_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", "_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE",
    "_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", "_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE",
    "_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE",
    "_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE")


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def body(text, name):
    start = text.index("HLE(%s) {" % name)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source():
    nfp = (ROOT / "host/nn_nfp.c").read_text(encoding="utf-8")
    header = (ROOT / "host/amiibo.h").read_text(encoding="utf-8")
    hid = (ROOT / "host/nn_hid.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    sys.path.insert(0, str(ROOT / "nx2"))
    import standalone
    have = standalone.implemented()
    for name in FUNCTIONS:
        check("imp_" + name in have, "host/ has no " + name)
    code = nfp[nfp.index("#include"):]
    check(len(re.findall(r"^HLE\(", code, re.M)) == len(FUNCTIONS), "host/nn_nfp.c has other SDK functions than the game's request makes")
    # a handle that is not a listed reader's: 0 and nothing written, as the lift's stub answers
    for name in FUNCTIONS[4:]:
        text = body(nfp, name)
        check("reader_of(ARG(" in text, name + " does not ask whose handle it is")
        check(re.search(r"reader >= 0 \? nfp_\w+\(&nfp, reader[^;]*\) : 0;", text) or "if (reader >= 0 && ARG(0)) G32(ARG(0))" in text,
              name + " does not answer 0 for a handle that is not a reader's")
        for write in re.findall(r"^[^\n]*(?:memcpy\(GP|G32\(ARG|G64\(ARG)[^\n]*$", text, re.M):
            check("reader >= 0" in write, name + " writes for a handle that is not a reader's: " + write.strip())
    listing = body(nfp, FUNCTIONS[3])
    check("nfp_list(&nfp, handles, max < NFP_READERS ? max : NFP_READERS)" in listing and "if (n > 0) G32(ARG(1)) = (uint32_t)n;" in listing
          and "for (int i = 0; i < n; i++) G64(ARG(0) + 8ULL * i) = handles[i];" in listing,
          "ListDevices writes a count or a handle while no reader is listed, or more than the game has room for")
    check("if (!n->listed) return -1;" in header and "n->listed && i < NFP_READERS && count < max" in header,
          "host/amiibo.h takes a handle or lists a reader before a figure was chosen")
    # the file: a checked name inside the amiibo folder, a bounded read, and no key anywhere
    load = nfp[nfp.index("static int load_dump("):nfp.index("void hos_amiibo_poll(")]
    check("if (!amiibo_name_ok(name) || !hos_share_game_dir(dir, sizeof dir)) return NFP_SAID_NO_FILE;" in load
          and 'snprintf(path, sizeof path, "%s/amiibo/%s", dir, name);' in load, "the dump is not opened by a checked name inside the amiibo folder")
    check("uint8_t data[AMIIBO_FILE_MAX + 1];" in load and "fread(data, 1, sizeof data, f)" in load and "amiibo_parse(data, n, tag)" in load,
          "the dump's read is not bounded")
    check(nfp.count("fopen(") == 1 and "fwrite" not in nfp and "fopen" not in header, "host/nn_nfp.c opens another file than the dump, or writes one")
    for text, where in ((code, "host/nn_nfp.c"), (header[header.index("#pragma once"):], "host/amiibo.h")):
        stripped = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        check(not re.search(r"key|retail|hmac|aes|decrypt\w*\(", stripped, re.I), where + " has code that names a key or decrypts")
    # the choice is taken at the pad read, the event signalled only when the readers appear
    states = hid[hid.index("static void npad_states("):]
    check("hos_amiibo_poll();" in states[:states.index("if (connected) {")], "host/nn_hid.c does not take the page's choice at the pad read")
    poll = nfp[nfp.index("void hos_amiibo_poll("):]
    check("rx_option(HOS_OPT_AMIIBO_ASK, 0)" in poll and "rx_option_set(HOS_OPT_AMIIBO_SAID, told = said)" in poll, "the page's line is not read, or the page not told")
    check("if (nfp_choose(&nfp, &tag)) event = availability_event;" in poll and "if (event) hos_system_event_signal(event);" in poll,
          "the game is not told that the readers appeared, or is told at every figure")
    check("availability_event = 0;" in body(nfp, FUNCTIONS[1]), "the game's event is kept after Finalize")
    check("enum { HOS_OPT_DEBUG_ASK = 115, HOS_OPT_DEBUG_TEXT, HOS_OPT_DEBUG_SAID, HOS_OPT_AMIIBO_ASK, HOS_OPT_AMIIBO_SAID };" in api,
          "gpu/vk/vk_api.h: the option ids 115 to 119")
    print("PASS: the SDK's functions answer as before until a figure is chosen, and no key is read")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    check_source()
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "amiibo_test.exe"
        subprocess.run([a.cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/amiibo_test.c"), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode or run.stdout.strip() != "ok":
            raise SystemExit("FAIL: tests/amiibo_test.c (exit %d)\n%s%s" % (run.returncode, run.stdout, run.stderr))
    print("PASS: the dump parser, the infos, the names, the readers' states and the game's task against them")


if __name__ == "__main__":
    main()
