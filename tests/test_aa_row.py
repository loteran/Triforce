"""The Insert menu's Anti-aliasing row (gpu/vk/aa_rows.h, gpu/vk/vk_menu.cpp) and the rule that finds the game's own SMAA
passes among a frame's compute dispatches (aa_rows.h; gpu/vk/vk_draw.cpp aa_stand_in, gpu/vk/vk_aa.cpp). No game, no GPU.

Compiled and run:

- aa_rows.h itself: the row's four choices and their saved numbers, RX_VK_AA over the row, the choice in effect with each
  Upscaler (the game's own SMAA while DLSS, DLAA or FSR draws), the three programs by their keys, the composition's two
  ends found among authored bindings (and nothing guessed when they are not as expected: no target, two targets, a target
  that is not written as integer words, no picture of the target's size, two of them, the target sampled in place), when
  the game's edge and weight passes are left out, and what is kept from frame to frame (a new choice's first frame runs
  the game's edge and weight passes; none replaced for a minute is said once).
- vk_menu.cpp's own text: the row's entry in rows[] with the menu's Row, its key in the options file (ini_key) and the
  line menu_init reads a saved value with (clamped to the row), its value's text and the line under its help (tx_aa,
  hn_aa) with each Upscaler, with RX_VK_AA and with the passes missing, and row_fixed for it.

Read in the sources: the row stands with the scaling rows on the Graphics tab, is the game's own by default, takes no
restart and is not a Quality preset's; a dispatch asks aa_leave_out before anything of it is recorded and aa_stand_in in
the stand-ins' place, both of which ask nothing more than a comparison with the game's own chosen; the readme and the
development guide have the row, its numbers and its switches.

    python -I -B tests/test_aa_row.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, at):
    """text from at to the end of the first {...} after it"""
    start = text.index("{", at)
    depth = 0
    for i in range(start, len(text)):
        depth += text[i] == "{"
        depth -= text[i] == "}"
        if depth == 0:
            return text[at:i + 1]
    raise SystemExit("no end for the block at %d" % at)


def definition(text, head):
    found = re.search(re.escape(head) + r"\([^;{]*\)\s*\{", text)
    check(found, "no definition of " + head)
    return block(text, found.start())


RULE = r'''#include "@ROOT@/gpu/vk/aa_rows.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
using namespace mkwvk::aa_rows;
namespace ur = mkwvk::upscaler_rows;

/* a binding as vk_draw.cpp's aa_stand_in describes it */
static Bound tex(uint64_t image, int format, uint32_t w, uint32_t h, bool flat = true, bool integer = false) {
    Bound b;
    b.image = image, b.format = format, b.width = w, b.height = h, b.flat = flat, b.integer = integer;
    return b;
}
static Bound img(uint64_t image, int format, uint32_t w, uint32_t h, bool written = true, bool integer = true, bool flat = true) {
    Bound b = tex(image, format, w, h, flat);
    b.written = written, b.integer = integer;
    return b;
}

int main() {
    /* the row: the saved numbers keep their meaning */
    CHECK(GAME == 0 && OFF == 1 && FXAA == 2 && SMAA == 3 && kValues == 4);
    CHECK(!std::strcmp(name(GAME), "Game SMAA") && !std::strcmp(name(OFF), "Off") && !std::strcmp(name(FXAA), "FXAA") &&
          !std::strcmp(name(SMAA), "SMAA Ultra"));
    CHECK(!std::strcmp(name(-1), name(GAME)) && !std::strcmp(name(4), name(GAME)));
    /* RX_VK_AA over the row; anything that is not a choice is the game's own */
    for (int row = 0; row < kValues; row++) {
        CHECK(chosen(row, -1) == row);
        for (int env = 0; env < kValues; env++) CHECK(chosen(row, env) == env);
        CHECK(chosen(row, 4) == GAME && chosen(row, 99) == GAME);
    }
    CHECK(chosen(-1, -1) == GAME && chosen(4, -1) == GAME && chosen(1000, -1) == GAME);
    /* with the Upscaler row: the choice with the game's own upscaler; while DLSS, DLAA, FSR 3.1, FSR 4 or a native AA draws
       from the tone mapped picture none, the game's SMAA left out of the jittered frames they gather (FSR3-06, FSR4-5); before
       the bloom the game's own SMAA (the upscaler took the scene before it), and RX_VK_AA's choice over them for an A/B; the
       row then has nothing to choose, nor with RX_VK_AA set */
    for (int up = 0; up < ur::kUpscalers; up++) {
        CHECK(upscaler_own(up) == (up != ur::GAME));
        for (int v = 0; v < kValues; v++) {
            CHECK(effective(v, up) == (up == ur::GAME ? v : OFF));
            CHECK(effective(v, up, false) == effective(v, up) && effective(v, up, true) == (up == ur::GAME ? v : GAME));
            CHECK(effective(v, up, false, true) == v && effective(v, up, true, true) == v);
        }
        CHECK(fixed(up, false) == (up != ur::GAME) && fixed(up, true));
    }
    CHECK(upscaler_own(ur::DLSS) && upscaler_own(ur::DLAA) && upscaler_own(ur::FSR3) && upscaler_own(ur::FSR3_AA) &&
          upscaler_own(ur::FSR4) && upscaler_own(ur::FSR4_AA) && !upscaler_own(ur::GAME));

    /* the game's three programs, and no other (the gameplay upscale's networks are not anti-aliasing passes) */
    CHECK(role(0x26fe0c70ab9fc547ull) == EDGES && role(0x685adb41edc19de7ull) == WEIGHTS && role(0x6637ac1d052d22d4ull) == COMPOSE);
    CHECK(role(0) == NONE && role(0xfd029a317a9a4b94ull) == NONE && role(0x9dc98871eb7fc101ull) == NONE);
    CHECK(role(0x6637ac1d052d22d5ull) == NONE && role(0x6637ac1d00000000ull) == NONE && role(~0ull) == NONE);
    CHECK(kEdgesKey != kWeightsKey && kWeightsKey != kComposeKey && kEdgesKey != kComposeKey);

    /* the composition as the game dispatches it: it samples the tone mapped picture (B10G11R11, the target's size), its
       weights and the lookup texture, and stores integer words into the B10G11R11 target */
    {
        const Bound textures[] = {tex(11, R11, 1536, 864), tex(12, OTHER, 1536, 864), tex(13, OTHER, 160, 560)};
        const Bound images[] = {img(21, R11, 1536, 864)};
        Ends e = compose_ends(textures, 3, images, 1);
        CHECK(!e.why && e.src == 0 && e.dst == 0);
        /* in any order of its bindings, and at any size of the 3D */
        const Bound shuffled[] = {tex(13, OTHER, 160, 560), tex(12, OTHER, 1536, 864), tex(11, R11, 1536, 864)};
        const Bound more[] = {img(31, OTHER, 384, 216), img(21, R11, 1536, 864)};
        e = compose_ends(shuffled, 3, more, 2);
        CHECK(!e.why && e.src == 2 && e.dst == 1);
        const Bound big[] = {tex(11, R11, 3840, 2160), tex(12, OTHER, 3840, 2160)};
        const Bound big_out[] = {img(21, R11, 3840, 2160)};
        e = compose_ends(big, 2, big_out, 1);
        CHECK(!e.why && e.src == 0 && e.dst == 0);
        /* the target as the RGB9E5 image at the same memory takes the same words */
        const Bound e5[] = {img(22, E5, 1536, 864)};
        e = compose_ends(textures, 3, e5, 1);
        CHECK(!e.why && e.src == 0 && e.dst == 0);
        /* other textures of the scene's format do not confuse it: another size (the bloom's), an array, the weights */
        const Bound busy[] = {tex(14, R11, 768, 432), tex(15, R11, 1536, 864, false), tex(11, R11, 1536, 864), tex(12, OTHER, 1536, 864)};
        e = compose_ends(busy, 4, images, 1);
        CHECK(!e.why && e.src == 2 && e.dst == 0);
        /* the weights it samples are read as integer words: whatever image holds them, one of the scene's format and size
           too, they are not the picture */
        const Bound words[] = {tex(12, R11, 1536, 864, true, true), tex(11, R11, 1536, 864)};
        e = compose_ends(words, 2, images, 1);
        CHECK(!e.why && e.src == 1 && e.dst == 0);
        /* an image it only reads, or writes as colours, is not the target */
        const Bound read_too[] = {img(41, R11, 1536, 864, false, true), img(42, R11, 1536, 864, true, false), img(21, R11, 1536, 864)};
        e = compose_ends(textures, 3, read_too, 3);
        CHECK(!e.why && e.src == 0 && e.dst == 2);
    }
    /* anything else is left to the game, with the reason: nothing is guessed */
    {
        const Bound textures[] = {tex(11, R11, 1536, 864), tex(12, OTHER, 1536, 864)};
        const Bound none_written[] = {img(21, R11, 1536, 864, false, true)};
        const Bound not_integer[] = {img(21, R11, 1536, 864, true, false)};
        const Bound wide[] = {img(21, OTHER, 1536, 864)};   /* (an RGBA16F scene: not this pass as it is known) */
        const Bound layered[] = {img(21, R11, 1536, 864, true, true, false)};
        const Bound unbound[] = {img(0, R11, 1536, 864)};
        const Bound empty[] = {img(21, R11, 0, 0)};
        const Bound two[] = {img(21, R11, 1536, 864), img(22, R11, 1536, 864)};
        const Bound good[] = {img(21, R11, 1536, 864)};
        for (const Bound* images : {none_written, not_integer, wide, layered, unbound, empty}) {
            const Ends e = compose_ends(textures, 2, images, 1);
            CHECK(e.why && e.src == -1 && e.dst == -1 && std::strstr(e.why, "no B10G11R11 target"));
        }
        Ends e = compose_ends(textures, 2, two, 2);
        CHECK(e.why && e.src == -1 && e.dst == -1 && std::strstr(e.why, "more than one"));
        e = compose_ends(textures, 2, good, 0);
        CHECK(e.why && e.dst == -1);
        /* the picture: none of the target's size, one of another format, the target itself sampled, two candidates */
        const Bound small[] = {tex(11, R11, 768, 432), tex(12, OTHER, 1536, 864)};
        const Bound other[] = {tex(11, E5, 1536, 864), tex(12, OTHER, 1536, 864)};
        const Bound in_place[] = {tex(21, R11, 1536, 864), tex(12, OTHER, 1536, 864)};
        const Bound level[] = {tex(11, R11, 1536, 864, false), tex(12, OTHER, 1536, 864)};
        const Bound as_words[] = {tex(11, R11, 1536, 864, true, true), tex(12, OTHER, 1536, 864)};   /* (the picture not read as colours) */
        for (const Bound* t : {small, other, in_place, level, as_words}) {
            e = compose_ends(t, 2, good, 1);
            CHECK(e.why && e.src == -1 && e.dst == -1 && std::strstr(e.why, "samples no B10G11R11 picture"));
        }
        const Bound twice[] = {tex(11, R11, 1536, 864), tex(16, R11, 1536, 864)};
        e = compose_ends(twice, 2, good, 1);
        CHECK(e.why && e.src == -1 && e.dst == -1 && std::strstr(e.why, "more than one picture"));
        e = compose_ends(textures, 0, good, 1);
        CHECK(e.why && e.src == -1 && e.dst == -1);
    }

    /* the game's edge and weight passes go only with another choice in effect and the composition before replaced;
       the composition itself and every other program are never left out; RX_VK_AA_KEEP keeps them */
    for (int r = NONE; r <= COMPOSE; r++)
        for (int mode = 0; mode < kValues; mode++)
            for (int replaced = 0; replaced < 2; replaced++)
                for (int keep = 0; keep < 2; keep++)
                    CHECK(leave_out(r, mode, replaced, keep) == ((r == EDGES || r == WEIGHTS) && mode != GAME && replaced && !keep));

    /* frame to frame. The game's own: nothing kept, nothing left out */
    {
        Track t;
        for (uint64_t f = 1; f < 10000; f++) CHECK(!t.begin(f, GAME) && !t.last_replaced && t.state == QUIET);
        /* Off chosen: its first frame's edge and weight passes still run (no composition replaced yet); from the frame after
           the first replaced composition they go */
        CHECK(!t.begin(10000, OFF) && !leave_out(EDGES, OFF, t.last_replaced, false) && t.state == QUIET);
        t.composed(10000, true);
        CHECK(t.last_replaced && t.state == REPLACED);
        CHECK(!t.begin(10001, OFF) && leave_out(EDGES, OFF, t.last_replaced, false) && leave_out(WEIGHTS, OFF, t.last_replaced, false));
        /* a composition that could not be replaced: the passes run again from the next frame, until one is replaced */
        t.composed(10001, false);
        CHECK(!t.begin(10002, OFF) && !leave_out(EDGES, OFF, t.last_replaced, false));
        t.composed(10002, true);
        CHECK(!t.begin(10003, OFF) && leave_out(EDGES, OFF, t.last_replaced, false));
        /* another choice goes on leaving them out (its composition is replaced all the same) */
        t.composed(10003, true);
        CHECK(!t.begin(10004, SMAA) && t.last_replaced && t.state == QUIET);
        t.composed(10004, true);
        CHECK(t.state == REPLACED);
        /* back to the game's own, then Off again: what was replaced before does not count, the passes run first */
        CHECK(!t.begin(10005, GAME) && !t.last_replaced && t.state == QUIET);
        CHECK(!t.begin(10006, OFF) && !t.last_replaced && !leave_out(EDGES, OFF, t.last_replaced, false));
        /* an upscaler with its own anti-aliasing comes on after the bloom: Off goes on in effect under it, the passes left out;
           before the bloom the choice in effect is the game's own: the same as back to the game's own */
        t.composed(10006, true);
        CHECK(!t.begin(10007, effective(OFF, ur::DLSS)) && t.last_replaced && leave_out(EDGES, OFF, t.last_replaced, false));
        CHECK(!t.begin(10008, effective(OFF, ur::DLSS, true)) && !t.last_replaced);
        CHECK(!t.begin(10009, effective(GAME, ur::FSR4)) && !t.last_replaced && !leave_out(EDGES, OFF, t.last_replaced, false));
    }
    /* a choice in effect and no composition replaced for a minute (another game version, a long screen without the 3D):
       said once, and again only after one was replaced or the choice changed */
    {
        Track t;
        int said = 0;
        for (uint64_t f = 100; f < 100 + 3 * kMissingFrames; f++) {
            said += t.begin(f, FXAA);
            CHECK((t.state == MISSING) == (f >= 100 + kMissingFrames) && !t.last_replaced);
        }
        CHECK(said == 1 && t.state == MISSING);
        uint64_t f = 100 + 3 * kMissingFrames;
        t.composed(f, true);
        CHECK(t.state == REPLACED);
        for (uint64_t g = f + 1; g < f + kMissingFrames; g++) CHECK(!t.begin(g, FXAA) && t.state == REPLACED);
        CHECK(t.begin(f + kMissingFrames, FXAA) && t.state == MISSING);
        CHECK(!t.begin(f + kMissingFrames + 1, SMAA) && t.state == QUIET);   /* (another choice: counted anew) */
        /* while compositions are replaced nothing is said, however long */
        Track u;
        for (uint64_t g = 1; g < 4 * kMissingFrames; g++) {
            CHECK(!u.begin(g, OFF));
            u.composed(g, true);
            CHECK(u.state == REPLACED);
        }
    }
    std::puts("aa rows: the row, the passes' rule and what is kept: all checks passed");
    return 0;
}
'''

MENU = r'''#include "@ROOT@/gpu/vk/aa_rows.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
using namespace mkwvk;
namespace ur = mkwvk::upscaler_rows;
@OPTIONS@
Options opts;
enum MenuTab { TAB_DISPLAY, TAB_GRAPHICS };
@ROW@
/* (stand-ins for what the menu asks the renderer: what upscales, RX_VK_AA, what became of the game's passes) */
int up_now = ur::GAME, env_now = -1, state_now = aa_rows::QUIET;
int upscaler() { return up_now; }
int aa_env_choice() { return env_now; }
int aa_state() { return state_now; }
bool reflex_available() { return true; }
namespace mkwvk::dlssg { bool reflex_on() { return false; } }   /* ((Triforce) Reflex through Streamline: row_fixed asks it) */
bool antilag_available() { return true; }
unsigned upscalers_available() { return ~0u; }
bool ffx3_usable() { return false; }   /* (linux fork: AMD's FSR 3.1 library, and NVIDIA's frame generation through NGX: row_fixed asks them) */
namespace mkwvk::ngxfg { bool vendor_env_set() { return false; } }
bool filter_needed() { return true; }
const char* dlss_preset_env() { return nullptr; }
int output_choice(int v) { return v; }
@TEXT@
@HINT@
@FIXED@
@KEY@
@INI_ROW@
Row rows[] = {
    @ENTRY@
};
static void read_line(char* line) {
    char* eq = std::strchr(line, '=');
    if (!eq) return;
    *eq = 0;
    @READ@
}
int main() {
    Row& r = rows[0];
    /* the row as rows[] has it: the Graphics tab, the game's own to SMAA Ultra, no restart, not a Quality preset's */
    CHECK(r.tab == TAB_GRAPHICS && !std::strcmp(r.name, "Anti-aliasing") && r.value == &opts.aa);
    CHECK(r.lo == aa_rows::GAME && r.hi == aa_rows::SMAA && r.step == 1 && r.text == tx_aa && r.hint == hn_aa);
    CHECK(!r.restart && r.preset_exempt && std::strstr(r.help, "Changes at once"));
    CHECK(opts.aa == aa_rows::GAME);   /* (the game's own unless chosen: as before the row) */
    /* its line in the options file, and a saved value read back as menu_init reads it: clamped to the row */
    CHECK(ini_row(r) && !std::strcmp(ini_key(r), "anti-aliasing"));
    for (int v = 0; v < aa_rows::kValues; v++) {
        char line[80];
        std::snprintf(line, sizeof line, "%s=%d\n", ini_key(r), v);
        opts.aa = -5;
        read_line(line);
        CHECK(opts.aa == v);
    }
    {
        char high[80] = "anti-aliasing=9\n", low[80] = "anti-aliasing=-3\n", other[80] = "anti_aliasing=2\n", none[80] = "anti-aliasing\n";
        opts.aa = 1, read_line(high);
        CHECK(opts.aa == aa_rows::SMAA);
        read_line(low);
        CHECK(opts.aa == aa_rows::GAME);
        opts.aa = 1, read_line(other), read_line(none);
        CHECK(opts.aa == 1);
    }
    /* the value's text and the line under the help, with the game's own upscaler: the choice */
    const char* const names[] = {"Game SMAA", "Off", "FXAA", "SMAA Ultra"};
    for (int v = 0; v < aa_rows::kValues; v++) {
        opts.aa = v, up_now = ur::GAME, env_now = -1, state_now = aa_rows::QUIET;
        CHECK(!std::strcmp(tx_aa(v), names[v]) && !row_fixed(&opts.aa));
        CHECK(hn_aa(v, -1) && hn_aa(v, -1) == hn_aa(v, 0));
        /* every line a value can have, for the help's height: its own, DLSS's, FSR's, the switch's, the missing passes' */
        int n = 0;
        while (hn_aa(v, n)) n++;
        CHECK(n == 5 && !hn_aa(v, 5) && !hn_aa(v, 6));
        for (int w = 0; w < v; w++) CHECK(std::strcmp(hn_aa(v, 0), hn_aa(w, 0)));
        /* replaced: nothing more to say */
        state_now = aa_rows::REPLACED;
        CHECK(hn_aa(v, -1) == hn_aa(v, 0));
        /* none replaced for a minute: said, but not of the game's own, which replaces nothing */
        state_now = aa_rows::MISSING;
        CHECK(hn_aa(v, -1) == (v == aa_rows::GAME ? hn_aa(v, 0) : hn_aa(v, 4)));
    }
    CHECK(std::strstr(hn_aa(0, 0), "game's own SMAA") && std::strstr(hn_aa(1, 0), "None") && std::strstr(hn_aa(2, 0), "FXAA") &&
          std::strstr(hn_aa(3, 0), "SMAA at its highest preset"));
    CHECK(std::strstr(hn_aa(1, 4), "the picture is the game's"));
    CHECK(!std::strcmp(tx_aa(-1), names[0]) && !std::strcmp(tx_aa(7), names[0]) && hn_aa(7, 0) == hn_aa(0, 0));   /* (past the row) */
    /* with the Upscaler row: while DLSS, DLAA or FSR draws the row says whose anti-aliasing the picture has, has nothing to
       choose, and keeps the saved choice */
    for (int up = 1; up < ur::kUpscalers; up++)
        for (int v = 0; v < aa_rows::kValues; v++) {
            opts.aa = v, up_now = up, env_now = -1, state_now = aa_rows::MISSING;
            const bool nvidia = up == ur::DLSS || up == ur::DLAA;
            CHECK(!std::strcmp(tx_aa(v), nvidia ? "DLSS's own" : "FSR's own") && row_fixed(&opts.aa));
            CHECK(hn_aa(v, -1) == hn_aa(v, nvidia ? 1 : 2) && std::strstr(hn_aa(v, -1), "nothing to set here"));
            CHECK(std::strstr(hn_aa(v, -1), nvidia ? "NVIDIA DLSS and DLAA" : "AMD FSR") && std::strstr(hn_aa(v, -1), "kept"));
            CHECK(opts.aa == v);
        }
    /* RX_VK_AA: the run's choice shown whatever the row holds, nothing to choose, and the line says why */
    for (int env = 0; env < aa_rows::kValues; env++)
        for (int v = 0; v < aa_rows::kValues; v++) {
            opts.aa = v, up_now = ur::GAME, env_now = env, state_now = aa_rows::REPLACED;
            CHECK(!std::strcmp(tx_aa(v), names[env]) && row_fixed(&opts.aa));
            CHECK(hn_aa(v, -1) == hn_aa(v, 3) && std::strstr(hn_aa(v, -1), "RX_VK_AA"));
            state_now = aa_rows::MISSING;
            CHECK(hn_aa(v, -1) == hn_aa(v, env == aa_rows::GAME ? 3 : 4));
        }
    /* the other rows' fixing is theirs: the row's own value does not fix another. ((Triforce, options_version 7) DLAA is the
       Render resolution's Native (100%) step: that row has its steps) */
    up_now = ur::DLAA, env_now = -1;
    CHECK(row_fixed(&opts.aa) && !row_fixed(&opts.resolution) && !row_fixed(&opts.sharpness));
    std::puts("aa row: its entry, its saved line, its text with each Upscaler: all checks passed");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (VK / "vk_menu.h").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    aa = (VK / "vk_aa.cpp").read_text(encoding="utf-8")

    # the row: on the Graphics tab, (Triforce, options_version 7) in its own group after the Upscaling group's rows (it shows
    # only with the game's own upscaler, row_shown), the window's filter gone
    order = ['"Upscaler", &opts.dlss', '"Render resolution", &opts.resolution', '"Sharpness", &opts.sharpness',
             'MENU_HEADING(TAB_GRAPHICS, "Anti-aliasing")', '"Anti-aliasing", &opts.aa']
    at = [menu.find(row) for row in order]
    check(-1 not in at and at == sorted(at), "vk_menu.cpp: the Anti-aliasing row is not in its group after the Upscaling group")
    check(menu.count('"Anti-aliasing", &opts.aa') == 1, "vk_menu.cpp: more than one Anti-aliasing row")
    check("(rows[i].value != &opts.aa || up == ur::GAME)" in definition(menu, "bool row_shown"),
          "vk_menu.cpp: the Anti-aliasing row shows while DLSS or FSR smooths the edges itself")
    entry = block(menu, menu.rindex("{TAB_GRAPHICS,", 0, at[4]))
    check(re.search(r"\bint aa = 0;", header), "vk_menu.h: Anti-aliasing is not the game's own by default")
    # a press on a row with nothing to choose changes and saves nothing, and a Quality preset does not set the row
    check(definition(menu, "void set_row").split("{", 1)[1].lstrip().startswith("// (nothing else to choose") and
          "if (row_fixed(r.value)) return;" in definition(menu, "void set_row"), "set_row no longer leaves a fixed row alone")
    check("void apply_preset" not in menu, "a Quality preset is back, and could set the Anti-aliasing row")
    read = menu[menu.index("            for (Row& r : rows) {\n                if (!ini_row(r) || strcmp(line, ini_key(r))) continue;"):]
    read = block(read, 0)
    check("std::min(std::max(v, r.lo), r.hi)" in read, "menu_init no longer clamps a saved value to its row")
    check('if (ini_row(r)) put(ini_key(r), std::to_string(*r.value));' in definition(menu, "void save"),
          "save no longer writes every row as <key>=<number>")

    # the renderer's side: what is in effect is the row's rule, asked with what upscales as the menu's Upscaler row shows it
    mode = definition(aa, "int aa_mode")
    check("ar::chosen(opts.aa, env_choice())" in mode and "ar::effective(choice, upscaler_now(), temporal_before_bloom(), env_choice() >= 0)" in mode,
          "aa_mode does not take the row's choice (RX_VK_AA over it) through aa_rows.h's effective")
    check("frame_read.load(std::memory_order_acquire) == frame" in mode and "std::lock_guard lk(s.mu);" in mode,
          "aa_mode no longer takes the choice once a frame")
    check(re.sub(r"\s+", " ", definition(aa, "int upscaler_now")).endswith(
        "{ if (fsr3_wanted()) return fsr3_native() ? upscaler_rows::FSR3_AA : upscaler_rows::FSR3; return temporal_mode(); }"),
        "vk_aa.cpp's upscaler_now is not what the menu's Upscaler row shows")
    check(re.search(r"int upscaler\(\) \{ return fsr3_wanted\(\) \? \(int\)\(fsr3_native\(\) \? ur::FSR3_AA : ur::FSR3\) : temporal_mode\(\); \}", menu),
          "vk_menu.cpp's upscaler() changed: vk_aa.cpp's upscaler_now follows it")
    check('getenv("RX_VK_AA")' in aa and 'getenv("RX_VK_AA_KEEP")' in aa, "vk_aa.cpp no longer reads its two switches")
    # the game's own chosen and nothing left to free: a dispatch asks a comparison and nothing else
    check(re.sub(r"\s+", " ", definition(aa, "bool aa_asked")) ==
          "bool aa_asked() { return ar::chosen(opts.aa, env_choice()) != ar::GAME || engaged.load(std::memory_order_relaxed) || upscaler_asks(); }",
          "aa_asked is no longer the row's choice, whether anything is left to free and whether an upscaler leaves the SMAA out")
    asks = definition(aa, "bool upscaler_asks")
    check("ar::effective(ar::GAME, upscaler_now(), temporal_before_bloom(), env_choice() >= 0) != ar::GAME" in asks and
          "frame.load(std::memory_order_acquire) != now" in asks,
          "upscaler_asks does not ask aa_rows.h's effective once a frame whether an upscaler leaves the game's SMAA out")
    leave = definition(aa, "bool aa_leave_out")
    check(leave.split("{", 1)[1].lstrip().startswith("if (!aa_asked()) return false;") and
          "ar::leave_out(ar::role(key), mode, last_replaced.load(std::memory_order_acquire), keep_passes())" in leave,
          "aa_leave_out does not answer false at once for the game's own, or not by aa_rows.h's leave_out")
    begun = definition(aa, "void frame_begun")
    check("s.track.begin(frame, want)" in begun and "last_replaced.store(s.track.last_replaced" in begun and
          "state_now.store(s.track.state" in begun, "frame_begun does not keep the passes' state with aa_rows.h's Track")
    composed = definition(aa, "void aa_composed")
    check("s.track.composed(texture_frame_no(), replaced);" in composed and "last_replaced.store(s.track.last_replaced" in composed,
          "aa_composed does not note the composition with aa_rows.h's Track")

    # the dispatch: the edge and weight passes are asked about before the program's pipeline and descriptors are made, the
    # composition in the stand-ins' place, after the descriptors (its views) and the barrier that orders it
    dispatch = definition(draw, "void dispatch")
    places = [dispatch.find(s) for s in ("if (!cp || !cp->ready.load(std::memory_order_acquire) || cp->failed) SKIP_HERE();",
                                         "if (aa_leave_out(cp->key)) return;", "build_descriptors(s, p);",
                                         'full_barrier(cmd, "barrier (dispatch)");', "if (taau_stand_in(cmd, s, p)) return void(st_dispatches++);",
                                         "if (aa_stand_in(cmd, s, p)) return void(st_dispatches++);", "if (!p.compute_pipeline) SKIP_HERE();",
                                         "vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.compute_pipeline);")]
    check(-1 not in places and places == sorted(places), "dispatch no longer asks aa_leave_out and aa_stand_in where it did")
    stand = definition(draw, "bool aa_stand_in")
    check(stand.split("{", 1)[1].lstrip().startswith("if (aa_rows::role(p.key) != aa_rows::COMPOSE || !aa_asked()) return false;"),
          "aa_stand_in does not answer false at once for another program, or for the game's own chosen")
    for need in ("const int mode = aa_mode();", "if (mode == aa_rows::GAME || p.stages.empty()) return false;",
                 "aa_rows::compose_ends(textures, ntex, images, nimg)", "if (ends.why) return aa_composed(false, ends.why), false;",
                 "VK_FORMAT_B10G11R11_UFLOAT_PACK32 ? aa_rows::R11", "VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 ? aa_rows::E5 : aa_rows::OTHER",
                 "textures[ntex++].integer = d.is_integer;", "images[nimg].written = d.is_written;",
                 "d.is_integer || image_format(d.format) == VK_FORMAT_R32_UINT",
                 "ViewUse::Storage, VK_FORMAT_R32_UINT, &out, &level", "aa_record(cmd, mode, texture_views[ends.src], dst, out->width, out->height)",
                 "if (done) descriptors_disturbed();"):
        check(need in stand, "aa_stand_in no longer has: " + need)
    check(stand.count("aa_composed(") >= 5 and "return done;" in stand, "aa_stand_in does not say what became of each composition")

    # the docs
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    check("| Anti-aliasing | Game SMAA, Off, FXAA, SMAA Ultra" in readme, "readme.md's settings table has no Anti-aliasing row")
    guide = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    check("| `anti-aliasing` | 0 the game's own SMAA, 1 Off, 2 FXAA, 3 SMAA Ultra |" in guide,
          "docs/development.md's options table has no anti-aliasing")
    check("| `RX_VK_AA=<0..3>`, `RX_VK_AA_KEEP=1` |" in guide, "docs/development.md's switches table has no RX_VK_AA")

    with tempfile.TemporaryDirectory(prefix="aa-row-") as tmp:
        work = Path(tmp)
        src, exe = work / "rule.cpp", work / "rule.exe"
        src.write_text(RULE.replace("@ROOT@", ROOT.as_posix()), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        fixture = MENU.replace("@ROOT@", ROOT.as_posix())
        for mark, text in (("@OPTIONS@", re.search(r"struct Options \{.*?\n\};", header, re.S).group(0)),
                           ("@ROW@", block(menu, menu.index("struct Row {")) + ";"),
                           ("@TEXT@", definition(menu, "const char* tx_aa")),
                           ("@HINT@", definition(menu, "const char* hn_aa")),
                           ("@FIXED@", definition(menu, "bool row_fixed")),
                           ("@KEY@", definition(menu, "const char* ini_key")),
                           ("@INI_ROW@", "bool is_merged(const int*) { return false; }   /* ((Triforce, options_version 7) no merged row here */\n" + definition(menu, "bool ini_row")),
                           ("@ENTRY@", entry + ","),
                           ("@READ@", read)):
            fixture = fixture.replace(mark, text)
        src, exe = work / "menu.cpp", work / "menu.exe"
        src.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-missing-field-initializers",
                        str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the Anti-aliasing row, its saved line, its interplay with the Upscaler row, and the rule for the game's SMAA passes")


if __name__ == "__main__":
    main()
