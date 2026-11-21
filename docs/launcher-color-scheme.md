# The launcher's color scheme

Settings > Launcher has a **Color scheme** row under Music and Sounds. Its value is a diamond swatch and the hex
(`#E0B23E`). Selecting it (mouse, Enter, A) drops a picker open under the row, with the launcher's own .25 s height
transition; selecting the row again, or Escape / B, closes it. The picker has a saturation / brightness square, a hue bar
(one of the launcher's sliders), **Hex**, **R**, **G** and **B** fields checked as typed (six hex digits with or without
`#`, whole numbers 0-255; a wrong value gets a red border and a line saying what is allowed), eight ready colors with
Triforce gold first, and **Reset to gold**.

The color takes the place of the gold everywhere the launcher uses it: text and links, tab underlines, section titles,
sliders and their diamonds, borders, hairlines, outlines, glows, the focus ring, the build line and the wordmark. The
polished bars (Play, "Find my best settings", "Build the game", Apply, ...) keep their gradient, inner hairline, sheen and
emboss, all made from the new color, and their text takes dark or light ink, whichever has more contrast. Triforce gold
gives the look exactly as it was: with it the page sets nothing, and the stylesheets' own values draw it.

Two limits keep any color usable, both far from gold:

- **Text and lines never go too dark.** What is drawn in the color on the dark ground is lightened until it reads at
  4.5:1 or better on `#0a0908` (black draws them in `#787878`).
- **Polished bars never go too light.** They are shaded no lighter than 78% lightness, so a near-white color gives a
  silver bar, not a flat white one.

## How it works

- **theme.css** holds the gold as a family of `r, g, b` variables (`--gold-rgb`, `--gold-hi-rgb`, ... and the bars'
  own `--gold-bar-*-rgb`), each with today's value; `v3.css`, `proto.css` and `app.css` use them in place of every gold
  literal, alphas kept.
- **app.js** ("settings: Color scheme") moves each of today's shades from gold to the chosen color: the hue turns by as
  much, the saturation scales by as much, the lightness keeps its share of the way to white or to black. At gold it
  gives back every literal exactly. `paintColor()` sets the variables on `<html>` with the class `tint`, and removes them
  all for gold. The page adds the row itself after `sfx`; a row the launcher sends with `id: 'color'` takes its place.
- **Pictures.** The crest (`img/crest.png`) is redrawn under `html.tint` with `hue-rotate / saturate / brightness`,
  masked by `img/crest-gold.png` (its metal only), so the three stones stay. The title ornaments and glows get the same
  filter chain from `--tint-h/-s/-b`. The game's own logo and its glow are the game's art and are not tinted. CSS
  `hue-rotate` is an approximation: far turns (blue, violet) come close but not exact.

## How it is kept

1. The page sends `{t:'cmd', c:'color', hex:'#rrggbb'}`, 400 ms after the last change (as `sound`), and keeps the same
   value in `localStorage['triforce.color']`, which it reads at start before the first screen.
2. The launcher takes only a `#rrggbb` (`Logic/LauncherColor.cs`, `FromCommand` / `Parse`), in lower case. It takes it
   as the message arrives (`App/Backend.Core.cs`, `ColorArrived`, called from `OnMessage` before the command goes to a
   worker), into `LauncherColorChoice`, which holds the player's last color while the launcher runs. The command's
   worker (`On("color")`, `Keep`) then keeps the newest one as `Settings.Color`: `HKCU\Software\Triforce\Color` on
   Windows, `Color=` in `builder.ini` on Linux, `registry.txt` in a test run. Anything else is left out, with one log
   line a run.
3. The launcher sends back `LauncherColorChoice.Current` (the player's last color, else the kept one) as `state.color`
   at start (`OnReady`, beside `sound`) and with the list the Settings page asks for each time it opens
   (`settings.list`). The page then uses it, and it wins over the page's own storage, unless a choice of the player's is
   still waiting to be sent. Because the launcher sends what it holds and not what it has written, a `--snapshot` run
   (which writes nothing) and a `settings.list` whose worker runs before the `color` sent just ahead of it both send
   the new color, not the old one.
4. A settings file from before the color has no `Color`: it reads as Triforce gold (`#e0b23e`), which is also what
   **Reset to gold** sends. An uninstall removes it with the other settings.

## Checks

- `dotnet run --project launcher/LogicTests color`: the parsing of the page's message, the keeping and reading back in
  `registry.txt` and `builder.ini`, old files, a bad kept value, `--snapshot`'s no writes, and the color sent back
  under `--snapshot` and when the commands' workers run out of order.
- `python -I -B tests/test_launcher_color.py`: the page and the launcher agree on the message, the default and the image,
  the image rides in the exe (the `ui/**` embed), and every word the page shows spells "color": the page's own
  (`app.js`, `demo.js`, `index.html`) and every string in the launcher's C# (`App`, `Logic`, `Core`), the game options'
  help the Settings rows show as their tooltips among them. That help is the game menu's own words
  (`gpu/vk/vk_menu.cpp`, which "game options: schema matches vk_menu.cpp" compares); the menu's own table is spelled
  "color" by the in-game change set, not by this one.
- `node --check launcher/ui/app.js`.
