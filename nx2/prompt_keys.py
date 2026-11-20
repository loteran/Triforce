"""(Triforce) The keyboard and mouse face of the button prompts: the game's button icons showing the key or the mouse button
each control is bound to (NX2navi's prompts = pc; nx2/prompt_assets.py builds it with the controllers' faces, host/prompts.c
serves it).

Nothing is read for it but a font the repository carries: a key is a cap drawn here, a rounded square (wider for a word) with
its label cut out of it in Libertinus Sans Bold (third_party/fonts/libertinus, SIL OFL: the font the menus fall back on); an
arrow key's label is an arrow; a mouse button is a mouse with that button filled; the camera is a mouse between arrows.

The controls can be changed (the Controls page), so the font has an icon for every key that can be bound, at U+F700 + its
Windows key code (KEYS: the keys of gpu/vk/vk_menu.cpp bindable_keys), and the host points each button's code point at the
icon of the first key bound to it when it serves the font, as it does with a pad's inputs. What the font's own code points
show:

    the Switch's buttons (prompt_assets.SWITCH)        the key of each (DEFAULT: host/nn_hid.c key_default's first keys)
    the left stick's four directions (MOVES)           the movement keys
    the left stick, its two axes, the d-pad and its    those keys side by side, as they lie on the keyboard (CLUSTERS)
    two axes
    the right stick and its directions (CAMERA)        the mouse, with arrows for the way it moves

An icon of several keys cannot be one glyph per choice of keys. It is a glyph that calls a subroutine per key, and each key's
small cap is such a subroutine (a local one, as the game's font already has: mini). <face>/keys.bin lists where those calls
are in the font and each key's call, and the host writes the calls of the keys bound: "HPKB", u32 count, then count times
u32 offset (in the font, without its 8-byte header), u8 control (HOS_KEY_*), u8 0 x3; then u32 256 and for each key code the
two bytes of its call (0 0: no such key, the blank cap's).

    python -I nx2/prompt_keys.py --out <dir>   (every icon as it is drawn, in one PNG, to look at; needs Pillow)
"""
import io
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import prompt_assets as P  # noqa: E402

LETTERS = os.path.join(os.path.dirname(HERE), "third_party", "fonts", "libertinus", "LibertinusSans-Bold.ttf")
BASE = P.PAD_BASE   # (a key's icon is at BASE + its key code, as a pad input's is at BASE + HOS_PAD_*)

