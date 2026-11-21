// nx2/make_game.py as a child process (old Setup.cs Runner, Shell.cs Run/Line/Ended; hookshot_builder.py start_make/on_exit):
//   <python> -I <tools>\nx2\make_game.py <args>      cwd <tools>, stdout+stderr UTF-8, below-normal priority (Windows)
//   env: PYTHONIOENCODING=utf-8, HOOKSHOT_BUILDER_EXE (written into the game folder's builder= line), HOOKSHOT_NGX_SDK when
//        DLSS is on, EMSDK / RX_CPM / WGPU_NATIVE / HOOKSHOT_MINGW for tool folders that exist (Windows); Linux: DLSS on drops
//        HOOKSHOT_VK_PREBUILT (the Vulkan back end is compiled with NGX), DLSS off drops HOOKSHOT_NGX_SDK
// Success is the @@done line (and the game's program in that folder), never the exit code alone. Stop kills the whole tree.
using System.Diagnostics;
using System.Text;

namespace Triforce.Logic;

/// <summary>What to build: a new game folder from a source, or an existing folder again (an update, a mods rebuild).</summary>
public sealed record BuildRequest
{
    public GameSource? Source { get; init; }
    public string? OutFolder { get; init; }
    /// <summary>A language code (Languages); an update keeps the folder's own (make_game reads its language.txt).</summary>
    public string Language { get; init; } = "en-US";
    public string? UpdateFolder { get; init; }
    /// <summary>NVIDIA DLSS: null decides as the old Setup did (the setting, or a folder built with it, Dlss.ForBuild).</summary>
    public bool? Dlss { get; init; }
    /// <summary>Linux "Prepare shaders now" (unticked: --no-prepare-shaders). Windows always prepares them.</summary>
    public bool PrepareShaders { get; init; } = true;
    public IReadOnlyList<string> ExtraArgs { get; init; } = Array.Empty<string>();

    public static BuildRequest New(GameSource source, string outFolder, string language) => new() { Source = source, OutFolder = outFolder, Language = language };
    public static BuildRequest Update(string folder) => new() { UpdateFolder = folder };
    public bool IsUpdate => UpdateFolder != null;
}

public sealed class BuildResult
{
    public bool Success { get; init; }
    public bool Cancelled { get; init; }
    /// <summary>The game folder @@done named.</summary>
    public string? Folder { get; init; }
    /// <summary>Why it stopped, in one sentence: @@error's text, or the runner's own.</summary>
    public string? Failure { get; init; }
    public int? ExitCode { get; init; }
    public TimeSpan Elapsed { get; init; }
    /// <summary>The stage it was in when it stopped.</summary>
    public string? StoppedAt { get; init; }
    /// <summary>"make_game.py &lt;args&gt; (tools &lt;root&gt;)" for the log and the report.</summary>
    public string? CommandLine { get; init; }
    public string? OutFolder { get; init; }
    public BuildEvent.Times? Times { get; init; }
    /// <summary>The shaders were left to the game's first start (the line that said so).</summary>
    public string? ShadersNotPrepared { get; init; }
    public string? LogFile { get; init; }
}

public sealed class BuildRunner
{
    readonly TriforceContext ctx;
    Process? process;
    volatile bool cancelled;
    readonly StringBuilder details = new();
    readonly object gate = new();

    public BuildRunner(TriforceContext ctx) { this.ctx = ctx; }

    /// <summary>Every line make_game prints, as it comes (stdout and stderr).</summary>
    public event Action<string>? Line;
    /// <summary>Every line as a typed event.</summary>
    public event Action<BuildEvent>? Event;
    /// <summary>A stage began (the one before ended): the App moves its rows and plays the step sound.</summary>
    public event Action<string?, string>? StageChanged;

    /// <summary>The build's state while it runs (stage, progress, eta...).</summary>
    public BuildTracker Tracker { get; private set; } = new();
    /// <summary>The time estimate this build started with (BuildEstimator), for BuildClock.Remaining.</summary>
    public BuildEstimate? Estimate { get; private set; }
    public bool Running => process is { HasExited: false };

    /// <summary>All the lines so far but @@progress (the details drawer; the report's "build's last lines"). Cut in half past
    /// 4000 lines, as the old details box was.</summary>
    public string Details { get { lock (gate) return details.ToString(); } }

    /// <summary>Every line of the build, @@progress too, in &lt;work&gt;\launcher-build.log (the one before as launcher-build.old.log):
    /// the build's own words kept for a bug report (research/12 4.5 asked for it; the old Setup kept them in memory only).</summary>
    public string LogFile => Path.Combine(ctx.WorkFolder, "launcher-build.log");

