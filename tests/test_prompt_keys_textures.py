"""The keyboard and mouse face's pictures: a key's icon as an image, and the textures' blocks made from it, on a texture
file written here (no game file, no art: nx2/prompt_keys.py draws with Libertinus, third_party/fonts).

- A key's icon as the textures take it (prompt_keys.Icons.masks: the icon, its shape, its label), for a sample of keys (a
  letter, a word, a number-pad key, an arrow, a punctuation mark, three of the mouse's buttons): three images of the size
  asked for, ink in each, the label inside the shape; a word's cap wider than high, a mouse higher than wide.
- nx2/prompt_assets.py face_layers for this face: an image per key for each of the ocarina's six notes and the four letters
  on black, of its texture's size and mode, with ink; no controller picture and no leader lines.
- face_textures: an entry per image, its input the key's code, its blocks whole and inside its texture's data; the blocks
  of an image shared by several textures (encoded once now) are the ones encoding it for that texture gives; and written
  into the file they decode to the image again, where the game reads them (the block-linear layout).

    python -I -B tests/test_prompt_keys_textures.py
"""
import struct
import sys
from pathlib import Path

RUN_ALL_MODULES = ["fontTools", "PIL", "numpy", "astc_encoder", "texture2ddecoder"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import prompt_assets as P  # noqa: E402
import prompt_keys as K  # noqa: E402

ASTC_SRGB, BC4 = 0x2D06, 0x1D01   # (the formats of the two kinds of texture, as prompt_assets reads them)
SAMPLE = [ord("W"), 0x10, 0x20, 0x67, 0x26, 0xBC, 0x01, 0x04, 0x05]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def texture_file(textures):
    """a texture file as prompt_assets.bntx_textures reads one: the count at 0x24, the records' table's offset at 0x28; a
    record: format at +0x1c, width and height at +0x24, the block height's log2 at +0x34, its name's offset at +0x60 (u16
    length, the name), at +0x70 the offset of its data's offset. textures: [(name, format, width, height, log2)]"""
    count, room = len(textures), 0x8000
    table, records = 0x30, 0x30 + 8 * len(textures)
    names = records + 0x80 * count
    b = bytearray(names + 0x40 * count)
    data = (len(b) + room - 1) // room * room
    struct.pack_into("<I", b, 0x24, count)
    struct.pack_into("<Q", b, 0x28, table)
    for i, (name, fmt, w, h, log2) in enumerate(textures):
        rec, at = records + 0x80 * i, names + 0x40 * i
        struct.pack_into("<Q", b, table + 8 * i, rec)
        struct.pack_into("<I", b, rec + 0x1C, fmt)
        struct.pack_into("<II", b, rec + 0x24, w, h)
        struct.pack_into("<I", b, rec + 0x34, log2)
        struct.pack_into("<Q", b, rec + 0x60, at)
        struct.pack_into("<Q", b, rec + 0x70, at + 0x30)
        struct.pack_into("<H", b, at, len(name))
        b[at + 2:at + 2 + len(name)] = name.encode()
        struct.pack_into("<Q", b, at + 0x30, data + room * i)
    return bytes(b) + bytes(data - len(b) + room * count), room


def ink(im):
    return sum(i * n for i, n in enumerate(im.histogram()[:256])) / 255


def main():
    from PIL import ImageChops
    icons = K.Icons()
    check(sorted(icons.code) == sorted(K.KEYS), "the textures' icons are not the keys'")
    for vk in SAMPLE:
        icon, shape, label = icons.masks(vk, 83)
        check(icon.size == shape.size == label.size == (83, 83) and icon.mode == "L", "key %d: an image of %s" % (vk, icon.size))
        check(ink(icon) > 83 * 83 * 0.04 and ink(shape) > ink(icon) and ink(label) > 20,
              "key %d: ink %d, its shape %d, its label %d" % (vk, ink(icon), ink(shape), ink(label)))
        check(ImageChops.subtract(label, shape).getbbox() is None, "key %d's label is outside its shape" % vk)
        x0, y0, x1, y1 = shape.getbbox()
        if vk in (0x10, 0x20):
            check(x1 - x0 > (y1 - y0) * 1.3, "a word's cap is %dx%d" % (x1 - x0, y1 - y0))
        elif vk in K.MICE:
            check(y1 - y0 > (x1 - x0) * 1.3, "a mouse is %dx%d" % (x1 - x0, y1 - y0))
        else:
            check(abs((x1 - x0) - (y1 - y0)) <= 2 and x1 - x0 >= 80, "key %d's cap is %dx%d" % (vk, x1 - x0, y1 - y0))

    b, room = texture_file([(n, ASTC_SRGB, 93, 93, 2) for n in P.NOTES] + [(n, BC4, 64, 64, 1) for n in P.LETTERS])
    tex = P.bntx_textures(b)
    check(list(tex) == list(P.NOTES) + list(P.LETTERS) and tex["OcarinaButtonA_00"][1:5] == (ASTC_SRGB, 93, 93, 4) and
          tex["KeyTexFontY_00"][1:5] == (BC4, 64, 64, 2), "the texture file written here is not read as written")
    layers = P.face_layers(b, tex, "keyboard", None)
    check(len(layers) == len(K.KEYS) * (len(P.NOTES) + len(P.LETTERS)) and len({x.name for x in layers}) == len(layers),
          "%d images, not one per key and texture" % len(layers))
    for layer in layers:
        t = tex[layer.texture]
        check(layer.texture in P.NOTES or layer.texture in P.LETTERS, "an image for %s: a controller's picture or line" % layer.texture)
        check(layer.im.size == (t[2], t[3]) and layer.im.mode == ("RGBA" if layer.texture in P.NOTES else "L"),
              "%s is %s %s" % (layer.name, layer.im.size, layer.im.mode))
        check(layer.pad in K.KEYS and 0 <= layer.sw < len(P.SWITCH) and layer.name == "%s.key%d.png" % (layer.texture, layer.pad),
              "%s: Switch button %d on input %d" % (layer.name, layer.sw, layer.pad))
        check(layer.im.getbbox() is not None, "%s is empty" % layer.name)
    by_name = {x.name: x for x in layers}
    note, letter = by_name["OcarinaButtonB_00.key%d.png" % ord("W")], by_name["KeyTexFontX_00.key%d.png" % ord("W")]
    check(note.im is by_name["OcarinaButtonL_00.key%d.png" % ord("W")].im, "a key's note is drawn for every Switch button again")
    px = note.im.load()
    check(px[2, 2][3] == 0 and px[12, 46] == (255, 255, 255, 255) and any(px[x, 46] == (0, 0, 0, 255) for x in range(93)),
          "a note is not a white cap with a dark letter on nothing")
    check(letter.im.getpixel((2, 2)) == 0 and ink(letter.im) > 100 and ink(letter.im) < ink(by_name["KeyTexFontX_00.key1.png"].im),
          "a key's letter on black is not its label alone, or the mouse's not the whole mouse")

    entries = P.face_textures(b, tex, layers)
    check(len(entries) == len(layers), "%d entries for %d images" % (len(entries), len(layers)))
    for layer, (sw, pad, rs) in zip(layers, entries):
        t = tex[layer.texture]
        blocks = ((t[2] + 3) // 4) * ((t[3] + 3) // 4) * (16 if layer.texture in P.NOTES else 8)
        check((sw, pad) == (layer.sw, layer.pad) and sum(len(d) for _, d in rs) == blocks and
              all(t[5] <= off and off + len(d) <= t[5] + room for off, d in rs),
              "%s: %d bytes of %d, or outside its texture" % (layer.name, sum(len(d) for _, d in rs), blocks))
    for layer in (note, letter, by_name["OcarinaButtonR_00.key1.png"], by_name["KeyTexFontY_00.key32.png"]):
        got = entries[layers.index(layer)][2]
        want = P.runs(tex[layer.texture], layer.im)
        check([[o, bytes(d)] for o, d in got] == [[o, bytes(d)] for o, d in want],
              "%s: the blocks taken from the image's first encoding are not its own" % layer.name)
        filled = bytearray(b)
        for off, d in got:
            filled[off:off + len(d)] = d
        back = P.decode(bytes(filled), tex[layer.texture]).convert(layer.im.mode)
        worst = ImageChops.difference(back, layer.im).convert("L").resize((16, 16)).getextrema()[1]   # (of 6-pixel averages)
        check(worst < 40, "%s read back from the file differs by %d of 255" % (layer.name, worst))
    print("ok: %d keys' images for %d textures, their blocks where the file is read" % (len(K.KEYS), len(tex)))


if __name__ == "__main__":
    main()
