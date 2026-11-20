"""scripts/renodx_shaders.py: its pinned tables, the SHA-256 checks and when the shaders are compiled again, without the
network or the Slang compiler; and gpu/vk/vk_renodx.cpp's two halves (with RenoDX's shaders, and the stubs without).
(Triforce) And the glue for RenoDRT and the gamma correction (gpu/vk/shaders/renodx_glue): what is compiled for each shader,
the compiler's command, and the header's stamp following the glue's bytes. With HOOKSHOT_RENODX set to a build's folder of
RenoDX's sources and the Slang compiler, the shaders are compiled with the glue for real; without it that one check is
skipped.

Run with: python -I tests/test_renodx_shaders.py
"""
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import re
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("renodx_shaders_under_test", ROOT / "scripts" / "renodx_shaders.py")
renodx = importlib.util.module_from_spec(spec)
spec.loader.exec_module(renodx)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class Opener:
    """urlopen over a dict of url -> bytes; records what was asked for"""
    def __init__(self, contents):
        self.contents, self.asked = contents, []

    def __call__(self, url, timeout=None):
        if timeout != renodx.TIMEOUT:
            raise AssertionError("urlopen without the timeout (%r)" % (timeout,))
        self.asked.append(url)
        return io.BytesIO(self.contents[url])


class TableTests(unittest.TestCase):
    def test_pinned(self):
        self.assertRegex(renodx.COMMIT, r"^[0-9a-f]{40}$")
        self.assertIn(renodx.COMMIT, renodx.BASE)
        for path, sha in renodx.FILES:
            self.assertRegex(sha, r"^[0-9a-f]{64}$", path)
            self.assertFalse(path.startswith("/") or ".." in path.split("/"), path)
        for platform, (url, sha, compiler) in renodx.SLANG.items():
            self.assertRegex(sha, r"^[0-9a-f]{64}$", platform)
            self.assertIn(renodx.SLANG_VERSION, url)
            self.assertTrue(url.startswith("https://github.com/shader-slang/slang/releases/download/"), url)
            self.assertTrue(compiler.startswith("bin/slangc"), compiler)

    def test_every_shader_is_fetched(self):
        paths = {p for p, _ in renodx.FILES}
        self.assertIn("LICENSE", paths)
        for _, name, kind, _ in renodx.SHADERS:
            self.assertIn(renodx.GAME + "/" + name, paths)
            self.assertIn(kind, (0, 1, 2))
        self.assertEqual(len({crc for crc, *_ in renodx.SHADERS}), len(renodx.SHADERS))


class FetchTests(unittest.TestCase):
    def test_wrong_file_is_not_kept(self):
        with tempfile.TemporaryDirectory() as tmp:
            dst = os.path.join(tmp, "a", "file")
            with self.assertRaises(ValueError):
                renodx.fetch_one("u", digest(b"expected"), dst, Opener({"u": b"something else"}))
            self.assertFalse(os.path.exists(dst))
            self.assertFalse(os.path.exists(dst + ".part"))

    def test_a_file_already_there_is_not_asked_for(self):
        with tempfile.TemporaryDirectory() as tmp:
            dst = os.path.join(tmp, "file")
            opener = Opener({"u": b"data"})
            self.assertTrue(renodx.fetch_one("u", digest(b"data"), dst, opener))
            self.assertFalse(renodx.fetch_one("u", digest(b"data"), dst, opener))
            self.assertEqual(opener.asked, ["u"])

    def test_a_current_header_downloads_and_compiles_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertFalse(renodx.current(tmp))
            with open(renodx.header(tmp), "w", encoding="utf-8") as f:
                f.write("// renodx_shaders.py 0000000000000000\n")   # (another script's: made again)
            self.assertFalse(renodx.current(tmp))
            with open(renodx.header(tmp), "w", encoding="utf-8") as f:
                f.write(renodx.stamp() + "\n")
            self.assertTrue(renodx.current(tmp))
            opener = Opener({})
            self.assertEqual(renodx.prepare(tmp, "windows", opener), renodx.header(tmp))
            self.assertEqual(opener.asked, [])


def bindings(words):
    """A SPIR-V module's (set, binding) pairs: its OpDecorate DescriptorSet and Binding"""
    sets, binds, i = {}, {}, 5
    while i < len(words):
        op, n = words[i] & 0xFFFF, words[i] >> 16
        if op == 71 and words[i + 2] == 34:
            sets[words[i + 1]] = words[i + 3]
        if op == 71 and words[i + 2] == 33:
            binds[words[i + 1]] = words[i + 3]
        i += n
    return sorted((sets[k], binds[k]) for k in sets)


