"""Export save, Export save for a bug report and Import a save (host/save_share.c, host/save_tx_share.h, host/nn_fs.c), on real
folders in a temporary place. No game: tests/save_share_test.c is host/nn_fs.c's own text for the save (its long-path file
functions, the transaction's lock and thread, tx_mount, the import at the first mount, hos_fs_save_read and the import's
calls, cut out by this test) with host/save_share.c included whole; the menu and the rest of the host are stand-ins. The
program stands in a folder that plays the game folder, and each run of it is one start of the game.

Checked:
- the transaction's part alone: the newest save is the committed one or a finished commit still waiting, never the
  working copy; an import waits through the game's own saves, is replaced by the next one and gone when cancelled; the
  next start puts it in place with the save it replaces kept whole under a name the backups' pruning leaves alone; that
  start killed before each of its steps, then the start after: the import is the save and the replaced save is kept, never
  a mix; a first start with no save of its own; what waits is no save (dropped); the save cannot be kept (the import
  waits on, nothing changes); a finished commit held out of place by another program (that commit is what is kept);
- Export save: Triforce-save-<date>-<time>.zip in the export folder, read back by Python's zipfile: the manifest, the
  save's files as committed (what the game was writing since is not in it), nothing else; two in one second get two
  names; before the game mounted its save; while a Commit is still on its thread (the export waits for it and holds the
  save that Commit made, whole);
- Export save for a bug report: the same with triforce_log.txt, triforce_log.1.txt, triforce_options.ini and
  triforce_hang.txt, and no other file of the game folder; without a save it still holds those;
- Import a save: Triforce's export and NX2navi's are taken from the game folder's save-import folder, wait while the game
  saves on, and are the save at the next start with the old one kept; the zip moves to save-import/imported; a zip that
  is no save export, a damaged one and one with a path that leaves the save are refused whole and stay where they are;
  Cancel the import; an empty folder says where to put the zip;
- HOS_SAVE_TX=0 (the save written in place): nothing is exported or imported, a bug report holds the logs alone;
- the Downloads folder Windows names is a folder that exists (nothing is written there);
- in the sources: the rows' worker starts at the game's save mount, the read holds the transaction's lock and waits for
  the Commit's thread, the Commit's steps here are the ones nn_fs.c's own Commit makes.

    python -I -B tests/test_save_share.py [--cc gcc]
"""
import argparse
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"linux": "the program calls the Windows file functions (the game's Linux build has them from host/posix)"}

