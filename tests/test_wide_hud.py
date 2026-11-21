"""The HUD on a picture wider than 16:9 (host/oot_wide_hud.h, host/oot_wide_hud.c, nx2/prompt_assets.py HUD_SIDE_PANES).

With the Insert menu's Ultrawide HUD row on Screen edges the hearts and the item slot sit in the picture's top corners
and the small map at its right edge; on Middle everything keeps the middle 16:9, as before. The rules checked here,
without the game:

- A layout in the middle: its projection times the squeeze, nothing moved. A kept one (a fade, a menu's background):
  untouched. A 16:9 picture (squeeze 1) or no squeeze: untouched.
- A layout at an edge (the lab's switch): a point at the 16:9 grid's right edge lands on the picture's right edge, one
  at its left edge on the picture's left edge, and the layout keeps its shape.
- A pane moved in a layout (the hearts', the item slot's, the small map's) goes as far as a whole layout moved by its
  projection: a corner pane lands on the picture's corner through the middle projection.
- prompts/wide.bin: one entry per pane and copy of each layout, and one per animation key that puts a pane at the small
  map's place (the keys at 0 and other curves are left); a layout that is not the one known gives none.

python -I -B tests/test_wide_hud.py
"""
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import prompt_assets  # noqa: E402

FIXTURE = r'''
#include <math.h>
#include <stdio.h>
#include "oot_wide_hud.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { fails++; printf("FAIL line %d: %s\n", __LINE__, what); } } while (0)
static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

/* clip x of the layout's point x through the projection (first column's m00, the move m03) */
static float clip(const float first[4], float move, float x) { return first[0] * x + move; }

int main(void) {
    const float m00 = 2.0f / 3840.0f;
    const float q = (16.0f / 9.0f) / (21.0f / 9.0f);   /* a 21:9 picture */
    float first[4], move;

    /* the middle */
    first[0] = m00, first[1] = first[2] = first[3] = 0, move = 0;
    oot_hud_row(first, &move, q, 0, OOT_HUD_MIDDLE);
    CHECK(near(clip(first, move, 1920.0f), q) && near(clip(first, move, -1920.0f), -q) && move == 0.0f, "the middle 16:9");

    /* kept, and a 16:9 picture */
    first[0] = m00, move = 0;
    oot_hud_row(first, &move, q, 1, OOT_HUD_RIGHT);
    CHECK(first[0] == m00 && move == 0.0f, "a kept layout is untouched");
    oot_hud_row(first, &move, 1.0f, 0, OOT_HUD_RIGHT);
    CHECK(first[0] == m00 && move == 0.0f, "a 16:9 picture is untouched");
    oot_hud_row(first, &move, 0.0f, 0, OOT_HUD_LEFT);
    CHECK(first[0] == m00 && move == 0.0f, "no squeeze: untouched");

    /* the edges */
    first[0] = m00, move = 0;
    oot_hud_row(first, &move, q, 0, OOT_HUD_RIGHT);
    CHECK(near(clip(first, move, 1920.0f), 1.0f), "the grid's right edge on the picture's right edge");
    CHECK(near(clip(first, move, 1920.0f) - clip(first, move, -1920.0f), 2.0f * q), "its shape kept");
    first[0] = m00, move = 0;
    oot_hud_row(first, &move, q, 0, OOT_HUD_LEFT);
    CHECK(near(clip(first, move, -1920.0f), -1.0f), "the grid's left edge on the picture's left edge");

    /* a pane moved in the layout: as far as the projection would move it */
    const float out = oot_hud_out(q);
    first[0] = m00, move = 0;
    oot_hud_row(first, &move, q, 0, OOT_HUD_MIDDLE);
    CHECK(near(clip(first, move, 1920.0f + out), 1.0f), "the right corner pane on the picture's corner");
    CHECK(near(clip(first, move, -1920.0f - out), -1.0f), "the left corner pane on the picture's corner");
    CHECK(near(clip(first, move, 1626.0f + out) - 1.0f, clip(first, move, 1626.0f) - q), "a pane beside it keeps its distance to the edge");
    CHECK(oot_hud_out(1.0f) == 0.0f && oot_hud_out(0.0f) == 0.0f && oot_hud_out(1.5f) == 0.0f, "nothing on a 16:9 or narrower picture");
    CHECK(near(oot_hud_out((16.0f / 9.0f) / (32.0f / 9.0f)), 1920.0f), "32:9: a whole half grid further out");

    /* the classes */
    CHECK(oot_hud_keep_bit(0x84c04fb0ULL) == 2u && oot_hud_keep_bit(0x84c08300ULL) == 0u, "the menus' background is kept, the map's screen is not");

    printf(fails ? "wide hud: FAILED\n" : "wide hud: all checks passed\n");
    return fails != 0;
}
'''


