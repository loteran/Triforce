using System.Text;
using Triforce.Logic;
using static Triforce.LogicTests.T;

namespace Triforce.LogicTests;

static partial class Tests
{
    public static IEnumerable<(string Name, Func<Task> Run)> All()
    {
        (string, Func<Task>) Sync(string n, Action a) => (n, () => { a(); return Task.CompletedTask; });
        yield return Sync("settings: registry.txt round trip", SettingsTestRoot);
        yield return Sync("settings: builder.ini round trip (Linux store)", SettingsIni);
        yield return Sync("linux: the user folder follows the saves (triforce/user, then hookshot, then the game's)", LinuxUserFolder);
        yield return Sync("benchmark: an uneven run gets Steady frame rate Refresh steps, an even one keeps the player's", BenchSteady);
        yield return Sync("benchmark: a place left out is read and said", BenchSkipped);
        yield return Sync("settings: color scheme, the page's 'color' read", ColorParse);
        yield return Sync("settings: color scheme kept and read back (registry.txt, builder.ini, old files)", ColorKept);
        yield return Sync("settings: color scheme, the launcher's color (--snapshot, commands out of order)", ColorChoice);
        yield return Sync("startup: argument parsing", StartArgsParsing);
        yield return Sync("startup: lock name algorithm", LockName);
        yield return Sync("startup: installed run, old exe removed, second start defers", StartupInstalled);
        yield return Sync("startup: not installed shows Install; uninstall refused while open", StartupNotInstalled);
        yield return Sync("install + uninstall in a test root", InstallUninstall);
        yield return ("release: check, parse, notes against a fake server", ReleaseCheck);
        yield return ("release: download with size check and resume", DownloadResume);
        yield return ("release: self-swap, new launcher up", SelfSwapUp);
        yield return ("release: self-swap, new launcher fails, rolled back", SelfSwapFails);
        yield return Sync("release: decide what the start does", Decide);
        yield return Sync("tools: unpack the whole zip, refuse unsafe names", ToolsUnpack);
        yield return Sync("tools: base list and parts", ToolsParts);
        yield return ("tools: update from the fake server with progress", ToolsUpdate);
        yield return Sync("build: @@ protocol from a recorded transcript", Protocol);
        yield return Sync("build: estimates from history and without", Estimates);
        yield return ("build: runner with a fake make_game (done, args, log, times)", RunnerDone);
        yield return ("build: runner stop kills the tree", RunnerStop);
        yield return ("build: runner failure and test-root refusal", RunnerFail);
        yield return Sync("game: the built test game folder", GameFolder);
        yield return Sync("intake: folders, .nup header, packed romfs, space", Intake);
        yield return ("intake: make_game --check (fake)", IntakeCheck);
        yield return Sync("saves: slots of lab-game-merge98 (read only)", SavesListing);
        yield return ("saves: find, import, export with backups", SavesCopy);
        yield return Sync("mods: list, order, on/off, remove, rebuild mark", ModsFolder);
        yield return Sync("mods: add archives and their safety checks", ModsArchives);
        yield return ("mods: GameBanana install into the mods folder (fake server)", GameBananaInstall);
        yield return ("mods: GameBanana live list (read only)", GameBananaLive);
        yield return Sync("mods: GameBanana page parsing and plain text", GameBananaParse);
        yield return Sync("linux: release keys, modes, parts list", LinuxLogic);
        yield return Sync("steam: add, set, remove in a fake Steam folder", SteamShortcut);
        yield return Sync("report: names out", ReportNames);
        yield return Sync("languages", LanguagesTest);
        yield return Sync("media: the game's music loop through decode, mix and WAV (smpl)", MusicLoop);
        yield return Sync("media: the WAV cache made again with the loop, an older file replaced", MusicCache);
    }

    // ---- settings ----

