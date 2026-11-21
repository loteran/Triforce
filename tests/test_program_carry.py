"""The program cache carried over an update of the translator (gpu/vk/program_carry.h has the rule; no game, no GPU).

A kept program is used by another build of the translator only when the two have the same translation stamp (nx2/build.py
translation_stamp, HOOKSHOT_TRANSLATION_STAMP): the translator's files without vk_api.h, without the parts of vk_shader.cpp and
vk_bank.cpp marked as outside it, and without comments, blank lines and the spaces at lines' ends. These checks hold that rule:

- the stamp, by nx2/build.py's own functions run on authored files: every edit that can change a translation changes it (code
  anywhere else, a literal that looks like a comment, a number with digit separators, an #include's name, a raw string, the
  line after a backslash, a mark that does not pair), and the edits that cannot do not (vk_api.h, the marked parts, comments,
  blank lines, spaces at a line's end, CRLF), while the whole translator's stamp still changes with all of them but CRLF;
- every C and C++ file of the translator in this repository, as the stamp takes it, is the same to clang's lexer as the file
  itself, comments aside: the same tokens, starting the same lines (skipped without clang);
- vk_shader.cpp's marked parts: the five reviewed ones, each between definitions, none touching anything of a translation (Eden,
  the translation's thread state, a record's fields), the rest of the file taking from them only the names listed below with
  why each cannot change a translation, make_module taking its SPIR-V const; vk_api.h's names nowhere in the translator and the
  host's functions it calls only those whose answers are in a record's key or the stamp;
- vk_bank.cpp's marked parts: the reviewed ones, each between definitions, defining nothing that stamped code names (vk_shader.cpp's
  and the other stamped files' calls, the stamped vk_bank.cpp functions' callees and shared state) and naming nothing a translation
  reaches (what those calls reach through vk_bank.cpp's stamped functions: texture_of's registrations, which texture_released
  and the registrations write, stay in the stamp); the check itself finds such parts in vk_bank.cpp edited so;
- the build's record of the stamp: the build log names the stamped files that changed it against the last release's
  (nx2/released_translation_stamp.json, which this source's stamp equals) and, as a note, against the previous build's;
- the stamp's code: disk_stamp takes the translation stamp with the carry, the whole one without it or in a build without the
  translation stamp; disk_init lists records only for a file of this very stamp; the switch is documented;
- program_carry.h's verdicts, compiled and run, and disk_init cut out of vk_shader.cpp and run on cache files authored here: a
  file of this build kept, one of another build for the same stamp carried (its head then names this build), one of another
  stamp thrown away with its keys counted for the log's "translated again", a new one made with its head.

    python -I -B tests/test_program_carry.py [--cxx clang++]
"""
import argparse
import ast
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "nx2" / "build.py"
VK = ROOT / "gpu" / "vk"
COMPILER = None
OUT_MARK = "// ---- (Triforce) Outside the translation stamp"
IN_MARK = "// ---- (Triforce) Inside the translation stamp again"
C_LIKE = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc")


def build_functions():
    """nx2/build.py's stamp functions and its whole stamp's block, to run here"""
    tree = ast.parse(BUILD.read_text(encoding="utf-8"), filename=str(BUILD))
    names = {"strip_comments", "translation_text", "translation_stamp", "translation_files", "translation_record_read",
             "translation_changed", "translation_stamp_note", "translation_stamp_keep"}
    assigns = lambda n, name: isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in n.targets)
    functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names or assigns(n, "RELEASED_STAMP")]
    assert {n.name for n in functions if isinstance(n, ast.FunctionDef)} == names and len(functions) == len(names) + 1
    ns = {"os": os, "hashlib": hashlib, "json": json, "__file__": str(BUILD)}
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(BUILD), "exec"), ns)
    branch = [n for n in ast.walk(tree) if isinstance(n, ast.If) and any(assigns(s, "translator") for s in n.body)]
    assert len(branch) == 1
    body = [n for n in branch[0].body if assigns(n, "stamp") or assigns(n, "translator") or isinstance(n, ast.For) and
            isinstance(n.iter, ast.BinOp) and any(isinstance(x, ast.Name) and x.id == "translator" for x in ast.walk(n.iter))]
    assert len(body) == 3, "expected the whole stamp's hash, its translator list and its file loop"
    whole = compile(ast.Module(body=body, type_ignores=[]), str(BUILD), "exec")
    return ns, whole


NS, WHOLE = build_functions()


def translator_files(vk):
    """The translator's files as nx2/build.py lists them for its stamps (eden, sirit, compat; then the four of gpu/vk)"""
    files = [os.path.join(d, f) for top in ("eden", "sirit", "compat") for d, _, fs in os.walk(os.path.join(vk, top)) for f in fs]
    return sorted(files), [os.path.join(vk, n) for n in ("vk_shader.cpp", "vk_bank.cpp", "vk_state.h", "vk_api.h")]


def stamped(path, vk=VK):
    """A file's text as the translation stamp takes it"""
    rel = os.path.relpath(path, vk).replace("\\", "/")
    return NS["translation_text"](rel, Path(path).read_bytes()).decode("utf-8")


def blank_literals(text):
    return re.sub(r'"(?:\\.|[^"\\\n])*"', '""', re.sub(r"'(?:\\.|[^'\\\n])'", "''", text))


def marked_sections(text):
    """The parts of a file between its marks (the mark lines left out), in order"""
    sections, cur = [], None
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith(OUT_MARK):
            cur = []
        elif s.startswith(IN_MARK):
            sections.append("\n".join(cur))
            cur = None
        elif cur is not None:
            cur.append(line)
    return sections


def defined_names(section):
    """The names a part defines at namespace scope: functions, variables (each of a list), structs, classes, usings"""
    body = blank_literals(NS["strip_comments"](section.encode())[0].decode())
    names = []
    for line in body.split("\n"):
        if not line or line[0] in " \t}#" or line.startswith(("namespace", "template")):
            continue
        m = re.match(r"(?:struct|class|enum(?:\s+class)?)\s+(\w+)", line) or re.match(r"using\s+(\w+)\s*=", line)
        if m:
            names.append(m.group(1))
            continue
        stops = [i for i in (line.find("("), line.find("="), line.find("{"), line.find(";")) if i >= 0]
        first = min(stops) if stops else len(line)
        if first < len(line) and line[first] == "(":
            m = re.search(r"([\w:~]+)\s*$", line[:first])
            if m:
                names.append(m.group(1).split("::")[-1])
            continue
        decl, depth, piece, pieces = line.split(";")[0], 0, "", []
        for ch in decl:
            depth += (ch in "<({[") - (ch in ">)}]")
            if ch == "," and depth == 0:
                pieces.append(piece)
                piece = ""
            else:
                piece += ch
        pieces.append(piece)
        for p in pieces:
            m = re.search(r"(\w+)\s*$", re.split(r"[={\[]", p)[0])
            if m:
                names.append(m.group(1))
    return names


