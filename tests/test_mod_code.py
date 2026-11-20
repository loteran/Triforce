"""What the game says at its start of the mods' code patches (host/mod_code.h, host/mods.c; authored files, no game).

A code patch (.ips, .pchtxt) is applied when the game is built (nx2/code_mods.py, tests/test_code_mods.py), so a patch
file put into the mods folder later does nothing until the next build, and one taken out or switched off stays in the
game until then. The build's record in the game folder (triforce_code_mods.txt) is what the start holds against the mods
folder, and the log says which is which.

Compiled and run (tests/mod_code_test.c): the record's reader on lines of every kind, and, on a record that
nx2/code_mods.py itself wrote for an authored game and mods folder, what the start finds once the folder has changed: a
patch built in, one edited, one the build refused, one edited after it was refused, a new one, one deleted and one whose
mod was switched off. The number a file's contents have is the same on both sides.

Read in host/mods.c: the record is the game folder's, read before the mods are walked; every patch file of a mod is
held against it by the path the build wrote; what is gone is said with or without a mods folder; nothing says any more
that exefs patches are left out.

    python -I -B tests/test_mod_code.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "nx2"), str(ROOT / "tests")]
import code_mods  # noqa: E402
import test_code_mods as authored  # noqa: E402  (its modules and patches)

BUILT, CHANGED, REFUSED, WAITS = range(4)   # (host/mod_code.h MOD_CODE_*)


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def run(command):
    flags = (subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == "nt" else 0
    result = subprocess.run(command, creationflags=flags, capture_output=True, text=True, encoding="utf-8", errors="replace")
    check(result.returncode == 0, "%s:\n%s%s" % (command[0], result.stdout, result.stderr))
    return result.stdout


def check_sources():
    mods = (ROOT / "host" / "mods.c").read_text(encoding="utf-8")
    header = (ROOT / "host" / "mod_code.h").read_text(encoding="utf-8")
    check('#define MOD_CODE_RECORD "%s"' % code_mods.RECORD in header, "the build and the game do not name the record the same")
    check("enum {\n    MOD_CODE_BUILT," in header and header.index("MOD_CODE_BUILT,") < header.index("MOD_CODE_CHANGED,") < header.index("MOD_CODE_REFUSED,")
          < header.index("MOD_CODE_WAITS"), "host/mod_code.h's states are not in this test's order")
    load = block(mods, "static void code_record_load(")
    check("hos_game_path_a(path, MAX_PATH);" in load and "MOD_CODE_RECORD" in load and "mod_code_parse(text, built, BUILT_MOST)" in load,
          "the record is not read from the game folder")
    patches = block(mods, "static int code_patches(")
    check('snprintf(rel, sizeof rel, "%s/exefs/%s", mod->name, fd.cFileName);' in patches and "mod_code_is_patch(fd.cFileName)" in patches
          and "mod_code_state(built, nbuilt, rel, mod_code_fnv(data, len), len, &why)" in patches,
          "a mod's patch files are not held against the record by the path the build wrote")
    for said in ("is built into the game", "was not applied when the game was built: %s", "changed since the game was built", "is not built into the game"):
        check(said in patches, "host/mods.c does not say of a patch file that it " + said)
    gone = block(mods, "static void code_patches_gone(")
    check("mod_code_gone(built, nbuilt, &at)" in gone and "gone from the mods folder or switched off" in gone and "code patches wait for a build" in gone,
          "the patches the game has and the mods folder no longer does are not said")
    init = block(mods, "void hos_mods_init(")
    check(init.index("code_record_load();") < init.index("mods_scan();") < init.index("code_patches_gone();"),
          "the record is not read before the mods are walked, or what is gone not said after")
    scan = block(mods, "static void mods_scan(")
    check("code_patches(&mods[i]);" in scan and "code_patches_gone" not in scan, "what is gone is only said when there is a mods folder")
    # ((Triforce, 11-18) a mod whose code waits for a build keeps its RomFS files waiting too: its new files over the old
    # code crashed the game at its start; HOS_MODS_SPLIT=1 loads them anyway)
    check("const int waits = code_patches(&mods[i]);" in scan and "const int hold = waits > 0 && !split;" in scan
          and 'const int files = hold ? 0 : index_romfs(dir, "", 0);' in scan and "HOS_MODS_SPLIT" in scan,
          "a mod whose code waits for a build still has its RomFS files read")
    check(scan.index("code_patches(&mods[i]);") < scan.index("index_romfs(dir"), "the RomFS files are read before the mod's code is known")
    check("waits += state == MOD_CODE_CHANGED || state == MOD_CODE_WAITS;" in patches and "return waits;" in patches,
          "code_patches does not say how many of a mod's patches wait")
    check("exefs patches are left out" not in mods, "host/mods.c still says exefs patches are left out")
    # (the walk both sides make: the path under the mods folder is the mod's name, exefs, the file)
    check('out.append(("%s/exefs/%s" % (name, entry), path))' in (ROOT / "nx2" / "code_mods.py").read_text(encoding="utf-8"),
          "nx2/code_mods.py no longer names a patch file <mod>/exefs/<file>")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="cc")
    a = p.parse_args()
    check_sources()
    os.environ.pop("HOS_MODS", None)   # (the game's own switch for another mods folder: not this test's)
    with tempfile.TemporaryDirectory(prefix="triforce-mod-code-") as tmp:
        tmp = Path(tmp)
        exe = tmp / "mod-code.exe"
        run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "host"), str(ROOT / "tests" / "mod_code_test.c"), "-o", str(exe)])
        check("PASS" in run([str(exe)]), "the record's reader")

        # a game folder as a build leaves it, by the build's own code
        out, exefs, mods = tmp / "game", tmp / "work" / "exefs", tmp / "game" / "mods"
        exefs.mkdir(parents=True)
        (exefs / "main").write_bytes(authored.nso(authored.MAIN_ID))
        main_id = authored.MAIN_HEX

        def patch(rel, data):
            path = mods / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))
            return path

        pchtxt = lambda offset, value: "@nsobid-%s\n@enabled\n%08X %s\n" % (main_id, offset + 0x100, value)
        patch("built/exefs/%s.ips" % main_id, authored.ips([(0x100 + 0x40, b"\x1f\x20\x03\xd5")]))
        edited = patch("edited/exefs/e.pchtxt", pchtxt(0x50, "02"))
        patch("old/exefs/0123.ips", authored.ips([(0x140, b"\x00")]))
        retried = patch("retried/exefs/r.pchtxt", pchtxt(0x900000, "04"))
        gone = patch("gone/exefs/g.pchtxt", pchtxt(0x60, "05"))
        patch("off/exefs/o.pchtxt", pchtxt(0x70, "06"))
        said = []
        entries = code_mods.build_step(str(exefs), str(out), said.append)
        check([e["state"] for e in entries] == ["applied", "applied", "applied", "applied", "refused", "refused"], "the authored build: %r" % said)
        code_mods.write_record(str(out), entries)

        # the folder changes after the build
        edited.write_text(pchtxt(0x50, "22"), encoding="utf-8")
        retried.write_text(pchtxt(0x80, "04"), encoding="utf-8")
        gone.unlink()
        (mods / "off").rename(mods / "-off")
        patch("new/exefs/n.pchtxt", pchtxt(0x90, "07"))
        now = code_mods.patch_files(str(mods))   # (the walk host/mods.c makes)
        command = [str(exe), str(out / code_mods.RECORD)]
        for rel, path in now:
            command += [rel, path]
        lines = run(command).splitlines()
        states = {line.split(" ", 2)[1]: (int(line.split(" ", 1)[0]), line.split(" | ", 1)[1]) for line in lines if not line.startswith("gone ")}
        check(states["built/exefs/%s.ips" % main_id] == (BUILT, ""), "a patch that is built in: %r" % lines)
        check(states["edited/exefs/e.pchtxt"] == (CHANGED, ""), "a patch edited since the build: %r" % lines)
        check(states["old/exefs/0123.ips"][0] == REFUSED and states["old/exefs/0123.ips"][1].startswith("it is for build 0123, which is not this game's code"),
              "a patch the build refused: %r" % lines)
        check(states["retried/exefs/r.pchtxt"] == (WAITS, ""), "a patch edited after it was refused: %r" % lines)
        check(states["new/exefs/n.pchtxt"] == (WAITS, ""), "a patch added after the build: %r" % lines)
        check(len(states) == 5, "the patch files of the folder: %r" % lines)
        check(sorted(line[5:] for line in lines if line.startswith("gone ")) == ["gone/exefs/g.pchtxt", "off/exefs/o.pchtxt"],
              "the patches that are built in and gone or switched off: %r" % lines)
        # (and a folder with no mods at all: everything that was built in is gone)
        lines = run([str(exe), str(out / code_mods.RECORD)]).splitlines()
        check(len(lines) == 4 and all(line.startswith("gone ") for line in lines), "with no mods folder: %r" % lines)
        walked = mods_c_itself(a.cc, tmp, out, mods, main_id)
    print("PASS: the mods' code patches at the game's start: built in, changed, refused, waiting for a build, gone" + walked)


MODS_FIXTURE = r'''
#include "mods.c"
#include <stdarg.h>
const char hos_main_build_id[] = "@BUILD@";
void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
void hos_mod_cheats_add(const char *mod, const char *path) {
    const char *name = strrchr(path, '\\');
    printf("cheats: %s <- %s\n", mod, name ? name + 1 : path);
}
void hos_mod_cheats_ready(void) { printf("cheats: ready\n"); }
int hos_prompts_romfs(const char *key, char *out, size_t cap) { (void)key, (void)out, (void)cap; return 0; }
int hos_prompts_font_file(const char *key, char *out, size_t cap) { (void)key, (void)out, (void)cap; return 0; }
int main(void) {
    hos_mods_init(NULL);
    char found[MAX_PATH];
    printf("romfs: %d %d\n", hos_mods_romfs("UI/x.zs", found, sizeof found), hos_mods_romfs("UI/none.zs", found, sizeof found));
    return 0;
}
'''


def mods_c_itself(cc, tmp, out, mods, main_id):
    """host/mods.c whole, on Windows (its file calls are Windows'): its own walk over the authored mods folder, the record
    beside the program as beside the game, and what it logs and hands to the cheats"""
    if os.name != "nt":
        return " (host/mods.c's own walk is run on Windows only)"
    build = tmp / "mods-c"
    build.mkdir()
    (build / "mods.c").write_text((ROOT / "host" / "mods.c").read_text(encoding="utf-8"), encoding="utf-8")
    # (what mods.c takes from hos.h, in its place: the copy beside it is found first)
    (build / "hos.h").write_text("#include <stddef.h>\n#include <stdint.h>\nvoid hos_log(const char *fmt, ...);\n"
                                 "extern const char hos_main_build_id[];\n"
                                 # ((Triforce) hos_mods_table_size reads the guest's path through GP: the walk never calls it)
                                 "#define GP(a) ((void *)(uintptr_t)(a))\n", encoding="utf-8")
    (build / "t.c").write_text(MODS_FIXTURE.replace("@BUILD@", main_id), encoding="utf-8")
    game = tmp / "walked game"
    game.mkdir()
    (game / code_mods.RECORD).write_bytes((out / code_mods.RECORD).read_bytes())
    exe = game / "walk.exe"
    run([cc, "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-I" + str(build), "-I" + str(ROOT / "host"), str(build / "t.c"), "-o", str(exe)])
    cheat = "[A cheat]\n04100000 00000010 0000000A\n"
    for rel in ("zz-cheats/cheats/%s.txt" % main_id[:16].upper(), "new/cheats/%s.TXT" % main_id[:16], "new/cheats/0123456789ABCDEF.txt",
                "new/cheats/readme.txt", "new/cheats/%s.txt.bak" % main_id[:16], "-nocheat/cheats/%s.txt" % main_id[:16], "art/romfs/UI/x.zs",
                # (and folders for the walk itself: the order, the deeper layouts, what is off or passed over)
                "A-upper/romfs/a.bin", "a/b/c/d/romfs/a.bin", "contents/0400486024DEC000/deep/romfs/a.bin", "p/q/r/s/t/romfs/a.bin",
                ".hidden/romfs/a.bin", "_under/romfs/a.bin", "Zed/romfs/a.bin"):
        path = mods / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(cheat, encoding="utf-8")
    env = dict(os.environ, HOS_MODS=str(mods))
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
    result = subprocess.run([str(exe)], env=env, creationflags=flags, capture_output=True, text=True, encoding="utf-8", errors="replace")
    said = result.stdout
    check(result.returncode == 0, "host/mods.c's walk:\n" + said + result.stderr)
    log = [line[5:] for line in said.splitlines() if line.startswith("log: ")]
    for want in ("mods: built/exefs/%s.ips is built into the game" % main_id,
                 "mods: edited/exefs/e.pchtxt changed since the game was built: the game has it as it was then",
                 "mods: retried/exefs/r.pchtxt is not built into the game",
                 "mods: new/exefs/n.pchtxt is not built into the game",
                 "mods: gone/exefs/g.pchtxt is built into the game, and is gone from the mods folder or switched off: the game has it until it is built again",
                 "mods: off/exefs/o.pchtxt is built into the game, and is gone from the mods folder or switched off: the game has it until it is built again",
                 "mods: new/cheats/0123456789ABCDEF.txt is for another build of the game (this one is %s): not read" % main_id[:16]):
        check(want in log, "host/mods.c does not say: %s\n%s" % (want, said))
    check(any(line.startswith("mods: old/exefs/0123.ips was not applied when the game was built: it is for build 0123") for line in log), said)
    check(any(line.startswith("mods: 5 code patches wait for a build") for line in log), "how many patches wait for a build:\n" + said)
    check(sum("is off (its name starts with -)" in line for line in log) == 2 and sum("is off (its name starts with _)" in line for line in log) == 1,
          "the mods that are off:\n" + said)
    # (the build's walk of the same folder, nx2/code_mods.py find_mods, finds the same mods in the same order)
    held = " its RomFS files wait for the build too, with its code"
    walk_c = [line[len("mods: "):line.rindex(": ")] if line.endswith(" RomFS files") else line[len("mods: "):line.index(":" + held)]
              for line in log if line.endswith(" RomFS files") or held in line]
    # ((Triforce, 11-18) the mods whose code waits for a build (edited, new, retried) keep their RomFS files out until it)
    check(sorted(line[len("mods: "):line.index(":" + held)] for line in log if held in line) == ["edited", "new", "retried"],
          "the mods whose RomFS files wait with their code:\n" + said)
    walk_py = [name for name, _ in code_mods.find_mods(str(mods))]
    check(walk_c == walk_py and "a/b/c/d" in walk_py and "contents/0400486024DEC000/deep" in walk_py and len(walk_py) == 12
          and walk_py.index("A-upper") < walk_py.index("a/b/c/d") < walk_py.index("art") < walk_py.index("Zed") < walk_py.index("zz-cheats"),
          "host/mods.c and nx2/code_mods.py do not walk the mods folder alike:\n%r\n%r" % (walk_c, walk_py))
    check(not any("readme.txt" in line or ".bak" in line for line in log), "a file that is no cheat file is spoken of:\n" + said)
    handed = [line for line in said.splitlines() if line.startswith("cheats: ")]
    check(handed == ["cheats: new <- %s.TXT" % main_id[:16], "cheats: zz-cheats <- %s.txt" % main_id[:16].upper(), "cheats: ready"],
          "the cheat files handed over, in the mods' order, then ready: %r" % handed)
    check("romfs: 1 0" in said, "the RomFS mod beside them is no longer indexed:\n" + said)
    # (and with no mods folder at all the built-in patches are still said, and the cheats made ready)
    result = subprocess.run([str(exe)], env=dict(os.environ, HOS_MODS=str(tmp / "no such folder")), creationflags=flags, capture_output=True,
                            text=True, encoding="utf-8", errors="replace")
    check(result.stdout.count("is gone from the mods folder or switched off") == 4 and "mods: 4 code patches wait for a build" in result.stdout
          and "cheats: ready" in result.stdout, "host/mods.c without a mods folder:\n" + result.stdout)
    return "; host/mods.c's own walk says the same and hands the cheat files over"


if __name__ == "__main__":
    main()
