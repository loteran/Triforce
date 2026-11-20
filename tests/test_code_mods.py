"""Code mods: the mods' .ips and .pchtxt patches of the game's code, applied before the game is translated
(nx2/code_mods.py, nx2/make_game.py; authored modules and patches, no game).

A build applies the patches of <game folder>/mods to its own copy of the game's code, and the lift key hashes that copy.
Run here on modules written by this test:

- the IPS and IPS32 reader: records, runs, the end's letters as an offset, files cut short, a write into the header;
- the IPSwitch (.pchtxt) reader: @nsobid-, offset_shift, @enabled and @disabled, @stop, strings, both byte orders, notes,
  and a patch line that cannot be read;
- the mods folder walked as host/mods.c walks it: names, the folders that are off, the deeper layouts, the order;
- a patch for another build refused with a line that names both ids, one that writes outside the module refused whole,
  two mods' patches of the same bytes (the later name's stay, and the build says so);
- the patched module read back by the translator's own loader (third_party/a64-kit/tools/nso.py), where lz4 is there;
- the lift key (make_game.py's own lift_key): another key with a patch, the first key again with the patch switched
  off or gone, the same key for a patch that changes nothing, another for an edited patch;
- what the game folder remembers (triforce_code_mods.txt) and what the next build says about it.

Read in make_game.py: the patches are applied after the copy for updates is made and before the lift key is taken.

    python -I -B tests/test_code_mods.py
"""
import ast
import hashlib
import os
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
NX2 = ROOT / "nx2"
sys.path.insert(0, str(NX2))
import code_mods  # noqa: E402

MAIN_ID = bytes.fromhex("e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348") + bytes(12)
SDK_ID = bytes.fromhex("11223344556677889900aabbccddeeff00112233") + bytes(12)
MAIN_HEX = MAIN_ID.hex()[:40]


def nso(build_id, text_size=0x400, rodata_size=0x300, data_size=0x200, fill=0x11):
    """A plain NSO the translator's loader can read: .text at 0, .rodata at 0x1000 (MOD0 and an empty dynamic section
    at its start), .data at 0x2000; every other byte of a segment is `fill` plus the segment's number"""
    text = bytearray([fill]) * text_size
    struct.pack_into("<II", text, 0, 0x14000002, 0x1000)   # (b +8; the MOD0 header's offset)
    rodata = bytearray([fill + 1]) * rodata_size
    rodata[0:28] = b"MOD0" + struct.pack("<6i", 0x20, 0x1200, 0x1300, 0, 0, 0x100)
    rodata[0x20:0x30] = bytes(16)   # (DT_NULL)
    data = bytearray([fill + 2]) * data_size
    head = bytearray(0x100)
    head[0:4] = b"NSO0"
    struct.pack_into("<I", head, 0xC, 0x38)   # (the three hashes are to be checked; nothing compressed)
    name = b"authored\0\0\0\0\0\0\0\0"
    at = 0x100 + len(name)
    for i, (seg, memory) in enumerate(((text, 0), (rodata, 0x1000), (data, 0x2000))):
        struct.pack_into("<3I", head, 0x10 + 0x10 * i, at, memory, len(seg))
        struct.pack_into("<I", head, 0x60 + 4 * i, len(seg))
        head[0xA0 + 0x20 * i:0xC0 + 0x20 * i] = hashlib.sha256(seg).digest()
        at += len(seg)
    struct.pack_into("<I", head, 0x3C, 0x100)
    head[0x40:0x60] = build_id
    return bytes(head) + name + bytes(text) + bytes(rodata) + bytes(data)


def ips(records, magic=b"PATCH"):
    """records: (file offset, bytes) or (file offset, (run length, byte))"""
    width, end = (3, b"EOF") if magic == b"PATCH" else (4, b"EEOF")
    out = bytearray(magic)
    for offset, body in records:
        out += offset.to_bytes(width, "big")
        if isinstance(body, tuple):
            out += b"\0\0" + body[0].to_bytes(2, "big") + bytes([body[1]])
        else:
            out += len(body).to_bytes(2, "big") + body
    return bytes(out + end)


def production():
    """LIFT_KEY_NOT, lift_key_file and lift_key themselves, cut out of make_game.py (as tests/test_lift_key_files.py)"""
    source = NX2 / "make_game.py"
    tree = ast.parse(source.read_text(encoding="utf-8"), filename=str(source))
    body = [n for n in tree.body
            if (isinstance(n, ast.Assign) and any(isinstance(x, ast.Name) and x.id == "LIFT_KEY_NOT" for x in n.targets))
            or (isinstance(n, ast.FunctionDef) and n.name in ("lift_key_file", "lift_key"))]
    assert len(body) == 3
    scope = dict(os=os, sys=sys, hashlib=hashlib, HERE=str(NX2), ROOT=str(ROOT))
    exec(compile(ast.Module(body=body, type_ignores=[]), str(source), "exec"), scope)
    return scope


