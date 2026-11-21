"""The launcher's Color scheme (Settings > Launcher): the page and the launcher agree on the message, the default and
the image, read from the source (no launcher is built or started).

The page (launcher/ui/app.js) draws any color in place of Triforce gold and sends {t:'cmd', c:'color', hex:'#rrggbb'};
the launcher (launcher/App, launcher/Logic) keeps it with its other settings (Settings.Color, LauncherColor) and sends
it back as state.color at start and with the Settings list. The parsing and the keeping are tested in
launcher/LogicTests (dotnet run --project launcher/LogicTests color); this checks what joins the two:

- the page sends 'color' with hex, takes state.color, and keeps a choice not sent yet over the launcher's older one;
- the page's gold (GOLD, the first preset) is the launcher's default (LauncherColor.Default);
- the launcher takes 'color' as it arrives and puts that choice (else the kept color) in the start state and in the
  list the page asks for, so a --snapshot run or a list handled first does not set the page back;
- img/crest-gold.png (the crest's metal, the mask the tint uses) is a PNG and rides in the exe with the rest of ui/;
- every word the page shows spells "color" (US): the page's own words (app.js, demo.js, index.html) and every string the
  launcher's C# has (launcher/App, Logic, Core), the game options' help that the Settings rows show among them.

    python -I -B tests/test_launcher_color.py
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "launcher"
UI = LAUNCHER / "ui"


def read(path):
    return path.read_text(encoding="utf-8")


def without_comments(js):
    """The script's code with its /* */ and // comments taken out (a // inside a string, as in a URL, takes the rest of
    its line with it: fine for looking for words)."""
    js = re.sub(r"/\*.*?\*/", "", js, flags=re.S)
    return re.sub(r"(^|[^:\\])//[^\n]*", r"\1", js)


class LauncherColorTests(unittest.TestCase):
    def setUp(self):
        self.app = read(UI / "app.js")
        self.color_cs = read(LAUNCHER / "Logic" / "LauncherColor.cs")
        self.core = read(LAUNCHER / "App" / "Backend.Core.cs")
        self.settings = read(LAUNCHER / "App" / "Backend.Settings.cs")

    def test_same_default(self):
        page = re.search(r"const GOLD = '(#[0-9a-f]{6})';", self.app)
        launcher = re.search(r'public const string Default = "(#[0-9a-f]{6})";', self.color_cs)
        self.assertTrue(page and launcher)
        self.assertEqual(page.group(1), launcher.group(1))
        self.assertEqual(page.group(1), "#e0b23e")
        self.assertIn("const PRESETS = [[GOLD, 'Triforce gold']", self.app)

    def test_page_message(self):
        self.assertIn("cmd('color', { hex: CS.hex })", self.app)
        self.assertIn("if (typeof m.color === 'string') setColor(m.color, 'host');", self.app)
        # (the launcher's kept color is older than a choice still waiting for its 400 ms)
        self.assertIn("if (from === 'host' && colorSave) return;", self.app)
        self.assertIn("colorSave = setTimeout(() => { colorSave = 0;", self.app)

    def test_launcher_keeps_and_sends(self):
        # (the color is taken as the message arrives, before its handler goes to a worker, and what is sent back is that
        # choice: a --snapshot run writes nothing, and a 'settings.list' may run before the 'color' just ahead of it)
        backend = read(LAUNCHER / "App" / "Backend.cs")
        cmd = backend.split('case "cmd":', 1)[1].split("break;", 1)[0]
        self.assertLess(cmd.index('if (c == "color") ColorArrived(m);'), cmd.index("Task.Run("))
        self.assertIn("colorChoice.Take(m)", self.core)
        self.assertIn('On("color", _ => colorChoice.Keep());', self.core)
        self.assertIn("LauncherColor.FromCommand(message)", self.color_cs)
        self.assertIn("settings.Color = hex;", self.color_cs)
        on_ready = self.core.split("void OnReady()", 1)[1].split("\n    }\n", 1)[0]
        self.assertIn('["color"] = colorChoice.Current', on_ready)
        self.assertIn('On("settings.list", _ => SendSettings(color: true));', self.settings)
        self.assertIn('if (color) o["color"] = colorChoice.Current;', self.settings)
        self.assertNotIn('["color"] = ctx.Settings.Color', self.settings + self.core)
        self.assertNotIn('["color"] = s.Color', self.core)
        names = read(LAUNCHER / "Logic" / "Platform" / "Settings.cs")
        self.assertIn('public const string Color = "Color";', names)

    def test_crest_mask_embedded(self):
        png = (UI / "img" / "crest-gold.png").read_bytes()
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        self.assertLess(len(png), 64 * 1024)
        self.assertIn("img/crest-gold.png", read(UI / "app.css"))
        csproj = read(LAUNCHER / "App" / "Triforce.Launcher.csproj")
        self.assertIn('<EmbeddedResource Include="..\\ui\\**\\*" LogicalName="ui/%(RecursiveDir)%(Filename)%(Extension)" />', csproj)

    def test_us_spelling(self):
        for name in ("app.js", "demo.js", "index.html"):
            text = read(UI / name)
            if name.endswith(".js"):
                text = without_comments(text)
            else:
                text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
            self.assertEqual(re.findall(r"(?i)colour", text), [], name)
        # (what the launcher sends the page: every string in its C#, the game options' help among them, which the
        # Settings page shows as each row's tooltip; comments are not shown and keep their own spelling)
        literal = re.compile(r'"(?:[^"\\\n]|\\.)*"')
        for part in ("App", "Logic", "Core"):
            for path in sorted((LAUNCHER / part).rglob("*.cs")):
                if {"bin", "obj"} & set(path.relative_to(LAUNCHER).parts):
                    continue
                found = [s for s in literal.findall(read(path)) if re.search(r"(?i)colour", s)]
                self.assertEqual(found, [], path.relative_to(ROOT).as_posix())


if __name__ == "__main__":
    unittest.main()