class GlueTests(unittest.TestCase):
    """(Triforce) RenoDRT and the gamma correction: Triforce's own files compiled in front of RenoDX's scene shaders and its
    encoder (gpu/vk/shaders/renodx_glue), and the header's stamp over them"""

    def glue_in(self, tmp):
        """The module's glue folder for a test: a copy in tmp, put back at the test's end"""
        folder = os.path.join(tmp, "glue")
        os.makedirs(folder)
        for name in renodx.GLUE_FILES:
            with open(os.path.join(renodx.GLUE_DIR, name), "rb") as f, open(os.path.join(folder, name), "wb") as out:
                out.write(f.read())
        was = renodx.GLUE_DIR
        self.addCleanup(setattr, renodx, "GLUE_DIR", was)
        renodx.GLUE_DIR = folder
        return folder

    def test_the_glue_is_in_the_tree(self):
        self.assertEqual(Path(renodx.GLUE_DIR).resolve(), (ROOT / "gpu" / "vk" / "shaders" / "renodx_glue").resolve())
        self.assertEqual(sorted(os.listdir(renodx.GLUE_DIR)), sorted(renodx.GLUE_FILES))
        kinds = {kind for _, _, kind, _ in renodx.SHADERS}
        # (the scene shaders and the encoder, by their part; the fog is compiled as RenoDX has it)
        self.assertEqual(renodx.GLUE, {0: "triforce_output.slangi", 2: "triforce_scene.slangi"})
        self.assertTrue(set(renodx.GLUE) < kinds and 1 in kinds)
        for name in renodx.GLUE.values():
            self.assertIn(name, renodx.GLUE_FILES)
        for name in renodx.GLUE_FILES:
            text = (Path(renodx.GLUE_DIR) / name).read_text(encoding="utf-8")
            # (RenoDX's sources are named, never carried: the files are fetched, and the glue includes them from there)
            for included in re.findall(r'#include "([^"]+)"', text):
                self.assertTrue(included in renodx.GLUE_FILES or renodx.GAME + "/" + included in {p for p, _ in renodx.FILES}, included)
            self.assertIn("(Triforce)", text)
            self.assertNotIn("Copyright", text)

    def test_a_unit_is_the_glue_then_renodxs_shader(self):
        for crc, name, kind, _ in renodx.SHADERS:
            text = renodx.unit(name, kind)
            if kind == 1:
                self.assertIsNone(text)
                continue
            lines = text.split("\n")
            self.assertEqual(lines[:2], ["#version 450", "#extension GL_EXT_scalar_block_layout : require"])
            includes = [l for l in lines if l.startswith("#include")]
            self.assertEqual(includes, ['#include "%s"' % renodx.GLUE[kind], '#include "%s"' % name])

    def test_the_compilers_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "src")
            for crc, name, kind, _ in renodx.SHADERS:
                command, spv = renodx.compile_command("slangc", name, kind, src, tmp, crc)
                self.assertEqual(spv, os.path.join(tmp, "%08X.spv" % crc))
                if kind == 1:   # (as before the glue: RenoDX's shader by its name, in its folder)
                    self.assertEqual(command, ["slangc", name] + renodx.FLAGS + ["-o", spv])
                    continue
                path = command[1]
                self.assertEqual(command, ["slangc", path, "-I", src, "-I", renodx.GLUE_DIR] + renodx.FLAGS + ["-o", spv])
                self.assertEqual(os.path.dirname(path), tmp)
                with open(path, encoding="utf-8") as f:
                    self.assertEqual(f.read(), renodx.unit(name, kind))

    def test_the_stamp_follows_the_glue(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = renodx.stamp()
            folder = self.glue_in(tmp)
            self.assertEqual(renodx.stamp(), first)   # (the same files elsewhere: the same stamp)
            for name in renodx.GLUE_FILES:
                path = os.path.join(folder, name)
                with open(path, "rb") as f:
                    data = f.read().replace(b"\r\n", b"\n")
                with open(path, "wb") as f:   # (a checkout with CRLF is the same glue)
                    f.write(data.replace(b"\n", b"\r\n"))
                self.assertEqual(renodx.stamp(), first, name)
                with open(path, "wb") as f:   # (a change to any of the three, the one the others include too)
                    f.write(data + b"// changed\n")
                changed = renodx.stamp()
                self.assertNotEqual(changed, first, name)
                # (and a header with the stamp from before is made again)
                with open(renodx.header(tmp), "w", encoding="utf-8") as f:
                    f.write(first + "\n")
                self.assertFalse(renodx.current(tmp), name)
                with open(path, "wb") as f:
                    f.write(data)
                self.assertEqual(renodx.stamp(), first, name)
                self.assertTrue(renodx.current(tmp), name)
            # (which shader gets which glue is part of it)
            was = dict(renodx.GLUE)
            self.addCleanup(setattr, renodx, "GLUE", was)
            renodx.GLUE = {2: "triforce_scene.slangi"}
            self.assertNotEqual(renodx.stamp(), first)

    def test_the_header_says_it_has_the_glue(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "renodx", *renodx.GAME.split("/"))
            os.makedirs(src)
            ran = []

            class Done:
                returncode, stdout = 0, b""

            def run(command, cwd, stdout, stderr):
                """slangc's stand-in: four words for each shader, the fourth its place in the list"""
                self.assertEqual(cwd, src)
                ran.append(command)
                with open(command[command.index("-o") + 1], "wb") as f:
                    f.write(struct.pack("<4I", 0x07230203, 0x00010500, 0, len(ran)))
                return Done()

            out = os.path.join(tmp, "out", renodx.HEADER)
            renodx.compile_header(os.path.join(tmp, "renodx"), "slangc", out, say=lambda line: None, first_line=renodx.stamp(), run=run)
            text = Path(out).read_text(encoding="utf-8")
            self.assertEqual(text.split("\n")[0], renodx.stamp())
            self.assertIn("\n#define RENODX_SPV_GLUE 1\n", text)
            self.assertEqual(len(ran), len(renodx.SHADERS))
            for (crc, name, kind, _), command in zip(renodx.SHADERS, ran):
                self.assertEqual("-I" in command, kind in renodx.GLUE, name)
                self.assertIn("static const uint32_t RENODX_%08X[] = {" % crc, text)
            # (every checksum a game shader has names its replacement: the four and the other four, ALSO)
            rows = re.findall(r"^    \{0x([0-9A-F]{8})u, RENODX_([0-9A-F]{8}), sizeof RENODX_\2 / 4, (\d)\},$", text, re.M)
            self.assertEqual(len(rows), len(renodx.SHADERS) + sum(len(v) for v in renodx.ALSO.values()))
            kinds = {"%08X" % crc: kind for crc, _, kind, _ in renodx.SHADERS}
            for crc, array, kind in rows:
                self.assertEqual(int(kind), kinds[array])
        source = (ROOT / "gpu" / "vk" / "vk_renodx.cpp").read_text(encoding="utf-8")
        self.assertRegex(source, r"#ifndef RENODX_SPV_GLUE\n(.*\n)*?\s+logf\(")

    @unittest.skipUnless(os.path.isdir(os.path.join(os.environ.get("HOOKSHOT_RENODX", ""), "renodx")) and
                         os.path.isdir(os.path.join(os.environ.get("HOOKSHOT_RENODX", ""), "slang")),
                         "HOOKSHOT_RENODX is not a folder with RenoDX's sources and the Slang compiler (a build's renodx folder)")
    def test_the_glue_compiles_with_renodxs_sources(self):
        """Where a build's folder is at hand (nothing is downloaded, and the folder is only read): the four shaders compile
        with the glue, the three with glue read its settings at set 1 binding 5 and otherwise bind what RenoDX's shader
        alone binds, and the fog is RenoDX's alone"""
        folder = os.environ["HOOKSHOT_RENODX"]
        compiler = renodx.slangc(folder, renodx.host_platform())
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, renodx.HEADER)
            renodx.compile_header(os.path.join(folder, "renodx"), compiler, out, say=lambda line: None)
            text = Path(out).read_text(encoding="utf-8")
            glued = {crc: [int(w, 16) for w in re.findall(r"0x[0-9a-f]{8}", body)]
                     for crc, body in re.findall(r"RENODX_([0-9A-F]{8})\[\] = \{(.*?)\};", text, re.S)}
            was = dict(renodx.GLUE)
            self.addCleanup(setattr, renodx, "GLUE", was)
            renodx.GLUE = {}
            renodx.compile_header(os.path.join(folder, "renodx"), compiler, out, say=lambda line: None)
            text = Path(out).read_text(encoding="utf-8")
            alone = {crc: [int(w, 16) for w in re.findall(r"0x[0-9a-f]{8}", body)]
                     for crc, body in re.findall(r"RENODX_([0-9A-F]{8})\[\] = \{(.*?)\};", text, re.S)}
        for crc, name, kind, _ in renodx.SHADERS:
            key = "%08X" % crc
            if kind in was:
                self.assertEqual(bindings(glued[key]), sorted(bindings(alone[key]) + [(1, 5)]), name)
            else:
                self.assertEqual(glued[key], alone[key], name)