class Folder(unittest.TestCase):
    """A game folder with a mods folder, and a copy of the game's code as a build has it"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="code-mods-")
        self.root = Path(self.tmp.name)
        self.out, self.exefs = self.root / "game", self.root / "work" / "exefs"
        self.mods = self.out / "mods"
        self.mods.mkdir(parents=True)
        self.said = []
        self.had_mods = os.environ.pop("HOS_MODS", None)   # (the game's own switch for another mods folder: not this test's)
        self.fresh()

    def tearDown(self):
        if self.had_mods is not None:
            os.environ["HOS_MODS"] = self.had_mods
        self.tmp.cleanup()

    def fresh(self):
        """The copy as a build starts with it: unpatched"""
        self.exefs.mkdir(parents=True, exist_ok=True)
        (self.exefs / "main").write_bytes(nso(MAIN_ID))
        (self.exefs / "sdk").write_bytes(nso(SDK_ID, fill=0x41))
        (self.exefs / "main.npdm").write_bytes(b"META" + bytes(60))

    def patch(self, rel, data):
        path = self.mods / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, str):
            path.write_text(data, encoding="utf-8")
        else:
            path.write_bytes(data)
        return path

    def apply(self):
        self.said = []
        return code_mods.apply(str(self.exefs), str(self.mods), self.said.append)

    def image(self, name="main"):
        m = code_mods.Module(str(self.exefs / name))
        m.load()
        return bytes(m.image)


class Ips(unittest.TestCase):
    def test_records_and_runs(self):
        data = ips([(0x100 + 0x10, b"\x01\x02\x03\x04"), (0x100 + 0x1000, (5, 0xEE)), (0x100, b"\x99")])
        self.assertEqual(code_mods.parse_ips(data), [(0x10, b"\x01\x02\x03\x04"), (0x1000, b"\xee" * 5), (0, b"\x99")])
        self.assertEqual(code_mods.parse_ips(data + b"\x00\x12\x34"), code_mods.parse_ips(data))   # (a truncation length after EOF)
        self.assertEqual(code_mods.parse_ips(ips([])), [])

    def test_ips32(self):
        data = ips([(0x100 + 0x01234568, b"\xaa\xbb"), (0x100 + 4, (3, 7))], b"IPS32")
        self.assertEqual(code_mods.parse_ips(data), [(0x01234568, b"\xaa\xbb"), (4, b"\x07\x07\x07")])

    def test_the_ends_letters_as_an_offset(self):
        # (0x454F46 is "EOF": a record there is a record while the file goes on after it)
        data = ips([(0x454F46, b"\x05\x06"), (0x200, b"\x07")])
        self.assertEqual(code_mods.parse_ips(data), [(0x454F46 - 0x100, b"\x05\x06"), (0x100, b"\x07")])

    def test_cut_short(self):
        whole = ips([(0x110, b"\x01\x02\x03\x04"), (0x1100, (5, 0xEE))])
        for cut in (len(whole) - 3, len(whole) - 4, len(whole) - 6, 12, 9, 6):   # (no EOF, in the run, in a record, in a head)
            with self.assertRaises(code_mods.Refused, msg=cut) as e:
                code_mods.parse_ips(whole[:cut])
            self.assertIn("cut short", str(e.exception))
        with self.assertRaises(code_mods.Refused):
            code_mods.parse_ips(ips([(0x110, b"\x01")], b"IPS32")[:-1])

    def test_not_an_ips_file(self):
        for data in (b"", b"PATC", b"NSO0" + bytes(32), b"@nsobid-00\n"):
            with self.assertRaises(code_mods.Refused) as e:
                code_mods.parse_ips(data)
            self.assertIn("not an IPS file", str(e.exception))

    def test_a_write_into_the_header(self):
        with self.assertRaises(code_mods.Refused) as e:
            code_mods.parse_ips(ips([(0xFF, b"\x01")]))
        self.assertIn("header", str(e.exception))


class Pchtxt(unittest.TestCase):
    def test_a_file(self):
        text = "\n".join((
            "@nsobid-" + MAIN_HEX.upper(),
            "# a title",
            "@flag offset_shift 0x100",
            "@flag print_values",
            "00000010 FFFFFFFF",             # (before any @enabled: not a patch)
            "// first patch [author]",
            "@enabled",
            "00000010 1F2003D5",
            "00001020 \"Zelda\\n\\\"x\\\"\\0\"   // a string",
            "00000020 0102 // two bytes",
            "a line of text that is no patch",
            "@disabled",
            "00000030 AABBCCDD",
            "@enabled",
            "@little-endian",
            "00000040 11223344",
            "@big-endian",
            "00000044 11223344",
            "@stop",
            "00000050 EEEEEEEE",
        ))
        build, writes = code_mods.parse_pchtxt(text)
        self.assertEqual(build, MAIN_HEX.upper())
        self.assertEqual(writes, [(0x10, bytes.fromhex("1F2003D5")), (0x1020, b'Zelda\n"x"\0'), (0x20, b"\x01\x02"),
                                  (0x40, bytes.fromhex("44332211")), (0x44, bytes.fromhex("11223344"))])

    def test_offsets_count_the_header_without_a_shift(self):
        build, writes = code_mods.parse_pchtxt("@nsobid-AB\n@enabled\n00000110 01\n")
        self.assertEqual((build, writes), ("AB", [(0x10, b"\x01")]))
        with self.assertRaises(code_mods.Refused) as e:
            code_mods.parse_pchtxt("@nsobid-AB\n@enabled\n00000010 01\n")
        self.assertIn("line 3", str(e.exception))
        self.assertIn("header", str(e.exception))

    def test_a_patch_line_that_cannot_be_read(self):
        for line, why in (("00000010 1F2003D", "not a value"), ("00000010 XYZ1", "not a value"), ('00000010 "open', "closing quote"),
                          ('00000010 "a\\q"', "escape"), ("@flag offset_shift lots", "offset_shift")):
            text = "@nsobid-AB\n@flag offset_shift 0x100\n@enabled\n00000000 00\n" + line + "\n"
            with self.assertRaises(code_mods.Refused, msg=line) as e:
                code_mods.parse_pchtxt(text)
            self.assertIn(why, str(e.exception))
            self.assertIn("line 5", str(e.exception))
        # (the same lines while patches are off are notes)
        self.assertEqual(code_mods.parse_pchtxt("@nsobid-AB\n@disabled\n00000010 XYZ1\n")[1], [])

    def test_no_build_id(self):
        self.assertIsNone(code_mods.parse_pchtxt("@enabled\n00000110 01\n")[0])

    def test_same_build(self):
        full = MAIN_ID.hex()
        for named in (MAIN_HEX, MAIN_HEX.upper(), full, MAIN_HEX + "00"):
            self.assertTrue(code_mods.same_build(named, full), named)
        for named in (MAIN_HEX[:16], MAIN_HEX[:-1], MAIN_HEX[:-1] + "9", "", "zz", MAIN_HEX + "1", full + "00"):
            self.assertFalse(code_mods.same_build(named, full), named)


class ModsFolder(Folder):
    def test_the_walk_is_host_mods_c(self):
        for rel in ("b-mod/exefs/x.pchtxt", "A-mod/exefs/x.pchtxt", "c-mod/romfs/UI/x.zs", "-off/exefs/x.pchtxt", "_off/exefs/x.pchtxt",
                    ".hidden/exefs/x.pchtxt", "contents/0400486024DEC000/deep/exefs/x.pchtxt", "contents/-off/deep/exefs/x.pchtxt",
                    "d-mod/cheats/E0F3CCFAF50065AE.txt", "a/b/c/d/exefs/x.pchtxt", "p/q/r/s/t/exefs/x.pchtxt",
                    "outer/exefs/x.pchtxt", "outer/inner/exefs/x.pchtxt", "empty/readme.txt"):
            self.patch(rel, "x")
        self.patch("exefs/x.pchtxt", "x")   # (the mods folder itself is no mod)
        names = [name for name, _ in code_mods.find_mods(str(self.mods))]
        # (by name, the letters' case ignored: "-" sorts before "/")
        self.assertEqual(names, ["A-mod", "a/b/c/d", "b-mod", "c-mod", "contents/0400486024DEC000/deep", "d-mod", "outer"])
        files = [rel for rel, _ in code_mods.patch_files(str(self.mods))]
        self.assertEqual(files, ["A-mod/exefs/x.pchtxt", "a/b/c/d/exefs/x.pchtxt", "b-mod/exefs/x.pchtxt",
                                 "contents/0400486024DEC000/deep/exefs/x.pchtxt", "outer/exefs/x.pchtxt"])
        # (and the walk in host/mods.c is still the one this follows)
        mods_c = (ROOT / "host" / "mods.c").read_text(encoding="utf-8")
        for needle in ("if (depth >= 4) return;", "fd.cFileName[0] == '.'", "fd.cFileName[0] == '-' || fd.cFileName[0] == '_'",
                       "_stricmp(((const Mod *)a)->name, ((const Mod *)b)->name)", '"%s\\\\cheats"', "depth > 0"):
            self.assertTrue(needle in mods_c, "host/mods.c no longer has: " + needle)

    def test_a_mods_files_in_the_order_of_their_names(self):
        for name in ("b.pchtxt", "A.ips", "c.IPS", "notes.txt", "d.pchtxt.bak"):
            self.patch("m/exefs/" + name, "x")
        (self.mods / "m" / "exefs" / "folder.ips").mkdir()
        self.assertEqual([rel for rel, _ in code_mods.patch_files(str(self.mods))],
                         ["m/exefs/A.ips", "m/exefs/b.pchtxt", "m/exefs/c.IPS"])

    def test_no_mods_folder(self):
        self.assertEqual(code_mods.find_mods(str(self.root / "none")), [])
        self.assertEqual(code_mods.apply(str(self.exefs), str(self.root / "none"), self.said.append), [])


class Apply(Folder):
    def test_a_patch_of_each_kind(self):
        before_sdk = (self.exefs / "sdk").read_bytes()
        self.patch("fps/exefs/%s.ips" % MAIN_HEX, ips([(0x100 + 0x40, b"\x1f\x20\x03\xd5"), (0x100 + 0x1080, (4, 0))]))
        self.patch("names/exefs/names.pchtxt", "@nsobid-%s\n@flag offset_shift 0x100\n@enabled\n00002010 \"Hi\"\n" % MAIN_HEX)
        entries = self.apply()
        self.assertEqual([(e["state"], e["path"], e["module"], e["changed"]) for e in entries],
                         [("applied", "fps/exefs/%s.ips" % MAIN_HEX, "main", 8), ("applied", "names/exefs/names.pchtxt", "main", 2)])
        image = self.image()
        self.assertEqual(image[0x40:0x44], b"\x1f\x20\x03\xd5")
        self.assertEqual(image[0x1080:0x1084], bytes(4))
        self.assertEqual(image[0x2010:0x2012], b"Hi")
        self.assertEqual((image[0x3F], image[0x44], image[0x107F], image[0x1084], image[0x200F], image[0x2012]),
                         (0x11, 0x11, 0x12, 0x12, 0x13, 0x13))
        self.assertEqual((self.exefs / "sdk").read_bytes(), before_sdk)   # (a module no patch is for: its file as it was)
        said = "\n".join(self.said)
        self.assertIn("code mods: fps/exefs/%s.ips applied to main: 8 bytes changed" % MAIN_HEX, said)
        self.assertIn("code mods: names/exefs/names.pchtxt applied to main: 2 bytes changed", said)
        self.assertIn("translated with main patched", said)
        self.assertFalse((self.exefs / "main.part").exists())

    def test_the_file_written_is_a_whole_plain_module(self):
        self.patch("m/exefs/%s.ips" % MAIN_HEX, ips([(0x100 + 0x2000, b"\xff")]))
        self.apply()
        raw = (self.exefs / "main").read_bytes()
        self.assertEqual(raw[:4], b"NSO0")
        self.assertEqual(raw[0x40:0x60], MAIN_ID)
        self.assertEqual(struct.unpack_from("<I", raw, 0xC)[0], 0x38)
        for i, size in enumerate((0x400, 0x300, 0x200)):
            offset, memory, length = struct.unpack_from("<3I", raw, 0x10 + 0x10 * i)
            self.assertEqual((memory, length, struct.unpack_from("<I", raw, 0x60 + 4 * i)[0]), (0x1000 * i, size, size))
            self.assertEqual(hashlib.sha256(raw[offset:offset + length]).digest(), raw[0xA0 + 0x20 * i:0xC0 + 0x20 * i])
        self.assertEqual(raw[0x100:0x110], b"authored\0\0\0\0\0\0\0\0")
        self.assertEqual(len(raw), len(nso(MAIN_ID)))

    def test_the_translators_loader_reads_the_patch(self):
        try:
            import lz4.block  # noqa: F401  (third_party/a64-kit/tools/nso.py imports it)
        except ImportError:
            self.skipTest("no lz4 in this Python: the translator's loader cannot be imported")
        sys.path.insert(0, str(ROOT / "third_party" / "a64-kit" / "tools"))
        try:
            import nso as kit_nso
        finally:
            sys.path.pop(0)
        self.patch("m/exefs/%s.ips" % MAIN_HEX, ips([(0x100 + 0x80, b"\xc0\x03\x5f\xd6"), (0x100 + 0x2004, b"\x01\x02")]))
        plain = kit_nso.Nso(str(self.exefs / "main"))
        self.apply()
        patched = kit_nso.Nso(str(self.exefs / "main"))
        self.assertEqual(patched.u32(0x80), 0xD65F03C0)
        self.assertEqual(bytes(patched.image[0x2004:0x2006]), b"\x01\x02")
        self.assertEqual(patched.segments, plain.segments)
        self.assertEqual((patched.build_id, patched.bss_size, patched.mod0), (plain.build_id, plain.bss_size, plain.mod0))
        differ = [i for i in range(len(plain.image)) if plain.image[i] != patched.image[i]]
        self.assertEqual(differ, [0x80, 0x81, 0x82, 0x83, 0x2004, 0x2005])

    def test_another_build_is_refused(self):
        other = "0123456789abcdef0123456789abcdef01234567"
        self.patch("old/exefs/%s.ips" % other, ips([(0x140, b"\x00")]))
        self.patch("old/exefs/other.pchtxt", "@nsobid-%s\n@enabled\n00000140 00\n" % other.upper())
        self.patch("old/exefs/prefix.pchtxt", "@nsobid-%s\n@enabled\n00000140 00\n" % MAIN_HEX[:16])
        before = (self.exefs / "main").read_bytes()
        entries = self.apply()
        self.assertEqual([e["state"] for e in entries], ["refused"] * 3)
        self.assertEqual((self.exefs / "main").read_bytes(), before)
        for line, named in zip(self.said, (other, other, MAIN_HEX[:16])):
            self.assertIn("is not applied: it is for build %s, which is not this game's code" % named, line)
            self.assertIn("main " + MAIN_HEX, line)   # (and what this game's builds are)
            self.assertIn("sdk " + SDK_ID.hex()[:40], line)
        self.assertTrue(all(e["why"] for e in entries))

    def test_a_patch_for_another_module(self):
        self.patch("m/exefs/%s.ips" % SDK_ID.hex()[:40], ips([(0x100 + 8, b"\x77")]))
        before = (self.exefs / "main").read_bytes()
        self.assertEqual([(e["state"], e["module"]) for e in self.apply()], [("applied", "sdk")])
        self.assertEqual(self.image("sdk")[8], 0x77)
        self.assertEqual((self.exefs / "main").read_bytes(), before)

    def test_files_that_name_no_build(self):
        self.patch("m/exefs/patch.ips", ips([(0x140, b"\x00")]))
        self.patch("m/exefs/none.pchtxt", "@enabled\n00000140 00\n")
        self.patch("m/exefs/odd.pchtxt", "@nsobid-main\n@enabled\n00000140 00\n")
        self.apply()
        self.assertIn("no @nsobid- line", self.said[0])   # (in the order of their names: none, odd, patch)
        self.assertIn("its @nsobid- line is not a build id", self.said[1])
        self.assertIn("its name is not a build id", self.said[2])

    def test_writes_outside_the_module_refuse_the_whole_file(self):
        before = (self.exefs / "main").read_bytes()
        cases = {
            "past": ips([(0x100 + 0x10, b"\xaa"), (0x100 + 0x21FF, b"\x01\x02")]),          # (the last byte of .data and one more)
            "far": ips([(0x100 + 0x10, b"\xaa"), (0x100 + 0x900000, b"\x01")]),
            "cave": ips([(0x100 + 0x10, b"\xaa"), (0x100 + 0x400, b"\x1f\x20\x03\xd5")]),    # (the empty space after .text)
            "across": ips([(0x100 + 0x3FE, b"\x01\x02\x03\x04")]),                           # (.text's end into that space)
        }
        for name, data in cases.items():
            self.patch("%s/exefs/%s.ips" % (name, MAIN_HEX), data)
        entries = self.apply()
        self.assertEqual([e["state"] for e in entries], ["refused"] * 4)
        self.assertEqual((self.exefs / "main").read_bytes(), before)   # (not even the writes that were inside)
        why = {e["path"].split("/")[0]: e["why"] for e in entries}
        self.assertIn("it writes 2 bytes at main+0x21ff, past the end of its image (0x2200)", why["past"])
        self.assertIn("past the end of its image", why["far"])
        self.assertIn("it writes 4 bytes at main+0x400, outside its .text, .rodata and .data", why["cave"])
        self.assertIn("code cave", why["cave"])
        self.assertIn("main+0x3fe", why["across"])
        # (the edges themselves are inside)
        for name in cases:
            (self.mods / name / "exefs" / (MAIN_HEX + ".ips")).unlink()
        self.patch("edge/exefs/%s.ips" % MAIN_HEX, ips([(0x100, b"\x01"), (0x100 + 0x3FF, b"\x02"), (0x100 + 0x21FF, b"\x03")]))
        self.assertEqual([e["state"] for e in self.apply()], ["applied"])
        image = self.image()
        self.assertEqual((image[0], image[0x3FF], image[0x21FF]), (1, 2, 3))

    def test_a_file_cut_short_or_unreadable_is_refused(self):
        whole = ips([(0x100 + 0x10, b"\xaa\xbb\xcc\xdd")])
        self.patch("m/exefs/%s.ips" % MAIN_HEX, whole[:-2])
        self.patch("n/exefs/bad.pchtxt", "@nsobid-%s\n@enabled\n00000110 0\n" % MAIN_HEX)
        before = (self.exefs / "main").read_bytes()
        entries = self.apply()
        self.assertEqual([e["state"] for e in entries], ["refused", "refused"])
        self.assertIn("cut short", entries[0]["why"])
        self.assertIn("line 3", entries[1]["why"])
        self.assertEqual((self.exefs / "main").read_bytes(), before)

    def test_two_mods_on_the_same_bytes(self):
        self.patch("01-first/exefs/%s.ips" % MAIN_HEX, ips([(0x100 + 0x10, b"\x01\x02\x03\x04"), (0x100 + 0x20, b"\xaa")]))
        self.patch("02-second/exefs/second.pchtxt", "@nsobid-%s\n@flag offset_shift 0x100\n@enabled\n00000012 F3F4F5\n" % MAIN_HEX)
        entries = self.apply()
        image = self.image()
        self.assertEqual(image[0x10:0x16], b"\x01\x02\xf3\xf4\xf5\x11")   # (the later name's bytes where both write)
        self.assertEqual(image[0x20], 0xAA)
        self.assertEqual([(e["path"].split("/")[0], e["changed"]) for e in entries], [("01-first", 3), ("02-second", 3)])
        self.assertTrue(any("02-second/exefs/second.pchtxt writes over 2 bytes of 01-first/exefs/%s.ips" % MAIN_HEX in line
                            for line in self.said), self.said)
        # (the order is the names', whichever was made first: the same two the other way round)
        self.fresh()
        (self.mods / "01-first").rename(self.mods / "03-first")
        entries = self.apply()
        self.assertEqual(self.image()[0x10:0x16], b"\x01\x02\x03\x04\xf5\x11")
        self.assertEqual([(e["path"].split("/")[0], e["changed"]) for e in entries], [("02-second", 1), ("03-first", 5)])
        self.assertTrue(any(line.startswith("code mods: 03-first/") and "writes over 2 bytes of 02-second/" in line for line in self.said))

    def test_two_files_of_one_mod(self):
        self.patch("m/exefs/a.pchtxt", "@nsobid-%s\n@enabled\n00000110 AAAA\n" % MAIN_HEX)
        self.patch("m/exefs/b.pchtxt", "@nsobid-%s\n@enabled\n00000111 BB\n" % MAIN_HEX)
        self.apply()
        self.assertEqual(self.image()[0x10:0x12], b"\xaa\xbb")

    def test_a_patch_that_changes_nothing_leaves_the_file(self):
        before = (self.exefs / "main").read_bytes()
        self.patch("same/exefs/%s.ips" % MAIN_HEX, ips([(0x100 + 0x40, b"\x11\x11"), (0x100 + 0x1040, (3, 0x12))]))
        entries = self.apply()
        self.assertEqual([(e["state"], e["changed"]) for e in entries], [("applied", 0)])
        self.assertEqual((self.exefs / "main").read_bytes(), before)
        self.assertIn("0 bytes changed (it writes what the game has there already", self.said[0])
        self.assertFalse(any("translated with" in line for line in self.said))

    def test_a_module_that_is_no_nso_is_passed_over(self):
        (self.exefs / "main").write_bytes(b"game code")   # (as other tests' stand-ins: nothing to patch, nothing read)
        self.patch("m/exefs/%s.ips" % MAIN_HEX, ips([(0x140, b"\x00")]))
        self.assertEqual([e["state"] for e in self.apply()], ["refused"])


class LiftKey(Folder):
    @classmethod
    def setUpClass(cls):
        cls.scope = production()

    def key(self):
        # (lift_key imports kit and standalone for the HLE list: stand-ins, as the list is not what is tested)
        had = {name: sys.modules.get(name) for name in ("kit", "standalone")}
        sys.modules["kit"] = types.ModuleType("kit")
        standalone = types.ModuleType("standalone")
        standalone.implemented = lambda: ["nn::os::A"]
        sys.modules["standalone"] = standalone
        try:
            return self.scope["lift_key"](str(self.exefs), str(self.root / "work"), {"OOT_WORK": "w"})
        finally:
            for name, module in had.items():
                if module is None:
                    del sys.modules[name]
                else:
                    sys.modules[name] = module

    def build(self):
        """As make_game.py: a fresh copy of the game's code, the patches applied to it, the key taken"""
        self.fresh()
        self.apply()
        return self.key()

    def test_the_key_follows_the_patches(self):
        plain = self.build()
        self.assertEqual(plain, self.build())
        patch = self.patch("fps/exefs/%s.ips" % MAIN_HEX, ips([(0x100 + 0x40, b"\x1f\x20\x03\xd5")]))
        patched = self.build()
        self.assertNotEqual(patched, plain)        # (a patch added: translated again)
        self.assertEqual(patched, self.build())    # (the same patch at the next build: not again)
        (self.mods / "fps").rename(self.mods / "-fps")
        self.assertEqual(self.build(), plain)      # (switched off: the key of the game as it is)
        (self.mods / "-fps").rename(self.mods / "fps")
        self.assertEqual(self.build(), patched)
        patch.write_bytes(ips([(0x100 + 0x40, b"\x1f\x20\x03\xd4")]))
        edited = self.build()
        self.assertNotIn(edited, (plain, patched))   # (edited: another key)
        patch.unlink()
        self.assertEqual(self.build(), plain)      # (taken away)
        self.patch("data/exefs/d.pchtxt", "@nsobid-%s\n@enabled\n00002110 55\n" % MAIN_HEX)
        self.assertNotEqual(self.build(), plain)   # (a patch of the data counts too: the lift reads the whole image)

    def test_what_does_not_change_the_code_keeps_the_key(self):
        plain = self.build()
        self.patch("same/exefs/%s.ips" % MAIN_HEX, ips([(0x100 + 0x40, b"\x11\x11")]))                 # (the game's own bytes)
        self.patch("old/exefs/0123456789abcdef.ips", ips([(0x100 + 0x40, b"\x00")]))                    # (another build)
        self.patch("cave/exefs/%s.ips" % MAIN_HEX, ips([(0x100 + 0x800, b"\x00")]))                     # (refused)
        self.patch("art/romfs/UI/x.zs", b"a RomFS mod")
        self.patch("cheat/cheats/E0F3CCFAF50065AE.txt", "[a cheat]\n04000000 00002000 00000001\n")   # (run by the game, not built in)
        self.assertEqual(self.build(), plain)

    def test_the_patcher_itself_is_not_in_the_key(self):
        scope = self.scope
        self.assertIn("code_mods.py", scope["LIFT_KEY_NOT"])
        self.assertFalse(scope["lift_key_file"]("code_mods.py"))
        self.assertTrue(scope["lift_key_file"]("lift.py"))


