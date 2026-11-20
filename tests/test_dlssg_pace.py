"""Who holds the frame limit while NVIDIA DLSS frame generation runs: Reflex's sleep or the game's own wait (no game, no GPU).

With DLSS frame generation the game's frame waited twice: the host's wait where the frame starts (host/nn_vi.c) and Reflex's
sleep on the replay's thread after a present. With the Display tab's Reflex frame limit row on, Reflex's sleep holds the
frame rate cap and the host's wait steps aside (gpu/vk/vk_dlssg.cpp pace, after NX2navi's vk_dlssg_pace). The rule is
gpu/vk/dlssg_pace.h.

Compiles and runs the rule (tests/test_dlssg_pace.c, as C and as C++: vk_dlssg.cpp includes it), and reads the source for
what cannot run without the game and NVIDIA's files: that the row is off by default and pace() does nothing then, that
Reflex's limit is what pace() asked and Streamline took, that the replay's thread leaves its own sleep out exactly while the
game's presenting thread takes it, that a sleep Streamline does not take gives the frame back, and that the limit is only
ever wanted while frames are generated.

    python -I -B tests/test_dlssg_pace.py [--cc gcc] [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

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


def check_source():
    src = (ROOT / "gpu/vk/vk_dlssg.cpp").read_text(encoding="utf-8")
    real = src[src.index("#else", src.index("#ifndef HOOKSHOT_DLSSG")):]
    stubs = src[src.index("#ifndef HOOKSHOT_DLSSG"):src.index("#else", src.index("#ifndef HOOKSHOT_DLSSG"))]
    check("bool pace(int64_t) { return false; }" in stubs, "a build without Streamline no longer answers that the host waits")
    menu_h = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    check("int reflex_limit = 0;" in menu_h, "the Reflex frame limit row is not off by default")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    check('{TAB_DISPLAY, "Reflex frame limit", &opts.reflex_limit, 0, 1, 1, tx_onoff,' in menu, "the row is not on the Display tab")
    check(menu.index('"Game frame rate cap"') < menu.index('"Reflex frame limit"') < menu.index('{TAB_DISPLAY, "VSync"'),
          "the row is not beside the Game frame rate cap")

    pace = block(real, "bool pace(int64_t period_ns) {")
    # (the row off, and nothing of it left over: nothing decided, nothing asked of Streamline)
    off = pace.index("if (!opts.reflex_limit && !limit_want.load(std::memory_order_relaxed) && pace_said < 0) return false;")
    check(pace.index("if (!active()) return false;") < off < pace.index("dlssg_pace_step("), "pace() decides with the row off")
    check("opts.reflex_limit != 0 && p_reflex && p_sleep && !refused && sleeps != 0" in pace, "what allows Reflex to hold the limit changed")
    check("const uint64_t period = mult > 1 && period_ns > 0 ? (uint64_t)period_ns : 0;" in pace,
          "Reflex may hold the limit while no frame is generated")
    check("limit_set.load(std::memory_order_acquire), &want, &found)" in pace, "the rule is not given the limit Streamline last took")
    check("limit_want.store(want, std::memory_order_release);" in pace, "the wanted limit is not handed on")
    # (the sleep is taken only for a held frame, and a sleep that was not taken gives the frame, and the run, back)
    held = pace[pace.index("if (held) {"):]
    held = held[:held.index("game_sleeps.store(")]
    check("p_sleep(*t)" in held and "p_sleep(" not in pace.replace(held, ""), "Reflex's sleep is taken for a frame it does not hold")
    check("held = false, pace_state.bad = 1" in held and "limit_want.store(0" in held, "a sleep that was not taken still counts as Reflex's hold")
    check(pace.rstrip().endswith("return held;\n}"), "pace() answers something other than whether Reflex holds this frame")
    check("game_sleeps.store(held, std::memory_order_release);" in pace, "the replay's thread is not told who sleeps")

    start = block(real, "void frame_start() {")
    check("if (t && p_sleep && presented && !game_sleeps.load(std::memory_order_acquire)) p_sleep(*t);" in start,
          "the replay's thread sleeps in Reflex beside the game's presenting thread (or no longer at all)")

    apply = block(real, "void reflex_apply() {")
    check("limit_refused.load(std::memory_order_relaxed) ? 0 : limit_want.load(std::memory_order_acquire)" in apply and
          "o.frameLimitUs = us;" in apply, "Reflex's frame limit is not the one pace() asked for")
    check("limit_set.store(took ? us : 0, std::memory_order_release);" in apply, "what Streamline took is not kept")
    check("if (us) reflex_state();" in apply, "Streamline is asked for Reflex's state with no limit to give (the row off)")
    frame = block(real, "void frame(VkCommandBuffer cmd, uint32_t out_width, uint32_t out_height) {")
    check(frame.index("reflex_apply();") < frame.index("const uint32_t n = frame_no.load();"),
          "the limit is not handed to Streamline before the present")
    marker = block(real, "void present_marker(bool after) {")
    check("if (after && opts.reflex_limit && active()) hosted_ns.store(host->now_ns()" in marker, "the newest present's time is not kept")

    # (the host asks through vk_pace.cpp, and only its own thread's answer steps the wait aside: tests/test_pace_handoff.py)
    pace_cpp = (ROOT / "gpu/vk/vk_pace.cpp").read_text(encoding="utf-8")
    start = block(pace_cpp, "int frame_start() {")
    # (Reflex is asked with the period only where RivaTuner does not hold the frame: tests/test_rtss_limit.py)
    check("const bool reflex = dlssg::pace(rtss ? 0 : period) || (!dlssg::active() && reflex_pace(rtss || movie ? 0 : period));" in start and "const int who = pace_owner(rtss, reflex);" in start and
          "return who != PACE_GAME;" in start, "vk_pace.cpp no longer asks vk_dlssg.cpp")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    check("if (id == HOS_OPT_PACE_START) return pace::frame_start();" in replay and
          "if (id == HOS_OPT_PACE_PERIOD) return pace::set_period_ns(value);" in replay, "the renderer no longer answers the host")
    for doc, needle in (("readme.md", "| Reflex frame limit |"), ("docs/development.md", "`HOS_PACE_HANDOFF=0`")):
        check(needle in (ROOT / doc).read_text(encoding="utf-8"), doc + " does not say " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    src = str(ROOT / "tests/test_dlssg_pace.c")
    inc = ["-I", str(ROOT / "gpu/vk")]
    with tempfile.TemporaryDirectory(prefix="dlssg-pace-") as tmp:
        # (clang: undefined behaviour and unsigned wrap-around trap, since every period and time of the rule is a uint64_t)
        version = subprocess.run([args.cc, "--version"], capture_output=True, text=True).stdout
        ub = "undefined,unsigned-integer-overflow"
        trap = ["-fsanitize=" + ub, "-fsanitize-trap=" + ub] if "clang" in version else []
        for compiler, std, name in ((args.cc, "-std=gnu11", "c"), (args.cxx, "-std=c++20", "cxx")):
            exe = Path(tmp) / ("dlssg-pace-" + name)
            lang = ["-x", "c++"] if name == "cxx" else []
            subprocess.run([compiler, std, "-O1", "-Wall", "-Wextra", "-Werror", *(trap if name == "c" else []), *inc, *lang, src,
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
    print("PASS: Reflex or the game's own wait holds each frame, as gpu/vk/dlssg_pace.h rules")


if __name__ == "__main__":
    main()