    static void SettingsTestRoot()
    {
        var root = NewRoot("settings");
        var ctx = Ctx(root);
        ctx.Settings.GameFolder = @"D:\Games\Triforce";
        ctx.Settings.Language = "de";
        ctx.Settings.Dlss = true;
        ctx.Settings.Set("Language", "fr");
        ctx.Platform.Settings.SetUninstall("InstallLocation", root);
        ctx.Platform.Settings.SetUninstallNumber("NoModify", 1);
        var again = Ctx(root);
        Equal(@"D:\Games\Triforce", again.Settings.GameFolder, "GameFolder read back");
        Equal("fr", again.Settings.Language, "the last value for a name wins");
        Check(again.Settings.Dlss, "Dlss read back");
        Equal(40, again.Settings.MusicVolume, "music volume default");
        again.Settings.MusicVolume = 250;
        Equal(100, Ctx(root).Settings.MusicVolume, "volume clamped");
        var lines = File.ReadAllLines(Path.Combine(root, "registry.txt"));
        Check(lines.Contains(@"Software\Triforce\GameFolder=D:\Games\Triforce"), "the old Setup's line format: " + string.Join(" / ", lines));
        Check(lines.Contains(@"Software\Microsoft\Windows\CurrentVersion\Uninstall\Triforce\NoModify=1"), "uninstall entry lines");
        Equal(1, lines.Count(l => l.StartsWith(@"Software\Triforce\Language=")), "one line per name after a write");
        again.Platform.Settings.RemoveAll();
        Equal(null, Ctx(root).Settings.Language, "settings removed");
        Equal(root, Ctx(root).Platform.Settings.GetUninstall("InstallLocation"), "uninstall entry kept until removed");
        var snap = Ctx(root);
        snap.NoWrites = true;
        snap.Settings.Language = "ja";
        Equal(null, Ctx(root).Settings.Language, "NoWrites writes nothing");
    }

    static void BenchSkipped()
    {
        var root = NewRoot("benchskip");
        var f = Path.Combine(root, "results.json");
        File.WriteAllText(f, "{\"version\":1,\"stopped\":false,\"scenes\":[{\"name\":\"Hyrule Field\",\"avg\":60,\"low1\":50,\"frametimes_ms\":[16.6]}],\"skipped\":[\"Lake Hylia\"],\"settings\":{}}");
        var run = Benchmark.ReadResults(f, TimeSpan.FromSeconds(1));
        Equal(1, run.Scenes.Count, "the measured place");
        Equal("Lake Hylia", run.Skipped.Single(), "the place left out");
        Check(Benchmark.SkippedNote(run.Skipped).Contains("Lake Hylia was left out"), "the verdict's note");
        Equal("", Benchmark.SkippedNote(Array.Empty<string>()), "no note without one");
        File.WriteAllText(f, "{\"version\":1,\"stopped\":false,\"scenes\":[]}");
        Equal(0, Benchmark.ReadResults(f, TimeSpan.Zero).Skipped.Count, "an older game's results: none");
    }

    static void BenchSteady()
    {
        var now = new Dictionary<string, int> { [Benchmark.KSteady] = 0 };
        var uneven = new[] { new PlaceResult("Hyrule Field", 45, 28), new PlaceResult("Kokiri Forest", 58, 55) };
        var r = Benchmark.Recommend(now, uneven, null, null, 60).Single(c => c.Key == Benchmark.KSteady);
        Equal(Benchmark.SteadyRefreshSteps, r.To, "lows under 80 % of the average: Refresh steps (36 fps on 60 Hz was uneven)");
        var even = new[] { new PlaceResult("Hyrule Field", 60, 58) };
        Equal(0, Benchmark.Recommend(now, even, null, null, 60).Single(c => c.Key == Benchmark.KSteady).To, "even: the player's own");
    }

    static void LinuxUserFolder()
    {
        var root = NewRoot("linuxuser");
        var p = new LinuxPlatform(new Dictionary<string, string?> { ["TRIFORCE_TEST_ROOT"] = root });
        var game = Path.Combine(root, "game");
        var now = Path.Combine(p.DataHome, "triforce", "user");
        var old = Path.Combine(p.DataHome, "hookshot");
        Equal(now, p.UserFolder(game), "no saves anywhere: the new folder");
        Directory.CreateDirectory(Path.Combine(game, "user", "save", "1"));
        Equal(Path.Combine(game, "user"), p.UserFolder(game), "saves the game could not move out of its folder: there");
        Directory.CreateDirectory(Path.Combine(old, "save", "1"));
        Equal(old, p.UserFolder(game), "an older build's ~/.local/share/hookshot, not renamed yet: there");
        Equal(Path.Combine(old, "save", "abc"), p.SaveFolder(game, "abc"), "SaveFolder under it");
        Directory.CreateDirectory(Path.Combine(now, "save", "1"));
        Equal(now, p.UserFolder(game), "triforce/user once it has saves: first");
        Equal(Path.Combine(now, "save", "abc"), p.SaveFolder(game, "abc"), "SaveFolder under the new one");
    }

