// The game's Options menu as data: gpu/vk/vk_menu.cpp's Row table (rows[]), the tx_* functions that label its values, the
// keys ini_key gives them, the defaults of vk_menu.h's Options, and the Quality preset (apply_preset, hn_preset). Every row
// the game keeps in triforce_options.ini is here except the ones KeptKeys names (the Debug tab, the Layout preset); the Game
// language is language.txt's, not this file's (Languages.cs). Labels are the menu's own words: players compare them.
// Where the menu's label depends on this PC (the display's refresh, a gyroscope, what the driver runs) the label here is the
// one the menu shows when it knows nothing of it. When vk_menu.cpp's table changes, change this one: the test
// "game options: schema matches vk_menu.cpp" reads the table and says what differs.
using System.Globalization;

namespace Triforce.Logic;

public static class GameOptionsSchema
{
    /// <summary>The file the game reads at its start and writes at every change in its menu, in the game folder.</summary>
    public const string FileName = "triforce_options.ini";
    /// <summary>The options_version the game writes (vk_menu.cpp save): the file's format, which menu_init's rules read.</summary>
    public const int CurrentVersion = 7;

    public const string OutputResKey = "output_res";
    public const string ResolutionKey = "resolution";
    public const string UpscalerKey = "dlss";
    public const string PresetKey = "quality_preset";
    public const string RumbleKey = "rumble_strength";
    public const string GpuKey = "gpu_device";
    public const string MicKey = "microphone";
    public const string HandheldKey = "handheld_profile";
    public const string VersionKey = "options_version";
    public const string MicDeviceKey = "microphone_device";

    /// <summary>The names of the graphics cards, in the order the game counts them (its GPU device row's 1 is the first),
    /// for the GPU device row's labels. The launcher sets it (UseSystemGpus: Gpus.List()). The game lists the cards
    /// Vulkan gives it, which is usually but not always the order Windows lists them in. Null: "1: Device 1" and so on, as
    /// the menu shows a card it has no name for.</summary>
    public static Func<IReadOnlyList<string>>? GpuNames { get; set; }

    /// <summary>GpuNames from Gpus.List() (this PC's display adapters).</summary>
    public static void UseSystemGpus() => GpuNames = () => Gpus.List().Select(g => g.Name).ToList();

    /// <summary>((Triforce) 11-19, ticket "GPU read issue") GpuNames as the game counts them: the game folder's
    /// triforce_gpus.txt ("&lt;value&gt;\t&lt;name&gt;" lines, gpu/vk/vk_main.cpp pick_device), which the game writes at every
    /// start. Windows' order of the display adapters (Gpus.List()) is not Vulkan's on every PC: on a laptop with an Intel UHD
    /// 770 and an RTX 3050 the launcher's "1: NVIDIA" saved the value the game reads as the Intel. Without the file (a game
    /// that has not started since v0.5.3): Windows' order, as before.</summary>
    public static void UseGameGpus(Func<string?> gameFolder) => GpuNames = () =>
    {
        try
        {
            if (gameFolder() is { } g && File.Exists(Path.Combine(g, GpuListFile)))
            {
                var names = new List<string>();
                foreach (var line in File.ReadAllLines(Path.Combine(g, GpuListFile)))
                {
                    var tab = line.IndexOf('\t');
                    if (tab > 0 && int.TryParse(line.AsSpan(0, tab), out var v) && v == names.Count + 1) names.Add(line[(tab + 1)..].Trim());
                }
                if (names.Count > 0) return names;
            }
        }
        catch (Exception) { }
        return Gpus.List().Select(x => x.Name).ToList();
    };
    public const string GpuListFile = "triforce_gpus.txt";

    // ---- the keys ----

    /// <summary>vk_menu.cpp ini_key: a few rows keep the keys they were first saved with; every other row's key is its
    /// name lower-cased, its spaces as '_', cut to 31 characters.</summary>
    public static string IniKey(string name) => name switch
    {
        "Output resolution" => OutputResKey,
        "Render resolution" => ResolutionKey,
        "Game frame rate cap" => "frame_rate_cap",
        "Window scaling filter" => "upscaling",
        "Upscaler" => UpscalerKey,
        "Start next frame early" => "tail_early",
        _ => Cut(new string(name.Select(c => c == ' ' ? '_' : c is >= 'A' and <= 'Z' ? (char)(c + 32) : c).ToArray()), 31),
    };
    static string Cut(string s, int n) => s.Length > n ? s[..n] : s;

    // ---- the labels (the tx_* functions) ----

    static int V(IGameOptionValues? o, string key, int dflt) => o?.Get(key) ?? dflt;
    static string Pick(string[] t, int v) => t[Math.Clamp(v, 0, t.Length - 1)];
    static string OrFirst(string[] t, int v) => v >= 0 && v < t.Length ? t[v] : t[0];
    static string Pct(int v) => v.ToString(CultureInfo.InvariantCulture) + "%";

