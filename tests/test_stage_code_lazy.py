"""The stages read for the program picks before their code is lowered (vk_shader.cpp stage_head, read_code, codes_whole,
program_pick, mesh_program_locked; RX_VK_STAGE_CODE_LAZY; no game, no GPU).

Of the 60 us a pick spent on each stage of a program object met for the first time, 48 were the lowering of its code to Maxwell
("the program picks read the stages' code 45.6 ms" in the save's load-in frame, lab run r33-t28-s1-1), though a pick needs the
lowered code only to translate a program, and the shaders are prepared before the title: the pick finds the program, read from
the program cache or read ahead. Now a pick reads a stage's head (its key, stage and header: what finds the program) and lowers
the code only when it translates a variant. A program found in the cache was made from stages that lowered (its key names
them); one not found has its stages lowered before it is translated, and should one not lower, the pick starts again from those
that do: the program it was before.

Compiles and runs tests/test_stage_code_lazy.cpp with vk_shader.cpp's struct Code, hash_bytes, control_end, code_key,
stage_head, its stage code cache's part with read_code, the lists with codes_of and codes_whole, mesh_program_locked,
program_code_keys, compute_code_key and program_pick cut out as they are, each scenario a process of its own:
- transcript: what each pick of a set of authored program objects gives (stages that lower and that do not, vertex, fragment,
  compute, task, mesh, a header alone, a geometry stage, a program the cache has, the same bytes in another object, another
  variant): the same with RX_VK_STAGE_CODE_LAZY on and off, RX_VK_STAGE_CODE_CACHE on and off; every translation made of
  stages lowered;
- found: a program the cache has is picked with none of its stages lowered, a variant to translate lowers them once, and the
  same bytes in another object are neither lowered nor keyed again;
- reread: an object none of whose stages lowers gives none and is read again at the next pick, as before;
- eager: with RX_VK_STAGE_CODE_LAZY=0, or a switch that acts in the lowering (RX_VK_REJECT_STAGES, RX_VK_REJECT_KEYS,
  RX_VK_OLD_LOWERING, RX_VK_DUMP_NATIVE, RX_VK_DUMP_FAILED), each stage is lowered as it is read, as before;
- unlocked: a pick reading a program object's stages from a slow guest memory does not hold the shaders' lock
  (RX_VK_STAGE_CODE_UNLOCKED, codes_read, program_read_once.h's gate), and another pick goes on meanwhile;
- gate: with a program's release waiting (the gate closed) the stages are read under the lock, as before.
Then each fails with the change undone, one part at a time (the mutations below). And reads the source for what the test does
not compile: that parse_code's head is stage_head's, that the picks read heads through codes_read, and that the switches are in
the development guide.

    python -I -B tests/test_stage_code_lazy.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
SWITCHES = ("RX_VK_STAGE_CODE_LAZY", "RX_VK_STAGE_CODE_CACHE", "RX_VK_STAGE_CODE_UNLOCKED", "RX_VK_REJECT_STAGES",
            "RX_VK_REJECT_KEYS", "RX_VK_OLD_LOWERING", "RX_VK_DUMP_NATIVE", "RX_VK_DUMP_FAILED", "RX_VK_DUMP_STAGES",
            "RX_VK_SYNC_SHADERS")
# (the transcript with the stages lowered as read and nothing kept: the picks as before)
BEFORE = {"RX_VK_STAGE_CODE_LAZY": "0", "RX_VK_STAGE_CODE_CACHE": "0"}
CONFIGS = [{"RX_VK_STAGE_CODE_LAZY": "0"}, {}, {"RX_VK_STAGE_CODE_CACHE": "0"}]
# (the switches that act in the lowering: a stage lowered as it is read with any of them; values that change nothing here)
EAGER = [{"RX_VK_STAGE_CODE_LAZY": "0"}, {"RX_VK_REJECT_STAGES": "4"}, {"RX_VK_REJECT_KEYS": "-"}, {"RX_VK_OLD_LOWERING": "0"},
         {"RX_VK_DUMP_NATIVE": "0123456789abcdef"}, {"RX_VK_DUMP_FAILED": "."}]

# (what undoes the change, one part at a time: (what, the scenario that must fail, the text cut from vk_shader.cpp and its
# replacement; or an environment). A transcript fails when it differs from BEFORE's or the run fails)
MUTATIONS = [
    ("a pick takes a stage read as a head that did not lower", "transcript", "picks",
     "if (c.ok && !c.header_only) stage_code[c.stage] = &c;", "if (!c.header_only) stage_code[c.stage] = &c;"),
    ("no new pick when a stage did not lower", "transcript", "picks",
     "if (std::memcmp(now, stage_code, sizeof now)) return program_pick_locked(s, kind, color_types, cs);",
     "if (false) return program_pick_locked(s, kind, color_types, cs);"),
    ("a translation's stages not lowered first", "transcript", "picks",
     "    codes_whole(s);\n    const Code* now[8]{};", "    const Code* now[8]{};"),
    ("a mesh pick takes a stage that did not lower", "transcript", "picks",
     "        if (!c.ok) continue;\n        if (c.header_only) m.prim = &c;", "        if (c.header_only) m.prim = &c;"),
    ("no new mesh pick when a stage did not lower", "transcript", "picks",
     "if (!(mesh_stages(cs) == m)) return mesh_program_locked(s, color_types);",
     "if (false) return mesh_program_locked(s, color_types);"),
    ("program_code_keys names stages read as heads", "transcript", "picks",
     "    codes_whole(s);\n    for (const Code& c : cs) {\n        if (!c.ok) continue;",
     "    for (const Code& c : cs) {\n        if (!c.ok) continue;"),
    ("a list none of whose stages lowers is kept", "reread", "types",
     "if (!v.empty() && std::none_of(v.begin(), v.end(), [](const Code& c) { return c.ok; })) v.clear();", ""),
    ("a head taken as lowered", "transcript", "types", "c.ok = true, c.whole = false;", "c.ok = true;"),
    ("RX_VK_STAGE_CODE_LAZY=0", "found", "env", "RX_VK_STAGE_CODE_LAZY", "0"),
    ("the stages read under the lock", "unlocked", "picks", "draw_once.unlocked([&] { read = stages_read(s); });",
     "read = stages_read(s);"),
    ("the lock let go while a release waits", "gate", "picks",
     "if (!stage_unlocked() || !draw_once.may_let_go()) return codes_of(s);", "if (!stage_unlocked()) return codes_of(s);"),
    ("RX_VK_STAGE_CODE_UNLOCKED=0", "unlocked", "env", "RX_VK_STAGE_CODE_UNLOCKED", "0"),
]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def section(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def function(text, head):
    at = text.index(head)
    return text[at:text.index("\n}\n", at)]


def replaced(text, old, new):
    if text.count(old) != 1:
        raise SystemExit("the mutation's text is not there once: %r" % old)
    return text.replace(old, new)


def cut(shader):
    """vk_shader.cpp's parts the test compiles: (the stage's code, its reading and the lists; the picks)"""
    types = "\n".join([section(shader, "constexpr uint32_t kControlMagic", "/// A stage's package"),
                       block(shader, "struct Code {") + ";",
                       section(shader, "/// Whether this is a control section", "/// A control section and its code"),
                       section(shader, "std::unordered_map<uint64_t, std::vector<std::unique_ptr<Program>>> programs;",
                               "const Shader::Profile& profile()"),
                       section(shader, "const std::vector<Code>& codes_of(", "std::atomic<int64_t> st_translated")])
    picks = "\n".join([section(shader, "struct MeshStages {", "const Program* mesh_program_for("),
                       section(shader, "std::string program_code_keys(const NvnState& s) {", "/// The last answer for this serial"),
                       section(shader, "std::atomic<int64_t> st_pick_lock_ns", "void program_wait(")])
    return types, picks


def check_source(shader):
    parse = function(shader, "Code parse_code(const uint8_t* ctl, const uint8_t* area, uint32_t end) {")
    head = "    Code c = stage_head(ctl, area, end);"
    if not parse.split("\n")[1].startswith(head) or "    if (c.whole) return c;\n    c.ok = false, c.whole = true;" not in parse:
        raise SystemExit("vk_shader.cpp parse_code does not begin with stage_head's head (the test's parse_code does)")
    if "Code c;" in parse or "c.key = " in parse:
        raise SystemExit("vk_shader.cpp parse_code makes a head of its own besides stage_head's")
    codes = function(shader, "std::vector<Code> stages_read(const NvnState& s) {")
    if "read_code(s.prog.sd[i], true)" not in codes:
        raise SystemExit("vk_shader.cpp stages_read does not read the picks' stages as heads")
    for head in ("const Program* program_pick(const NvnState& s, ProgramKind kind, const uint8_t color_types[8]) {",
                 "const Program* mesh_program_locked("):
        if "= codes_read(s);" not in function(shader, head):
            raise SystemExit("vk_shader.cpp %s does not read its stages with the lock let go (codes_read)" % head.split("(")[0])
    guide = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    for switch in ("RX_VK_STAGE_CODE_LAZY", "RX_VK_STAGE_CODE_UNLOCKED"):
        if switch not in guide:
            raise SystemExit("docs/development.md: %s is not in the switches table" % switch)


def build(cxx, tmp, name, types, picks):
    d = Path(tmp) / name
    d.mkdir()
    (d / "stage_lazy_types.inc").write_text(types, encoding="utf-8")
    (d / "stage_lazy_picks.inc").write_text(picks, encoding="utf-8")
    exe = d / "stage-code-lazy.exe"
    r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-pthread",
                        "-I" + str(d), "-I" + str(VK), str(ROOT / "tests/test_stage_code_lazy.cpp"), "-o", str(exe)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise SystemExit("the test did not compile (%s):\n%s" % (name, r.stdout))
    return exe


def run(exe, scenario, env=None):
    e = {k: v for k, v in os.environ.items() if k not in SWITCHES}
    e.update(env or {})
    return subprocess.run([str(exe), scenario], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120, env=e)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    check_source(shader)
    types, picks = cut(shader)
    with tempfile.TemporaryDirectory(prefix="stage-code-lazy-") as tmp:
        exe = build(args.cxx, tmp, "as-is", types, picks)
        before = run(exe, "transcript", BEFORE)
        if before.returncode:
            raise SystemExit("FAIL transcript (as before):\n%s" % before.stdout)
        for env in CONFIGS:
            r = run(exe, "transcript", env)
            if r.returncode or r.stdout != before.stdout:
                raise SystemExit("FAIL transcript with %s, not as before:\n%s\nbefore:\n%s" % (env or "the defaults", r.stdout,
                                                                                               before.stdout))
            print("PASS transcript with %s: the picks as before (%d)" % (env or "the defaults", len(before.stdout.splitlines()) - 1))
        for s in ("found", "reread", "unlocked", "gate"):
            r = run(exe, s)
            if r.returncode:
                raise SystemExit("FAIL %s:\n%s" % (s, r.stdout))
            print("PASS " + r.stdout.strip().splitlines()[-1])
        for env in EAGER:
            r = run(exe, "found", env)
            if r.returncode == 0:
                raise SystemExit("FAIL: with %s the stages were not lowered as read" % env)
            print("PASS eager with %s: lowered as read, as before" % env)
        for i, (what, scenario, part, old, new) in enumerate(MUTATIONS):
            if part == "env":
                r = run(exe, scenario, {old: new})
            else:
                t = replaced(types, old, new) if part == "types" else types
                p = replaced(picks, old, new) if part == "picks" else picks
                r = run(build(args.cxx, tmp, "mutant%d" % i, t, p), scenario)
            survived = r.returncode == 0 and (scenario != "transcript" or r.stdout == before.stdout)
            if survived:
                raise SystemExit("FAIL: the mutation survived (%s): scenario %s passed" % (what, scenario))
            print("PASS mutation caught: %s (%s: %s)" % (what, scenario, (r.stdout.strip().splitlines() or ["?"])[-1]))
    print("PASS: the picks lower a stage's code only to translate, and pick as before")


if __name__ == "__main__":
    main()
