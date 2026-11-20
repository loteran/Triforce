"""The zip of a save export (host/save_zip.c, host/save_pack.c; no game, no Windows calls).

The System tab's Export save writes the save as a zip and Import a save takes one back, Triforce's or NX2navi's. The zip
code is the game's own, with no library: stored and deflated members, CRC-32, a central directory, UTF-8 names. Checked
here, with tests/save_zip_test.c built from the two files as they are:

- its own round trips: the CRC-32's check value, deflate and inflate over sizes from 0 to a megabyte, data cut short or
  changed, a zip cut short, the manifest's JSON, which paths a save's file may have, an export opened again, and every
  211th byte of an export damaged in turn (opened as it was, or refused: never something else);
- against Python: a zip it writes is read by zipfile (testzip, names in UTF-8 with their flag, contents, time stamp,
  stored where deflate does not help) and one zipfile writes is read by it (stored, deflated at levels 0 to 9, a folder
  entry, a UTF-8 name, an empty file), raw deflate both ways against zlib (its fixed, RLE and Huffman-only streams too);
- what the reader leaves out or refuses: an encrypted member, a 64-bit zip, a name that differs between the member and
  the directory, a zip cut short;
- an export as zipfile sees it: the manifest's keys, the save under save/, a bug report's files under bug-report/, no
  key file, nothing else;
- what an import takes: Triforce's export, NX2navi's as its launcher writes it and as its game does; and refuses: no
  manifest, another format, a newer version, another game, a path that leaves the save (.., a backslash, a drive, a
  device name), a link, a name twice, hundreds of files, more bytes than a save has (unpacking nothing), no save file,
  a bug report alone, a count that differs from the manifest's, damaged data, and a file that is no zip.

    python -I -B tests/test_save_zip.py [--cc gcc]
"""
import argparse
import io
import json
import random
import shutil
import subprocess
import sys
import tempfile
import warnings
import zipfile
import zlib
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
TITLE = "0400486024dec000"
UTF8_NAME = "名前 ñ/セーブ.sav"


def blob(seed, n, mix=3):
    """n bytes, part of them random and part a repeated text: what a save and a log look like to deflate"""
    r = random.Random(seed)
    out = bytearray()
    while len(out) < n:
        out += r.randbytes(97) if r.randrange(mix) == 0 else b"triforce save data " * 5
    return bytes(out[:n])


