using System.Text;
using System.Text.RegularExpressions;
using Triforce.Logic;
using static Triforce.LogicTests.T;

namespace Triforce.LogicTests;

static partial class Tests
{
    // (READ ONLY: the owner's own game folder. Nothing here writes there; the tests that write work on a copy in a test root)
    const string RealGame = @"D:\src\triforce-game";

    public static IEnumerable<(string Name, Func<Task> Run)> GameOptionsTests()
    {
        (string, Func<Task>) Sync(string n, Action a) => (n, () => { a(); return Task.CompletedTask; });
        yield return Sync("game options: schema matches vk_menu.cpp's table", GameOptionsMatchMenu);
        yield return Sync("game options: keys, labels, choices, no Hookshot", GameOptionsLabels);
        yield return Sync("game options: the real triforce_options.ini (read only)", GameOptionsRealFile);
        yield return Sync("game options: round trip, merge, atomic write (a copy)", GameOptionsWrite);
        yield return Sync("game options: Set's rules, Quality preset, Apply", GameOptionsSetRules);
        yield return Sync("game options: an older options_version, a new file", GameOptionsOldVersion);
    }

    // ---- the schema against the game's source ----

    /// <summary>gpu/vk of this source tree (the launcher's own repository), found from this file's folder up, else from the test
    /// program's.</summary>
    static string? MenuSource([System.Runtime.CompilerServices.CallerFilePath] string here = "")
    {
        foreach (var start in new[] { Path.GetDirectoryName(here), AppContext.BaseDirectory })
            for (var d = start is { Length: > 0 } ? new DirectoryInfo(start) : null; d != null; d = d.Parent)
                if (File.Exists(Path.Combine(d.FullName, "gpu", "vk", "vk_menu.cpp"))) return Path.Combine(d.FullName, "gpu", "vk");
        return null;
    }

    sealed record MenuRow(string Tab, string Name, string Value, string Lo, string Hi, string Step, string Text, string? Help, bool Restart, bool Exempt, bool Heading);

