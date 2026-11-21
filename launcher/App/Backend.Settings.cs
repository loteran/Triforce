using System.Text.Json.Nodes;
using Triforce.Core.GameFiles;
using Triforce.Logic;

namespace Triforce.Launcher;

/// <summary>The Settings page (DESIGN 6 Settings): a plain list in groups, Launcher, Game, Shortcuts, Help, Remove. The page
/// draws the list the launcher sends (state "settings"); pressing a row sends cmd 'setting' {id}. The two volume sliders are the
/// page's and the core's ('sound'); they are only listed here. The Color scheme row under them is the page's own (ui/app.js adds
/// it): its 'color' {hex} is kept by the core (LauncherColorChoice, Settings.Color) and comes back as state.color at start and
/// with the list. The benchmark (Backend.Bench.cs) is offered once a game is built.</summary>
public sealed partial class Backend
{
    const string DiscordUrl = "https://discord.gg/triforce";

    partial void RegisterSettings()
    {
        On("settings.list", _ => SendSettings(color: true));
        On("setting", m => SettingPressed((string?)m["id"] ?? (string?)m["arg"]));
        On("settings.lang", m => LanguageChosen((string?)m["id"]));
        On("settings.dlss", m =>
        {
            if ((string?)m["id"] != "yes") return;
            new Dlss(ctx).Accept();
            Toast("NVIDIA DLSS is included from the next build.", "success");
            SendSettings();
        });
        On("settings.uninstall", m =>
        {
            if ((string?)m["id"] != "yes") return;
            Log.Write("uninstall from Settings");
            try { new Uninstaller(ctx).Run(); }
            catch (Exception e) { Log.Write("uninstall: " + e); Toast("Triforce could not be removed: " + H(e.Message), "error"); return; }
            host.Close();
        });
    }

    partial void AddSettingsState(JsonObject state)
    {
        state["settings"] = SettingsRows();
        state["bench"] = BenchState();
        state["gameopts"] = GameOptsState();
    }

    /// <summary>The list again. color: the kept color too, as at start (only when the page asks for the list, which it does each time
    /// the Settings page opens; not after a row's change, so a color still being chosen there is not set back).</summary>
    void SendSettings(bool color = false)
    {
        var o = new JsonObject();
        AddSettingsState(o);
        if (color) o["color"] = colorChoice.Current;
        SendState(o);
    }

    // ---------------- the list ----------------

    static JsonObject Head(string h) => new() { ["h"] = h };

    static JsonObject Row(string id, string label, string? value, string act, bool danger = false, bool off = false)
    {
        var o = new JsonObject { ["id"] = id, ["label"] = label, ["value"] = value ?? "", ["actLabel"] = act };
        if (danger) o["danger"] = true;
        if (off) o["off"] = true;
        return o;
    }

    JsonArray SettingsRows()
    {
        var game = GameDir;
        var rows = new JsonArray
        {
            Head("Launcher"),
            new JsonObject { ["id"] = "music", ["label"] = "Music", ["kind"] = "slider" },
            new JsonObject { ["id"] = "sfx", ["label"] = "Sounds", ["kind"] = "slider" },
            Head("Game"),
            Row("folder", "Game folder", game ?? "Not built yet", "Open", off: game == null),
        };

        // language: the one chosen, and whether the game folder was built in it
        var chosen = Languages.Find(ctx.Settings.Language) ?? (game != null ? Languages.Find(Game.FolderLanguage(game)) : null) ?? Languages.Default();
        var built = game != null ? Game.FolderLanguage(game) : null;
        rows.Add(Row("lang", "Game language", chosen.NativeName + (game != null && built != null && built != chosen.Code ? " · needs a build again" : ""), "Change"));

        var dlss = new Dlss(ctx);
        if (dlss.Available)
        {
            var inGame = game != null && new Game(ctx).HasDlss(game);
            var value = dlss.Chosen ? (game != null && !inGame ? "Included · needs a build again" : "Included") : (inGame ? "Left out · needs a build again" : "Left out");
            rows.Add(Row("dlss", "NVIDIA DLSS", value, dlss.Chosen ? "Leave out" : "Include"));
        }
        rows.Add(Row("rebuild", "Build the game again", game != null ? Capital(RebuildTime()) : "Build the game first", "Build", off: game == null));

        rows.Add(Head("Shortcuts"));
        rows.Add(Row("desktop", "Desktop shortcut", HasShortcut(ShortcutPlace.Desktop) ? "On" : "Off", HasShortcut(ShortcutPlace.Desktop) ? "Remove" : "Make"));
        var menu = ctx.IsWindows ? "Start menu" : "Applications menu";
        rows.Add(Row("startmenu", menu, HasShortcut(ShortcutPlace.StartMenu) ? "On" : "Off", HasShortcut(ShortcutPlace.StartMenu) ? "Remove" : "Make"));
        rows.Add(SteamRow(game));

        rows.Add(Head("Help"));
        // ((Triforce) 11-20, ticket "The log button brings me to the wrong folder": the game writes triforce_log.txt (and .1 to .4)
        // beside its program in the game folder, which is what a bug report asks for; the launcher's own log is elsewhere)
        var gameLogs = GameLogFolder(game);
        rows.Add(Row("gamelog", "Game logs", gameLogs ?? "Build the game first", "Open", off: gameLogs == null));
        rows.Add(Row("log", "Launcher log", ctx.Platform.LogFolder(ctx.Home), "Open"));
        rows.Add(Row("report", "Report", "What to paste when you ask for help", "Copy"));
        rows.Add(Row("discord", "Discord", "discord.gg/triforce", "Open"));
        rows.Add(Row("versions", "Versions", VersionsLine(game), "Copy"));

        rows.Add(Head("Remove"));
        rows.Add(Row("uninstall", "Uninstall Triforce", "Your game folders and saves stay", "Uninstall", danger: true));
        return rows;
    }

