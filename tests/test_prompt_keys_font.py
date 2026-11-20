"""The keyboard and mouse face's font, and the host's part in it, on a font written here (no game file).

nx2/prompt_keys.py game_font makes the game's icon font the keyboard and mouse face's; host/prompts.c points its icons at
the keys bound when it serves it (cmap4_bind, bind_keys). The stand-in font has a square at every code point the face
uses, and a few subroutines of its own, as the game's has.

- Every key's icon is in the font at U+F700 + its key code, as high as the stand-in's A and no wider than WIDE of it, its
  label inside the cap; the blank cap at U+F700; each Switch button and each of the left stick's directions shows its key
  out of the box.
- The icons of several keys (the left stick, the d-pad, their axes) are the keys' small caps where they should be, drawn
  through the subroutines; the right stick's icons are redrawn inside their boxes.
- The host's patch (the production functions, compiled): with other keys bound, the font the host makes shows exactly
  what a font built for those keys shows, for every code point that follows the controls: the first key bound, the
  second when the first is empty, the blank cap for a control with no key or a key with no icon. A keys.bin cut short, or
  one that names a place outside the font or one that is no call, changes nothing.
- A font with no subroutines of its own gets the icons of several keys drawn whole, and no patches.
- hint.bin's widths are by key code for this face: a word's cap needs room, a letter's none.

    python -I -B tests/test_prompt_keys_font.py [--cc gcc]
"""
import argparse
import io
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_MODULES = ["fontTools"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import prompt_assets  # noqa: E402
import prompt_keys as K  # noqa: E402

BOX, ADVANCE = (40, 20, 600, 580), 640   # (the stand-in's glyphs: a square, and its advance)
# the keys bound in the host's part, by control (first, second): A's first empty, B's changed, the movement keys on the
# arrows, T, none and the mouse's fourth button, the d-pad's up on a key with no icon; the rest out of the box
BOUND = {0: (0, ord("K")), 1: (ord("T"), 0), K.MOVE_UP: (0x26, 0), K.MOVE_LEFT: (ord("T"), ord("F")), K.MOVE_DOWN: (0, 0),
         K.MOVE_RIGHT: (0x05, 0), K.PAD_UP: (0xE8, 0)}


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


def stand_in(subrs=3):
    """a font like the game's icon font in what game_font reads: OpenType with CFF outlines, a square at each code point,
    `subrs` local subroutines"""
    from fontTools.cffLib import SubrsIndex
    from fontTools.fontBuilder import FontBuilder
    from fontTools.misc.psCharStrings import T2CharString
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    from fontTools.ttLib import TTFont
    codes = sorted(set(prompt_assets.SWITCH) | set(K.MOVES) | set(K.CLUSTERS) | set(K.CAMERA) | {0xE04A})
    names, charstrings, metrics, cmap = [".notdef"], {}, {".notdef": (ADVANCE, 0)}, {}
    for name in [".notdef"] + ["u%04X" % c for c in codes]:
        pen = T2CharStringPen(ADVANCE, None)
        pen.moveTo((BOX[0], BOX[1]))
        pen.lineTo((BOX[2], BOX[1]))
        pen.lineTo((BOX[2], BOX[3]))
        pen.lineTo((BOX[0], BOX[3]))
        pen.closePath()
        charstrings[name] = pen.getCharString()
        if name != ".notdef":
            names.append(name)
            metrics[name], cmap[int(name[1:], 16)] = (ADVANCE, BOX[0]), name
    fb = FontBuilder(1000, isTTF=False)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cmap)
    fb.setupCFF("StandIn", {"FullName": "Stand In"}, charstrings, {})
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=700, descent=-300)
    fb.setupNameTable({"familyName": "Stand In", "styleName": "Regular"})
    fb.setupOS2()
    fb.setupPost()
    if subrs:
        private = fb.font["CFF "].cff.topDictIndex[0].Private
        private.Subrs = SubrsIndex()
        for _ in range(subrs):
            private.Subrs.append(T2CharString(program=["return"]))
    out = io.BytesIO()
    fb.save(out)
    font = TTFont(io.BytesIO(out.getvalue()))
    got = len(getattr(font["CFF "].cff.topDictIndex[0].Private, "Subrs", None) or [])
    check(got == subrs, "the stand-in font has %d subroutines, not %d" % (got, subrs))
    return out.getvalue()


