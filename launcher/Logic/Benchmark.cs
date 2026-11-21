// "Find my best settings" (DESIGN 12): the game visits four places by itself, twice (the player's settings uncapped, then with
// an upscaler), the frame rates are read from its log, and a small rule table says what to change.
//
// Safety, in this order:
//   * the saves: the game runs on a COPY of the game folder's user\ (--user <copy>), so whatever it writes goes there and the
//     player's own saves are never touched (no switch in the game needed: the host takes --user, as the lab scripts use it);
//   * the options file: triforce_options.ini (beside the game's exe, where the game reads it) is copied aside first and put
//     back afterwards whatever happens; a "restore.txt" marker in the work folder lets the next launcher start put it back if
//     this one was killed in the middle (RestoreIfLeft);
//   * the game: refused while the player's game runs; the benchmark's own game is closed at the end, and on Stop.
// The game's switches it uses are its own test hooks: HOS_OOT_CHEATS=1 (the teleports, "!cheat 1000+n" through
// HOS_INPUT_LIVE, host/hos_main.c oot_destinations), HOS_OOT_PROTECT_HEARTS=1, HOS_INPUT_LIVE (presses: title, Continue, Load).
// The log lines read: "vi: N frames presented (X fps over the last 5.0 s, ...)", "vi: 1% low X fps over the last ..." (added
// with this, host/nn_vi.c; a game built before it gives the slowest 5 s window instead), "presents, X fps; a frame: N draws".
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace Triforce.Logic;

public sealed record BenchmarkRequest(string GameFolder, bool TryUpscaler = true, TimeSpan? PerPlace = null);

/// <summary>Where it is: Phase "boot" | "next" | "place" | "done"; Fraction of the whole run.</summary>
public sealed record BenchmarkProgress(string Phase, string Place, int Pass, int Passes, double? Fps, double Fraction, string Text);

/// <summary>One place's measurement: average fps and the 1 % low (LowIsWindow: the game had no 1 % line, so it is the slowest
/// 5 s average).</summary>
public sealed record PlaceResult(string Place, double AverageFps, double OnePercentLow, bool LowIsWindow = false)
{
    public double Low01 { get; init; }
    public IReadOnlyList<double> FrameTimes { get; init; } = Array.Empty<double>();
}

/// <summary>One benchmark run as the game wrote it.</summary>
public sealed record BenchRun(IReadOnlyList<PlaceResult> Scenes, string? Gpu, bool Stopped, string? Error, TimeSpan Took)
{
    /// <summary>((Triforce, 11-19) the places the game would not take Link to, left out (host/hos_bench.c skip_scene)</summary>
    public IReadOnlyList<string> Skipped { get; init; } = Array.Empty<string>();
}

/// <summary>A setting the benchmark would change: its triforce_options.ini key, the menu's row name, the value now and the
/// recommended one (the App shows them with the menu's own labels).</summary>
public sealed record SettingChange(string Key, string Label, int From, int To)
{
    public bool Changes => From != To;
}

public sealed record BenchmarkResult(IReadOnlyList<PlaceResult> Pass1, IReadOnlyList<PlaceResult>? Pass2, string? Pass2Name,
    IReadOnlyList<SettingChange> Recommended, string Summary, TimeSpan Took);

public interface IBenchmarkRunner
{
    Task<BenchmarkResult> RunBenchmarkAsync(BenchmarkRequest request, IProgress<BenchmarkProgress>? progress, CancellationToken token);
    void Apply(string gameFolder, IReadOnlyList<SettingChange> changes);
}

public sealed record BenchPlace(string Name, int Teleport);

public sealed class Benchmark : IBenchmarkRunner
{
    readonly TriforceContext ctx;
    public Benchmark(TriforceContext ctx) { this.ctx = ctx; }

    /// <summary>The tour (host/hos_main.c oot_destinations: 0 Kokiri Forest, 1 Hyrule Field, 3 Market, 7 Lake Hylia): open
    /// field, a crowd, foliage, water.</summary>
    public static readonly IReadOnlyList<BenchPlace> Places = new BenchPlace[]
    {
        new("Hyrule Field", 1001), new("Castle Town Market", 1003), new("Kokiri Forest", 1000), new("Lake Hylia", 1007),
    };