    static string Capital(string s) => s.Length > 0 ? char.ToUpperInvariant(s[0]) + s.Substring(1) : s;

    /// <summary>((Triforce) 11-20) The folder the game writes triforce_log.txt in: its program's (Game.FindExe: the game folder on
    /// Windows, bin/ under it on Linux with the launcher script). Null without a built game.</summary>
    string? GameLogFolder(string? game) =>
        game != null && new Game(ctx).FindExe(game) is { } exe ? Path.GetDirectoryName(exe) : null;

    string VersionsLine(string? game) =>
        "launcher " + BuildInfo.Version + " · tools " + (new Tools(ctx).InstalledVersion ?? "none") + " · game " + (game != null ? Game.BuiltVersion(game) ?? "unknown" : "not built");

    bool HasShortcut(ShortcutPlace place)
    {
        try { return File.Exists(ctx.Platform.ShortcutPath(place)); } catch (Exception) { return false; }
    }

    JsonObject SteamRow(string? game)
    {
        var steam = new SteamLibrary(ctx);
        string? root;
        try { root = steam.Root(); } catch (Exception) { root = null; }
        if (root == null) return Row("steam", "Steam", "Steam is not on this PC", "Add", off: true);
        if (game == null) return Row("steam", "Steam", "Build the game first", "Add", off: true);
        var user = steam.User();
        if (user == null) return Row("steam", "Steam", "Sign in to Steam once first", "Add", off: true);
        var running = steam.Running(root) ? " · close Steam first" : "";
        var exe = steam.ShortcutTarget(game);
        return steam.Added(user, exe)
            ? Row("steam", "Steam", "In your library" + running, "Remove")
            : Row("steam", "Steam", "Not in your library" + running, "Add");
    }

    // ---------------- a row pressed ----------------

    void SettingPressed(string? id)
    {
        var game = GameDir;
        switch (id)
        {
            case "folder":
                if (game == null) { Toast("There is no game folder yet. Build the game first.", "error"); return; }
                OpenPlace(game);
                break;
            case "lang":
            {
                var cur = Languages.Find(ctx.Settings.Language) ?? (game != null ? Languages.Find(Game.FolderLanguage(game)) : null) ?? Languages.Default();
                Choice("Game language", "The game's text and voices. A change takes effect when the game is built again.", "settings.lang",
                    Languages.All.Select(l => (l.Code, l.NativeName, (string?)(l.NativeName != l.EnglishName ? l.EnglishName : null), l.Code == cur.Code)), "Choose");
                break;
            }
            case "dlss":
            {
                var dlss = new Dlss(ctx);
                if (dlss.Chosen)
                {
                    dlss.TurnOff();
                    Toast("NVIDIA DLSS is left out from the next build.", "success");
                    SendSettings();
                    return;
                }
                var html = string.Join("", dlss.LicenseText.Split("\n\n").Select(p => "<p>" + H(p).Replace("\n", "<br>") + "</p>"));
                Ask("NVIDIA DLSS", html, "settings.dlss", new[] { ("no", "Cancel", false), ("yes", Dlss.AcceptLabel, true) });
                break;
            }
            case "rebuild":
                if (game == null) { Toast("Build the game first.", "error"); return; }
                StartRebuild();
                break;
            case "desktop":
            case "startmenu":
                ToggleShortcut(id == "desktop" ? ShortcutPlace.Desktop : ShortcutPlace.StartMenu);
                break;
            case "steam":
                SteamPressed(game);
                break;
            case "log":
                OpenPlace(ctx.Platform.LogFolder(ctx.Home));
                break;
            case "gamelog":
                if (GameLogFolder(game) is { } logs) OpenPlace(logs);
                break;
            case "report":
                Copy(SettingsReport(game));
                break;
            case "discord":
                OpenLink(DiscordUrl);
                break;
            case "versions":
                Copy("Triforce " + VersionsLine(game));
                break;
            case "uninstall":
            {
                var plan = new Uninstaller(ctx).Describe();
                string List(IEnumerable<string> l) => "<ul style=\"margin:4px 0 0 20px\">" + string.Join("", l.Select(x => "<li>" + H(Capital(x)) + "</li>")) + "</ul>";
                Ask("Uninstall Triforce", "<p>This removes:</p>" + List(plan.Removes) + "<p>This keeps:</p>" + List(plan.Keeps), "settings.uninstall",
                    new[] { ("no", "Keep Triforce", false), ("yes", "Uninstall", true) });
                break;
            }
        }
    }

