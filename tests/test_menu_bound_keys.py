"""The options' tabs and Back answer to the keys the player bound to the Switch's L, R and B, and the guides name them
(gpu/vk/vk_menu.cpp, gpu/vk/vk_ui.h, host/nn_hid.c; no game).

The lab's playtest showed the hints at odds: the tab strip's ends named the keys bound to L and R (Q and R), the guides
said "Category Q E" and "Back Esc", and the game's own tab said "Back Shift" (key_b=16). Our tabs took neither R nor
Shift. Now the guides and the strip name the same keys, taken from the bindings as the game's guides are, and the menu
takes them beside its own Q, E, Tab and Esc.

The key handling is compiled from menu_tick as it is (the block between its markers) with stand-ins for Windows and the
host, and run: a bound R steps a tab once per press; a bound Shift is Back as it is let go, and not when Tab was pressed
with it (Shift+Tab still steps back a tab), nor while a binding waits for its key, nor when it went down with the menu
closed, nor when the window lost the front while it was held, nor when another key, a mouse button or a
pad press went with it; a bound R tapped between two ticks counts from the host's queue, once; a bound key the menu has its own use for (an arrow) keeps that use; a mouse button bound to B is no key of the
menu's; the gamepad input L is bound to steps a tab unless it is one the menu uses (A's). The keys bound to L and R as the
game has them (for its own tab, which opens ours) are read every tick. While the Mods screen's search takes text
(menu_mods_typing) the bound keys type there: a bound R is no category and Shift no Back, and nothing of theirs is kept
for after; the guides then name the menu's own Q, E and Esc. The rest is read in the source: the labels, the
glyphs, the strip, and the host's Back tapped for a let-go modifier.

    python -I -B tests/test_menu_bound_keys.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def function(text, head):
    start = text.index(head)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def enum_block(text, head):
    start = text.index(head)
    return text[start:text.index("};", start) + 2]


FIXTURE = r'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
@ENUMS@
enum {
    VK_INSERT = 0x2d, VK_UP = 0x26, VK_DOWN = 0x28, VK_LEFT = 0x25, VK_RIGHT = 0x27, VK_ESCAPE = 0x1b, VK_F1 = 0x70,
    VK_RETURN = 0x0d, VK_DELETE = 0x2e, VK_BACK = 8, VK_TAB = 9, VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_HOME = 0x24,
    VK_END = 0x23, VK_F8 = 0x77, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12, VK_RBUTTON = 2, VK_LBUTTON = 1,
    VK_LSHIFT = 0xa0, VK_RSHIFT = 0xa1, VK_LCONTROL = 0xa2, VK_RCONTROL = 0xa3, VK_LMENU = 0xa4, VK_RMENU = 0xa5, VK_LWIN = 0x5b
};
static bool key_down[256];
static short GetAsyncKeyState(int k) { return key_down[k & 255] ? (short)0x8000 : 0; }
static int bind_key[HOS_KEY_N][2], bind_pad[HOS_SW_N];
static int binding(int set, int button, int slot) { return set == HOS_BIND_PAD ? (slot ? -1 : bind_pad[button]) : bind_key[button][slot]; }
static bool queue[256];   // (the host's queue of presses, host/rx_backend.c: a key's press until it is taken)
static int take_key_press(int k) { const bool q = queue[k & 255]; queue[k & 255] = false; return q; }
static struct { int (*binding)(int, int, int); int (*take_key_press)(int); } host_stub{binding, take_key_press}, *host = &host_stub;
static struct { bool v; bool load() const { return v; } } open_now{true};
static int listening;
static bool pad_last;
static bool focused = true;
static int pad_confirm = HOS_PAD_EAST, pad_back = HOS_PAD_SOUTH;
static bool typing_now;   // (the Mods screen's search taking text)
static bool menu_mods_typing() { return typing_now; }
@HELPERS@
struct Out { bool left, right, back, game_l, game_r; };
static uint32_t pad_press;
static Out tick() {
    enum { K_N = 17 };
    static bool held[K_N];
    static const int keys[K_N] = {VK_INSERT, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_ESCAPE, VK_F1, VK_RETURN, VK_DELETE, VK_BACK,
                                  'Q', 'E', VK_TAB, VK_PRIOR, VK_NEXT, VK_HOME, VK_END};
    bool press[K_N] = {};
    for (int i = 0; i < K_N; i++) {
        const bool down = key_down[keys[i]];
        press[i] = down && !held[i];
        held[i] = down;
    }
    const bool mine = focused;
    bool tab_left = false, tab_right = false;
    int gk_vk[20] = {}, gk_n = 0;   // ((11-19) the game's keys menu_tick reads before (none here): their queued presses shared
    bool gk_q[20] = {};             // with these keys' read)
    auto pad = [&](int p) { return listening != 1 && (pad_press >> p & 1) != 0; };
@TRACKER@
    pad_press = 0;
    return {tab_left, tab_right, press[5], game_lr[0], game_lr[1]};
}
int main() {
    bind_key[HOS_SW_L][0] = 'Q', bind_key[HOS_SW_R][0] = 'R', bind_key[HOS_SW_B][0] = VK_SHIFT;   // (the defaults)
    bind_pad[HOS_SW_L] = HOS_PAD_LSHOULDER, bind_pad[HOS_SW_R] = HOS_PAD_RSHOULDER;
    // R: the next tab, once a press
    key_down['R'] = true;
    pad_last = true;
    Out o = tick();
    CHECK(o.right && !o.left && !o.back && o.game_r && !pad_last);   // (and the prompts show the keys)
    o = tick();
    CHECK(!o.right && !o.game_r);   // (held: no second press)
    key_down['R'] = false;
    CHECK(!tick().right);
    // Shift: Back as it is let go
    key_down[VK_SHIFT] = true;
    CHECK(!tick().back);
    CHECK(!tick().back);
    key_down[VK_SHIFT] = false;
    o = tick();
    CHECK(o.back && !o.left && !o.right);
    CHECK(!tick().back);
    // Shift+Tab: no Back (the menu's own keys step back a tab)
    key_down[VK_SHIFT] = true;
    tick();
    key_down[VK_TAB] = true;
    tick();
    key_down[VK_TAB] = false, key_down[VK_SHIFT] = false;
    CHECK(!tick().back);
    // a binding waiting for its key: Shift is the key, not Back
    listening = 2;
    key_down[VK_SHIFT] = true;
    tick();
    listening = 0;
    key_down[VK_SHIFT] = false;
    CHECK(!tick().back);
    // pressed in play, let go with the menu open: no Back
    open_now.v = false;
    key_down[VK_SHIFT] = true;
    tick();
    open_now.v = true;
    key_down[VK_SHIFT] = false;
    CHECK(!tick().back);
    // Shift held as the window loses the front (Win+Shift+S, Shift+Alt+Tab): no Back, then or as it comes back
    key_down[VK_SHIFT] = true;
    tick();
    focused = false;
    CHECK(!tick().back);
    key_down[VK_SHIFT] = false;
    CHECK(!tick().back);
    focused = true;
    CHECK(!tick().back);
    // Shift with another key, a mouse button or a pad press: no Back as Shift is let go (Shift+R steps a tab only)
    static const int withs[] = {'R', 'A', VK_LBUTTON, VK_LWIN, -1};
    for (int with : withs) {
        key_down[VK_SHIFT] = key_down[VK_LSHIFT] = true;
        tick();
        if (with < 0) pad_press = 1u << HOS_PAD_NORTH;
        else key_down[with] = true;
        o = tick();
        CHECK(o.right == (with == 'R') && !o.back);
        if (with >= 0) key_down[with] = false;
        tick();
        key_down[VK_SHIFT] = key_down[VK_LSHIFT] = false;
        o = tick();
        CHECK(!o.back && !o.right);
    }
    // Shift alone, its left key read as well: still Back
    key_down[VK_SHIFT] = key_down[VK_LSHIFT] = true;
    tick();
    key_down[VK_SHIFT] = key_down[VK_LSHIFT] = false;
    CHECK(tick().back);
    // R tapped between two ticks: the host's queue has it, one tab
    queue['R'] = true;
    o = tick();
    CHECK(o.right && !queue['R']);
    CHECK(!tick().right);
    // Shift held and R tapped between two ticks: one tab, and no Back as Shift is let go
    key_down[VK_SHIFT] = true;
    tick();
    queue['R'] = true;
    o = tick();
    CHECK(o.right && !o.back);
    key_down[VK_SHIFT] = false;
    CHECK(!tick().back);
    // R down at a tick and its queued press taken at the next: one tab, not two
    key_down['R'] = true;
    CHECK(tick().right);
    queue['R'] = true;
    CHECK(!tick().right);
    key_down['R'] = false;
    CHECK(!tick().right);
    // a queued press from another window is not ours (and is taken, so it does not come later)
    focused = false, queue['R'] = true;
    CHECK(!tick().right);
    focused = true;
    CHECK(!tick().right);
    // the menu closed: our tabs take nothing, the game's tab sees its keys
    open_now.v = false;
    key_down['R'] = true;
    o = tick();
    CHECK(!o.right && o.game_r);
    key_down['R'] = false;
    tick();
    open_now.v = true;
    // a key the menu has its own use for keeps it; a mouse button is not one of the menu's keys
    bind_key[HOS_SW_R][0] = VK_RIGHT, bind_key[HOS_SW_B][0] = VK_RBUTTON;
    key_down[VK_RIGHT] = true;
    CHECK(!tick().right);
    key_down[VK_RIGHT] = false;
    tick();
    CHECK(menu_bound_key(HOS_SW_R, 0) == 0 && menu_bound_key(HOS_SW_B, 0) == 0 && menu_bound_key(HOS_SW_L, 0) == 0);
    bind_key[HOS_SW_R][1] = 'T';
    CHECK(menu_bound_key(HOS_SW_R, 1) == 'T' && menu_modifier_key(VK_SHIFT) && !menu_modifier_key('T'));
    // the gamepad: L bound to the left face button steps a tab; bound to A's button it stays A
    bind_pad[HOS_SW_L] = HOS_PAD_WEST;
    pad_press = 1u << HOS_PAD_WEST;
    CHECK(tick().left);
    bind_pad[HOS_SW_L] = pad_confirm;
    pad_press = 1u << pad_confirm;
    CHECK(!tick().left);
    // the Mods screen's search taking text: a bound R is a letter, not the next category; Shift let go alone no Back
    bind_key[HOS_SW_R][0] = 'R', bind_key[HOS_SW_R][1] = 0, bind_key[HOS_SW_B][0] = VK_SHIFT;
    bind_pad[HOS_SW_L] = HOS_PAD_WEST;
    typing_now = true;
    key_down['R'] = true;
    o = tick();
    CHECK(!o.right && !o.left && !o.back);
    key_down['R'] = false;
    queue['R'] = true;
    CHECK(!tick().right && !queue['R']);   // (taken, so it does not come once the search stops taking text)
    key_down[VK_SHIFT] = true;
    tick();
    key_down[VK_SHIFT] = false;
    CHECK(!tick().back);
    pad_press = 1u << HOS_PAD_WEST;   // (the gamepad input bound to L is no letter: still the categories)
    CHECK(tick().left);
    typing_now = false;
    o = tick();
    CHECK(!o.right && !o.back);   // (nothing kept from the typing)
    key_down['R'] = true;
    CHECK(tick().right);
    key_down['R'] = false;
    tick();
    std::puts("ok");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    ui = (ROOT / "gpu/vk/vk_ui.h").read_text(encoding="utf-8")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    hid = (ROOT / "host/nn_hid.c").read_text(encoding="utf-8")
    tick = function(menu, "void menu_tick() {")

    # the guides' keys and the strip's: the bindings', else the menu's own
    glyph = function(ui, "inline float glyph(")
    check("key(x, c.key_tab_l)" in glyph and "key(x + w + gap, c.key_tab_r)" in glyph and "return key(x, c.key_back);" in glyph,
          "the guides do not name the keys the tabs and Back take")
    check('key(x, "Q")' not in glyph and 'key(x, "Esc")' not in glyph, "the guides still name Q, E and Esc whatever is bound")
    ctx = function(menu, "ui::Ctx ui_ctx(")
    check("{HOS_SW_L, HOS_SW_R, HOS_SW_B}" in ctx and "menu_bound_key(buttons[b], 0)" in ctx and '{"Q", "E", "Esc"}' in ctx
          and "c.key_tab_l = names[0], c.key_tab_r = names[1], c.key_back = names[2];" in ctx,
          "ui_ctx does not take the labels from the bindings the menu takes")
    check("const bool typing = menu_mods_typing();" in ctx and "int vk = typing ? 0 : menu_bound_key(buttons[b], 0);" in ctx,
          "the guides name the bound keys while the Mods search takes them as text")
    check("bool menu_mods_typing() { return open_now.load() && page == kModsPage && mods_text_wanted.load(" in menu,
          "menu_mods_typing is not the Mods screen's search taking text")
    tabs = function(menu, "void draw_tabs(")
    check("(i ? c.key_tab_r : c.key_tab_l)" in tabs and "!c.pad && sel < 0 && can_bind()" in tabs,
          "the strip's ends do not name the keys our tabs take (the game's own tab: its bindings)")
    controls = function(menu, "void draw_controls(")
    check('{ui::kKey, "Cancel", "Esc"}' in controls, "a binding's wait names B's key, which it would bind, for cancel")

    # the host: a Back from a let-go modifier taps the game's Back too
    check("hos_binding(HOS_BIND_KEY, HOS_SW_B, k)" in hid and "now - b_key_ns < 300000000LL" in hid,
          "host/nn_hid.c does not tap the game's Back for our tabs left with B's key")
    # the game's own tab opens ours with the keys read every tick
    check("if (game_lr[side]) go[side] = true, pad_last = false;" in tick and "static bool key_held" not in tick,
          "the game's tab does not take its keys from the tick's reading")

    begin = tick.index("    bool bound_go[3] = {}, game_lr[2] = {};")
    end_mark = "if (p >= 0 && p < HOS_PAD_N && !own && pad(p)) (side ? tab_right : tab_left) = true;\n        }\n    }\n"
    end = tick.index(end_mark, begin) + len(end_mark)
    tracker = tick[begin:end]
    check("if (!menu_mods_typing()) tab_left |= bound_go[0], tab_right |= bound_go[1], press[5] |= bound_go[2];" in tracker,
          "the bound keys are not the tabs and Back")
    # a click or the wheel with a modifier held (read after this block, with the mouse): it is no press of its own
    wheel = tick.index("if (ptr.pressed || ptr.wheel)")
    check(wheel > end and "for (auto& armed : bound_armed) armed[0] = armed[1] = false;" in tick[wheel:wheel + 200],
          "a click or the wheel does not keep a held modifier from counting as its press")
    helpers = "\n".join(function(menu, head) for head in ("bool menu_own_key(", "bool menu_modifier_key(",
                                                         "bool menu_same_modifier(", "int menu_bound_key("))
    enums = "\n".join(enum_block(api, head) for head in ("enum { HOS_SW_A", "enum { HOS_PAD_SOUTH", "enum { HOS_BIND_PAD",
                                                         "enum { HOS_KEY_MOVE_UP"))
    fixture = FIXTURE.replace("@ENUMS@", enums).replace("@HELPERS@", helpers).replace("@TRACKER@", tracker)
    with tempfile.TemporaryDirectory(prefix="menu-bound-keys-") as tmp:
        cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", str(cpp), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode or run.stdout.strip() != "ok":
            raise SystemExit("FAIL: the bound keys in the menu (exit %d)\n%s%s" % (run.returncode, run.stdout, run.stderr))
    print("PASS: the tabs and Back take the keys bound to L, R and B, and the guides and the strip name them")


if __name__ == "__main__":
    main()
