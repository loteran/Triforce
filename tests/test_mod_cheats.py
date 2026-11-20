"""The cheats of the mods' cheat files (host/mod_cheats.h, host/mod_cheats.c, host/mods.c; authored cheats, no game).

A mod's cheats/<build id>.txt is read at the start, each cheat has a row on the System tab, and the ones that are on are
run once a game frame by an interpreter over the game's memory. Compiled and run here:

- tests/mod_cheats_test.c: host/mod_cheats.h alone against a stand-in memory. Every opcode the interpreter runs, form by
  form (stores, ifs with their else and nesting, loops, the registers, the arithmetic, buttons, the register ifs, saved and
  static registers), the opcodes it does not run (each refused by name when the file is read, never passed over), opcodes
  cut short, the bounds of every kind of access (outside the heap and the image, across their ends, addresses that wrap,
  the modules' code), a loop without end, and the file reader (names, master codes, headings, words that are none, a
  name without its bracket, more than there is room for);
- tests/mod_cheats_host_test.c: host/mod_cheats.c's own text against a stand-in game, menu and game folder: what the
  host names to the menu, what a switch does and that it is kept for the next start (a second run on the same folder),
  three cheats of one name, the master code's register, a button's cheat, what is said once in the log, and that rows
  that cannot be switched are not.

Read in the sources: the regions are where host/hos_svc.c tells the game they are, the frame's call is at the game's
present, the buttons are the ones the game is given, host/mods.c hands over the file named after this build and says so
of another build's, and nx2/standalone.py writes main's base and the modules' code for the host (run here on an authored
image's description, its C compiled).

    python -I -B tests/test_mod_cheats.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
BUILD = "E0F3CCFAF50065AE"   # (the first 8 bytes of a main's build id: a cheat file's name)

A_FILE = """{Master}
40010000 00000010 00000600
[Rupees]
64010000 00000000 000003E7
[--- Section ---]
20000000
[Hold ZL]
80000100 01100000 00000700 00000001 20000000
[Code patch]
04000000 00000100 D503201F
[Pause]
FF000000
[Wild]
04100000 00100000 00000001
[Null chain]
58120000 00000F00 54021000 00000000 01100000 00000710 00000001
[Rupees]
01100000 00000720 00000002
"""
B_FILE = "[Rupees]\r\n01100000 00000730 00000003\r\n[A cheat with a name longer than a row shows whole]\r\n01100000 00000740 00000004\r\n"


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


def run(command, says=None, **more):
    """A compiler or a fixture run; a fixture must say `says`"""
    flags = (subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == "nt" else 0
    result = subprocess.run(command, creationflags=flags, capture_output=True, text=True, encoding="utf-8", errors="replace", **more)
    check(result.returncode == 0 and (says is None or says in result.stdout),
          "%s:\n%s%s" % (" ".join(str(c) for c in command[:1] + command[-2:]), result.stdout, result.stderr))
    return result.stdout


def check_sources():
    read = lambda *parts: ROOT.joinpath(*parts).read_text(encoding="utf-8")
    cheats, header, mods, svc = read("host", "mod_cheats.c"), read("host", "mod_cheats.h"), read("host", "mods.c"), read("host", "hos_svc.c")
    native, hid, api = read("host", "nvn_native.c"), read("host", "nn_hid.c"), read("gpu", "vk", "vk_api.h")
    # the regions: where the game itself is told they are
    check("{hos_main_base, HOS_HEAP_BASE, HOS_HEAP_BASE, 0x08000000ULL}" in cheats, "the regions' bases are not main, the heap, the alias region, the address space")
    for line in ("case 2: return HOS_HEAP_BASE;", "case 4: return HOS_HEAP_BASE;", "case 12: return 0x08000000ULL;"):
        check(line in svc, "host/hos_svc.c no longer tells the game its regions as the cheats take them: " + line)
    check("enum { MOD_CHEAT_MAIN, MOD_CHEAT_HEAP, MOD_CHEAT_ALIAS, MOD_CHEAT_ASLR, MOD_CHEAT_REGIONS };" in header, "the regions' order is not the format's")
    # the memory: every access through the checks, writes told to the renderer's write watch
    check("if (!mod_cheat_readable((const ModCheatLayout *)ctx, a, n)) return 0;" in block(cheats, "static int mem_read("), "a read is not checked against the game's memory")
    write = block(cheats, "static int mem_write(")
    check("if (!mod_cheat_writable((const ModCheatLayout *)ctx, a, n)) return 0;" in write and "HOS_WRITTEN(GP(a), n);" in write,
          "a write is not checked against the game's memory and its code, or not marked for the renderer")
    check(cheats.count("GP(") == block(cheats, "static int mem_read(").count("GP(") + write.count("GP(") == 4,"host/mod_cheats.c reaches the game's memory outside mem_read and mem_write")
    layout = block(cheats, "static ModCheatLayout game_layout(")
    check("{hos_image_base, hos_image_end, HOS_HEAP_BASE, hos_heap_size, hos_code, hos_code_count}" in layout, "the game's memory is not its image and its heap")
    # once a game frame, at the game's present; the buttons the game is given
    present = block(native, "NV(nvnQueuePresentTexture) {")
    check(present.index("hos_vi_native_presented();") < present.index("hos_mod_cheats_frame();"), "the cheats are not run at the game's present")
    check(native.count("hos_mod_cheats_frame();") == 1, "the cheats are run from more than one place")
    states = block(hid, "static void npad_states(")
    check("if (connected) __atomic_store_n(&hos_npad_buttons, buttons, __ATOMIC_RELAXED);" in states
          and states.index("hos_npad_buttons") > states.index("hos_frame_step_buttons(buttons);") and states.index("hos_npad_buttons") < states.index("G64(e + 8) = buttons;"),
          "the cheats' buttons are not the ones the game is given")
    # the file of this build, and another build's said
    files = block(mods, "static void cheat_files(")
    check('snprintf(id, sizeof id, "%.16s", hos_main_build_id);' in files and "!_strnicmp(fd.cFileName, id, 16)) hos_mod_cheats_add(mod->name, path);" in files,
          "host/mods.c does not hand over the cheat file named after this build")
    check("is for another build of the game (this one is %s): not read" in files, "a cheat file for another build is not said")
    init = block(mods, "void hos_mods_init(")
    check(init.index("mods_scan();") < init.index("hos_mod_cheats_ready();"), "the cheats are not made ready after the mods are read")
    check("has_romfs || has_exefs || has_cheats" in mods, "a folder with cheats alone is not a mod")
    # the rows' ids are this feature's own
    enum = block(api, "enum { HOS_OPT_MODCHEAT_TEXT")
    names = re.findall(r"\bHOS_OPT_\w+", enum)
    check("HOS_OPT_MODCHEAT_TEXT = 130" in enum and 130 + len(names) - 1 <= 134, "the option ids are not within 130 to 134")
    check("MAX_ROWS = 96" in cheats and "constexpr int kSlots = 96;" in read("gpu", "vk", "mod_cheat_rows.h"), "the host's rows and the menu's slots differ")


def check_image_c(cc, tmp):
    """nx2/standalone.py write_image_c on an authored image's description: main's base and each module's code for the host"""
    sys.path.insert(0, str(ROOT / "nx2"))
    import standalone

    class Image:
        def in_code(self, pc):
            return False

        def word(self, pc):
            return 0

    module = lambda name, base, text: {"name": name, "base": base, "size": 0x2000, "init": base + 0x10, "build_id": "ab",
                                        "segments": [{"name": ".text", "addr": base, "size": text},
                                                     {"name": ".rodata", "addr": base + 0x1000, "size": 0x100},
                                                     {"name": ".data", "addr": base + 0x1800, "size": 0x100}]}
    meta = {"base": 0x80000000, "end": 0x80006000, "exports": {},
            "modules": [module("rtld", 0x80000000, 0x1000), module("main", 0x80002000, 0x800), module("sdk", 0x80004000, 0xC00)]}
    exefs, gen = tmp / "exefs", tmp / "gen"
    exefs.mkdir(), gen.mkdir()
    (exefs / "main.npdm").write_bytes(b"META" + bytes(0x100))
    standalone.write_image_c(str(gen), Image(), meta, str(exefs))
    text = (gen / "hos_image.c").read_text(encoding="utf-8")
    check("const uint64_t hos_main_base = 0x80002000ULL;" in text, "hos_image.c does not have main's base:\n" + text)
    check("const int hos_code_count = 3;" in text and
          "const uint64_t hos_code[][2] = {{0x80000000ULL, 0x80001000ULL}, {0x80002000ULL, 0x80002800ULL}, {0x80004000ULL, 0x80004c00ULL}};" in text,
          "hos_image.c does not have each module's code:\n" + text)
    run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", str(gen / "hos_image.c")])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="cc")
    a = p.parse_args()
    check_sources()
    with tempfile.TemporaryDirectory(prefix="triforce-mod-cheats-") as tmp:
        tmp = Path(tmp)
        warnings = ["-std=c11", "-Wall", "-Wextra", "-Werror"]
        # the reader and the interpreter
        exe = tmp / "interpreter.exe"
        run([a.cc] + warnings + ["-I" + str(ROOT / "host"), str(ROOT / "tests" / "mod_cheats_test.c"), "-o", str(exe)])
        run([str(exe)], says="PASS")
        # the host's side: host/mod_cheats.c from after its includes on, with the menu's ids from vk_api.h
        source = (ROOT / "host" / "mod_cheats.c").read_text(encoding="utf-8")
        api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
        marker = '#include "mod_cheats.h"\n'
        check(source.count(marker) == 1, "host/mod_cheats.c no longer includes mod_cheats.h last")
        (tmp / "mod_cheats_actual.inc").write_text(source[source.index(marker) + len(marker):], encoding="utf-8")
        (tmp / "mod_cheats_constants.inc").write_text(block(api, "enum { HOS_OPT_MODCHEAT_TEXT") + ";\n" + block(api, "enum { HOS_MODCHEAT_ON") + ";\n",
                                                      encoding="utf-8")
        game = tmp / "game folder"
        cheats_a, cheats_b = game / "mods" / "A" / "cheats" / (BUILD + ".txt"), game / "mods" / "B" / "cheats" / (BUILD.lower() + ".TXT")
        for path, text in ((cheats_a, A_FILE), (cheats_b, B_FILE)):
            path.parent.mkdir(parents=True)
            path.write_bytes(text.encode("utf-8"))
        exe = tmp / "host.exe"
        run([a.cc] + warnings + ["-I" + str(ROOT / "host"), "-I" + str(tmp), str(ROOT / "tests" / "mod_cheats_host_test.c"), "-o", str(exe)])
        for start in ("first", "second"):
            run([str(exe), str(game), str(cheats_a), str(cheats_b), start], says="PASS", cwd=str(tmp))
        kept = (game / "triforce_cheats.txt").read_text(encoding="utf-8").splitlines()
        check(kept[0].startswith("#") and sorted(line.split(" ", 1)[1] for line in kept[1:]) == ["A/Hold ZL", "A/Null chain", "A/Wild", "B/Rupees"],
              "what the game folder keeps of the switches: %r" % kept)
        check(all(re.fullmatch(r"[0-9a-f]{8} .+", line) for line in kept[1:]), "a kept switch is not its number and its name: %r" % kept)
        check_image_c(a.cc, tmp)
    print("PASS: the mods' cheats: the reader, every opcode and its bounds, the host's rows and switches, what is kept for the next start")


if __name__ == "__main__":
    main()