def function(text, head):
    at = text.index(head)
    return text[at:text.index("\n}\n", at)]


# ---- the stamp, on authored files ----

SHADER = """// A note on the file.
#include <a//b.h>
#define TWO 1 + \\
    1
int code = 1'000;   // (a note)
const char* s = "a//b /* c */";
const char c = '/';
/* a block
   comment */ int after = 2;
// ---- (Triforce) Outside the translation stamp, to the next mark: authored
// what it is
int not_translation = 3;
// ---- (Triforce) Inside the translation stamp again ----
int translation = 4;
"""
RAW = """int raw_before = 0;
const char* r = R"x(raw // text
  )x";   // (a note)
int raw_after = 1;
"""


class StampTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="triforce-carry-stamp-")
        self.addCleanup(self.temp.cleanup)
        self.vk = Path(self.temp.name) / "gpu" / "vk"
        self.files = {"eden/a.cpp": "int a = 1;\n", "eden/raw.cpp": RAW, "sirit/b.h": "struct B {};\n", "sirit/notes.txt": "notes\n",
                      "compat/c.h": "struct C {};\n", "vk_shader.cpp": SHADER, "vk_bank.cpp": "int bank;\n",
                      "vk_state.h": "struct State {};\n", "vk_api.h": "enum { HOS_OPT_X = 1 };\n", "vk_draw.cpp": "int draw;\n"}
        self.write(self.files)

    def write(self, files):
        for name, text in files.items():
            path = self.vk / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text.encode())

    def stamps(self):
        ns = {"os": os, "hashlib": hashlib, "vk": str(self.vk)}
        exec(WHOLE, ns)
        eden, _ = translator_files(str(self.vk))
        return ns["stamp"].hexdigest()[:16], NS["translation_stamp"](str(self.vk), eden)

    def edited(self, name, old, new):
        self.write(self.files)
        text = self.files[name]
        self.assertIn(old, text, name)
        self.write({name: text.replace(old, new, 1)})
        try:
            return self.stamps()
        finally:
            self.write(self.files)

    def test_edits_that_can_change_a_translation_change_it(self):
        whole, translation = self.stamps()
        for name, old, new in (
                ("eden/a.cpp", "a = 1", "a = 2"), ("sirit/b.h", "{}", "{ int b; }"), ("compat/c.h", "{}", "{ int c; }"),
                ("sirit/notes.txt", "notes", "notes, not C: taken whole"), ("vk_bank.cpp", "bank", "bank2"),
                ("vk_state.h", "{}", "{ int s; }"), ("vk_shader.cpp", "translation = 4", "translation = 5"),
                ("vk_shader.cpp", '"a//b /* c */"', '"a//b /* d */"'), ("vk_shader.cpp", '"a//b', '"a//c'),
                ("vk_shader.cpp", "'/'", "'*'"), ("vk_shader.cpp", "1'000;", "1'001;"), ("vk_shader.cpp", "1'000;", "1'000 + 0;"),
                ("vk_shader.cpp", "<a//b.h>", "<a//c.h>"), ("vk_shader.cpp", "    1\n", "    2\n"),
                ("vk_shader.cpp", "1 + \\\n", "1 + \\\n\n"),   # (a blank line after a backslash ends the macro there)
                ("vk_shader.cpp", "comment */ int after = 2;", "comment */ int after = 3;"),
                ("vk_shader.cpp", "comment */ int after = 2;", "comment */ // int after = 2;"),
                ("eden/raw.cpp", "raw // text", "raw // texT"), ("eden/raw.cpp", "\n  )x\"", "\n   )x\""),
                ("eden/raw.cpp", "raw_after = 1", "raw_after = 2")):
            with self.subTest(file=name, edit=new):
                w, t = self.edited(name, old, new)
                self.assertNotEqual(t, translation, "an edit that can change a translation kept the translation stamp")
                self.assertNotEqual(w, whole)

    def test_edits_that_cannot_change_a_translation_keep_it(self):
        whole, translation = self.stamps()
        for name, old, new in (
                ("vk_api.h", "HOS_OPT_X = 1", "HOS_OPT_X = 2, HOS_OPT_Y"),
                ("vk_shader.cpp", "not_translation = 3;", "not_translation = 9;\nvoid more() { int x = 1; }"),
                ("vk_shader.cpp", "// what it is", "// what it is now"),
                ("vk_shader.cpp", "// A note on the file.", "// Another note."), ("vk_shader.cpp", "// (a note)", "// (another)"),
                ("vk_shader.cpp", "   // (a note)", ""), ("vk_shader.cpp", "a block", "a longer block"),
                ("vk_shader.cpp", "int translation", "// A new line of notes.\n/* and a block\n   of them */\n\nint translation"),
                ("vk_shader.cpp", "int translation = 4;", "int translation = 4;   \n\n"),
                ("eden/raw.cpp", "// (a note)", "// (another)"), ("eden/a.cpp", "a = 1;", "a = 1;   // (a note)")):
            with self.subTest(file=name, edit=new):
                w, t = self.edited(name, old, new)
                self.assertEqual(t, translation, "an edit that cannot change a translation changed the translation stamp")
                self.assertNotEqual(w, whole, "the whole translator's stamp no longer sees every edit")
        self.write({n: t.replace("\n", "\r\n") for n, t in self.files.items()})
        self.assertEqual(self.stamps(), (whole, translation))
        self.write(self.files)
        self.write({"vk_draw.cpp": "int draw_changed;\n"})
        self.assertEqual(self.stamps(), (whole, translation))

    def test_marks_that_do_not_pair_take_the_file_whole(self):
        lone = self.files["vk_shader.cpp"].replace("// ---- (Triforce) Inside the translation stamp again ----\n", "")
        self.write({"vk_shader.cpp": lone})
        with mock.patch("builtins.print"):
            _, before = self.stamps()
            self.write({"vk_shader.cpp": lone.replace("not_translation = 3", "not_translation = 9")})
            _, after = self.stamps()
        self.assertNotEqual(before, after, "an unpaired mark left code out of the translation stamp")
        twice = self.files["vk_shader.cpp"].replace("int translation", "// ---- (Triforce) Inside the translation stamp again\nint translation")
        self.write({"vk_shader.cpp": twice})
        with mock.patch("builtins.print"):
            _, before = self.stamps()
            self.write({"vk_shader.cpp": twice.replace("not_translation = 3", "not_translation = 9")})
            _, after = self.stamps()
        self.assertNotEqual(before, after, "a second end mark was taken for a pair")

    def test_the_record_names_the_files_that_moved_the_stamp(self):
        """nx2/build.py translation_stamp_note against the last release's record and translation_stamp_keep's: kept, or the
        stamped files that changed it. The release's comes first and stays the reference: a build after one that moved the stamp
        still says it is not the release's"""
        record = str(Path(self.temp.name) / "translation_stamp.json")
        released = str(Path(self.temp.name) / "released_translation_stamp.json")

        def now():
            eden, _ = translator_files(str(self.vk))
            return NS["translation_stamp"](str(self.vk), eden), NS["translation_files"](str(self.vk), eden)

        def note():
            with mock.patch("builtins.print") as said:
                changed = NS["translation_stamp_note"](record, *now(), released=released)
            return changed, "\n".join(" ".join(map(str, c.args)) for c in said.call_args_list)

        stamp, files = now()
        self.assertEqual(sorted(files), ["compat/c.h", "eden/a.cpp", "eden/raw.cpp", "sirit/b.h", "sirit/notes.txt", "vk_bank.cpp",
                                         "vk_shader.cpp", "vk_state.h"], "the record's files are not the translation stamp's")
        changed, said = note()
        self.assertEqual(changed, (None, None))
        self.assertIn("translation stamp %s (no record of the last release's" % stamp, said)
        self.assertIn("no record of the previous build's translation stamp in this folder", said)
        # (the release's record as the repository keeps it: translation_stamp_keep's, with the release's name)
        NS["translation_stamp_keep"](released, stamp, files)
        data = json.loads(Path(released).read_text(encoding="utf-8"))
        data["release"] = "v9.9.9"
        Path(released).write_text(json.dumps(data), encoding="utf-8")
        NS["translation_stamp_keep"](record, stamp, files)
        self.assertFalse(os.path.exists(record + ".tmp"))
        changed, said = note()
        self.assertEqual(changed, ([], []))
        self.assertIn("translation stamp %s, as v9.9.9's: the build's part of the shader caches' stamp is kept" % stamp, said)
        self.assertIn("the translation stamp is the previous build's in this folder", said)
        self.assertNotIn("carry over", said)
        # (what cannot change a translation: kept, and nothing named)
        self.write({"vk_shader.cpp": SHADER.replace("not_translation = 3", "not_translation = 9").replace("// (a note)", "// (b)"),
                    "vk_api.h": "enum { HOS_OPT_X = 2 };\n", "vk_draw.cpp": "int draw2;\n"})
        changed, said = note()
        self.assertEqual(changed, ([], []))
        self.assertIn("as v9.9.9's", said)
        # (code: the files named, the stamp before too)
        self.write(self.files)
        self.write({"vk_bank.cpp": "int bank2;\n", "eden/a.cpp": "int a = 2;\n"})
        moved, _ = now()
        changed, said = note()
        self.assertEqual(changed, (["eden/a.cpp", "vk_bank.cpp"], ["eden/a.cpp", "vk_bank.cpp"]))
        self.assertIn("translation stamp %s, NOT v9.9.9's (%s): at this update a player's program and pipeline caches from "
                      "v9.9.9 start again" % (moved, stamp), said)
        self.assertIn("Changed against it by: eden/a.cpp, vk_bank.cpp", said)
        self.assertIn("in this folder was %s; changed since by: eden/a.cpp, vk_bank.cpp" % stamp, said)
        # (the next build in the folder, nothing changed since: the previous build's, but still NOT the release's)
        NS["translation_stamp_keep"](record, *now())
        changed, said = note()
        self.assertEqual(changed, (["eden/a.cpp", "vk_bank.cpp"], []))
        self.assertIn("NOT v9.9.9's", said)
        self.assertIn("Changed against it by: eden/a.cpp, vk_bank.cpp", said)
        self.assertIn("the translation stamp is the previous build's in this folder", said)
        self.assertNotIn("as v9.9.9's", said)
        # (a file new to the translator, one gone)
        self.write(self.files)
        (self.vk / "eden" / "new.h").write_bytes(b"int n;\n")
        (self.vk / "sirit" / "notes.txt").unlink()
        changed, said = note()
        self.assertEqual(changed[0], ["eden/new.h", "sirit/notes.txt"])
        self.assertIn("Changed against it by: eden/new.h (new), sirit/notes.txt (gone)", said)
        # (more than 12: those and a count)
        _, named = NS["translation_changed"]({}, {"f%02d" % i: "x" for i in range(15)})
        self.assertEqual(named, ", ".join(["f%02d (new)" % i for i in range(12)] + ["3 more"]))
        # (a record cut short, or of something else: none to compare with)
        for text in ("{", "[]", '{"stamp": 1, "files": {}}', '{"files": {}}'):
            with self.subTest(record=text):
                Path(record).write_text(text, encoding="utf-8")
                Path(released).write_text(text, encoding="utf-8")
                self.assertEqual(note()[0], (None, None))

    def test_the_repository_keeps_the_last_release_stamp(self):
        """The translation stamp of this source is the last release's (nx2/released_translation_stamp.json, made from that
        release's source): moving it starts every player's program and pipeline caches again at the update. A change that has
        to move it changes this test with it, saying why; the record itself changes only when a release goes out"""
        record = NS["translation_record_read"](NS["RELEASED_STAMP"])
        self.assertIsNotNone(record, "no record of the last release's translation stamp in nx2")
        rel_stamp, rel_files, release = record
        self.assertEqual(release, "v0.5.1")
        self.assertEqual(rel_stamp, "f6b970031161afbe")
        eden, _ = translator_files(str(VK))
        files = NS["translation_files"](str(VK), eden)
        changed, named = NS["translation_changed"](rel_files, files)
        self.assertEqual(changed, [], "the translation's text differs from %s's in: %s" % (release, named))
        self.assertEqual(NS["translation_stamp"](str(VK), eden), rel_stamp,
                         "this source's translation stamp is not %s's: every player's shader caches start again" % release)

    def test_the_build_keeps_the_record_once_compiled(self):
        src = BUILD.read_text(encoding="utf-8")
        self.assertIn('translation_def = "-DHOOKSHOT_TRANSLATION_STAMP=0x%sull" % translation_now', src)
        self.assertIn("translation_now = translation_stamp(vk, translator)", src)
        self.assertIn("translation_record = (os.path.join(out, TRANSLATION_RECORD), translation_now, translation_files(vk, translator))",
                      src)
        # (said after the back end's jobs are made, outside the branch that makes them: tests/test_shader_stamp_cache.py runs it)
        self.assertLess(src.index("        jobs += vk_jobs\n"), src.index("translation_stamp_note(*translation_record)"))
        # (written once this build's objects are compiled, not by a dry run nor a build that failed)
        keep = src.index("    if build_vk:   # ((Triforce) the translation stamp this build compiled")
        self.assertIn("translation_stamp_keep(*translation_record)", src[keep:keep + 200])
        self.assertLess(src.index("if a.dry_run:"), keep)
        compiled = src.index('print("compiled in %.0fs, %d failed"')
        self.assertLess(src.index("    if still:\n        return 1\n", compiled), keep)
        self.assertLess(keep, src.index("    if EARLY:   # (nothing to link yet"))