    static void SettingsIni()
    {
        var root = NewRoot("ini");
        var file = Path.Combine(root, "triforce", "builder.ini");
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        File.WriteAllText(file, "kind=nup\nnup=/home/a/oot.nup\nout=/home/a/Games/Old\nlanguage=en-US\nprepare=1\nout=/home/a/Games/Triforce\n");
        var s = new IniSettingsStore(file);
        Equal("/home/a/Games/Triforce", s.Get(SettingNames.GameFolder), "GameFolder is the last out= line (hookshot-launch)");
        s.Set(SettingNames.Language, "ja");
        s.Set(SettingNames.Dlss, "1");
        s.Set(SettingNames.SetupSkip, "v0.5.2");
        var text = File.ReadAllText(file);
        Check(text.Contains("kind=nup\n") && text.Contains("prepare=1\n") && text.Contains("nup=/home/a/oot.nup\n"), "the Tk builder's keys kept: " + text);
        Check(text.Contains("language=ja\n") && text.Contains("dlss=1\n") && text.Contains("SetupSkip=v0.5.2\n"), "written under the shared names: " + text);
        Equal(1, text.Split('\n').Count(l => l.StartsWith("out=")), "one out= line after a rewrite");
        Throws<ArgumentException>(() => s.Set("x", "a\nb"), "a line break is refused");
    }

    static void ColorParse()
    {
        Equal("#e0b23e", LauncherColor.Default, "the default is Triforce gold, the page's GOLD");
        Equal("#5e9cf0", LauncherColor.Parse("#5E9CF0"), "upper case kept as lower case");
        Equal("#0a0908", LauncherColor.Parse("#0a0908"), "lower case as it is");
        foreach (var bad in new[] { null, "", "#", "#fff", "e0b23e", "#e0b23", "#e0b23e0", "#e0b23eff", "#gggggg", " #e0b23e", "#e0b23e ",
                                     "#e0b23e\n", "##e0b23", "rgb(224,178,62)", "gold", "#e0 23e", "#" + (char)0xFF45 + "0b23e" })
            Equal(null, LauncherColor.Parse(bad), "not a #rrggbb: [" + bad + "]");

        string? Cmd(string json) => LauncherColor.FromCommand(System.Text.Json.Nodes.JsonNode.Parse(json));
        Equal("#5e9cf0", Cmd("""{"t":"cmd","c":"color","hex":"#5E9CF0"}"""), "the page's message");
        Equal("#e0b23e", Cmd("""{"t":"cmd","c":"color","hex":"#e0b23e"}"""), "Reset to gold");
        Equal(null, Cmd("""{"t":"cmd","c":"color"}"""), "no hex");
        Equal(null, Cmd("""{"t":"cmd","c":"color","hex":null}"""), "hex null");
        Equal(null, Cmd("""{"t":"cmd","c":"color","hex":14725694}"""), "hex a number");
        Equal(null, Cmd("""{"t":"cmd","c":"color","hex":true}"""), "hex a bool");
        Equal(null, Cmd("""{"t":"cmd","c":"color","hex":["#e0b23e"]}"""), "hex an array");
        Equal(null, Cmd("""{"t":"cmd","c":"color","hex":{"v":"#e0b23e"}}"""), "hex an object");
        Equal(null, Cmd("""{"t":"cmd","c":"color","hex":"#e0b23e; background:url(x)"}"""), "hex with more after it");
        Equal(null, Cmd("""{"t":"cmd","c":"color","hex":"blue"}"""), "a name");
        Equal(null, Cmd("""["#e0b23e"]"""), "a message that is not an object");
        Equal(null, LauncherColor.FromCommand(null), "no message");
    }