class Record(Folder):
    def test_written_and_read_back(self):
        self.patch("fps/exefs/%s.ips" % MAIN_HEX, ips([(0x100 + 0x40, b"\x1f\x20\x03\xd5")]))
        self.patch("old/exefs/0123.ips", ips([(0x140, b"\x00")]))
        entries = self.apply()
        code_mods.write_record(str(self.out), entries)
        text = (self.out / "triforce_code_mods.txt").read_text(encoding="utf-8")
        lines = [line for line in text.splitlines() if not line.startswith("#")]
        data = (self.mods / "fps" / "exefs" / (MAIN_HEX + ".ips")).read_bytes()
        self.assertEqual(lines[0], "applied\t%s\t%d\tfps/exefs/%s.ips\tmain\t4" % (code_mods.fnv64(data), len(data), MAIN_HEX))
        self.assertTrue(lines[1].startswith("refused\t"))
        self.assertIn("\told/exefs/0123.ips\tit is for build 0123", lines[1])
        back = code_mods.read_record(str(self.out))
        self.assertEqual([(e["state"], e["path"], e["fnv"], e["size"], e["module"], e["changed"], e["why"]) for e in back],
                         [(e["state"], e["path"], e["fnv"], e["size"], e["module"], e["changed"], e["why"]) for e in entries])
        self.assertFalse((self.out / "triforce_code_mods.txt.new").exists())
        code_mods.write_record(str(self.out), [])   # (a build without patch files: no record, as before code mods)
        self.assertFalse((self.out / "triforce_code_mods.txt").exists())
        self.assertEqual(code_mods.read_record(str(self.out)), [])

    def test_fnv64(self):
        self.assertEqual(code_mods.fnv64(b""), "cbf29ce484222325")
        self.assertEqual(code_mods.fnv64(b"a"), "af63dc4c8601ec8c")
        self.assertEqual(code_mods.fnv64(b"foobar"), "85944171f73967e8")

    def test_what_the_next_build_says(self):
        """An update builds the folder's patches again, and says which of the last build's are gone, changed or refused"""
        say = []
        gone = self.patch("gone/exefs/%s.ips" % MAIN_HEX, ips([(0x100 + 0x40, b"\x01")]))
        stays = self.patch("stays/exefs/s.pchtxt", "@nsobid-%s\n@enabled\n00000150 02\n" % MAIN_HEX)
        edited = self.patch("edited/exefs/e.pchtxt", "@nsobid-%s\n@enabled\n00000160 03\n" % MAIN_HEX)
        broken = self.patch("broken/exefs/b.pchtxt", "@nsobid-%s\n@enabled\n00000170 04\n" % MAIN_HEX)
        self.patch("off/exefs/o.pchtxt", "@nsobid-%s\n@enabled\n00000180 05\n" % MAIN_HEX)
        first = code_mods.build_step(str(self.exefs), str(self.out), say.append)
        self.assertEqual(sum(e["state"] == "applied" for e in first), 5)
        # (a first build has nothing to compare with)
        self.assertFalse(any("the build before" in line or "this game folder before" in line for line in say), say)
        self.assertIn("code mods: 5 of 5 patch files applied (%s)" % (self.mods), say[-1])
        code_mods.write_record(str(self.out), first)

        self.fresh()   # (the update: the copy kept in the folder again, the mods folder as it is now)
        gone.unlink()
        edited.write_text("@nsobid-%s\n@enabled\n00000160 33\n" % MAIN_HEX, encoding="utf-8")
        broken.write_text("@nsobid-%s\n@enabled\n00900170 04\n" % MAIN_HEX, encoding="utf-8")
        (self.mods / "off").rename(self.mods / "_off")
        self.patch("new/exefs/n.pchtxt", "@nsobid-%s\n@enabled\n00000190 06\n" % MAIN_HEX)
        say = []
        second = code_mods.build_step(str(self.exefs), str(self.out), say.append)
        text = "\n".join(say)
        self.assertIn("code mods: gone/exefs/%s.ips was built into this game folder before and is gone from its mods folder (or "
                      "switched off): this build is made without it" % MAIN_HEX, text)
        self.assertIn("code mods: off/exefs/o.pchtxt was built into this game folder before and is gone", text)
        self.assertIn("code mods: edited/exefs/e.pchtxt changed since the build before", text)
        self.assertIn("code mods: broken/exefs/b.pchtxt was built into this game folder before and is refused now", text)
        self.assertIn("code mods: new/exefs/n.pchtxt applied to main: 1 bytes changed", text)
        self.assertFalse(any(line.startswith("code mods: stays/") and "before" in line for line in say))
        self.assertIn("code mods: 3 of 4 patch files applied", say[-1])
        image = self.image()   # (the same patches built in again, as they are now)
        self.assertEqual((image[0x40], image[0x50], image[0x60], image[0x70], image[0x80], image[0x90]), (0x11, 2, 0x33, 0x11, 0x11, 6))
        self.assertTrue(stays.exists())
        code_mods.write_record(str(self.out), second)

        self.fresh()   # (and with every mod gone: said once, and the record goes)
        for name in ("stays", "edited", "broken", "new"):
            (self.mods / name).rename(self.mods / ("-" + name))
        say = []
        third = code_mods.build_step(str(self.exefs), str(self.out), say.append)
        self.assertEqual(third, [])
        self.assertEqual(sorted(line.split(" ")[2] for line in say), ["edited/exefs/e.pchtxt", "new/exefs/n.pchtxt", "stays/exefs/s.pchtxt"])
        code_mods.write_record(str(self.out), third)
        self.assertFalse((self.out / "triforce_code_mods.txt").exists())
        self.assertEqual(self.image(), bytes(code_mods_original()))

    def test_the_mods_folder_of_a_build(self):
        had = os.environ.pop("HOS_MODS", None)
        try:
            self.assertEqual(code_mods.mods_folder(str(self.out)), os.path.join(str(self.out), "mods"))
            os.environ["HOS_MODS"] = str(self.root / "elsewhere")
            self.assertEqual(code_mods.mods_folder(str(self.out)), str(self.root / "elsewhere"))
        finally:
            os.environ.pop("HOS_MODS", None)
            if had is not None:
                os.environ["HOS_MODS"] = had