    static string TxOnoff(int v, IGameOptionValues? o) => v != 0 ? "On" : "Off";
    static string TxMenuShortcut(int v, IGameOptionValues? o) => v != 0 ? "F1 only" : "Insert or F1";
    static string TxHdr(int v, IGameOptionValues? o) => v == 1 ? "Expanded" : v == 2 ? "Native" : "Off";
    static string TxNits(int v, IGameOptionValues? o) => v.ToString(CultureInfo.InvariantCulture) + " nits";
    /// <summary>tx_fps_cap. 6, Monitor's rate, shows the display's rate in the game ("Monitor's rate (144)") once it knows it.</summary>
    static string TxFpsCap(int v, IGameOptionValues? o)
    {
        if (v < 0) return v == -1 ? "45" : v == -2 ? "40" : "30";
        if (v >= 6) return "Monitor's rate";
        return new[] { "60 (the console's)", "90", "120", "144", "165", "240" }[v];
    }
    static string TxSteadyFps(int v, IGameOptionValues? o) => v == 1 ? "Any rate" : v == 2 ? "Refresh steps" : "Off";
    /// <summary>tx_gyro without a controller's answer (the game adds "(gyro found)" and the like).</summary>
    static string TxGyro(int v, IGameOptionValues? o) => v != 0 ? "On" : "Off";
    static string TxMousecam(int v, IGameOptionValues? o) => v != 0 ? "Native" : "As the stick";
    static string TxSens(int v, IGameOptionValues? o) =>
        (v < 0 ? "-" : "") + (Math.Abs(v) / 100).ToString(CultureInfo.InvariantCulture) + "." + (Math.Abs(v) % 100).ToString("00", CultureInfo.InvariantCulture);
    static readonly string[] Upscales = { "Off", "Catmull-Rom", "Lanczos", "AMD FSR 1" };
    /// <summary>tx_filter as tx_upscale: the game shows "Not needed (window = output)" while its window is the output's size.</summary>
    static string TxFilter(int v, IGameOptionValues? o) => OrFirst(Upscales, v);
    /// <summary>tx_reflex as on an NVIDIA card with Reflex (elsewhere the game shows Off, and On with DLSS frame generation).</summary>
    static string TxReflex(int v, IGameOptionValues? o) => Pick(new[] { "Off", "On", "On + Boost" }, v);
    /// <summary>tx_antilag as on an AMD card with Anti-Lag 2 (elsewhere the game shows Off).</summary>
    static string TxAntilag(int v, IGameOptionValues? o) => v != 0 ? "On" : "Off";
    static readonly string[] Upscalers = { "Game", "NVIDIA DLSS", "NVIDIA DLAA", "AMD FSR 3.1", "AMD FSR 4", "AMD FSR 4 Native AA", "AMD FSR 3.1 Native AA" };
    /// <summary>tx_upscaler by the value chosen (the game shows what runs: the game's own where the choice cannot).</summary>
    static string TxUpscaler(int v, IGameOptionValues? o) => Pick(Upscalers, v);
    /// <summary>tx_dlss_model without RX_VK_DLSS_PRESET.</summary>
    static string TxDlssModel(int v, IGameOptionValues? o) => Pick(new[] { "Transformer (K)", "Transformer 2 (M)", "Transformer 2 (L)", "NVIDIA default" }, v);
    static string TxAniso(int v, IGameOptionValues? o) => OrFirst(new[] { "Game", "2x", "4x", "8x", "16x" }, v);
    /// <summary>tx_tex_detail: the game adds the level Automatic gives ("Automatic (Full)") once its first texture is made.</summary>
    static string TxTexDetail(int v, IGameOptionValues? o) => OrFirst(new[] { "Automatic", "Full", "Reduced", "Low" }, v);
    static string TxPercent(int v, IGameOptionValues? o) => Pct(v);
    static string TxRdxTonemap(int v, IGameOptionValues? o) => v == 2 ? "RenoDRT" : v != 0 ? "PsychoV35" : "Vanilla";
    static string TxRdxGamma(int v, IGameOptionValues? o) => v == 1 ? "2.2" : v == 2 ? "BT.1886 (2.4)" : "Off";
    static string TxRdxMidgray(int v, IGameOptionValues? o) => v == 1 ? "Vanilla" : v == 2 ? "Eye adaptation" : "Render";
    static string TxRdxSpace(int v, IGameOptionValues? o) => v == 1 ? "BT.709" : v == 2 ? "DCI-P3" : "Vanilla";
    static string TxRdxGrade(int v, IGameOptionValues? o) => v.ToString(CultureInfo.InvariantCulture);
    static string TxSpeed(int v, IGameOptionValues? o) => v != 0 ? "Real time" : "Per frame";
    static string TxTextSpeed(int v, IGameOptionValues? o) => OrFirst(new[] { "Normal", "2x", "4x", "Instant" }, v);
    static string TxUltrawide(int v, IGameOptionValues? o) => Pick(new[] { "16:9", "16:10", "21:9", "32:9" }, v);
    static string TxWideHud(int v, IGameOptionValues? o) => v != 0 ? "Screen edges" : "Middle";
    static string TxWideMenus(int v, IGameOptionValues? o) => Pick(new[] { "Full width", "Centred, blurred sides", "Centred, black bars" }, v);
    static string TxPrompts(int v, IGameOptionValues? o) =>
        Pick(new[] { "Auto", "Switch", "Xbox", "PlayStation 4", "PlayStation 5", "Steam Deck", "Keyboard & mouse" }, v);
    static string TxRumble(int v, IGameOptionValues? o) => v != 0 ? Pct(v) : "Off";
    static string TxVolume(int v, IGameOptionValues? o) => v != 0 ? Pct(v) : "Muted";
    static string TxMenuSounds(int v, IGameOptionValues? o) => v != 0 ? Pct(v) : "Off";
    static string TxSprint(int v, IGameOptionValues? o) => v == 1 ? "Left stick click" : v == 2 ? "B" : "Off";
    static string TxSprintMode(int v, IGameOptionValues? o) => v != 0 ? "Hold" : "Toggle";
    static string TxStickDeadZone(int v, IGameOptionValues? o) => OrFirst(new[] { "Default (24%)", "5%", "10%", "15%", "20%", "30%" }, v);
    static string TxOwnDeadZone(int v, IGameOptionValues? o) => v < 0 ? "As Stick dead zone" : Pct(v);
    static string TxWalkKey(int v, IGameOptionValues? o) => OrFirst(new[] { "Off", "Caps Lock", "Shift", "Ctrl", "Tab" }, v);
    static string TxAudioOutput(int v, IGameOptionValues? o) => OrFirst(new[] { "Auto", "Stereo", "5.1" }, v);
    static string TxDeadzone(int v, IGameOptionValues? o) => Pick(new[] { "Low", "Standard", "High" }, v);
    static string TxRobust(int v, IGameOptionValues? o) => OrFirst(new[] { "Auto", "Off", "On" }, v);
    static string TxMesh(int v, IGameOptionValues? o) => v == 1 ? "Compute fallback" : "Auto";
    static string TxPerf(int v, IGameOptionValues? o) => v == 1 ? "On" : v >= 2 ? "Full" : "Off";
    static string TxDlssg(int v, IGameOptionValues? o) => v == 1 ? "2x" : v == 2 ? "3x" : v == 3 ? "4x" : "Off";
    /// <summary>tx_framegen where AMD's library can make the frames (elsewhere the game shows a saved AMD FSR 4 as 2x).</summary>
    static string TxFramegen(int v, IGameOptionValues? o) => Pick(new[] { "Off", "Auto", "2x", "3x", "4x", "5x", "6x", "7x", "8x", "AMD FSR 4" }, v);
    static readonly string[] Presets = { "Custom", "Minimum (6GB)", "Low (8GB)", "Balanced (12GB)", "High (16GB)", "Ultra (20GB+)" };
    static string TxPreset(int v, IGameOptionValues? o) => Pick(Presets, v);
    /// <summary>tx_gpu: 0 "Auto", then "&lt;n&gt;: &lt;name&gt;" counted from 0 (GpuNames), "&lt;n&gt;: Device &lt;n&gt;" without a name.</summary>
    static string TxGpu(int v, IGameOptionValues? o)
    {
        if (v <= 0) return "Auto";
        var idx = v - 1;
        var names = GpuNamesNow();
        return idx < names.Count ? idx + ": " + names[idx] : idx + ": Device " + idx;
    }
    static IReadOnlyList<string> GpuNamesNow()
    {
        try { return GpuNames?.Invoke() ?? Array.Empty<string>(); } catch (Exception) { return Array.Empty<string>(); }
    }
    /// <summary>((Triforce) 11-20) The GPU device row's card for a value, by the name the row shows for it (GpuNames): what
    /// GameOptionsFile.Save writes as gpu_device_name beside the number, so that the game finds the card the player clicked
    /// whatever order the list was in (gpu/vk/gpu_choice.h pick). Null for Auto or a value with no name.</summary>
    public static string? GpuNameOf(int v)
    {
        if (v <= 0) return null;
        var names = GpuNamesNow();
        return v - 1 < names.Count && names[v - 1].Trim() is { Length: > 0 } n ? n : null;
    }
    public const string GpuNameKey = "gpu_device_name";
    /// <summary>tx_mic without the game's device list: Off, Default, and a device by the name the file keeps
    /// (microphone_device), "Microphone" without one.</summary>
    static string TxMic(int v, IGameOptionValues? o)
    {
        if (v <= 0) return "Off";
        if (v == 1) return "Default";
        var name = o?.Raw(MicDeviceKey) is { } raw ? MicName(raw) : "";
        return name.Length > 0 ? name : "Microphone";
    }
    /// <summary>The microphone_device line's name as text: the file's bytes are UTF-8 (read here as Latin-1, byte for byte).</summary>
    internal static string MicName(string raw) => System.Text.Encoding.UTF8.GetString(System.Text.Encoding.Latin1.GetBytes(raw)).Trim();

