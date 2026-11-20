"""NVIDIA's Streamline files for DLSS frame generation (scripts/streamline_sdk.py, nx2/make_game.py place_dlssg_runtime).

Without the network: an archive made here stands for NVIDIA's release, and the script's file table is pointed at it.
- every listed file is taken out of the archive into the SDK folder at its own path, also from an archive whose paths
  have a folder in front; a second fetch downloads nothing;
- a file whose SHA-256 is another than the pinned one stops the fetch, and is not left in the folder;
- a file missing from the archive stops the fetch;
- the archive read by HTTP ranges gives the same files, and far less than the archive is read; a server that serves
  no ranges gets the archive read whole;
- the game folder gets the DLLs and the two license texts in dlssg, and loses them again;
- make_game.py puts them there only for a complete SDK folder, and takes them away for a build without it.

python -I -B tests/test_streamline_sdk.py
"""
import hashlib
import importlib.util
import io
import os
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sl = load("streamline_sdk", ROOT / "scripts" / "streamline_sdk.py")


def content(name):
    return (("the file %s\n" % name) * 400).encode()


def fake_tables():
    """The script's three tables with the SHA-256 of this test's own contents"""
    def with_digests(table):
        return [(name, hashlib.sha256(content(name)).hexdigest()) for name, _ in table]
    return with_digests(sl.HEADERS), with_digests(sl.RUNTIME), with_digests(sl.LICENSES)


def archive_bytes(prefix="", leave_out=(), change=()):
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(prefix + "docs/big.bin", os.urandom(600000))   # (what a fetch has no reason to read)
        for name, _ in sl.HEADERS + sl.RUNTIME + sl.LICENSES:
            if name in leave_out:
                continue
            z.writestr(prefix + name, content(name) + (b"changed" if name in change else b""))
        z.writestr(prefix + "bin/x64/other.dll", os.urandom(300000))
    return data.getvalue()


