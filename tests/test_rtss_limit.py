"""RivaTuner Statistics Server as the game's frame limiter: the worker and the pacing against a fake RTSS (no RTSS, no game).

Where RTSS is installed and running, the game's frame limit goes into RTSS's profile for the game's executable through
RTSS's hooks library, is read back, and once RTSS is seen holding the game to it the game's own wait steps aside
(gpu/vk/rtss_limit.cpp, gpu/vk/vk_pace.cpp; the rules are tests/test_rtss_decide.py's). Nothing here loads RTSS's
library or calls anything of RTSS's: every call goes through rtss_limit.h's Sys, which tests/test_rtss_limit.cpp gives as
a stand-in (a fake hooks library with the profile calls, fake profile files, a fake shared memory, a fake state file and
a fake clock), and gpu/vk/rtss_sys_win.cpp, the real one, is only read.

Builds tests/test_rtss_limit.cpp with the real rtss_limit.cpp and vk_pace.cpp cut out of its source (its worker thread
left out: the scenarios poll the worker every 500 ms of the fake clock) and runs each scenario in a process of its own: a
profile that takes the write and one that does not (silent failure, read back); the player's own limit where it does
not; the restore at the exit; the state file across a kill (written before the first change, read at the next start,
left alone when the player changed the profile since, other executables' lines kept, an unreadable file left alone);
HOS_RTSS=0 (no call of RTSS's at all, a leftover line only reported); RTSS not installed or not running; the limit
written for each frame generation mode and multiplier, a multiplier that flickers; the retry after a give-back; RTSS
that never shows its hold; and one limiter in every combination of caps, frame generation modes, the Reflex row, a
movie, a minimised window and each physical safeguard. Then reads the sources for what cannot run here.

    python -I -B tests/test_rtss_limit.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def code_of(text):
    """The text without its // comments (what the compiler sees, near enough for a search for calls)"""
    return "\n".join(line.split("//")[0] for line in text.splitlines())


def once(text, old, new):
    check(text.count(old) == 1, "gpu/vk/vk_pace.cpp no longer has %r once" % old)
    return text.replace(old, new)


def pace_cut():
    """vk_pace.cpp as the test compiles it: its includes of the renderer's headers out (the test gives stand-ins), the
    worker thread out, report_left on the calling thread, and a reset of its state between scenarios"""
    src = (ROOT / "gpu/vk/vk_pace.cpp").read_text(encoding="utf-8")
    for inc in ('#include "vk_dlssg.h"\n', '#include "vk_framegen.h"\n', '#include "vk_internal.h"\n', '#include "vk_reflex.h"\n'):
        src = once(src, inc, "")
    src = once(src, "std::thread(rtss_worker).detach();", "test_worker_started();")
    src = once(src, "std::thread(rtss::report_left).detach();", "rtss::report_left();")
    reset = ("void test_reset() {\n"
             "    period_ns = 0, movie_on = 0, owner_now = GAME, yields = 0, fg_now = PACE_FG_OFF, mult_now = 1, hosted = 0;\n"
             "    watch = RtssWatch{}, start_last = start_before = 0, begun = 0, mode_known = -2;\n"
             "    dlssg_try_known = -1;\n"
             "}\n")
    return once(src, "\n} // namespace\n\nvoid set_period_ns", "\n" + reset + "} // namespace\n\nvoid set_period_ns")