    // the options file's keys (gpu/vk/vk_menu.cpp ini_key; values gpu/vk/vk_menu.h, upscaler_rows.h, fps_cap.h)
    public const string KCap = "frame_rate_cap", KVsync = "vsync", KSteady = "steady_frame_rate", KUpscaler = "dlss",
        KResolution = "resolution", KPreset = "quality_preset", KFrameGen = "frame_generation";
    // frame_rate_cap: 0 60, 1 90, 2 120, 3 144, 4 165, 5 240, 6 the monitor's. dlss (the Upscaler row): 0 the game's, 1 DLSS,
    // 3 FSR 3.1, 4 FSR 4. resolution: 3 = 67 % (Quality), 5 = 58 % (Balanced), 4 = 50 % (Performance). quality_preset 0 Custom.
    static readonly (int value, int fps)[] Caps = { (0, 60), (2, 120), (3, 144), (5, 240) };
    public const int UpGame = 0, UpDlss = 1, UpFsr3 = 3, ResQuality = 3, ResBalanced = 5, CapUncapped = 5;
    public const int SteadyRefreshSteps = 2;   // (fps_cap.h: 0 Off, 1 Any rate, 2 Refresh steps)

    public string WorkFolder => Path.Combine(ctx.WorkFolder, "benchmark");
    string Marker => Path.Combine(WorkFolder, "restore.txt");
    static string OptionsOf(string game) => Path.Combine(game, "triforce_options.ini");

    /// <summary>Puts a player's options file back when a benchmark was cut off (the launcher killed in the middle). Call at start.</summary>
    public void RestoreIfLeft()
    {
        try
        {
            if (!File.Exists(Marker)) return;
            var lines = File.ReadAllLines(Marker);
            var target = lines.ElementAtOrDefault(0);
            var backup = lines.ElementAtOrDefault(1);
            if (target != null && ctx.AllowedForGame(target))
            {
                if (backup == "none") File.Delete(target);
                else if (backup != null && File.Exists(backup)) File.Copy(backup, target, true);
                ctx.Log.Write("benchmark: the options file put back after a cut-off run: " + target);
            }
            File.Delete(Marker);
        }
        catch (Exception e) { ctx.Log.Write("benchmark: putting the options back: " + e.Message); }
    }

    /// <summary>What the second pass would use on this PC, or null (DLSS when the game was built with it on an NVIDIA card,
    /// else AMD FSR 3.1, which runs everywhere; none when the player already uses an upscaler).</summary>
    public (int upscaler, string name)? SecondPass(string game, IReadOnlyDictionary<string, int> now)
    {
        if (now.TryGetValue(KUpscaler, out var u) && u != UpGame) return null;
        if (Gpus.HasNvidia() && new Game(ctx).HasDlss(game)) return (UpDlss, "NVIDIA DLSS");
        return (UpFsr3, "AMD FSR 3.1");
    }

    /// <summary>Does this game have benchmark mode (a build from v0.5.2 on: host/ answers HOS_BENCHMARK)? Its exe names the
    /// switch; read once per exe file (size + time).</summary>
    public static bool Supports(string exe)
    {
        try
        {
            var fi = new FileInfo(exe);
            var key = exe + "|" + fi.Length + "|" + fi.LastWriteTimeUtc.Ticks;
            lock (supportCache) if (supportCache.TryGetValue(key, out var known)) return known;
            var needle = Encoding.ASCII.GetBytes("HOS_BENCHMARK");
            var found = false;
            using (var f = File.OpenRead(exe))
            {
                var buf = new byte[1 << 22];
                var keep = needle.Length - 1;
                int got, have = 0;
                while (!found && (got = f.Read(buf, have, buf.Length - have)) > 0)
                {
                    var n = have + got;
                    found = buf.AsSpan(0, n).IndexOf(needle) >= 0;
                    have = Math.Min(keep, n);
                    Array.Copy(buf, n - have, buf, 0, have);
                }
            }
            lock (supportCache) supportCache[key] = found;
            return found;
        }
        catch (Exception) { return false; }
    }
    static readonly Dictionary<string, bool> supportCache = new();

    /// <summary>Supports, or (tests) a test run told to take it as given (TRIFORCE_BENCH_ASSUME=1: the stand-in game).</summary>
    public bool Ready(string exe) => Supports(exe) || ctx.TestRoot != null && ctx.Env("TRIFORCE_BENCH_ASSUME") == "1";

