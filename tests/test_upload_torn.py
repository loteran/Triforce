"""A re-upload of texture storage the CPU was rewriting when its submit was captured keeps its last bytes for that frame
(gpu/vk/upload_torn.h; vk_texture.cpp torn_later, asked by upload_again_later; vk_memory.cpp memory_rewritten_since_capture).
No game, no GPU.

test29's tours showed white frames at the Market after a tour (r36-t29-tour, r37-t29-tour: 310 and 332 cell blinks, mean luma
up to 214). Each one the log's "dark frame" lines name is a frame whose replay uploaded a burst of the textures the game
rewrites from the CPU there, the frame after it uploading some of them again; the uploads read the storage as it was at the
submit, and in test29 the CPU was still writing those pages while the replay read them ("texture storage read as submitted":
150,000 to 250,000 pages that differed from their copy at the read, against 0 to 9,415 in test27, test30 and test31). A copy
taken while the game moves or refills a texture holds bytes passing through; BC6H decodes them to bright colours the bloom
spreads over the picture.

1. vk_memory.cpp memory_rewritten_since_capture and vk_texture.cpp's upload_again_later with torn_later, cut from the source,
   compiled with the header beside a model of guest memory, the captures' copies and a texture's image
   (tests/upload_torn_test.cpp): the burst frame of a refill (another picture, or the same bytes as the storm's) is drawn
   with the image's last picture, not the bytes passing through, and the next frame holds the new picture whole; so for a
   texture not page-aligned and for a refill caught in its last two pages; a texture under 1 MB, a dispatch's look, pages no
   capture holds, writes to pages a texture only shares at its edges, one the frame's budget left the frame before: as
   before; one the CPU rewrites at every frame is left once in 40 frames at most; a refill still going at the next frame is
   uploaded then; RX_VK_UPLOAD_TORN=0 makes the wrong frame (the model reproduces it).
2. Each rule of the header and of the cut functions is broken in turn in a copy, and the model must fail.
3. The source read: texture_touch keeps the image's bytes when upload_again_later says so (its upload owed to the next frame),
   upload_again_later asks the rule for a draw's look only and before the budget, texture_frame logs it, the docs say what
   RX_VK_UPLOAD_TORN=0 does.

    python -I -B tests/test_upload_torn.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

MEM_HEAD = "/// (Triforce, wp18-flash) Whether the CPU has rewritten [guest, guest + n)"
MEM_END = "std::atomic<int64_t> st_overlaid{0}, st_differed{0};"
TEX_HEAD = "std::atomic<int64_t> st_again_later{0}, st_again_later_frame{0};"
TEX_END = "int64_t texture_uploads_later() {"

HEADER_MUTATIONS = [
    ("the pages are never compared", "c && std::memcmp(c, live(p), kPage) != 0) return true;",
     "c && std::memcmp(c, live(p), kPage) != 0 && false) return true;"),
    ("only the first page is compared", "        if (p == end - 1) return false;\n", "        return false;\n"),
    ("the last page is not always compared", "        if (p >= end - 1) p = end - 1;   // (the last whole page, always)\n",
     "        if (p >= end) return false;\n"),
    ("a page shared at the start is compared", "*first = (guest + kPage - 1) / kPage;", "*first = guest / kPage;"),
    ("a page shared at the end is compared", "*end = (guest + n) / kPage;", "*end = (guest + n + kPage - 1) / kPage;"),
    ("no frames found so lately", "return !(s.used && s.id == id && frame >= s.frame && frame - s.frame <= kGap);",
     "return !(s.used && s.id == id && frame >= s.frame && frame - s.frame <= kGap) || true;"),
    ("textures under 1 MB too", "inline constexpr uint64_t kMinBytes = 1ull << 20;", "inline constexpr uint64_t kMinBytes = 0;"),
    ("off unless set", "return !env || !*env || std::strtol(env, nullptr, 10) != 0;",
     "return env && *env && std::strtol(env, nullptr, 10) != 0;"),
    ("two frames in a row", "inline constexpr uint32_t kFrames = 1;", "inline constexpr uint32_t kFrames = 2;"),
]

TEX_MUTATIONS = [
    ("the rule is not asked", "    if (torn_later(tex)) return true;   // ((Triforce, wp18-flash) its storage being rewritten: its last bytes this frame)\n", ""),
    ("a dispatch's look is left too", "    if (!upload_defer_ok) return false;\n    if (torn_later(tex)) return true;",
     "    if (torn_later(tex)) return true;\n    if (!upload_defer_ok) return false;"),
    ("a texture found so is not noted", "    torn_recent.seen(tex.id, frame_no);\n", ""),
    ("one the budget left the frame before is left again",
     " || !upload_owed::may_wait_again(tex.owed, upload_torn::kFrames))\n        return false;", ")\n        return false;"),
]

MEM_MUTATIONS = [
    ("no copy is found", "return it != shadow.end() ? it->second.b.get() : nullptr;", "return it == shadow.end() ? nullptr : nullptr;"),
    ("the regions the other way round", "if (!shadow_regions.count((uint32_t)(p >> 9))) return nullptr;",
     "if (shadow_regions.count((uint32_t)(p >> 9))) return nullptr;"),
]


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


def cut(text, head, end, what):
    a = text.find(head)
    check(a >= 0 and text.count(head) == 1, "%s: its start is not once in the source" % what)
    b = text.find(end, a)
    check(b > a, "%s: its end is missing" % what)
    return text[a:b]


def check_source(tex, dev):
    touch = block(tex, "Texture* texture_touch(Texture* tex, bool for_sampling) {")
    i = touch.find("if (tex->uploaded && upload_again_later(*tex)) {")
    check(i >= 0, "texture_touch no longer asks upload_again_later before a re-upload")
    branch = touch[i:touch.index("return tex;", i)]
    check("upload_owed::later(tex->owed, partial ? &dirty : nullptr, frame_no);" in branch,
          "texture_touch no longer keeps the image's bytes and owes the upload when upload_again_later says so")
    again = block(tex, "bool upload_again_later(const Texture& tex) {")
    at = [again.find(s) for s in ("if (!upload_defer_ok) return false;", "if (torn_later(tex)) return true;", "if (!budget_ns ||")]
    check(min(at) >= 0 and at == sorted(at), "upload_again_later asks the rule other than for a draw's look first, before the budget")
    frame = block(tex, "void texture_frame(uint64_t completed) {")
    check("re-uploads of storage the CPU rewrote since its capture" in frame and "st_torn_found_frame.exchange(0" in frame,
          "texture_frame no longer logs the re-uploads found so")
    check("`RX_VK_UPLOAD_TORN=0`" in dev, "docs/development.md does not say what RX_VK_UPLOAD_TORN=0 does")


def build(cxx, tmp, name, header_dir, mem, tex):
    fixture = (ROOT / "tests" / "upload_torn_test.cpp").read_text(encoding="utf-8")
    check(fixture.count("/* CUT MEMORY */\n") == 1 and fixture.count("/* CUT TEXTURE */\n") == 1, "the fixture's cut markers")
    fixture = fixture.replace("/* CUT MEMORY */\n", cut(mem, MEM_HEAD, MEM_END, "memory_rewritten_since_capture"))
    fixture = fixture.replace("/* CUT TEXTURE */\n", cut(tex, TEX_HEAD, TEX_END, "upload_again_later and torn_later"))
    cpp, exe = Path(tmp) / (name + ".cpp"), Path(tmp) / (name + ".exe")
    cpp.write_text(fixture, encoding="utf-8")
    built = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(header_dir), "-I", str(VK), str(cpp),
                            "-o", str(exe)], capture_output=True, text=True)
    return exe if built.returncode == 0 else None, built.stdout + built.stderr


def run(exe, *args, env_extra=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_")}
    env.update(env_extra or {})
    r = subprocess.run([str(exe)] + list(args), env=env, capture_output=True, text=True, timeout=120)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    mem = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    header = (VK / "upload_torn.h").read_text(encoding="utf-8")
    dev = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    check_source(tex, dev)
    with tempfile.TemporaryDirectory(prefix="upload-torn-") as tmp:
        exe, out = build(args.cxx, tmp, "fixture", VK, mem, tex)
        check(exe is not None, "the fixture did not build:\n" + out[-4000:])
        ok, out = run(exe)
        print(out)
        check(ok, "upload_torn_test")
        ok, out = run(exe, "off", env_extra={"RX_VK_UPLOAD_TORN": "0"})
        print(out)
        check(ok, "upload_torn_test off")
        n = 0
        for what, old, new in HEADER_MUTATIONS:
            check(header.count(old) == 1, "header mutation %r: its text is not once in upload_torn.h" % what)
            inc = Path(tmp) / ("h%d" % n)
            inc.mkdir()
            (inc / "upload_torn.h").write_text(header.replace(old, new), encoding="utf-8")
            exe, out = build(args.cxx, tmp, "h%d" % n, inc, mem, tex)
            n += 1
            check(exe is not None, "the mutation %r does not build:\n%s" % (what, out[-2000:]))
            if run(exe)[0]:
                raise SystemExit("FAIL: the model passes when %s" % what)
            print("PASS: the model fails when %s" % what)
        for what, old, new in TEX_MUTATIONS:
            check(tex.count(old) == 1, "source mutation %r: its text is not once in vk_texture.cpp" % what)
            exe, out = build(args.cxx, tmp, "t%d" % n, VK, mem, tex.replace(old, new))
            n += 1
            check(exe is not None, "the mutation %r does not build:\n%s" % (what, out[-2000:]))
            if run(exe)[0]:
                raise SystemExit("FAIL: the model passes when %s" % what)
            print("PASS: the model fails when %s" % what)
        for what, old, new in MEM_MUTATIONS:
            check(mem.count(old) == 1, "source mutation %r: its text is not once in vk_memory.cpp" % what)
            exe, out = build(args.cxx, tmp, "m%d" % n, VK, mem.replace(old, new), tex)
            n += 1
            check(exe is not None, "the mutation %r does not build:\n%s" % (what, out[-2000:]))
            if run(exe)[0]:
                raise SystemExit("FAIL: the model passes when %s" % what)
            print("PASS: the model fails when %s" % what)
    print("PASS: a re-upload of storage the CPU was rewriting at its submit keeps the image's last picture for that frame, the "
          "next frame's whole; the rest as before; RX_VK_UPLOAD_TORN=0 as before")


if __name__ == "__main__":
    main()
