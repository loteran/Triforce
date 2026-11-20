"""A re-upload checked against the bytes its image holds (gpu/vk/upload_same.h; vk_texture.cpp texture_touch, same_look and
same_done; vk_memory.cpp memory_visit_as_submitted). No game, no GPU.

After some tours of the lab's scenario S2 the game rewrote, from the CPU, a few big BC textures the Market samples with the bytes
they had, 40 to 60 MB every 4 to 5 frames (the Market at 62 to 78 fps against 83 to 96, ~26 GB of uploads in its stay). Now a
re-upload hashes the units of the storage the writes reached (a level's and layer's GOB-block rows, as the upload reads them) and
uploads only those whose bytes are not the image's; the upload reads the copy that was hashed, so the hashes kept are of what
the image got, and anything else that changes the image forgets them.

1. The header and the two cut pieces (vk_memory.cpp's visit of guest memory as submitted, vk_texture.cpp's check from
   upload_same_on to same_done) compiled and run around stand-ins (tests/upload_same_test.cpp): the hash and the units, the
   check's rules (what is looked at, what is uploaded, what is forgotten), the rest after misses, the copy's reads, the visit's
   parts and its kept copies, and texture_touch's flow over a model texture: the same bytes again (nothing uploaded), a row
   changed (that row alone), A then B then A, a write while an upload is under way, the GPU's writes and unchecked uploads, an
   upload that read past the copy, guest memory against the capture's kept copies, the textures not checked, RX_VK_UPLOAD_SAME=0,
   and a random mix of all of it with the image checked against what the game had at the submit after every look.
2. The source read: texture_touch looks after the waits and the budget's decisions and before the diagnostics and the upload, on
   a partial re-upload only, gives a check that found nothing to upload the upload's bookkeeping and no upload, and ends the
   checked upload right after it; upload() counts every upload as a change to the image and texture_written every GPU write;
   upload_inner reads the copy in its CPU path's two reads and gathers nothing past it; same_units goes by the same rows as
   upload_inner; the switch is documented.
3. (Triforce, 11-16) ASTC decoded on the GPU (upload_astc_gpu), which the game rewrote with the same bytes at the S2 Market after a
   tour (~1.4 GB in 10 s staged and decoded again): checked too, 2D only, with one unit a layer from its first kept level on, the
   same expressions as upload_astc_gpu's; its two reads of a layer read the copy, guest memory past it; the 8 MB rule not for it;
   RX_VK_UPLOAD_SAME_ASTC=0 as before. The fixture's "astc" case models upload_astc_gpu's staging (a layer changed staged alone,
   the same bytes nothing, A B A, a write before tail0, a single layer, a BC1 image read again whole), "astc_off" the switch.
4. (Triforce, 11-16) The parallel replay's twin (an earlier segment's upload again of what a later segment uploaded in the same
   submission), which uploaded the whole storage and forgot the kept hashes, so that the S2 Market's rewrites of the same bytes
   were uploaded again at every one (round46: ~97% of what was still uploaded unchecked in those loops). Now it uploads what that
   submission's uploads asked for (upload() notes it; the whole storage when one was whole or the image was made again), from a
   copy that keeps the hashes when it holds their bytes. The fixture models two segments of a submission, each its own uploads
   applied in queue order at the submit: "twin_rules" the header's rules, "twin" the loop (one twin, then found the same) and
   each case (a row, a write between the later look and the twin, the image made again by the upload or after it, a whole upload,
   one not checked, the earlier worker first, and hashes kept in an earlier submission that an unchecked upload or a GPU write
   in this one made not the image's: the twin without a copy), "twin_random" a mix, the earlier segment's draws seeing the bytes as submitted and
   the kept hashes being the image's throughout; "twin_off" and "twin_random" with RX_VK_TWIN_SAME=0 the twin as before. The
   source read: texture_touch's twin and upload()'s note as the fixture has them.
5. (Triforce, 11-17) The source read of the upload into the earliest segment still recorded (RX_VK_UPLOAD_EARLIEST; its queue
   order is tests/test_parallel_init_order.py's): only texture_touch's main upload, not for a texture the GPU drew or a sparse
   one, let go right after the upload, the segment it went into noted at both of the main path's notes; the CPU-over-GPU
   branch, the twin and texture_bc1_to_bc3 still the recording segment's own; the switch documented.

    python -I -B tests/test_upload_same.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
CASES = ("hash", "units", "rules", "misses", "copy", "visit", "kept", "skip", "aba", "forget", "submitted", "unwanted", "rest",
         "astc", "random", "twin_rules", "twin", "twin_random")


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
    mem = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    header = (VK / "vk_texture.h").read_text(encoding="utf-8")

    check('#include "upload_same.h"' in header and "upload_same::State same;" in header, "vk_texture.h: Texture has no upload_same::State")
    touch = block(tex, "Texture* texture_touch(Texture* tex, bool for_sampling) {")
    # (after the budget's decisions and the questions that note what they saw, before the diagnostics and the upload)
    in_order(touch, ["if (tex->uploaded && upload_again_later(*tex)) {", "\n    const WriteGen gen = memory_generation();",
                     "if (tex->bc && !tex->uploaded && tex->bc_retry_frame > frame_no) return tex;",
                     "    const int same = partial ? same_look(*tex, dirty) : 0;\n    if (same == 1) {",
                     "static const bool stamp_src = getenv(\"RX_VK_STAMP_SRC\") != nullptr;",
                     "const bool up_ok = upload(*tex, split ? &split_part : partial || sparse_written ? &dirty : nullptr);\n",
                     "    if (same == 2) same_done(*tex, up_ok);"], "texture_touch")
    check(touch.count("same_look(") == 1 and touch.count("same_done(") == 1, "texture_touch checks a re-upload in more than one place")
    nothing = touch[touch.index("    if (same == 1) {"):]
    nothing = nothing[:nothing.index("\n    }\n") + 6]
    for needle in ["tex->upload_gen = gen;", "upload_owed::paid(tex->owed);", "memory_note_texture_upload(tex->gpu, tex->guest_size);",
                   "reupload_note(*tex);", "return tex;"]:
        check(needle in nothing, "texture_touch: a check that found nothing to upload no longer does " + needle)
    check("upload(" not in nothing.replace("memory_note_texture_upload(", "").replace("reupload_note(", ""),
          "texture_touch uploads after a check that found nothing to upload")
    check("const bool partial = !no_partial && !gpu_dirty && tex->uploaded && !tex->gpu_written && !tex->released && "
          "!tex->owed.whole;" in touch, "texture_touch's partial re-upload is not what the check is made for")
    up = block(tex, "bool upload(Texture& tex, const Ranges* dirty) {")
    in_order(up, ["upload_same::changed(tex.same);", "const bool ok = upload_inner(tex, dirty);"], "upload()")
    written = block(tex, "void texture_written(Texture* tex, const char* why) {")
    check("upload_same::changed(tex->same);" in written, "texture_written: a GPU write does not forget a re-upload's hashes")
    inner = block(tex, "bool upload_inner(Texture& tex, const Ranges* dirty) {")
    in_order(inner, ["const uint8_t* src = same_snap_read(tex, src_off, len);", "if (!src) src = guest_bytes(tex.gpu + src_off, len, scratch);",
                     "const uint8_t* snap = from < total ? same_snap_read(tex, from, std::min<uint64_t>(need, total - from)) : nullptr;",
                     "const bool gather_parts = !snap && nthreads > 1", "} else if (snap && total - from >= need) {\n                        src = snap;",
                     "if (snap) std::memcpy(scratch.data(), snap, total - from);"], "upload_inner")
    check(inner.count("same_snap_read(") == 2, "upload_inner reads the check's copy elsewhere than its CPU path's two reads")
    # (the units are upload_inner's rows: the same condition for going by rows, the same row size)
    units = block(tex, "void same_units(const Texture& tex, std::vector<upload_same::Unit>& u) {")
    check("if (!dirty || linear || tex.astc || widen_from || g.td > 1 || g.tile_d) {" in inner and
          "const bool rows = !(linear || tex.astc || widen_from || g.td > 1 || g.tile_d);" in units,
          "same_units goes by rows where upload_inner does not, or the other way")
    check("const uint64_t row_bytes = gobs_x * (512ull << (g.tile_h + g.tile_d));" in inner and
          "rows ? gobs_x * (512ull << g.tile_h) : span" in units, "same_units' rows are not upload_inner's")
    for needle in ["const uint64_t src_off = g.offset + (uint64_t)layer * tex.L.layer_size;", "if (src_off >= total) continue;",
                   "const uint64_t span = linear ? (uint64_t)stride * g.th : std::max<uint64_t>(g.swizzled, 1);",
                   "const uint32_t copy_layers = tex.type == VK_IMAGE_TYPE_3D ? 1 : tex.layers;"]:
        check(needle in inner and needle in units, "same_units and upload_inner differ in " + needle)
    wanted = block(tex, "bool same_wanted(const Texture& tex, int* why = nullptr) {")
    for needle in ["tex.astc", "tex.bc", "tex.sparse", "upload_same::kMaxBytes", "!is_bc(tex.format) && !is_astc(tex.format) && tex.guest_size >= (8u << 20)"]:
        check(needle in wanted, "same_wanted no longer leaves out " + needle)
    # ((Triforce, 11-16) ASTC decoded on the GPU: 2D only (upload_astc_gpu's), behind its switch; a BC3 image not from ASTC still
    # left out; the 8 MB rule (for what the GPU's unswizzle and the all-zero clear take) not for decoded ASTC, whose format is RGBA8)
    check("if (tex.astc && tex.type == VK_IMAGE_TYPE_2D && !upload_same_astc_on())" in wanted and
          "(tex.astc && tex.type != VK_IMAGE_TYPE_2D) || (tex.bc && !tex.astc) || tex.sparse ||" in wanted,
          "same_wanted takes ASTC other than of a 2D image, or not behind RX_VK_UPLOAD_SAME_ASTC, or a BC3 image not from ASTC")
    check("(!tex.astc && !is_bc(tex.format) && !is_astc(tex.format) && tex.guest_size >= (8u << 20))" in wanted,
          "same_wanted's 8 MB rule refuses decoded ASTC (RGBA8) or no longer refuses the uncompressed")
    check("if (tex.astc && (up_route = UP_ASTC, upload_astc_gpu(tex, total, dirty))) return true;" in inner,
          "upload_inner no longer sends ASTC to upload_astc_gpu first")
    astc_on = block(tex, "bool upload_same_astc_on() {")
    check('getenv("RX_VK_UPLOAD_SAME_ASTC")' in astc_on and 'atoi(getenv("RX_VK_UPLOAD_SAME_ASTC")) != 0' in astc_on,
          "upload_same_astc_on no longer reads RX_VK_UPLOAD_SAME_ASTC")
    # (upload_astc_gpu's two reads of a layer, the BC1 image's own copy and the staging, read the check's copy where it is under
    # way, guest memory (as submitted) past it; nothing else of the storage read there)
    gpu_astc = block(tex, "bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {")
    decl = "const uint8_t* same_snap_read(const Texture& tex, uint64_t off, uint64_t n);"
    check(decl in tex and tex.index(decl) < tex.index("bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {"),
          "same_snap_read is not declared before upload_astc_gpu")
    def read(pad, dst):   # (the copy's bytes, else guest memory's as before: tests/test_tex_bc1.py reads the BC1 one's line)
        return (pad + "if (const uint8_t* src = same_snap_read(tex, at, n))   // ((Triforce) a checked re-upload's copy: same_look)\n" +
                pad + "    std::memcpy(%s, src, n);\n" % dst + pad + "else\n" +
                pad + "    std::memcpy(%s, guest_bytes(tex.gpu + at, n, scratch), n);\n" % dst)
    check(gpu_astc.count("same_snap_read(") == 2 and gpu_astc.count("same_snap_read(tex, at, n))") == 2,
          "upload_astc_gpu does not read the check's copy in exactly its two reads of a layer")
    check(read(" " * 16, "own.data() + k * tail") in gpu_astc,
          "upload_astc_gpu's read into a BC1 image's own copy is not the check's copy, else guest memory")
    check(read(" " * 8, "s.data + k * tail") in gpu_astc,
          "upload_astc_gpu's staging read is not the check's copy, else guest memory")
    check(gpu_astc.count("guest_bytes(") == 2, "upload_astc_gpu reads the storage elsewhere than its two reads")
    # (same_units' ASTC units are what upload_astc_gpu stages: the same layer size, first kept level, layout check and layer start)
    for needle in ["const uint64_t layer_size = tex.layers > 1 ? tex.L.layer_size : std::max<uint64_t>(tex.L.layer_size, total);",
                   "const uint64_t tail0 = level_geo(tex, 0).offset;",
                   "if (!layer_size || tail0 >= layer_size || (tail0 & 15) || (layer_size & 15))",
                   "for (uint32_t layer = 0; layer < tex.layers; layer++)",
                   "if (at >= total) continue;"]:
        check(needle in gpu_astc and needle in units, "same_units' ASTC units and upload_astc_gpu differ in " + needle)
    check("overlaps(dirty, (uint64_t)layer * layer_size + tail0, (uint64_t)(layer + 1) * layer_size)" in gpu_astc and
          gpu_astc.count("const uint64_t at = (uint64_t)layers[k] * layer_size + tail0;") == 2 and
          "const uint64_t at = (uint64_t)layer * layer_size + tail0;" in units,
          "same_units' ASTC unit does not begin where upload_astc_gpu's layer does")
    check("upload_same::add_units(u, at, layer_size - tail0, layer_size - tail0, total);" in units and
          "const uint64_t tail = layer_size - tail0;" in gpu_astc and "const uint64_t n = std::min(tail, total - at);" in gpu_astc,
          "same_units' ASTC unit is not the layer's tail upload_astc_gpu stages")
    check(units.index("if (tex.astc) {") < units.index("const bool linear"), "same_units' ASTC units are not made apart")
    # (the line of re-uploads not checked: texture_touch's three ways of uploading again)
    check(touch.count("same_unchecked(") == 3 and "same_unchecked(SAME_TWIN, " in touch and "same_unchecked(SAME_CPU_OVER_GPU, " in touch,
          "texture_touch does not count the re-uploads it makes unchecked")
    check("`RX_VK_UPLOAD_SAME_ASTC=0`" in (ROOT / "docs" / "development.md").read_text(encoding="utf-8"),
          "docs/development.md does not say what RX_VK_UPLOAD_SAME_ASTC=0 does")
    # (the GPU's unswizzle and the all-zero clear, which do not read guest_bytes, are for nothing same_wanted lets through)
    check("if (total < (8u << 20) || tex.block_w != 1 || tex.block_h != 1) return false;" in block(tex, "bool unswizzle_gpu_takes(") and
          "is_bc(tex.format) || is_astc(tex.format)" in block(tex, "bool unswizzle_gpu_takes(") and
          "if (!unswizzle_gpu_takes(tex, total, vf)) return false;" in block(tex, "bool upload_unswizzle_gpu("),
          "upload_unswizzle_gpu takes textures same_wanted lets through")
    check("total < (32ull << 20)" in block(tex, "bool all_zero_takes(") and
          "if (!all_zero_takes(tex, total, dirty, widened_format)) return false;" in block(tex, "bool upload_all_zero("),
          "upload_all_zero takes textures same_wanted lets through")
    # ((Triforce, 11-16) the parallel replay's twin: what this submission's uploads asked for (noted by upload() around
    # upload_inner, with the image before and after it), from a copy that keeps the hashes when it holds their bytes; the
    # fixture's seg_look and seg_upload are these lines)
    twin = touch[touch.index("if (tex->uploaded && tex->upload_value == rec_value && tex->upload_seg > tl_seg) {"):]
    twin = twin[:twin.index("\n    }\n") + 6]
    in_order(twin, ["const Ranges* noted = twin_same_on() ? upload_same::twin_ranges(tex->same.seg, rec_value, same_image_id(*tex)) : nullptr;",
                    "if (noted) twin_part = *noted;", "const Ranges* part = noted ? &twin_part : nullptr;",
                    "const bool looked = part && same_twin_look(*tex, *part);", "const bool up_ok = upload(*tex, part);",
                    "same_twin_done(*tex, part, looked, up_ok);", "if (up_ok) same_unchecked(SAME_TWIN, up_route, part, tex->guest_size);",
                    "tex->upload_seg = tl_seg;", "return tex;"], "texture_touch's twin")
    check(twin.count("upload(") - twin.count("_upload(") == 1, "texture_touch's twin uploads elsewhere than with what was noted")
    in_order(up, ["upload_same::changed(tex.same);", "const uint64_t image0 = same_image_id(tex);",
                  "const bool ok = upload_inner(tex, dirty);",
                  "if (twin_same_on()) upload_same::note_upload(tex.same.seg, rec_value, image0, same_image_id(tex), dirty);"],
             "upload()'s note for the twin")
    check(touch.index("if (tex->uploaded && tex->upload_value == rec_value && tex->upload_seg > tl_seg) {") <
          touch.index("if (tex->uploaded && tex->checked_epoch == epoch) return tex;"),
          "texture_touch looks at checked_epoch before the twin: an earlier segment would sample what a later one uploads after it")
    # ((Triforce, 11-17) the main path's upload into the earliest segment still recorded (upload_earliest.h, UploadRedirect; its
    # queue order in tests/test_parallel_init_order.py): only there, not for a texture the GPU drew or a sparse one, the redirect
    # let go right after the upload (before the check's bookkeeping), upload_seg the segment it went into at both of the main
    # path's notes; the CPU-over-GPU branch, the twin and texture_bc1_to_bc3 in the recording segment's own uploads)
    main_up = ("    UploadRedirect early(upload_earliest::wanted(upload_earliest_on(), tex->gpu_written, tex->sparse) && tl_seg_index >= 0 &&\n"
               "                         !upload_reads_live(*tex, split ? &split_part : partial || sparse_written ? &dirty : nullptr));\n"
               "    const bool up_ok = upload(*tex, split ? &split_part : partial || sparse_written ? &dirty : nullptr);\n"
               "    const uint64_t up_seg = early.order;   // (the segment it is in: its own, or the earlier one's)\n"
               "    const bool moved = early.to != nullptr;\n"
               "    early.done();\n"
               "    if (moved && up_ok) upload_earliest_note(*tex, split ? &split_part : partial || sparse_written ? &dirty : nullptr);\n"
               "    if (same == 2) same_done(*tex, up_ok);")
    check(main_up in touch, "texture_touch's main upload is not the one redirected, or the redirect is held past it")
    check(touch.count("UploadRedirect") == 1 and tex.count("UploadRedirect early(") == 1 and "UploadRedirect" not in twin,
          "an upload other than texture_touch's main one is recorded into an earlier segment's init command buffer")
    check(touch.count("tex->upload_seg = up_seg;") == 2 and touch.count("upload_seg = tl_seg;") == 2 and
          touch.index("UploadRedirect early(") < touch.index("tex->upload_seg = up_seg;"),
          "texture_touch's main path does not note the segment its upload went into (the split part's and the whole one's)")
    cpu_over = touch[touch.index("if (tex->gpu_written && !tex->released) {"):touch.index("if (tex->uploaded && tex->upload_value == rec_value")]
    check("upload(*tex, &inner)" in cpu_over and "tex->upload_value = rec_value, tex->upload_seg = tl_seg;" in cpu_over and
          "UploadRedirect" not in cpu_over, "the CPU-over-GPU branch's upload is no longer its segment's own")
    bc13 = block(tex, "bool texture_bc1_to_bc3(Texture* tex, const char* why) {")
    check("tex->upload_value = rec_value, tex->upload_seg = tl_seg;" in bc13 and "UploadRedirect" not in bc13,
          "texture_bc1_to_bc3's upload is no longer its segment's own")
    earliest_on = block(tex, "bool upload_earliest_on() {")
    check('getenv("RX_VK_UPLOAD_EARLIEST")' in earliest_on and 'atoi(getenv("RX_VK_UPLOAD_EARLIEST")) != 0' in earliest_on,
          "upload_earliest_on no longer reads RX_VK_UPLOAD_EARLIEST")
    # ((Triforce, 11-17) only an upload staged from the CPU as it is recorded: upload_reads_live says whether upload_inner may read
    # it on the GPU (the GPU unswizzle) or from live memory (the all-zero clear), by their own checks, and upload_inner takes
    # neither for a redirected one (a GPU write a segment between makes into that memory would run after the read)
    live = block(tex, "bool upload_reads_live(")
    check("unswizzle_gpu_takes(tex, total, vf)" in live and "all_zero_takes(tex, total, dirty, widen_from != 0)" in live and
          "memory_has_shadow(tex.gpu, total)" in live and "if (!tex.gpu || tex.astc || tex.bc) return false;" in live,
          "upload_reads_live does not ask upload_inner's GPU-read ways their own checks")
    inner = block(tex, "bool upload_inner(Texture& tex, const Ranges* dirty) {")
    check("if (!tl_up_redirect && !(tex.gpu < (1ull << 40) && memory_has_shadow(tex.gpu, total)) && upload_unswizzle_gpu(tex, total, dirty))" in inner
          and "if (!tl_up_redirect && upload_all_zero(tex, total, dirty, widen_from != 0)) return true;" in inner and
          inner.count("upload_unswizzle_gpu(") == 1 and inner.count("upload_all_zero(") == 1,
          "upload_inner may read a redirected upload on the GPU")
    # ((Triforce, 11-17) its line every 600 frames from texture_frame, with RX_VK_UPLOAD_EARLIEST on or off, the waits of the
    # segments' ends with it; upload_earliest_note only counts)
    check("upload_earliest_report();" in block(tex, "void texture_frame(uint64_t completed) {") and
          "if (frame_no % 600) return;" in block(tex, "void upload_earliest_report() {") and
          "logf(" not in block(tex, "void upload_earliest_note("),
          "the line of the uploads into an earlier segment's init is not said every 600 frames from texture_frame")
    redirect = block(tex, "struct UploadRedirect {")
    check("explicit UploadRedirect(bool want) : order(tl_seg) {" in redirect and
          "if (!want || tl_seg_index < 0 || tl_up_redirect) return;" in redirect,
          "UploadRedirect does more than note the recording segment when not wanted (RX_VK_UPLOAD_EARLIEST=0: as before)")
    check("`RX_VK_UPLOAD_EARLIEST=0`" in (ROOT / "docs" / "development.md").read_text(encoding="utf-8"),
          "docs/development.md does not say what RX_VK_UPLOAD_EARLIEST=0 does")
    twin_on = block(tex, "bool twin_same_on() {")
    check('getenv("RX_VK_TWIN_SAME")' in twin_on and 'atoi(getenv("RX_VK_TWIN_SAME")) != 0' in twin_on,
          "twin_same_on no longer reads RX_VK_TWIN_SAME")
    fixture_src = (ROOT / "tests" / "upload_same_test.cpp").read_text(encoding="utf-8")
    for needle in ["const Ranges* noted = twin_same_on() ? upload_same::twin_ranges(t.same.seg, value, same_image_id(t)) : nullptr;",
                   "const bool looked = part && same_twin_look(t, *part);", "same_twin_done(t, part, looked, true);",
                   "if (twin_same_on()) upload_same::note_upload(t.same.seg, value, image0, same_image_id(t), part);"]:
        check(needle in fixture_src, "the fixture's twin is not texture_touch's: " + needle)
    # (the twin makes a copy only where every unit it reads has a hash kept for the image as it is: the fixture's case counts it)
    twin_look = block(tex, "bool same_twin_look(Texture& tex, const Ranges& part) {")
    in_order(twin_look, ["upload_same::reached(s, part, tl_same_read);", "if (!upload_same::twin_known(s, tl_same_read)) return false;",
                         "return same_copy(tex);"], "same_twin_look")
    check("CHECK(st_twin_plain.load() == plain0 + 1 && st_twin_kept.load() == kept0 && st_twin_forgot.load() == forgot0);" in fixture_src,
          "the fixture no longer checks that a twin over hashes not the image's makes no copy")
    check("`RX_VK_TWIN_SAME=0`" in (ROOT / "docs" / "development.md").read_text(encoding="utf-8"),
          "docs/development.md does not say what RX_VK_TWIN_SAME=0 does")
    on = block(tex, "bool upload_same_on() {")
    check('getenv("RX_VK_UPLOAD_SAME")' in on and 'atoi(getenv("RX_VK_UPLOAD_SAME")) != 0' in on, "upload_same_on no longer reads RX_VK_UPLOAD_SAME")
    guide = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    check("`RX_VK_UPLOAD_SAME=0`" in guide, "docs/development.md does not say what RX_VK_UPLOAD_SAME=0 does")

    visit_head = "/// (Triforce, 11-16) [guest, guest + n) as memory_as_submitted has it"
    start = mem.find(visit_head)
    check(start >= 0, "vk_memory.cpp: memory_visit_as_submitted's note is missing")
    hashes = block(mem, "void memory_hash_as_submitted(")
    visit = mem[start:mem.index(hashes) + len(hashes)] + "\n"
    check("void memory_visit_as_submitted(" in visit and visit.count("shadow_n.load(std::memory_order_relaxed) && !overlay_off()") == 2,
          "vk_memory.cpp: memory_visit_as_submitted or memory_hash_as_submitted does not read the kept copies as memory_as_submitted does")
    # (a kept copy's hash goes whenever a capture copies other bytes into it: the copies into the kept pages are take_capture's
    # alone, each queued where the hash is let go unless this take found the page with its copy's bytes, and a page the take
    # copies twice loses it)
    check("    uint64_t hash = 0;\n};" in mem[mem.index("struct ShadowPage {"):], "vk_memory.cpp: ShadowPage keeps no hash")
    check(mem.count("{slot.b.get(), ") == 2 and mem.count("pairs.push_back({slot.b.get(), ") == 2,
          "vk_memory.cpp: something else than take_capture's copies writes a kept page (its hash would not go)")
    take = block(mem, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed) {")
    in_order(take, ["same_pages.push_back({p, gens.page_tex(p)});", "for (const auto& sp : same_pages) same_set.push_back(sp.first);",
                    "std::sort(same_set.begin(), same_set.end());", "ShadowPage& slot = shadow[p];", "slot.seq = shadow_seq;",
                    "if (slot.hash) {", "if (std::binary_search(same_set.begin(), same_set.end(), p)) hashed_kept.push_back(p);",
                    "else slot.hash = 0;", "pairs.push_back({slot.b.get(), (const uint8_t*)c->stage.get()",
                    "pairs.push_back({slot.b.get(), (const uint8_t*)c->map", "copy_pairs(pairs.data(), (int64_t)pairs.size());",
                    "if (hashed_kept[k] == hashed_kept[k - 1])", "s->second.hash = 0;"], "take_capture")
    check(sum(mem.count(x) for x in (".hash = 0;", "s.hash = upload_same")) == 3 and "(s.hash = upload_same::content_hash(s.b.get(), 4096))" in hashes,
          "vk_memory.cpp: a kept page's hash is set or let go elsewhere than take_capture and memory_hash_as_submitted")
    same_now = block(tex, "uint64_t same_hash_now(const Texture& tex, const upload_same::Unit& u) {")
    check("memory_hash_as_submitted(" in same_now and "same_keep_on()" in same_now, "same_hash_now does not use the kept hashes")
    check("`RX_VK_UPLOAD_SAME_KEEP=0`" in guide, "docs/development.md does not say what RX_VK_UPLOAD_SAME_KEEP=0 does")
    cut_head = "// ---- (Triforce, 11-16) a re-upload checked against the bytes its image holds (upload_same.h)"
    start = tex.find(cut_head)
    check(start >= 0, "vk_texture.cpp: the re-upload check's section is missing")
    cut = tex[start:tex.index("bool upload_inner(Texture& tex, const Ranges* dirty);", start)]

    fixture = (ROOT / "tests" / "upload_same_test.cpp").read_text(encoding="utf-8")
    check(fixture.count("/* VISIT */\n") == 1 and fixture.count("/* CHECK */\n") == 1, "the fixture's cut markers")
    fixture = fixture.replace("/* VISIT */\n", visit).replace("/* CHECK */\n", cut)
    with tempfile.TemporaryDirectory(prefix="upload-same-") as tmp:
        cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        built = subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                                "-D_CRT_SECURE_NO_WARNINGS", "-I", str(VK), str(cpp), "-o", str(exe)], capture_output=True, text=True)
        check(built.returncode == 0, "the fixture did not build:\n" + built.stderr[-4000:])
        env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_")}
        for case in CASES:
            r = subprocess.run([str(exe), case], env=env, capture_output=True, text=True, timeout=120)
            print((r.stdout + r.stderr).strip())
            check(r.returncode == 0, "upload_same_test %s" % case)
        r = subprocess.run([str(exe), "off"], env=dict(env, RX_VK_UPLOAD_SAME="0"), capture_output=True, text=True, timeout=60)
        print((r.stdout + r.stderr).strip())
        check(r.returncode == 0, "upload_same_test off")
        r = subprocess.run([str(exe), "astc_off"], env=dict(env, RX_VK_UPLOAD_SAME_ASTC="0"), capture_output=True, text=True, timeout=60)
        print((r.stdout + r.stderr).strip())
        check(r.returncode == 0, "upload_same_test astc_off")
        for case in ("twin_off", "twin_random"):   # ((Triforce, 11-16) RX_VK_TWIN_SAME=0: the twin as before, whole, forgetting)
            r = subprocess.run([str(exe), case], env=dict(env, RX_VK_TWIN_SAME="0"), capture_output=True, text=True, timeout=120)
            print((r.stdout + r.stderr).strip() + " (RX_VK_TWIN_SAME=0)")
            check(r.returncode == 0, "upload_same_test %s with RX_VK_TWIN_SAME=0" % case)
        for case in ("skip", "submitted", "random"):   # (RX_VK_UPLOAD_SAME_KEEP=0: every byte hashed at each look, nothing else)
            r = subprocess.run([str(exe), case], env=dict(env, RX_VK_UPLOAD_SAME_KEEP="0"), capture_output=True, text=True, timeout=120)
            print((r.stdout + r.stderr).strip() + " (RX_VK_UPLOAD_SAME_KEEP=0)")
            check(r.returncode == 0, "upload_same_test %s with RX_VK_UPLOAD_SAME_KEEP=0" % case)
    print("PASS: a re-upload uploads only the units whose bytes are not its image's, from the copy it hashed; whatever else changes "
          "the image forgets the hashes; ASTC decoded on the GPU too, a layer a unit; RX_VK_UPLOAD_SAME=0 and "
          "RX_VK_UPLOAD_SAME_ASTC=0 as before; the parallel replay's twin uploads what was asked for, keeping the hashes when its "
          "copy holds their bytes (RX_VK_TWIN_SAME=0 as before)")


if __name__ == "__main__":
    main()