    void LanguageChosen(string? code)
    {
        if (Languages.Find(code) is not { } lang) return;
        ctx.Settings.Language = lang.Code;
        var game = GameDir;
        if (game != null && Game.FolderLanguage(game) != lang.Code)
            Toast(H(lang.NativeName) + " is used from the next build. Build the game again to play in it.", "success");
        else Toast("The game's language is " + H(lang.NativeName) + ".", "success");
        SendSettings();
    }

    void ToggleShortcut(ShortcutPlace place)
    {
        var path = ctx.Platform.ShortcutPath(place);
        try
        {
            ctx.Require(path);
            if (File.Exists(path))
            {
                File.Delete(path);
                if (place == ShortcutPlace.Desktop) ctx.Settings.Desktop = false; else ctx.Settings.StartMenu = false;
                Toast("The shortcut is removed.", "success");
            }
            else
            {
                var exe = ctx.IsWindows ? (ctx.InstalledDir is { } d ? Path.Combine(d, BuildInfo.SetupAsset) : ctx.SelfPath) : ctx.Env("APPIMAGE") ?? ctx.SelfPath;
                ctx.Platform.CreateShortcut(place, new Shortcut(exe, "", Path.GetDirectoryName(exe)!, exe, "Play Triforce"));
                if (place == ShortcutPlace.Desktop) ctx.Settings.Desktop = true; else ctx.Settings.StartMenu = true;
                Toast(File.Exists(path) ? "The shortcut is made." : "The shortcut could not be made.", File.Exists(path) ? "success" : "error");
            }
        }
        catch (Exception e)
        {
            Log.Write("shortcut: " + e.Message);
            Toast("The shortcut could not be changed: " + H(e.Message), "error");
        }
        SendSettings();
    }

    void SteamPressed(string? game)
    {
        var steam = new SteamLibrary(ctx);
        var root = steam.Root();
        if (root == null) { Toast("Steam is not on this PC.", "error"); return; }
        if (game == null) { Toast("Build the game first, then add it to Steam.", "error"); return; }
        var user = steam.User();
        if (user == null) { Toast("Start Steam, sign in once, close it, then try again.", "error"); return; }
        if (steam.Running(root)) { Toast(H(SteamLibrary.CloseFirst), "error"); return; }
        var exe = steam.ShortcutTarget(game);
        try
        {
            if (steam.Added(user, exe))
            {
                steam.Remove(user, exe);
                Toast("Triforce is out of your Steam library.", "success");
            }
            else
            {
                steam.Add(user, exe, SteamPictures());
                Toast("Triforce is in your Steam library. Start Steam to see it.", "success");
            }
        }
        catch (Exception e) when (e is SteamException || e is InvalidOperationException || e is IOException)
        {
            Toast(H(e.Message), "error");
        }
        SendSettings();
    }

    /// <summary>Steam's pictures, made from the game's own title logo: the logo centred on the launcher's dark (the "logo" kind,
    /// drawn over Steam's own hero picture, without the dark). None when the game's art is not read.</summary>
    Dictionary<string, byte[]> SteamPictures()
    {
        var pics = new Dictionary<string, byte[]>();
        try
        {
            if (MediaFor("png", "Logo_USEU_00") is not { } png) return pics;
            var (lw, lh, rgba) = Png.Decode(png);
            foreach (var (kind, _, w, h) in SteamVdf.Pictures)
            {
                var clear = kind == "logo";
                var fill = kind == "icon" ? 0.92 : kind == "logo" ? 0.96 : 0.72;
                pics[kind] = Png.Encode(Compose(rgba, lw, lh, w, h, fill, clear), w, h);
            }
        }
        catch (Exception e) { Log.Write("steam art: " + e.Message); }
        return pics;
    }

