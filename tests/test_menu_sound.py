"""The options' own sounds (host/menu_sound.c): the game's interface sounds, after NX2navi's menu sounds, read from the
player's game files, decoded and mixed over the game's output.

tests/menu_sound_test.c compiles host/menu_sound.c as it is, with stand-ins for the host around it, and the end of
host/nn_audio.c's render_frame cut out here as it is (from the options' sounds to the 16 bits). No sound device and no
game file: every BWAV it plays is written by the test from samples it makes up. Each scenario runs in a process of its
own (menu_sound.c's state is the process's):

- decode: PCM16 and DSP-ADPCM, one channel by each pan and two, other rates brought to 48 kHz; files cut short, with
  wrong offsets, counts, codecs or channels refused, read from memory that ends where nothing can be read
- voices: a free voice, else the oldest; a restart inside the mix, at either point; nothing mixed past a sound's end;
  the cursor's tick spaced and under the others
- load: the seven files read once, by their RomFS paths; those not there or not a sound left out, silent
- frames: the sounds the options asked for, in the next block, in order, at the row's level; nothing read or played
  with the row at 0 or without a renderer
- master: the sounds in the front left and right, under the Master volume, the block held at the 16 bits' ends

The source is read too: render_frame adds the sounds after the game's limiter and before the Master volume; the file
reader goes through the mods' table; the two option ids are 135 and 136 (tests/test_menu_sound_events.py has the
renderer's answers to them).

    python -I -B tests/test_menu_sound.py [--cc cc]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = ("decode", "voices", "load", "frames", "master")


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="cc")
    a = p.parse_args()
    audio = (ROOT / "host" / "nn_audio.c").read_text(encoding="utf-8")
    fs = (ROOT / "host" / "nn_fs.c").read_text(encoding="utf-8")
    sound = (ROOT / "host" / "menu_sound.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")

    frame = audio[audio.index("static void render_frame("):]
    frame = frame[:frame.index("\n}\n") + 3]
    start = frame.index("/* (Triforce) the options' own sounds")
    check(frame.index("if (lim_enabled)") < start < frame.index("the Insert menu's Master volume"),
          "render_frame does not add the options' sounds after the game's limiter and before the Master volume")
    check("hos_menu_sound_frame(out[0], out[1], FRAME);" in frame[start:], "render_frame does not add the sounds to the front pair")
    tail = frame[start:frame.rindex("\n}\n")] + "\n"
    check(re.search(r"^#define FRAME 240\b", audio, re.M) and re.search(r"^#define RATE 48000\b", audio, re.M),
          "nn_audio.c's block is no longer 240 frames at 48 kHz (menu_sound.c's MIX_RATE and the test's FRAME)")
    check("#define MIX_RATE 48000" in sound, "menu_sound.c no longer mixes at the output's 48 kHz")

    reader = fs[fs.index("uint8_t *hos_fs_rom_file("):]
    reader = reader[:reader.index("\n}\n")]
    check("hos_mods_romfs(rel, path, sizeof path)" in reader and "guest_path_ok(rel)" in reader,
          "hos_fs_rom_file does not look in the mods' table first, or takes any path")
    check("hos_fs_rom_file(files[id], FILE_MOST, &size)" in sound and "CreateThread(NULL, 0, load_main" in sound,
          "menu_sound.c does not read the files through nn_fs.c on a thread of its own")
    check(re.search(r"enum \{ HOS_OPT_MENU_SOUND = 135, HOS_OPT_MENU_SOUND_VOLUME \};", api),
          "vk_api.h's options for the sounds are not 135 and 136")
    for i in range(135, 140):
        check(len(re.findall(r"HOS_OPT_\w+ = %d\b" % i, api)) <= 1, "option %d is taken twice" % i)

    flags = (subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == "nt" else 0
    with tempfile.TemporaryDirectory(prefix="menu-sound-") as tmp:
        work = Path(tmp)
        (work / "render_tail.inc").write_text(tail, encoding="utf-8")
        exe = work / "menu_sound_test.exe"
        subprocess.run([a.cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(work),
                        str(ROOT / "tests" / "menu_sound_test.c"), "-lm", "-o", str(exe)], check=True, creationflags=flags)
        failed = []
        for s in SCENARIOS:
            r = subprocess.run([str(exe), s], capture_output=True, text=True, creationflags=flags)
            sys.stdout.write(r.stdout)
            sys.stderr.write(r.stderr)
            if r.returncode:
                failed.append("%s (exit %d)" % (s, r.returncode))
        check(not failed, "menu_sound_test: " + ", ".join(failed))
    print("PASS: the options' own sounds: decoder, voices, files, blocks, Master volume")


if __name__ == "__main__":
    main()
