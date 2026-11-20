"""Button prompts beside a mod that ships its own resource size table (host/prompts.c, host/mods.c; #bug-tracker 11-18).

The prompts' font is larger than the game's; our prompts/rsizetable.zs has room for it, but a mod's own size table wins over
ours, and then the game dropped the font, so prompts.c gave the prompts up ("the game's own icons in texts"). The size hook
the lifted code calls for every file (host/mods.c hos_mods_table_size, nx2/oot_patches.py patch_mod_sizes) now gives the
font its room too. Checked here, by the source:
- the hook answers for the font (hos_prompts_font_file) before it looks the path up among the mods' files' others, and
  says it once in the log;
- prompts.c gives up only when that hook has not run in this game (hos_mods_size_hook_seen), not whenever a mod has a
  size table;
- hos_prompts_font_file answers only for the font's own key, through hos_prompts_romfs (which builds it once).
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(text, head):
    at = text.index(head)
    end = text.index("{", at) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[at:end]


class PromptsModSizeTable(unittest.TestCase):
    def test_hook_sizes_the_font(self):
        mods = (ROOT / "host" / "mods.c").read_text(encoding="utf-8")
        hook = function(mods, "uint64_t hos_mods_table_size(uint64_t size, uint64_t path) {")
        self.assertIn("InterlockedExchange(&hos_mods_size_hook_seen, 1)", hook)
        self.assertLess(hook.index("hos_mods_size_hook_seen"), hook.index("if (!table_n"))   # (seen also with no mods)
        self.assertIn("hos_prompts_font_file(key, built, sizeof built)", hook)
        self.assertIn("zs_content_size(built)", hook)
        self.assertIn("button prompts' font gets", hook)

    def test_prompts_give_up_only_without_the_hook(self):
        src = (ROOT / "host" / "prompts.c").read_text(encoding="utf-8")
        build = function(src, "static int build(char *out, size_t cap) {")
        gate = build[build.index('hos_mods_has(SIZES_PATH, ".rsizetable.zs")'):]
        self.assertTrue(re.search(r"if \(!hos_mods_size_hook_seen\) \{\s*hos_log\(\"prompts: a mod has the resource size table: "
                                  r"the game's own icons in texts\"\);\s*return 0;", gate), "the fallback is not gated on the hook")
        font = function(src, "int hos_prompts_font_file(const char *key, char *out, size_t cap) {")
        self.assertIn("if (strcmp(key, FONT_PATH)) return 0;", font)
        self.assertIn("hos_prompts_romfs(FONT_PATH, out, cap)", font)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