    /// <summary>make_game.py's arguments for a request (old Shell.cs StartBuild/Run): the source, --out, --language; or --update;
    /// --no-prepare-shaders (Linux, unticked); --fetch-ngx-sdk --accept-nvidia-license with DLSS; --fsr unless --fsr/--no-fsr is
    /// there (Windows: FSR 4 in every build); in a test run --work &lt;root&gt;\build.</summary>
    public static List<string> Arguments(TriforceContext ctx, BuildRequest r, bool dlss)
    {
        var a = new List<string>();
        if (r.UpdateFolder != null) a.AddRange(new[] { "--update", r.UpdateFolder });
        else
        {
            if (r.Source == null || r.OutFolder == null) throw new ArgumentException("A new build needs the game files and a folder.");
            a.AddRange(r.Source.Args);
            a.AddRange(new[] { "--out", r.OutFolder, "--language", Languages.Find(r.Language)?.Code ?? "en-US" });
        }
        a.AddRange(r.ExtraArgs);
        if (!r.PrepareShaders && !ctx.IsWindows && !a.Contains("--no-prepare-shaders")) a.Add("--no-prepare-shaders");
        if (dlss && !a.Contains("--fetch-ngx-sdk")) a.AddRange(new[] { "--fetch-ngx-sdk", "--accept-nvidia-license" });
        // (11-15: AMD FSR 4 in every build: its code built in and AMD's two signed libraries fetched beside the game: make_game --fsr)
        if (ctx.IsWindows && !a.Contains("--fsr") && !a.Contains("--no-fsr")) a.Add("--fsr");
        // (a test run works in its own build folder, the one its uninstall removes. Left to itself make_game.py works in the PC's
        // real one and begins by stopping whatever build runs there)
        if (ctx.TestRoot != null && !a.Contains("--work")) a.AddRange(new[] { "--work", Path.Combine(ctx.TestRoot, "build") });
        return a;
    }

    /// <summary>The process: python -I make_game.py args, in the tools folder, with the build's environment.</summary>
    public static ProcessStartInfo StartInfo(TriforceContext ctx, ToolSet tools, IEnumerable<string> args, string? ngxSdk, bool dlss)
    {
        var si = new ProcessStartInfo(tools.Python)
        {
            WorkingDirectory = tools.Root, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
        };
        si.ArgumentList.Add("-I");
        si.ArgumentList.Add(tools.MakeGame);
        foreach (var a in args) si.ArgumentList.Add(a);
        si.Environment["PYTHONIOENCODING"] = "utf-8";
        // (written into the game folder: the game starts the launcher again from here to update, host/hos_update.c)
        if (ctx.IsWindows || string.IsNullOrEmpty(si.Environment.TryGetValue("HOOKSHOT_BUILDER_EXE", out var b) ? b : null))
            si.Environment["HOOKSHOT_BUILDER_EXE"] = ctx.BuilderExe;
        if (dlss && ngxSdk != null) si.Environment["HOOKSHOT_NGX_SDK"] = ngxSdk;
        if (ctx.IsWindows)
        {
            foreach (var (name, folder) in new[] { ("EMSDK", "emsdk"), ("RX_CPM", "cpm"), ("WGPU_NATIVE", "wgpu-native"), ("HOOKSHOT_MINGW", "llvm-mingw") })
            {
                var path = Path.Combine(tools.Root, folder);
                if (Directory.Exists(path)) si.Environment[name] = path;
            }
        }
        else if (dlss) si.Environment.Remove("HOOKSHOT_VK_PREBUILT");   // (build.py compiles hookshot_vk.so with NGX)
        else si.Environment.Remove("HOOKSHOT_NGX_SDK");                  // (one set in the shell is not passed on when unticked)
        return si;
    }