    // ---- the Output resolution and the Render resolution ----

    /// <summary>The Output resolution's sizes by their saved value (kOutputs); 6 came after the others so that saved values
    /// keep their sizes.</summary>
    public static readonly IReadOnlyList<(int W, int H)> Outputs = new[] { (2048, 1152), (2560, 1440), (3840, 2160), (1920, 1080), (5120, 1440), (5120, 2880), (5120, 2160) };
    /// <summary>The order the menu shows them in, smallest to largest (kOutputOrder).</summary>
    public static readonly IReadOnlyList<int> OutputOrder = new[] { 3, 0, 1, 2, 4, 6, 5 };
    static int OutputChoice(int v) => v >= 0 && v < Outputs.Count ? v : 0;
    static string TxOutputRes(int v, IGameOptionValues? o)
    {
        var (w, h) = Outputs[OutputChoice(v)];
        return w + "x" + h;
    }

    // (upscaler_rows.h)
    const int Game = 0, Dlss = 1, Dlaa = 2, Fsr3 = 3, Fsr4 = 4, Fsr4Aa = 5, Fsr3Aa = 6;
    /// <summary>The Upscaler row's order in the menu: each engine's native anti-aliasing after it (kOrder).</summary>
    public static readonly IReadOnlyList<int> UpscalerOrder = new[] { Game, Dlss, Dlaa, Fsr3, Fsr3Aa, Fsr4, Fsr4Aa };
    static readonly int[] Percents = { 0, 100, 75, 67, 50, 58, 33 };
    static int Percent(int v) => v > 0 && v < Percents.Length ? Percents[v] : 0;
    static bool FullScale(int up) => up == Dlaa || up == Fsr4Aa || up == Fsr3Aa;
    static bool ScalesByMode(int up) => up == Dlss || up == Fsr3 || up == Fsr4;
    static readonly int[] GameSteps = { 0, 1, 2, 3, 4 }, DlssSteps = { 0, 3, 5, 4, 6 };
    /// <summary>The Render resolution values the menu steps through with this Upscaler (none with DLAA or a native AA:
    /// the 3D is drawn at full size).</summary>
    public static IReadOnlyList<int> ResolutionSteps(int upscaler) => FullScale(upscaler) ? Array.Empty<int>() : ScalesByMode(upscaler) ? DlssSteps : GameSteps;
    /// <summary>upscaler_rows.h snap: v as one of the upscaler's steps, the nearest in size (the larger on a tie); Auto stays,
    /// and with a full-size upscaler v is kept for the next.</summary>
    public static int SnapResolution(int upscaler, int v)
    {
        if (FullScale(upscaler) || Percent(v) == 0) return Percent(v) == 0 ? 0 : v;
        var steps = ScalesByMode(upscaler) ? DlssSteps : GameSteps;
        int best = v, bestD = 1000;
        foreach (var s in steps)
        {
            if (s == v) return v;
            if (Percent(s) == 0) continue;
            var d = Math.Abs(Percent(s) - Percent(v));
            if (d < bestD || (d == bestD && Percent(s) > Percent(best))) { best = s; bestD = d; }
        }
        return best;
    }
    static string ModeName(int v) => Percent(v) switch { 67 => "Quality", 58 => "Balanced", 50 => "Performance", 33 => "Ultra Performance", _ => "" };
    /// <summary>tx_resolution: by the Upscaler chosen, and the size the game draws the 3D at for a fixed share of the
    /// Output resolution (the handheld profile's 1536x864), a twelfth of it as the game takes it.</summary>
    static string TxResolution(int v, IGameOptionValues? o)
    {
        var up = V(o, UpscalerKey, 0);
        if (up == Dlaa) return "Native (DLAA)";
        if (up == Fsr4Aa) return "Native (FSR 4)";
        if (up == Fsr3Aa) return "Native (FSR 3.1)";
        var p = Percent(v);
        if (p == 0) return "Auto (dynamic)";
        if (ScalesByMode(up) && ModeName(v).Length > 0) return ModeName(v) + " (" + p + "%)";
        var (ow, oh) = V(o, HandheldKey, 0) != 0 ? (1536, 864) : Outputs[OutputChoice(V(o, OutputResKey, 0))];
        var k = Math.Min(Math.Max((int)(p / 100.0f * 12.0f + 0.5f), 1), 12);
        return p + "% (" + (ow * k + 11) / 12 + "x" + oh * k / 12 + ")";
    }
    static readonly string[] AaNames = { "Game SMAA", "Off", "FXAA", "SMAA Ultra" };
    /// <summary>tx_aa: the choice, or whose anti-aliasing the picture has while DLSS or FSR draws (the row does not apply).</summary>
    static string TxAa(int v, IGameOptionValues? o)
    {
        var up = V(o, UpscalerKey, 0);
        if (up == Dlss || up == Dlaa) return "DLSS's own";
        if (up != Game) return "FSR's own";
        return OrFirst(AaNames, v);
    }