# every key a control can be bound to, by its Windows key code, and its label: a line, two ("Num" small over the key), or a
# drawing ("@up" an arrow, "@mouse_l" the mouse with its left button filled). F1 is the menu's.
KEYS = {ord(c): c for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"}
KEYS.update({0x70 + n - 1: "F%d" % n for n in range(2, 13)})
KEYS.update({0x60 + n: "Num\n%d" % n for n in range(10)})
KEYS.update({
    0x20: "Space", 0x10: "Shift", 0x11: "Ctrl", 0x12: "Alt", 0x09: "Tab", 0x0D: "Enter", 0x08: "Bksp", 0x14: "Caps",
    0x26: "@up", 0x28: "@down", 0x25: "@left", 0x27: "@right", 0x24: "Home", 0x23: "End", 0x21: "PgUp", 0x22: "PgDn",
    0x6B: "Num\n+", 0x6D: "Num\n-", 0x6A: "Num\n*", 0x6F: "Num\n/", 0x6E: "Num\n.",
    0xBA: ";", 0xBB: "=", 0xBC: ",", 0xBD: "-", 0xBE: ".", 0xBF: "/", 0xC0: "`", 0xDB: "[", 0xDC: "\\", 0xDD: "]", 0xDE: "'",
    0x01: "@mouse_l", 0x02: "@mouse_r", 0x04: "@mouse_m", 0x05: "@mouse_4", 0x06: "@mouse_5"})
# the first key of each control out of the box, in HOS_KEY_* order (host/nn_hid.c key_default): the Switch's buttons A B X Y
# LS RS L R ZL ZR Plus Minus Left Up Right Down, then move up, down, left, right and camera up, down, left, right
DEFAULT = [0x45, 0x10, 0x20, 0x01, 0x11, 0x43, 0x51, 0x52, 0x5A, 0x58, 0x09, 0x4D, 0x31, 0x32, 0x33, 0x34, 0x57, 0x53, 0x41,
           0x44, 0x26, 0x28, 0x25, 0x27]
MOVE_UP, MOVE_DOWN, MOVE_LEFT, MOVE_RIGHT = 16, 17, 18, 19
PAD_LEFT, PAD_UP, PAD_RIGHT, PAD_DOWN = 12, 13, 14, 15
# the left stick pushed one way: that movement key (the font's code point: the control)
MOVES = {0xE052: MOVE_UP, 0xE050: MOVE_DOWN, 0xE05A: MOVE_LEFT, 0xE058: MOVE_RIGHT}
# the icons of several keys: each key's control and its place (column, row from the top)
CLUSTERS = {
    0xE05E: [(MOVE_UP, 1, 0), (MOVE_LEFT, 0, 1), (MOVE_DOWN, 1, 1), (MOVE_RIGHT, 2, 1)],   # (the left stick)
    0xE05C: [(MOVE_UP, 0, 0), (MOVE_DOWN, 0, 1)], 0xE054: [(MOVE_LEFT, 0, 0), (MOVE_RIGHT, 1, 0)],
    0xE049: [(PAD_UP, 1, 0), (PAD_LEFT, 0, 1), (PAD_DOWN, 1, 1), (PAD_RIGHT, 2, 1)],       # (the d-pad)
    0xE06A: [(PAD_UP, 1, 0), (PAD_LEFT, 0, 1), (PAD_DOWN, 1, 1), (PAD_RIGHT, 2, 1)],
    0xE064: [(PAD_UP, 0, 0), (PAD_DOWN, 0, 1)], 0xE065: [(PAD_LEFT, 0, 0), (PAD_RIGHT, 1, 0)]}
# the right stick is the camera, which the mouse turns: the mouse and the ways it moves (the camera's keys, a second way
# to turn it, do not show)
CAMERA = {0xE05F: "xy", 0xE068: "xy", 0xE053: "y", 0xE051: "y", 0xE05D: "y", 0xE059: "x", 0xE05B: "x", 0xE055: "x"}
MICE = {vk for vk, label in KEYS.items() if label.startswith("@mouse")}   # (their icons are no caps)

# ---- the drawings, in units of their own: a cap is H high, its corners R round, a word's as wide as the word up to
# P.WIDE caps; a mouse MOUSE wide. A contour that is ink runs counter-clockwise; a label is cut out of its cap by running
# the other way, which is the way Libertinus's outlines run (TrueType's)
H, R, MOUSE, STROKE = 1000, 180, 640, 80
KAPPA = 0.5523   # (a quarter circle's control points, of its radius)
MINI, MINI_R, MINI_GAP = 290, 52, 20   # (a cap in an icon of several keys, the game's font's units: two rows fill an icon)
MINI_MARK = [(16, 30), (30, 30), (30, 44), (16, 44)]   # (a small cap's last contour, which ends at its last point: see mini)


def rounded(pen, x0, y0, x1, y1, r):
    """a rounded rectangle, counter-clockwise from (x0 + r, y0) (a side of no length, a round end's, is left out)"""
    k = r * (1 - KAPPA)
    at = (x0 + r, y0)
    pen.moveTo(at)
    for line, c1, c2, to in ((x1 - r, y0), (x1 - k, y0), (x1, y0 + k), (x1, y0 + r)), \
                            ((x1, y1 - r), (x1, y1 - k), (x1 - k, y1), (x1 - r, y1)), \
                            ((x0 + r, y1), (x0 + k, y1), (x0, y1 - k), (x0, y1 - r)), \
                            ((x0, y0 + r), (x0, y0 + k), (x0 + k, y0), (x0 + r, y0)):
        if line != at:
            pen.lineTo(line)
        pen.curveTo(c1, c2, to)
        at = to
    pen.closePath()


def polygon(pen, points):
    pen.moveTo(points[0])
    for p in points[1:]:
        pen.lineTo(p)
    pen.closePath()


def arc(pen, cx, cy, r, a0, a1):
    """a circle's arc from angle a0 to a1 (degrees, counter-clockwise), from the pen's place at its start"""
    steps = max(1, math.ceil(abs(a1 - a0) / 90))
    for i in range(steps):
        t0, t1 = (math.radians(a0 + (a1 - a0) * j / steps) for j in (i, i + 1))
        k = 4 / 3 * math.tan((t1 - t0) / 4) * r
        pen.curveTo((cx + r * math.cos(t0) - k * math.sin(t0), cy + r * math.sin(t0) + k * math.cos(t0)),
                    (cx + r * math.cos(t1) + k * math.sin(t1), cy + r * math.sin(t1) - k * math.cos(t1)),
                    (cx + r * math.cos(t1), cy + r * math.sin(t1)))


def arrow(pen, cx, cy, size, way):
    """an arrow (counter-clockwise) pointing up, left, down or right, size long, about (cx, cy)"""
    a, b = size / 2, size / 6
    up = [(0, a), (-a, 0), (-b, 0), (-b, -a), (b, -a), (b, 0), (a, 0)]
    turn = {"up": (1, 0), "left": (0, 1), "down": (-1, 0), "right": (0, -1)}[way]   # (cos, sin of a quarter turn each)
    polygon(pen, [(cx + x * turn[0] - y * turn[1], cy + x * turn[1] + y * turn[0]) for x, y in up])


class Letters:
    """Libertinus Sans Bold's outlines, for the labels"""

    def __init__(self):
        from fontTools.ttLib import TTFont
        font = TTFont(LETTERS)
        self.glyphs, self.cmap, self.advance, self.cap = font.getGlyphSet(), font.getBestCmap(), font["hmtx"], font["OS/2"].sCapHeight

    def ink(self, text):
        """a line's ink, from its left to its right, in the font's units"""
        from fontTools.pens.boundsPen import BoundsPen
        x, lo, hi = 0, None, None
        for ch in text:
            g = self.cmap[ord(ch)]
            b = BoundsPen(self.glyphs)
            self.glyphs[g].draw(b)
            if b.bounds:
                lo = x + b.bounds[0] if lo is None else min(lo, x + b.bounds[0])
                hi = x + b.bounds[2] if hi is None else max(hi, x + b.bounds[2])
            x += self.advance[g][0]
        return lo or 0, hi or 0

    def draw(self, pen, text, scale, x, y):
        """a line from x on the baseline y, as the font's outlines run (clockwise); a glyph made of others as outlines too"""
        from fontTools.pens.recordingPen import DecomposingRecordingPen
        from fontTools.pens.transformPen import TransformPen
        for ch in text:
            g = self.cmap[ord(ch)]
            outline = DecomposingRecordingPen(self.glyphs)
            self.glyphs[g].draw(outline)
            outline.replay(TransformPen(pen, (scale, 0, 0, scale, x, y)))
            x += self.advance[g][0] * scale


def lines_of(letters, label, h, room):
    """a label's lines in a cap h high with room for the ink: [(text, scale, the ink's width, baseline from the cap's middle)].
    One letter is half the cap high, a word less, and what is wider than the room is made smaller (one letter has a square
    cap's room: a W's cap is no wider than an I's); of two lines the upper is the smaller ("Num")"""
    def line(text, cap_height, middle):
        s = cap_height / letters.cap
        lo, hi = letters.ink(text)
        fits = min(room, h * 0.64) if len(text) == 1 else room
        if (hi - lo) * s > fits:
            s = fits / (hi - lo)
        return text, s, (hi - lo) * s, middle - s * letters.cap / 2
    lines = label.split("\n")
    if len(lines) == 1:
        return [line(lines[0], h * (0.5 if len(lines[0]) == 1 else 0.4), 0)]
    return [line(lines[0], h * 0.2, h * 0.24), line(lines[1], h * 0.38, -h * 0.13)]


def key_width(letters, vk):
    """how wide a key's icon is (it is H high)"""
    label = KEYS.get(vk, "")
    if vk in MICE:
        return MOUSE
    if not label or label.startswith("@"):
        return H
    widest = max(w for _, _, w, _ in lines_of(letters, label, H, H * (P.WIDE - 0.3)))
    return int(min(H * P.WIDE, max(H, widest + H * 0.36)))


def cap(pen, letters, label, w, h, r, room):
    """a cap w by h from (0, 0), its label cut out of it"""
    from fontTools.pens.reverseContourPen import ReverseContourPen
    from fontTools.pens.transformPen import TransformPen
    rounded(pen, 0, 0, w, h, r)
    if label.startswith("@mouse"):   # (in an icon of several keys: a small mouse cut out of the cap, its lines 30 thick)
        s = h * 0.72 / H
        mouse(TransformPen(ReverseContourPen(pen), (s, 0, 0, s, w / 2 - s * MOUSE / 2, h * 0.14)), label[7:], 30 / s)
    elif label.startswith("@"):
        arrow(ReverseContourPen(pen), w / 2, h / 2, h * 0.5, label[1:])
    elif label:
        for text, s, width, base in lines_of(letters, label, h, room):
            lo, _ = letters.ink(text)
            letters.draw(pen, text, s, w / 2 - width / 2 - lo * s, h / 2 + base)


def mouse(pen, button, stroke=STROKE):
    """a mouse MOUSE by H from (0, 0): its outline, and the button filled ("l", "r", "m"; "4", "5": the number on its palm;
    "": none). No part touches another: the same contours cut a small mouse out of a cap"""
    from fontTools.pens.reverseContourPen import ReverseContourPen
    from fontTools.pens.transformPen import TransformPen
    half, gap = MOUSE / 2, max(40, stroke / 2)
    rounded(pen, 0, 0, MOUSE, H, half)
    rounded(ReverseContourPen(pen), stroke, stroke, MOUSE - stroke, H - stroke, half - stroke)
    top, r = H - half, half - stroke - gap   # (the buttons: the upper end's inside, a gap off the outline and each other)
    if button in ("l", "r"):
        side = pen if button == "l" else TransformPen(ReverseContourPen(pen), (-1, 0, 0, 1, MOUSE, 0))
        x = half - gap / 2
        a = math.degrees(math.acos((x - half) / r))
        side.moveTo((half - r, top - 120))
        side.lineTo((x, top - 120))
        side.lineTo((x, top + r * math.sin(math.radians(a))))
        arc(side, half, top, r, a, 180)
        side.closePath()
    elif button == "m":   # (the wheel)
        wheel = min(55, 0.3 * r)
        rounded(pen, half - wheel, top - 0.35 * r, half + wheel, top + 0.6 * r, wheel)
    elif button:   # (the side buttons have no place in this picture: their number)
        cap_height = (half - stroke - 20) / 0.625
        s = cap_height / letters_once().cap
        lo, hi = letters_once().ink(button)
        letters_once().draw(ReverseContourPen(pen), button, s, half - s * (lo + hi) / 2, half - cap_height / 2)


def letters_once(_made=[]):
    if not _made:
        _made.append(Letters())
    return _made[0]


def camera_size(axes):
    return {"xy": (820, 1000), "x": (820, 500), "y": (320, 1000)}[axes]


def camera(pen, axes):
    """the mouse moving: a small mouse, an arrowhead each way it moves (axes "xy", "x" or "y"), from (0, 0); camera_size"""
    from fontTools.pens.reverseContourPen import ReverseContourPen
    from fontTools.pens.transformPen import TransformPen
    w, h = camera_size(axes)
    cx, cy = w / 2, h / 2
    body = TransformPen(pen, (1, 0, 0, 1, cx - 160, cy - 250))
    rounded(body, 0, 0, 320, 500, 160)
    rounded(ReverseContourPen(body), 60, 60, 260, 440, 100)
    polygon(pen, [(cx - 30, cy + 60), (cx + 30, cy + 60), (cx + 30, cy + 150), (cx - 30, cy + 150)])   # (its wheel)
    if "y" in axes:
        polygon(pen, [(cx, cy + 500), (cx - 120, cy + 340), (cx + 120, cy + 340)])
        polygon(pen, [(cx, cy - 500), (cx + 120, cy - 340), (cx - 120, cy - 340)])
    if "x" in axes:
        polygon(pen, [(cx - 410, cy), (cx - 250, cy - 120), (cx - 250, cy + 120)])
        polygon(pen, [(cx + 410, cy), (cx + 250, cy + 120), (cx + 250, cy - 120)])


def key(pen, letters, vk):
    """a key's icon from (0, 0): key_width wide, H high (a key code that is no key: the blank cap)"""
    if vk in MICE:
        mouse(pen, KEYS[vk][7:])
    else:
        cap(pen, letters, KEYS.get(vk, ""), key_width(letters, vk), H, R, H * (P.WIDE - 0.3))


def mini(pen, letters, vk):
    """a key's small cap, MINI square, for an icon of several keys. Its outline begins at (MINI_R, 0) with the cap and ends
    at MINI_MARK's last point whatever the key: the last contour is a small square inside the cap's ink (it runs the cap's
    way: filled all the same, and no label reaches that corner), so that the glyph that calls it knows where the pen is
    before and after, and any key's cap can stand in for another's"""
    cap(pen, letters, KEYS.get(vk, ""), MINI, MINI, MINI_R, MINI * 0.74)
    polygon(pen, MINI_MARK)


def standalone():
    """the icons as a font of their own (OpenType, each at BASE + its key code, the camera's at BASE + 0x100 on), in the
    drawings' units, a cap half an em high (prompt_assets.Icons draws an icon on a sheet two ems wide): what the textures'
    images are drawn from"""
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    letters = letters_once()
    names, charstrings, metrics, cmap = [".notdef"], {}, {".notdef": (H, 0)}, {}
    pen = T2CharStringPen(H, None)
    charstrings[".notdef"] = pen.getCharString()
    for vk in sorted(KEYS):
        name, w = "key_%d" % vk, key_width(letters, vk)
        pen = T2CharStringPen(w + 100, None)
        key(pen, letters, vk)
        names.append(name)
        charstrings[name], metrics[name], cmap[BASE + vk] = pen.getCharString(), (w + 100, 0), name
    for i, axes in enumerate(("xy", "x", "y")):
        name, w = "camera_" + axes, camera_size(axes)[0]
        pen = T2CharStringPen(w + 100, None)
        camera(pen, axes)
        names.append(name)
        charstrings[name], metrics[name], cmap[BASE + 0x100 + i] = pen.getCharString(), (w + 100, 0), name
    fb = FontBuilder(2 * H, isTTF=False)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cmap)
    fb.setupCFF("TriforceKeys", {"FullName": "Triforce Keys"}, charstrings, {})
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=H, descent=0)
    fb.setupNameTable({"familyName": "Triforce Keys", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=H, sTypoDescender=0, usWinAscent=H, usWinDescent=0)
    fb.setupPost()
    out = io.BytesIO()
    fb.save(out)
    return out.getvalue()


class Icons(P.Icons):
    """the keys' icons for the textures (the ocarina's notes, the letters on black), by key code: prompt_assets.Icons drawn
    from standalone(). plain: the icons whose letter on black is the whole icon, not what is cut out of it (the mice)"""

    def __init__(self):
        self.path, self.code, self.plain = io.BytesIO(standalone()), {vk: BASE + vk for vk in KEYS}, MICE


def subr_bias(count):
    """the number a charstring subtracts from a subroutine's index, by how many subroutines the font has (CFF)"""
    return 107 if count < 1240 else 1131 if count < 33900 else 32768


def game_font(otf, default=DEFAULT):
    """the game's font (decrypted) as the keyboard and mouse face, the controls' keys out of the box `default`: (the font,
    binds). binds: {"patches": [(offset in the font, control)], "calls": {key code: the two bytes of its small cap's call}},
    what the host writes for other keys (write_binds); no patches when the font cannot take the subroutines (none of its
    own, or too many): the icons of several keys then show the keys out of the box whatever is bound"""
    from fontTools.misc.psCharStrings import T2CharString
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.ttLib import TTFont
    from fontTools.ttLib.tables.DefaultTable import DefaultTable

    game = TTFont(io.BytesIO(otf))
    gset, cmap = game.getGlyphSet(), dict(game.getBestCmap())
    top = game["CFF "].cff.topDictIndex[0]
    chars, order, hmtx = top.CharStrings, list(game.getGlyphOrder()), game["hmtx"]
    own = chars[cmap[P.SWITCH[0]]]   # (a charstring of the font: every new one shares its private dictionary and subroutines)
    letters = letters_once()

    def bounds(name):
        p = BoundsPen(gset)
        gset[name].draw(p)
        return p.bounds

    def fitted(box, w, h, advance):
        """the transform of a drawing w by h into a glyph's box (as prompt_assets.face_font's draw: a square about the box's
        middle, as high as it and up to P.WIDE times as wide, the glyph widened), and the glyph's advance"""
        side = max(box[2] - box[0], box[3] - box[1])
        s = min(side * P.WIDE / w, side / h)
        over = max(0, s * w - side)
        return (s, 0, 0, s, (box[0] + box[2] + over) / 2 - s * w / 2, (box[1] + box[3]) / 2 - s * h / 2), advance + round(over)

    def charstring(pen):
        return pen.getCharString(private=own.private, globalSubrs=own.globalSubrs)

    # each key's icon, in the box of the game's A
    a_box, a_advance = bounds(cmap[P.SWITCH[0]]), hmtx[cmap[P.SWITCH[0]]][0]
    for vk in [0] + sorted(KEYS):
        t, width = fitted(a_box, key_width(letters, vk), H, a_advance)
        pen = T2CharStringPen(width, gset)
        key(TransformPen(pen, t), letters, vk)
        name = "key_%d" % vk
        chars.charStringsIndex.append(charstring(pen))
        chars.charStrings[name] = len(chars.charStringsIndex) - 1
        top.charset.append(name)
        order.append(name)
        hmtx.metrics[name] = (width, 0)
    # the camera's: the mouse moving, inside the right stick's glyphs' boxes (their advances stay)
    for code, axes in CAMERA.items():
        box = bounds(cmap[code]) if code in cmap else None
        if box:
            w, h = camera_size(axes)
            s = min((box[2] - box[0]) / w, (box[3] - box[1]) / h)
            pen = T2CharStringPen(hmtx[cmap[code]][0], gset)
            camera(TransformPen(pen, (s, 0, 0, s, (box[0] + box[2] - s * w) / 2, (box[1] + box[3] - s * h) / 2)), axes)
            chars[cmap[code]] = charstring(pen)
    # each key's small cap as a subroutine, then the icons of several keys calling them. The calls the host rewrites are
    # all two bytes long (a subroutine's number less the bias from 108 to 1131): empty subroutines fill up to the first
    subrs = getattr(own.private, "Subrs", None)
    codes = [0] + sorted(KEYS)
    bias = subr_bias(len(subrs)) if subrs is not None else 0
    first = max(len(subrs), bias + 108) if subrs is not None else 0
    live = subrs is not None and subr_bias(first + len(codes)) == bias and first + len(codes) - 1 - bias <= 1131
    index = {}
    if live:
        while len(subrs) < first:
            subrs.append(T2CharString(program=["return"], private=own.private, globalSubrs=own.globalSubrs))
        for vk in codes:
            pen = T2CharStringPen(None, gset)
            mini(pen, letters, vk)
            program = charstring(pen).program
            at = next(i for i, token in enumerate(program) if isinstance(token, str))
            assert program[at] in ("rmoveto", "hmoveto", "vmoveto") and program[-1] == "endchar", program[:4]
            index[vk] = len(subrs)
            subrs.append(T2CharString(program=program[at + 1:-1] + ["return"], private=own.private,
                                      globalSubrs=own.globalSubrs))
    clusters = []   # ((the glyph's program, [(where in it a call's number is, its control)]))
    for code, cells in CLUSTERS.items():
        if code not in cmap or not bounds(cmap[code]):
            continue
        name, step = cmap[code], MINI + MINI_GAP
        box, advance = bounds(name), hmtx[name][0]
        cols, rows = max(c for _, c, _ in cells) + 1, max(r for _, _, r in cells) + 1
        w, h = cols * step - MINI_GAP, rows * step - MINI_GAP
        over = max(0, w - (box[2] - box[0]))
        x0, y0 = round((box[0] + box[2] + over) / 2 - w / 2), round((box[1] + box[3]) / 2 - h / 2)
        width = advance + round(over)
        program, calls, at = [width], [], (0, 0)
        pen = T2CharStringPen(width, gset)
        for control, col, row in cells:
            ox, oy = x0 + col * step, y0 + (rows - 1 - row) * step
            if live:
                program += [ox + MINI_R - at[0], oy - at[1], "rmoveto"]
                calls.append((len(program), control))
                program += [index[default[control]] - bias, "callsubr"]
                at = (ox + MINI_MARK[-1][0], oy + MINI_MARK[-1][1])
            else:
                mini(TransformPen(pen, (1, 0, 0, 1, ox, oy)), letters, default[control])
        if live:
            chars[name] = T2CharString(program=program + ["endchar"], private=own.private, globalSubrs=own.globalSubrs)
            clusters.append((program + ["endchar"], calls))
        else:
            chars[name] = charstring(pen)
        hmtx.metrics[name] = (width, hmtx[name][1])

    game.setGlyphOrder(order)
    gid = {n: i for i, n in enumerate(order)}
    mapping = {c: gid[n] for c, n in cmap.items() if c <= 0xFFFF}
    for vk in codes:
        mapping[BASE + vk] = gid["key_%d" % vk]
    single = {c: control for control, c in enumerate(P.SWITCH)}
    single.update(MOVES)
    for c, control in single.items():
        mapping[c] = gid["key_%d" % default[control]]
    table = DefaultTable("cmap")
    table.data = P.cmap4(mapping, set(single))
    game["cmap"] = table
    out = io.BytesIO()
    game.save(out)
    font = out.getvalue()

    def compiled(program):
        cs = T2CharString(program=list(program))
        cs.compile()
        return bytes(cs.bytecode)

    patches, seen = [], set()
    for program, calls in clusters:   # (where each call's number is in the font as written: the glyph's bytes, found whole)
        code = compiled(program)
        if code in seen:
            continue
        seen.add(code)
        found, at = [], font.find(code)
        while at >= 0:
            found.append(at)
            at = font.find(code, at + 1)
        if len(found) != sum(1 for p, _ in clusters if compiled(p) == code):
            raise SystemExit("prompts: an icon of several keys is in the font %d times" % len(found))
        for at in found:
            for where, control in calls:
                off = at + len(compiled(program[:where] + ["endchar"])) - 1
                assert 247 <= font[off] <= 250, font[off]
                patches.append((off, control))
    calls = {vk: compiled([index[vk] - bias, "callsubr"])[:2] for vk in index}
    return font, {"patches": sorted(patches), "calls": calls}


def write_binds(path, binds):
    with open(path, "wb") as f:
        f.write(b"HPKB" + struct.pack("<I", len(binds["patches"])))
        for off, control in binds["patches"]:
            f.write(struct.pack("<IB3x", off, control))
        f.write(struct.pack("<I", 256) + b"".join(binds["calls"].get(vk, b"\0\0") for vk in range(256)))


def hint_names():
    """the glyph of each key code's icon in game_font's font (None: no such key), for prompt_assets.write_hint"""
    return ["key_%d" % vk if vk == 0 or vk in KEYS else None for vk in range(256)]


def main():
    import argparse
    from PIL import Image, ImageDraw, ImageFont
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    font = ImageFont.truetype(io.BytesIO(standalone()), 192)
    codes = [BASE + vk for vk in sorted(KEYS)] + [BASE + 0x100 + i for i in range(3)]
    sheet = Image.new("L", (12 * 180, (len(codes) + 11) // 12 * 130), 0)
    for i, code in enumerate(codes):
        ImageDraw.Draw(sheet).text((i % 12 * 180 + 10, i // 12 * 130 + 10), chr(code), font=font, fill=255)
    os.makedirs(a.out, exist_ok=True)
    sheet.save(os.path.join(a.out, "keys.png"))
    print("prompts: keys.png, %d icons" % len(codes), file=sys.stderr)


if __name__ == "__main__":
    main()