def check_source():
    vk = ROOT / "gpu/vk"
    limit = (vk / "rtss_limit.cpp").read_text(encoding="utf-8")
    code = code_of(limit)
    # (only the two limit properties are ever set or read through RTSS's library, and only the game's own profile is
    # loaded, saved or removed)
    check('constexpr const char* kLimit = "FramerateLimit";' in limit and
          'constexpr const char* kDenominator = "FramerateLimitDenominator";' in limit, "the two properties' names changed")
    for call in ("set_property", "get_property"):
        names = set(re.findall(r"sys->%s\(\s*(\w+)" % call, code))
        check(names and names <= {"kLimit", "kDenominator"}, "%s is called with %s" % (call, sorted(names)))
    check(not re.search(r'"Framerate\w*"', code.replace('"FramerateLimit";', "").replace('"FramerateLimitDenominator";', "")),
          "a property named by a string of its own")
    for call in ("load_profile", "save_profile", "delete_profile"):
        args = set(re.findall(r"sys->%s\(([^)]*)\)" % call, code))
        check(args == {"s.profile"}, "%s is called for %s, not the game's own profile alone" % (call, sorted(args)))
    check(set(re.findall(r"sys->file_write\(([^,]+),", code)) == {"s.state_path"} and
          set(re.findall(r"sys->file_delete\(([^)]+)\)", code)) == {"s.state_path"},
          "a file other than the state file is written or deleted")
    check('"%s\\\\RTSSHooks64.dll"' in limit, "the hooks library is not RTSS's own, from its folder")
    # (HOS_RTSS=0: vk_pace.cpp calls nothing of rtss_limit.cpp's but start and report_left, which reaches only the game's own
    # executable name and state file)
    pace = (vk / "vk_pace.cpp").read_text(encoding="utf-8")
    frame = block(pace, "bool rtss_frame(")
    off = frame[frame.index("if (mode == 0) {"):frame.index("return false;", frame.index("if (mode == 0) {"))]
    check(set(re.findall(r"rtss::(\w+)", off)) == {"start", "real_sys", "report_left"}, "HOS_RTSS=0 calls more of RTSS's part")
    report = block(limit, "void report_left() {")
    check(set(re.findall(r"sys->(\w+)", report)) <= {"exe_name", "state_path", "file_read"}, "report_left reaches more than the game's files")
    check("if (rtss_mode() > 0) hosted.store(" in block(pace, "void presented() {"), "presents are noted with HOS_RTSS=0")
    exiting = block(pace, "void exiting() {")
    check("begun.store(1);" in exiting and "if (rtss_mode() > 0) rtss::restore();" in exiting, "the exit does not close RTSS's part")
    restore = block(limit, "void restore() {")
    check(restore.index("s.closed.store(1);") < restore.index("if (!s.ready.load(std::memory_order_acquire)) return;") <
          restore.index("put_back()"), "an exit before the worker found RTSS may reach it")
    check(frame.index("std::thread(rtss_worker)") > frame.index("if (mode == 0) {"), "the worker can start with HOS_RTSS=0")
    check('getenv("HOS_RTSS")' in block(pace, "int rtss_mode() {"), "HOS_RTSS is not read")
    # (who holds a frame: RTSS before Reflex, which is given no limit while RTSS holds; the display thread yields to RTSS)
    start = block(pace, "int frame_start() {")
    check("const bool reflex = dlssg::pace(rtss ? 0 : period) || (!dlssg::active() && reflex_pace(rtss || movie ? 0 : period));" in start and "const int who = pace_owner(rtss, reflex);" in start and
          "yields.store(pace_display_yields(who, fg)" in start and start.rstrip().endswith("return who != PACE_GAME;\n}"),
          "frame_start no longer decides one limiter by pace_rule.h")
    # (the real interface: the only file that reaches RTSS, the registry or its shared memory, and only to read them)
    win = code_of((vk / "rtss_sys_win.cpp").read_text(encoding="utf-8"))
    check(re.findall(r"OpenFileMapping\w*\(([^,]+),", win) == ["FILE_MAP_READ"] and
          re.findall(r"MapViewOfFile\w*\(\s*\w+,\s*([^,]+),", win) == ["FILE_MAP_READ"], "RTSS's shared memory is not only read")
    check(re.findall(r"RegGetValue\w*\(([^,]+),", win) == ["HKEY_LOCAL_MACHINE"], "the registry is read otherwise")
    for banned in ("RegSet", "RegCreate", "RegDelete", "CreateProcess", "ShellExecute", "WinExec", "TerminateProcess", "OpenProcess",
                   "FILE_MAP_WRITE", "FILE_MAP_ALL_ACCESS", "CreateFileMapping", "EnumProfiles", "SetProfileProperty(\"", "runas"):
        check(banned not in win, "rtss_sys_win.cpp uses " + banned)
    exports = set(re.findall(r'GetProcAddress\(dll, "(\w+)"\)', win))
    check(exports == {"LoadProfile", "SaveProfile", "DeleteProfile", "GetProfileProperty", "SetProfileProperty", "UpdateProfiles"},
          "RTSS's library is asked for %s" % sorted(exports))
    for name in ("void load_profile(", "void save_profile(", "void delete_profile("):
        check("if (name && *name)" in block(win, name), name + " may reach RTSS's global profile (an empty name)")
    check(re.search(r"CreateFileA\(path, GENERIC_READ,", win) and "GENERIC_WRITE" not in win, "a file is opened for writing")
    for f in vk.glob("*.cpp"):
        if f.name in ("rtss_sys_win.cpp", "rtss_limit.cpp"):
            continue
        text = code_of(f.read_text(encoding="utf-8", errors="replace"))
        for name in ("RTSSHooks64", "RTSSSharedMemoryV2", "LoadProfile", "SaveProfile", "SetProfileProperty"):
            check(name not in text, "%s reaches RTSS (%s)" % (f.name, name))
    build = (ROOT / "nx2/build.py").read_text(encoding="utf-8")
    check('"@" + vrsp, "-luser32", "-ladvapi32"]' in build, "triforce_vk.dll is not linked with advapi32 (the registry)")

    # (no row: on wherever RTSS runs; a line of the test builds' row is dropped without complaint)
    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8")
    menu_h = (vk / "vk_menu.h").read_text(encoding="utf-8")
    check('"RivaTuner limit"' not in menu and "opts.rtss" not in menu and "int rtss" not in menu_h, "the RivaTuner limit row is still there")
    check('mine |= key == "rivatuner_limit";' in block(menu, "void save() {"), "an old rivatuner_limit= line is written back")
    # ((Triforce, options_version 7) no Reflex frame limit row: the Low latency row's line in the readme says RivaTuner wins)
    check('"Reflex frame limit"' not in menu, "the Reflex frame limit row is still there")
    # ((Triforce) NX2navi's full overlay: its Frame limit row names who holds the frames, vk_pace.cpp's owner, after the rate)
    check("const int holder = mkwvk::pace::owner();" in menu and '" (RivaTuner)" : " (NVIDIA Reflex)"' in menu,
          "the overlay's Frame limit line does not say who holds the frames (vk_pace.cpp)")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("| RivaTuner limit |" not in readme and "`rivatuner_limit` |" not in dev, "the row is still documented")
    check("RivaTuner Statistics Server is installed and running" in readme and "`HOS_RTSS=0`" in readme, "readme.md does not say what happens")
    check("| `HOS_RTSS=0` |" in dev and "| `HOS_RTSS_DLSSG=1` |" in dev and "`triforce_rtss_profile.txt`" in dev,
          "docs/development.md does not have the switches and the state file")

    # (the exit puts the profile back before Streamline's shutdown returns from the call)
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    opt_set = block(replay, "t.option_set = [](int id, int value) {")
    check(0 <= opt_set.find("if (id == HOS_OPT_EXITING) pace::exiting();") < opt_set.find("return dlssg::shutdown();"),
          "pace::exiting() is not reached at the exit")
    check("if (id == HOS_OPT_PACE_MOVIE) return pace::set_movie(value);" in opt_set, "the renderer is not told of movies")
    # (the in-house frame generation: the display thread's own pacing stands aside while RTSS holds its presents, and the
    # game is held by the display thread taking its frames)
    fg = (vk / "vk_framegen.cpp").read_text(encoding="utf-8")
    check("const bool pace = opts.pace != 0 && !pace::display_yields();" in fg, "the display thread paces beside RTSS")
    check("(!pace::display_yields() || fg.jobs.empty())" in block(fg, "bool framegen_record("), "the game runs ahead of RTSS's hold")
    main = block(fg, "void display_main() {")
    check(main.index("fg.jobs.pop_front();") < main.index("if (pace::display_yields()) fg.cv.notify_all();") < main.index("present_image("),
          "the next frame waits for the shown frames' presents, not for its job to be taken")
    check("int framegen_multiplier() { return !framegen_on() ? 0 : opts.framegen == 1 ? -1 : multiplier(); }" in fg,
          "what a limit on presented frames is multiplied by changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="rtss-limit-") as tmp:
        (Path(tmp) / "pace_cut.inc").write_text(pace_cut(), encoding="utf-8")
        exe = Path(tmp) / "rtss-limit.exe"
        # (rtss_worker, the thread's loop, is left uncalled: the scenarios poll themselves)
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Werror", "-Wno-unused-function", "-I", str(ROOT / "gpu/vk"), "-I", tmp,
                        str(ROOT / "tests/test_rtss_limit.cpp"), "-o", str(exe)], check=True)
        names = subprocess.run([str(exe), "--list"], capture_output=True, text=True, check=True).stdout.split()
        check(len(names) >= 13, "the scenarios are not listed")
        failed = []
        for name in names:
            p = subprocess.run([str(exe), name], capture_output=True, text=True, timeout=600)
            if p.returncode:
                print(p.stdout + p.stderr)
                failed.append(name)
            else:
                print(p.stdout.strip().splitlines()[-1])
        check(not failed, "scenarios failed: " + ", ".join(failed))
    print("PASS: RivaTuner holds the game where it can, one limiter in every case, and its profile comes back")


if __name__ == "__main__":
    main()
