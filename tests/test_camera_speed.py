"""The Camera speed row (host/prompts.c build_camera, nx2/prompt_assets.py CAMERA).

The game's five camera sensitivities are five floats in a BYML file inside its camera pack. Setup's Update writes the
pack decompressed and a table of where the floats are; at the start the host serves the pack with each one times the
row's factor. Checked here, without the game:

- The table: each float of the StickSensitivity array, at its place in the pack, with the game's value; other arrays
  and other files of the pack are left out.
- A pack that is not the one known gives no table: no such file, no such array, an array of something else than
  floats, a value that is no speed.
- The host's and the menu's ends are wired: the pack's path, the row, its key in the options file, the mouse.

python -I -B tests/test_camera_speed.py
"""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import prompt_assets  # noqa: E402

RATES = (1.2, 1.6, 2.0, 2.4, 3.0)


def byml(entries):
    """a little-endian BYML whose root dictionary holds entries: [(key, value)], a value a float, or a list of floats (an
    array), or a list of ints (an array of ints)"""
    names = sorted(k for k, _ in entries)
    keys = bytearray(struct.pack("<I", 0xc2 | len(names) << 8))
    texts = b""
    first = 4 + 4 * (len(names) + 1)
    for n in names:
        keys += struct.pack("<I", first + len(texts))
        texts += n.encode() + b"\0"
    keys += struct.pack("<I", first + len(texts))
    keys += texts
    keys += bytes(-len(keys) % 4)
    keys_at = 16
    root_at = keys_at + len(keys)
    root = bytearray(struct.pack("<I", 0xc1 | len(entries) << 8))
    tail = bytearray()
    tail_at = root_at + 4 + 8 * len(entries)
    for k, v in sorted(entries):
        if isinstance(v, list):
            is_float = all(isinstance(x, float) for x in v)
            arr = struct.pack("<I", 0xc0 | len(v) << 8) + bytes([0xd2 if is_float else 0xd1] * len(v))
            arr += bytes(-len(arr) % 4)
            arr += b"".join(struct.pack("<f", x) if is_float else struct.pack("<i", x) for x in v)
            root += struct.pack("<I", names.index(k) | 0xc0 << 24) + struct.pack("<I", tail_at + len(tail))
            tail += arr
        else:
            root += struct.pack("<I", names.index(k) | 0xd2 << 24) + struct.pack("<f", v)
    return b"YB\x07\x00" + struct.pack("<III", keys_at, 0, root_at) + bytes(keys) + bytes(root) + bytes(tail)


def sarc(files):
    """a SARC of files: [(name, data)], as nx2/prompt_assets.py sarc_member reads one"""
    hl = 0x14
    n = len(files)
    names = b""
    nodes = b""
    data = b""
    for name, d in files:
        data += bytes(-len(data) % 8)
        nodes += struct.pack("<IIII", 0, 0x01000000 | len(names) // 4, len(data), len(data) + len(d))
        names += name.encode() + b"\0"
        names += bytes(-len(names) % 4)
        data += d
    sfat = b"SFAT" + struct.pack("<HHI", 12, n, 0x65) + nodes
    sfnt = b"SFNT" + struct.pack("<HH", 8, 0) + names
    data_at = hl + len(sfat) + len(sfnt)
    data_at += -data_at % 8
    head = b"SARC" + struct.pack("<HHIIHH", hl, 0xfeff, data_at + len(data), data_at, 0x100, 0)
    return (head + sfat + sfnt).ljust(data_at, b"\0") + data


GOOD = byml([("MoveSpeedRateFarDist", 40.0), ("Other", [0.5, 0.25]), ("StickSensitivity", list(RATES)),
             ("StickSensitivityLat2LngRatio", 1.78)])


def pack(member=GOOD, name=prompt_assets.CAMERA_MEMBER):
    return sarc([("GameParameter/Other.bgyml", byml([("StickSensitivity", [9.0, 9.5])])), (name, member), ("Last.bin", b"12345")])


class CameraSpeedTest(unittest.TestCase):
    def test_table(self):
        p = pack()
        got = prompt_assets.camera_speeds(p)
        self.assertEqual(len(got), 5)
        for (off, v), want in zip(got, RATES):
            self.assertAlmostEqual(v, want, places=5)
            self.assertEqual(struct.unpack_from("<f", p, off)[0], v)        # (its place in the pack)
        self.assertEqual([off for off, _ in got], list(range(got[0][0], got[0][0] + 20, 4)))   # (five in a row)
        with tempfile.TemporaryDirectory(prefix="camera-") as d:
            path = Path(d) / "camera.bin"
            prompt_assets.write_camera(path, got)
            raw = path.read_bytes()
        self.assertEqual(raw[:4], b"HPCM")
        self.assertEqual(struct.unpack_from("<I", raw, 4)[0], 5)
        self.assertEqual(len(raw), 8 + 8 * 5)
        self.assertEqual(struct.unpack_from("<If", raw, 8), got[0])

    def test_unknown_pack(self):
        self.assertIsNone(prompt_assets.camera_speeds(b"not a pack at all, only some bytes here"))
        self.assertIsNone(prompt_assets.camera_speeds(pack(name="Component/Elsewhere.bgyml")))                 # (no such file)
        self.assertIsNone(prompt_assets.camera_speeds(pack(byml([("MoveSpeedRateFarDist", 40.0)]))))           # (no such array)
        self.assertIsNone(prompt_assets.camera_speeds(pack(byml([("StickSensitivity", [1, 2, 3])]))))          # (not floats)
        self.assertIsNone(prompt_assets.camera_speeds(pack(byml([("StickSensitivity", 2.0)]))))                # (not an array)
        self.assertIsNone(prompt_assets.camera_speeds(pack(byml([("StickSensitivity", [1.2, 0.0, 2.0])]))))    # (no speed)
        self.assertIsNone(prompt_assets.camera_speeds(pack(b"YB\x07\x00" + bytes(40))))                        # (an empty file)

    def test_wired(self):
        prompts = (ROOT / "host/prompts.c").read_text(encoding="utf-8")
        self.assertIn('#define CAMERA_PATH "' + prompt_assets.CAMERA.lower() + '"', prompts)
        self.assertIn("{build, build_textures, build_layouts, build_camera}", prompts)
        self.assertIn('hos_ini_option("camera_speed", 100)', prompts)
        self.assertIn("v = v < 25 ? 25 : v > 300 ? 300 : v;", prompts)
        menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
        self.assertIn('{TAB_CONTROLS, "Camera speed", &opts.camera_speed, 25, 300, 5, tx_sens,', menu)
        hid = (ROOT / "host/nn_hid.c").read_text(encoding="utf-8")
        self.assertIn("ox /= cam, oy /= cam;", hid)


if __name__ == "__main__":
    unittest.main()
