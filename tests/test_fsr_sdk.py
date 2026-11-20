"""AMD's FSR 4 libraries for the players' build (scripts/fsr_sdk.py, nx2/make_game.py --fsr).

Without the network: a stand-in for AMD's repository hands out this test's own files, and the script's table is pointed
at them.
- both files are fetched into the folder; a second fetch downloads nothing; a damaged one alone is fetched again;
- a file of another size or SHA-256 stops the fetch and is not left in the folder;
- the game folder gets the two libraries and AMD's license text beside the exe, and loses exactly those again;
- make_game.py fetches with --fsr, places them at the end, and Setup asks for --fsr.
(Triforce) And AMD FSR 4's frame generation library, the third file of the same release (fsr.OPTIONAL):
- it is fetched after the two, from the same folder of AMD's repository, and placed beside the exe with them;
- when it cannot be had, or is another file than the pinned one, the fetch says so with the size and SHA-256 that came,
  leaves nothing of it behind, and the two libraries are fetched and placed as before; a later fetch gets it alone;
- AMD's license text beside the game names it; a build without FSR (--no-fsr) takes all three out of the game folder.

python -I -B tests/test_fsr_sdk.py
"""
import hashlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("fsr_sdk", ROOT / "scripts" / "fsr_sdk.py")
fsr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fsr)

LOADER, UPSCALER, FRAMEGEN = "amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_upscaler_vk.dll", "amd_fidelityfx_framegeneration_vk.dll"


def content(name):
    return (("AMD's %s\n" % name) * 300).encode()


def pinned(table):
    return [(name, len(content(name)), hashlib.sha256(content(name)).hexdigest()) for name, _, _ in table]