    /// <summary>A w x h picture: #0a0908 (or clear), the logo scaled to fit fill of it, centred (bilinear, over the background).</summary>
    static byte[] Compose(byte[] src, int sw, int sh, int w, int h, double fill, bool clear)
    {
        var o = new byte[w * h * 4];
        if (!clear)
            for (var i = 0; i < w * h; i++) { o[i * 4] = 0x0a; o[i * 4 + 1] = 0x09; o[i * 4 + 2] = 0x08; o[i * 4 + 3] = 255; }
        var k = Math.Min(w * fill / sw, h * fill / sh);
        int dw = Math.Max(1, (int)(sw * k)), dh = Math.Max(1, (int)(sh * k)), x0 = (w - dw) / 2, y0 = (h - dh) / 2;
        for (var y = 0; y < dh; y++)
        {
            var fy = Math.Clamp((y + 0.5) / k - 0.5, 0, sh - 1);
            int ya = (int)fy, yb = Math.Min(sh - 1, ya + 1);
            var ty = fy - ya;
            for (var x = 0; x < dw; x++)
            {
                var fx = Math.Clamp((x + 0.5) / k - 0.5, 0, sw - 1);
                int xa = (int)fx, xb = Math.Min(sw - 1, xa + 1);
                var tx = fx - xa;
                // (premultiplied, so the edges of the logo do not take on the colour of its clear pixels)
                double r = 0, g = 0, b = 0, a = 0;
                void Tap(int px, int py, double wt)
                {
                    var p = (py * sw + px) * 4;
                    var al = src[p + 3] / 255.0 * wt;
                    r += src[p] * al; g += src[p + 1] * al; b += src[p + 2] * al; a += al;
                }
                Tap(xa, ya, (1 - tx) * (1 - ty)); Tap(xb, ya, tx * (1 - ty)); Tap(xa, yb, (1 - tx) * ty); Tap(xb, yb, tx * ty);
                var q = ((y0 + y) * w + x0 + x) * 4;
                if (clear)
                {
                    if (a > 0) { o[q] = (byte)Math.Round(r / a); o[q + 1] = (byte)Math.Round(g / a); o[q + 2] = (byte)Math.Round(b / a); }
                    o[q + 3] = (byte)Math.Round(a * 255);
                }
                else
                {
                    o[q] = (byte)Math.Round(r + o[q] * (1 - a)); o[q + 1] = (byte)Math.Round(g + o[q + 1] * (1 - a)); o[q + 2] = (byte)Math.Round(b + o[q + 2] * (1 - a));
                }
            }
        }
        return o;
    }

    /// <summary>What a player pastes where they ask for help: versions, system, the game folder, the graphics card, the choices,
    /// the mods, then setup.log's last lines; the account's folder reads %USERPROFILE%.</summary>
    string SettingsReport(string? game)
    {
        var known = new List<string>
        {
            "Triforce " + VersionsLine(game) + ", " + Environment.OSVersion.VersionString,
            "Game folder: " + (game ?? ctx.Settings.GameFolder ?? "none") + (game != null ? " (language " + (Game.FolderLanguage(game) ?? "unknown") + (new Game(ctx).HasDlss(game) ? ", with NVIDIA DLSS" : "") + ")" : ""),
            "Graphics: " + (string.Join(", ", Gpus.List().Select(g => g.Name)) is { Length: > 0 } gpus ? gpus : "unknown"),
            "Launcher: " + ctx.Home + (ctx.TestRoot != null ? " (test run in " + ctx.TestRoot + ")" : ""),
        };
        if (game != null)
        {
            var mods = MineMods(game, false);
            known.Add("Mods: " + mods.Count + " (" + mods.Count(m => m.On) + " on)" + (mods.Count > 0 ? ": " + string.Join(", ", mods.Select(m => (m.On ? "" : "off ") + m.Rel)) : ""));
        }
        var profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        return Report.NoName(string.Join(Environment.NewLine, known) + Environment.NewLine + Environment.NewLine + "---- setup.log's last lines ----" + Environment.NewLine +
                             Report.Tail(ctx.Log.ReadAll(), Report.Lines), profile);
    }
}
