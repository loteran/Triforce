"""A parallel replay initializes each new image before any segment's use of it, in queue order.

A parallel replay (RX_VK_PARALLEL) records a big command buffer's segments on several threads and submits them in order.
A new image's first layout change (UNDEFINED -> GENERAL, and its clear) was recorded into the upload command buffer of
the worker that made it. That is whichever worker got there first, not the first segment in the queue. An earlier
segment that used the image too (a copy into it again, the BC3 encoder's dispatch) was submitted first and found it
UNDEFINED (VUID-vkCmdDraw-None-09600). The BC3 scratch's first layout change had the same problem.

Cuts the production code (the upload command buffer state, create()'s initialization, bc_scratch_layout, the segment
hooks in vk_texture.cpp, and replay_handle's segment jobs and submits in vk_replay.cpp) into
tests/parallel_init_order_test.cpp. Vulkan is a spy that plays the submitted command buffers in queue order: every use
of an image must come after its one UNDEFINED -> GENERAL change, and the serial path must record exactly the same calls
as before. Some cases run the segments' workers one after another in a set order (a later segment makes the image, an
earlier one uses it; the BC3 scratch and its second one); others run them at once, held at fixed points: a worker closes
its segment while another records into that segment's init command buffer, a worker makes an image after an earlier
segment was submitted. When a segment's init command buffer cannot be begun, no worker may start and the whole command
buffer is recorded in order on the replay thread. When one cannot be ended, the replay must stop for good (replay_fatal,
exit code 3) once every worker is done, with nothing that uses the lost images submitted, and no later work may run. The
init pools' startup (init_pools_make) is run with each pool creation and allocation failing in turn.

(Triforce, 11-17) A texture the segments sample, looked at first by a later segment's worker, was uploaded there and again by
each earlier segment that sampled it (the twin), after the later one's upload in the queue. Now the upload goes into the init
command buffer of the earliest segment still recorded (vk_texture.cpp UploadRedirect, cut with the upload command buffer
state; gpu/vk/upload_earliest.h). The fixture's texture_look is texture_touch's steps that decide it (tests/test_upload_same.py
reads them in the source): the "upload_" cases check that every draw sampling it comes after this submission's one upload in the
queue, with a barrier between, that no twin is made, which command buffer the upload is in and the segment it notes, a worker
closing its segment while another records into its init command buffer (each segment's own lock: another segment's end
does not wait), an image and the BC3
scratches made inside the upload (redirected or in the segment's own uploads), the worker's own upload command buffer given
back, an upload read on the GPU left in its segment's uploads (the twin as before),
the full barrier before an upload that is its init command buffer's first command, every order of four segments' workers and
every set of them sampling it, and RX_VK_UPLOAD_EARLIEST=0 recording call for call what it did before.

python -I -B tests/test_parallel_init_order.py --cxx clang++ --headers path/to/Vulkan-Headers/include
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
CASES = ("later_makes", "three", "scratch", "scratch2", "in_order", "serial", "close_race", "after_submit", "init_begin_fail",
         "init_end_fail", "alloc_fail", "own_uses", "own_all", "own_off", "upload_rules", "upload_later_first", "upload_three",
         "upload_own", "upload_own_nested", "upload_close_race", "upload_end_other", "upload_nested", "upload_then_make", "upload_barrier",
         "upload_gpu_route", "upload_random")
OFF_CASES = ("upload_later_first", "upload_off", "upload_gpu_route", "upload_random")   # (with RX_VK_UPLOAD_EARLIEST=0: as before)


def block(source, marker):
    if source.count(marker) != 1:
        raise SystemExit("marker not found once: " + marker)
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def cut(source, start, end, after=""):
    a = source.index(after) if after else 0
    if source.count(start, a) != 1:
        raise SystemExit("marker not found once: " + start)
    a = source.index(start, a)
    return source[a:source.index(end, a)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--headers", required=True, type=Path, help="the Vulkan headers' include folder")
    args = parser.parse_args()
    vk = ROOT / "gpu" / "vk"
    texture = (vk / "vk_texture.cpp").read_text(encoding="utf-8")
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")

    create = cut(texture, "total_bytes += x.bytes + x.pad;", "    static uint32_t ids;", after="Texture* create(")
    create = create[create.index("\n") + 1:]   # (from the line after: the image's initialization, to its Texture*)
    if "// into GENERAL, and cleared" not in create or "Texture* p = tex.get();" not in create:
        raise SystemExit("create()'s initialization moved: update this test")
    jobs = cut(replay, "    struct Job {", "    seg_ms += std::chrono::duration")
    have_seg_init = "struct SegInit {" in texture
    if "struct UploadRedirect {" not in texture or "if (!want || tl_seg_index < 0 || tl_up_redirect) return;" not in texture:
        raise SystemExit("FAIL: vk_texture.cpp has no UploadRedirect, or InitRedirect does not stand aside inside one")
    for step in ("j->order = base + k;", "j->index = (int)k;", "texture_set_segment(j->order);", "texture_segment_worker(j->index);"):
        if replay.count(step) != 1:   # (UploadRedirect's order: upload_earliest::order_of)
            raise SystemExit("FAIL: replay_handle no longer numbers its segments' order as their index from base: " + step)
    exits = {"init_end_fail": 3} if have_seg_init else {}   # (replay_fatal ends the process)
    for step in ("record_segment(c, j->g);", "texture_begin_recording(j->r->upload, j->r->value);", "vkQueueSubmit2(",
                 "j->uploads = texture_uploads_recorded();", "par_pool.cv.notify_all();"):
        if step not in jobs:
            raise SystemExit("the parallel block no longer has: " + step)
    hooks = [block(texture, m) for m in (
        "void texture_set_segment(uint64_t seg) {", "void texture_begin_recording(VkCommandBuffer upload, uint64_t value) {",
        "bool texture_uploads_recorded() {")]
    hooks += [block(texture, m) for m in ("void texture_segments_begin(", "void texture_segment_worker(", "int texture_segment_end(")
              if m in texture]
    have_pools = "bool init_pools_make(" in replay
    if have_pools:   # (the last fallible step of replay_start: no later failure returns with them made)
        start = block(replay, "bool replay_start() {")
        call = "if (par_threads_max() >= 2 && !init_pools_make(pi)) return false;"
        if call not in start or start.index(call) < start.index("for (Frame& f : frames)") or                 "return false" in start[start.index(call) + len(call):]:
            raise SystemExit("FAIL: replay_start does not make the init pools last (a later failure would leave them made)")
    inc = {
        "init_upload.inc": cut(texture, "// ---- the recording's upload command buffer ----", "// ---- staging:"),
        "init_create.inc": create,
        "init_scratch.inc": block(texture, "bool bc_scratch_layout("),
        "init_hooks.inc": "\n".join(hooks),
        "init_recording.inc": block(replay, "struct Recording {") + ";",
        "init_parstats.inc": cut(replay, "struct ParStats {", "\nstruct Segment {"),   # (what the jobs time their parts in)
        "init_jobs.inc": jobs,
        "init_pools.inc": "\n".join(block(replay, m) for m in ("void init_pools_destroy() {", "bool init_pools_make(")) if have_pools else "",
        "init_fatal.inc": block(replay, "[[noreturn]] void replay_fatal(") if have_seg_init else "",
    }
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    failed = []
    with tempfile.TemporaryDirectory(prefix="parallel-init-order-") as tmp:
        folder = Path(tmp)
        for name, text in inc.items():
            (folder / name).write_text(text + "\n", encoding="utf-8")
        binary = folder / "fixture.exe"
        command = [compiler, "-std=c++20", "-O1", "-w", "-I" + str(folder), "-I" + str(args.headers),
                   "-DHAVE_SEG_INIT=%d" % have_seg_init, "-DHAVE_INIT_POOLS=%d" % have_pools,
                   str(ROOT / "tests" / "parallel_init_order_test.cpp"), "-o", str(binary)]
        command += [] if os.name == "nt" else ["-pthread"]
        r = subprocess.run(command, capture_output=True, text=True, timeout=180)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("the fixture did not build")
        env = {k: v for k, v in os.environ.items() if k != "RX_VK_UPLOAD_EARLIEST"}
        runs = [(case, env, case) for case in CASES]
        runs += [(case, dict(env, RX_VK_UPLOAD_EARLIEST="0"), case + " (RX_VK_UPLOAD_EARLIEST=0)") for case in OFF_CASES]
        for case, case_env, label in runs:
            try:
                r = subprocess.run([str(binary), case], env=case_env, capture_output=True, text=True, timeout=90)
            except subprocess.TimeoutExpired:
                print(label + ": timed out")
                failed.append(label)
                continue
            print(r.stdout, end="", flush=True)
            print(r.stderr, end="", file=sys.stderr)
            if r.returncode != exits.get(case, 0) or "FAIL" in r.stdout:
                failed.append(label)
    if failed:
        raise SystemExit("FAIL: new images' initialization order or the init pools' startup: " + ", ".join(failed))
    print("PASS: each new image is initialized once, before every use in queue order; serial calls are unchanged; "
          "the init pools are all made or none; a texture the segments sample is uploaded once, in the earliest segment still "
          "recorded, before every draw that samples it (RX_VK_UPLOAD_EARLIEST=0: as before, the twin)")


if __name__ == "__main__":
    main()
