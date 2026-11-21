"""(Triforce, FSR3-03) AMD FSR 3.1 gets the motion of what moves by itself, as NVIDIA DLSS and AMD FSR 4 do. Its inputs
(gpu/vk/vk_fsr3_inputs.cpp) had the camera's motion alone (shaders/fsr3_prep.comp): Link, the others and what the game moves
got the motion of what is behind them, and FSR trailed or smeared them. The chain's redraws (gpu/vk/vk_temporal.cpp
capture_motion, actor_motion) served only its own engines: no draw stored its positions while FSR 3.1 drew
(temporal_capture_wanted asked temporal_wanted, false then), and pre_fsr3 took this frame's draws a second time and threw
them away.

What must hold, read from the source (no game, no GPU):
- the draws store their positions, and the skinned ones are noted, while FSR 3.1 upscales (temporal_motion_wanted), and the
  scene's sizes are known to them then (temporal_scene_size asks FSR's own inputs' rule);
- the redraws draw into the motion they are given and go by the history they are given (RedrawTarget), not the chain's own
  alone; the chain gives its own;
- FSR 3.1's motion image is a colour attachment, and its Frame carries the camera as drawn and the frame before's;
- in place of the upscale's dispatch: fsr3_upscale has the chain draw into FSR's motion after its inputs and before its
  passes (temporal_fsr3_motion), the frame's draws taken once, let go when the inputs were not made; the draw state set
  again after (vk_draw.cpp fsr3_stand_in);
- before the bloom: temporal_pre_tonemap takes the draws once and gives them to pre_fsr3, which gives them to fsr3_pre's
  own_motion, called after its inputs and before its passes;
- the Upscaler row's hints for FSR 3.1 no longer say that what moves by itself can trail.
The old lines fail.

    python -I -B tests/test_fsr3_own_motion.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(source, head):
    start = source.index(head)
    return source[start:source.index("\n}\n", start) + 3]


def checks(t):
    temporal, fsr3, inputs, inputs_h, draw, menu = (t["temporal"], t["fsr3"], t["inputs"], t["inputs_h"], t["draw"],
                                                    t["menu"])
    # the gates: FSR 3.1 too
    check("bool temporal_motion_wanted() { return temporal_wanted() || fsr3_wanted(); }" in temporal,
          "temporal_motion_wanted does not include AMD FSR 3.1")
    check("bool temporal_capture_wanted() { return motion_mode() == kMotionCapture && temporal_motion_wanted(); }" in temporal,
          "the draws store their positions only for the chain's own engines")
    skinned = function(temporal, "const char* temporal_note_skinned_draw(const SkinnedDraw& k) {")
    check("!temporal_motion_wanted()) return \"actor motion off\";" in skinned, "the skinned draws are noted only for the chain's engines")
    skin = function(draw, "static const char* skin_check(const NvnState& s, const Program& p, const DrawArgs& d, const Targets& at) {")
    check("!temporal_motion_wanted())" in skin and "|| !temporal_wanted())" not in skin,
          "skin_check takes the skinned draws only for the chain's engines")
    size = function(temporal, "bool temporal_scene_size(uint32_t width, uint32_t height) {")
    check("(fsr3_wanted() && fsr3_inputs::scene_size(width, height))" in size,
          "temporal_scene_size knows no scene while FSR 3.1 upscales in place of the dispatch")
    # the redraws draw where they are told, by the history they are told
    begin = function(temporal, "bool begin_redraw(")
    check("ca.imageView = motion," in begin and "s.motion.view" not in begin, "begin_redraw draws into the chain's motion alone")
    capture = function(temporal, "int capture_motion(VkCommandBuffer cmd, const Camera& cam,")
    check("const RedrawTarget& to" in capture and "s.captured_frame == to.last" in capture and
          "to.jitter_before[i]" in capture and "to.motion)" in capture, "capture_motion does not go by its target")
    check("s.last_frame" not in capture and "s.last_jitter" not in capture, "capture_motion still goes by the chain's history")
    actor = function(temporal, "int actor_motion(VkCommandBuffer cmd, const Camera& cam,")
    check("const RedrawTarget& to" in actor and "it->second.frame != to.last" in actor and "to.motion)" in actor,
          "actor_motion does not go by its target")
    check("s.last_frame" not in actor, "actor_motion still goes by the chain's history")
    chain = function(temporal, "VkFormat in_format, VkFormat out_format) {")   # (its definition, not its declaration)
    check("const RedrawTarget into{s.motion.view, last, s.last_jitter};" in chain and
          "captures, into)" in chain and "motion, into)" in chain, "the chain does not give its redraws its own motion")
    # FSR 3.1's inputs: an attachment, and the camera for the redraws
    prep = function(inputs, "bool prepare(VkCommandBuffer cmd, Texture* in, VkImageView in_view, Frame& out, bool linear) {")
    # ((Triforce, FG-4) and a copy's destination: the upscaler chain's motion copied over it for frame generation)
    check("make_image(cmd, s.motion, VK_FORMAT_R16G16_SFLOAT, rw, rh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)"
          in prep, "FSR 3.1's motion image is no colour attachment (nor a copy's destination): the redraws cannot draw into it")
    check("o.last = s.last_frame;" in prep and prep.index("o.last = s.last_frame;") < prep.index("s.last_frame = now;"),
          "FSR 3.1's Frame does not carry the frame its history is of")
    for line in ("std::memcpy(o.current, cam.current, sizeof o.current);", "std::memcpy(o.projection, cam.projection, sizeof o.projection);",
                 "std::memcpy(o.before, previous, sizeof o.before);", "std::memcpy(o.reproject, pc.reproject, sizeof o.reproject);",
                 "o.jitter_before[0] = s.last_jitter[0]", "o.depth = cam.depth;", "o.history = motion;"):
        check(line in prep, "FSR 3.1's Frame lacks what the redraws take: " + line)
    check(prep.index("o.jitter_before[0] = s.last_jitter[0]") < prep.index("s.last_jitter[0] = cam.jitter[0]"),
          "FSR 3.1's Frame carries this frame's jitter as the frame before's")
    check("struct Own {" in inputs_h and "} own;" in inputs_h, "vk_fsr3_inputs.h's Frame has no camera for the redraws")
    # fsr3_motion: FSR's motion as the target
    motion = function(temporal, "int fsr3_motion(VkCommandBuffer cmd, const fsr3_inputs::Frame& f, const std::vector<State::Captured>& captures) {")
    check("const RedrawTarget into{f.motion, o.last, o.jitter_before};" in motion and
          "capture_motion(cmd, cam, o.before, o.reproject, f.width, f.height, dview, o.history, captures, into)" in motion and
          "actor_motion(cmd, cam, o.before, o.reproject, f.width, f.height, dview, o.history, into)" in motion,
          "fsr3_motion does not draw into FSR 3.1's motion")
    check("take_captures()" not in motion, "fsr3_motion takes the frame's draws itself (its callers do, once)")
    check("camera_planes(cam);" in motion and "cam.depth = o.depth;" in motion, "fsr3_motion's camera lacks its planes or depth")
    # in place of the upscale's dispatch
    public = function(temporal, "int temporal_fsr3_motion(VkCommandBuffer cmd, const fsr3_inputs::Frame* f) {")
    check(public.count("take_captures()") == 1 and "return fsr3_motion(cmd, *f, captures);" in public and
          "std::lock_guard lk(s.mu);" in public, "temporal_fsr3_motion does not take the frame's draws once, under the lock")
    check("s.asked = texture_frame_no(), s.holding = true;" in public, "the redraws' buffers are not kept while FSR 3.1 draws")
    up = function(fsr3, "bool fsr3_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,")
    check("const int own = temporal_fsr3_motion(cmd, &f);" in up, "fsr3_upscale gives FSR 3.1 the camera's motion alone")
    check(up.index("fsr3_inputs::prepare(cmd, in, in_view, f, hdr)") < up.index("temporal_fsr3_motion(cmd, &f)") <
          up.index("std::lock_guard lk(o.mu);") < up.index("fsr3::upscale(cmd, ai)"),
          "fsr3_upscale's own motion is not drawn after its inputs, outside its lock and before its passes")
    check("if (upscaling) (void)temporal_fsr3_motion(cmd, nullptr);" in up, "the frame's draws are kept when FSR 3.1's inputs were not made")
    stand = function(draw, "bool fsr3_stand_in(VkCommandBuffer cmd, const NvnState& s, const Program& p) {")
    check("last_pipeline = VK_NULL_HANDLE;" in stand and "vertex_set.on = false" in stand,
          "fsr3_stand_in does not set the draw state again after the redraws")
    # before the bloom
    tone = function(temporal, "Texture* temporal_pre_tonemap(VkCommandBuffer cmd) {")
    check(tone.count("take_captures()") == 1 and "pre_fsr3(cmd, scene, view, rw, rh, ow, oh, captures)" in tone,
          "temporal_pre_tonemap does not give FSR 3.1 the draws it took")
    pre3 = function(temporal, "bool pre_fsr3(")
    check("take_captures()" not in pre3, "pre_fsr3 takes the frame's draws a second time and throws them away")
    check("s.skins.clear()" not in pre3, "pre_fsr3 throws the skinned draws away before the redraws")
    check("fsr3_motion(cmd, f, captures)" in pre3 and "fsr3_pre(cmd, scene, view, ow, oh, &picture, own_motion)" in pre3,
          "pre_fsr3 does not have the redraws draw into FSR's motion")
    pre = function(fsr3, "bool fsr3_pre(")
    check(pre.index("fsr3_inputs::prepare(cmd, scene, scene_view, f, true)") < pre.index("own_motion(f)") <
          pre.index("fsr3::upscale(cmd, ai)"), "fsr3_pre's own motion is not drawn after its inputs and before its passes")
    # the hints
    hints = function(menu, "const char* hn_upscaler(int v, int variant) {")
    table = hints[:hints.index("};")]
    entries = [e for e in table.split('",\n') if e.strip()][:7]
    # ((Triforce, options_version 7) one line per engine: FSR 3.1's Native AA reads as FSR 3.1's, v = ur::engine(v))
    fsr3_rows = [e for e in entries if "built from AMD's own shaders" in e]
    check(len(fsr3_rows) == 1 and "v = ur::engine(v);" in hints, "hn_upscaler's FSR 3.1 line is not found")
    for e in fsr3_rows:
        check("trail" not in e, "the FSR 3.1 hint still says that what moves by itself can trail")


MUTATIONS = [
    ("the old capture gate", "temporal",
     "bool temporal_capture_wanted() { return motion_mode() == kMotionCapture && temporal_motion_wanted(); }",
     "bool temporal_capture_wanted() { return motion_mode() == kMotionCapture && temporal_wanted(); }"),
    ("the old scene size", "temporal", " ||\n           (fsr3_wanted() && fsr3_inputs::scene_size(width, height));", ";"),
    ("the second take", "temporal", "    s.asked = texture_frame_no(), s.holding = true;\n    VkImageView picture = VK_NULL_HANDLE;",
     "    (void)take_captures();\n    s.asked = texture_frame_no(), s.holding = true;\n    VkImageView picture = VK_NULL_HANDLE;"),
    ("the chain's motion only", "temporal", "ca.imageView = motion,", "ca.imageView = s.motion.view,"),
    ("no attachment", "inputs", "rw, rh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)", "rw, rh)"),
    ("the camera's motion alone", "fsr3", "    const int own = temporal_fsr3_motion(cmd, &f);\n", "    const int own = 0;\n"),
    ("the old hint", "menu", "\"AMD's, on any graphics card, built from AMD's own shaders.\",",
     "\"AMD's, on any graphics card, built from AMD's own shaders. Things that move by themselves can trail for now.\","),
]


def main():
    texts = {"temporal": (VK / "vk_temporal.cpp").read_text(encoding="utf-8"),
             "fsr3": (VK / "vk_fsr3.cpp").read_text(encoding="utf-8"),
             "inputs": (VK / "vk_fsr3_inputs.cpp").read_text(encoding="utf-8"),
             "inputs_h": (VK / "vk_fsr3_inputs.h").read_text(encoding="utf-8"),
             "draw": (VK / "vk_draw.cpp").read_text(encoding="utf-8"),
             "menu": (VK / "vk_menu.cpp").read_text(encoding="utf-8")}
    checks(texts)
    for name, which, new, old in MUTATIONS:
        check(new in texts[which], "the mutation '%s' has nothing to change" % name)
        t = dict(texts)
        t[which] = t[which].replace(new, old, 1)
        try:
            checks(t)
        except SystemExit:
            continue
        raise SystemExit("FAIL: the mutation '%s' passed" % name)
    print("PASS: AMD FSR 3.1 gets the motion of what moves by itself, in place of the dispatch and before the bloom; the old "
          "lines fail")


if __name__ == "__main__":
    main()