class TranslatorFilesTests(unittest.TestCase):
    def test_every_translator_file_lexes_as_before(self):
        """translation_text leaves every token of every C and C++ file of the translator where it was (clang's raw lexer)"""
        clang = shutil.which("clang++") or shutil.which("clang")
        if not clang or b"clang" not in subprocess.run([clang, "--version"], capture_output=True).stdout:
            self.skipTest("no clang")
        eden, vk = translator_files(str(VK))
        before, after = [], []
        for path in eden + vk:
            if not path.endswith(C_LIKE):
                continue
            data = Path(path).read_bytes().replace(b"\r\n", b"\n").decode("utf-8")
            if marked_sections(data):   # (the marked parts left out of both: what is compared is the rest)
                keep, out = [], False
                for line in data.split("\n"):
                    s = line.strip()
                    if s.startswith(OUT_MARK) or s.startswith(IN_MARK):
                        out = s.startswith(OUT_MARK)
                    elif not out:
                        keep.append(line)
                data = "\n".join(keep)
            before.append(data)
            after.append(stamped(path))
        with tempfile.TemporaryDirectory(prefix="triforce-carry-lex-") as tmp:
            def tokens(texts, name):
                path = Path(tmp) / name
                path.write_bytes("\n".join(texts).encode("utf-8"))
                r = subprocess.run([clang, "-Xclang", "-dump-raw-tokens", "-fsyntax-only", "-x", "c++", "-std=c++20", str(path)],
                                   capture_output=True)
                out = []
                for line in r.stderr.decode("utf-8", "replace").splitlines():
                    kind, _, rest = line.partition(" ")
                    at = rest.rfind("Loc=<")
                    if at < 0 or kind == "comment":
                        continue
                    text = rest[rest.find("'") + 1:at].rstrip()[:-1]
                    if kind == "unknown" and not text.replace("\\n", "").replace("\\t", "").strip():
                        continue
                    out.append((kind, text, "[StartOfLine]" in rest[at:]))
                return out
            a, b = tokens(before, "before.cpp"), tokens(after, "after.cpp")
        self.assertGreater(len(a), 100000, "the lexer read too little: is this clang?")
        if a != b:
            at = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
            self.fail("token %d: the stamp's text is not the file's C++ (%r, %r)" % (at, a[at:at + 3], b[at:at + 3]))