    /// <summary>One benchmark run: the game in its benchmark mode (it loads the newest save by itself, visits the scenes with
    /// its own panel on screen, writes its results and quits). Settings for the run: the player's, uncapped and without VSync,
    /// plus <paramref name="upscaler"/> at Quality when given. Stop: the token (the game is closed and writes "stopped").</summary>
    public async Task<BenchRun> RunOnceAsync(string game, (int upscaler, string name)? upscaler, IProgress<BenchmarkProgress>? progress, CancellationToken token)
    {
        var g = new Game(ctx);
        var exe = g.FindExe(game) ?? throw new BenchmarkException("There is no built game in " + game + ".");
        if (!Ready(exe)) throw new BenchmarkException("Your game was built before the benchmark existed: it comes with the next update.");
        if (g.IsRunning(game)) throw new BenchmarkException("Close the game first: the benchmark starts it by itself.");
        if (new Saves(ctx).Newest(game) == null) throw new BenchmarkException("Play until your first save, then run it: the benchmark starts from your newest save.");
        var ini = OptionsOf(game);
        if (!ctx.AllowedForGame(ini)) throw new BenchmarkException(ctx.TestOnly);
        Directory.CreateDirectory(WorkFolder);
        RestoreIfLeft();
        var userCopy = Path.Combine(WorkFolder, "user");
        if (Directory.Exists(userCopy)) Directory.Delete(userCopy, true);
        CopyUser(ctx.Platform.UserFolder(game), userCopy);   // ((Triforce) 11-19: on Linux it is outside the game folder)
        var backup = Path.Combine(WorkFolder, "options.backup.ini");
        var hadOptions = File.Exists(ini);
        if (hadOptions) File.Copy(ini, backup, true);
        File.WriteAllLines(Marker, new[] { ini, hadOptions ? backup : "none" });
        var results = Path.Combine(WorkFolder, "results.json");
        if (File.Exists(results)) File.Delete(results);
        var t0 = DateTime.UtcNow;
        try
        {
            var keys = new Dictionary<string, int> { [KCap] = CapUncapped, [KVsync] = 0, [KSteady] = 0, [KFrameGen] = 0 };
            if (upscaler is { } u) { keys[KUpscaler] = u.upscaler; keys[KResolution] = ResQuality; keys[KPreset] = 0; }
            BenchIni.Set(ini, keys);
            var si = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = game, RedirectStandardError = true, RedirectStandardOutput = true };
            foreach (var a in new[] { Path.Combine(game, "image.bin"), "--romfs", Path.Combine(game, "romfs"), "--user", userCopy }) si.ArgumentList.Add(a);
            si.Environment["HOS_BENCHMARK"] = results;
            si.Environment["HOS_OOT_CHEATS"] = "1";
            si.Environment["HOS_OOT_PROTECT_HEARTS"] = "1";
            si.Environment["HOS_INPUT_SCRIPT_BACKGROUND"] = "1";
            si.Environment["HOOKSHOT_NO_UPDATE_CHECK"] = "1";
            if (ctx.Env("TRIFORCE_BENCH_GAME_ENV") is { Length: > 0 } extra)   // (tests: e.g. the stand-in game's speed)
                foreach (var kv in extra.Split(';')) if (kv.IndexOf('=') is var i and > 0) si.Environment[kv[..i]] = kv[(i + 1)..];
            var logPath = Path.Combine(WorkFolder, upscaler == null ? "run.log" : "run-upscaler.log");
            using var log = new StreamWriter(logPath, false, new UTF8Encoding(false)) { AutoFlush = true };
            var gate = new object();
            string? state = null;
            void Got(string? l)
            {
                if (l == null) return;
                lock (gate) log.WriteLine(l);
                // the game's own progress lines ("bench: ..."), for the launcher's line while it runs
                var at = l.IndexOf("bench: ", StringComparison.Ordinal);
                if (at >= 0) state = l[(at + 7)..].Trim();
            }
            var p = Process.Start(si) ?? throw new BenchmarkException("The game would not start.");
            GameJob.Tie(p);
            p.ErrorDataReceived += (_, e) => Got(e.Data);
            p.OutputDataReceived += (_, e) => Got(e.Data);
            p.BeginErrorReadLine(); p.BeginOutputReadLine();
            ctx.Log.Write("benchmark: started the game in benchmark mode (pid " + p.Id + ")" + (upscaler is { } up2 ? " with " + up2.name : ""));
            using (token.Register(() => { try { if (!p.HasExited && !(p.CloseMainWindow() && p.WaitForExit(15000))) p.Kill(true); } catch { } }))
            {
                while (!p.HasExited)
                {
                    progress?.Report(new BenchmarkProgress("run", state ?? "", 1, 1, null, 0, state ?? "Starting the game"));
                    try { await Task.Delay(500, token); } catch (OperationCanceledException) { break; }
                }
                try { await p.WaitForExitAsync(CancellationToken.None).WaitAsync(TimeSpan.FromSeconds(20)); } catch { }
            }
            token.ThrowIfCancellationRequested();
            if (!File.Exists(results)) throw new BenchmarkException("The game closed without results" + (p.HasExited ? " (exit code " + p.ExitCode + ")" : "") + ". Its log is in " + logPath + ".");
            var run = ReadResults(results, DateTime.UtcNow - t0);
            if (run.Stopped) throw new OperationCanceledException();
            if (run.Scenes.Count == 0) throw new BenchmarkException(run.Error ?? "The benchmark measured nothing.");
            return run;
        }
        finally
        {
            try
            {
                if (hadOptions) File.Copy(backup, ini, true); else if (File.Exists(ini)) File.Delete(ini);
                File.Delete(Marker);
            }
            catch (Exception e) { ctx.Log.Write("benchmark: putting the options back: " + e.Message); }
            try { Directory.Delete(userCopy, true); } catch { }
        }
    }

    /// <summary>The game's results file: {"version":1,"stopped":false,"gpu":..,"error":..,"scenes":[{name,avg,low1,low01,frames,seconds,frametimes_ms}]}.</summary>
    public static BenchRun ReadResults(string path, TimeSpan took)
    {
        using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(path));
        var r = doc.RootElement;
        var scenes = new List<PlaceResult>();
        if (r.TryGetProperty("scenes", out var arr))
            foreach (var s in arr.EnumerateArray())
            {
                double D(string k) => s.TryGetProperty(k, out var v) && v.TryGetDouble(out var d) ? d : 0;
                var ft = new List<double>();
                if (s.TryGetProperty("frametimes_ms", out var f)) foreach (var x in f.EnumerateArray()) if (x.TryGetDouble(out var d)) ft.Add(d);
                scenes.Add(new PlaceResult(s.TryGetProperty("name", out var n) ? n.GetString() ?? "" : "", D("avg"), D("low1"), false) { Low01 = D("low01"), FrameTimes = ft });
            }
        var skipped = new List<string>();
        if (r.TryGetProperty("skipped", out var sk) && sk.ValueKind == System.Text.Json.JsonValueKind.Array)
            foreach (var x in sk.EnumerateArray()) if (x.GetString() is { Length: > 0 } name) skipped.Add(name);
        return new BenchRun(scenes, r.TryGetProperty("gpu", out var gpu) ? gpu.GetString() : null,
            r.TryGetProperty("stopped", out var st) && st.ValueKind == System.Text.Json.JsonValueKind.True,
            r.TryGetProperty("error", out var er) ? er.GetString() : null, took) { Skipped = skipped };
    }

    /// <summary>The old interface: one run, then the rules (the App runs the upscaler pass on its own, when asked).</summary>
    public async Task<BenchmarkResult> RunBenchmarkAsync(BenchmarkRequest request, IProgress<BenchmarkProgress>? progress, CancellationToken token)
    {
        var now = BenchIni.Read(OptionsOf(request.GameFolder));
        var run = await RunOnceAsync(request.GameFolder, null, progress, token);
        var recs = Recommend(now, run.Scenes, null, null, DisplayHz());
        return new BenchmarkResult(run.Scenes, null, null, recs, Summary(run.Scenes, null, null) + SkippedNote(run.Skipped), run.Took);
    }

    static void CopyUser(string from, string to)
    {
        Directory.CreateDirectory(to);
        if (!Directory.Exists(from)) return;
        foreach (var f in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(from, f);
            if (rel.StartsWith("temp", StringComparison.OrdinalIgnoreCase) || Path.GetFileName(f).Equals("save.lock", StringComparison.OrdinalIgnoreCase)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(to, rel))!);
            File.Copy(f, Path.Combine(to, rel), true);
        }
    }
    /// <summary>DESIGN 12's rules. <paramref name="now"/>: the player's options; second: pass 2's upscaler.</summary>
    public static List<SettingChange> Recommend(IReadOnlyDictionary<string, int> now, IReadOnlyList<PlaceResult> p1, IReadOnlyList<PlaceResult>? p2,
        (int upscaler, string name)? second, int displayHz)
    {
        int Now(string k, int d) => now.TryGetValue(k, out var v) ? v : d;
        var recs = new List<SettingChange>();
        var worst1 = p1.Min(r => r.OnePercentLow);
        var use2 = p2 != null && second != null && p2.Min(r => r.OnePercentLow) >= worst1 * 1.15;
        var chosen = use2 ? p2! : p1;
        var worstLow = chosen.Min(r => r.OnePercentLow);
        // the upscaler: the second pass's, when it raises the worst place's 1 % low by 15 % or more
        recs.Add(new SettingChange(KUpscaler, "Upscaler", Now(KUpscaler, UpGame), use2 ? second!.Value.upscaler : Now(KUpscaler, UpGame)));
        if (use2) recs.Add(new SettingChange(KResolution, "Render resolution", Now(KResolution, 1), worstLow < 60 ? ResBalanced : ResQuality));
        // the frame rate cap: the highest the worst place's low stays above, never above the display's refresh
        var cap = 0;
        foreach (var (value, fps) in Caps)
            if (worstLow >= fps && (displayHz <= 0 || fps <= displayHz + 1)) cap = value;
        recs.Add(new SettingChange(KCap, "Game frame rate cap", Now(KCap, 0), cap));
        // steady frame rate: on where the lows fall under 80 % of the average anywhere. ((Triforce) 11-19, ticket "screen tearing":
        // "Any rate" (1) held 36 fps on an RTX 2060 Max-Q's 60 Hz screen, frames shown for one refresh, then two: uneven, seen as
        // tearing. "Refresh steps" (2) holds rates that divide the refresh (60, 30), even on a screen without G-Sync/FreeSync)
        var uneven = chosen.Any(r => r.OnePercentLow < r.AverageFps * 0.8);
        recs.Add(new SettingChange(KSteady, "Steady frame rate", Now(KSteady, 0), uneven ? SteadyRefreshSteps : Now(KSteady, 0)));
        // frame generation: never turned on by this (DESIGN 12: only below 60 with DLSS-G / FSR 4 FG, not measured yet)
        recs.Add(new SettingChange(KFrameGen, "Frame generation", Now(KFrameGen, 0), Now(KFrameGen, 0)));
        if (recs.Any(r => r.Changes && (r.Key == KUpscaler || r.Key == KResolution)))
            recs.Add(new SettingChange(KPreset, "Quality preset", Now(KPreset, 0), 0));
        return recs;
    }

    /// <summary>((Triforce, 11-19) the verdict's note for places left out ("" when none)</summary>
    public static string SkippedNote(IReadOnlyList<string> skipped) => skipped.Count == 0 ? ""
        : " " + string.Join(" and ", skipped) + (skipped.Count == 1 ? " was" : " were") + " left out: the game would not take Link there from your save.";

    public static string Summary(IReadOnlyList<PlaceResult> p1, IReadOnlyList<PlaceResult>? p2, string? second)
    {
        var w1 = p1.Min(r => r.OnePercentLow);
        var s = w1 >= 60 ? $"Your PC holds <b>above 60 fps everywhere</b> at your settings (the slowest place's low: {w1:0})."
                         : $"At your settings the slowest place dips to {w1:0} fps.";
        if (p2 != null && second != null)
        {
            var w2 = p2.Min(r => r.OnePercentLow);
            s += w2 >= w1 * 1.15 ? $" With {second} the slowest place holds {w2:0}, so it is worth turning on."
                                 : $" {second} changed little here ({w2:0}), so it stays as it is.";
        }
        return s;
    }

    /// <summary>The main display's refresh rate (Windows), 0 when not known.</summary>
    public static int DisplayHz()
    {
        if (!OperatingSystem.IsWindows()) return 0;
        try
        {
            var dm = new DEVMODE { dmSize = (short)System.Runtime.InteropServices.Marshal.SizeOf<DEVMODE>() };
            return EnumDisplaySettings(null, -1, ref dm) ? dm.dmDisplayFrequency : 0;
        }
        catch { return 0; }
    }
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential, CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    struct DEVMODE
    {
        [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    }
    [System.Runtime.InteropServices.DllImport("user32", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    static extern bool EnumDisplaySettings(string? device, int mode, ref DEVMODE dm);

    /// <summary>Apply: the recommended rows into the game folder's options file (only those that change; every other line
    /// as it is). The caller refuses while the game runs.</summary>
    public void Apply(string gameFolder, IReadOnlyList<SettingChange> changes)
    {
        var ini = OptionsOf(gameFolder);
        if (!ctx.AllowedForGame(ini)) throw new BenchmarkException(ctx.TestOnly);
        BenchIni.Set(ini, changes.Where(c => c.Changes).ToDictionary(c => c.Key, c => c.To));
    }
}

public sealed class BenchmarkException : Exception
{
    public BenchmarkException(string message) : base(message) { }
}

/// <summary>triforce_options.ini as plain key=value lines: read, and keys set in place (a missing key added at the end), the
/// file written whole beside and moved over it (as the game's own file_whole.h: a reader never meets half a file).</summary>
public static class BenchIni
{
    public static Dictionary<string, int> Read(string path)
    {
        var d = new Dictionary<string, int>(StringComparer.Ordinal);
        if (!File.Exists(path)) return d;
        foreach (var l in File.ReadAllLines(path))
            if (l.IndexOf('=') is var i and > 0 && int.TryParse(l[(i + 1)..].Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var v)) d[l[..i].Trim()] = v;
        return d;
    }

    public static void Set(string path, IReadOnlyDictionary<string, int> keys)
    {
        var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : new List<string>();
        var left = new Dictionary<string, int>(keys);
        for (var n = 0; n < lines.Count; n++)
            if (lines[n].IndexOf('=') is var i and > 0 && left.Remove(lines[n][..i].Trim(), out var v)) lines[n] = lines[n][..i].Trim() + "=" + v.ToString(CultureInfo.InvariantCulture);
        foreach (var (k, v) in left) lines.Add(k + "=" + v.ToString(CultureInfo.InvariantCulture));
        var fresh = path + ".new";
        File.WriteAllText(fresh, string.Join("\n", lines) + "\n");
        File.Move(fresh, path, true);
    }
}

/// <summary>Windows: a job object that kills what it holds when its last handle closes, so a game the benchmark started never
/// outlives the launcher (a crash, Task Manager). Elsewhere: nothing (the process group ends with Stop).</summary>
static class GameJob
{
    static IntPtr job;
    public static void Tie(Process p)
    {
        if (!OperatingSystem.IsWindows()) return;
        try
        {
            if (job == IntPtr.Zero)
            {
                job = CreateJobObject(IntPtr.Zero, null);
                var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
                info.BasicLimitInformation.LimitFlags = 0x2000;   // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                var size = System.Runtime.InteropServices.Marshal.SizeOf(info);
                var ptr = System.Runtime.InteropServices.Marshal.AllocHGlobal(size);
                System.Runtime.InteropServices.Marshal.StructureToPtr(info, ptr, false);
                SetInformationJobObject(job, 9 /* ExtendedLimitInformation */, ptr, (uint)size);
                System.Runtime.InteropServices.Marshal.FreeHGlobal(ptr);
            }
            AssignProcessToJobObject(job, p.Handle);
        }
        catch (Exception) { }
    }
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    struct JOBOBJECT_BASIC_LIMIT_INFORMATION { public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public uint LimitFlags; public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize; public uint ActiveProcessLimit; public UIntPtr Affinity; public uint PriorityClass, SchedulingClass; }
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    struct IO_COUNTERS { public ulong a, b, c, d, e, f; }
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION { public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation; public IO_COUNTERS IoInfo; public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed; }
    [System.Runtime.InteropServices.DllImport("kernel32", CharSet = System.Runtime.InteropServices.CharSet.Unicode)] static extern IntPtr CreateJobObject(IntPtr a, string? name);
    [System.Runtime.InteropServices.DllImport("kernel32")] static extern bool SetInformationJobObject(IntPtr job, int cls, IntPtr info, uint size);
    [System.Runtime.InteropServices.DllImport("kernel32")] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
}