class Fixture:
    def __init__(self, cc, tmp):
        self.tmp = tmp
        self.exe = tmp / ("save_zip_test.exe" if sys.platform == "win32" else "save_zip_test")
        build = subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-I", str(ROOT / "host"),
                                str(ROOT / "tests" / "save_zip_test.c"), str(ROOT / "host" / "save_zip.c"),
                                str(ROOT / "host" / "save_pack.c"), "-o", str(self.exe)], capture_output=True, text=True)
        if build.returncode:
            raise SystemExit("FAIL: tests/save_zip_test.c does not build:\n" + build.stderr)
        self.n = 0

    def run(self, *args, ok=True):
        r = subprocess.run([str(self.exe)] + [str(a) for a in args], cwd=self.tmp, capture_output=True, timeout=300)
        if ok and r.returncode:
            raise SystemExit("FAIL: save_zip_test %s: exit %d\n%s%s" % (" ".join(str(a) for a in args), r.returncode,
                                                                       r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")))
        return r

    def folder(self):
        self.n += 1
        d = self.tmp / ("d%d" % self.n)
        d.mkdir()
        return d

    def list_file(self, members):
        """the list file the program takes names from, and the data files beside it (plain names: the list is UTF-8)"""
        d = self.folder()
        lines = []
        for i, (name, data) in enumerate(members):
            (d / ("%d.in" % i)).write_bytes(data)
            lines.append("%s\t%s/%d.in" % (name, d.name, i))
        (d / "list.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        return d

    def listing(self, zip_bytes):
        """(the directory's verdict, [(method, size, crc, is_dir, is_link, extracted, name, bytes or None)])"""
        d = self.folder()
        (d / "in.zip").write_bytes(zip_bytes)
        lines = self.run("list", d / "in.zip", d).stdout.decode("utf-8").splitlines()
        verdict = int(lines[0].split()[1])
        out = []
        for line in lines[1:]:
            i, method, size, crc, is_dir, is_link, got, name = line.split("\t")
            data = (d / (i + ".bin")).read_bytes() if got == "1" else None
            out.append((int(method), int(size), int(crc, 16), int(is_dir), int(is_link), int(got), name, data))
        return verdict, out

    def opened(self, zip_bytes):
        """(the verdict's name, made by, exported, {path below the save: bytes})"""
        d = self.folder()
        (d / "in.zip").write_bytes(zip_bytes)
        lines = self.run("open", d / "in.zip", d).stdout.decode("utf-8").splitlines()
        verdict, made_by, exported, count, why = lines[0].split("\t")
        files = {}
        for line in lines[1:]:
            i, size, rel = line.split("\t")
            files[rel] = (d / (i + ".bin")).read_bytes()
            assert len(files[rel]) == int(size)
        assert int(count) == len(files) and why
        return verdict, made_by, exported, files


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def check_written_by_c(fx):
    members = [("hello.txt", b"hello"), ("empty.bin", b""), ("save/slot_00/progress.sav", blob(1, 120556)),
               ("random.bin", random.Random(2).randbytes(20000)), (UTF8_NAME, b"utf-8 name"),
               ("bug-report/triforce_log.txt", b"[hos host] vk: a line of the log\n" * 6000)]
    d = fx.list_file(members)
    fx.run("write", d / "list.txt", d / "out.zip")
    with zipfile.ZipFile(d / "out.zip") as z:
        check(z.testzip() is None, "zipfile finds a bad member in a zip the game's code wrote")
        check(z.namelist() == [m[0] for m in members], "zipfile reads other names: %r" % z.namelist())
        for name, data in members:
            info = z.getinfo(name)
            check(z.read(name) == data, "zipfile reads other bytes for " + name)
            check(info.CRC == zlib.crc32(data) and info.file_size == len(data), "the directory's CRC-32 or size of " + name)
            check(info.date_time == (2026, 11, 15, 14, 22, 34), "the time stamp of " + name)
            check(bool(info.flag_bits & 0x800) == (not name.isascii()), "the UTF-8 flag of " + name)
        check(z.getinfo("random.bin").compress_type == zipfile.ZIP_STORED, "random bytes were not stored as they are")
        check(z.getinfo("save/slot_00/progress.sav").compress_type == zipfile.ZIP_DEFLATED, "a save file was not deflated")
        log = z.getinfo("bug-report/triforce_log.txt")
        check(log.compress_size * 20 < log.file_size, "a log deflates to %d of %d bytes" % (log.compress_size, log.file_size))


def python_zip(members, compression, level=None, extra=None):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", compression, compresslevel=level) as z:
        for name, data in members:
            z.writestr(name, data)
        if extra:
            extra(z)
    return out.getvalue()


def check_read_by_c(fx):
    members = [("a.txt", b"hello"), ("dir/", b""), ("dir/empty", b""), (UTF8_NAME, b"utf-8 name"),
               ("big.bin", blob(3, 400000)), ("zeros", bytes(300000)), ("random", random.Random(4).randbytes(70000))]
    for compression, level in [(zipfile.ZIP_STORED, None)] + [(zipfile.ZIP_DEFLATED, n) for n in (0, 1, 6, 9)]:
        verdict, got = fx.listing(python_zip(members, compression, level))
        what = "a zip of zipfile's (%s, level %s)" % ("stored" if compression == zipfile.ZIP_STORED else "deflated", level)
        check(verdict == 0 and len(got) == len(members), what + " is not read: verdict %d, %d members" % (verdict, len(got)))
        for (name, data), (method, size, crc, is_dir, is_link, extracted, got_name, got_data) in zip(members, got):
            check(got_name == name, what + ": the name %r is read as %r" % (name, got_name))
            check(is_dir == name.endswith("/") and not is_link, what + ": folder or link flags of " + name)
            if not is_dir:
                check(size == len(data) and crc == zlib.crc32(data), what + ": size or CRC-32 of " + name)
                check(extracted == 1 and got_data == data, what + ": the bytes of " + name)
    # raw deflate, both ways
    d = fx.folder()
    for i, data in enumerate((b"", b"a", b"abcabcabcabc" * 3, blob(5, 250000), bytes(100000), random.Random(6).randbytes(40000))):
        (d / "plain").write_bytes(data)
        fx.run("deflate", d / "plain", d / "packed")
        check(zlib.decompress((d / "packed").read_bytes(), -15) == data, "zlib does not inflate the game's deflate of sample %d" % i)
        for level in range(10):
            for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED, zlib.Z_HUFFMAN_ONLY, getattr(zlib, "Z_RLE", 3), getattr(zlib, "Z_FIXED", 4)):
                c = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
                (d / "packed").write_bytes(c.compress(data) + c.flush())
                r = fx.run("inflate", d / "packed", d / "back", len(data), ok=False)
                check(r.returncode == 0 and (d / "back").read_bytes() == data,
                      "the game's inflate fails on zlib's stream of sample %d (level %d, strategy %d)" % (i, level, strategy))
        (d / "packed").write_bytes(zlib.compress(data)[2:-4] + b"\x00")   # (and with a size that is not its own: refused)
        check(fx.run("inflate", d / "packed", d / "back", len(data) + 1, ok=False).returncode != 0, "inflate takes a wrong size")


def check_reader_refuses(fx):
    good = python_zip([("save/account.sav", b"x" * 300), ("b", b"y" * 300)], zipfile.ZIP_DEFLATED)
    check(fx.listing(good)[0] == 0, "the control zip is not read")
    # (an encrypted member: bit 0 of the flags in the directory)
    cd = good.index(b"PK\x01\x02")
    enc = bytearray(good)
    enc[cd + 8] |= 1
    check(fx.listing(bytes(enc))[0] == 3, "an encrypted member is not refused as unsupported")
    # (a 64-bit zip: a member's size or the member count moved to the 64-bit records, which this reader does not have)
    big = bytearray(good)
    big[cd + 24:cd + 28] = b"\xff\xff\xff\xff"
    check(fx.listing(bytes(big))[0] == 3, "a member with a 64-bit size is not refused as unsupported")
    end = good.rindex(b"PK\x05\x06")
    many = bytearray(good)
    many[end + 8:end + 12] = b"\xff\xff\xff\xff"
    check(fx.listing(bytes(many))[0] == 3, "a zip with a 64-bit member count is not refused as unsupported")
    # (a member written with room for 64-bit sizes that did not need them: its directory entry is an ordinary one)
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        with z.open("roomy", "w", force_zip64=True) as f:
            f.write(b"data")
    verdict, got = fx.listing(out.getvalue())
    check(verdict == 0 and got[0][7] == b"data", "a member with unused 64-bit room in its own header is not read")
    # (the name before a member's bytes is not its name in the directory)
    other = bytearray(good)
    at = good.index(b"save/account.sav")
    other[at + 5] ^= 0x01
    check(fx.listing(bytes(other))[0] == 2, "a member whose two names differ is not refused as damaged")
    for cut in (0, 1, 21, 22, len(good) // 2, len(good) - 1):
        check(fx.listing(good[:cut])[0] != 0, "a zip cut to %d bytes is read" % cut)
    # (a link, as zip programs on Unix store one: its mode in the attributes)
    info = zipfile.ZipInfo("link")
    info.create_system = 3
    info.external_attr = 0o120777 << 16
    verdict, got = fx.listing(python_zip([], zipfile.ZIP_STORED, extra=lambda z: z.writestr(info, "target")))
    check(verdict == 0 and got[0][4] == 1, "a link is not marked as one")


def manifest(**over):
    m = {"format": "triforce-save", "format_version": 1, "game": "The Legend of Zelda: Ocarina of Time", "title_id": TITLE,
         "triforce": "v0.4.0", "exported": "2026-11-15T14:22:34Z", "exported_by": "game", "files": 3, "bug_report": False}
    m.update(over)
    return {k: v for k, v in m.items() if v is not None}


SAVE = [("account.sav", blob(7, 320)), ("option.sav", blob(8, 42100)), ("slot_00/progress.sav", blob(9, 120556))]


def export_zip(man, save=SAVE, others=(), raw_names=(), compression=zipfile.ZIP_DEFLATED):
    """a zip as an export is laid out; raw_names: members whose names zipfile would tidy up, written as given"""
    out = io.BytesIO()
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")   # (a name twice is one of the cases)
        with zipfile.ZipFile(out, "w", compression) as z:
            if man is not None:
                z.writestr("manifest.json", man if isinstance(man, (str, bytes)) else json.dumps(man, indent=1) + "\n")
            for rel, data in save:
                z.writestr("save/" + rel, data)
            for name, data in others:
                z.writestr(name, data)
            for name, data, link in raw_names:
                info = zipfile.ZipInfo("x")
                info.filename = name
                info.compress_type = compression
                if link:
                    info.create_system = 3
                    info.external_attr = 0o120777 << 16
                z.writestr(info, data)
    return out.getvalue()


def check_export(fx):
    members = SAVE + [("slot_00/caption.sav", blob(10, 30442)), ("prod.keys", b"a key file left in the save folder"),
                      ("report:triforce_log.txt", b"[hos host] triforce v0.4.0\n" * 500), ("report:triforce_options.ini", b"fullscreen=1\n")]
    for bug in (0, 1):
        d = fx.list_file(members if bug else [m for m in members if not m[0].startswith("report:")])
        said = fx.run("export", d / "list.txt", d / "out.zip", bug).stdout.decode()
        check("files 4" in said, "the export counts %r, not the save's 4 files" % said)
        with zipfile.ZipFile(d / "out.zip") as z:
            check(z.testzip() is None, "zipfile finds a bad member in an export")
            names = set(z.namelist())
            want = {"manifest.json"} | {"save/" + m[0] for m in members if not m[0].startswith("report:") and not m[0].endswith(".keys")}
            if bug:
                want |= {"bug-report/triforce_log.txt", "bug-report/triforce_options.ini"}
            check(names == want, "an export holds %r, not %r" % (sorted(names), sorted(want)))
            m = json.loads(z.read("manifest.json"))
            check(m == {"format": "triforce-save", "format_version": 1, "game": "The Legend of Zelda: Ocarina of Time", "title_id": TITLE,
                        "triforce": 'v0.4.0 "test"', "exported": "2026-11-15T14:22:34Z", "exported_by": "game", "files": 4,
                        "bug_report": bool(bug)}, "the manifest is %r" % m)
            for rel, data in members:
                if not rel.startswith("report:") and not rel.endswith(".keys"):
                    check(z.read("save/" + rel) == data, "the export's " + rel + " is not the save's")
        # (and the game's own reader takes it back, whole)
        verdict, made_by, exported, files = fx.opened((d / "out.zip").read_bytes())
        check(verdict == "OK" and made_by == 'Triforce v0.4.0 "test"' and exported == "2026-11-15", "an export is not taken back: %s, %s, %s" % (verdict, made_by, exported))
        check(files == {m[0]: m[1] for m in members if not m[0].startswith("report:") and not m[0].endswith(".keys")}, "an export comes back with other files")


def check_import(fx):
    want = dict(SAVE)

    def expect(name, zip_bytes, verdict, files=None):
        got = fx.opened(zip_bytes)
        check(got[0] == verdict, "%s: %s, expected %s" % (name, got[0], verdict))
        check(got[3] == (files if files is not None and verdict == "OK" else {} if verdict != "OK" else want), name + ": the files that came out")
        return got

    got = expect("Triforce's export", export_zip(manifest()), "OK")
    check(got[1] == "Triforce v0.4.0" and got[2] == "2026-11-15", "who made an export and when: %r" % (got[1:3],))
    expect("stored, not deflated", export_zip(manifest(), compression=zipfile.ZIP_STORED), "OK")
    # NX2navi's, as its launcher writes it (zipfile, deflated, its keys) and as its game does (the keys in another layout)
    navi = manifest(format="nx2navi-save", triforce=None, nx2navi="0.12.0", game_version="1.1.1", exported_by="launcher", bug_report=True)
    got = expect("NX2navi's launcher's export", export_zip(navi, others=[("bug-report/nx2navi.log", b"log"), ("bug-report/nx2navi.ini", b"ini")]), "OK")
    check(got[1] == "NX2navi 0.12.0", "NX2navi's export is said to be made by %r" % got[1])
    game_text = ('{\n "format": "nx2navi-save",\n "format_version": 1,\n "game": "The Legend of Zelda: Ocarina of Time",\n "title_id": "%s",\n'
                 ' "game_version": "1.1.1",\n "nx2navi": "0.11.2",\n "exported": "2026-11-13T09:00:00Z",\n "exported_by": "game",\n'
                 ' "files": 3,\n "bug_report": false\n}\n' % TITLE)
    expect("NX2navi's game's export", export_zip(game_text), "OK")
    expect("the title id in capitals", export_zip(manifest(title_id=TITLE.upper())), "OK")
    expect("a key file beside the save", export_zip(manifest(), save=SAVE + [("prod.keys", b"k")]), "OK")
    expect("no count in the manifest", export_zip(manifest(files=None)), "OK")
    expect("a folder entry", export_zip(manifest(), others=[("save/slot_00/", b"")]), "OK")

    expect("no manifest", export_zip(None), "NOT_A_SAVE")
    expect("a manifest that is no JSON object", export_zip("[1, 2]"), "NOT_A_SAVE")
    expect("another format", export_zip(manifest(format="some-other-save")), "NOT_A_SAVE")
    expect("no format version", export_zip(manifest(format_version=None)), "NOT_A_SAVE")
    expect("a format version that is text", export_zip(manifest(format_version="1")), "NOT_A_SAVE")
    expect("format version 0", export_zip(manifest(format_version=0)), "NOT_A_SAVE")
    expect("a newer format", export_zip(manifest(format_version=2)), "NEWER")
    expect("another game", export_zip(manifest(title_id="0100000000010000")), "OTHER_GAME")
    expect("no title id", export_zip(manifest(title_id=None)), "OTHER_GAME")
    expect("a save without a save file", export_zip(manifest(files=1), save=[("notes.sav", b"n")]), "NOT_A_SAVE")
    expect("a bug report alone", export_zip(manifest(files=0), save=[], others=[("bug-report/triforce_log.txt", b"log")]), "NOT_A_SAVE")
    expect("fewer files than the manifest counts", export_zip(manifest(files=4)), "DAMAGED")
    expect("more files than the manifest counts", export_zip(manifest(files=2)), "DAMAGED")
    for bad in ("save/../evil.sav", "save/slot_00/../../evil.sav", "save//x.sav", "save/sub\\x.sav", "save/C:/x.sav", "save/nul.sav",
                "save/slot_00/COM1", "save/.hidden", "save/x.", "save/a b.sav", "save/a/b/c/d/e.sav", "save/caf\u00e9.sav"):
        expect("the path %r" % bad, export_zip(manifest(files=4), raw_names=[(bad, b"x", False)]), "UNSAFE")
    expect("a link in the save", export_zip(manifest(files=4), raw_names=[("save/slot_01", b"../../elsewhere", True)]), "UNSAFE")
    expect("a name twice", export_zip(manifest(files=4), raw_names=[("save/account.sav", b"again", False)]), "UNSAFE")
    expect("a name twice but for its case", export_zip(manifest(files=4), raw_names=[("save/Account.sav", b"again", False)]), "UNSAFE")
    many = [("slot_%02d/f%03d.sav" % (i % 90, i), b"x") for i in range(300)]
    expect("hundreds of files", export_zip(manifest(files=303), save=SAVE + many), "UNSAFE")
    expect("more bytes than a save has", export_zip(manifest(files=4), save=SAVE + [("slot_01/progress.sav", bytes(70 << 20))]), "UNSAFE")
    # (a member's bytes changed: its CRC-32 says so, and nothing comes out, not even the members that are whole)
    good = export_zip(manifest(), compression=zipfile.ZIP_STORED)
    at = good.index(SAVE[2][1][:64]) + 1000
    expect("a byte of a save file changed", good[:at] + bytes([good[at] ^ 0x40]) + good[at + 1:], "DAMAGED")
    packed = export_zip(manifest())
    cd = packed.index(b"PK\x01\x02")
    for at in range(len(packed) // 3, cd, max(1, (cd - len(packed) // 3) // 12)):
        got = fx.opened(packed[:at] + bytes([packed[at] ^ 0x08]) + packed[at + 1:])
        check(got[0] in ("DAMAGED", "NOT_A_SAVE", "UNSAFE") or (got[0] == "OK" and got[3] == want), "a byte changed at %d: %s with other files" % (at, got[0]))
    expect("a zip cut short", packed[:len(packed) - 30], "NOT_A_SAVE")
    expect("text, not a zip", b"this is not a zip file, it only has its name\n" * 4, "NOT_A_SAVE")
    expect("an empty file", b"", "NOT_A_SAVE")
    expect("a zip of something else", python_zip([("readme.txt", b"hello")], zipfile.ZIP_DEFLATED), "NOT_A_SAVE")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    cc = shutil.which(a.cc) or shutil.which("gcc") or shutil.which("clang") or shutil.which("cc")
    if not cc:
        print("skip: no C compiler")
        return
    with tempfile.TemporaryDirectory(prefix="triforce-save-zip-") as tmp:
        fx = Fixture(cc, Path(tmp).resolve())
        own = fx.run("selftest").stdout.decode()
        check("PASS" in own and "FAIL" not in own, "the program's own checks:\n" + own)
        check_written_by_c(fx)
        check_read_by_c(fx)
        check_reader_refuses(fx)
        check_export(fx)
        check_import(fx)
    print("PASS: the save export's zip: its own round trips, and against Python's zipfile and zlib both ways")


if __name__ == "__main__":
    main()