    static void ColorKept()
    {
        // registry.txt (Windows' values in a test run): a file from before the color has no Color line and reads as gold
        var root = NewRoot("color");
        File.WriteAllLines(Path.Combine(root, "registry.txt"), new[] { @"Software\Triforce\Language=de", @"Software\Triforce\MusicVolume=55", @"Software\Triforce\Muted=1" });
        var old = Ctx(root);
        Equal(LauncherColor.Default, old.Settings.Color, "an old settings file: the default");
        Equal("de", old.Settings.Language, "and its other values as before");
        Equal(55, old.Settings.MusicVolume, "music volume as before");
        Check(old.Settings.Muted, "muted as before");
        Check(!File.ReadAllText(Path.Combine(root, "registry.txt")).Contains("Color"), "reading writes nothing");

        old.Settings.Color = "#5E9CF0";
        Equal("#5e9cf0", Ctx(root).Settings.Color, "kept, lower case, read back by a new start");
        var lines = File.ReadAllLines(Path.Combine(root, "registry.txt"));
        Check(lines.Contains(@"Software\Triforce\Color=#5e9cf0"), "one line beside the others: " + string.Join(" / ", lines));
        Check(lines.Contains(@"Software\Triforce\Language=de") && lines.Contains(@"Software\Triforce\MusicVolume=55"), "the other values kept");

        Throws<ArgumentException>(() => old.Settings.Color = "blue", "a bad color is refused");
        Throws<ArgumentException>(() => old.Settings.Color = "#fff", "three digits are refused");
        Equal("#5e9cf0", Ctx(root).Settings.Color, "and the kept one stays");

        Ctx(root).Settings.Color = LauncherColor.Default;
        Equal(LauncherColor.Default, Ctx(root).Settings.Color, "Reset to gold kept");
        Equal(1, File.ReadAllLines(Path.Combine(root, "registry.txt")).Count(l => l.StartsWith(@"Software\Triforce\Color=")), "one line per name after a write");

        // a kept value that does not read (changed by hand) is the default, not an error
        Ctx(root).Settings.Set(SettingNames.Color, "#12345");
        Equal(LauncherColor.Default, Ctx(root).Settings.Color, "an unreadable kept value: the default");
        Ctx(root).Settings.Set(SettingNames.Color, "#ABCDEF");
        Equal("#abcdef", Ctx(root).Settings.Color, "a kept upper-case value reads as lower case");

        var snap = Ctx(root);
        snap.NoWrites = true;
        snap.Settings.Color = "#4fc3b4";
        Equal("#abcdef", Ctx(root).Settings.Color, "NoWrites writes no color");

        Ctx(root).Platform.Settings.RemoveAll();
        Equal(LauncherColor.Default, Ctx(root).Settings.Color, "settings removed (uninstall): the default");

        // builder.ini (Linux): its own key, the Tk builder's keys kept
        var iroot = NewRoot("color-ini");
        var file = Path.Combine(iroot, "triforce", "builder.ini");
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        File.WriteAllText(file, "kind=nup\nout=/home/a/Games/Triforce\nlanguage=en-US\nMusicVolume=30\n");
        var ini = new Settings(new IniSettingsStore(file), () => false);
        Equal(LauncherColor.Default, ini.Color, "an old builder.ini: the default");
        Equal(30, ini.MusicVolume, "and its volume as before");
        ini.Color = "#C77DDA";
        Equal("#c77dda", new Settings(new IniSettingsStore(file), () => false).Color, "kept in builder.ini");
        var text = File.ReadAllText(file);
        Check(text.Contains("Color=#c77dda\n") && text.Contains("kind=nup\n") && text.Contains("out=/home/a/Games/Triforce\n") && text.Contains("MusicVolume=30\n"),
            "Color= beside the Tk builder's keys: " + text);
    }

