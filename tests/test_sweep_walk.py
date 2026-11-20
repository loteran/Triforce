"""A texture sweep under the budget orders only the textures it can take (vk_texture.cpp texture_sweep; source read, no game).

texture_sweep ordered the whole cache by last use for every pass, under the budget too: every 10 presents, and for the dead
render targets' passes between them, up to every present while the camera moves (3,200 textures in the owner's test18 session,
0.17 ms a pass). Under the budget a pass takes only a texture the GPU drew that something drew over since, or one the game let go
while those go early; every other texture is passed over. Now only textures the GPU drew or the game let go are ordered for such
a pass (after NX2navi's texture table walked by its entries, rewritten); over the budget, and with RX_VK_SWEEP_ALL=1, all are.

What must hold, read from the source: the filter's condition; that every way a texture can be taken under the budget needs one
of the two (the loop's exit for a pass under the budget, drawn_over and drawn_over_shifted false for a texture the GPU never drew,
gone_early only for one the game let go), that what the filtered list is summed for counts only those (the finalized ones the
GPU drew, the let-go mass), that the hold's pass (over the budget) is never filtered, that the retired count is not read off the
list, the switch in the development guide. Each of those is then broken in turn in a copy of the source and the check must fail.

    python -I -B tests/test_sweep_walk.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(text, head):
    at = text.index(head)
    end = text.index("{", at) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[at:end]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit(where + " lacks: " + needle)


def check(tex, dev):
    sweep = body(tex, "void texture_sweep(uint64_t completed, bool sweep, uint64_t budget, uint64_t evict_to, bool known, uint64_t used, "
                      "uint64_t allowed,\n                   bool dead_only, uint64_t hold_to, uint64_t rule_to) {")
    need("texture_sweep", sweep, [
        # (the filter: everything over the budget or with the switch, else drawn by the GPU or let go by the game)
        'static const bool sweep_all = getenv("RX_VK_SWEEP_ALL") && atoi(getenv("RX_VK_SWEEP_ALL")) != 0;',
        "if (!sweep || sweep_all || kv.second->gpu_written || kv.second->let_go) order.push_back({kv.second->last_frame, kv.second.get()});",
        # (under the budget the loop takes only what was drawn over or let go early)
        "\n        if (sweep && !overwritten && !gone_early) continue;",
        "const bool overwritten = shifted || drawn_over(tex);",
        "const bool gone_early = let_go_short && tex->let_go && !tex->gpu_written &&",
        # (drawn over: only a texture the GPU drew)
        "    const auto drawn_over = [&](const Texture* tex) {\n        if (!tex->gpu_written) return false;",
        "if (!dead_targets || !tex->gpu_written || !tex->first_render_frame",
        # (the sums over the list: drawn ones and let-go ones only)
        "if (!keep_finalized_max || !tex->gpu_written || !tex->released",
        "if (tex->let_go && !tex->gpu_written && let_go_due(tex)) mass += tex->bytes + tex->pad;",
        "if (tex->gpu_written && tex->first_render_frame && tex->guest_size) by_start.push_back(tex);",
        # (the hold's pass is over the budget: never filtered)
        "const bool by_hold = evict_to != 0 && !sweep;",
        # (the retired count from the cache's size, not the list's)
        "const size_t textures_before = textures.size();",
        "const size_t n = textures_before - textures.size();"])
    if "order.size()" in sweep:
        raise SystemExit("texture_sweep reads a count off the ordered list, which a pass under the budget filters")
    # (nothing else is taken from the list under the budget: the whole-texture asks walk the cache itself)
    need("texture_sweep", sweep, ["for (Texture* want : whole_pending)\n            for (auto& kv : textures)"])
    loops = sweep.count("for (auto& [last, tex] : order)")
    if loops != 6:
        raise SystemExit("texture_sweep walks the ordered list %d times, not 6: a new walk must be checked against the filter" % loops)
    need("docs/development.md", dev, ["`RX_VK_SWEEP_ALL=1`"])


MUTATIONS = [
    ("the filter leaves out the textures the GPU drew",
     "if (!sweep || sweep_all || kv.second->gpu_written || kv.second->let_go)", "if (!sweep || sweep_all || kv.second->let_go)"),
    ("the filter leaves out the textures the game let go",
     "if (!sweep || sweep_all || kv.second->gpu_written || kv.second->let_go)", "if (!sweep || sweep_all || kv.second->gpu_written)"),
    ("the filter applies over the budget",
     "if (!sweep || sweep_all || kv.second->gpu_written || kv.second->let_go)", "if (sweep_all || kv.second->gpu_written || kv.second->let_go)"),
    ("a pass under the budget takes a live texture",
     "\n        if (sweep && !overwritten && !gone_early) continue;",
     "\n        if (sweep && !overwritten && !gone_early && tex->gpu_written) continue;"),
    ("drawn over counts a texture the GPU never drew",
     "    const auto drawn_over = [&](const Texture* tex) {\n        if (!tex->gpu_written) return false;",
     "    const auto drawn_over = [&](const Texture* tex) {\n        if (!tex->bytes) return false;"),
    ("the let-go mass counts drawn textures",
     "if (tex->let_go && !tex->gpu_written && let_go_due(tex)) mass +=",
     "if (!tex->gpu_written && let_go_due(tex)) mass +="),
    ("the retired count is read off the list", "const size_t n = textures_before - textures.size();", "const size_t n = order.size() - textures.size();"),
    ("a new walk of the list", "    static uint64_t let_go_n, let_go_bytes, let_go_logged_at;\n",
     "    static uint64_t let_go_n, let_go_bytes, let_go_logged_at;\n    for (auto& [last, tex] : order) (void)tex;\n"),
]


def main():
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check(tex, dev)
    for what, old, new in MUTATIONS:
        if tex.count(old) != 1:
            raise SystemExit("mutation %r: its text is not once in vk_texture.cpp" % what)
        try:
            check(tex.replace(old, new), dev)
        except SystemExit:
            print("PASS: the check fails when %s" % what)
            continue
        raise SystemExit("the check passes when %s" % what)
    print("PASS: a sweep under the budget orders only the textures it can take")


if __name__ == "__main__":
    main()