    /// <summary>Runs the build to its end. Never throws for the build's own failures: the result says what happened.</summary>
    public async Task<BuildResult> RunAsync(BuildRequest request, CancellationToken token = default)
    {
        var tools = new Tools(ctx).Find();
        var sw = Stopwatch.StartNew();
        if (tools == null) return new BuildResult { Failure = "The Triforce tools are not here: start Triforce again to download them.", Elapsed = sw.Elapsed };
        // (a test run builds and updates game folders only inside its own folder)
        var target = request.UpdateFolder ?? request.OutFolder;
        if (target != null && !ctx.Allowed(target)) return new BuildResult { Failure = ctx.TestOnly, Elapsed = sw.Elapsed, OutFolder = target };
        var dlss = request.Dlss ?? new Dlss(ctx).ForBuild(request.UpdateFolder);
        var args = Arguments(ctx, request, dlss);
        var line = "make_game.py " + string.Join(" ", args) + " (tools " + tools.Root + ")";
        ctx.Log.Write("build: " + line);
        var history = BuildHistory.Load(ctx.WorkFolder);
        Estimate = BuildEstimator.Estimate(history, request.IsUpdate, Environment.ProcessorCount, Gpus.Main()?.Vendor, shaders: ctx.IsWindows || request.PrepareShaders);
        Tracker = new BuildTracker();
        cancelled = false;
        lock (gate) { details.Clear(); detailLines = 0; }

        StreamWriter? log = null;
        try
        {
            Directory.CreateDirectory(ctx.WorkFolder);
            if (File.Exists(LogFile)) File.Move(LogFile, Path.Combine(ctx.WorkFolder, "launcher-build.old.log"), true);
            log = new StreamWriter(LogFile, false, new UTF8Encoding(false)) { AutoFlush = true };
            log.WriteLine("# " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "  " + line);
        }
        catch (Exception) { log = null; }

        // (stdout and stderr come on two threads: the state is changed under the lock, the App's handlers run outside it)
        void Take(string l)
        {
            BuildEvent e;
            string? was;
            lock (gate)
            {
                try { log?.WriteLine(l); } catch (Exception) { }
                was = Tracker.Stage;
                e = Tracker.Feed(l);
                if (!l.StartsWith("@@progress", StringComparison.Ordinal))
                {
                    details.Append(l).Append('\n');
                    if (++detailLines > 4000)
                    {
                        details.Remove(0, details.Length / 2);
                        detailLines = CountLines();
                    }
                }
            }
            if (e is BuildEvent.Stage s) StageChanged?.Invoke(was, s.Id);
            Event?.Invoke(e);
            Line?.Invoke(l);
        }

        int? code = null;
        string? failure = null;
        try
        {
            var si = StartInfo(ctx, tools, args, ctx.NgxSdkFolder, dlss);
            using var p = ctx.Platform.StartTree(si);
            process = p;
            try { if (ctx.IsWindows) p.PriorityClass = ProcessPriorityClass.BelowNormal; } catch (Exception) { }
            try { p.StandardInput.Close(); } catch (Exception) { }
            p.ErrorDataReceived += (_, e) => { if (e.Data != null) Take(e.Data); };
            p.BeginErrorReadLine();
            using (token.Register(Stop))
            {
                for (string? l; (l = await p.StandardOutput.ReadLineAsync(CancellationToken.None).ConfigureAwait(false)) != null;) Take(l);
                await p.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
                p.WaitForExit();   // (the stderr reader drains)
            }
            code = p.ExitCode;
        }
        catch (Exception e) { failure = "The build tools could not start: " + e.Message; }
        finally
        {
            process = null;
            try { log?.Dispose(); } catch (Exception) { }
        }
        Tracker.End();
        sw.Stop();

        var done = Tracker.DoneFolder;
        var ok = failure == null && !cancelled && done is { Length: > 0 } && new Game(ctx).FindExe(done) != null;
        if (!ok && failure == null)
            failure = cancelled ? "cancelled"
                : done is { Length: > 0 } ? "The build said it finished, but there is no game in " + done + "."
                : code == 0 ? "The build stopped."
                : Tracker.Error ?? "The build tools stopped without saying why. See the details.";
        var result = new BuildResult
        {
            Success = ok, Cancelled = cancelled, Folder = ok ? done : null, Failure = ok ? null : failure, ExitCode = code, Elapsed = sw.Elapsed,
            StoppedAt = ok ? null : Tracker.Stage, CommandLine = line, OutFolder = target, Times = Tracker.Times, ShadersNotPrepared = Tracker.NotPrepared,
            LogFile = log != null ? LogFile : null,
        };
        ctx.Log.Write("build ended: " + (ok ? "done, " + done : failure) + " after " + BuildClock.Span(sw.Elapsed));
        if (ok) Finished(done!, request);
        return result;
    }

    int detailLines;

    int CountLines()
    {
        var n = 0;
        for (var i = 0; i < details.Length; i++) if (details[i] == '\n') n++;
        return n;
    }

    /// <summary>A build that ended well (old Shell.cs Ended): GameFolder = the @@done folder, the mods' code marked as built, the
    /// stage times kept, Linux's menu entry written, and a notification (the App shows it when its window is not in front).</summary>
    void Finished(string folder, BuildRequest request)
    {
        ctx.Settings.GameFolder = folder;
        try { new ModManager(ctx).MarkBuilt(folder); } catch (Exception e) { ctx.Log.Write("mods: the build's mark could not be kept: " + e.Message); }
        var stages = Tracker.StageSeconds.Count > 0 ? (IReadOnlyDictionary<string, double>)Tracker.StageSeconds : Tracker.ReportedSteps;
        if (stages != null && stages.Count > 0) BuildHistory.RecordStages(ctx.WorkFolder, request.IsUpdate ? "update" : "build", stages);
        if (!ctx.IsWindows)
        {
            try
            {
                var crest = new Tools(ctx).Find() is { } t ? Path.Combine(t.Root, "assets", "triforce.png") : null;
                new Game(ctx).AddToMenu(folder, crest);
            }
            catch (Exception e) { ctx.Log.Write("menu entry: " + e.Message); }
        }
        ctx.Platform.Notify("Your game is ready", request.IsUpdate ? "The update is done. Press Play." : "Triforce built your game. Press Play.");
    }

    /// <summary>Stop: the whole tree killed (taskkill /T /F; Linux the process group), waiting up to 5 s. The result says "cancelled".
    /// What is done is kept: a new build into the same folder reuses the unpacked game files, the translation and the objects.</summary>
    public void Stop()
    {
        cancelled = true;
        var p = process;
        if (p != null) ctx.Platform.KillTree(p);
    }
}