# ---- the rule, in vk_shader.cpp ----

# (what the rest of vk_shader.cpp, in the stamp, may take from its marked parts, and why that cannot change a translation)
ALLOWED = {
    "make_module": "a module of a stage's SPIR-V, which it takes const: a record holds the SPIR-V, and a translation needs only "
                   "that its modules are made (a program read back makes them the same way)",
    "program_pack": "a ready program's stages packed, after its record is written; a program read back is packed alike",
    "PackSkip": "whether the modules made on this thread are packed (pack_note): what is kept of a module's SPIR-V in RAM, "
                "never the SPIR-V itself nor the record",
    "stage_module_destroy": "a stage's module let go (vkDestroyShaderModule, its counts and packed store with it), after its "
                            "pipelines are made: the record and the translation are done by then",
    "DiskRecord": "where a record is in the file", "disk_mu": "the file's lock", "disk_in": "the file read",
    "disk_out": "the file written", "disk_idx": "the index written", "disk_records": "the list of records",
    "disk_listing": "whether records written are listed (the precompile)", "disk_on": "whether the cache is there",
    "idx_put": "an index entry, written after its record", "st_cache_hits": "a count for the log",
    "dropped_note": "a count for the log", "disk_init": "the file opened, its records listed when it is of this stamp",
    "program_cache_load": "the records of a key read for its list (program_cache_read, in the stamp, reads each and checks its key)",
    "st_draw_reads": "a count for the stall line", "st_draw_read_ns": "a time for the stall line",
    "st_ra_taken": "a count for the stall line",
    "draw_once": "(Triforce) where a pick lets shader_mu go to read a program object's stages (codes_read): when they are read, "
                 "never what: the same guest memory read and made into the same codes, the lock held or not",
}
# (what a marked part may not name: Eden, the translation's thread state and entry points, a record's fields being set)
DENIED = [r"\bShader::", r"\busing\s+namespace\b", r"\bEnv\b",
          r"\b(?:forced|seed_capture|seed_cs_control|questions_in|questions_out|questions_at|answers_in)\b",
          r"\b(?:translate|translate_into|translate_mesh_into|record_answers|make_profile|profile|profile_graphics|profile_pinned|"
          r"host_info|EmitSPIRV|TranslateProgram|still_valid|answers_hash|graphics_key|mesh_key|code_key)\s*\(",
          r"(?:\.|->)(?:answers|questions|workgroup|shared_memory|info|c1|nvn_stage|vk_stage|key|variant|compute|mesh|fallback|"
          r"fallback_task|stages)\s*(?:=[^=]|\+=|\.push_back|\.emplace_back|\.clear|\.resize)"]
FIRSTS = ["spirv_crc32", "DiskRecord", "disk_init", "ra_mu", "program_cache_load_some"]
HOST_CALLS = {"gpu_read", "gpu_guest", "gpu_ptr", "guest_base", "guest_size", "depth_minus_w"}
# (vk_bank.cpp's marked parts, by the first name each defines: the samplers' generation moved for the texture LOD bias, which only
# vk_draw.cpp's sampler_for reads)
BANK_FIRSTS = ["sampler_bias_changed"]
MARKED = {"vk_shader.cpp": FIRSTS, "vk_bank.cpp": BANK_FIRSTS}
# (what a translation reaches in vk_bank.cpp, at the least: what vk_shader.cpp calls, and what those read)
BANK_REACHED = {"seed_cs_control", "compute_ssbo_table", "seed_cs_buffer_word", "texture_slot_version", "registrations_generation",
                "texture_of", "driver_bank_word", "cs_layout", "cs_layout_of", "CsLayout", "pointer_table", "tex_regs", "TexReg",
                "reg_mu", "slot_versions", "kSlotVersions", "reg_gen"}


