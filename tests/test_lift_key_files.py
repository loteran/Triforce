"""The lift key leaves out only files the lift cannot read (nx2/make_game.py LIFT_KEY_NOT, lift_key_file).

The key decides whether a build translates the game again. It held every file of nx2/, so a change to the build driver, to
the version or to a test translated the whole game again on every player's PC (2:48 of a 3:05 update build, measured), for
the same C. The files left out now must stay ones the lift cannot reach, or a build would keep a stale translation:

  - none of them is imported by nx2/lift.py or nx2/split_hot.py, directly or through what those import;
  - none of the files those do import names one of them in code (as a module, or as a file name outside a comment);
  - every other file of nx2/ is still in the key, and lift_key() really leaves these out and no others.

    python -I -B tests/test_lift_key_files.py
"""
import ast
import io
import os
from modulefinder import ModuleFinder
from pathlib import Path
import sys
import tempfile
import tokenize
import unittest

ROOT = Path(__file__).resolve().parents[1]
NX2 = ROOT / "nx2"
KIT = ROOT / "third_party" / "a64-kit"
SOURCE = NX2 / "make_game.py"


def production():
    """LIFT_KEY_NOT, lift_key_file and lift_key themselves, cut out of make_game.py"""
    tree = ast.parse(SOURCE.read_text(encoding="utf-8"), filename=str(SOURCE))
    body = [n for n in tree.body
            if (isinstance(n, ast.Assign) and any(isinstance(x, ast.Name) and x.id == "LIFT_KEY_NOT" for x in n.targets))
            or (isinstance(n, ast.FunctionDef) and n.name in ("lift_key_file", "lift_key"))]
    assert len(body) == 3, [getattr(n, "name", "LIFT_KEY_NOT") for n in body]
    import hashlib
    scope = dict(os=os, sys=sys, hashlib=hashlib, HERE=str(NX2), ROOT=str(ROOT))
    exec(compile(ast.Module(body=body, type_ignores=[]), str(SOURCE), "exec"), scope)
    return scope


def nx2_files():
    out = []
    for d, dirs, fs in os.walk(NX2):
        dirs[:] = sorted(x for x in dirs if x != "__pycache__")
        out += [os.path.relpath(os.path.join(d, f), NX2).replace(os.sep, "/") for f in sorted(fs)]
    return out


def lift_imports():
    """The files of nx2/ and of a64-kit that the lift's two programs import, by their static imports"""
    seen = set()
    for program in ("lift.py", "split_hot.py"):
        finder = ModuleFinder(path=[str(NX2), str(KIT / "tools"), str(KIT), str(KIT / "lift")] + sys.path)
        finder.run_script(str(NX2 / program))
        for module in finder.modules.values():
            if module.__file__ and Path(module.__file__).resolve().is_relative_to(ROOT):
                seen.add(Path(module.__file__).resolve())
    return seen


def code_words(path):
    """A Python file's names and the text of its strings, without its comments and docstrings-as-comments kept apart:
    (names, strings)"""
    names, strings = set(), []
    with open(path, "rb") as f:
        for tok in tokenize.tokenize(f.readline):
            if tok.type == tokenize.NAME:
                names.add(tok.string)
            elif tok.type == tokenize.STRING:
                strings.append(tok.string)
    return names, strings