    static void ColorChoice()
    {
        static System.Text.Json.Nodes.JsonNode Msg(string hex) => System.Text.Json.Nodes.JsonNode.Parse("{\"t\":\"cmd\",\"c\":\"color\",\"hex\":\"" + hex + "\"}")!;
        var root = NewRoot("color-choice");
        Ctx(root).Settings.Color = "#5e9cf0";

        // --snapshot (NoWrites): the choice is not kept, but state.color (Current) is the player's, not the kept one
        var snap = Ctx(root);
        snap.NoWrites = true;
        var c = new LauncherColorChoice(snap.Settings);
        Equal("#5e9cf0", c.Current, "before the page sends one: the kept color");
        Equal("#c77dda", c.Take(Msg("#C77DDA")), "the page's color taken, lower case");
        c.Keep();
        Equal("#c77dda", c.Current, "NoWrites: the chosen color, so the Settings list does not set the page back");
        Equal("#5e9cf0", Ctx(root).Settings.Color, "and nothing written");
        Equal(null, c.Take(Msg("blue")), "a bad color is not taken");
        Equal("#c77dda", c.Current, "and the choice stays");

        // a 'settings.list' handled before the 'color' just ahead of it has been kept: Current is already the new color
        var ctx = Ctx(root);
        c = new LauncherColorChoice(ctx.Settings);
        c.Take(Msg("#4fc3b4"));
        Equal("#4fc3b4", c.Current, "taken as it arrives, before its worker runs");
        Equal("#5e9cf0", ctx.Settings.Color, "not kept yet");
        // two colors whose workers run the wrong way round: the newest is kept
        c.Take(Msg("#e05a5a"));
        c.Keep(); c.Keep();
        Equal("#e05a5a", Ctx(root).Settings.Color, "the newest kept");
        Equal("#e05a5a", c.Current, "and sent");
        c.Take(Msg(LauncherColor.Default));
        c.Keep();
        Equal(LauncherColor.Default, Ctx(root).Settings.Color, "Reset to gold kept");
    }

    // ---- startup ----

