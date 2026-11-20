"""The stages' decoded code kept by their bytes (gpu/vk/stage_code_cache.h, vk_shader.cpp read_code; no game, no GPU).

A program pick that met a program object for the first time read its stages from guest memory, took their code keys and
decoded each (captured and lowered to Maxwell) under the shaders' lock: "the program picks read the stages' code 45.6 ms" in
the save's load-in frame (lab run r33-t28-s1-1), 48 of a stage's 60 us the decoding. The game makes program objects again for
stages it has had before; now what was made of a stage's bytes is kept by them, and a stage whose bytes were met before is a
copy, not a decoding.

Compiles and runs tests/test_stage_code_cache.cpp with vk_shader.cpp's struct Code, hash_bytes, control_end, code_key and its
cache's part with read_code cut out as they are, each scenario a process of its own:
- budget: RX_VK_STAGE_CODE_CACHE's MB, its default and its bounds;
- hit: equal bytes find what was made of them, a byte more, fewer or other finds nothing, and the lookup hash takes every byte;
- collide: entries under one lookup hash are told apart by their bytes;
- replace: a put for equal bytes replaces what was kept, once;
- evict: the least recently used go first, within the budget, and one alone over it is not kept;
- none: a budget of 0 keeps nothing;
- threads: finds and puts on four threads, whatever is found is what its bytes made;
- read: read_code decodes a stage whose bytes it met at another address once, its copy is the decoding's, and a byte changed in
  the code area or the control section is decoded again;
- read-none: RX_VK_STAGE_CODE_CACHE=0 decodes each read, as before.
Then each fails with the change undone, one part at a time (the mutations below). And reads the source: that vk_shader.cpp
includes the header, that the picks' stages_read reads through read_code, and that the switch is in the development guide.

    python -I -B tests/test_stage_code_cache.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
SCENARIOS = ["budget", "hit", "collide", "replace", "evict", "none", "threads", "read", "read-none"]
ENV = {"read-none": {"RX_VK_STAGE_CODE_CACHE": "0"}}

# (what undoes the change, one part at a time: (what, the scenario that must fail, the part changed (the header or the functions
# cut from vk_shader.cpp), the text and its replacement; or an environment))
MUTATIONS = [
    ("an entry taken by its lookup hash alone", "collide", "header", "if (*it->second->bytes == bytes) {", "if (true) {"),
    ("a find does not make the entry the most recently used", "evict", "header",
     "                lru_.splice(lru_.begin(), lru_, it->second);\n                return it->second->value;",
     "                return it->second->value;"),
    ("no budget", "evict", "header", "while (used_ > budget_ && !lru_.empty()) {", "while (false) {"),
    ("a put for equal bytes keeps both", "replace", "header", "if (*it->second->bytes == *bytes) at = it->second;",
     "if (false) at = it->second;"),
    ("an entry's bytes not counted in the budget", "evict", "header",
     "lru_.push_front(Entry{bytes, h, std::move(value), bytes->size() + cost});",
     "lru_.push_front(Entry{bytes, h, std::move(value), cost});"),
    ("read_code does not look in the cache", "read", "functions", "stage_cache().find(*bytes); kept && (head",
     "stage_cache().find(std::vector<uint8_t>()); kept && (head"),
    ("read_code keeps nothing", "read", "functions",
     "    }\n    stage_cache().put(bytes, std::make_shared<const Code>(c), code_cost(c));", "    }\n    (void)code_cost(c);"),
    ("RX_VK_STAGE_CODE_CACHE=0", "read", "env", "RX_VK_STAGE_CODE_CACHE", "0"),
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
    """vk_shader.cpp's parts the test compiles: the constants and hash_bytes, struct Code, and control_end through read_code"""
    return "\n".join([section(shader, "constexpr uint32_t kControlMagic", "/// A stage's package"),
                      block(shader, "struct Code {") + ";",
                      section(shader, "/// Whether this is a control section", "/// A control section and its code")])


def check_source(shader):
    if '#include "stage_code_cache.h"' not in shader:
        raise SystemExit("vk_shader.cpp does not include stage_code_cache.h")
    codes = function(shader, "std::vector<Code> stages_read(const NvnState& s) {")
    if "read_code(s.prog.sd[i]" not in codes:
        raise SystemExit("vk_shader.cpp stages_read: the picks do not read their stages through read_code (the cache)")
    if "RX_VK_STAGE_CODE_CACHE" not in (ROOT / "docs/development.md").read_text(encoding="utf-8"):
        raise SystemExit("docs/development.md: RX_VK_STAGE_CODE_CACHE is not in the switches table")


def build(cxx, tmp, name, functions, header):
    d = Path(tmp) / name
    d.mkdir()
    (d / "stage_code_functions.inc").write_text(functions, encoding="utf-8")
    (d / "stage_code_cache.h").write_text(header, encoding="utf-8")
    exe = d / "stage-code-cache.exe"
    r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-pthread", "-I" + str(d),
                        str(ROOT / "tests/test_stage_code_cache.cpp"), "-o", str(exe)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise SystemExit("the test did not compile (%s):\n%s" % (name, r.stdout))
    return exe


def run(exe, scenario, env=None):
    e = dict(os.environ)
    e.pop("RX_VK_STAGE_CODE_CACHE", None)
    e.pop("RX_VK_DUMP_STAGES", None)
    e.update(ENV.get(scenario, {}))
    e.update(env or {})
    return subprocess.run([str(exe), scenario], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120, env=e)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    check_source(shader)
    functions = cut(shader)
    header = (VK / "stage_code_cache.h").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="stage-code-cache-") as tmp:
        exe = build(args.cxx, tmp, "as-is", functions, header)
        for s in SCENARIOS:
            r = run(exe, s)
            if r.returncode:
                raise SystemExit("FAIL %s:\n%s" % (s, r.stdout))
            print("PASS " + r.stdout.strip().splitlines()[-1])
        for i, (what, scenario, part, old, new) in enumerate(MUTATIONS):
            if part == "env":
                r = run(exe, scenario, {old: new})
            else:
                f = replaced(functions, old, new) if part == "functions" else functions
                h = replaced(header, old, new) if part == "header" else header
                r = run(build(args.cxx, tmp, "mutant%d" % i, f, h), scenario)
            if r.returncode == 0:
                raise SystemExit("FAIL: the mutation survived (%s): scenario %s passed" % (what, scenario))
            print("PASS mutation caught: %s (%s: %s)" % (what, scenario, (r.stdout.strip().splitlines() or ["?"])[-1]))
    print("PASS: the stages' decoded code is kept by their bytes")


if __name__ == "__main__":
    main()
