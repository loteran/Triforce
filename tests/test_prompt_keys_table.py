"""The keyboard and mouse face of the button prompts: its table of keys, and the row that chooses it.

Game-free. The table (nx2/prompt_keys.py) is checked against the sources it follows:
- KEYS has an icon for every key the Controls page can bind (gpu/vk/vk_menu.cpp bindable_keys) and for no other, each with
  a label of its own, a one-character label the character the page shows for that key (key_name);
- DEFAULT is the first key of every control out of the box (host/nn_hid.c key_default), and every key of that table has an
  icon;
- the controls the icons of several keys and the left stick's directions follow are the ones gpu/vk/vk_api.h numbers
  that way, and host/prompts.c points the same code points at the movement keys (cmap4_bind's moves);
- no code point of the font is given two meanings.
The row: Auto's choice (host/prompts.c auto_face, compiled and run): the first controller's face from its name, the
Xbox's for XInput's nameless one, with no controller the device used last in the run before (prompts_auto.txt), else the
keyboard and mouse; the row's names end with "Keyboard & mouse", its default is still Auto, and the documents name the new
choice.
(Triforce, 11-19) Following the device used last (host/prompts.c device_face, follow_record and pad_font_due, compiled
and run): which face a device is, when prompts_auto.txt gets it (3 s of a controller, 30 s of the keys used in 5 s of
them: a key pressed once and the game left alone is not), and when the overlay shows the font of the controller used last
(at once the first, another face once it has been the last for a second: two controllers given the same presses do not
make it flicker); and the sources that feed and read them: host/nn_hid.c's device used last and its uses, the captured
mouse only past an eighth of the stick, host/pad_sdl.c's scans and the controller used last's name, Auto's wait by the
scans, Auto's memory in the user's folder (Setup makes prompts/ again at an update), a font a face built once a run and
tried again when not written, the host interface's prompt_pad_face and the overlay's font of it (gpu/vk/vk_imgui.cpp:
one a face, made once), and pad_name's names of it.

    python -I -B tests/test_prompt_keys_table.py [--cc gcc]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import prompt_assets  # noqa: E402
import prompt_faces  # noqa: E402
import prompt_keys  # noqa: E402

# Windows' key codes, by the names the sources use
VK = {"VK_LBUTTON": 1, "VK_RBUTTON": 2, "VK_MBUTTON": 4, "VK_BACK": 8, "VK_TAB": 9, "VK_RETURN": 13, "VK_SHIFT": 16,
      "VK_CONTROL": 17, "VK_MENU": 18, "VK_CAPITAL": 20, "VK_SPACE": 32, "VK_PRIOR": 33, "VK_NEXT": 34, "VK_END": 35,
      "VK_HOME": 36, "VK_LEFT": 37, "VK_UP": 38, "VK_RIGHT": 39, "VK_DOWN": 40, "VK_NUMPAD0": 0x60, "VK_MULTIPLY": 0x6A,
      "VK_ADD": 0x6B, "VK_SUBTRACT": 0x6D, "VK_DECIMAL": 0x6E, "VK_DIVIDE": 0x6F, "VK_F1": 0x70, "VK_F2": 0x71,
      "VK_F12": 0x7B, "VK_OEM_1": 0xBA, "VK_OEM_PLUS": 0xBB, "VK_OEM_COMMA": 0xBC, "VK_OEM_MINUS": 0xBD,
      "VK_OEM_PERIOD": 0xBE, "VK_OEM_2": 0xBF, "VK_OEM_3": 0xC0, "VK_OEM_4": 0xDB, "VK_OEM_5": 0xDC, "VK_OEM_6": 0xDD,
      "VK_OEM_7": 0xDE}


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def code(token):
    """a key code as the sources write one: 'A', 0x05, 65, VK_SPACE, VK_NUMPAD0 + 9"""
    total = 0
    for part in token.split("+"):
        part = part.strip()
        if re.fullmatch(r"'.'", part):
            total += ord(part[1])
        elif part in VK:
            total += VK[part]
        else:
            total += int(part, 0)
    return total


def bindable(menu):
    body = block(menu, "const std::vector<int>& bindable_keys()")
    keys = []
    for lo, hi in re.findall(r"for \(int c = ([^;]+); c <= ([^;]+); c\+\+\)", body):
        keys += range(code(lo), code(hi) + 1)
    listed = re.search(r"for \(int c : \{([^}]*)\}\)", body)
    check(listed is not None, "gpu/vk/vk_menu.cpp bindable_keys has no list of keys")
    keys += [code(t) for t in listed.group(1).split(",") if t.strip()]
    check(len(keys) == len(set(keys)) and len(keys) > 80, "bindable_keys was not read whole (%d keys)" % len(keys))
    return keys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    hid = (ROOT / "host/nn_hid.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    prompts = (ROOT / "host/prompts.c").read_text(encoding="utf-8")
    K = prompt_keys

    # every key that can be bound has an icon, and nothing else has one
    keys = bindable(menu)
    check(sorted(keys) == sorted(K.KEYS), "the icons' keys are not the Controls page's: without an icon %s, never bound %s"
          % (sorted(set(keys) - set(K.KEYS)), sorted(set(K.KEYS) - set(keys))))
    check(0 not in K.KEYS, "key code 0 is the blank cap's, no key's")
    labels = list(K.KEYS.values())
    check(len(set(labels)) == len(labels) and all(labels), "two keys share a label, or one has none")
    name = block(menu, "const char* key_name(int vk)")
    for vk_name, said in re.findall(r'case (\w+): return "((?:[^"\\]|\\.)*)";', name):
        said = said.encode().decode("unicode_escape")
        if vk_name in VK and len(said) == 1:
            check(K.KEYS[VK[vk_name]] == said, "%s: the page says %r, the icon %r" % (vk_name, said, K.KEYS[VK[vk_name]]))
    check(K.MICE == {1, 2, 4, 5, 6}, "the mouse's buttons are not the five the page binds")

    # the controls out of the box
    table = re.search(r"key_default\[HOS_KEY_N\]\[2\] = \{(.*?)\};", hid, re.S)
    check(table is not None, "host/nn_hid.c has no key_default")
    rows = [[code(t) for t in row.split(",") if t.strip()] for row in re.findall(r"\{([^{}]*)\}", table.group(1))]
    check([r[0] for r in rows] == K.DEFAULT, "DEFAULT is not key_default's first keys: %s" % [r[0] for r in rows])
    check(all(k in K.KEYS for r in rows for k in r), "a key of key_default has no icon")

    # the controls' numbers
    def enum(first):
        m = re.search(r"enum \{ (%s[^}]*)\}" % first, api)
        check(m is not None, "gpu/vk/vk_api.h has no enum starting %s" % first)
        return [re.sub(r"\s*=.*", "", e.strip()) for e in m.group(1).split(",") if e.strip()]
    sw, key = enum("HOS_SW_A"), enum("HOS_KEY_MOVE_UP")
    check(len(sw) - 1 == len(prompt_assets.SWITCH) and len(sw) - 1 + len(key) - 1 == len(K.DEFAULT),
          "the controls are not %d buttons and %d keys' places" % (len(prompt_assets.SWITCH), len(K.DEFAULT) - len(sw) + 1))
    number = {n: i for i, n in enumerate(sw[:-1] + key[:-1])}
    for mine, theirs in ((K.MOVE_UP, "HOS_KEY_MOVE_UP"), (K.MOVE_DOWN, "HOS_KEY_MOVE_DOWN"), (K.MOVE_LEFT, "HOS_KEY_MOVE_LEFT"),
                         (K.MOVE_RIGHT, "HOS_KEY_MOVE_RIGHT"), (K.PAD_LEFT, "HOS_SW_LEFT"), (K.PAD_UP, "HOS_SW_UP"),
                         (K.PAD_RIGHT, "HOS_SW_RIGHT"), (K.PAD_DOWN, "HOS_SW_DOWN")):
        check(mine == number[theirs], "%s is %d, not %d" % (theirs, number[theirs], mine))
    moves = re.search(r"moves\[4\] = \{(.*?)\};", prompts, re.S)
    check(moves is not None, "host/prompts.c cmap4_bind has no moves")
    host = {int(c, 16): number[n] for c, n in re.findall(r"\{(0x[0-9A-Fa-f]+), (HOS_KEY_\w+)\}", moves.group(1))}
    check(host == K.MOVES, "host/prompts.c points %s at the movement keys, the font has %s" % (host, K.MOVES))
    check(all(c in number.values() for cells in K.CLUSTERS.values() for c, _, _ in cells), "an icon of several keys has no control")
    for cells in K.CLUSTERS.values():
        check(len({(col, row) for _, col, row in cells}) == len(cells), "two keys of an icon share a place")

    # one meaning a code point
    groups = [set(prompt_assets.SWITCH), set(K.MOVES), set(K.CLUSTERS), set(K.CAMERA)]
    check(sum(len(g) for g in groups) == len(set().union(*groups)), "a code point of the font has two meanings")
    base = prompt_assets.PAD_BASE
    check(all(not base <= c < base + 0x200 for g in groups for c in g) and base + 0x200 <= 0xF900,
          "the keys' icons (U+%04X on) run into the game's" % base)
    check(prompt_faces.FACES.get("keyboard", {}).get("keys") is True and list(prompt_faces.FACES)[-1] == "keyboard",
          "nx2/prompt_faces.py: the keyboard face is not the last one, or not the keys'")

    # the row
    names = re.search(r"const char\* tx_prompts\(int v\) \{\s*static const char\* t\[HOS_PROMPT_N \+ 1\] = \{([^}]*)\}", menu)
    check(names is not None, "gpu/vk/vk_menu.cpp has no tx_prompts")
    names = re.findall(r'"([^"]*)"', names.group(1))
    faces = enum("HOS_PROMPT_SWITCH")
    check(names[0] == "Auto" and names[-1] == "Keyboard & mouse" and len(names) == len(faces),
          "the row's names %s are not Auto and a name per face (%d), the keyboard's last" % (names, len(faces) - 1))
    check(faces[-2:] == ["HOS_PROMPT_KEYBOARD", "HOS_PROMPT_N"], "the keyboard is not the last face: %s" % faces)
    check(re.search(r'\{TAB_PAGE, "Button prompts", &opts\.prompts, 0, HOS_PROMPT_N, 1, tx_prompts,', menu) is not None,
          "the Button prompts row does not reach HOS_PROMPT_N")
    check(re.search(r"int prompts = 0;", (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")) is not None,
          "the row's default is no longer Auto")
    check("if (choice >= 1 && choice <= HOS_PROMPT_N) return choice - 1;" in prompts, "a face chosen by the row is not taken as it is")
    # (a controller's name on the Controls page: no row of names for the keyboard, which is no controller)
    check("t[face >= 0 && face < HOS_PROMPT_KEYBOARD ? face + 1 : 0][p]" in menu, "pad_name reads a row the table does not have")
    # (the menu's own strip shows the icon font while a controller is its last input: never the keys' font)
    check("host->prompt_face() != HOS_PROMPT_KEYBOARD" in (ROOT / "gpu/vk/vk_imgui.cpp").read_text(encoding="utf-8"),
          "the menu would show the keys' icons for a controller's buttons")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    row = re.search(r"^\| Button prompts \|(.*)\|$", readme, re.M)
    check(row is not None and all(n in row.group(1) for n in names), "readme.md's settings table does not list the row's choices")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check(re.search(r"^\| `button_prompts` \|.*6 Keyboard & mouse", dev, re.M) is not None,
          "docs/development.md's options table has no button_prompts with 6 Keyboard & mouse")

    # Auto's choice, and following the device used last
    harness = "\n".join([
        "#include <ctype.h>", "#include <stdint.h>", "#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>",
        '#include "gpu/vk/vk_api.h"',
        block(prompts, "static int contains("),
        block(prompts, "static int auto_face("),
        block(prompts, "static int device_face("),
        re.search(r"typedef struct \{ int held, recorded; int64_t held_ns; int active; \} Follow;", prompts).group(0),
        re.search(r"#define KEYS_ACTIVE \d+", prompts).group(0),
        block(prompts, "static int follow_record("),
        re.search(r"#define PAD_HOLD_NS \d+LL", prompts).group(0),
        block(prompts, "static int pad_font_due("),
        "int main(int argc, char **argv) {",
        '    if (!strcmp(argv[1], "auto")) printf("%d\\n", auto_face(argv[2], atoi(argv[3]), atoi(argv[4])));',
        '    else if (!strcmp(argv[1], "device")) printf("%d\\n", device_face(atoi(argv[2]), argv[3]));',
        '    else if (!strcmp(argv[1], "due"))',
        '        printf("%d\\n", pad_font_due(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoll(argv[5]) * 1000000LL,',
        '                                     (unsigned)atoi(argv[6])));',
        "    else {   /* follow: what prompts_auto.txt had, then a face, its time in ms and whether its device was used, each */",
        "        Follow f = {-1, atoi(argv[2]), 0, 0};",
        "        for (int i = 3; i + 2 < argc; i += 3)",
        '            if (follow_record(&f, atoi(argv[i]), atoll(argv[i + 1]) * 1000000LL, atoi(argv[i + 2])))',
        '                printf("%s %d\\n", argv[i + 1], f.recorded);',
        "    }",
        "    return 0;",
        "}", ""])
    F = {n[len("HOS_PROMPT_"):]: i for i, n in enumerate(faces)}
    want = [("", 0, -1, "KEYBOARD"), ("", 1, -1, "XBOX"), ("Xbox Series X Controller", 0, -1, "XBOX"),
            ("8BitDo Pro 2", 0, -1, "XBOX"), ("DualSense Wireless Controller", 0, -1, "PS5"), ("PS4 Controller", 0, -1, "PS4"),
            ("Steam Deck", 0, -1, "STEAMDECK"), ("Nintendo Switch Pro Controller", 0, -1, "SWITCH"),
            ("Xbox 360 Controller", 1, -1, "XBOX"),
            # (none there: the last run's device; a controller there, or XInput's, before it; nothing sensible: the keys)
            ("", 0, F["XBOX"], "XBOX"), ("", 0, F["PS5"], "PS5"), ("", 0, F["KEYBOARD"], "KEYBOARD"), ("", 1, F["PS5"], "XBOX"),
            ("DualSense Wireless Controller", 0, F["XBOX"], "PS5"), ("", 0, 99, "KEYBOARD")]
    devices = [(0, "", -1), (1, "Xbox Series X Controller", F["KEYBOARD"]), (2, "", F["XBOX"]),
               (2, "Xbox Wireless Controller", F["XBOX"]), (2, "DualSense Wireless Controller", F["PS5"]),
               (2, "Nintendo Switch Pro Controller", F["SWITCH"])]
    K_, X, P = F["KEYBOARD"], F["XBOX"], F["PS5"]
    keys_used = [x for t in range(0, 30001, 250) for x in (K_, t, 1)]   # (the keys used at every look for 30 s)
    follows = [
        # (a controller for 3 s is recorded, and once only)
        ([-1, X, 0, 1, X, 1000, 0, X, 2999, 0, X, 3000, 0, X, 9000, 0], ["3000 %d" % X]),
        # (the keys need 30 s: a key between a controller's presses, or one to close the game, keeps the controller's)
        ([X, K_, 0, 1, K_, 20000, 1, X, 21000, 1, K_, 22000, 1, K_, 51999, 1], []),
        ([X] + keys_used, ["30000 %d" % K_]),
        # ((11-19) and used in 5 s of them: a key pressed once, or the mouse bumped, then the game left alone is not the keys)
        ([X, K_, 0, 1, K_, 30000, 0, K_, 90000, 0], []),
        ([X, K_, 0, 1, K_, 250, 1, K_, 500, 1, K_, 40000, 0], []),
        ([X] + [x for t in range(0, 4751, 250) for x in (K_, t, 1)] + [K_, 30000, 0], ["30000 %d" % K_]),
        # (what the file already has is not written again; another controller after 3 s is)
        ([X, X, 0, 1, X, 60000, 0, P, 61000, 1, P, 64000, 0], ["64000 %d" % P]),
        # (no device yet: nothing)
        ([-1, -1, 0, 0, -1, 90000, 0], []),
    ]
    # (served, face, shown, held ms, failed): the first controller's font at once; another face's once it has been the one
    # used last for a second, not at each press of two controllers taking turns
    dues = [((K_, X, -1, 0, 0), 1), ((K_, X, X, 5000, 0), 0), ((K_, P, X, 0, 0), 0), ((K_, P, X, 999, 0), 0),
            ((K_, P, X, 1000, 0), 1), ((K_, K_, -1, 5000, 0), 0), ((K_, -1, -1, 0, 0), 0), ((X, P, -1, 0, 0), 0),
            ((K_, X, -1, 0, 1 << X), 0), ((F["SWITCH"], X, -1, 0, 0), 0)]
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "auto.c", Path(tmp) / "auto"
        c.write_text(harness, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), str(c), "-o", str(exe)], check=True)

        def run(*args):
            return subprocess.run([str(exe)] + [str(x) for x in args], check=True, capture_output=True, text=True).stdout

        for name, xinput, remembered, face in want:
            got = int(run("auto", name, xinput, remembered))
            check(faces[got] == "HOS_PROMPT_" + face, "Auto with controller %r%s, the last run's face %d: %s, not %s"
                  % (name, " and XInput's" if xinput else "", remembered, faces[got], face))
        for device, name, face in devices:
            got = int(run("device", device, name))
            check(got == face, "device %d (%r) is face %d, not %d" % (device, name, got, face))
        for seq, writes in follows:
            got = run("follow", *seq).split("\n")[:-1]
            check(got == writes, "following %s writes %s, not %s" % (seq, got, writes))
        for args, due in dues:
            got = int(run("due", *args))
            check(got == due, "the overlay's font for served %d, face %d, shown %d, held %d ms, failed %d: %d, not %d"
                  % (args + (got, due)))
    choose = block(prompts, "static int choose_face(")
    check("auto_face(name, xinput, remembered)" in choose and "!name[0] && hos_xinput_connected()" in choose and
          "name[0] || xinput ? -1 : remembered_face(dir)" in choose,
          "choose_face does not ask auto_face, asks XInput while SDL names a controller, or the last run's device before both")
    check("hos_sdl_pad_scans()" in choose and "scans < 0 || scans >= AUTO_SCANS" in choose and "i < 60" in choose,
          "Auto's wait is not SDL's scans, at most 3 s, and none without SDL")
    check("int hos_xinput_connected(void) {" in hid, "host/nn_hid.c has no hos_xinput_connected")
    # (the device used last: a controller where its presses count, the keys and the mouse where theirs do; stored only)
    reader = block(hid, "static void read_input(")
    check("player_used(), device_used(2);" in reader and "if (bound_down(i, 0)) *buttons |= 1ULL << i, player_used(), device_used(1);"
          in reader and "if (bound_down(i, 1)) *buttons |= 1ULL << i, device_used(1);" in reader and
          "if (mouse[2] > 4096 || mouse[2] < -4096 || mouse[3] > 4096 || mouse[3] < -4096) device_used(1);" in reader and "if (keys[0] || keys[1] || cx || cy) device_used(1);" in reader,
          "host/nn_hid.c read_input does not say which device was used last")
    check(not re.search(r"\b(fopen|hos_log|Sleep)\(", block(hid, "static void device_used(")),
          "device_used does more than store the device")
    check("InterlockedIncrement(&input_uses[device]);" in block(hid, "static void device_used(") and
          "unsigned hos_input_uses(int device) {" in hid, "host/nn_hid.c does not count each device's uses")
    pad = (ROOT / "host/pad_sdl.c").read_text(encoding="utf-8")
    check("void hos_sdl_pad_last_name(char *out, size_t cap) {" in pad and "pads[i].used_ns > pads[last].used_ns" in pad,
          "host/pad_sdl.c does not name the controller used last")
    check("InterlockedIncrement(&scans)" in pad and pad.count("InterlockedExchange(&scans, -1);") == 3 and
          "scans < SCANS_FAST ? 250000000LL : 1000000000LL" in pad,
          "host/pad_sdl.c does not count its scans, quickly at first, or say when SDL will not run")
    follow = block(prompts, "static DWORD WINAPI follow_main(void *arg) {")
    check("follow_main" in block(prompts, "static int setup(") and 'snprintf(file, sizeof file, "External.pad.%d.bfotf.zs", face);' in follow
          and "const int got = built >> face & 1 ? 1 : build_font(dir, face, file, 1, out, sizeof out);" in follow,
          "host/prompts.c does not follow the device used last, or build the overlay's font of it once a face")
    check("pad_font_due(served, face, (int)pad_face_shown, now - seen_ns, failed) && now >= retry_ns[face]" in follow and
          "else if (got < 0) failed |= 1u << face;" in follow and "else retry_ns[face] = now + 2000000000LL;" in follow,
          "a face's font not written is not tried again, or one the folder has none of is")
    check("const int active = device >= 1 && device <= 2 && used != uses[device];" in follow and
          "follow_record(&follow, face, now, active)" in follow, "follow_main does not tell follow_record whether the device was used")
    # ((11-19) Auto's memory beside the saves: Setup makes prompts/ again from nothing at an update)
    check('snprintf(out, cap, "%s/prompts_auto.txt", hos_user_dir ? hos_user_dir : "user");' in block(prompts, "static void auto_path(")
          and "auto_path(path, sizeof path);" in block(prompts, "static int remembered_face(") and "auto_path(path, sizeof path);" in follow
          and "prompts/auto.txt" not in follow, "Auto's device used last is kept in prompts/, which Setup makes again at an update")
    check(re.search(r"int \(\*prompt_pad_face\)\(void\);\s*\} MkwVkHost;", api) is not None and
          "#define MKWVK_API_VERSION 15" in api, "gpu/vk/vk_api.h has no prompt_pad_face last, at version 15")
    check("hos_bench_state, hos_prompts_pad_face,\n};" in (ROOT / "host/rx_backend.c").read_text(encoding="utf-8"),
          "host/rx_backend.c does not give the back end hos_prompts_pad_face")
    imgui = (ROOT / "gpu/vk/vk_imgui.cpp").read_text(encoding="utf-8")
    check('"prompts/External.pad." + std::to_string(pad) + ".bfotf.zs"' in imgui and
          "glyphs_follow();\n    ImGui_ImplVulkan_NewFrame();" in imgui and "std::thread(read_glyphs).detach()" in block(imgui, "void glyphs_follow("),
          "the overlay does not read the controller used last's icons, or not again on its own thread")
    gf = block(imgui, "void glyphs_follow(")
    check("fonts_.glyph = pad_glyphs[pad], glyph_pad.store(pad, std::memory_order_relaxed);" in gf and
          gf.index("if (pad_glyphs[pad]) {") < gf.index("std::thread(read_glyphs)") and
          "if (pad >= 0 && pad < HOS_PROMPT_N) pad_glyphs[pad] = fonts_.glyph;" in block(imgui, "void load_glyphs("),
          "a controller face shown again adds another font to the atlas")
    check("face = host->prompt_pad_face();" in block(menu, "const char* pad_name(int p)"),
          "pad_name does not name the controller used last's buttons with the keys' face")
    print("ok: %d keys with an icon each, the controls' tables agree, Auto chooses %d cases right, and the device used last "
          "is followed in %d" % (len(keys), len(want), len(devices) + len(follows) + len(dues)))


if __name__ == "__main__":
    main()