class FakeResponse:
    def __init__(self, data, status):
        self.data, self.status = data, status

    def read(self):
        return self.data

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class StreamlineSdkTest(unittest.TestCase):
    def setUp(self):
        self.saved = (sl.HEADERS, sl.RUNTIME, sl.LICENSES)
        sl.HEADERS, sl.RUNTIME, sl.LICENSES = fake_tables()
        self.tmp = tempfile.TemporaryDirectory(prefix="streamline-sdk-")
        self.dir = Path(self.tmp.name)
        self.said = []

    def tearDown(self):
        sl.HEADERS, sl.RUNTIME, sl.LICENSES = self.saved
        self.tmp.cleanup()

    def say(self, line):
        self.said.append(line)

    def test_fetch_from_archive(self):
        sdk = self.dir / "sdk"
        sl.fetch(str(sdk), say=self.say, archive=zipfile.ZipFile(io.BytesIO(archive_bytes())))
        self.assertEqual(sl.missing(str(sdk)), [])
        self.assertTrue(sl.complete(str(sdk)))
        self.assertTrue((sdk / "include" / "sl_dlss_g.h").is_file())
        self.assertTrue((sdk / "bin" / "x64" / "nvngx_dlssg.dll").is_file())
        self.assertFalse((sdk / "bin" / "x64" / "other.dll").exists())
        self.said.clear()
        sl.fetch(str(sdk), say=self.say, archive=None)   # (nothing is missing: no archive is opened at all)
        self.assertIn("already", self.said[0])

    def test_folder_in_front(self):
        sdk = self.dir / "sdk"
        sl.fetch(str(sdk), say=self.say, archive=zipfile.ZipFile(io.BytesIO(archive_bytes("streamline-sdk-v2/"))))
        self.assertTrue(sl.complete(str(sdk)))

    def test_only_what_is_missing(self):
        sdk = self.dir / "sdk"
        sl.fetch(str(sdk), say=self.say, archive=zipfile.ZipFile(io.BytesIO(archive_bytes())))
        victim = sdk / "bin" / "x64" / "sl.pcl.dll"
        victim.write_bytes(b"damaged")
        self.assertEqual(sl.missing(str(sdk)), ["bin/x64/sl.pcl.dll"])
        self.assertFalse(sl.complete(str(sdk)))
        sl.fetch(str(sdk), say=self.say, archive=zipfile.ZipFile(io.BytesIO(archive_bytes())))
        self.assertEqual(victim.read_bytes(), content("bin/x64/sl.pcl.dll"))

    def test_another_file_refused(self):
        sdk = self.dir / "sdk"
        with self.assertRaises(ValueError):
            sl.fetch(str(sdk), say=self.say, archive=zipfile.ZipFile(io.BytesIO(archive_bytes(change=("bin/x64/sl.dlss_g.dll",)))))
        self.assertFalse((sdk / "bin" / "x64" / "sl.dlss_g.dll").exists())
        self.assertFalse(sl.complete(str(sdk)))

    def test_missing_from_archive(self):
        with self.assertRaises(ValueError):
            sl.fetch(str(self.dir / "sdk"), say=self.say, archive=zipfile.ZipFile(io.BytesIO(archive_bytes(leave_out=("license.txt",)))))

    def test_ranges(self):
        data = archive_bytes()
        asked = []

        def opener(request, timeout=0):
            first, last = (int(x) for x in request.headers["Range"].split("=")[1].split("-"))
            asked.append(last - first + 1)
            return FakeResponse(data[first:last + 1], 206)

        archive, reader = sl.open_archive("https://example.invalid/sdk.zip", len(data), opener, self.say)
        sdk = self.dir / "sdk"
        sl.fetch(str(sdk), say=self.say, archive=archive)
        self.assertTrue(sl.complete(str(sdk)))
        self.assertIsNotNone(reader)
        self.assertEqual(reader.fetched, sum(asked))

    def test_no_ranges(self):
        data = archive_bytes()

        def opener(request, timeout=0):
            return FakeResponse(data, 200)

        archive, reader = sl.open_archive("https://example.invalid/sdk.zip", len(data), opener, self.say)
        self.assertIsNone(reader)
        self.assertTrue(any("read whole" in line for line in self.said))
        sdk = self.dir / "sdk"
        sl.fetch(str(sdk), say=self.say, archive=archive)
        self.assertTrue(sl.complete(str(sdk)))

    def test_game_folder(self):
        sdk, game = self.dir / "sdk", self.dir / "game"
        sl.fetch(str(sdk), say=self.say, archive=zipfile.ZipFile(io.BytesIO(archive_bytes())))
        game.mkdir()
        dst = Path(sl.install_runtime(str(sdk), str(game)))
        self.assertEqual(dst, game / "dlssg")
        names = sorted(p.name for p in dst.iterdir())
        self.assertEqual(names, sorted(["nvngx_dlssg.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.interposer.dll", "sl.pcl.dll",
                                        "sl.reflex.dll", "streamline-LICENSE.txt", "nvidia-rtx-sdks-LICENSE.txt"]))
        (dst / "the players own note.txt").write_text("kept")
        self.assertTrue(sl.remove_runtime(str(game)))
        self.assertEqual([p.name for p in dst.iterdir()], ["the players own note.txt"])   # (only NVIDIA's files go)
        self.assertFalse(sl.remove_runtime(str(self.dir / "no such game")))

    def test_real_table(self):
        headers, runtime, licenses = self.saved
        self.assertEqual(len(runtime), 6)
        self.assertIn("include/sl_dlss_g.h", dict(headers))
        for name, digest in headers + runtime + licenses:
            self.assertRegex(digest, r"^[0-9a-f]{64}$", name)
        self.assertEqual(set(sl.GAME_LICENSE_NAMES), {name for name, _ in licenses})

    def test_dlss_beside_streamline(self):
        """(11-18) with Streamline up NGX finds nvngx_dlss.dll only in dlssg: make_game.py puts a copy of the ngx one there,
        and takes it away with Streamline's files (the folder then goes too) or when the build has no DLSS"""
        sys.path.insert(0, str(ROOT / "nx2"))
        try:
            mg = load("make_game_for_test", ROOT / "nx2" / "make_game.py")
        finally:
            sys.path.remove(str(ROOT / "nx2"))
        sdk, game = self.dir / "sdk", self.dir / "game"
        sl.fetch(str(sdk), say=self.say, archive=zipfile.ZipFile(io.BytesIO(archive_bytes())))
        (game / "ngx").mkdir(parents=True)
        (game / "ngx" / "nvngx_dlss.dll").write_bytes(b"dlss")
        saved = (mg.streamline_script, mg.LINUX, os.environ.get("HOOKSHOT_STREAMLINE_SDK"))
        mg.streamline_script, mg.LINUX = (lambda: sl), False
        try:
            os.environ["HOOKSHOT_STREAMLINE_SDK"] = str(sdk)
            dst = Path(mg.place_dlssg_runtime(str(game)))
            self.assertEqual((dst / "nvngx_dlss.dll").read_bytes(), b"dlss")
            (game / "ngx" / "nvngx_dlss.dll").unlink()   # (a build without DLSS: no stale copy)
            mg.place_dlssg_runtime(str(game))
            self.assertFalse((dst / "nvngx_dlss.dll").exists())
            (dst / "nvngx_dlss.dll").write_bytes(b"old")   # (a build without Streamline: the folder goes whole)
            os.environ["HOOKSHOT_STREAMLINE_SDK"] = ""
            self.assertIsNone(mg.place_dlssg_runtime(str(game)))
            self.assertFalse(dst.exists())
        finally:
            mg.streamline_script, mg.LINUX = saved[0], saved[1]
            if saved[2] is None:
                os.environ.pop("HOOKSHOT_STREAMLINE_SDK", None)
            else:
                os.environ["HOOKSHOT_STREAMLINE_SDK"] = saved[2]

    def test_make_game(self):
        src = (ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")
        for needle in ("fetch_streamline_sdk(a)", "place_dlssg_runtime(out)", 'os.environ["HOOKSHOT_STREAMLINE_SDK"] = sdk',
                       "sl.remove_runtime(out)"):
            self.assertIn(needle, src)
        build = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
        self.assertIn('os.environ.get("HOOKSHOT_STREAMLINE_SDK", "")', build)
        self.assertIn('"-DHOOKSHOT_DLSSG"', build)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