def read(font):
    from fontTools.ttLib import TTFont
    f = TTFont(io.BytesIO(font))
    return f, f.getBestCmap(), f.getGlyphSet()


def bounds(glyphs, name):
    from fontTools.pens.boundsPen import BoundsPen
    pen = BoundsPen(glyphs)
    glyphs[name].draw(pen)
    return pen.bounds


def outline(glyphs, name):
    from fontTools.pens.recordingPen import RecordingPen
    pen = RecordingPen()
    glyphs[name].draw(pen)
    return pen.value


def near(one, other):
    """two outlines the same but for a unit's rounding (a small cap drawn in its place, or drawn at the origin and moved: a
    coordinate ending in .5 rounds the other way)"""
    return len(one) == len(other) and all(
        op == op2 and len(pts) == len(pts2) and all(abs(p[0] - q[0]) <= 1 and abs(p[1] - q[1]) <= 1 for p, q in zip(pts, pts2))
        for (op, pts), (op2, pts2) in zip(one, other))


def cluster_box(cells):
    """where game_font puts an icon of several keys in the stand-in's box"""
    step = K.MINI + K.MINI_GAP
    cols, rows = max(c for _, c, _ in cells) + 1, max(r for _, _, r in cells) + 1
    w, h = cols * step - K.MINI_GAP, rows * step - K.MINI_GAP
    over = max(0, w - (BOX[2] - BOX[0]))
    x0, y0 = round((BOX[0] + BOX[2] + over) / 2 - w / 2), round((BOX[1] + BOX[3]) / 2 - h / 2)
    return (x0, y0, x0 + w, y0 + h), ADVANCE + round(over)


def first_keys(bound):
    """the key each control shows with these keys bound: the first, else the second; none, or one with no icon: 0"""
    keys = list(K.DEFAULT)
    for control, (a, b) in bound.items():
        k = a or b
        keys[control] = k if k in K.KEYS else 0
    return keys