    // ---- the rows ----

    static GameOptionRow Row(GameOptionTab tab, string? section, string name, int lo, int hi, int step, int dflt, GameOptionKind kind,
        Func<int, IGameOptionValues?, string> format, bool restart, bool exempt, string help)
    {
        var key = IniKey(name);
        Func<IReadOnlyList<GameOptionChoice>>? list = null;
        if (kind == GameOptionKind.Choice)
        {
            IEnumerable<int> values = key switch
            {
                OutputResKey => OutputOrder,
                UpscalerKey => UpscalerOrder,
                ResolutionKey => GameSteps,
                MicKey => new[] { 0, 1 },
                _ => Enumerable.Range(lo, hi - lo + 1),
            };
            if (key == GpuKey)
                list = () => new[] { new GameOptionChoice(0, "Auto") }
                    .Concat(GpuNamesNow().Take(hi).Select((n, i) => new GameOptionChoice(i + 1, i + ": " + n))).ToList();
            else
            {
                var fixedList = values.Select(v => new GameOptionChoice(v, format(v, null))).ToList();
                list = () => fixedList;
            }
        }
        return new GameOptionRow
        {
            Tab = tab, Section = section, Name = name, Key = key, Lo = lo, Hi = hi, Step = step, Default = dflt, Kind = kind,
            Format = format, ChoiceList = list, Restart = restart, PresetExempt = exempt, Help = help,
        };
    }