class LiftKeyFiles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scope = production()
        keep = cls.scope["lift_key_file"]
        cls.keep = staticmethod(keep)
        cls.files = nx2_files()
        cls.left_out = [f for f in cls.files if not keep(f)]
        cls.imported = lift_imports()

    def test_what_is_left_out(self):
        # (the driver, the other steps, the version, the prompts and the tests: nothing of the translator)
        for name in self.scope["LIFT_KEY_NOT"]:
            self.assertTrue((NX2 / name).is_file(), "%s is in LIFT_KEY_NOT and not in nx2/" % name)
            self.assertFalse(self.keep(name), name)
        for f in self.left_out:
            name = f.rsplit("/", 1)[-1]
            self.assertTrue(f in self.scope["LIFT_KEY_NOT"] or f.startswith("prompt_faces/") or name.startswith("test_"), f)
        for f in ("lift.py", "split_hot.py", "chunks.py", "standalone.py", "oot_patches.py", "lifted_c.py", "kit.py", "rt/mkw_host.h",
                  "testing.py", "contest_x.py", "rt/test_x.c"):
            self.assertTrue(self.keep(f), f)

    def test_the_lift_imports_none_of_them(self):
        self.assertIn((NX2 / "chunks.py").resolve(), self.imported)   # (the finder did follow the lift's imports)
        self.assertIn((NX2 / "standalone.py").resolve(), self.imported)
        for f in self.left_out:
            self.assertNotIn((NX2 / f).resolve(), self.imported, "the lift imports nx2/%s, which the lift key leaves out" % f)

    def test_the_lifts_files_name_none_of_them_in_code(self):
        # (a file opened or started by its name, or a module imported by a name made at run time, would not show as an
        # import: none of the lift's own files has such a name in a string, or the module's as a name in code)
        modules = {f[:-3] for f in self.left_out if f.endswith(".py") and "/" not in f}
        for path in sorted(self.imported):
            names, strings = code_words(path)
            docs = {ast.get_docstring(n, clean=False) for n in ast.walk(ast.parse(path.read_text(encoding="utf-8")))
                    if isinstance(n, (ast.Module, ast.FunctionDef, ast.ClassDef, ast.AsyncFunctionDef))}
            docs = {d for d in docs if d}
            for module in sorted(modules):
                if module in ("build", "version", "nup"):   # (ordinary words, used as local names: their imports are checked above)
                    continue
                self.assertNotIn(module, names, "%s uses the name %s" % (path.name, module))
            for text in strings:
                try:
                    value = ast.literal_eval(text)
                except (SyntaxError, ValueError):
                    continue   # (an f-string: its fixed parts are checked below as text)
                if not isinstance(value, str) or value in docs:
                    continue
                for f in self.left_out:
                    self.assertNotIn(f.rsplit("/", 1)[-1], value, "%s has %s in a string" % (path.name, f))

    def test_the_key_changes_with_the_translator_only(self):
        # (lift_key itself over a copy of the tree's names: a change to a file left out keeps the key, one to a kept file
        # changes it)
        scope = self.scope
        with tempfile.TemporaryDirectory(prefix="lift-key-") as tmp:
            root = Path(tmp)
            here = root / "nx2"
            (root / "third_party" / "a64-kit").mkdir(parents=True)
            (here / "prompt_faces").mkdir(parents=True)
            exefs, work = root / "exefs", root / "work"
            exefs.mkdir(), work.mkdir()
            (exefs / "main").write_bytes(b"game code")
            names = ["lift.py", "chunks.py", "make_game.py", "build.py", "nup.py", "version.py", "shader_census.py",
                     "prompt_assets.py", "prompt_draw.py", "prompt_faces.py", "prompt_requirements.txt", "prompt_keys.py",
                     "test_nup_paths.py", "prompt_faces/readme.txt", "code_mods.py"]
            for n in names:
                (here / n).write_text("# %s\n" % n, encoding="utf-8")
            (root / "third_party" / "a64-kit" / "a64_lift.py").write_text("# kit\n", encoding="utf-8")
            scope.update(HERE=str(here), ROOT=str(root))
            # (lift_key imports kit and standalone for the HLE list: stand-ins, as the list is not what is tested)
            import types
            sys.modules["kit"] = types.ModuleType("kit")
            standalone = types.ModuleType("standalone")
            standalone.implemented = lambda: ["nn::os::A", "nn::os::B"]
            sys.modules["standalone"] = standalone
            try:
                key = lambda: scope["lift_key"](str(exefs), str(work), {"OOT_WORK": "w"})
                first = key()
                self.assertEqual(first, key())
                for n in names:
                    (here / n).write_text("# %s, changed\n" % n, encoding="utf-8")
                    changed = key() != first
                    self.assertEqual(changed, n in ("lift.py", "chunks.py"), n)
                    (here / n).write_text("# %s\n" % n, encoding="utf-8")
                    self.assertEqual(key(), first, n)
                (root / "third_party" / "a64-kit" / "a64_lift.py").write_text("# kit, changed\n", encoding="utf-8")
                self.assertNotEqual(key(), first)
            finally:
                del sys.modules["kit"], sys.modules["standalone"]
                scope.update(HERE=str(NX2), ROOT=str(ROOT))


if __name__ == "__main__":
    unittest.main()