ROOT = Path(__file__).resolve().parents[1]
UID = "5a3c9e1f0b7d24680000000000000001"
TITLE = "0400486024dec000"


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def cut(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def check_sources(fs, share):
    mount = cut(fs, "HLE(_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE) {", "\n}\n")
    check("hos_save_share_start();" in mount and mount.index("hos_save_share_start();") < mount.index("tx_mount("),
          "host/nn_fs.c's save mount no longer starts the save rows' worker (before the mount, so also with HOS_SAVE_TX=0)")
    read = cut(fs, "int hos_fs_save_read(", "\n}\n")
    order = [read.index(s) for s in ("if (!save_tx_enabled()) return -2;", "AcquireSRWLockExclusive(&tx_lock);", "tx_wait_locked();",
                                     "if (tx_plain) files = -2;", "tx_share_newest(&p, newest, sizeof newest)", "tx_share_read(newest,",
                                     "ReleaseSRWLockExclusive(&tx_lock);")]
    check(order == sorted(order), "hos_fs_save_read no longer reads the newest save under the transaction's lock, after the Commit's thread ended")
    check("work" not in read, "hos_fs_save_read names the working copy")
    tx_mount = cut(fs, "static int tx_mount(", "\n}\n")
    check("ok = save_tx_mount(&m->p, note, sizeof note);\n        ok = tx_import_at_mount(&m->p, ok);" in tx_mount,
          "tx_mount no longer looks for a waiting import right after it put the disk right")
    at_mount = cut(fs, "static int tx_import_at_mount(", "\n}\n")
    check(at_mount.index("if (tx_import_looked) return ok;") < at_mount.index("tx_import_looked = 1;") < at_mount.index("tx_share_import_apply("),
          "a waiting import is no longer applied at the run's first mount alone")
    # (the fixture's Commit is nn_fs.c's own, by hand: the same steps in the same order)
    commit = cut(fs, "HLE(_ZN2nn2fs6CommitEPKc) {", "\n}\n")
    steps = ["AcquireSRWLockExclusive(&tx_lock);", "tx_wait_locked();", "save_tx_stage(&tx_mounts[i].p);", "tx_finish_paths = tx_mounts[i].p;",
             "tx_thread = CreateThread(NULL, 0, tx_finish_main, NULL, 0, NULL);", "ReleaseSRWLockExclusive(&tx_lock);"]
    at = [commit.index(s) for s in steps]
    check(at == sorted(at), "nn::fs::Commit in host/nn_fs.c changed: tests/save_share_test.c's game_commit no longer mirrors it")
    # (the worker: what it is asked, a tenth of a second apart, on a thread of its own)
    main = cut(share, "static DWORD WINAPI share_main(", "\n}\n")
    for ask, does in (("HOS_SHARE_EXPORT", "do_export(0)"), ("HOS_SHARE_EXPORT_BUG", "do_export(1)"), ("HOS_SHARE_IMPORT", "do_import()"),
                      ("HOS_SHARE_IMPORT_CANCEL", "do_import_cancel()")):
        check("case %s: %s; break;" % (ask, does) in main, "the worker no longer answers " + ask)
    check("WaitForSingleObject(share_wake, 100);" in main and "rx_option(HOS_OPT_SHARE_ASK, 0)" in main, "the worker no longer asks the menu ten times a second")
    check("CreateThread(NULL, 0, share_main, NULL, 0, NULL)" in cut(share, "void hos_save_share_start(void) {", "\n}\n"), "the work is no longer on a thread of its own")
    check("FOLDERID_Downloads" in share and '"SHGetKnownFolderPath"' in share and "0x374DE290, 0x123F, 0x4565, {0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B}" in share,
          "the Downloads folder is no longer Windows' known folder")


def body(tag):
    return (tag * 3000).encode()


def tag_of(folder):
    """the tag a save folder was written with, None when it is not one whole save of one tag"""
    files = [folder / "account.sav", folder / "option.sav", folder / "slot_00" / "progress.sav"]
    if not all(f.is_file() for f in files):
        return None
    data = [f.read_bytes() for f in files]
    for n in (2, 3):
        tag = data[0][:n].decode("ascii", "replace")
        if all(d == body(tag) for d in data):
            return tag
    return None


def export_like(tag, fmt="triforce-save", made="triforce", version="v0.4.0", extra=(), files=3, format_version=1):
    out = io.BytesIO()
    man = {"format": fmt, "format_version": format_version, "game": "The Legend of Zelda: Ocarina of Time", "title_id": TITLE, made: version,
           "exported": "2026-11-14T08:00:00Z", "exported_by": "game", "files": files, "bug_report": False}
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.json", json.dumps(man, indent=1) + "\n")
        for rel in ("account.sav", "option.sav", "slot_00/progress.sav"):
            z.writestr("save/" + rel, body(tag))
        for name, data in extra:
            info = zipfile.ZipInfo("x")
            info.filename = name
            z.writestr(info, data)
    return out.getvalue()


def damaged(zip_bytes, member):
    """the zip with one bit of that member's packed bytes changed"""
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as z:
        info = z.getinfo(member)
    name_len, extra_len = int.from_bytes(zip_bytes[info.header_offset + 26:info.header_offset + 28], "little"),         int.from_bytes(zip_bytes[info.header_offset + 28:info.header_offset + 30], "little")
    at = info.header_offset + 30 + name_len + extra_len + info.compress_size // 2
    return zip_bytes[:at] + bytes([zip_bytes[at] ^ 0x10]) + zip_bytes[at + 1:]


class Game:
    def __init__(self, cc, tmp):
        self.dir = tmp / "game folder"   # (a space in it, as a player's has)
        self.dir.mkdir()
        self.out = tmp / "exports"
        self.out.mkdir()
        self.user = self.dir / "user"
        self.save = self.user / "save" / UID
        self.tx = self.user / "save-tx" / UID
        self.backups = self.user / "save-backup" / UID
        self.inbox = self.dir / "save-import"
        self.exe = self.dir / "Triforce.exe"
        fs = (ROOT / "host" / "nn_fs.c").read_text(encoding="utf-8")
        paths = cut(fs, "#define HOS_FS_PATH 1024", "const char *hos_romfs_dir, *hos_user_dir;")
        save = cut(fs, '#include "save_tx.h"', "void hos_save_share_start(void);   /* host/save_share.c */")
        check("static int tx_mount(" in save and "int hos_fs_save_import_note(" in save and "fs_wide(" in paths, "the parts of host/nn_fs.c the test builds moved")
        inc = tmp / "inc"
        inc.mkdir()
        (inc / "nn_fs_cut.inc").write_text(paths + save, encoding="utf-8", newline="\n")
        build = subprocess.run([cc, "-std=gnu11", "-Wall", "-Wno-unused-function", "-O1", "-D_CRT_SECURE_NO_WARNINGS",
                                "-I", str(inc), "-I", str(ROOT / "host"), str(ROOT / "tests" / "save_share_test.c"),
                                str(ROOT / "host" / "save_pack.c"), str(ROOT / "host" / "save_zip.c"), "-o", str(tmp / "t.exe")],
                               capture_output=True, text=True)
        check(build.returncode == 0, "tests/save_share_test.c does not build:\n" + build.stderr)
        shutil.copyfile(tmp / "t.exe", self.exe)

    def run(self, *args, env=None, ok=True):
        e = os.environ.copy()
        for name in list(e):
            if name.startswith("HOS_"):
                del e[name]
        e["HOS_SAVE_EXPORT_DIR"] = str(self.out)
        e.update(env or {})
        r = subprocess.run([str(self.exe)] + list(args), env=e, capture_output=True, timeout=120)
        out = r.stdout.decode("utf-8", "replace")
        if (ok and r.returncode) or "FAIL" in out:
            raise SystemExit("FAIL: save_share_test %s: exit %d\n%s%s" % (" ".join(args), r.returncode, out, r.stderr.decode("utf-8", "replace")))
        return out

    def said(self, out, row):
        lines = [line[len("said %d: " % row):] for line in out.splitlines() if line.startswith("said %d: " % row)]
        check(len(lines) == 1, "the row %d was told %d lines:\n%s" % (row, len(lines), out))
        return lines[0]

    def new_zips(self, before):
        return sorted(set(self.out.glob("*.zip")) - before)


def read_export(path, tag, report=None):
    with zipfile.ZipFile(path) as z:
        check(z.testzip() is None, "zipfile finds a bad member in %s" % path.name)
        names = set(z.namelist())
        save = {"save/account.sav", "save/option.sav", "save/slot_00/progress.sav"} if tag else set()
        want = {"manifest.json"} | save | {"bug-report/" + n for n in (report or {})}
        check(names == want, "%s holds %r, not %r" % (path.name, sorted(names), sorted(want)))
        m = json.loads(z.read("manifest.json"))
        check(m["format"] == "triforce-save" and m["format_version"] == 1 and m["title_id"] == TITLE and m["triforce"] == "v0.4.0-test"
              and m["exported_by"] == "game" and m["files"] == len(save) and m["bug_report"] == (report is not None)
              and re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", m["exported"]), "the manifest of %s: %r" % (path.name, m))
        for name in save:
            check(z.read(name) == body(tag), "%s in %s is not the save the game committed (%r)" % (name, path.name, z.read(name)[:6]))
        for name, data in (report or {}).items():
            check(z.read("bug-report/" + name) == data, "bug-report/%s in %s is not the game folder's file" % (name, path.name))
    check(not list(path.parent.glob("*.part")), "a .part file is left beside the zip")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    fs = (ROOT / "host" / "nn_fs.c").read_text(encoding="utf-8")
    share = (ROOT / "host" / "save_share.c").read_text(encoding="utf-8")
    check_sources(fs, share)
    cc = shutil.which(a.cc) or shutil.which("gcc") or shutil.which("clang")
    if sys.platform != "win32" or not cc:
        print("skip: the program calls the Windows file functions and needs a C compiler; the sources were checked")
        return
    with tempfile.TemporaryDirectory(prefix="triforce-save-share-") as tmp:
        g = Game(cc, Path(tmp).resolve())
        check("PASS" in g.run("unit"), "the transaction's part")

        # the Downloads folder Windows names: a folder that exists (HOS_SAVE_EXPORT_DIR unset; nothing is written there)
        out = g.run("downloads", env={"HOS_SAVE_EXPORT_DIR": ""})
        found, _, where = out.strip().partition("downloads ")[2].partition(" ")
        check(found == "1" and Path(where).is_dir(), "Windows' Downloads folder was not found: %r" % out)

        # no save yet: nothing to export; a bug report still has the logs
        report = {"triforce_log.txt": b"[hos host] triforce v0.4.0-test\n[hos host] vk: device 0: a GPU\n" * 40, "triforce_log.1.txt": b"the run before\n",
                  "triforce_options.ini": b"fullscreen=1\nframe_rate_cap=0\n", "triforce_hang.txt": b"no frame for 20 seconds\n"}
        for name, data in report.items():
            (g.dir / name).write_bytes(data)
        for decoy in ("image.bin", "triforce_shaders.bin", "triforce_log.2.txt", "language.txt", "romfs/Pack/x.pack"):   # (never in a zip)
            (g.dir / decoy).parent.mkdir(parents=True, exist_ok=True)
            (g.dir / decoy).write_bytes(b"game data")
        before = set(g.out.glob("*.zip"))
        check("There is no save yet" in g.said(g.run("export", "0"), 1) and not g.new_zips(before), "an export without a save")
        line = g.said(g.run("export", "1"), 2)
        zips = g.new_zips(before)
        check(len(zips) == 1 and "without a save" in line and str(zips[0]) in line, "a bug report without a save: %r" % line)
        check(re.fullmatch(r"Triforce-save-\d{8}-\d{6}-bug-report\.zip", zips[0].name), "the bug report's name: " + zips[0].name)
        read_export(zips[0], None, report)

        # the game saves v1 and goes on writing: the export is v1
        g.run("play", "v1")
        check(tag_of(g.save) == "v1" and tag_of(g.tx / "work") == "zz", "the fixture's save")
        before = set(g.out.glob("*.zip"))
        line = g.said(g.run("export", "0"), 1)
        zips = g.new_zips(before)
        check(len(zips) == 1 and line == "Saved as " + str(zips[0]), "the row is told %r for %r" % (line, zips))
        check(re.fullmatch(r"Triforce-save-\d{8}-\d{6}\.zip", zips[0].name), "the export's name: " + zips[0].name)
        read_export(zips[0], "v1")
        line = g.said(g.run("export", "1"), 2)
        bug = [z for z in g.new_zips(before) if "bug-report" in z.name]
        check(len(bug) == 1 and line == "Saved as " + str(bug[0]), "the bug report row is told %r" % line)
        read_export(bug[0], "v1", report)
        # (two exports in the same second: two files)
        before = set(g.out.glob("*.zip"))
        for _ in range(3):
            g.run("export", "0")
        check(len(g.new_zips(before)) == 3, "three exports in a row left %d zips" % len(g.new_zips(before)))
        # (before the game mounted its save: the same save)
        before = set(g.out.glob("*.zip"))
        out = g.run("early")
        check("waits 0" in out and len(g.new_zips(before)) == 1, "an export before the save's mount:\n" + out)
        read_export(g.new_zips(before)[0], "v1")
        # (the export folder cannot be written: beside the game instead)
        blocked = g.out / "a file, not a folder"
        blocked.write_bytes(b"")
        line = g.said(g.run("export", "0", env={"HOS_SAVE_EXPORT_DIR": str(blocked)}), 1)
        beside = sorted(g.dir.glob("Triforce-save-*.zip"))
        check(len(beside) == 1 and line == "Saved as " + str(beside[0]), "an export folder that cannot be written: %r" % line)
        read_export(beside[0], "v1")
        beside[0].unlink()

        # an export while a Commit is still on its thread: it waits, and holds what that Commit made
        before = set(g.out.glob("*.zip"))
        out = g.run("busy", "b2")
        waited = int(re.search(r"busy export after (\d+) ms", out).group(1))
        check(waited >= 100, "the export did not wait for the Commit's thread (%d ms)" % waited)
        check(len(g.new_zips(before)) == 1, "the export during a Commit")
        read_export(g.new_zips(before)[0], "b2")
        g.run("start")
        check(tag_of(g.save) == "b2", "the save after the held Commit")

        # Import a save: an empty folder says where the zip goes
        out = g.run("import")
        check(str(g.inbox) in g.said(out, 3) and g.inbox.is_dir() and "waits 0" in out, "the Import row with nothing to import:\n" + out)
        # refused whole, and left where they are
        refused = {"notes.zip": b"this is no zip", "other.zip": export_like("xx", fmt="some-save"), "newer.zip": export_like("xx", format_version=9),
                   "escape.zip": export_like("xx", extra=[("save/../../escaped.sav", b"out")], files=4),
                   "damaged.zip": None}
        refused["damaged.zip"] = damaged(export_like("xx"), "save/option.sav")
        for name, data in refused.items():
            (g.inbox / name).write_bytes(data)
            out = g.run("import")
            line = g.said(out, 3)
            check(line.startswith(name + " was not imported.") and "waits 0" in out, "%s: %r" % (name, line))
            check((g.inbox / name).is_file() and not (g.tx / "import").exists() and not (g.tx / "import-new").exists(), name + " left something behind")
            check(not (g.dir / "escaped.sav").exists() and not (g.user / "escaped.sav").exists() and not (g.tx / "escaped.sav").exists(), "a path left the save")
            (g.inbox / name).unlink()
        check(tag_of(g.save) == "b2", "a refused import changed the save")

        # taken: it waits while the game saves on, and is the save at the next start, the old one kept
        (g.inbox / "Triforce-save-20261114-080000.zip").write_bytes(export_like("i1"))
        out = g.run("import", "v5")
        line = g.said(out, 3)
        check("becomes your save at the next start" in line and "Triforce v0.4.0" in line and "2026-11-14" in line, "the Import row is told %r" % line)
        check("import flag 1" in out and out.rstrip().endswith("waits 1"), "the import does not wait:\n" + out)
        check(tag_of(g.save) == "v5" and tag_of(g.tx / "import") == "i1", "the game's own save after the import was staged")
        check(not list(g.inbox.glob("*.zip")) and (g.inbox / "imported" / "Triforce-save-20261114-080000.zip").is_file(), "the imported zip was not moved away")
        out = g.run("start")
        check("note 1 The imported save was put in place at this start. The save it replaced is kept in " in out, "the start after an import:\n" + out)
        check(tag_of(g.save) == "i1" and tag_of(g.tx / "work") == "i1", "the imported save is not the save")
        kept = [d for d in g.backups.iterdir() if d.name.startswith("import-")]
        check(len(kept) == 1 and tag_of(kept[0]) == "v5" and str(kept[0]) in out, "the replaced save is not kept whole: %r" % kept)
        check("waits 0" in out and not (g.tx / "import").exists(), "the import still waits after it was applied")
        out = g.run("start")
        check("note 0" in out and tag_of(g.save) == "i1", "the start after that one")

        # NX2navi's export, then cancelled: the save stays
        (g.inbox / "NX2navi-save-2026-11-13_09-00-00.zip").write_bytes(export_like("n2", fmt="nx2navi-save", made="nx2navi", version="0.12.0"))
        out = g.run("import-cancel")   # (both in one run of the game: a start between them would put it in place)
        lines = [line for line in out.splitlines() if line.startswith("said 3: ")]
        check(len(lines) == 2 and "NX2navi 0.12.0" in lines[0] and "becomes your save at the next start" in lines[0], "NX2navi's export is not taken:\n" + out)
        check("staged n2" in out, "NX2navi's save did not wait whole:\n" + out)
        check("The import was cancelled" in lines[1] and "import flag 0" in out and out.rstrip().endswith("waits 0"), "Cancel the import:\n" + out)
        check(not (g.tx / "import").exists(), "a cancelled import still waits")
        g.run("start")
        check(tag_of(g.save) == "i1" and len([d for d in g.backups.iterdir() if d.name.startswith("import-")]) == 1, "a cancelled import changed the save")

        # HOS_SAVE_TX=0: the save is written in place; nothing is copied out of it or staged for it
        (g.inbox / "again.zip").write_bytes(export_like("i9"))
        before = set(g.out.glob("*.zip"))
        out = g.run("plain", env={"HOS_SAVE_TX": "0"})
        lines = [line for line in out.splitlines() if line.startswith("said ")]
        check(len(lines) == 3 and "Not in this run" in lines[0] and "without a save" in lines[1] and "was not imported" in lines[2], "with HOS_SAVE_TX=0:\n" + out)
        zips = g.new_zips(before)
        check(len(zips) == 1 and "bug-report" in zips[0].name, "with HOS_SAVE_TX=0 an export was written")
        read_export(zips[0], None, report)
        check((g.inbox / "again.zip").is_file() and not (g.tx / "import").exists() and tag_of(g.save) == "i1", "with HOS_SAVE_TX=0 an import was staged")
    print("PASS: save export, bug report export and import, on real folders, through host/nn_fs.c's and host/save_share.c's own code")


if __name__ == "__main__":
    main()