class SourceTests(unittest.TestCase):
    def test_the_renderer_builds_without_the_shaders(self):
        source = (ROOT / "gpu" / "vk" / "vk_renodx.cpp").read_text(encoding="utf-8")
        with_shaders, stubs = source.split("#else", 1)
        self.assertIn('#ifdef HOOKSHOT_RENODX', with_shaders)
        self.assertLess(with_shaders.index("#ifdef HOOKSHOT_RENODX"), with_shaders.index('#include "renodx_spv.h"'))
        header = (ROOT / "gpu" / "vk" / "vk_renodx.h").read_text(encoding="utf-8")
        declared = set(re.findall(r"^(?:[\w:<>*&]+ )+\*?(renodx_\w+)\(", header, re.M))
        self.assertTrue(declared)
        for name in declared:   # (every function of the header is in both halves)
            self.assertIn(name + "(", with_shaders, name)
            self.assertIn(name + "(", stubs, name)
        self.assertIn("bool renodx_built() { return false; }", stubs)

    def test_the_compiled_shaders_are_never_committed(self):
        ignored = (ROOT / ".gitignore").read_text(encoding="utf-8").split("\n")
        self.assertIn(renodx.HEADER, ignored)
        self.assertFalse((ROOT / "gpu" / "vk" / renodx.HEADER).exists(),
                         "gpu/vk/renodx_spv.h is in the tree: the build takes HOOKSHOT_RENODX's, and this one would be included first")


if __name__ == "__main__":
    unittest.main()