    /// <summary>Every row, in the menu's order (Display, Graphics, System, Audio, Controls as the table has them; the
    /// Motion page's rows and Button prompts after the Controls tab's own).</summary>
    public static readonly IReadOnlyList<GameOptionRow> All = new[]
    {
        Row(GameOptionTab.Display, "Screen", "Fullscreen", 0, 1, 1, 0, GameOptionKind.Choice, TxOnoff, false, true,
            "Fills the whole screen with the game, without the window's border. The same as F10 or Alt+Enter."),
        Row(GameOptionTab.Display, "Screen", "Output resolution", 0, 6, 1, 0, GameOptionKind.Choice, TxOutputRes, true, false,
            "The game's whole frame, HUD included, before it is scaled to the window. Docked only. Applies after a restart."),
        Row(GameOptionTab.Display, "Screen", "GPU device", 0, 8, 1, 0, GameOptionKind.Choice, TxGpu, true, true,
            "Which graphics card the game runs on. Applies after a restart."),
        Row(GameOptionTab.Display, "Widescreen", "Ultrawide", 0, 3, 1, 0, GameOptionKind.Choice, TxUltrawide, true, false,
            "The camera's shape: the game's own 16:9, or a fixed 16:10, 21:9 or 32:9 for a screen or a window of that shape. A screen of another shape shows the picture between black bars. Applies after a restart."),
        Row(GameOptionTab.Display, "Widescreen", "Ultrawide HUD", 0, 1, 1, 0, GameOptionKind.Choice, TxWideHud, true, true,
            "With Ultrawide: where the hearts, the item slot and the map sit on a screen wider than 16:9. Middle: in the middle 16:9, as on a normal screen. Screen edges: out in the screen's corners. Applies after a restart."),
        Row(GameOptionTab.Display, "Widescreen", "Ultrawide menus", 0, 2, 1, 1, GameOptionKind.Choice, TxWideMenus, false, false,
            "With Ultrawide: the pause, item and map menus, and what shows beside them. Loading screens and pre-rendered movies, which draw no world either, get the same sides; a fade always covers the whole screen. Changes at once."),
        Row(GameOptionTab.Display, "Frame rate", "Game frame rate cap", -3, 6, 1, 0, GameOptionKind.Choice, TxFpsCap, true, true,
            "Limits frames rendered by the game. Generated frames are additional. Takes a restart. 30, 40 and 45 are for a PC or handheld that holds one of them steadily where 60 swings; the game keeps its speed. Past 60 is experimental: the game keeps real time, but some cutscenes and bosses can misbehave (Gohma, the intro's narration). 60 is the safe choice. Monitor's rate is the refresh rate of the screen the game starts on, from 30 to 240."),
        Row(GameOptionTab.Display, "Frame rate", "Steady frame rate", 0, 2, 1, 0, GameOptionKind.Choice, TxSteadyFps, true, true,
            "Paces the game to a frame rate this PC holds where you are, under the Game frame rate cap, and shows each frame a period after the last, so that frames come evenly instead of each as soon as it is ready. It goes down quickly where frames run late and back up slowly where there is room; loads and single hitches do not move it. Gives up some frame rate for steadiness. Any rate suits a screen with variable refresh (G-SYNC, FreeSync) or VSync off; Refresh steps keeps to whole divisions of the refresh rate (at 240 Hz: 120, 80, 60 ...) for VSync without variable refresh. Takes a restart."),
        Row(GameOptionTab.Display, "Frame rate", "Reflex frame limit", 0, 1, 1, 0, GameOptionKind.Choice, TxOnoff, false, true,
            "With DLSS frame generation: NVIDIA Reflex holds the Game frame rate cap where each frame starts, in place of the game's own wait, so a frame waits once and the controls are read later. The game's own wait is back whenever no frame is being generated. Where RivaTuner Statistics Server is the game's limiter it holds the frames instead. Changes at once."),
        Row(GameOptionTab.Display, "Frame rate", "VSync", 0, 1, 1, 1, GameOptionKind.Choice, TxOnoff, false, true,
            "Whether a finished frame waits for the screen's next refresh. Off shows it at once: a little less delay, and the picture can tear. Neither changes the frame rate, which the Game frame rate cap limits. Changes at once."),
        Row(GameOptionTab.Display, "Frame rate", "FPS counter", 0, 2, 1, 0, GameOptionKind.Choice, TxPerf, false, true,
            "On: frames per second in the top right corner. Full: a performance overlay in its place, with the GPU's and the CPU's load, temperatures and memory, the frame rate and its 1% and 0.1% lows, the frame times and their graph, the waits per frame and the picture's settings."),
        Row(GameOptionTab.Display, "High dynamic range", "HDR", 0, 2, 1, 0, GameOptionKind.Choice, TxHdr, false, true,
            "HDR10 out on an HDR display (Windows: Use HDR on in the display settings). Expanded: the game's SDR picture, its white at the paper white and its highlights up to the peak. Native: the game's own HDR, told the display's peak and paper white."),
        Row(GameOptionTab.Display, "High dynamic range", "HDR paper white", 80, 400, 10, 200, GameOptionKind.Slider, TxNits, false, true,
            "How bright the picture's white is with HDR on. Around 200 for a normal room; higher in a bright one."),
        Row(GameOptionTab.Display, "High dynamic range", "HDR peak brightness", 400, 10000, 100, 1000, GameOptionKind.Slider, TxNits, false, true,
            "The brightest highlights with HDR on: set it to your display's peak brightness."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX HDR", 0, 1, 1, 0, GameOptionKind.Choice, TxOnoff, true, true,
            "With HDR set to Native: RenoDX's tone mapping, eye adaptation and color grading in place of the game's own, and its rows below. Off: the game's own HDR. Applies after a restart. RenoDX is by Carlos Lopez (clshortfuse), under the MIT license."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX tonemapper", 0, 2, 1, 1, GameOptionKind.Choice, TxRdxTonemap, false, true,
            "With HDR Native: how the scene's light is brought to the display. Vanilla: the game's own curve. PsychoV35: RenoDX's, which keeps hue and saturation into the highlights, up to the peak brightness. RenoDRT: RenoDX's, which carries the game's SDR look up to the peak by brightness. RenoDX is by Carlos Lopez (clshortfuse), under the MIT license."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX gamma correction", 0, 2, 1, 0, GameOptionKind.Choice, TxRdxGamma, false, true,
            "Shows the picture as a 2.2 or BT.1886 (2.4) display would, as the game was made to be seen: darker shadows. Off keeps the sRGB curve. With every tonemapper. PsychoV35 and RenoDRT still end at the peak brightness; Vanilla's highlights are the game's own, which 2.2 dims a little and 2.4 brightens a little."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX midgray", 0, 2, 1, 1, GameOptionKind.Choice, TxRdxMidgray, false, true,
            "With PsychoV35: what the scene's middle grey is held to. Render: as rendered. Vanilla: where the game's own curve puts it. Eye adaptation: it follows what is on screen over a few seconds, weighted towards the middle of the view, as eyes adjust."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX match tone slope", 0, 1, 1, 1, GameOptionKind.Choice, TxOnoff, false, true,
            "With PsychoV35: gives the mid tones the contrast of the game's own curve. Off: PsychoV35's own, softer."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX tone slope", 0, 100, 1, 50, GameOptionKind.Slider, TxRdxGrade, false, true,
            "With PsychoV35: the steepness of the tone curve through middle grey. 50 leaves it unchanged."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX hue clip", 0, 100, 5, 50, GameOptionKind.Slider, TxPercent, false, true,
            "With PsychoV35: how much bright colors turn as they do where the game's SDR picture clips: fire towards yellow and white. 0% keeps their hue."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX UI brightness", 80, 400, 10, 200, GameOptionKind.Slider, TxNits, false, true,
            "How bright the menus' and the HUD's white is with RenoDX's tonemapper. The scene's follows HDR paper white."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX color space", 0, 2, 1, 1, GameOptionKind.Choice, TxRdxSpace, false, true,
            "The colors the picture is taken to be in before HDR10's. Vanilla: the game's own matrix. BT.709: as the SDR picture. DCI-P3: more saturated."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX exposure", 0, 200, 5, 100, GameOptionKind.Slider, TxPercent, false, true,
            "The scene's brightness before RenoDX's tone mapping. 100% is the game's."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX highlights", 0, 100, 5, 50, GameOptionKind.Slider, TxRdxGrade, false, true,
            "RenoDX's color grading: the bright parts of the scene. 50 is neutral."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX shadows", 0, 100, 5, 50, GameOptionKind.Slider, TxRdxGrade, false, true,
            "RenoDX's color grading: the dark parts of the scene. 50 is neutral."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX contrast", 0, 100, 5, 50, GameOptionKind.Slider, TxRdxGrade, false, true,
            "RenoDX's color grading: the scene's contrast. 50 is neutral."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX saturation", 0, 100, 5, 50, GameOptionKind.Slider, TxRdxGrade, false, true,
            "RenoDX's color grading: the scene's saturation. 50 is neutral."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX highlight saturation", 0, 100, 1, 50, GameOptionKind.Slider, TxRdxGrade, false, true,
            "With RenoDRT: the strength of the colors of the brightest parts. 50 leaves it unchanged."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX blowout", 0, 100, 1, 0, GameOptionKind.Slider, TxRdxGrade, false, true,
            "With RenoDRT: turns the brightest parts towards white. 0 keeps their color."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX flare", 0, 100, 1, 0, GameOptionKind.Slider, TxRdxGrade, false, true,
            "With RenoDRT: lifts the darkest parts as a little stray light would. 0 keeps black black."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX hue correction", 0, 100, 5, 100, GameOptionKind.Slider, TxPercent, false, true,
            "With RenoDRT: how much bright colors keep the hue they have before the tonemapper. 0% lets them shift as the curve takes them."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX color grading", 0, 100, 5, 100, GameOptionKind.Slider, TxPercent, false, true,
            "With RenoDRT: how much of the game's own color grading the HDR picture keeps. 0% is the scene without it."),
        Row(GameOptionTab.Display, "RenoDX", "RenoDX bloom", 0, 100, 5, 50, GameOptionKind.Slider, TxRdxGrade, false, true,
            "How strong the glow around bright things is. 50 is the game's."),
        Row(GameOptionTab.Graphics, "Quality", "Quality preset", 0, 5, 1, 0, GameOptionKind.Choice, TxPreset, false, false,
            "Sets the scaling rows and memory limits for a GPU with that much memory. The Upscaler and Frame generation stay yours."),
        Row(GameOptionTab.Graphics, "Quality", "Render resolution", 0, 6, 1, 0, GameOptionKind.Choice, TxResolution, true, false,
            "The share of the output the 3D is drawn at; the Upscaler fills in the rest. Applies after a restart."),
        Row(GameOptionTab.Graphics, "Quality", "Texture detail", 0, 3, 1, 0, GameOptionKind.Choice, TxTexDetail, true, false,
            "How large the game's textures are kept in video memory. Automatic suits the graphics card; a Quality preset sets it for its card's size. Applies after a restart."),
        Row(GameOptionTab.Graphics, "Quality", "Texture filtering", 0, 4, 1, 0, GameOptionKind.Choice, TxAniso, false, false,
            "Anisotropic filtering: keeps ground, paths and walls sharp where they slope away from the camera. Game's is what the game asks for; 16x is the sharpest, at a small cost."),
        Row(GameOptionTab.Graphics, "Upscaling and edges", "Upscaler", 0, 6, 1, 0, GameOptionKind.Choice, TxUpscaler, false, false,
            "What scales the 3D up to the Output resolution."),
        Row(GameOptionTab.Graphics, "Upscaling and edges", "DLSS model", 0, 3, 1, 0, GameOptionKind.Choice, TxDlssModel, false, true,
            "Which of NVIDIA's DLSS networks draws the picture."),
        Row(GameOptionTab.Graphics, "Upscaling and edges", "Upscale before bloom", 0, 1, 1, 0, GameOptionKind.Choice, TxOnoff, true, true,
            "With NVIDIA DLSS, DLAA or AMD FSR as the Upscaler: On, it works on the scene before the game's glow and tone mapping, so both are made from the sharpened scene. Off: on the finished picture, as before. Applies after a restart."),
        Row(GameOptionTab.Graphics, "Upscaling and edges", "Anti-aliasing", 0, 3, 1, 0, GameOptionKind.Choice, TxAa, false, true,
            "How the 3D's edges are smoothed before it is scaled up: by the game's own SMAA, not at all, by FXAA, or by SMAA at its Ultra preset. Changes at once."),
        Row(GameOptionTab.Graphics, "Upscaling and edges", "Sharpness", 0, 100, 10, 0, GameOptionKind.Slider, TxPercent, false, false,
            "Sharpens the picture as it goes to the window, with any filter. Too much shows halos around edges."),
        Row(GameOptionTab.Graphics, "Upscaling and edges", "Window scaling filter", 0, 3, 1, 0, GameOptionKind.Choice, TxFilter, false, false,
            "How the finished picture is scaled to the window, when its size differs from the Output resolution."),
        Row(GameOptionTab.Graphics, "Smoothness", "Frame generation", 0, 9, 1, 0, GameOptionKind.Choice, TxFramegen, false, true,
            "Makes frames between the game's own, for smoother motion, at a little input delay. Fast motion or camera cuts can show artifacts."),
        Row(GameOptionTab.Graphics, "Smoothness", "DLSS frame generation", 0, 3, 1, 0, GameOptionKind.Choice, TxDlssg, true, true,
            "NVIDIA RTX 40 and 50 cards: frames made between the game's own by NVIDIA DLSS, from the scene's depth and motion, with NVIDIA Reflex. The Frame generation row above is not used with it. Applies after a restart."),
        Row(GameOptionTab.Graphics, "Smoothness", "Frame pacing", 0, 1, 1, 1, GameOptionKind.Choice, TxOnoff, false, false,
            "When each frame is shown."),
        Row(GameOptionTab.Graphics, "Latency", "NVIDIA Reflex", 0, 2, 1, 2, GameOptionKind.Choice, TxReflex, false, false,
            "NVIDIA GPUs: each frame starts as late as it can, for less input delay. With DLSS frame generation it runs through NVIDIA Streamline; not used with the other frame generation."),
        Row(GameOptionTab.Graphics, "Latency", "AMD Anti-Lag 2", 0, 1, 1, 0, GameOptionKind.Choice, TxAntilag, false, false,
            "AMD GPUs: each frame starts as late as it can, for less input delay. Not used with frame generation."),
        Row(GameOptionTab.Graphics, "Advanced", "Handheld profile", 0, 1, 1, 0, GameOptionKind.Choice, TxOnoff, true, false,
            "Runs the game as if the console were undocked. Applies after a restart."),
        Row(GameOptionTab.Graphics, "Advanced", "Robust buffers", 0, 2, 1, 1, GameOptionKind.Choice, TxRobust, true, false,
            "Bounds-checked buffer reads: a read past a buffer's end is dropped instead of stopping the GPU. Applies after a restart."),
        Row(GameOptionTab.Graphics, "Advanced", "Mesh shaders", 0, 1, 1, 0, GameOptionKind.Choice, TxMesh, true, false,
            "How the game's mesh-shader draws run. Try Compute fallback on an AMD card that shows glowing walls or loses the GPU. Applies after a restart."),
        Row(GameOptionTab.System, "Game", "Game speed", 0, 1, 1, 1, GameOptionKind.Choice, TxSpeed, false, false,
            "How the game keeps time when it runs below 60 fps."),
        Row(GameOptionTab.System, "Game", "Text speed", 0, 3, 1, 0, GameOptionKind.Choice, TxTextSpeed, false, false,
            "How fast the letters of dialogue and other messages appear."),
        Row(GameOptionTab.System, "Game", "Discord status", 0, 1, 1, 1, GameOptionKind.Choice, TxOnoff, false, true,
            "Shows \"Playing Triforce\" and the time played in your Discord status, while Discord runs on this PC."),
        Row(GameOptionTab.Audio, null, "Master volume", 0, 100, 5, 100, GameOptionKind.Slider, TxVolume, false, true,
            "How loud the game is, in percent of its own volume. The game's sounds and music keep their balance."),
        Row(GameOptionTab.Audio, null, "Menu sounds", 0, 100, 5, 80, GameOptionKind.Slider, TxMenuSounds, false, true,
            "How loud these options' own sounds are: the game's interface sounds, played from your game files as you move through the tabs and rows. Off is silent. The Master volume is over them too."),
        Row(GameOptionTab.Audio, null, "Audio output", 0, 2, 1, 0, GameOptionKind.Choice, TxAudioOutput, true, true,
            "The speakers the game mixes its sound for. Applies after a restart."),
        Row(GameOptionTab.Audio, null, "Microphone", 0, 18, 1, 1, GameOptionKind.Choice, TxMic, false, true,
            "What the ocarina hears when the game's microphone option is on (playing an instrument, or humming)."),
        Row(GameOptionTab.Controls, "Camera", "Camera speed", 25, 300, 5, 100, GameOptionKind.Slider, TxSens, true, true,
            "How fast the right stick turns the camera: times the game's own speed, at any Camera sensitivity of the game's Options. The mouse keeps its own sensitivity. Applies after a restart."),
        Row(GameOptionTab.Controls, "Camera", "Invert camera X", 0, 1, 1, 0, GameOptionKind.Choice, TxOnoff, false, true,
            "Inverts horizontal look (left/right) on the right analog stick."),
        Row(GameOptionTab.Controls, "Camera", "Invert camera Y", 0, 1, 1, 0, GameOptionKind.Choice, TxOnoff, false, true,
            "Inverts vertical look (up/down) on the right analog stick."),
        Row(GameOptionTab.Controls, "Camera", "Mouse camera", 0, 1, 1, 0, GameOptionKind.Choice, TxMousecam, false, true,
            "How the captured mouse turns the camera."),
        Row(GameOptionTab.Controls, "Camera", "Mouse sensitivity", 10, 500, 10, 100, GameOptionKind.Slider, TxSens, false, true,
            "How far the camera turns for a mouse movement, with either Mouse camera: at 1.00 native look turns 0.1 degree a count. The - and = keys change it in game too."),
        Row(GameOptionTab.Controls, "Camera", "Invert mouse vertical", 0, 1, 1, 0, GameOptionKind.Choice, TxOnoff, false, true,
            "Swaps up and down for the mouse."),
        Row(GameOptionTab.Controls, "Sticks", "Stick dead zone", 0, 5, 1, 0, GameOptionKind.Choice, TxStickDeadZone, false, true,
            "How far a stick moves before the game sees it, on each axis. Lower for a precise stick, higher for one that drifts. The game's own dead zone still follows."),
        Row(GameOptionTab.Controls, "Sticks", "Left stick dead zone", -1, 50, 1, -1, GameOptionKind.Slider, TxOwnDeadZone, false, true,
            "The left stick's own dead zone, in percent of its travel on each axis, in place of Stick dead zone: a worn stick can have a larger one than the other. 0% cuts nothing; the game's own dead zone still follows."),
        Row(GameOptionTab.Controls, "Sticks", "Right stick dead zone", -1, 50, 1, -1, GameOptionKind.Slider, TxOwnDeadZone, false, true,
            "The right stick's own dead zone, in percent of its travel on each axis, in place of Stick dead zone: a worn stick can have a larger one than the other. 0% cuts nothing; the game's own dead zone still follows."),
        Row(GameOptionTab.Controls, "Movement", "Sprint button", 0, 2, 1, 0, GameOptionKind.Choice, TxSprint, false, true,
            "Link sprints from a button press, without rolling first. Left stick click: the game has no other use for it. B: sprints only while Link moves, and puts the weapon away as usual otherwise (with the game's Swap Jump Controls B is the jump: use the stick's click)."),
        Row(GameOptionTab.Controls, "Movement", "Sprint mode", 0, 1, 1, 0, GameOptionKind.Choice, TxSprintMode, false, true,
            "Toggle: a press starts the sprint and the next press, or stopping, ends it. Hold: Link sprints while the button is held."),
        Row(GameOptionTab.Controls, "Movement", "Walk key", 0, 4, 1, 0, GameOptionKind.Choice, TxWalkKey, false, true,
            "A key that makes Link walk on the keyboard: held with the movement keys, they push the left stick only as far as Walk scale. While it is chosen the key presses nothing else."),
        Row(GameOptionTab.Controls, "Movement", "Walk scale", 20, 90, 1, 51, GameOptionKind.Slider, TxSens, false, true,
            "How far the movement keys push the left stick while the Walk key is held: 1.00 would be all the way, lower walks slower. A controller's stick and the camera's keys are not changed."),
        Row(GameOptionTab.Controls, "Vibration", "Rumble strength", 0, 300, 25, 100, GameOptionKind.Slider, TxRumble, false, true,
            "How strongly the controller rumbles, in percent of the game's rumble. Off turns it off. Over 100% a rumble that would pass the motors' full strength stops there."),
        Row(GameOptionTab.Controls, "Vibration", "Vibration curve", 20, 150, 5, 100, GameOptionKind.Slider, TxSens, false, true,
            "Shapes the rumble of a controller's two motors. 1.00 is the game's. Lower lifts the many weak rumbles so they can be felt and hardly changes the strong ones; higher quiets them. A Switch controller's HD rumble is not changed by it."),
        Row(GameOptionTab.Controls, "Vibration", "Trigger vibration", 0, 1, 1, 0, GameOptionKind.Choice, TxOnoff, false, true,
            "Xbox One and Series controllers: the motors in the triggers rumble too, with the lighter of the two motors. Controllers without them rumble as before."),
        Row(GameOptionTab.Controls, "Other", "Controller in background", 0, 1, 1, 0, GameOptionKind.Choice, TxOnoff, false, false,
            "Whether the controller plays the game while another window has the focus."),
        Row(GameOptionTab.Controls, "Other", "Menu shortcut", 0, 1, 1, 0, GameOptionKind.Choice, TxMenuShortcut, false, true,
            "Which keys open and close these options from anywhere. F1 always does; F1 only leaves Insert free for an overlay."),
        Row(GameOptionTab.Controls, "Motion", "Motion controls", 0, 1, 1, 1, GameOptionKind.Choice, TxGyro, false, true,
            "Aims and looks with the controller's gyroscope where the game uses motion, as on the Switch. Works with Switch Pro controllers and pads in Switch mode, DualSense and DualShock 4. Shown beside it: whether the connected pad has one."),
        Row(GameOptionTab.Controls, "Motion", "Motion sensitivity X", 10, 600, 10, 100, GameOptionKind.Slider, TxPercent, false, true,
            "How far the aim moves left and right for a given turn of the controller. 100% is the console's own."),
        Row(GameOptionTab.Controls, "Motion", "Motion sensitivity Y", 10, 300, 10, 100, GameOptionKind.Slider, TxPercent, false, true,
            "How far the aim moves up and down for a given tilt of the controller. 100% is the console's own."),
        Row(GameOptionTab.Controls, "Motion", "Invert motion X", 0, 1, 1, 0, GameOptionKind.Choice, TxOnoff, false, true,
            "Turns the motion aim's left and right around."),
        Row(GameOptionTab.Controls, "Motion", "Invert motion Y", 0, 1, 1, 0, GameOptionKind.Choice, TxOnoff, false, true,
            "Turns the motion aim's up and down around."),
        Row(GameOptionTab.Controls, "Motion", "Motion deadzone", 0, 2, 1, 1, GameOptionKind.Choice, TxDeadzone, false, true,
            "How slow a turn counts as none, the console's three settings: Standard is its own. High steadies a pad that jitters when held still; Low keeps the smallest movements."),
        Row(GameOptionTab.Controls, "Controls", "Button prompts", 0, 6, 1, 0, GameOptionKind.Choice, TxPrompts, true, true,
            "The button icons the game shows, in its texts and menus: those of this controller, or the keyboard's and the mouse's, for the buttons or keys your controls put each action on."),
    };

