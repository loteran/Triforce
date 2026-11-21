"""Hold the last picture while new textures come in (gpu/vk/present_hold.h; vk_texture.cpp, vk_replay.cpp do_present).

At an area change one frame took every new texture's upload at once (523 ms for 363 MB in the owner's test5 session). New
textures now have a budget a frame too: once a frame's uploads took 12 ms or 64 MB, a new texture a draw samples is left for
the next frame and the draw samples the dummy in its place. By the owner's choice such a frame is not shown: the window gets
the last complete picture again, with the Insert menu and the overlays drawn over it, until a frame is drawn with every
texture complete, or the hold reaches its bound (RX_VK_HOLD_MS, 250). No frame generation makes a frame from a held one.

What must hold, checked without the game or a GPU:
- present_hold.h (compiled and run, tests/present_hold_test.cpp): a frame owing a texture its first bytes is held until one is
  drawn complete; the frame at the bound is held with the flush (nothing left for later) and the one after it shown whatever it
  holds; a rest after a hold at its bound, doubling, back to the shortest after a hold that ended by itself or a quiet stretch;
  a card whose every frame owes textures is never frozen (a minute of it: no hold past the bound, under 5% of frames held);
- the budget, cut from vk_texture.cpp and run against stand-ins (tests/present_hold_budget_test.cpp): 12 ms or 64 MB, the
  frame's first upload always going, never for a dispatch, without the hold or at its bound; a new ASTC texture under the hold
  has the same budget and is counted owed, at the bound never left, and as before without the hold;
- the source (read): do_present decides before the stall line and gives the next frame's draws their mode; a held frame shows
  the held picture through present_image, where the menu and the overlays are drawn, or through frame generation's display
  thread when it runs, and is never given to frame generation; its depth and motion are dropped; the frame shown after a hold
  starts frame generation's history anew (no frame made between it and the picture before the hold), DLSS frame generation
  takes a held present as a frame with no scene and the next as a new start, and the temporal upscalers start their history
  anew after a held frame; the cache's lookups of the dummy go stale at the frame's end; the stall line and the 600-frame line
  say what was held; the switches are in the development guide.

    python -I -B tests/test_present_hold.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

RULE_CASES = ["off", "held-until-complete", "bound", "flush", "rest-doubles", "never-frozen", "resting-owed", "off-while-holding"]
BUDGET_CASES = ["budget", "astc-hold", "astc-as-before"]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit(where + " no longer has: " + needle)


def in_order(where, text, needles):
    at = [text.index(n) if n in text else -1 for n in needles]
    if -1 in at or at != sorted(at):
        raise SystemExit(where + ": not in this order: " + " / ".join(needles))


def check_source(src):
    tex = (src / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    frame = block(tex, "void texture_frame(uint64_t completed) {")
    in_order("vk_texture.cpp texture_frame (the dummy's lookups go stale)", frame,
             ["if (frame_owed.exchange(0, std::memory_order_relaxed)) {", "destroy_gen++;", "frame_no++;"])
    need("vk_texture.cpp texture_frame", frame, ["frame_upload_bytes.store(0"])
    need("vk_texture.cpp upload()", block(tex, "bool upload(Texture& tex, const Ranges* dirty) {"), ["frame_upload_bytes.fetch_add("])

    replay = (src / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    step = block(replay, "bool hold_step() {")
    need("vk_replay.cpp hold_step", step,
         ["const int64_t bound = dev.offscreen ? 0 : hold_bound_ns();",
          # ((Triforce) a frame with draws left out for a pipeline is owed too: tests/test_pipe_wait.py)
          "present_hold::decide(hold, bound, texture_frame_owed() > 0 || left_out > 0, now, present);",
          "texture_hold_mode(present_hold::mode(hold, bound, present + 1));", "held_last.store(held"])
    need("vk_replay.cpp hold_bound_ns", block(replay, "int64_t hold_bound_ns() {"), ['getenv("RX_VK_HOLD_MS")', ": 250.0"])
    present = block(replay, "void do_present(const nvn_tex* t, const int crop[4]) {")
    in_order("vk_replay.cpp do_present (decided before the stall line and before the frame counts on)", present,
             ["const bool held = hold_step();", "status_tick();", "texture_frame(completed());"])
    need("vk_replay.cpp do_present", present, [
        "const bool have = hold.frames == 1 ? held_take(r.cmd, tex) : held_pic.valid;",
        "fsr3_inputs::take(dropped);",
        "} else if (was_held && tex) {\n            framegen_new_history();",
        "const bool fg_after = framegen_on() && tex && !fg_inline && !held && !ffx_swapchain;",   # (linux fork: AMD's swapchain)
        "interpolated = framegen_on() && tex && fg_inline && !held && !ffx_swapchain &&",
        "else if (!held) menu_note_frame(true, ffx_swapchain && ffxfg::generating() ? 2 : dlssg::multiplier());",   # ((Triforce, 11-16) DLSS frame generation's frames counted)
        "if (!held && tex && src && !dev.offscreen) {",
        "if (framegen_on()) return framegen_queue_held(src, direct_view, sw, sh, show_crop);",
        "present_image(src, sw, sh, show_crop, direct_view);"])
    if "if (tex && src && !dev.offscreen && !held) {" not in present:
        raise SystemExit("vk_replay.cpp do_present: a held frame makes the present's copy in another format (its ring the held "
                         "picture may be taken from)")
    take = block(replay, "bool held_take(VkCommandBuffer cmd, const Texture* now) {")
    need("vk_replay.cpp held_take", take, ["last_shown.tex == now", "texture_destroyed_since(last_shown.id, last_shown.gone)",
                                           "vkCmdCopyImage(cmd, from, VK_IMAGE_LAYOUT_GENERAL, held_pic.image"])
    # (the menu, the counters and the notices: drawn by present_image over whatever it shows, the held picture too)
    shown = block(replay, "void present_image(VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImageView view) {")
    need("vk_replay.cpp present_image", shown, ["menu_tick();", "menu_record(f.cmd, index", "dlssg::frame(f.cmd"])
    tick = block(replay, "void status_tick() {")
    need("vk_replay.cpp status_tick", tick, ["f.new_later = new_later - s_new_later, f.held = hold_now.held",
                                             "sw::describe_holds(hs, hold_bound_ns() / 1e6"])

    fg = (src / "gpu/vk/vk_framegen.cpp").read_text(encoding="utf-8")
    display = block(fg, "void display_main() {")
    need("vk_framegen.cpp display_main (the held picture, after the frames before it)", display,
         ["if (!j.set) {", "present_image(j.held, j.held_w, j.held_h, j.has_crop ? j.crop : nullptr, j.held_view);"])
    need("vk_framegen.cpp framegen_new_history", block(fg, "void framegen_new_history() {"), ["fg.have_prev = false;", "fg.last_at = 0;"])
    record = block(fg, "bool framegen_record(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4]) {")
    # (no frame made without the frame before: blend comes from have_prev, and every maker asks it)
    need("vk_framegen.cpp framegen_record", record, [
        "const bool blend = fg.have_prev && !gap;",
        "if (blend) {\n            n = auto_slots(",
        "fsr4_frame(cmd, *set, cur, uw, uh, sequence, blend);",
        "!none && blend && m == 2 && fsr3fi::frames_wanted()",
        "if (set->made == 0 && !none && blend && m > 1) {"])
    need("vk_framegen.cpp fsr4_frame", block(fg, "framegen_rows::Made fsr4_frame("), ["in.reset = f.reset || !blend;"])

    dlssg = (src / "gpu/vk/vk_dlssg.cpp").read_text(encoding="utf-8")
    sl_frame = block(dlssg, "void frame(VkCommandBuffer cmd, uint32_t out_width, uint32_t out_height) {\n    if (!active()) return;")
    need("vk_dlssg.cpp frame", sl_frame, ["const bool held = present_hold_now();",
                                          "fsr3_inputs::take(in) && in.depth_image && in.motion_image && !held;",
                                          "if (held || present_after_hold()) after_hold = true;",
                                          "c.reset = in.reset || after_hold ?"])
    temporal = (src / "gpu/vk/vk_temporal.cpp").read_text(encoding="utf-8")
    # ((Triforce) a camera cut starts one too: tests/test_fsr3_history.py)
    need("vk_temporal.cpp", temporal, ["const bool reset = !last || now - last > 2 || present_held_last() || cut;"])
    # ((Triforce) AMD FSR 3.1's own inputs as well: frames drawn during a hold are not carried into its history)
    inputs = (src / "gpu/vk/vk_fsr3_inputs.cpp").read_text(encoding="utf-8")
    need("vk_fsr3_inputs.cpp prepare", block(inputs, "bool prepare(VkCommandBuffer cmd, Texture* in, VkImageView in_view, Frame& out, bool linear) {"),
         ["const bool held = !gap && present_held_last();", "const bool reset = gap || held || cut;"])

    dev = (src / "docs/development.md").read_text(encoding="utf-8")
    for switch in ("RX_VK_HOLD_MS", "RX_VK_UPLOAD_NEW_MS", "RX_VK_UPLOAD_NEW_MB"):
        if "`" + switch not in dev:
            raise SystemExit("docs/development.md: " + switch + " is not in the switches table")
    return tex


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    tex = check_source(a.source)
    cut = "\n".join([block(tex, "bool new_texture_over_budget() {"), block(tex, "bool new_texture_later(const Texture&) {"),
                     block(tex, "bool new_texture_waits() {"), block(tex, "bool upload_defer(const Texture& tex) {")])
    fixture = (ROOT / "tests/present_hold_budget_test.cpp").read_text(encoding="utf-8")
    assert fixture.count("/* CUT */\n") == 1
    fixture = fixture.replace("/* CUT */\n", cut + "\n")
    env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_UPLOAD_")}
    with tempfile.TemporaryDirectory(prefix="present-hold-") as temp:
        flags = ["-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-I" + str(a.source / "gpu/vk")]
        rule = Path(temp) / "rule.exe"
        subprocess.run([a.cxx, *flags, str(ROOT / "tests/present_hold_test.cpp"), "-o", str(rule)], check=True)
        for case in RULE_CASES:
            subprocess.run([str(rule), case], check=True, timeout=20)
        cpp, budget = Path(temp) / "budget.cpp", Path(temp) / "budget.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, *flags, str(cpp), "-o", str(budget)], check=True)
        for case in BUDGET_CASES:
            subprocess.run([str(budget), case], check=True, timeout=20, env=env)
    print(f"{len(RULE_CASES)} cases of the hold's rule and {len(BUDGET_CASES)} of the new textures' budget passed")


if __name__ == "__main__":
    main()