    static void StartArgsParsing()
    {
        var a = StartArgs.Parse(new[] { "--relaunched", "--merge" });
        Check(a.Relaunched && a.Merge && a.UpdateFolder == null, "--relaunched --merge");
        a = StartArgs.Parse(new[] { "--update", @"C:\Games\Triforce", "--relaunched" });
        Check(a.UpdateFolder == @"C:\Games\Triforce" && a.Merge && a.Relaunched, "--update sets merge");
        a = StartArgs.Parse(new[] { "--update", @"C:\Games\Triforce", "--tag", "v0.5.2" });
        Equal("v0.5.2", a.Tag, "--tag from the game");
        // (a folder ending in a backslash, quoted on a command line: \" escapes the quote)
        a = StartArgs.Parse(new[] { "--update", "C:\\Games\\Triforce\" --tag v0.5.2" });
        Equal(@"C:\Games\Triforce\", a.UpdateFolder, "the folder repaired");
        Equal("v0.5.2", a.Tag, "and the tag after it");
        a = StartArgs.Parse(new[] { "--uninstall", "--quiet", "--something-new", "x" });
        Check(a.Uninstall && a.Quiet && a.Unknown.Contains("--something-new"), "unknown arguments kept aside, not fatal");
        a = StartArgs.Parse(new[] { "--launch", "/home/a/Games/Triforce" });
        Equal("/home/a/Games/Triforce", a.LaunchFolder, "Linux --launch");
        a = StartArgs.Parse(new[] { "--update", "/g", "--tag", "v1", "--fetched" });
        Check(a.Fetched && a.UpdateFolder == "/g" && a.Tag == "v1", "Linux --update --tag --fetched");
        a = StartArgs.Parse(new[] { "--install-to", @"D:\T", "--no-desktop" });
        Check(a.InstallTo == @"D:\T" && a.NoDesktop && !a.NoStartMenu, "--install-to");
        Equal("--relaunched|--merge", string.Join("|", new StartArgs { Merge = true }.PassOn(true)), "self-update passes --relaunched --merge");
        Equal("--update|C:\\G|--relaunched", string.Join("|", new StartArgs { Merge = true, UpdateFolder = @"C:\G" }.PassOn(true)), "with the game's folder");
        Equal("--merge", string.Join("|", new StartArgs { Merge = true }.PassOn(false)), "install relaunch");
    }

    static void LockName()
    {
        var a = @"C:\Users\you\AppData\Local\Programs\Triforce";
        var name = InstanceLock.Name(a, null);
        Equal(@"Local\TriforceSetup-8f6ec578ae853054-C--USERS-YOU-APPDATA-LOCAL-PROGRAMS-TRIFORCE", name, "FNV-1a 64 + tail, as the old Setup (PowerShell reference)");
        Equal(name, InstanceLock.Name(a.ToLowerInvariant() + @"\", null), "case and a trailing slash");
        Equal(name, InstanceLock.Name(@"C:\Users\you\AppData\Local\Programs\Other\..\Triforce", null), "another way to the same folder");
        Check(name != InstanceLock.Name(a, @"E:\scratch\root"), "a test root names another lock");
        Equal(name + "-up", InstanceLock.UpName(a, null), "the handover sign");
        var tail = InstanceLock.Name(@"C:\" + new string('x', 400), null);
        Check(tail.Length < 200 && tail.EndsWith(new string('X', 60)), "the tail is the last 60 characters");
    }

    static void StartupInstalled()
    {
        var root = NewRoot("startup");
        var dir = Path.Combine(root, "Programs", "Triforce");
        Directory.CreateDirectory(dir);
        var exe = Path.Combine(dir, BuildInfo.SetupAsset);
        File.WriteAllText(exe, "installed");
        File.WriteAllText(Path.Combine(dir, "TriforceSetup.old.exe"), "replaced one");
        var ctx = Ctx(root, exe);
        ctx.Platform.Settings.SetUninstall("InstallLocation", dir);
        Check(ctx.RunningInstalled, "running installed");
        var r = Startup.Run(ctx, new[] { "--relaunched", "--merge" });
        try
        {
            Equal(StartScreen.Check, r.Screen, "an installed run checks for updates");
            Check(r.Args.Merge && r.SignalUpWhenShown, "merge carried, up sign asked");
            Check(!File.Exists(Path.Combine(dir, "TriforceSetup.old.exe")), "TriforceSetup.old.exe removed");
            Check(r.Lock is { Held: true }, "the lock held");
            var second = Startup.Run(Ctx(root, exe), Array.Empty<string>());
            Equal(StartScreen.Exit, second.Screen, "a second start ends");
            Equal(0, second.ExitCode, "after asking the first to the front");
            var un = Startup.Run(Ctx(root, exe), new[] { "--uninstall", "--quiet" });
            Check(un.Screen == StartScreen.Exit && un.ExitCode == 1 && un.Message == null, "uninstall refused while open, quietly");
        }
        finally { r.Lock?.Dispose(); }
        var after = Startup.Run(Ctx(root, exe), Array.Empty<string>());
        try { Equal(StartScreen.Check, after.Screen, "the lock is free again after Release"); }
        finally { after.Lock?.Dispose(); }
    }

    static void StartupNotInstalled()
    {
        var root = NewRoot("notinstalled");
        var ctx = Ctx(root, Path.Combine(root, "Downloads", BuildInfo.SetupAsset));
        var r = Startup.Run(ctx, Array.Empty<string>());
        try { Equal(StartScreen.Install, r.Screen, "a download that is not installed shows Install"); }
        finally { r.Lock?.Dispose(); }
        var u = Startup.Run(ctx, new[] { "--uninstall" });
        try { Equal(StartScreen.Uninstall, u.Screen, "uninstall goes on when nothing is open"); }
        finally { u.Lock?.Dispose(); }
        // (installed elsewhere, started from a download: the download becomes the installed launcher; here that copy cannot start,
        // so this one carries on)
        var dir = Path.Combine(root, "Programs", "Triforce");
        var self = Path.Combine(root, "Downloads", BuildInfo.SetupAsset);
        Directory.CreateDirectory(dir);
        Directory.CreateDirectory(Path.GetDirectoryName(self)!);
        File.WriteAllText(self, "downloaded copy, no program");
        File.WriteAllText(Path.Combine(dir, BuildInfo.SetupAsset), "installed copy");
        var c2 = Ctx(root, self);
        c2.Platform.Settings.SetUninstall("InstallLocation", dir);
        var r2 = Startup.Run(c2, new[] { "--merge" });
        try
        {
            Equal("downloaded copy, no program", File.ReadAllText(Path.Combine(dir, BuildInfo.SetupAsset)), "the download was copied over the installed copy");
            Equal(StartScreen.Check, r2.Screen, "and, the installed copy not starting, this one carries on");
        }
        finally { r2.Lock?.Dispose(); }
    }

    static void InstallUninstall()
    {
        var root = NewRoot("install");
        var self = Path.Combine(root, "Downloads", BuildInfo.SetupAsset);
        Directory.CreateDirectory(Path.GetDirectoryName(self)!);
        File.WriteAllText(self, "launcher bytes");
        var ctx = Ctx(root, self);
        var inst = new Installer(ctx);
        Equal(Path.Combine(root, "Programs", "Triforce"), inst.DefaultDir, "default install folder under the test root's LocalAppData");
        var exe = inst.Run(inst.DefaultDir, desktop: true, startMenu: true);
        Equal("launcher bytes", File.ReadAllText(exe), "copied");
        Equal(inst.DefaultDir, ctx.InstalledDir, "registered (InstallLocation)");
        Equal("\"" + exe + "\" --uninstall --quiet", ctx.Platform.Settings.GetUninstall("QuietUninstallString"), "quiet uninstall string");
        Check(File.Exists(Path.Combine(root, "Desktop", "Triforce.lnk")), "desktop shortcut in the test root");
        Check(File.Exists(Path.Combine(root, "Start Menu", "Triforce", "Triforce.lnk")), "start menu shortcut in the test root");
        Throws<InvalidOperationException>(() => inst.Run(Path.Combine(Path.GetTempPath(), "elsewhere-" + Guid.NewGuid().ToString("N")), false, false), "outside the test root refused");
        Directory.CreateDirectory(Path.Combine(root, "build", "objcache"));
        ctx.Settings.GameFolder = Path.Combine(root, "Games", "Triforce");
        var plan = new Uninstaller(ctx).Describe();
        Check(plan.Keeps.Any(k => k.Contains("saves")) && plan.Removes.Count >= 3, "the plan says what goes and stays");
        new Uninstaller(ctx).Run();
        Check(!File.Exists(Path.Combine(root, "Desktop", "Triforce.lnk")) && !Directory.Exists(Path.Combine(root, "Start Menu", "Triforce")), "shortcuts gone");
        Check(!Directory.Exists(Path.Combine(root, "build")), "work folder gone");
        Equal(null, Ctx(root, self).Platform.Settings.GetUninstall("InstallLocation"), "uninstall entry gone");
        Equal(null, Ctx(root, self).Settings.GameFolder, "settings gone");
    }

    // ---- report, languages ----

    static void ReportNames()
    {
        var text = @"C:\Users\Ann\AppData\x and c:/users/ann//Games and C:\Users\Anna\y and C:\Users\ANN~1\Temp";
        var o = Report.NoName(text, @"C:\Users\Ann", @"C:\Users\ANN~1");
        Equal(@"%USERPROFILE%\AppData\x and %USERPROFILE%//Games and C:\Users\Anna\y and %USERPROFILE%\Temp", o, "the account's folder read as %USERPROFILE%");
        Equal("b\nc", Report.Tail("a\nb\nc\n\n", 2).Replace("\r", ""), "tail");
    }

    static void LanguagesTest()
    {
        Equal(16, Languages.All.Count, "16 codes");
        Equal("zh-Hant", Languages.Default("zh-TW").Code, "zh-TW");
        Equal("zh-Hans", Languages.Default("zh-CN").Code, "zh-CN");
        Equal("es-419", Languages.Default("es-MX").Code, "es-MX");
        Equal("es", Languages.Default("es-ES").Code, "es-ES");
        Equal("pt-BR", Languages.Default("pt-PT").Code, "pt-PT");
        Equal("de", Languages.Default("de-AT").Code, "de-AT");
        Equal("en-US", Languages.Default("xx-YY").Code, "unknown");
        Equal("fr-CA", Languages.Default("fr-CA").Code, "exact");
    }
}
