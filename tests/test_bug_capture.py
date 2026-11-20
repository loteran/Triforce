"""The bug capture: F8, or the System tab's Bug capture row (host/bug_capture.c, gpu/vk/vk_bugshot.cpp, gpu/vk/bugshot_pixels.h,
host/display.c, gpu/vk/vk_menu.cpp). No game, no GPU.

A capture is one folder a player can send with a problem report, captures/<date>-<time> in the game folder: frame.png,
triforce_options.ini, the save as Export save writes it, log-tail.txt and capture.txt.

Run: tests/save_share_test.c, which is host/bug_capture.c and host/save_share.c whole with host/nn_fs.c's own text for the
save, on a folder that plays the game folder. The renderer, the game's thread and the controls are stand-ins: the fake
renderer writes captures/frame-pending.png a few questions after it was asked, as the real one does three presents on.

- F8: the press itself only notes the frame count, asks the renderer for the picture and the game's thread where Link is;
  the folder is written by the save rows' thread. In it: the picture moved in, the settings, the save the game committed
  (read back by Python's zipfile; not what it was writing since), the log's last 400 lines up to the capture's own, and
  capture.txt, last, with the version, the log's lines for the GPUs and the driver, the frame count at the press, the frame
  rate before it and the map and Link's place the game's thread said;
- one line in the log says where the capture goes; the row is told the folder, the foot of the screen a short line;
- Link's place is the game's thread's first answer after the press's own line in the log, whichever of the capture's
  other lines comes before or after it;
- a press while one is being written is skipped; the next one gets a folder of its own;
- no picture from the renderer, and no frame shown at all (three seconds, then it is asked for no longer): the rest of
  the folder is written and capture.txt says why there is no picture;
- at the title screen (no answer from the game's thread): the place is "not known";
- F8 bound to a control by the player: no capture from the key;
- the thread itself: nothing asked, nothing done; the row's ask and the key's ask each write one capture.

Compiled and run: gpu/vk/bugshot_pixels.h (tests/bug_capture_pixels_test.cpp): RGBA and BGRA, 10-bit sRGB codes, and HDR10
brought to sRGB with the paper white as white, at three paper whites.

Read in the sources: F8 is not one of the default keys; the window's key handler takes it once a press and only calls the
note; the press does no file work; the picture is copied before the overlays and not during the start-up screen, from the
window's image, which goes back to the layout it had, and read three presents later, the number of frame slots a present
waits for; the swapchain still asks for read-back where the driver offers it; the option ids; the row and the readme.

    python -I -B tests/test_bug_capture.py [--cc gcc] [--cxx g++]
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import test_save_share as share   # (the game folder of the save rows' test, and its program)

check = share.check


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_sources():
    read = lambda rel: (ROOT / rel).read_text(encoding="utf-8")
    display, capture, hid, menu = read("host/display.c"), read("host/bug_capture.c"), read("host/nn_hid.c"), read("gpu/vk/vk_menu.cpp")
    shot, replay, main_cpp, api, readme = read("gpu/vk/vk_bugshot.cpp"), read("gpu/vk/vk_replay.cpp"), read("gpu/vk/vk_main.cpp"), read("gpu/vk/vk_api.h"), read("readme.md")
    # the key: free by default, taken once a press, and the handler only notes it
    defaults = hid[hid.index("static const unsigned char key_default[HOS_KEY_N][2] = {"):]
    defaults = defaults[:defaults.index("};")]
    check("VK_F8" not in defaults and "0x77" not in defaults, "F8 is one of the default keys (host/nn_hid.c key_default)")
    keydown = display[display.index("case WM_KEYDOWN: {"):display.index("case WM_SYSKEYDOWN:")]
    check("if (wp == VK_F8 && !(lp & (1 << 30))) hos_bug_capture_key();" in keydown, "host/display.c's key handler no longer takes F8 once a press")
    key = block(capture, "void hos_bug_capture_key(void) {")
    check("hos_binding(HOS_BIND_KEY, button, slot) == VK_F8" in key and key.index("return;") < key.index('hos_bug_capture_ask("F8");'),
          "F8 bound to a control by the player still takes a capture")
    ask = block(capture, "int hos_bug_capture_ask(const char *trigger) {")
    for work in ("fopen", "CreateDirectory", "hos_save_export", "hos_log_flush", "Sleep(", "MoveFileEx", "read_part", "write_file"):
        check(work not in ask, "the press itself does file work or waits (%s): the game's window would stall" % work)
    check("rx_option_set(HOS_OPT_BUG_SHOT, 1);" in ask and 'hos_oot_live_crawl("!where");' in ask and "hos_save_share_wake();" in ask,
          "the press no longer asks for the picture, for Link's place, or wakes the thread that writes the folder")
    check(ask.index('hos_log("capture: bug capture %d asked for (%s) at present %lld"') < ask.index('hos_oot_live_crawl("!where");'),
          "the press's own line is no longer in the log before the game's thread is asked where Link is: its answer could not be told from an older one")
    work = block(capture, "void hos_bug_capture_work(void) {")
    check(work.index("capture.txt.part") > work.index("log-tail.txt") > work.index("frame.png") and
          work.rindex("MoveFileExA(path, other, MOVEFILE_REPLACE_EXISTING)") > work.index("capture.txt.part"), "capture.txt is no longer written last, whole")
    check("InterlockedCompareExchange(&capture_state, 2, 1) != 1" in work, "a capture can be written twice at once")
    # the picture: before the overlays, not during the start-up screen, three presents before it is read
    record = block(menu, "void menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) {")
    call = "if (!start) bugshot_record(cmd, index, w, h);"
    check(call in record and record.index(call) < min(record.index(s) for s in ("ui::begin(", "ui::idle()", "draw_menu(", "draw_fps(", "ui::record(")),
          "the capture's picture is not taken before the overlays are drawn (or is taken of the start-up screen)")
    rec = block(shot, "void bugshot_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) {")
    order = [rec.index(s) for s in ("barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);",
                                    "vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);",
                                    "b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;",
                                    "barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);",
                                    "presents_left = 3;")]
    check(order == sorted(order), "vk_bugshot.cpp no longer copies the window's image out and puts it back as it was")
    check("const VkImage image = dev.swap_images[index];" in rec and "if (--presents_left > 0) return;" in rec, "the copy is not of the window's image, or is read too soon")
    check("caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT" in rec and "dev.offscreen" in rec, "a window whose images cannot be read back is copied from all the same")
    present = replay[replay.index("void present_image(VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImageView view) {"):]
    slots = [present.index(s) for s in ("frame_index = (frame_index + 1) % 3;", "vkWaitForFences(dev.device, 1, &f.done, VK_TRUE, UINT64_MAX)",
                                        "menu_record(f.cmd, index, dev.swap_extent.width, dev.swap_extent.height);", "vkQueueSubmit2(dev.queue, 1, &si, f.done);")]
    check(slots == sorted(slots), "a present no longer has three frame slots whose fence it waits for before menu_record: vk_bugshot.cpp's three presents no longer hold")
    check("if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)\n        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;" in main_cpp,
          "the swapchain no longer asks for read-back where the driver offers it")
    check(shot.rstrip().rfind("#include") == shot.rfind('#include "vk_work.h"'), "vk_work.h is not vk_bugshot.cpp's last include: its commands would not go the recorder's way")
    # the options between the host and the renderer
    option = replay[replay.index("t.option = "):]
    option = option[:option.index("};") + 2]
    check("if (id == HOS_OPT_BUG_SHOT) return menu_bug_shot();" in option and "int menu_bug_shot() { return bugshot_state(); }" in menu,
          "the host cannot ask what became of the picture")
    sets = block(menu, "void menu_option_set(int id, int value) {")
    check("if (id == HOS_OPT_BUG_SHOT) {" in sets and "bugshot_request(value != 0);" in sets, "the host cannot ask for the picture")
    enum = block(api, "enum { HOS_OPT_SHARE_ASK")
    names = re.findall(r"\bHOS_OPT_\w+", enum)
    check("HOS_OPT_SHARE_ASK = 70" in enum and names[-1] == "HOS_OPT_BUG_SHOT" and 70 + len(names) - 1 <= 79, "the option ids are not within 70 to 79")
    # the row, the key where the window has no handler of the host's, the notice, the readme
    check('{TAB_SYSTEM, "Bug capture (F8)", nullptr,' in menu and 'if (!strcmp(r.name, "Bug capture (F8)")) return HOS_SHARE_CAPTURE;' in menu,
          "the System tab has no Bug capture (F8) row that acts on a press")
    tick = block(menu, "void menu_tick() {")
    check("!(host && host->take_key_press) && (GetAsyncKeyState(VK_F8) & 0x8000) != 0" in tick and "share_ask.store(HOS_SHARE_CAPTURE_KEY" in tick,
          "where the window has no key handler of the host's (Linux) F8 is not read")
    check("share_notice_new.exchange(false" in tick and "share_lines[HOS_SHARE_NOTICE].c_str()" in tick, "the host's line for the foot of the screen is not shown")
    for needle in ("| Bug capture (F8) |", "**F8**", "captures", "capture.txt", "frame.png", "log-tail.txt"):
        check(needle in readme, "readme.md does not say %r of the bug capture" % needle)


def run_pixels(cxx, tmp):
    exe = tmp / "pixels.exe"
    build = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu" / "vk"), str(ROOT / "tests" / "bug_capture_pixels_test.cpp"), "-o", str(exe)],
                           capture_output=True, text=True)
    check(build.returncode == 0, "tests/bug_capture_pixels_test.cpp does not build:\n" + build.stderr)
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    check(run.returncode == 0 and "PASS" in run.stdout, "gpu/vk/bugshot_pixels.h:\n" + run.stdout + run.stderr)


def folders(g):
    d = g.dir / "captures"
    return sorted(p for p in d.iterdir() if p.is_dir()) if d.is_dir() else []


def read_capture(g, folder, out, save_tag, picture, place=True, trigger="F8"):
    """one capture's folder, whole: its files and what capture.txt says"""
    check(re.fullmatch(r"\d{8}-\d{6}(-\d+)?", folder.name), "the capture's folder is named " + folder.name)
    want = {"capture.txt", "triforce_options.ini", "log-tail.txt"} | ({"frame.png"} if picture else set())
    names = {p.name for p in folder.iterdir()}
    zips = [n for n in names if n.endswith(".zip")]
    check(names - set(zips) == want and len(zips) == 1 and re.fullmatch(r"Triforce-save-\d{8}-\d{6}\.zip", zips[0]), "%s holds %r" % (folder.name, sorted(names)))
    check((folder / "triforce_options.ini").read_bytes() == (g.dir / "triforce_options.ini").read_bytes(), "the capture's settings are not the game folder's")
    if picture:
        check((folder / "frame.png").read_bytes() == b"the frame's picture", "the capture's picture is not the renderer's")
    check(not (g.dir / "captures" / "frame-pending.png").exists(), "the renderer's picture was left in captures")
    share.read_export(folder / zips[0], save_tag)
    # one line in the log says where it goes, and the log's end is in the folder up to that line and Link's place after it
    said = [line for line in out.splitlines() if line.startswith("log: capture: bug capture ")]
    mine = [line for line in said if line.endswith(" goes to " + str(folder))]
    check(len(mine) == 1 and any(("asked for (%s) at present " % trigger) in line for line in said), "the log does not say once where the capture goes:\n" + "\n".join(said))
    log = (g.dir / "triforce_log.txt").read_bytes()
    tail = (folder / "log-tail.txt").read_bytes()
    check(tail and tail in log and tail.count(b"\n") == 400 and not tail.startswith(b"\n"), "log-tail.txt is not 400 whole lines of the log (%d)" % tail.count(b"\n"))
    marker = b"asked for (%s) at present " % trigger.encode()
    check(marker in tail and str(folder).encode() in tail, "the log's end in the capture stops before the capture's own line")
    check(log.index(tail) > 0 and log[log.index(tail) - 1:log.index(tail)] == b"\n", "log-tail.txt does not start at a line's start")
    text = (folder / "capture.txt").read_text(encoding="utf-8")
    presents = int(re.search(r"presents (\d+) where asked", out).group(1))
    for needle in ("Triforce bug capture " + folder.name, "trigger = " + trigger, "version = v0.4.0-test", "log = triforce v0.4.0-test (source 0123abcd)",
                   "log = vk: device 0: Some GPU 9000, Vulkan 1.4, driver 0x9b404000, type 2", "log = vk: device 1: Another GPU, Vulkan 1.3, driver 0x1, type 1",
                   "log = vk: Some GPU 9000, Vulkan 1.4, driver 0x9b404000; mesh shaders yes", "log = vk: vendor fixes for NVIDIA (621.01)",
                   "presents = %d " % presents, "settings = triforce_options.ini", "save = %s (3 files" % zips[0], "log tail = log-tail.txt"):
        check(needle in text, "capture.txt of %s does not say %r:\n%s" % (folder.name, needle, text))
    check("[hos " not in text and "log = line " not in text, "capture.txt carries log lines that are not the build's or the GPU's")
    rate = float(re.search(r"frame rate = ([\d.]+) a second over the second before the press", text).group(1))
    # (6 frames every tenth of a second, as this PC's clock counts a tenth: slower on a busy one, never faster. The 600
    # frames shown after the press are in neither number)
    check(15 <= rate <= 62, "capture.txt's frame rate is %.1f for 6 frames every tenth of a second" % rate)
    tenths = re.search(r"frames each tenth of a second before the press =((?: \d+(?:/\d+ms)?)+)\n", text).group(1).split()
    check(len(tenths) >= 10 and {t.split("/")[0] for t in tenths} == {"6"}, "capture.txt's frames by the tenth of a second: %r" % tenths)
    check(all(int(t.split("/")[1][:-2]) > 125 for t in tenths if "/" in t), "a tenth of a second is written with its length: %r" % tenths)
    if place:
        check('place = map "KokiriForest_Child" loading 0 event 0 child 1 pos -12.5 3.0 480.2\n' in text, "capture.txt does not have the place the game's thread said:\n" + text)
    else:
        check("place = not known" in text, "capture.txt names a place nobody said:\n" + text)
    check(("picture = frame.png" in text) == picture and ("picture = none" in text) != picture, "capture.txt on its picture:\n" + text)
    return text


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    check_sources()
    cc, cxx = shutil.which(a.cc) or shutil.which("gcc") or shutil.which("clang"), shutil.which(a.cxx) or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        print("skip: no C++ compiler; the sources were checked")
        return
    with tempfile.TemporaryDirectory(prefix="triforce-bug-capture-") as tmp:
        tmp = Path(tmp).resolve()
        run_pixels(cxx, tmp)
        if sys.platform != "win32" or not cc:
            print("PASS: the picture's pixels and the sources (the capture's own program calls the Windows file functions: not run here)")
            return
        g = share.Game(cc, tmp)
        # the game folder: settings, a log with the lines a real one starts with and a thousand after them, a save
        (g.dir / "triforce_options.ini").write_bytes(b"fullscreen=1\nframe_rate_cap=2\nkey_zr=88,70\n")
        head = ["[a64] unimplemented import _ZN2nn2ro12ProtectRelroEPKvS2_S2_S2_S2_ (lr 0x81237908)", "[hos host] triforce v0.4.0-test (source 0123abcd)",
                "[hos host] oe: output resolution 2560x1440", "[hos sead::MainThread] fs: mounted content: -> C:\\game\\romfs",
                "[hos host] vk: device 0: Some GPU 9000, Vulkan 1.4, driver 0x9b404000, type 2", "[hos host] vk: device 1: Another GPU, Vulkan 1.3, driver 0x1, type 1",
                "[hos host] vk: vendor fixes for NVIDIA (621.01): out-of-bounds texel fetches 0 0, NaN tests by bits 0, gather offset 0",
                "[hos host] vk: Some GPU 9000, Vulkan 1.4, driver 0x9b404000; mesh shaders yes, host memory import yes, pipeline libraries yes"]
        (g.dir / "triforce_log.txt").write_bytes(("\n".join(head + ["[hos host] line %d of the run" % i for i in range(1000)]) + "\n").encode())
        g.run("play", "v1")
        check(share.tag_of(g.save) == "v1", "the fixture's save")

        # F8 in play
        out = g.run("capture", "picture")
        made = folders(g)
        check(len(made) == 1, "F8 made %d capture folders:\n%s" % (len(made), out))
        check("where asked 1" in out and out.splitlines().count("shot asked") == 1 and "log: capture: F8 again: skipped" in out, "the press:\n" + out)
        row = g.said(out, share_capture_row())
        check(row == "Saved in " + str(made[0]), "the row is told %r" % row)
        notice = g.said(out, share_notice_row())
        check(notice == "Bug capture saved: captures\\" + made[0].name, "the notice is %r" % notice)
        read_capture(g, made[0], out, "v1", picture=True)
        took = int(re.search(r"written in (\d+) ms", out).group(1))
        check(took < 2900, "a capture with its picture took %d ms: as long as one that waits for a frame that never comes" % took)

        # the renderer has no picture; no frame is shown at all; the title screen; two presses
        out = g.run("capture", "nopicture")
        made = folders(g)
        check(len(made) == 2 and "without a picture" in g.said(out, share_capture_row()), "a capture without a picture:\n" + out)
        text = read_capture(g, made[1], out, "v1", picture=False)
        check("the renderer could not read the window's picture" in text, "capture.txt on a picture the renderer could not take:\n" + text)
        out = g.run("capture", "noframes")
        made = folders(g)
        check(len(made) == 3 and "shot asked no longer" in out, "a capture while no frame is shown:\n" + out)
        text = read_capture(g, made[2], out, "v1", picture=False)
        check("no frame was shown in three seconds" in text and 2900 <= int(re.search(r"written in (\d+) ms", out).group(1)), "capture.txt without frames:\n" + text)
        out = g.run("capture", "title")
        made = folders(g)
        check(len(made) == 4, "a capture at the title screen:\n" + out)
        read_capture(g, made[3], out, "v1", picture=True, place=False)
        out = g.run("capture", "twice")
        made = folders(g)
        check(len(made) == 6 and made[4] != made[5], "two captures one after the other:\n" + out)
        for folder, trigger in ((made[4], "F8"), (made[5], "the System tab's row")):
            check((folder / "capture.txt").is_file() and ("trigger = " + trigger) in (folder / "capture.txt").read_text(encoding="utf-8"), "%s is not whole" % folder.name)

        # F8 bound to a control: the key is the game's
        out = g.run("capture", "bound")
        check(len(folders(g)) == 6 and "log: capture: F8 is bound to one of the controls" in out and "shot asked" not in out, "F8 bound to a control:\n" + out)

        # the thread itself: the row's ask, the key's ask (where the window has no handler of the host's), and a save row's
        for ask, row in ((share_capture_row(), share_capture_row()), (share_capture_row() + 1, share_capture_row()), (1, 1)):
            before = len(folders(g))
            out = g.run("thread", str(ask))
            check(len(folders(g)) == before + (1 if ask != 1 else 0) and len([l for l in out.splitlines() if l.startswith("said %d: Saved " % row)]) == 1,
                  "the thread, asked %d:\n%s" % (ask, out))
        check(all((f / "capture.txt").is_file() and not (f / "capture.txt.part").exists() for f in folders(g)), "a capture folder is not whole")
    print("PASS: the bug capture: its folder, its picture's pixels, the press, the thread, and where it is wired in")


def share_ids():
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    names = re.findall(r"\bHOS_SHARE_\w+", block(api, "enum { HOS_SHARE_EXPORT"))
    return {name: i + 1 for i, name in enumerate(names)}


def share_capture_row():
    return share_ids()["HOS_SHARE_CAPTURE"]


def share_notice_row():
    return share_ids()["HOS_SHARE_NOTICE"]


if __name__ == "__main__":
    main()
