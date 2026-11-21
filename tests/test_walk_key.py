"""The walk key (Controls): a key that, held with the movement keys, makes Link walk instead of run.

A key is down or up, so the movement keys push the left stick all the way and on the keyboard alone Link always runs.
NX2navi's walk_key and walk_scale, here the Insert menu's Walk key row (Off by default; Caps Lock, Shift, Ctrl, Tab) and
Walk scale row (0.20 to 0.90, 0.51 by default). host/nn_hid.c: walk_key_code is the row's key, walk_keys scales the
movement keys' part of the left stick while it is held, and bound_down leaves the walk key out of the buttons, so a key
that a control has by default (Shift is B's) does not press it at every walking step.

Checked here, with the two functions compiled from the source: the row's keys in the row's order; the scale along one
axis and on a diagonal (the same distance from the centre in all eight directions), its limits and its default; the
camera's keys left alone. Then the source: read_input sets the walk key before any key is read and scales the keys'
stick alone; each key the row offers that a control has by default is named, with that control, in the row's own line
about it (Alt is not offered: Windows takes it for the window's menu); the keys exist on Linux's key table; the options,
the rows, the options file's keys and the two tables that document them. tests/test_bound_keys.py has the walk key and
the buttons, tests/test_input_background.py's "walk" case the whole path to the game's controller state.

    python -I -B tests/test_walk_key.py [--cc gcc] [--cxx g++]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
# (the row's keys, in its order: the name it shows, Windows' key code's name, and the code)
KEYS = [("Caps Lock", "VK_CAPITAL", 0x14), ("Shift", "VK_SHIFT", 0x10), ("Ctrl", "VK_CONTROL", 0x11), ("Tab", "VK_TAB", 0x09)]
# (a control of host/nn_hid.c's bind_names, as the row's line about a key has to name it)
CONTROL_SAYS = {"b": "B", "ls": "left stick press", "plus": "Plus"}


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, marker):
    """text from marker to the end of the first {...} after it"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def run(compiler, src, name, tmp, flags, libs=()):
    path = Path(tmp) / name
    path.write_text(src, encoding="utf-8")
    exe = Path(tmp) / (name + ".exe")
    subprocess.run([compiler, *flags, "-Wall", "-Werror", "-o", str(exe), str(path), *libs], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    check(r.returncode == 0, name + ": " + r.stderr.strip())


C = r'''#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
/* KEY_CODES */
enum { VK_MENU = 0x12, VK_LMENU = 0xA4, VK_RMENU = 0xA5 };
/* CODE */
/* KEYS */
int main(void) {
    static const int want[] = {0, /* WANT */};
    enum { ROWS = sizeof want / sizeof *want };
    /* the row's values are its keys, in its order; anything else is no key */
    for (int v = 0; v < ROWS; v++) CHECK(walk_key_code(v) == want[v]);
    CHECK(walk_key_code(ROWS) == 0 && walk_key_code(-1) == 0 && walk_key_code(99) == 0 && walk_key_code(-2147483647 - 1) == 0);
    for (int v = -3; v < 40; v++)   /* (Alt is the window menu's key on Windows: never the walk key) */
        CHECK(walk_key_code(v) != VK_MENU && walk_key_code(v) != VK_LMENU && walk_key_code(v) != VK_RMENU);

    /* the scale: one key pushes its axis percent of all the way; two keys push a diagonal as far from the centre as one
       key pushes; the camera's keys are never touched; no key, nothing */
    for (int percent = 20; percent <= 90; percent++) {
        const int one = 32767 * percent / 100;
        for (int x = -1; x <= 1; x++)
            for (int y = -1; y <= 1; y++) {
                int32_t k[4] = {x * 32767, y * 32767, 32767, -32767};
                walk_keys(k, percent);
                CHECK(k[2] == 32767 && k[3] == -32767);
                if (!x || !y) {
                    CHECK(k[0] == x * one && k[1] == y * one);
                    continue;
                }
                CHECK(k[0] * x > 0 && k[1] * y > 0 && k[0] * x == k[1] * y);       /* (the same way, the same on both) */
                CHECK(fabs(hypot(k[0], k[1]) - one) <= 2.0);                       /* (as far out as one key) */
            }
    }
    {   /* the default, 0.51: just over half of the stick's travel (half of a key's 32767 is one short of 16384) */
        int32_t k[4] = {32767, 0, 0, 0}, d[4] = {-32767, 32767, 0, 0};
        walk_keys(k, 51);
        walk_keys(d, 51);
        CHECK(k[0] == 16711 && k[1] == 0);
        CHECK(d[0] == -11816 && d[1] == 11816);
    }
    {   /* past the row's ends it is the end's; 0 or less (an option table that does not know the row) is the default */
        static const int given[] = {0, -7, 1, 19, 91, 100, 4000}, taken[] = {51, 51, 20, 20, 90, 90, 90};
        for (int n = 0; n < 7; n++) {
            int32_t a[4] = {32767, -32767, 5, 6}, b[4] = {32767, -32767, 5, 6};
            walk_keys(a, given[n]);
            walk_keys(b, taken[n]);
            CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == 5 && a[3] == 6);
        }
    }
    printf("walk: the row's keys in its order; the keys' stick at the scale in all eight directions, the camera's kept\n");
    return 0;
}
'''

CXX = r'''#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
/* OPTIONS */
Options opts;
/* TABS */
/* ROW */
/* HINT_IN */
/* HINTS_MACRO */
/* TX_SENS */
/* TX_WALK */
/* HN_WALK */
Row rows[] = {/* THE_ROWS */};
/* STOPS */
/* INI_KEY */
static int output_choice(int v) { return v; }
static void parse(const char* text) {
    char line[80];
    snprintf(line, sizeof line, "%s", text);
    char* eq = strchr(line, '=');
    *eq = 0;
    /* LOAD_LOOP */
}
int main() {
    static const char* const names[] = {"Off", /* NAMES */};
    static const char* const replaces[] = {nullptr, /* REPLACES */};   // (the control each key has by default, as its line names it)
    const int n = (int)(sizeof names / sizeof names[0]);
    Options fresh;
    CHECK(fresh.walk_key == 0 && fresh.walk_scale == 51);   // (out of the box: no walk key, the keys run as before)
    const Row& key = rows[0];
    const Row& scale = rows[1];
    CHECK(key.value == &opts.walk_key && key.lo == 0 && key.hi == n - 1 && key.step == 1);
    CHECK(scale.value == &opts.walk_scale && scale.lo == 20 && scale.hi == 90 && scale.step == 1);
    CHECK(key.preset_exempt && !key.restart && scale.preset_exempt && !scale.restart);
    CHECK(stops_at_ends(scale));
    for (int v = 0; v < n; v++) {
        CHECK(!strcmp(key.text(v), names[v]));
        // (each value has one line about it, and the menu's height measure, variant 0 on, ends after it)
        const char* line = key.hint(v, -1);
        CHECK(line && key.hint(v, 0) == line && key.hint(v, 1) == nullptr);
        if (v) CHECK(strstr(line, names[v]) == line);   // (it starts with the key's name)
        if (replaces[v]) CHECK(strstr(line, replaces[v]) && strstr(line, "by default") && strstr(line, "does not press"));
        else if (v) CHECK(strstr(line, "presses no button by default"));
    }
    CHECK(!strcmp(key.text(-1), "Off") && !strcmp(key.text(n), "Off"));
    CHECK(!strcmp(scale.text(51), "0.51") && !strcmp(scale.text(20), "0.20") && !strcmp(scale.text(90), "0.90"));
    CHECK(!strcmp(ini_key(key), "walk_key") && !strcmp(ini_key(scale), "walk_scale"));
    parse("walk_key=2\n"), parse("walk_scale=35\n");
    CHECK(opts.walk_key == 2 && opts.walk_scale == 35);
    parse("walk_key=77\n"), parse("walk_scale=5\n");
    CHECK(opts.walk_key == n - 1 && opts.walk_scale == 20);
    parse("walk_key=-3\n"), parse("walk_scale=250\n");
    CHECK(opts.walk_key == 0 && opts.walk_scale == 90);
    parse("walk_key=lalt\n");   // (NX2navi's file names the key; here it is the row's number: no key)
    CHECK(opts.walk_key == 0);
    printf("menu: Walk key Off and %d keys, each bound one named in its line; Walk scale 0.20 to 0.90, 0.51 out of the box\n", n - 1);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    hid, api, replay = read("host", "nn_hid.c"), read("gpu", "vk", "vk_api.h"), read("gpu", "vk", "vk_replay.cpp")
    menu, header = read("gpu", "vk", "vk_menu.cpp"), read("gpu", "vk", "vk_menu.h")

    # ---- the row's keys: nn_hid.c's codes and the menu's names, one order ----
    code = block(hid, "static int walk_key_code(")
    codes = re.search(r"static const int codes\[\] = \{0, ([^}]*)\};", code)
    check(codes is not None, "walk_key_code has no table of the row's keys")
    codes = [c.strip() for c in codes.group(1).split(",")]
    check(codes == [k[1] for k in KEYS], "the Walk key row's keys are not %s, in that order: %s" % ([k[1] for k in KEYS], codes))
    names = re.search(r'static const char\* t\[\] = \{"Off", ([^}]*)\};', block(menu, "const char* tx_walk_key("))
    check(names is not None and [s.strip().strip('"') for s in names.group(1).split(",")] == [k[0] for k in KEYS],
          "the menu's names for the Walk key row are not nn_hid.c's keys, in its order")

    # ---- what each key is bound to out of the box, which the row's line about it must name ----
    binds = re.search(r"static const char \*const bind_names\[HOS_KEY_N\] = \{([^}]*)\};", hid)
    defaults = re.search(r"static const unsigned char key_default\[HOS_KEY_N\]\[2\] = \{\s*(.*?)\};", hid, re.S)
    check(binds is not None and defaults is not None, "nn_hid.c's controls and their default keys were not found")
    controls = [s.strip().strip('"') for s in binds.group(1).split(",")]
    keys_of = [[k.strip() for k in entry.split(",")] for entry in re.findall(r"\{([^{}]*)\}", defaults.group(1))]
    check(len(controls) == len(keys_of) == 24, "the controls and their default keys do not pair up")
    replaces = []
    for name, vk, _ in KEYS:
        has = [controls[i] for i, keys in enumerate(keys_of) if vk in keys]
        check(len(has) <= 1, "%s is two controls' key by default: %s" % (name, has))
        check(not has or has[0] in CONTROL_SAYS, "%s is %s's key by default, and this test has no name for that control" % (name, has))
        replaces.append(CONTROL_SAYS[has[0]] if has else None)
    check(replaces == [None, "B", "left stick press", "Plus"],
          "the default keys changed: Caps Lock free, Shift B's, Ctrl the left stick press's, Tab Plus's was %s" % replaces)

    # ---- the source: the walk key is set before any key is read, is no control's key, and scales the keys' stick alone ----
    down = block(hid, "static int bound_down(")
    check("if (vk <= 0 || vk == walk_vk) continue;" in down, "the walk key still presses the control it is bound to")
    rd = re.sub(r"\s+", " ", block(hid, "static void read_input("))
    set_at = rd.find("walk_vk = walk_key_code(rx_option(HOS_OPT_WALK_KEY, 0));")
    check(set_at >= 0, "read_input does not take the walk key from the Walk key row (Off without the menu)")
    check(set_at < rd.index("bound_down("), "a key is read before the walk key is known: it would press its button for one read")
    scaled = "if (walk_vk && key(walk_vk)) walk_keys(keys, rx_option(HOS_OPT_WALK_SCALE, 51));"
    check(rd.count("walk_keys(") == 1 and scaled in rd, "the movement keys are not scaled by Walk scale while the walk key is held")
    built = rd.index("int32_t keys[4] = {(bound_down(HOS_KEY_MOVE_RIGHT, captured)")
    check(built < rd.index(scaled) < rd.index("stick_max(stick, keys);"),
          "the walk scale is not on the keys' own stick before it joins the pads' and the script's")
    check("walk_" not in block(hid, "static void pads_read(") and "walk_" not in block(hid, "static void scripted_input("),
          "a controller's stick or a scripted press is scaled by the walk key")
    behind = rd[rd.index("if (!game_in_front()"):]
    check(set_at > rd.index("if (!game_in_front()") + behind.index("return;"),
          "the walk key is read while another window is in front")

    # ---- Linux: the row's keys are in the key table SDL's keyboard is read into ----
    posix = block(read("host", "posix", "display_sdl.c"), "static void keys_update(")
    for name, vk, _ in KEYS:
        check(re.search(r"\bv\[%s\] = " % vk, posix) is not None, "host/posix/display_sdl.c has no %s: the row's %s would do nothing on Linux" % (vk, name))

    # ---- the options: 54 and 55 ----
    ids = re.search(r"enum \{ HOS_OPT_RUMBLE_CURVE = 50, HOS_OPT_RUMBLE_TRIGGERS, HOS_OPT_LSTICK_ZONE, HOS_OPT_RSTICK_ZONE, "
                    r"HOS_OPT_WALK_KEY,\s+HOS_OPT_WALK_SCALE\b[^}]*\}", api)
    check(ids is not None, "HOS_OPT_WALK_KEY and HOS_OPT_WALK_SCALE are not options 54 and 55")
    option = block(replay, "t.option = [](int id) -> int {")
    check("if (id == HOS_OPT_WALK_KEY) return opts.walk_key;" in option
          and "if (id == HOS_OPT_WALK_SCALE) return std::clamp(opts.walk_scale, 20, 90);" in option,
          "the host cannot see the Walk key row or its scale (options 54 and 55)")

    # ---- the rows and the options file ----
    key_row = re.search(r'\{TAB_CONTROLS, "Walk key", &opts\.walk_key, 0, %d, 1, tx_walk_key,\s*"[^}]*hn_walk_key, false, true\},'
                        % len(KEYS), menu)
    scale_row = re.search(r'\{TAB_CONTROLS, "Walk scale", &opts\.walk_scale, 20, 90, 1, tx_sens,\s*"[^}]*nullptr, false, true\},', menu)
    check(key_row is not None and scale_row is not None and key_row.start() < scale_row.start(),
          "the Controls tab has no Walk key row (Off and the %d keys) with a Walk scale row (0.20 to 0.90) under it" % len(KEYS))
    hints = re.search(r"^MENU_HINTS\(hn_walk_key, 0,.*?\"\)\n", menu, re.S | re.M)
    macro = re.search(r"^#define MENU_HINTS\(fn, lo, \.\.\.\).*?\n    \}\n", menu, re.S | re.M)
    check(hints is not None and macro is not None, "the Walk key row's lines about its keys (hn_walk_key) were not found")
    loop = block(block(menu, "void menu_init("), "for (Row& r : rows) {")
    cxx = CXX
    for marker, text in (("/* OPTIONS */", re.search(r"struct Options \{.*?\n\};", header, re.S).group(0)),
                         ("/* TABS */", re.search(r"enum MenuTab \{.*?\};", menu, re.S).group(0)),
                         ("/* ROW */", re.search(r"struct Row \{.*?\n\};", menu, re.S).group(0)),
                         ("/* HINT_IN */", block(menu, "const char* hint_in(")),
                         ("/* HINTS_MACRO */", macro.group(0)),
                         ("/* TX_SENS */", block(menu, "const char* tx_sens(")),
                         ("/* TX_WALK */", block(menu, "const char* tx_walk_key(")),
                         ("/* HN_WALK */", hints.group(0)),
                         ("/* THE_ROWS */", key_row.group(0) + "\n" + scale_row.group(0)),
                         ("/* STOPS */", block(menu, "bool stops_at_ends(")),
                         ("/* INI_KEY */", "bool is_merged(const int*) { return false; }   /* ((Triforce, options_version 7) no merged row here */\n" + block(menu, "bool ini_row(") + "\n" + block(menu, "const char* ini_key(")),
                         ("/* LOAD_LOOP */", loop),
                         ("/* NAMES */", ", ".join('"%s"' % k[0] for k in KEYS)),
                         ("/* REPLACES */", ", ".join('"%s"' % r if r else "nullptr" for r in replaces))):
        cxx = cxx.replace(marker, text)
    c = C
    for marker, text in (("/* KEY_CODES */", "enum { %s };" % ", ".join("%s = %#x" % (k[1], k[2]) for k in KEYS)),
                         ("/* CODE */", code), ("/* KEYS */", block(hid, "static void walk_keys(")),
                         ("/* WANT */", ", ".join(k[1] for k in KEYS))):
        c = c.replace(marker, text)

    # ---- the tables that document them ----
    check(re.search(r"^\| Walk key \| Off, Caps Lock, Shift, Ctrl, Tab", read("readme.md"), re.M) is not None,
          "readme.md's settings table has no Walk key row with its keys")
    check(re.search(r"^\| Walk scale \| 0\.20 to 0\.90", read("readme.md"), re.M) is not None,
          "readme.md's settings table has no Walk scale row")
    dev = read("docs", "development.md")
    check(re.search(r"^\| `walk_key` \| 0 Off, 1 Caps Lock, 2 Shift, 3 Ctrl, 4 Tab \|", dev, re.M) is not None
          and re.search(r"^\| `walk_scale` \| hundredths, 20 to 90", dev, re.M) is not None,
          "docs/development.md's options table does not say what walk_key's and walk_scale's numbers are")

    with tempfile.TemporaryDirectory(prefix="walk-key-") as tmp:
        run(a.cc, c, "walk.c", tmp, ["-std=c11", "-O1", "-Wextra"], ["-lm"])
        run(a.cxx, cxx, "menu.cpp", tmp, ["-std=c++20"])
    print("ok")


if __name__ == "__main__":
    main()