def layout(name, panes, curves=()):
    """a made-up layout as nx2/prompt_assets.py reads one: "BFFH", the names' and the records' tables' offsets at +0x118 and
    +0x120, the count at +0x130, the name before "RootPane" among the texts; panes: [(name, x)]; curves: [(value past the
    last key, [(value, slope)])], each an array as layout_keys reads one, 8-aligned, the first value 8 bytes before it"""
    names = ["RootPane"] + [n for n, _ in panes]
    xs = [0.0] + [x for _, x in panes]
    n = len(names)
    head = 0x140
    names_at, recs_at = head, head + 8 * n
    rec0 = recs_at + 8 * n
    texts_at = rec0 + 0x44 * n
    texts = b"\0" + name.encode() + b"\0"
    offs = []
    for t in names:
        offs.append(texts_at + len(texts))
        texts += t.encode() + b"\0"
    b = bytearray(texts_at) + texts
    b[0:4] = b"BFFH"
    struct.pack_into("<QQ", b, 0x118, names_at, recs_at)
    struct.pack_into("<I", b, 0x130, n)
    for k in range(n):
        struct.pack_into("<Q", b, names_at + 8 * k, offs[k])
        struct.pack_into("<Q", b, recs_at + 8 * k, rec0 + 0x44 * k)
        struct.pack_into("<f", b, rec0 + 0x44 * k + 0x1c, xs[k])
    for last, keys in curves:
        b += bytes(-len(b) % 8)
        b += struct.pack("<ff", last, 0.0)
        b += struct.pack("<QII", len(b) + 16, 2 * len(keys), 4)
        for value, slope in keys:
            b += struct.pack("<ff", value, slope)
    return bytes(b)


GOOD = [("N_VRS_00", 1626.0), ("N_ScaleL_00", -1920.0), ("N_ScaleR_00", 1920.0), ("N_SubItemBaseFishing_00", 1270.0)]
MAP = [("N_VRS_00", 1536.0), ("N_MiniMapCap_00", 1470.0), ("N_LocalMap_00", 0.0), ("N_ItemIcon_00", 1666.0),
       ("N_Field_01", 1490.0), ("N_Dungeon_01", 1536.0), ("N_MiniMapReplace_00", 1470.0), ("P_MiniMapFrame_00", 1536.0),
       ("S_Floor_00", 1706.0), ("P_CapDungeon_00", 1536.0), ("N_PatternFieldMini_01", 0.0)]
# (the icons' layers: at 0 for the whole-screen map's patterns, at the small map's place for its own; then two curves that
# are not places: one with a slope, one with another value)
MAP_CURVES = [(0.0, [(0.0, 0.0), (0.0, 0.0), (1490.0, 0.0), (1490.0, 0.0), (0.0, 0.0)]),
              (1536.0, [(0.0, 0.0), (1536.0, 0.0), (1536.0, 0.0)]),
              (0.0, [(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)]),
              (0.0, [(0.0, 0.0), (0.5, 0.0), (1.0, 0.0)])]


def archive(status=None, the_map=None):
    one = layout("PlayerStatus_00", GOOD) if status is None else status
    two = layout("MapMenu_00", MAP, MAP_CURVES) if the_map is None else the_map
    return b"SARC" + bytes(60) + layout("Other_00", [("N_ScaleL_00", -1920.0)]) + one + two + one + two