def host_program(folder, cc, bound=BOUND):
    """host/prompts.c's own functions that put the keys bound into a font (bind_font: cmap4_bind; bind_keys), compiled
    with a table of keys in the controls' place: <exe> <font> <keys.bin> <out> writes the font as the host would serve
    it and says how many buttons and how many calls it wrote; a fourth word: bind_keys alone"""
    src = (ROOT / "host" / "prompts.c").read_text(encoding="utf-8")
    harness = "\n".join([
        "#include <stdint.h>", "#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>", '#include "gpu/vk/vk_api.h"',
        "#define PAD_BASE 0xF700",
        "static int hos_binding(int set, int button, int slot) {",
        "    static const unsigned char out_of_the_box[HOS_KEY_N] = {%s};" % ", ".join(map(str, K.DEFAULT)),
        "    if (set != HOS_BIND_KEY || button < 0 || button >= HOS_KEY_N || slot < 0 || slot > 1) return set == HOS_BIND_KEY ? 0 : -1;",
        *("    if (button == %d) return slot ? %d : %d;" % (c, b, a_) for c, (a_, b) in bound.items()),
        "    return slot ? 0 : out_of_the_box[button];",
        "}",
        re.search(r"static const uint16_t switch_code\[HOS_SW_N\] = \{[^}]*\};", src).group(0),
        re.search(r"static uint32_t rd32le\(.*", src).group(0),
        re.search(r"static unsigned rd16\(.*", src).group(0),
        re.search(r"static uint32_t rd32\(.*", src).group(0),
        re.search(r"static void wr16\(.*", src).group(0),
        block(src, "static unsigned cmap4_glyph("),
        block(src, "static int bound_input("),
        block(src, "static int cmap4_bind("),
        block(src, "static int bind_keys("),
        block(src, "static int bind_font("),
        "static unsigned char font[1 << 20], keys[1 << 16];",
        "int main(int argc, char **argv) {",
        "    FILE *f = fopen(argv[1], \"rb\");",
        "    const size_t n = fread(font, 1, sizeof font, f);",
        "    fclose(f);",
        "    f = fopen(argv[2], \"rb\");",
        "    const size_t kn = fread(keys, 1, sizeof keys, f);",
        "    fclose(f);",
        "    const int buttons = argc > 4 ? 0 : bind_font(font, n, HOS_PROMPT_KEYBOARD);",
        "    printf(\"%d %d\\n\", buttons, bind_keys(font, n, keys, kn));",
        "    f = fopen(argv[3], \"wb\");",
        "    fwrite(font, 1, n, f);",
        "    fclose(f);",
        "    return 0;",
        "}", ""])
    c, exe = folder / "keys.c", folder / "keys"
    c.write_text(harness, encoding="utf-8")
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), str(c), "-o", str(exe)], check=True)
    return exe


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    base = prompt_assets.PAD_BASE
    game = stand_in()
    font, binds = K.game_font(game)
    f, cmap, glyphs = read(font)

    # the keys' icons
    side = BOX[3] - BOX[1]
    for vk in [0] + sorted(K.KEYS):
        check(cmap.get(base + vk) == "key_%d" % vk, "key %d has no icon at U+%04X" % (vk, base + vk))
        b = bounds(glyphs, "key_%d" % vk)
        check(b is not None and abs(b[3] - b[1] - side) <= 1, "key %d's icon is %s high, not %d" % (vk, b and b[3] - b[1], side))
        check(b[2] - b[0] <= side * prompt_assets.WIDE + 1, "key %d's icon is %d wide" % (vk, b[2] - b[0]))
        check(f["hmtx"]["key_%d" % vk][0] >= ADVANCE and b[2] <= f["hmtx"]["key_%d" % vk][0],
              "key %d's icon runs past its advance" % vk)
    drawn = {vk: repr(outline(glyphs, "key_%d" % vk)) for vk in [0] + sorted(K.KEYS)}
    check(len(set(drawn.values())) == len(drawn), "two keys have the same icon")
    check(all(len(d) > len(drawn[0]) for vk, d in drawn.items() if vk and vk not in K.MICE), "a key's cap has no label")
    for control, code in list(enumerate(prompt_assets.SWITCH)) + [(c, code) for code, c in K.MOVES.items()]:
        check(cmap[code] == "key_%d" % K.DEFAULT[control], "U+%04X shows %s, not key %d" % (code, cmap[code], K.DEFAULT[control]))

    # the icons of several keys, and the camera's
    check(len(binds["patches"]) == sum(len(cells) for cells in K.CLUSTERS.values()) and len(binds["calls"]) == len(K.KEYS) + 1,
          "%d patches and %d calls" % (len(binds["patches"]), len(binds["calls"])))
    check(all(len(c) == 2 and 247 <= c[0] <= 250 for c in binds["calls"].values()), "a call is not two bytes of a number 108 to 1131")
    for code, cells in K.CLUSTERS.items():
        want, advance = cluster_box(cells)
        got = bounds(glyphs, cmap[code])
        check(got == want and f["hmtx"][cmap[code]][0] == advance,
              "U+%04X: its keys fill %s with advance %d, not %s with %d" % (code, got, f["hmtx"][cmap[code]][0], want, advance))
        contours = sum(1 for op, _ in outline(glyphs, cmap[code]) if op == "moveTo")
        check(contours >= 3 * len(cells), "U+%04X has %d contours for %d keys" % (code, contours, len(cells)))
    for code in K.CAMERA:
        b = bounds(glyphs, cmap[code])
        check(b is not None and b[0] >= BOX[0] - 1 and b[1] >= BOX[1] - 1 and b[2] <= BOX[2] + 1 and b[3] <= BOX[3] + 1 and
              b != BOX and f["hmtx"][cmap[code]][0] == ADVANCE, "U+%04X: the mouse is at %s" % (code, b))
    check(outline(glyphs, cmap[0xE04A]) == outline(read(game)[2], "uE04A"), "an icon that is no control's was redrawn")

    # a small cap begins and ends where the glyph that calls it expects, whatever the key
    from fontTools.pens.recordingPen import RecordingPen
    letters = K.letters_once()
    for vk in [0] + sorted(K.KEYS):
        pen = RecordingPen()
        K.mini(pen, letters, vk)
        check(pen.value[0] == ("moveTo", ((K.MINI_R, 0),)) and pen.value[-2] == ("lineTo", (K.MINI_MARK[-1],)) and
              pen.value[-1][0] == "closePath", "key %d's small cap begins at %s and ends at %s" % (vk, pen.value[0], pen.value[-2]))

    # the host's part
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        exe = host_program(tmp, a.cc)
        (tmp / "font.otf").write_bytes(font)
        K.write_binds(tmp / "keys.bin", binds)
        said = subprocess.run([str(exe), str(tmp / "font.otf"), str(tmp / "keys.bin"), str(tmp / "out.otf")], check=True,
                              capture_output=True, text=True).stdout.split()
        check(said == [str(len(prompt_assets.SWITCH) + len(K.MOVES)), str(len(binds["patches"]))],
              "the host bound %s buttons and directions and wrote %s calls" % tuple(said))
        _, got_cmap, got = read((tmp / "out.otf").read_bytes())
        _, want_cmap, want = read(K.game_font(game, first_keys(BOUND))[0])
        for code in list(prompt_assets.SWITCH) + list(K.MOVES):
            check(got_cmap[code] == want_cmap[code], "U+%04X: the host shows %s, a font for these keys %s"
                  % (code, got_cmap[code], want_cmap[code]))
        for code in K.CLUSTERS:
            check(outline(got, got_cmap[code]) == outline(want, want_cmap[code]),
                  "U+%04X: the host's keys are not the ones a font built for these keys shows" % code)
        check(got_cmap[prompt_assets.SWITCH[0]] == "key_%d" % ord("K") and got_cmap[0xE050] == "key_0" and
              got_cmap[0xE058] == "key_5", "the second key, no key and a mouse button: %s %s %s"
              % (got_cmap[prompt_assets.SWITCH[0]], got_cmap[0xE050], got_cmap[0xE058]))
        check(outline(got, got_cmap[0xE05E]) != outline(glyphs, cmap[0xE05E]), "the left stick's keys did not change")
        # (files the host must leave the font alone for: cut short; a place past the font's end; a place that is no call)
        whole = (tmp / "keys.bin").read_bytes()
        count = len(binds["patches"])
        far = whole[:8] + struct.pack("<I", len(font) - 1) + whole[12:]
        off = binds["patches"][0][0]
        elsewhere = whole[:8] + struct.pack("<I", off + 2) + whole[12:]
        check(not 247 <= font[off + 2] <= 250, "the stand-in's bytes after a call look like a call")
        for name, data, written in (("short", whole[:11], 0), ("no keys", whole[:12 + 8 * count], 0),
                                    ("cut calls", whole[:-1], 0), ("far", far, count - 1), ("elsewhere", elsewhere, count - 1)):
            (tmp / "bad.bin").write_bytes(data)
            said = subprocess.run([str(exe), str(tmp / "font.otf"), str(tmp / "bad.bin"), str(tmp / "bad.otf"), "keys"],
                                  check=True, capture_output=True, text=True).stdout.split()
            check(said == ["0", str(written)], "a keys.bin that is %s: %s calls written, not %d" % (name, said[1], written))
            if not written:
                check((tmp / "bad.otf").read_bytes() == font, "a keys.bin that is %s changed the font" % name)

    # a font with no subroutines of its own: the keys out of the box, drawn whole
    plain, none = K.game_font(stand_in(0))
    _, plain_cmap, plain_glyphs = read(plain)
    check(none["patches"] == [] and none["calls"] == {}, "patches for a font that has no subroutines")
    for code, cells in K.CLUSTERS.items():
        check(bounds(plain_glyphs, plain_cmap[code]) == cluster_box(cells)[0], "U+%04X drawn whole is elsewhere" % code)
        check(near(outline(plain_glyphs, plain_cmap[code]), outline(glyphs, cmap[code])),
              "U+%04X drawn whole is not what the subroutines draw" % code)

    # the ocarina hint's widths, by key code
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "hint.bin"
        prompt_assets.write_hint(path, font, [(64, 0, 1.5)], K.hint_names())
        b = path.read_bytes()
        check(b[:4] == b"HPHN" and struct.unpack_from("<I", b, 4)[0] == 256 and len(b) == 8 + 4 * 256 + 4 + 12,
              "hint.bin is not 256 widths and the panes")
        extra = struct.unpack_from("<256f", b, 8)
        check(extra[ord("W")] == 0 and extra[0] == 0 and extra[0x20] > 0 and extra[0xFF] == 0,
              "hint.bin: W %s, Space %s" % (extra[ord("W")], extra[0x20]))
        prompt_assets.write_hint(path, prompt_assets.face_font(game, "switch"), [(64, 0, 1.5)])
        check(struct.unpack_from("<I", path.read_bytes(), 4)[0] == len(prompt_assets.PADS), "a controller's hint.bin changed")
    print("ok: %d keys' icons, %d icons of several keys through %d calls, and the host's keys are a built font's"
          % (len(K.KEYS), len(K.CLUSTERS), len(binds["patches"])))


if __name__ == "__main__":
    main()