    static readonly Dictionary<string, GameOptionRow> byKey = All.ToDictionary(r => r.Key, StringComparer.Ordinal);
    static readonly Dictionary<string, GameOptionRow> byName = All.ToDictionary(r => r.Name, StringComparer.Ordinal);

    /// <summary>The row of a triforce_options.ini key (exact, as the game compares them), null for any other key.</summary>
    public static GameOptionRow? ByKey(string key) => byKey.GetValueOrDefault(key);
    /// <summary>The row of a key; KeyNotFoundException for a key no row has.</summary>
    public static GameOptionRow Get(string key) => ByKey(key) ?? throw new KeyNotFoundException("no Options row has the key " + key);
    /// <summary>The row by its name as the menu shows it.</summary>
    public static GameOptionRow? ByName(string name) => byName.GetValueOrDefault(name);
    /// <summary>A tab's rows, in order.</summary>
    public static IReadOnlyList<GameOptionRow> RowsOf(GameOptionTab tab) => All.Where(r => r.Tab == tab).ToList();
    /// <summary>A tab's sections that have rows here, in order (Audio has none: its rows' Section is null).</summary>
    public static IReadOnlyList<string> Sections(GameOptionTab tab) => RowsOf(tab).Select(r => r.Section).OfType<string>().Distinct().ToList();

