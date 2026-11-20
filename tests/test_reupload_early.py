"""The re-uploads made before a parallel replay's segments (gpu/vk/reupload_early.h; vk_texture.cpp texture_reupload_early,
called by vk_replay.cpp replay_handle after the dry pass). No game, no GPU.

After some tours of the lab's scenario S2 the game kept rewriting, from the CPU, the storage of textures the Market samples:
4 to 12 re-uploads of 28 to 84 MB in ~13% of the frames, each in a segment's texture_touch under the recording lock, the other
recording threads queued behind it. Now the textures the CPU keeps rewriting are looked at once on the replay thread before
the segments begin, their uploads in the recording before them.

1. The header and vk_texture.cpp's early looks, cut from the source, compiled and run around stand-ins
   (tests/reupload_early_test.cpp): "list" (the hot list's and wanted()'s rules), "picks" (the hot textures the rule wants
   touched, owed first, with a draw's leave to defer and only then, their age given back; the GPU's, finalized, unsampled,
   small, once-only and only-cleared ones left to the segments), "gone" (a texture gone since it was listed dropped unread),
   "busy" (nothing while the segments record), "off" (RX_VK_REUPLOAD_EARLY=0: nothing listed or looked at).
2. The source read: replay_handle looks after the dry pass and the one-segment case, before the mirrors' syncs and the
   submit of the recording before the segments, and before the segments begin (par_active); texture_touch notes a re-upload
   from the CPU's writes only (not a GPU buffer's write, not an upload over what the GPU drew); the switch is documented.

    python -I -B tests/test_reupload_early.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
CASES = ("list", "picks", "gone", "busy")


def check(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def block(text, head):
    start = text.find(head)
    check(start >= 0, "%s is missing" % head)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def in_order(text, steps, what):
    at = 0
    for s in steps:
        i = text.find(s, at)
        check(i >= 0, "%s: %r is missing or out of order" % (what, s))
        at = i + len(s)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")

    handle = block(replay, "const uint8_t* replay_handle(")
    in_order(handle, ["rmode = Mode::Dry;", "replay(none, state, h);", "rmode = Mode::Serial;",
                      "if (seg.size() == 1 || (tail && seg.size() == 2)) {", "Recording& r = *rec_cur;",
                      "texture_reupload_early();", "memory_sync_mirrors_before_segments();", "submit_recording(r, false, 0, true);",
                      "par_active = true;"], "replay_handle")
    check(handle.count("texture_reupload_early();") == 1, "replay_handle looks early more than once")

    touch = block(tex, "Texture* texture_touch(Texture* tex, bool for_sampling) {")
    done = touch[touch.index("    if (up_ok) {"):touch.index("    } else if (tex->bc) {")]
    check("if (tex->uploaded && !gpu_dirty && !tex->gpu_written && !tex->sparse && tex->guest_size >= reupload_early::kMinBytes)\n"
          "            reupload_note(*tex);" in done,
          "texture_touch notes other uploads than the CPU's re-writes of a texture held before")
    check(done.index("reupload_note(*tex);") < done.index("tex->gpu_written = false;"),
          "texture_touch notes the re-upload after it has forgotten whether the GPU drew the image")
    # ((Triforce) and a re-upload of the CPU's writes the same-bytes check found nothing to upload in (upload_same.h): still one the
    # CPU keeps rewriting. That branch is a partial re-upload's only: uploaded before, not the GPU's, not finalized)
    same = touch[touch.index("    if (same == 1) {"):]
    same = same[:same.index("\n    }\n")]
    check("if (tex->guest_size >= reupload_early::kMinBytes) reupload_note(*tex);" in same and
          "    const int same = partial ? same_look(*tex, dirty) : 0;\n    if (same == 1) {" in touch,
          "texture_touch notes a re-upload the same-bytes check left out otherwise than a CPU re-upload of a texture held before")
    check(touch.count("reupload_note(") == 2, "texture_touch notes re-uploads in more than those two places")

    early = block(tex, "void texture_reupload_early() {")
    check("par_active.load(std::memory_order_relaxed)) return;" in early, "texture_reupload_early runs while segments record")
    on = block(tex, "bool reupload_early_on() {")
    check('getenv("RX_VK_REUPLOAD_EARLY")' in on and 'atoi(getenv("RX_VK_REUPLOAD_EARLY")) != 0' in on,
          "reupload_early_on no longer reads RX_VK_REUPLOAD_EARLY (on unless 0)")
    guide = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    check("`RX_VK_REUPLOAD_EARLY=0`" in guide, "docs/development.md does not say what RX_VK_REUPLOAD_EARLY=0 does")

    start = tex.index("// ---- (Triforce, 11-16) the re-uploads made before a parallel replay's segments (reupload_early.h) ----")
    end = tex.index("Texture* texture_touch(Texture* tex, bool for_sampling) {\n", start)
    cut = tex[start:end]
    fixture = (ROOT / "tests" / "reupload_early_test.cpp").read_text(encoding="utf-8")
    check(fixture.count("/* CUT */\n") == 1, "the fixture's cut marker")
    fixture = fixture.replace("/* CUT */\n", cut)
    with tempfile.TemporaryDirectory(prefix="reupload-early-") as tmp:
        cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        built = subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(VK), str(cpp), "-o", str(exe)],
                               capture_output=True, text=True)
        check(built.returncode == 0, "the fixture did not build:\n" + built.stderr[-4000:])
        env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_")}
        for case in CASES:
            r = subprocess.run([str(exe), case], env=env, capture_output=True, text=True, timeout=60)
            print((r.stdout + r.stderr).strip())
            check(r.returncode == 0, "reupload_early_test %s" % case)
        r = subprocess.run([str(exe), "off"], env=dict(env, RX_VK_REUPLOAD_EARLY="0"), capture_output=True, text=True, timeout=60)
        print((r.stdout + r.stderr).strip())
        check(r.returncode == 0, "reupload_early_test off")
    print("PASS: the textures the CPU keeps rewriting are looked at before the segments, the rule's own ones only, none gone, "
          "never while the segments record; RX_VK_REUPLOAD_EARLY=0 as before")


if __name__ == "__main__":
    main()
