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
keyboard and mouse with no controller, the Switch's for XInput's nameless one; the row's names end with "Keyboard & mouse",
its default is still Auto, and the documents name the new choice.

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

    # Auto's choice
    harness = "\n".join([
        "#include <ctype.h>", "#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>", '#include "gpu/vk/vk_api.h"',
        block(prompts, "static int contains("),
        block(prompts, "static int auto_face("),
        "int main(int argc, char **argv) {",
        "    (void)argc;",
        "    printf(\"%d\\n\", auto_face(argv[1], atoi(argv[2])));",
        "    return 0;",
        "}", ""])
    want = [("", 0, "KEYBOARD"), ("", 1, "SWITCH"), ("Xbox Series X Controller", 0, "XBOX"), ("8BitDo Pro 2", 0, "XBOX"),
            ("DualSense Wireless Controller", 0, "PS5"), ("PS4 Controller", 0, "PS4"), ("Steam Deck", 0, "STEAMDECK"),
            ("Nintendo Switch Pro Controller", 0, "SWITCH"), ("Xbox 360 Controller", 1, "XBOX")]
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "auto.c", Path(tmp) / "auto"
        c.write_text(harness, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), str(c), "-o", str(exe)], check=True)
        for name, xinput, face in want:
            got = int(subprocess.run([str(exe), name, str(xinput)], check=True, capture_output=True, text=True).stdout)
            check(faces[got] == "HOS_PROMPT_" + face, "Auto with controller %r%s: %s, not %s"
                  % (name, " and XInput's" if xinput else "", faces[got], face))
    choose = block(prompts, "static int choose_face(")
    check("auto_face(name, xinput)" in choose and "!name[0] && hos_xinput_connected()" in choose,
          "choose_face does not ask auto_face, or asks XInput while SDL names a controller")
    check("int hos_xinput_connected(void) {" in hid, "host/nn_hid.c has no hos_xinput_connected")
    print("ok: %d keys with an icon each, the controls' tables agree, and Auto chooses %d cases right" % (len(keys), len(want)))


if __name__ == "__main__":
    main()
