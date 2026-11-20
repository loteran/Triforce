"""A texture whose upload found no staging memory is not marked uploaded: the upload stays owed and is tried again.

A texture is uploaded again when its storage changed since its last upload: page write generations past the texture's
(upload_gen), virtual pages mapped anew, a GPU buffer's write. Each is noted as seen when asked, before the upload. When
the upload then found no staging memory (vk_texture.cpp staging: "no 64 MB arena could be made", a card or a PC short of
memory), texture_touch marked the texture uploaded at that generation all the same, so that a failing upload is not made
at every draw: its image held nothing, or what it held before, until the game wrote other bytes into its memory. A page
written again with the bytes it had is no write to a texture (RX_VK_TEX_SAME), so that could be never: wrong or missing
textures until the area was left or the game started again. NX2navi had the same with its content hash (its 054bd556).

What must hold now:
- gpu/vk/upload_owed.h (compiled and run here, no game or GPU): what a failed upload leaves owed, its wait and the ranges;
- texture_touch's own two parts that decide it, cut from the source and run around an upload that succeeds, finds no
  memory or is of an image no upload fills (tests/upload_owed_test.cpp): the generation and the "seen" notes are kept, the
  next try comes after the wait and brings what the failed one was to bring, and an image no upload fills is marked
  uploaded as before;
- (Triforce, the hold of the last picture) a new texture met after the frame's uploads took their budget: its first upload owed
  to the next frame and the draw given the dummy (null), the dummy again for the frame's later draws, uploaded at the next
  frame, at most kLaterMax frames in a row; uploaded now for a look that may not leave it (a dispatch, the hold's bound), and
  never left when its upload found no memory;
- upload() says when it was the staging memory, the texture the GPU drew into owes nothing, and the CPU's writes over a
  texture the GPU cleared keep their generation too.

python -I -B tests/test_upload_owed.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

CASES = ["rules", "new-texture", "written-part", "remapped-part", "owed-alone", "gpu-write", "no-upload-fills-it", "bc3",
         "from-memory-note", "later-rules", "again-rules", "left-for-later", "left-for-later-gpu-write", "first-rules", "new-left-for-later",
         "new-left-dispatch", "new-left-starved", "split-goes-on"]


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


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    tex = (a.source / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    header = (a.source / "gpu/vk/vk_texture.h").read_text(encoding="utf-8")
    need("vk_texture.h", header, ['#include "upload_owed.h"', "upload_owed::Owed owed;"])

    # (the upload says when it was the staging memory, and only then)
    need("vk_texture.cpp: upload()", block(tex, "bool upload(Texture& tex, const Ranges* dirty) {"),
         ["upload_starved = false;\n    const bool ok = upload_inner(tex, dirty);"])
    inner = block(tex, "bool upload_inner(Texture& tex, const Ranges* dirty) {")
    need("vk_texture.cpp: upload_inner()", inner, ["if (!st.buffer) return upload_starved = true, false;"])
    if inner.count("upload_starved") != 1 or "const Staging st = staging(" not in inner:
        raise SystemExit("vk_texture.cpp: upload_inner() calls another failure than the staging's one of no memory")

    # ((Triforce, 11-16) an upload left for later by the budget: at most RX_VK_UPLOAD_AGAIN_FRAMES frames in a row, 2 unless set)
    again = block(tex, "bool upload_again_later(const Texture& tex) {")
    need("vk_texture.cpp: upload_again_later()", again, ["!upload_owed::may_wait_again(tex.owed, upload_again_frames())"])
    need("vk_texture.cpp: upload_again_frames()", block(tex, "uint32_t upload_again_frames() {"),
         ['getenv("RX_VK_UPLOAD_AGAIN_FRAMES")', ": 2u;"])
    touch = block(tex, "Texture* texture_touch(Texture* tex, bool for_sampling) {")
    # (the wait comes before the questions that note what they saw, and after the look that a submission makes once)
    wait = "    if (upload_owed::waits(tex->owed, frame_no)) {\n"
    order = [touch.index("    tex->checked_epoch = epoch;\n    // (Triforce) An upload that found no memory is owed"), touch.index(wait),
             touch.index("const bool gpu_dirty = tex->uploaded && gpu_wrote_into(*tex);"),
             touch.index("!texture_dirty(*tex, since, dirty, !tex->gpu_written) && !owed) return tex;")]
    if order != sorted(order):
        raise SystemExit("vk_texture.cpp: texture_touch() asks what changed before the owed upload's wait")
    # (the upload that happened pays, in the branch the fixture stands in for)
    done = touch[touch.index("    if (up_ok) {"):touch.index("    } else if (tex->bc) {")]
    need("vk_texture.cpp: texture_touch(), the upload that happened", done,
         ["tex->uploaded = true;\n        tex->upload_gen = gen;", "upload_owed::paid(tex->owed);"])
    need("vk_texture.cpp: texture_touch(), the upload itself", touch,
         ["const bool up_ok = upload(*tex, split ? &split_part : partial || sparse_written ? &dirty : nullptr);",
          "            from_memory = true;"])
    # ((Triforce) a new layered texture's first upload in parts, layer_split.h: a part that is not the last owes the rest and
    # gives the draw the dummy, before the branch that marks the texture uploaded; the last keeps the first part's generation)
    split_at = touch.index("    if (split && up_ok && !split_rest.empty()) {")
    if not (touch.index("const bool split = !partial && !sparse_written && split_take(*tex, split_part, split_rest);") < split_at <
            touch.index("    if (up_ok) {")):
        raise SystemExit("vk_texture.cpp: texture_touch() does not take a part of a layered texture's first upload before the upload")
    need("vk_texture.cpp: texture_touch(), a part that is not the last", touch[split_at:split_at + 1800],
         ["upload_owed::later(tex->owed, &split_rest, frame_no);", "tex->upload_gen = split_from;", "return nullptr;"])
    need("vk_texture.cpp: texture_touch(), the last part", done, ["if (split && split_going) tex->upload_gen = split_from;"])
    # (the CPU's writes over a texture the GPU only cleared: the generation stays when their upload found no memory)
    over = touch[touch.index("        if (!inner.empty() && upload(*tex, &inner)) {"):touch.index("    {   // (RX_VK_REUPLOAD_DIM")]
    if over.index("} else if (!inner.empty() && upload_starved) {") > over.index("tex->upload_gen = gen;") or \
            "            return tex;\n        }\n        tex->upload_gen = gen;" not in over:
        raise SystemExit("vk_texture.cpp: texture_touch() moves the generation of CPU writes whose upload found no memory")
    # (what the GPU drew is the content: nothing an upload owed is brought over it later)
    need("vk_texture.cpp: texture_written()", block(tex, "void texture_written(Texture* tex, const char* why) {"),
         ["if (tex->owed.from) upload_owed::paid(tex->owed);"])
    # (nor over storage the render targets took since the failed try: that branch uploads nothing, and what was owed from
    # before would bring the memory in at the next look after all)
    over_rt = touch[touch.index("    if (owned && !texture_dirty(*tex, owned, dirty)) {"):]
    over_rt = over_rt[:over_rt.index("        return tex;\n    }\n")]
    need("vk_texture.cpp: texture_touch(), storage the GPU rendered", over_rt,
         ["tex->uploaded = true;", "if (tex->owed.from) upload_owed::paid(tex->owed);"])

    # ((Triforce) the hold of the last picture: a texture waiting for its first bytes, left by the budget, is the dummy again
    # for a draw while the hold may leave it, and is uploaded now for anything else; any other wait returns as before)
    need("vk_texture.cpp: texture_touch(), the wait", touch[touch.index(wait):touch.index(wait) + 400],
         ["        if (!upload_owed::first_later(tex->owed, tex->uploaded)) return tex;\n"
          "        if (new_texture_waits()) return nullptr;\n    }"])
    # (and a new texture after the frame's budget: its whole first upload owed to the next frame, the draw given the dummy;
    # asked after the uploads left for later of textures held before, before anything that notes what it saw)
    first = "    if (!split_going && upload_owed::may_leave_first(tex->owed, tex->uploaded) && new_texture_later(*tex)) {"
    if not (touch.index("if (tex->uploaded && upload_again_later(*tex)) {") < touch.index(first) <
            touch.index("\n    const WriteGen gen = memory_generation();")):
        raise SystemExit("vk_texture.cpp: texture_touch() leaves a new texture for later elsewhere than before the upload's notes")
    head = touch[touch.index(wait):]
    head = head[:head.index("    const WriteGen gen = memory_generation();")]
    tail = touch[touch.index("    } else if (tex->bc) {"):]
    for part, needles in [(head, ["const uint64_t wrote_seen = tex->gpu_wrote_seen;", "&& !tex->owed.whole;",
                                  "if (owed && partial) upload_owed::add(dirty, tex->owed.ranges);",
                                  first, "upload_owed::later(tex->owed, nullptr, frame_no);\n        return nullptr;"]),
                          (tail, ["} else if (upload_starved) {", "tex->gpu_wrote_seen = wrote_seen;",
                                  "upload_owed::failed(tex->owed, partial || sparse_written ? &dirty : nullptr, frame_no, tex->bc ? 30 : 0);"])]:
        need("vk_texture.cpp: texture_touch()", part, needles)
    starved = tail[tail.index("} else if (upload_starved) {"):tail.index("    } else {")]
    if "tex->uploaded = true" in starved or "tex->upload_gen" in starved:
        raise SystemExit("vk_texture.cpp: a texture whose upload found no staging memory is marked uploaded")

    fixture = (ROOT / "tests/upload_owed_test.cpp").read_text(encoding="utf-8")
    for marker, text in [("/* HEAD */\n", head), ("/* TAIL */\n", tail + "\n")]:
        assert fixture.count(marker) == 1, marker
        fixture = fixture.replace(marker, text)
    with tempfile.TemporaryDirectory(prefix="upload-owed-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                        "-I" + str(a.source / "gpu/vk"), str(cpp), "-o", str(exe)], check=True)
        for case in CASES:
            subprocess.run([str(exe), case], check=True, timeout=20)
    print(f"{len(CASES)} cases of an upload that did not happen passed")


if __name__ == "__main__":
    main()