    // ---- the keys of the file that are not rows here ----

    /// <summary>The file's keys the launcher knowingly leaves as they are (GameOptionsFile keeps their lines, in order), and why.
    /// Besides these: every pad_* and key_* line (KeptPrefixes).</summary>
    public static readonly IReadOnlyDictionary<string, string> KeptKeys = new Dictionary<string, string>(StringComparer.Ordinal)
    {
        [VersionKey] = "the file's format: the rules the game reads an older file by (GameOptionsFile.Save writes 7 when it brings an older file up to date)",
        [MicDeviceKey] = "the Microphone row's device, by its name: the game's to choose from the devices it lists",
        [GpuNameKey] = "the GPU device row's card by its name (11-20): Save writes it with the row; the game finds the card by it first",
        ["layout_preset"] = "the Controls page's Layout preset: choosing one in the game rebinds the pad_ lines, which a changed value alone would not do",
        ["debug_tools"] = "the Debug tab (not ported)",
        ["descriptor_reuse"] = "the Debug tab's Developer rows (not ported)",
        ["fast_pipeline_link"] = "the Debug tab's Developer rows (not ported)",
        ["sampler_cache"] = "the Debug tab's Developer rows (not ported)",
        ["lazy_write-back"] = "the Debug tab's Developer rows (not ported)",
        ["replay_helper_threads"] = "the Debug tab's Developer rows (not ported)",
        ["tail_early"] = "the Debug tab's Start next frame early (not ported)",
        ["opaque_textures_as_bc1"] = "the Debug tab's Developer rows (not ported)",
        ["menu_size"] = "the old Menu size row, gone from the menu (the game's menus have one size): the game keeps the line and reads nothing from it",
        ["dynamic_resolution"] = "the Render resolution's old key: read once by the game where the file has no resolution line",
        ["debug_tab"] = "an old line (Alt+G's Debug tab): the game drops it at its next save",
        ["rivatuner_limit"] = "an old test builds' line: the game drops it at its next save",
        ["discord_app_id"] = "the Discord status's application, for a test application (host/discord_rpc.c): no row",
    };
    /// <summary>The binding lines of the Controls page's grid: pad_&lt;button&gt;=&lt;input&gt; and key_&lt;button&gt;=&lt;key&gt;,&lt;key&gt;.</summary>
    public static readonly IReadOnlyList<string> KeptPrefixes = new[] { "pad_", "key_" };
    /// <summary>A key that is not a row here but is known (KeptKeys, KeptPrefixes): kept in the file as it is.</summary>
    public static bool IsKept(string key) => KeptKeys.ContainsKey(key) || KeptPrefixes.Any(p => key.StartsWith(p, StringComparison.Ordinal));