def code_mods_original():
    """The image of the authored main as nso() makes it"""
    with tempfile.TemporaryDirectory(prefix="code-mods-plain-") as tmp:
        path = os.path.join(tmp, "main")
        with open(path, "wb") as f:
            f.write(nso(MAIN_ID))
        m = code_mods.Module(path)
        m.load()
        return bytes(m.image)


class Driver(unittest.TestCase):
    def test_where_make_game_applies_them(self):
        source = (NX2 / "make_game.py").read_text(encoding="utf-8")
        main = source[source.index("def main(a=None):"):source.index("def cli():")]
        step = main.index("code_patches = _code_mods.build_step(exefs, out,")
        # (after the copy for updates is made in each of the three ways the game is read, and after the build is checked)
        self.assertEqual(main.count("shutil.copytree(exefs, kept)"), 2)
        self.assertLess(main.rindex("shutil.copytree(exefs, kept)"), step)
        self.assertLess(main.rindex("shutil.copytree(kept, exefs)"), step)
        self.assertLess(main.index("check_supported_build(exefs)"), step)
        # (before the key is taken and before the lift reads the copy)
        self.assertLess(step, main.index("key = lift_key(exefs, work, env)"))
        self.assertLess(step, main.index('"nx2/lift.py", "--standalone", "--write", "--exefs", exefs'))
        # (the record only once the game folder has the build)
        self.assertLess(main.index("os.replace(dst + \".new\", dst)"), main.index("_code_mods.write_record(out, code_patches)"))

    def test_the_lift_does_not_import_it(self):
        for name in ("lift.py", "chunks.py", "standalone.py", "image.py", "nso2.py", "split_hot.py"):
            text = (NX2 / name).read_text(encoding="utf-8")
            self.assertNotIn("code_mods", text, name)


if __name__ == "__main__":
    unittest.main()