class WideHudTest(unittest.TestCase):
    def test_rules(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        with tempfile.TemporaryDirectory(prefix="widehud-") as d:
            tmp = Path(d)
            (tmp / "t.c").write_text(FIXTURE, encoding="utf-8")
            binary = tmp / ("t.exe" if sys.platform == "win32" else "t")
            build = subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-O1", "-I", str(ROOT / "host"),
                                    str(tmp / "t.c"), "-o", str(binary), "-lm"], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("all checks passed", run.stdout)

    def test_table(self):
        blarc = archive()
        got = prompt_assets.wide_panes(blarc)
        # (the hearts' layout: four panes; the map's: eight panes, the two keys at 1490, the two at 1536 and that curve's
        # value past its last key; two copies of each)
        self.assertEqual(len(got), 2 * (4 + 8 + 2 + 2 + 1))
        for off, side, x in got:
            self.assertEqual(struct.unpack_from("<f", blarc, off)[0], x)
            self.assertNotEqual(x, 0.0)
        self.assertEqual({x for _, side, x in got if side < 0}, {-1920.0})
        self.assertEqual({x for _, side, x in got if side > 0}, {1920.0, 1626.0, 1270.0, 1536.0, 1470.0, 1490.0, 1706.0})
        first = blarc.index(layout("PlayerStatus_00", GOOD))
        self.assertTrue(all(off >= first for off, _, _ in got))        # (the other layout's pane of the same name: left)
        self.assertNotIn(1666.0, {x for _, _, x in got})                 # (the whole-screen map's own pane: left)
        self.assertEqual(len({off for off, _, _ in got}), len(got))
        with tempfile.TemporaryDirectory(prefix="widehud-") as d:
            path = Path(d) / "wide.bin"
            prompt_assets.write_wide(path, got)
            raw = path.read_bytes()
        self.assertEqual(raw[:4], b"HPWD")
        self.assertEqual(struct.unpack_from("<I", raw, 4)[0], len(got))
        self.assertEqual(len(raw), 8 + 12 * len(got))
        self.assertEqual(struct.unpack_from("<Ib3xf", raw, 8), got[0])

    def test_keys(self):
        lay = layout("MapMenu_00", MAP, MAP_CURVES)
        keys = prompt_assets.layout_keys(lay, {1490.0, 1536.0})
        self.assertEqual(sorted(struct.unpack_from("<f", lay, o)[0] for o in keys), [1490.0, 1490.0, 1536.0, 1536.0, 1536.0])
        self.assertEqual(prompt_assets.layout_keys(lay, {1.0, 2.0}), [])      # (a curve with a slope is no place)
        self.assertEqual(prompt_assets.layout_keys(lay, {1.0}), [])           # (nor one with another value among them)

    def test_unknown_layout(self):
        self.assertIsNone(prompt_assets.wide_panes(b"SARC" + bytes(60)))                                   # (no such layout)
        self.assertIsNone(prompt_assets.wide_panes(archive(status=layout("PlayerStatus_00", GOOD[:3]))))   # (a pane missing)
        moved = [(n, 1800.0 if n == "N_ScaleR_00" else x) for n, x in GOOD]
        self.assertIsNone(prompt_assets.wide_panes(archive(status=layout("PlayerStatus_00", moved))))      # (not at the corner)
        twice = layout("PlayerStatus_00", GOOD + [("N_Other_00", 1270.0)])
        self.assertIsNone(prompt_assets.wide_panes(archive(status=twice)))                                 # (a place held elsewhere)
        other = layout("MapMenu_00", MAP + [("N_Else_00", 1706.0)], MAP_CURVES)
        self.assertIsNone(prompt_assets.wide_panes(archive(the_map=other)))                                # (the same, the map's)
        ramp = layout("MapMenu_00", MAP, MAP_CURVES + [(0.0, [(0.0, 0.0), (1536.0, 3.0)])])
        self.assertIsNone(prompt_assets.wide_panes(archive(the_map=ramp)))                                 # (a key that moves there)
        self.assertIsNone(prompt_assets.wide_panes(archive(the_map=b"")))                                  # (no map layout)
        self.assertIsInstance(prompt_assets.WIDE, float)   # (the icons' own constant: a table of this file once took its name)

    def test_wired(self):
        host = (ROOT / "host/oot_wide_hud.c").read_text(encoding="utf-8")
        self.assertIn('hos_ini_option("ultrawide_hud", 0)', host)
        prompts = (ROOT / "host/prompts.c").read_text(encoding="utf-8")
        self.assertIn("hos_wide_hud_out()", prompts)
        wide = (ROOT / "nx2/oot_wide.py").read_text(encoding="utf-8")
        self.assertIn("hos_wide_hud_row(c, c->x[0] - 0x1b8, _row, &_tx);", wide)
        menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
        # ((Triforce, options_version 7) the Ultrawide group's HUD position, kept as ultrawide_hud=, which oot_wide_hud.c reads)
        self.assertIn('"HUD position", &opts.wide_hud, 0, 1, 1, tx_wide_hud,', menu)
        self.assertIn('{"HUD position", "ultrawide_hud"}', menu)


if __name__ == "__main__":
    unittest.main()