class FsrSdkTest(unittest.TestCase):
    def setUp(self):
        self.saved, self.saved_optional = fsr.RUNTIME, fsr.OPTIONAL
        fsr.RUNTIME, fsr.OPTIONAL = pinned(self.saved), pinned(self.saved_optional)
        self.tmp = tempfile.TemporaryDirectory(prefix="fsr-sdk-")
        self.dir = Path(self.tmp.name)
        self.asked = []
        self.said = []

    def tearDown(self):
        fsr.RUNTIME, fsr.OPTIONAL = self.saved, self.saved_optional
        self.tmp.cleanup()

    def get(self, url):
        self.asked.append(url)
        return content(url.rsplit("/", 1)[1])

    def test_fetch(self):
        folder = self.dir / "sdk"
        fsr.fetch(str(folder), say=self.said.append, get=self.get)
        self.assertTrue(fsr.complete(str(folder)))
        # (the two, then the frame generation library, each from the same folder of AMD's repository)
        self.assertEqual(self.asked, [fsr.BASE + LOADER, fsr.BASE + UPSCALER, fsr.BASE + FRAMEGEN])
        self.assertEqual(fsr.have_optional(str(folder)), [FRAMEGEN])
        fsr.fetch(str(folder), say=self.said.append, get=self.get)
        self.assertEqual(len(self.asked), 3)   # (nothing asked for again)
        self.assertIn("already", self.said[-1])
        (folder / LOADER).write_bytes(b"damaged")
        self.assertEqual(fsr.missing(str(folder)), [LOADER])
        fsr.fetch(str(folder), say=self.said.append, get=self.get)
        self.assertEqual(self.asked[3:], [fsr.BASE + LOADER])
        self.assertTrue(fsr.complete(str(folder)))
        (folder / FRAMEGEN).write_bytes(b"damaged")   # (and a damaged third one alone)
        self.assertEqual(fsr.have_optional(str(folder)), [])
        self.assertTrue(fsr.complete(str(folder)))
        fsr.fetch(str(folder), say=self.said.append, get=self.get)
        self.assertEqual(self.asked[4:], [fsr.BASE + FRAMEGEN])
        self.assertEqual(fsr.have_optional(str(folder)), [FRAMEGEN])

    def test_another_file_refused(self):
        folder = self.dir / "sdk"
        with self.assertRaises(ValueError):
            fsr.fetch(str(folder), say=self.said.append, get=lambda url: content(url) + b"x")
        self.assertEqual(sorted(p.name for p in folder.iterdir()), [])
        self.assertFalse(fsr.complete(str(folder)))

    def test_frame_generation_library_is_not_required(self):
        """It cannot be had (no such file at AMD's, or another file than the pinned one): said, nothing of it left, and FSR 4
        the upscaler is fetched and placed as before; the next fetch asks for it alone"""
        for case, bad in (("gone", OSError("HTTP Error 404: Not Found")), ("other", content(FRAMEGEN) + b"another build")):
            with self.subTest(case=case):
                folder, game = self.dir / ("sdk-" + case), self.dir / ("game-" + case)
                self.asked.clear(), self.said.clear()

                def get(url, bad=bad):
                    if not url.endswith(FRAMEGEN):
                        return self.get(url)
                    self.asked.append(url)
                    if isinstance(bad, Exception):
                        raise bad
                    return bad

                fsr.fetch(str(folder), say=self.said.append, get=get)   # (no exception: the build goes on)
                self.assertTrue(fsr.complete(str(folder)))
                self.assertEqual(fsr.have_optional(str(folder)), [])
                self.assertEqual(sorted(p.name for p in folder.iterdir()), [LOADER, UPSCALER])
                self.assertIn(FRAMEGEN, self.said[-1])
                self.assertIn("without it", self.said[-1])
                if case == "other":   # (what came, for the table: its size and SHA-256)
                    self.assertIn("%d bytes" % len(bad), self.said[-1])
                    self.assertIn(hashlib.sha256(bad).hexdigest(), self.said[-1])
                game.mkdir()
                (game / FRAMEGEN).write_bytes(b"left by a build before")
                placed = fsr.install_runtime(str(folder), str(game))
                self.assertEqual(sorted(Path(p).name for p in placed), [LOADER, UPSCALER])
                self.assertFalse((game / FRAMEGEN).exists())   # (not left beside a loader of another fetch)
                before = len(self.asked)
                fsr.fetch(str(folder), say=self.said.append, get=self.get)
                self.assertEqual(self.asked[before:], [fsr.BASE + FRAMEGEN])
                self.assertEqual(fsr.have_optional(str(folder)), [FRAMEGEN])

    def test_game_folder(self):
        folder, game = self.dir / "sdk", self.dir / "game"
        fsr.fetch(str(folder), say=self.said.append, get=self.get)
        game.mkdir()
        (game / "Triforce.exe").write_bytes(b"the game")
        placed = fsr.install_runtime(str(folder), str(game))
        self.assertEqual(sorted(Path(p).name for p in placed), [FRAMEGEN, LOADER, UPSCALER])
        self.assertTrue(all(Path(p).parent == game for p in placed))   # (beside the exe, where gpu/vk/vk_fsr4.cpp looks)
        self.assertTrue((game / fsr.LICENSE_NAME).is_file())
        text = (game / fsr.LICENSE_NAME).read_text(encoding="utf-8", errors="replace")
        self.assertIn("Advanced Micro Devices", text)
        for name in (LOADER, UPSCALER, FRAMEGEN):   # (AMD's text names each file placed, among those under its MIT license)
            self.assertIn("signedbin\\" + name, text)
        self.assertTrue(fsr.remove_runtime(str(game)))
        self.assertEqual([p.name for p in game.iterdir()], ["Triforce.exe"])
        self.assertFalse(fsr.remove_runtime(str(game)))

    def test_make_game_places_and_takes_out(self):
        """nx2/make_game.py's own step: a build with FSR 4 puts the three files and the license text beside the exe; a build
        without it (--no-fsr, or no --fsr) takes all of them out again and fetches nothing"""
        mg_spec = importlib.util.spec_from_file_location("make_game_fsr_under_test", ROOT / "nx2" / "make_game.py")
        make_game = importlib.util.module_from_spec(mg_spec)
        mg_spec.loader.exec_module(make_game)
        work, game = self.dir / "work", self.dir / "game"
        game.mkdir()
        (game / "Triforce.exe").write_bytes(b"the game")
        fsr.fetch(str(work / "fsr-sdk"), say=self.said.append, get=self.get)
        with mock.patch.object(make_game, "fsr_script", return_value=fsr), mock.patch.object(make_game, "LINUX", False), \
                mock.patch.dict("os.environ", {"HOOKSHOT_FSR_SDK": ""}), mock.patch("sys.stdout", io.StringIO()) as out:
            placed = make_game.place_fsr_runtime(str(game), str(work), True)
            self.assertEqual(sorted(Path(p).name for p in placed), [FRAMEGEN, LOADER, UPSCALER])
            self.assertIn(FRAMEGEN, out.getvalue())
            self.assertEqual(sorted(p.name for p in game.iterdir()),
                             ["Triforce.exe", fsr.LICENSE_NAME, FRAMEGEN, LOADER, UPSCALER])
            asked = len(self.asked)
            self.assertEqual(make_game.place_fsr_runtime(str(game), str(work), False), [])
            self.assertEqual([p.name for p in game.iterdir()], ["Triforce.exe"])
            self.assertEqual(len(self.asked), asked)   # (nothing fetched for a build without FSR)

    def test_real_table(self):
        self.assertEqual([name for name, _, _ in self.saved], [LOADER, UPSCALER])
        self.assertEqual([name for name, _, _ in self.saved_optional], [FRAMEGEN])
        for name, size, digest in self.saved + self.saved_optional:
            self.assertGreater(size, 30000, name)
            self.assertRegex(digest, r"^[0-9a-f]{64}$", name)
        self.assertTrue(Path(fsr.LICENSE_SOURCE).is_file())
        # (one release for all three: the tag of the headers in third_party/fidelityfx, AMD's folder of signed libraries)
        self.assertEqual(fsr.BASE, "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/")
        self.assertIn("v2.4.0", (ROOT / "third_party" / "fidelityfx" / "README.md").read_text(encoding="utf-8"))
        # (the name the game looks for beside AMD's loader)
        self.assertIn('kFgFile = "%s"' % FRAMEGEN, (ROOT / "gpu" / "vk" / "vk_fsr4.cpp").read_text(encoding="utf-8"))

    def test_wired(self):
        src = (ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")
        for needle in ("fetch_fsr_runtime(work)", 'place_fsr_runtime(out, work, os.environ.get("HOOKSHOT_FSR") == "1")',
                       "fsr.remove_runtime(out)"):
            self.assertIn(needle, src)
        shell = (ROOT / "setup" / "Shell.cs").read_text(encoding="utf-8")
        self.assertIn('args.Add("--fsr")', shell)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