    // ---- the Quality preset ----

    /// <summary>What each Quality preset sets (vk_menu.cpp apply_preset), in the menu's own words (hn_preset).</summary>
    public static readonly IReadOnlyList<string> PresetSummaries = new[]
    {
        "Each row as you set it. Changing most rows yourself comes back to Custom.",
        "Resolution 50% (with DLSS its nearest mode), Lanczos, Sharpness 30%, Handheld profile.",
        "Resolution 75% (with DLSS its nearest mode), Lanczos, Sharpness 30%, Handheld profile.",
        "Resolution 100% (with DLSS its nearest mode), Catmull-Rom, Sharpness 30%.",
        "Resolution 100% (with DLSS its nearest mode), Lanczos, Sharpness 40%.",
        "Resolution 100% (with DLSS its nearest mode), Lanczos, Sharpness 50%.",
    };

    /// <summary>The rows a Quality preset sets and their values (apply_preset): the Render resolution (then moved to the
    /// Upscaler's nearest step: GameOptionsFile.Set does that), the Window scaling filter, Sharpness, Frame pacing, Game speed,
    /// the Handheld profile and the Texture detail. Custom (0) sets nothing. The Upscaler and Frame generation stay the
    /// player's. (The game also sets its memory budgets from the preset; they are not kept in the file.)</summary>
    public static IReadOnlyDictionary<string, int> PresetValues(int preset)
    {
        var (res, filter, sharp, handheld) = preset switch
        {
            1 => (4, 2, 30, 1),
            2 => (2, 2, 30, 1),
            3 => (1, 1, 30, 0),
            4 => (1, 2, 40, 0),
            5 => (1, 2, 50, 0),
            _ => (-1, 0, 0, 0),
        };
        if (res < 0) return new Dictionary<string, int>();
        int[] detail = { 0, 3, 2, 1, 1, 1 };
        return new Dictionary<string, int>(StringComparer.Ordinal)
        {
            [ResolutionKey] = res, ["upscaling"] = filter, ["sharpness"] = sharp, ["frame_pacing"] = 1, ["game_speed"] = 1,
            [HandheldKey] = handheld, ["texture_detail"] = detail[preset],
        };
    }
}