def code_of(text):
    """A file's code without comments, literals and preprocessor lines, its marks as @@OUT@@ and @@IN@@"""
    text = "\n".join("@@OUT@@" if l.strip().startswith(OUT_MARK) else "@@IN@@" if l.strip().startswith(IN_MARK) else l
                     for l in text.replace("\r\n", "\n").split("\n"))
    lines, directive = [], False
    for line in blank_literals(NS["strip_comments"](text.encode())[0].decode()).split("\n"):
        directive = directive or line.lstrip().startswith("#")
        lines.append("" if directive else line)
        directive = directive and line.rstrip().endswith("\\")
    return "\n".join(lines)


def mark_scopes(text):
    """For each mark of a file, whether it is at namespace scope (between definitions, never inside one)"""
    code, stack, last, scopes = code_of(text), [], 0, []
    for m in re.finditer(r"[{}]|@@OUT@@|@@IN@@", code):
        if m.group() == "{":
            stack.append(bool(re.search(r"\bnamespace\b[^;{}]*$", code[last:m.start()])))
        elif m.group() == "}":
            stack.pop()
        else:
            scopes.append(all(stack))
        last = m.end()
    return scopes


def top_level_parts(text):
    """A file's definitions at namespace scope, in order, each as (its code, whether it is in the translation stamp): what ends
    at a ';' or a closing brace at namespace scope; a mark and a namespace's braces end one too"""
    code, parts, stack, start, inside = code_of(text), [], [], 0, True

    def part(end):
        if code[start:end].strip(" \t\n;}"):
            parts.append((code[start:end], inside))

    for m in re.finditer(r"@@OUT@@|@@IN@@|[{};]", code):
        t = m.group()
        if t in ("@@OUT@@", "@@IN@@"):
            part(m.start())
            start, inside = m.end(), t == "@@IN@@"
        elif t == "{":
            namespace = all(stack) and bool(re.search(r"\bnamespace\b[^;{}]*$", code[start:m.start()]))
            stack.append(namespace)
            if namespace:
                start = m.end()
        elif t == "}":
            if stack.pop():
                part(m.start())
                start = m.end()
            elif all(stack):
                part(m.end())
                start = m.end()
        elif all(stack):
            part(m.end())
            start = m.end()
    part(len(code))
    return parts


def part_names(part):
    """The names a definition at namespace scope gives: a function's, a type's, each variable's of its declaration"""
    s = " ".join(part.split())
    m = re.match(r"(?:typedef )?(?:struct|class|union|enum(?: class| struct)?) (\w+)", s) or re.match(r"using (\w+) ?=", s)
    if m:
        return [m.group(1)]
    names, depth, angle, head, init = [], 0, 0, "", False
    for ch in s:
        if depth == 0 and angle == 0 and ch in "(=[{,;":
            if not init:
                words = re.findall(r"\w+", head)
                if words:
                    names.append(words[-1])
                if ch == "(":
                    return names[-1:]   # (a function: the name before its parameters)
            if ch == ";":
                break
            if ch == ",":
                init, head = False, ""
                continue
            init = True
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif depth == 0 and not init:
            if ch == "<":
                angle += 1
            elif ch == ">":
                angle = max(angle - 1, 0)
            elif angle == 0:
                head += ch
    return list(dict.fromkeys(names))


def stamped_words(vk=VK, leave_out="vk_bank.cpp"):
    """The names the translation stamp's text of the translator's C and C++ files uses (but leave_out's, and vk_api.h): every
    name in it, but what a declaration without a body at namespace scope declares (vk_state.h's prototypes of vk_bank.cpp's
    functions use none of them)"""
    eden, files = translator_files(str(vk))
    words = set()
    for path in eden + files[:-1]:
        if path.endswith(C_LIKE) and os.path.basename(path) != leave_out:
            for code, _ in top_level_parts(stamped(path, vk)):
                used = set(re.findall(r"\b\w+\b", code))
                words |= used if "{" in code else used - set(part_names(code))
    return words


def bank_check(text, outside_words):
    """vk_bank.cpp's marked parts against the rule: (what a translation reaches in the file, what is wrong). Stamped code (the
    other files' words, outside_words, and vk_bank.cpp's own stamped parts) names nothing a marked part defines; a marked part names
    nothing a translation reaches: what the other files name of vk_bank.cpp, and what that names through its definitions, in turn"""
    parts = top_level_parts(text)
    defs = {}
    for code, inside in parts:
        for name in part_names(code):
            defs.setdefault(name, []).append(set(re.findall(r"\b\w+\b", code)))
    reached, todo = set(), sorted(set(defs) & outside_words)
    while todo:
        name = todo.pop()
        if name not in reached:
            reached.add(name)
            for words in defs[name]:
                todo += sorted((words & set(defs)) - reached)
    stamped_code = set(outside_words)
    for code, inside in parts:
        if inside:
            stamped_code |= set(re.findall(r"\b\w+\b", code))
    # (and what changes what a translation reaches through a call: the stamped definitions that name it, and in turn those that
    # name them. A marked part that calls texture_released writes texture_of's registrations as surely as one that names tex_regs)
    writers = set(reached)
    grew = True
    while grew:
        grew = False
        for code, inside in parts:
            if inside:
                own = set(part_names(code)) - writers
                if own and set(re.findall(r"\b\w+\b", code)) & writers:
                    writers |= own
                    grew = True
    problems = []
    for code, inside in parts:
        if inside:
            continue
        own, words = part_names(code), set(re.findall(r"\b\w+\b", code))
        first = own[0] if own else code.strip()[:40]
        problems += ["%s, outside the stamp (the part from %s), is named by stamped code" % (n, first) for n in own if n in stamped_code]
        problems += ["the part from %s, outside the stamp, names %s, which a translation reaches" % (first, n)
                     for n in sorted((words & reached) - set(own))]
        problems += ["the part from %s, outside the stamp, names %s, which changes what a translation reaches" % (first, n)
                     for n in sorted((words & writers) - reached - set(own))]
    return reached, problems


def marked_out(text, head, end="\n}\n"):
    """text with the definition that starts at head marked as outside the stamp (to see that bank_check finds it)"""
    at = text.index("\n" + head) + 1
    stop = text.index(end, at) + len(end)
    return (text[:at] + OUT_MARK + ", authored ----\n" + text[at:stop] + IN_MARK + " ----\n" + text[stop:])


class RuleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
        cls.sections = marked_sections(cls.shader)
        cls.stamped = stamped(VK / "vk_shader.cpp")

    def test_marks_only_where_reviewed_and_between_definitions(self):
        eden, vk = translator_files(str(VK))
        for path in eden + vk:
            if os.path.basename(path) not in MARKED or os.path.dirname(path) != str(VK):
                text = Path(path).read_text(encoding="utf-8", errors="replace")
                self.assertNotIn("Outside the translation stamp", text, path)
                self.assertNotIn("Inside the translation stamp again", text, path)
        for name, firsts in MARKED.items():
            with self.subTest(file=name):
                text = (VK / name).read_text(encoding="utf-8").replace("\r\n", "\n")
                self.assertEqual([defined_names(s)[0] for s in marked_sections(text)], firsts,
                                 "the marked parts are not the reviewed ones: a new one is reviewed here (vk_shader.cpp: ALLOWED, "
                                 "DENIED, FIRSTS; vk_bank.cpp: BANK_FIRSTS, bank_check)")
                # (each mark at namespace scope: the marked parts are whole definitions, never part of a function)
                scopes = mark_scopes(text)
                self.assertEqual(len(scopes), 2 * len(firsts))
                self.assertTrue(all(scopes), "a mark inside a definition (marks %s)" % scopes)
        self.assertFalse(all(mark_scopes("namespace a {\nvoid f() {\n" + OUT_MARK + "\n}\n" + IN_MARK + "\n}\n")),
                         "the scope check does not see a mark inside a function")

    def test_marked_parts_touch_nothing_of_a_translation(self):
        bank = (VK / "vk_bank.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
        for section, first in list(zip(self.sections, FIRSTS)) + list(zip(marked_sections(bank), BANK_FIRSTS)):
            code = blank_literals(NS["strip_comments"](section.encode())[0].decode())
            for pattern in DENIED:
                found = re.search(pattern, code)
                self.assertIsNone(found, "the marked part from %s names %r: that belongs in the translation stamp" %
                                  (first, found and found.group()))

    def test_bank_marked_parts_hold_nothing_a_translation_reads(self):
        """vk_bank.cpp's marked parts (bank_check): nothing stamped code names, nothing a translation reaches"""
        if not BANK_FIRSTS:
            self.skipTest("vk_bank.cpp has no marked parts on this branch (the texture LOD bias is not in it)")
        bank = (VK / "vk_bank.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
        outside = stamped_words()
        reached, problems = bank_check(bank, outside)
        self.assertEqual(problems, [])
        self.assertLessEqual(BANK_REACHED, reached, "the check no longer sees what a translation reaches in vk_bank.cpp")
        for name in ("sampler_bias_changed", "samp_gen", "register_texture_copy", "texture_released", "register_sampler_copy"):
            self.assertNotIn(name, reached, "a translation reaches %s: then what writes it stays in the stamp" % name)
        self.assertNotIn("sampler_bias_changed", stamped(VK / "vk_bank.cpp"))
        self.assertIn("bool texture_of(", stamped(VK / "vk_bank.cpp"))
        names = {n for code, _ in top_level_parts(bank) for n in part_names(code)}
        self.assertLessEqual({"slot_versions", "kPointerTable", "kPointerEntries", "regs_by_serial", "PointerEntry", "SampReg",
                              "driver_bank_fill", "sampler_bias_changed"}, names, "the definitions are not all read")
        # (the check finds a part marked out that it should not be: what writes texture_of's registrations, what a translation
        # calls, what a stamped function calls or reads, what the marked part shares with stamped code)
        unmarked = bank.replace(OUT_MARK, "// ").replace(IN_MARK, "// ")
        self.assertEqual(bank_check(unmarked, outside)[1], [])
        for head, end, found in (
                ("void texture_released(uint64_t serial) {", "\n}\n", "names tex_regs, which a translation reaches"),
                ("void register_texture_copy(int id, const nvn_tex& tex, const nvn_view* v) {", "\n}\n", "names reg_gen"),
                ("bool texture_of(int id,", "\n}\n", "texture_of, outside the stamp"),
                ("CsLayout cs_layout(const NvnState& s) {", "\n}\n", "cs_layout, outside the stamp"),
                ("std::atomic<uint64_t> reg_gen{1};", "\n", "reg_gen, outside the stamp"),
                ("std::atomic<uint64_t> samp_gen{1};", "\n", "samp_gen, outside the stamp (the part from samp_gen), is named"),
                ("uint64_t sampler_registrations_generation() {", "\n}\n", None)):
            with self.subTest(marked_out=head):
                problems = bank_check(marked_out(unmarked, head, end), outside)[1]
                if found is None:   # (vk_draw.cpp's alone: no stamped code names it)
                    self.assertEqual(problems, [])
                else:
                    self.assertTrue(any(found in p for p in problems), problems)
        # (and a marked part that calls what writes texture_of's registrations, rather than naming them itself)
        bias = "void sampler_bias_changed() { samp_gen.fetch_add(1, std::memory_order_acq_rel); }"
        self.assertIn(bias, bank)
        for call, found in (("texture_released(0);", "names texture_released, which changes what a translation reaches"),
                            ("register_texture_copy(0, {}, nullptr);", "names register_texture_copy, which changes"),
                            ("register_texture(0, {}, nullptr);", "names register_texture, which changes")):
            with self.subTest(calls=call):
                edited = bank.replace(bias, bias[:-1] + call + " }")
                problems = bank_check(edited, outside)[1]
                self.assertTrue(any(found in p for p in problems), problems)

    def test_the_rest_takes_only_what_is_listed(self):
        words = set(re.findall(r"\b\w+\b", blank_literals(self.stamped)))
        for section, first in zip(self.sections, FIRSTS):
            for name in defined_names(section):
                if name in words:
                    self.assertIn(name, ALLOWED, "the stamped code takes %s from the marked part from %s: either it moves into "
                                  "the stamp, or ALLOWED says why it cannot change a translation" % (name, first))
        self.assertRegex(self.shader, r"\nVkShaderModule make_module\(const std::vector<uint32_t>& spirv,")
        # (the program cache's record, its writing and its reading are in the stamp)
        for head in ("void put_head(", "void get_head(", "void put_info(", "void get_info(", "void program_cache_save(",
                     "void program_cache_read(", "struct Reader {", "uint64_t disk_stamp()", "void translate_into(",
                     "const Program* program_pick(", "bool still_valid(", "uint64_t graphics_key(", "uint64_t answers_hash("):
            self.assertIn(head, self.stamped, head + " is not in the translation stamp")

    def test_vk_api_is_not_read_by_the_translator(self):
        stripped = NS["strip_comments"]((VK / "vk_api.h").read_bytes().replace(b"\r\n", b"\n"))[0].decode()
        api = blank_literals(stripped)
        # (what it includes, before anything of its own: so nothing it defines reaches those headers)
        directives = re.findall(r"^\s*#\s*(\w+)(.*)$", stripped, re.M)
        includes = [rest.strip() for d, rest in directives if d == "include"]
        self.assertEqual(includes, ["<stdint.h>", '"../render/rx_nvn.h"'])
        self.assertLess(stripped.index('#include "../render/rx_nvn.h"'), stripped.index("#define"))
        # (the names it gives anything: its enums' constants, its macros, its types and functions; its structs' members are
        # reached as host->, below)
        names = set()
        for body in re.findall(r"\benum\b[^{;]*\{([^}]*)\}", api):
            names |= {m.group(1) for m in (re.match(r"\s*(\w+)", item) for item in body.split(",")) if m}
        names |= set(re.findall(r"^\s*#\s*define\s+(\w+)", api, re.M))
        top = re.sub(r"\bstruct\s+(\w+)\s*\{[^{}]*\}", r"struct \1", re.sub(r"\benum\b[^{;]*\{[^}]*\}", "enum", api))
        names |= set(re.findall(r"\b\w+\b", top)) - {
            "pragma", "once", "include", "ifdef", "ifndef", "endif", "else", "define", "extern", "enum", "typedef", "struct",
            "const", "__cplusplus", "_WIN32", "__declspec", "dllexport", "__attribute__", "visibility", "host", "stdint", "h",
            "render", "rx_nvn"} - {n for n in re.findall(r"\b\w+\b", top) if n.isdigit()}
        self.assertLessEqual({"HOS_OPT_GYRO", "HOS_SW_A", "MKWVK_API_VERSION", "MkwVkHost", "mkwvk_init_fn", "mkwvk_init"}, names)
        eden, vk = translator_files(str(VK))
        calls = set()
        for path in eden + vk[:-1]:
            if not path.endswith(C_LIKE):
                continue
            code = blank_literals(stamped(path))
            used = names & set(re.findall(r"\b\w+\b", code))
            self.assertFalse(used, "%s reads vk_api.h's %s: vk_api.h then belongs in the translation stamp" % (path, sorted(used)))
            calls |= set(re.findall(r"\bhost->(\w+)", code))
        self.assertTrue(calls, "no host call found: the check reads nothing")
        self.assertLessEqual(calls, HOST_CALLS, "a translation asks the host something new: is its answer in a record's key or "
                             "in disk_stamp? Then HOST_CALLS")
        self.assertIn("host->depth_minus_w()", function(self.shader, "uint64_t disk_stamp() {"))

    def test_the_stamp_code(self):
        self.assertIn('#include "program_carry.h"', self.shader)
        self.assertRegex(self.shader, r"#ifdef HOOKSHOT_TRANSLATION_STAMP\nconstexpr uint64_t kTranslationStamp = "
                                      r"HOOKSHOT_TRANSLATION_STAMP;\n#else\nconstexpr uint64_t kTranslationStamp = 0;\n#endif")
        carry = function(self.shader, "bool carry_on() {")
        self.assertIn('kTranslationStamp != 0 && program_carry::on(getenv("RX_VK_PROGRAM_CARRY"))', carry)
        stamp = function(self.shader, "uint64_t disk_stamp() {")
        self.assertIn("program_carry::translator_words(carry_on(), kTranslationStamp, HOOKSHOT_SHADER_STAMP, translator);", stamp)
        self.assertIn("add(translator, sizeof translator);", stamp)
        self.assertNotIn("const uint64_t translator = HOOKSHOT_SHADER_STAMP;", stamp)
        self.assertIn("uint64_t program_cache_stamp() { return disk_stamp(); }", self.shader)
        init = function(self.shader, "void disk_init() {")
        self.assertIn("verdict = program_carry::verdict(read, head[0], head[1], kept, kDiskMagic, stamp, writer);", init)
        self.assertIn("if (verdict == program_carry::Verdict::Kept || verdict == program_carry::Verdict::Carried) {", init)
        self.assertIn("const uint32_t h[2] = {kDiskMagic, writer};", init)
        self.assertIn("dropped_note(p.key);", function(self.shader, "void program_cache_save("))
        dev = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
        self.assertIn("RX_VK_PROGRAM_CARRY=0", dev)


# ---- program_carry.h and disk_init, compiled ----

CARRY_TEST = r"""
#include <cstdio>
#include <initializer_list>
#include "program_carry.h"
using namespace mkwvk::program_carry;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    CHECK(on(nullptr) && on("1") && !on("0") && !on("00"));
    CHECK(writer(0) == 1 && writer(0x0000000100000001ull) == 1 && writer(0x12345678ull) == 0x12345678u);
    const uint32_t M = 0x31505348;
    CHECK(verdict(false, M, 5, 7, M, 7, 5) == Verdict::Fresh);
    CHECK(verdict(true, M + 1, 5, 7, M, 7, 5) == Verdict::Fresh);
    CHECK(verdict(true, M, 5, 7, M, 7, 5) == Verdict::Kept);
    CHECK(verdict(true, M, 6, 7, M, 7, 5) == Verdict::Carried);
    CHECK(verdict(true, M, 0, 7, M, 7, 5) == Verdict::Carried);   // (a file of before the rule, of this very stamp)
    // (a file of another stamp is never used, whoever made it)
    for (uint32_t w : {0u, 5u, 6u}) CHECK(verdict(true, M, w, 8, M, 7, 5) == Verdict::Dropped);
    uint64_t t[2];
    translator_words(true, 0xAA, 0xBB, t);
    CHECK(t[0] == 0xAA && t[1] == 2);
    translator_words(false, 0xAA, 0xBB, t);
    CHECK(t[0] == 0xBB && t[1] == 1);
    translator_words(true, 0, 0xBB, t);   // (a build without the translation stamp)
    CHECK(t[0] == 0xBB && t[1] == 1);
    uint64_t u[2];
    translator_words(true, 0xBB, 0xBB, u);   // (the two rules never name one cache, even for equal stamps)
    translator_words(false, 0xBB, 0xBB, t);
    CHECK(u[0] == t[0] && u[1] != t[1]);
    if (!fails) std::printf("PASS\n");
    return fails != 0;
}
"""

INIT_TEST = r"""
#define NOMINMAX
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#define MAX_PATH 260
typedef unsigned long DWORD;
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _fileno fileno
#define _chsize_s ftruncate
#endif
#include "program_carry.h"
namespace mkwvk {
std::string test_dir;
uint64_t test_stamp = 0, test_whole = 0;
bool test_carry = true;
void logf(const char* fmt, ...) {
    char b[2048];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    std::printf("LOG %s\n", b);
}
DWORD game_module_path(char* out, DWORD n) {
    std::snprintf(out, n, "%s/Triforce.exe", test_dir.c_str());
    return (DWORD)std::strlen(out);
}
uint64_t disk_stamp() { return test_stamp; }
bool carry_on() { return test_carry; }
#define HOOKSHOT_SHADER_STAMP ::mkwvk::test_whole
@@FILE@@
@@INIT@@
}
int main(int argc, char** argv) {
    using namespace mkwvk;
    test_dir = argv[1];
    test_stamp = std::strtoull(argv[2], nullptr, 16);
    test_whole = std::strtoull(argv[3], nullptr, 16);
    test_carry = argv[4][0] == '1';
    disk_init();
    std::printf("WRITER %08x\nON %d\n", program_carry::writer(test_whole), (int)disk_on);
    for (const auto& [key, recs] : disk_records) std::printf("KEY %llx %zu\n", (unsigned long long)key, recs.size());
    std::printf("DROPPED %zu %zu\n", disk_dropped.size(), disk_dropped_records);
    {
        std::lock_guard lk(disk_mu);
        for (int i = 5; i < argc; i++) dropped_note(std::strtoull(argv[i], nullptr, 16));
    }
    std::printf("AGAIN %lld %lld\n", (long long)st_again, (long long)st_first);
    for (FILE* f : {disk_out, disk_in, disk_idx})
        if (f) std::fclose(f);
    return 0;
}
"""
MAGIC, IDX_MAGIC = 0x31505348, 0x58495048


def writer(whole):
    w = (whole ^ (whole >> 32)) & 0xFFFFFFFF
    return w or 1


class CompiledTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cxx = COMPILER or shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
        if not cls.cxx:
            raise unittest.SkipTest("no C++20 compiler")
        cls.temp = tempfile.TemporaryDirectory(prefix="triforce-carry-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.dir = Path(cls.temp.name)

    def compile(self, name, source):
        src, exe = self.dir / (name + ".cpp"), self.dir / (name + ".exe")
        src.write_text(source, encoding="utf-8")
        r = subprocess.run([self.cxx, "-std=c++20", "-O1", "-Wall", "-I" + str(VK), str(src), "-o", str(exe)],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return exe

    def test_verdicts(self):
        r = subprocess.run([str(self.compile("carry", CARRY_TEST))], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_disk_init_on_authored_caches(self):
        shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
        file_part = marked_sections(shader)[FIRSTS.index("DiskRecord")]
        init = function(shader, "void disk_init() {") + "\n}\n"
        exe = self.compile("init", INIT_TEST.replace("@@FILE@@", file_part).replace("@@INIT@@", init))
        G, G2, W1, W2, W3 = 0x1111, 0x2222, 0xA1A1A1A1, 0xB2B2B2B2, 0xC3C3C3C3
        records = [(0xA, b"\x00\x00\x00\x00first"), (0xA, b"\x00\x00\x00\x00second"), (0xB, b"\x01\x00\x00\x00third")]

        def author(folder, stamp, whole, magic=MAGIC):
            folder.mkdir()
            body = b"".join(struct.pack("<QQ", key, len(data)) + data for key, data in records)
            (folder / "triforce_programs.bin").write_bytes(struct.pack("<IIQ", magic, writer(whole), stamp) + body)
            idx = b"".join(struct.pack("<QQI", key, len(data), struct.unpack_from("<I", data)[0]) for key, data in records)
            (folder / "triforce_programs.idx").write_bytes(struct.pack("<IIQ", IDX_MAGIC, 1, stamp) + idx)

        def run(folder, stamp, whole, carry=True, *notes):
            r = subprocess.run([str(exe), str(folder), "%x" % stamp, "%x" % whole, "1" if carry else "0"] + ["%x" % k for k in notes],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            out = r.stdout
            keys = dict(re.findall(r"^KEY (\w+) (\d+)$", out, re.M))
            head = struct.unpack_from("<IIQ", (folder / "triforce_programs.bin").read_bytes())
            return out, keys, head

        # (a cache of this build: kept, its head as it was)
        kept = self.dir / "kept"
        author(kept, G, W1)
        out, keys, head = run(kept, G, W1)
        self.assertEqual(keys, {"a": "2", "b": "1"})
        self.assertIn("2 program keys kept", out)
        self.assertNotIn("carried", out)
        self.assertEqual(head, (MAGIC, writer(W1), G))
        # (of another build, for the same stamp: carried, and its head names this build)
        out, keys, head = run(kept, G, W2)
        self.assertEqual(keys, {"a": "2", "b": "1"})
        self.assertIn("their 3 programs carried over an update of the translator (made by its build %08x, this one is %08x)" %
                      (writer(W1), writer(W2)), out)
        self.assertEqual(head, (MAGIC, writer(W2), G))
        out, keys, head = run(kept, G, W2)
        self.assertEqual(keys, {"a": "2", "b": "1"})
        self.assertNotIn("carried", out, "the next start of the build that carried it is a plain one")
        # (for another stamp: thrown away whatever made it, the old keys counted for the log, a new file and index)
        for whole in (W2, W3):
            folder = self.dir / ("dropped-%x" % whole)
            author(folder, G, W2)
            out, keys, head = run(folder, G2, whole, True, 0xA, 0xC, 0xA)
            self.assertEqual(keys, {})
            self.assertIn("DROPPED 2 3", out)
            self.assertIn("0 program keys kept, 0 KB: the one there (3 programs of 2 keys", out)
            self.assertIn("translated again as they are met", out)
            self.assertIn("AGAIN 2 1", out)
            self.assertIn("1 programs translated again since the translation changed", out)
            self.assertEqual(head, (MAGIC, writer(whole), G2))
            self.assertEqual((folder / "triforce_programs.bin").stat().st_size, 16)
            self.assertEqual((folder / "triforce_programs.idx").read_bytes(), struct.pack("<IIQ", IDX_MAGIC, 1, G2))
        # (RX_VK_PROGRAM_CARRY=0 says so when it throws one away)
        folder = self.dir / "off"
        author(folder, G, W1)
        out, _, _ = run(folder, G2, W1, False)
        self.assertIn("or another build of it: RX_VK_PROGRAM_CARRY=0", out)
        # (none there, or not one of ours: a new one, with its head, and nothing counted)
        for name, magic in (("none", None), ("other", MAGIC + 1)):
            folder = self.dir / name
            if magic is None:
                folder.mkdir()
            else:
                author(folder, G, W1, magic)
            out, keys, head = run(folder, G, W1, True, 0xA)
            self.assertEqual(keys, {})
            self.assertIn("0 program keys kept, 0 KB\n", out)
            self.assertIn("DROPPED 0 0", out)
            self.assertIn("AGAIN 0 0", out)
            self.assertEqual(head, (MAGIC, writer(W1), G))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx")
    args, rest = parser.parse_known_args()
    COMPILER = args.cxx
    unittest.main(argv=[sys.argv[0], *rest])