    /// <summary>vk_menu.cpp's rows[] table, each entry's fields (comments out, adjacent string literals joined).</summary>
    static List<MenuRow> ReadMenuTable(string cpp)
    {
        var body = cpp[cpp.IndexOf("Row rows[] = {", StringComparison.Ordinal)..cpp.IndexOf("constexpr int kRowsEnd", StringComparison.Ordinal)];
        var sb = new StringBuilder();
        for (int i = 0, inStr = 0; i < body.Length; i++)
        {
            var c = body[i];
            if (inStr == 1)
            {
                sb.Append(c);
                if (c == '\\') { sb.Append(body[++i]); continue; }
                if (c == '"') inStr = 0;
                continue;
            }
            if (c == '"') { inStr = 1; sb.Append(c); continue; }
            if (c == '/' && i + 1 < body.Length && body[i + 1] == '/') { while (i < body.Length && body[i] != '\n') i++; sb.Append('\n'); continue; }
            sb.Append(c);
        }
        body = sb.ToString();
        var rows = new List<MenuRow>();
        foreach (Match m in Regex.Matches(body, @"\{(TAB_[A-Z]+),"))
        {
            int k = m.Index + m.Length, depth = 1;
            var inS = false;
            var fields = new List<string>();
            var cur = new StringBuilder();
            for (; depth > 0; k++)
            {
                var c = body[k];
                if (inS)
                {
                    cur.Append(c);
                    if (c == '\\') { cur.Append(body[++k]); continue; }
                    if (c == '"') inS = false;
                    continue;
                }
                if (c == '"') { inS = true; cur.Append(c); }
                else if (c == '{') { depth++; cur.Append(c); }
                else if (c == '}') { if (--depth > 0) cur.Append(c); }
                else if (c == ',') { fields.Add(cur.ToString().Trim()); cur.Clear(); }
                else cur.Append(c);
            }
            if (cur.ToString().Trim().Length > 0) fields.Add(cur.ToString().Trim());
            string? Lit(int i) => i < fields.Count && fields[i].StartsWith('"')
                ? string.Concat(Regex.Matches(fields[i], "\"((?:[^\"\\\\]|\\\\.)*)\"").Select(x => x.Groups[1].Value)).Replace("\\\"", "\"").Replace("\\\\", "\\")
                : null;
            string F(int i, string dflt) => i < fields.Count ? fields[i] : dflt;
            rows.Add(new MenuRow(m.Groups[1].Value, Lit(0) ?? fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], Lit(6),
                F(8, "false") == "true", F(9, "false") == "true", F(11, "false") == "true"));
        }
        return rows;
    }

    /// <summary>vk_menu.h's Options defaults, by field.</summary>
    static Dictionary<string, int> ReadOptionDefaults(string h)
    {
        var s = h[h.IndexOf("struct Options {", StringComparison.Ordinal)..h.IndexOf("extern Options opts;", StringComparison.Ordinal)];
        var d = new Dictionary<string, int>();
        foreach (var line in s.Split('\n'))
        {
            var code = line.Split("//")[0];
            var m = Regex.Match(code, @"^\s*int\s+(.*);");
            if (!m.Success) continue;
            foreach (var part in m.Groups[1].Value.Split(','))
            {
                var kv = part.Split('=');
                d[kv[0].Trim()] = int.Parse(kv[1].Trim());
            }
        }
        return d;
    }

    static void GameOptionsMatchMenu()
    {
        var dir = MenuSource();
        Check(dir != null, "gpu/vk/vk_menu.cpp is found above the test program");
        var table = ReadMenuTable(File.ReadAllText(Path.Combine(dir!, "vk_menu.cpp")));
        var defaults = ReadOptionDefaults(File.ReadAllText(Path.Combine(dir!, "vk_menu.h")));
        Check(table.Count > 100, "the table read: " + table.Count + " entries");
        var symbols = new Dictionary<string, int>
        {
            ["kLastOutput"] = 6, ["ur::kValues - 1"] = 6, ["ur::kUpscalers - 1"] = 6, ["ur::kModels - 1"] = 3, ["aa_rows::kValues - 1"] = 3,
            ["2 + kMaxMics"] = 18, ["HOS_PROMPT_N"] = 6,
        };
        int Num(string s, string what) => int.TryParse(s, out var v) ? v : symbols.TryGetValue(s, out v) ? v : throw new TestFailed(what + ": a symbol the test does not know, " + s);
        var tabs = new Dictionary<string, GameOptionTab>
        {
            ["TAB_DISPLAY"] = GameOptionTab.Display, ["TAB_GRAPHICS"] = GameOptionTab.Graphics, ["TAB_AUDIO"] = GameOptionTab.Audio,
            ["TAB_CONTROLS"] = GameOptionTab.Controls, ["TAB_SYSTEM"] = GameOptionTab.System, ["TAB_MOTION"] = GameOptionTab.Controls,
            ["TAB_PAGE"] = GameOptionTab.Controls,
        };
        var left = new[] { "&opts.language", "&opts.pad_layout" };   // (Game language: language.txt's; Layout preset: KeptKeys)
        var sections = new Dictionary<string, string>();
        var names = new List<string>();
        foreach (var r in table)
        {
            if (r.Heading) { sections[r.Tab] = r.Name; continue; }
            if (r.Value == "nullptr" || r.Tab == "TAB_DEBUG" || left.Contains(r.Value)) continue;
            var row = GameOptionsSchema.ByName(r.Name);
            Check(row != null, "the menu's row \"" + r.Name + "\" is in the schema");
            names.Add(r.Name);
            Equal(tabs[r.Tab], row!.Tab, r.Name + ": tab");
            var section = r.Tab == "TAB_MOTION" ? "Motion" : r.Tab == "TAB_PAGE" ? "Controls" : sections.GetValueOrDefault(r.Tab);
            Equal(section, row.Section, r.Name + ": section");
            Equal(Num(r.Lo, r.Name), row.Lo, r.Name + ": lo");
            Equal(Num(r.Hi, r.Name), row.Hi, r.Name + ": hi");
            Equal(Num(r.Step, r.Name), row.Step, r.Name + ": step");
            Equal(r.Help, row.Help, r.Name + ": help");
            Equal(r.Restart, row.Restart, r.Name + ": restart");
            Equal(r.Exempt, row.PresetExempt, r.Name + ": preset exempt");
            Equal(defaults[r.Value["&opts.".Length..]], row.Default, r.Name + ": default");
        }
        Equal(string.Join("|", names), string.Join("|", GameOptionsSchema.All.Select(r => r.Name)), "the rows, in the menu's order");
        // (the Debug tab's keys, and the Layout preset's, are the KeptKeys: each with its reason)
        foreach (var r in table.Where(r => r.Value != "nullptr" && !r.Heading && (r.Tab == "TAB_DEBUG" || r.Value == "&opts.pad_layout")))
            Check(GameOptionsSchema.KeptKeys.ContainsKey(GameOptionsSchema.IniKey(r.Name)), r.Name + "'s key " + GameOptionsSchema.IniKey(r.Name) + " is a kept key");
    }

    // ---- keys and labels ----

    static void GameOptionsLabels()
    {
        var all = GameOptionsSchema.All;
        Equal(all.Count, all.Select(r => r.Key).Distinct().Count(), "keys are unique");
        Check(all.All(r => r.Key.Length <= 31 && !r.Key.Contains(' ')), "keys as ini_key makes them");
        Equal("frame_rate_cap", GameOptionsSchema.Get("frame_rate_cap").Key, "renamed row keeps its key");
        Equal("Game frame rate cap", GameOptionsSchema.Get("frame_rate_cap").Name, "by key");
        Equal("amd_anti-lag_2", GameOptionsSchema.ByName("AMD Anti-Lag 2")!.Key, "AMD Anti-Lag 2's key");
        Equal("upscaling", GameOptionsSchema.ByName("Window scaling filter")!.Key, "Window scaling filter's key");
        Equal("tail_early", GameOptionsSchema.IniKey("Start next frame early"), "Start next frame early's key");
        Equal("export_save_for_a_bug_report", GameOptionsSchema.IniKey("Export save for a bug report"), "a long name: 28 letters");
        Equal("abcdefghijklmnopqrstuvwxyzabcde", GameOptionsSchema.IniKey("ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGH"), "cut at 31");
        Equal(null, GameOptionsSchema.ByKey("Fullscreen"), "keys are exact");
        Throws<KeyNotFoundException>(() => GameOptionsSchema.Get("nope"), "an unknown key");

        foreach (var r in all)
        {
            foreach (var text in new[] { r.Name, r.Help }.Concat(r.Choices.Select(c => c.Label)).Concat(new[] { r.Label(r.Lo), r.Label(r.Hi), r.Label(r.Default) }))
                Check(!text.Contains("hookshot", StringComparison.OrdinalIgnoreCase), r.Name + ": no Hookshot in \"" + text + "\"");
            Check(r.Help.Length > 10, r.Name + ": has its help");
            Check(r.Default >= r.Lo && r.Default <= r.Hi, r.Name + ": default in range");
            Equal(r.Default, r.Clamp(r.Default), r.Name + ": default on its steps");
            if (r.Kind == GameOptionKind.Choice)
            {
                Check(r.Key == GameOptionsSchema.GpuKey || r.Choices.Count >= 2, r.Name + ": choices");
                Check(r.Key == GameOptionsSchema.GpuKey || r.Choices.Any(c => c.Value == r.Default), r.Name + ": its default is a choice");
                Equal(r.Choices.Count, r.Choices.Select(c => c.Label).Distinct().Count(), r.Name + ": labels differ");
            }
            else Equal(0, r.Choices.Count, r.Name + ": a slider has no choice list");
        }
        Equal(1, all.Count(r => r.Tab == GameOptionTab.Graphics && r.Key == GameOptionsSchema.PresetKey), "the preset row");
        Equal("Screen,Widescreen,Frame rate,High dynamic range,RenoDX", string.Join(",", GameOptionsSchema.Sections(GameOptionTab.Display)), "Display's sections");
        Equal("Quality,Upscaling and edges,Smoothness,Latency,Advanced", string.Join(",", GameOptionsSchema.Sections(GameOptionTab.Graphics)), "Graphics' sections");
        Equal("Camera,Sticks,Movement,Vibration,Other,Motion,Controls", string.Join(",", GameOptionsSchema.Sections(GameOptionTab.Controls)), "Controls' sections");
        Equal(0, GameOptionsSchema.Sections(GameOptionTab.Audio).Count, "Audio has none");

        string L(string key, int v) => GameOptionsSchema.Get(key).Label(v);
        string Cs(string key) => string.Join("|", GameOptionsSchema.Get(key).Choices.Select(c => c.Value + "=" + c.Label));
        Equal("-3=30|-2=40|-1=45|0=60 (the console's)|1=90|2=120|3=144|4=165|5=240|6=Monitor's rate", Cs("frame_rate_cap"), "fps cap");
        Equal("3=1920x1080|0=2048x1152|1=2560x1440|2=3840x2160|4=5120x1440|6=5120x2160|5=5120x2880", Cs("output_res"), "output sizes, smallest first");
        Equal("0=Game|1=NVIDIA DLSS|2=NVIDIA DLAA|3=AMD FSR 3.1|6=AMD FSR 3.1 Native AA|4=AMD FSR 4|5=AMD FSR 4 Native AA", Cs("dlss"), "upscalers in the menu's order");
        Equal("0=Auto (dynamic)|1=100% (2048x1152)|2=75% (1536x864)|3=67% (1366x768)|4=50% (1024x576)", Cs("resolution"), "render resolution, the game's own");
        Equal("0=Custom|1=Minimum (6GB)|2=Low (8GB)|3=Balanced (12GB)|4=High (16GB)|5=Ultra (20GB+)", Cs("quality_preset"), "presets");
        Equal("0=Off|1=Default", Cs("microphone"), "microphone without devices");
        Equal("0=Game SMAA|1=Off|2=FXAA|3=SMAA Ultra", Cs("anti-aliasing"), "anti-aliasing");
        Equal("0=Off|1=Auto|2=2x|3=3x|4=4x|5=5x|6=6x|7=7x|8=8x|9=AMD FSR 4", Cs("frame_generation"), "frame generation");
        Equal("0=Auto|1=Switch|2=Xbox|3=PlayStation 4|4=PlayStation 5|5=Steam Deck|6=Keyboard & mouse", Cs("button_prompts"), "prompts");
        Equal("0=Auto|1=Off|2=On", Cs("robust_buffers"), "robust");
        Equal("0=Insert or F1|1=F1 only", Cs("menu_shortcut"), "menu shortcut");
        Equal("1.25", L("mouse_sensitivity", 125), "sensitivity");
        Equal("0.51", L("walk_scale", 51), "walk scale");
        Equal("10000 nits", L("hdr_peak_brightness", 10000), "nits");
        Equal("Muted", L("master_volume", 0), "volume 0");
        Equal("85%", L("master_volume", 85), "volume");
        Equal("Off", L("menu_sounds", 0), "menu sounds 0");
        Equal("Off", L("rumble_strength", 0), "rumble 0");
        Equal("As Stick dead zone", L("left_stick_dead_zone", -1), "own dead zone");
        Equal("0%", L("right_stick_dead_zone", 0), "own dead zone 0");
        Equal("50", L("renodx_bloom", 50), "grade");
        Equal("Default (24%)", L("stick_dead_zone", 0), "stick dead zone");
        Equal("Off", L("upscaling", 7), "past the row: its first");
        Equal("16:9", L("ultrawide", 0), "ultrawide");

        // (the GPU device row: the names the launcher gives it)
        var before = GameOptionsSchema.GpuNames;
        try
        {
            GameOptionsSchema.GpuNames = null;
            Equal("Auto", L("gpu_device", 0), "gpu 0");
            Equal("1: Device 1", L("gpu_device", 2), "gpu without names");
            Equal("0=Auto", Cs("gpu_device"), "gpu choices without names");
            GameOptionsSchema.GpuNames = () => new[] { "NVIDIA GeForce RTX 4070", "AMD Radeon(TM) Graphics" };
            Equal("0=Auto|1=0: NVIDIA GeForce RTX 4070|2=1: AMD Radeon(TM) Graphics", Cs("gpu_device"), "gpu choices");
            Equal("0: NVIDIA GeForce RTX 4070", L("gpu_device", 1), "gpu 1");
            GameOptionsSchema.UseSystemGpus();   // (not asked here: the tests do not read this PC's registry)
            Check(GameOptionsSchema.GpuNames != null, "the hook is set");
            // ((Triforce) 11-19) the game's own order, from its triforce_gpus.txt: Windows lists the Intel first on that laptop,
            // Vulkan the NVIDIA, and the row's values are Vulkan's
            var g = NewRoot("gpulist");
            File.WriteAllText(Path.Combine(g, GameOptionsSchema.GpuListFile), "1\tNVIDIA GeForce RTX 3050 6GB Laptop GPU\n2\tIntel(R) UHD Graphics 770\n");
            GameOptionsSchema.UseGameGpus(() => g);
            Equal("0: NVIDIA GeForce RTX 3050 6GB Laptop GPU", L("gpu_device", 1), "the game's first card is the row's 1");
            Equal("1: Intel(R) UHD Graphics 770", L("gpu_device", 2), "and its second the row's 2");
            File.WriteAllText(Path.Combine(g, GameOptionsSchema.GpuListFile), "garbage\n3\tout of order\n");
            GameOptionsSchema.UseGameGpus(() => g);
            Check(GameOptionsSchema.GpuNames!() != null, "a file with nothing usable: Windows' list (no throw)");
            // ((Triforce) 11-20) Save writes the card's name with its number (gpu_device_name: the game finds it by the name
            // first), and an empty one for Auto
            GameOptionsSchema.GpuNames = () => new[] { "AMD Radeon(TM) 860M Graphics", "NVIDIA GeForce RTX 5060 Laptop GPU" };
            Equal("NVIDIA GeForce RTX 5060 Laptop GPU", GameOptionsSchema.GpuNameOf(2), "the name of the row's 2");
            Equal(null, GameOptionsSchema.GpuNameOf(0), "Auto has no name");
            var nf = NewRoot("gpuname");
            var opt = GameOptionsFile.Load(nf);
            opt.Set(GameOptionsSchema.GpuKey, 2);
            opt.Save();
            var text = File.ReadAllText(Path.Combine(nf, GameOptionsSchema.FileName));
            Check(text.Contains("gpu_device=2") && text.Contains("gpu_device_name=NVIDIA GeForce RTX 5060 Laptop GPU"),
                  "the number and the name saved: " + text.Replace("\n", " | "));
            opt.Set(GameOptionsSchema.GpuKey, 0);
            opt.Save();
            text = File.ReadAllText(Path.Combine(nf, GameOptionsSchema.FileName));
            Check(text.Contains("gpu_device=0") && text.Contains("gpu_device_name=\r\n") || text.Contains("gpu_device_name=\n"),
                  "Auto: the name emptied: " + text.Replace("\n", " | "));
        }
        finally { GameOptionsSchema.GpuNames = before; }

        Equal(6, GameOptionsSchema.PresetSummaries.Count, "a summary for each preset");
        Equal(0, GameOptionsSchema.PresetValues(0).Count, "Custom sets nothing");
    }

    // ---- the real file ----

    static void GameOptionsRealFile()
    {
        var path = Path.Combine(RealGame, GameOptionsSchema.FileName);
        Check(File.Exists(path), "the real options file is there: " + path);
        var info = new FileInfo(path);
        var (len, time) = (info.Length, info.LastWriteTimeUtc);
        var f = GameOptionsFile.Load(RealGame);
        Check(f.Exists && f.Version == GameOptionsSchema.CurrentVersion, "read, version " + f.Version);
        // (every key is a row or a kept one: these, knowingly. options_version: the format; menu_size: the old Menu size row;
        // layout_preset: rebinds the pad_ lines in the game; the Debug tab's; pad_* and key_*: the Controls page's bindings)
        Equal("", string.Join(",", f.UnknownKeys), "no key nobody knows");
        var kept = f.OtherLines.Select(o => o.Key).Where(k => !k.StartsWith("pad_") && !k.StartsWith("key_")).ToList();
        foreach (var k in kept) Check(GameOptionsSchema.KeptKeys.ContainsKey(k), k + " is a kept key");
        Check(f.OtherLines.Any(o => o.Key.StartsWith("key_")), "the bindings are other lines");
        foreach (var r in GameOptionsSchema.All)
        {
            var v = f.Get(r.Key);
            var label = f.Label(r.Key);
            Check(label.Length > 0 && !label.Contains("Device"), r.Name + ": label \"" + label + "\"");
            if (r.Kind == GameOptionKind.Choice)
                Check(f.ChoicesOf(r.Key).Any(c => c.Value == v && c.Label == label), r.Name + ": " + v + " = \"" + label + "\" is one of its choices");
            else
                Check(Regex.IsMatch(label, @"^(-?\d+( nits|%)?|\d\.\d\d|Off|Muted|As Stick dead zone)$"), r.Name + ": a slider's label \"" + label + "\"");
            Check(f.Raw(r.Key) == null || v == r.AsRead(GameOptionsFile.Atoi(f.Raw(r.Key)!)), r.Name + ": the file's value as the game reads it");
        }
        info.Refresh();
        Equal(len, info.Length, "the real file is not written");
        Equal(time, info.LastWriteTimeUtc, "the real file's time is the same");
        Check(!File.Exists(path + ".new"), "nothing beside it");
    }

    /// <summary>The real file's lines as of 11-19 (version 7, CRLF), for the exact checks: the real one changes as he plays.</summary>
    const string OptionsFixture = "fullscreen=0\r\noutput_res=1\r\ngpu_device=0\r\nultrawide=0\r\nultrawide_hud=0\r\nultrawide_menus=1\r\nframe_rate_cap=0\r\n" +
        "fps_counter=1\r\nhdr=0\r\nhdr_paper_white=400\r\nhdr_peak_brightness=4000\r\nrenodx_midgray=2\r\nrenodx_color_space=2\r\nquality_preset=0\r\n" +
        "resolution=1\r\ntexture_detail=1\r\ndlss=0\r\ndlss_model=1\r\nanti-aliasing=1\r\nsharpness=50\r\nupscaling=2\r\nframe_pacing=1\r\nnvidia_reflex=0\r\n" +
        "robust_buffers=1\r\nmicrophone=1\r\nmouse_sensitivity=125\r\ncontroller_in_background=1\r\nlayout_preset=1\r\nbutton_prompts=2\r\ndebug_tools=0\r\n" +
        "replay_helper_threads=0\r\ntail_early=1\r\noptions_version=7\r\nmenu_size=1\r\npad_a=0\r\nkey_a=69,0\r\nkey_cam_right=39,76\r\n";

    static string NewGameFolder(string name, string content)
    {
        var game = Path.Combine(NewRoot(name), "game");
        Directory.CreateDirectory(game);
        File.WriteAllText(Path.Combine(game, GameOptionsSchema.FileName), content, Encoding.Latin1);
        return game;
    }

    static void GameOptionsWrite()
    {
        // (a copy of the real file, in a test root)
        var root = NewRoot("game-options");
        var game = Path.Combine(root, "game");
        Directory.CreateDirectory(game);
        var path = Path.Combine(game, GameOptionsSchema.FileName);
        File.Copy(Path.Combine(RealGame, GameOptionsSchema.FileName), path);
        var original = File.ReadAllBytes(path);
        var f = GameOptionsFile.Load(game);
        f.Save();
        Check(File.ReadAllBytes(path).SequenceEqual(original), "nothing changed: nothing written");
        f.Set("hdr", f.Get("hdr"));
        Equal(0, f.Changes.Count, "set to what it is: no change");
        f.Save();
        Check(File.ReadAllBytes(path).SequenceEqual(original), "still the same bytes");

        // (one row changed: that line alone)
        var sens = f.Get("mouse_sensitivity") == 200 ? 300 : 200;
        f.Set("mouse_sensitivity", sens);
        f.Save();
        var before = Encoding.Latin1.GetString(original).Split("\r\n");
        var after = File.ReadAllText(path, Encoding.Latin1).Split("\r\n");
        Equal(before.Length, after.Length, "as many lines, CRLF kept");
        for (var i = 0; i < before.Length; i++)
            Equal(before[i].StartsWith("mouse_sensitivity=") ? "mouse_sensitivity=" + sens : before[i], after[i], "line " + i);
        Equal(sens, GameOptionsFile.Load(game).Get("mouse_sensitivity"), "read back");
        Check(!File.Exists(path + ".new"), "no .new left");

        // (exact labels on the fixture)
        var fx = GameOptionsFile.Load(NewGameFolder("game-options-fixture", OptionsFixture));
        string Lb(string k) => fx.Label(k);
        Equal("2560x1440", Lb("output_res"), "output");
        Equal("100% (2560x1440)", Lb("resolution"), "render resolution at 2560x1440");
        Equal("1.25", Lb("mouse_sensitivity"), "mouse sensitivity 125");
        Equal("4000 nits", Lb("hdr_peak_brightness"), "peak");
        Equal("400 nits", Lb("hdr_paper_white"), "paper white");
        Equal("Off", Lb("anti-aliasing"), "aa");
        Equal("Xbox", Lb("button_prompts"), "prompts");
        Equal("On", Lb("fps_counter"), "fps counter");
        Equal("DCI-P3", Lb("renodx_color_space"), "colour space");
        Equal("Eye adaptation", Lb("renodx_midgray"), "midgray");
        Equal("Transformer 2 (M)", Lb("dlss_model"), "dlss model");
        Equal("Lanczos", Lb("upscaling"), "filter");
        Equal("50%", Lb("sharpness"), "sharpness");
        Equal("Off", Lb("robust_buffers"), "robust 1");
        Equal("Default", Lb("microphone"), "microphone");
        Equal("Full", Lb("texture_detail"), "texture detail");
        Equal("Off", Lb("nvidia_reflex"), "reflex 0");
        Equal("Per frame", GameOptionsSchema.Get("game_speed").Label(0), "game speed 0");
        Equal("Real time", Lb("game_speed"), "game speed: its default without a line");
        Equal("80%", Lb("menu_sounds"), "menu sounds' default");
        Equal("Middle", Lb("ultrawide_hud"), "wide HUD");
        Equal("Centred, blurred sides", Lb("ultrawide_menus"), "wide menus");
        Equal("layout_preset,debug_tools,replay_helper_threads,tail_early,options_version,menu_size,pad_a,key_a,key_cam_right",
            string.Join(",", fx.OtherLines.Select(o => o.Key)), "other lines in the file's order");
        Equal("69,0", fx.OtherLines.Single(o => o.Key == "key_a").Value, "a binding's value as it is");
        fx.Set("dlss", 1);
        Equal("Quality (67%)", fx.Label("resolution"), "with DLSS: its mode (100% moved to Quality)");
        Equal("DLSS's own", fx.Label("anti-aliasing"), "aa with DLSS");
        Equal("0,3,5,4,6", string.Join(",", fx.ChoicesOf("resolution").Select(c => c.Value)), "DLSS's steps");
        fx.Set("dlss", 2);
        Equal("Native (DLAA)", fx.Label("resolution"), "DLAA");
        fx.Set("dlss", 3);
        Equal("FSR's own", fx.Label("anti-aliasing"), "aa with FSR");
        fx.Set("dlss", 0);
        fx.Set("handheld_profile", 1);
        Equal("67% (1024x576)", fx.Label("resolution"), "handheld profile's size (100% went to DLSS's Quality, which stays with the game's own)");

        // (the merge: a line changed outside since Load stays; the launcher's own change goes in; an unknown line stays in place)
        var m = NewGameFolder("game-options-merge", OptionsFixture);
        var mp = Path.Combine(m, GameOptionsSchema.FileName);
        var a = GameOptionsFile.Load(m);
        a.Set("fullscreen", 1);
        a.Set("sharpness", 70);
        var outside = File.ReadAllText(mp, Encoding.Latin1).Replace("hdr=0\r\n", "hdr=2\r\nzz_tool_line=5\r\n").Replace("sharpness=50", "sharpness=20");
        File.WriteAllText(mp, outside, Encoding.Latin1);
        a.Save();
        var merged = File.ReadAllText(mp, Encoding.Latin1);
        Check(merged.Contains("\r\nhdr=2\r\nzz_tool_line=5\r\n"), "the outside change and its new line kept in place");
        Check(merged.StartsWith("fullscreen=1\r\n"), "the launcher's change in its line");
        Check(merged.Contains("\r\nsharpness=70\r\n"), "a row changed on both sides: the launcher's (the later writer)");
        Equal(outside.Replace("fullscreen=0", "fullscreen=1").Replace("sharpness=20", "sharpness=70"), merged, "every other line as the file had it");
        Equal("zz_tool_line", a.UnknownKeys.Single(), "an unknown key, kept");
        Equal(2, a.Get("hdr"), "this object holds the file as written");

        // (a row with no line goes in before options_version; duplicates all take the value)
        var d = NewGameFolder("game-options-dup", "sharpness=10\nsharpness=20\n#note\noptions_version=7\nmenu_size=1\n");
        var df = GameOptionsFile.Load(d);
        Equal(20, df.Get("sharpness"), "the last line wins, as the game reads it");
        df.Set("sharpness", 30);
        df.Set("vsync", 0);
        df.Save();
        Equal("sharpness=30\nsharpness=30\n#note\nvsync=0\noptions_version=7\nmenu_size=1\n", File.ReadAllText(Path.Combine(d, GameOptionsSchema.FileName)), "LF kept, line added, comment kept");

        // (a device's name in UTF-8: its bytes as they were, its label as text)
        var mic = "microphone_device=Mikrofon (Ä USB)";
        var micBytes = Encoding.UTF8.GetBytes("microphone=2\r\n" + mic + "\r\noptions_version=7\r\n");
        var mg = Path.Combine(NewRoot("game-options-mic"), "game");
        Directory.CreateDirectory(mg);
        File.WriteAllBytes(Path.Combine(mg, GameOptionsSchema.FileName), micBytes);
        var mf = GameOptionsFile.Load(mg);
        Equal("Mikrofon (Ä USB)", mf.Label("microphone"), "the device's name");
        Equal("0=Off|1=Default|2=Mikrofon (Ä USB)", string.Join("|", mf.ChoicesOf("microphone").Select(c => c.Value + "=" + c.Label)), "its choices");
        mf.Set("fullscreen", 1);
        mf.Save();
        var micAfter = File.ReadAllBytes(Path.Combine(mg, GameOptionsSchema.FileName));
        Check(micAfter.AsSpan().IndexOf(Encoding.UTF8.GetBytes(mic)) >= 0, "the name's UTF-8 bytes kept");

        // (atomic: the .new cannot be made, the file is as it was and the error is the caller's)
        var w = NewGameFolder("game-options-atomic", OptionsFixture);
        var wp = Path.Combine(w, GameOptionsSchema.FileName);
        Directory.CreateDirectory(wp + ".new");
        var wf = GameOptionsFile.Load(w);
        wf.Set("vsync", 0);
        Throws<Exception>(() => wf.Save(), "a failed write throws");
        Equal(OptionsFixture, File.ReadAllText(wp, Encoding.Latin1), "the old file whole");
        Equal(1, wf.Changes.Count, "the change is still to be saved");
        Directory.Delete(wp + ".new");
        wf.Save();
        Equal(0, GameOptionsFile.Load(w).Get("vsync"), "saved once it can be");
        Check(!File.Exists(wp + ".new") && !Directory.Exists(wp + ".new"), "nothing left beside it");
    }

    static void GameOptionsSetRules()
    {
        var f = GameOptionsFile.Load(NewGameFolder("game-options-set", OptionsFixture));
        Equal(10000, f.Set("hdr_peak_brightness", 12345), "past the end: the end");
        Equal(130, f.Set("mouse_sensitivity", 125), "onto the steps: the nearest, the larger on a tie");
        Equal(50, f.Set("rumble_strength", 60), "rumble on 25s");
        Equal(0, f.Set("output_res", 9), "an output past the list: the first");
        Equal(-1, f.Set("left_stick_dead_zone", -7), "a range below 0");
        Equal(-3, f.Set("frame_rate_cap", -3), "the cap's 30");

        // (the preset sets its rows; a row not exempt changed by hand makes it Custom, an exempt one does not)
        f.Set("quality_preset", 4);
        foreach (var (k, v) in new Dictionary<string, int> { ["resolution"] = 1, ["upscaling"] = 2, ["sharpness"] = 40, ["frame_pacing"] = 1, ["game_speed"] = 1, ["handheld_profile"] = 0, ["texture_detail"] = 1 })
            Equal(v, f.Get(k), "High sets " + k);
        Equal("High (16GB)", f.Label("quality_preset"), "the preset's label");
        f.Set("fullscreen", 1);
        Equal(4, f.Get("quality_preset"), "an exempt row leaves the preset");
        f.Set("sharpness", 40);
        Equal(4, f.Get("quality_preset"), "the same value is no change");
        f.Set("sharpness", 60);
        Equal(0, f.Get("quality_preset"), "a scaling row by hand: Custom");
        // (with DLSS the preset's resolution is its nearest mode)
        f.Set("dlss", 1);
        f.Set("quality_preset", 2);
        Equal(3, f.Get("resolution"), "Low's 75% with DLSS: Quality (67%)");
        Equal(2, f.Get("texture_detail"), "Low: Reduced textures");
        f.Set("quality_preset", 1);
        Equal(4, f.Get("resolution"), "Minimum's 50% is DLSS's Performance");
        Equal(3, f.Get("texture_detail"), "Minimum: Low textures");
        Equal(1, f.Get("handheld_profile"), "Minimum: handheld profile");
        // (another Upscaler moves the resolution onto its steps)
        f.Set("dlss", 0);
        f.Set("resolution", 2);
        f.Set("dlss", 3);
        Equal(3, f.Get("resolution"), "75% with FSR 3.1: 67%");
        Equal(0, f.Get("quality_preset"), "the Upscaler is not exempt");

        // (Apply: the preset first, then the rest by the same rules)
        var b = GameOptionsFile.Load(NewGameFolder("game-options-apply", OptionsFixture));
        var got = b.Apply(new Dictionary<string, int> { ["frame_generation"] = 2, ["quality_preset"] = 3, ["sharpness"] = 30 });
        Equal(3, got["quality_preset"], "a preset row at the preset's own value keeps the preset");
        Equal(2, got["frame_generation"], "frame generation set");
        Equal(1, b.Get("upscaling"), "Balanced: Catmull-Rom");
        var c = b.Apply(new Dictionary<string, int> { ["quality_preset"] = 5, ["sharpness"] = 80 });
        Equal(0, c["quality_preset"], "a preset row changed after it: Custom, as in the game");
        Equal(80, c["sharpness"], "and its value");
        var changes = b.Changes.Count;
        Throws<KeyNotFoundException>(() => b.Apply(new Dictionary<string, int> { ["vsync"] = 0, ["descriptor_reuse"] = 0 }), "a key that is no row");
        Equal(changes, b.Changes.Count, "nothing set then");
        Equal(1, b.Get("vsync"), "vsync untouched");
        b.Save();
        var back = GameOptionsFile.Load(Path.GetDirectoryName(b.Path)!);
        Equal(80, back.Get("sharpness"), "saved");
        Equal(2, back.Get("frame_generation"), "saved");
        Equal(0, back.Changes.Count, "a fresh read has no changes");
    }

    static void GameOptionsOldVersion()
    {
        // (options_version 5: Frame pacing's and Start next frame early's old off defaults, Ultrawide's 1 the monitor's shape)
        var g = NewGameFolder("game-options-v5", "frame_pacing=0\r\nultrawide=1\r\nnvidia_reflex=0\r\nsharpness=20\r\noptions_version=5\r\npad_a=0\r\n");
        var f = GameOptionsFile.Load(g, (2560, 1080));
        Equal(5, f.Version, "version");
        Equal(1, f.Get("frame_pacing"), "pacing on, as the game reads a file before 7");
        Equal(2, f.Get("ultrawide"), "the monitor's shape at 2560x1080: 21:9");
        Equal(0, f.Get("nvidia_reflex"), "Reflex as saved from 5 on");
        Equal(0, GameOptionsFile.Load(g).Get("ultrawide"), "without the screen: 16:9, as the game where it cannot ask");
        f.Set("sharpness", 30);
        f.Save();
        Equal("frame_pacing=1\r\nultrawide=2\r\nnvidia_reflex=0\r\nsharpness=30\r\noptions_version=7\r\npad_a=0\r\n",
            File.ReadAllText(Path.Combine(g, GameOptionsSchema.FileName)).Replace("tail_early=1\r\n", ""), "brought up to 7 as the game would read it");
        Check(File.ReadAllText(Path.Combine(g, GameOptionsSchema.FileName)).Contains("tail_early=1\r\noptions_version=7"), "the Debug tab's tail_early too, before the version");
        var again = GameOptionsFile.Load(g);
        Equal(7, again.Version, "now 7");
        again.Set("frame_pacing", 0);
        again.Save();
        Equal(0, GameOptionsFile.Load(g).Get("frame_pacing"), "and from 7 on the choice is read as it is");

        // (no options_version line: Game speed real time; no resolution line but the old dynamic_resolution)
        var o = NewGameFolder("game-options-v0", "game_speed=0\ndynamic_resolution=0\n");
        var of = GameOptionsFile.Load(o);
        Equal(1, of.Get("game_speed"), "the old default real time");
        Equal(1, of.Get("resolution"), "dynamic_resolution 0: 100%");
        Equal(2, of.Get("nvidia_reflex"), "Reflex On + Boost before 5");

        // (no file: the defaults; a Save makes it with the changed rows and the version)
        var n = Path.Combine(NewRoot("game-options-new"), "game");
        Directory.CreateDirectory(n);
        var nf = GameOptionsFile.Load(n);
        Check(!nf.Exists, "no file");
        Equal(1, nf.Get("vsync"), "default");
        Equal("Off", nf.Label("fullscreen"), "default label");
        nf.Save();
        Check(!File.Exists(nf.Path), "nothing to write: no file made");
        nf.Set("vsync", 0);
        nf.Set("master_volume", 83);
        nf.Save();
        var nl = OperatingSystem.IsWindows() ? "\r\n" : "\n";
        Equal("vsync=0" + nl + "master_volume=85" + nl + "options_version=7" + nl, File.ReadAllText(nf.Path), "a new file: the rows in the menu's order, then the version");
        Check(nf.Exists, "now there");
    }
}
