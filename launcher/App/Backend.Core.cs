using System.Diagnostics;
using System.Text;
using System.Text.Json.Nodes;
using Triforce.Core.GameFiles;
using Triforce.Logic;
using LogicSource = Triforce.Logic.GameSource;

namespace Triforce.Launcher;

/// <summary>First run (the game handed over, the reveal), the build, home, update and Play.</summary>
public sealed partial class Backend
{
    readonly HttpClient http = Downloader.CreateHttp();
    Release? release;
    Failure? checkFailed;
    bool checking;

    // first run
    LogicSource? source;
    string? outFolder;

    // the working screen
    CancellationTokenSource? workCts;
    BuildRunner? runner;
    string? workKind;            // "build" | "update"
    Func<Task>? retry;
    readonly StringBuilder logOut = new();
    Timer? logTimer;
    BuildResult? lastResult;
    // ((Triforce) 11-19, ticket "this is what i get on the new launcher": a failure before the build itself (the tools'
    // download, the check) left no BuildResult, and Copy report said "stopped at its start after 0 s: no reason given"; the
    // reason was in launcher.log only. Now the report says it, and setup.log has it too)
    string? lastFailure;

    // the game
    Process? gameProc;
    Timer? runningTimer;

    LauncherColorChoice colorChoice = null!;   // (the page's Color scheme: what state.color sends; RegisterCore)
    int colorRefused;            // (1 once a 'color' that was not a #rrggbb is logged)

    partial void AddSettingsState(JsonObject state);

    void RegisterCore()
    {
        OnAsync("build", _ => BuildFirst());
        On("stop", _ => StopWork());
        OnAsync("retry", _ => retry != null ? retry() : Task.CompletedTask);
        On("workback", _ => { StopWork(); Go(workKind == "update" ? "home" : "reveal"); });
        On("play", _ => Play());
        On("playold", _ => Play());
        On("showgame", _ => ShowGame());
        OnAsync("update", _ => UpdateNow(start.Args.UpdateFolder));
        On("forget", _ => { source = null; UseMedia(GameDir); });
        On("choose", m =>
        {
            switch ((string?)m["id"])
            {
                case "lang":
                    var cur = ctx.Settings.Language ?? Languages.Default().Code;
                    Choice("Game language", "The language the game's text and voices are in.", "lang",
                        Languages.All.Select(l => (l.Code, l.NativeName, l.NativeName != l.EnglishName ? l.EnglishName : (string?)null, l.Code == cur)), "Choose");
                    break;
                case "folder":
                    PickThen(true, "Where should the game go?", p =>
                    {
                        outFolder = Path.GetFileName(Path.TrimEndingDirectorySeparator(p)).Equals("Triforce", StringComparison.OrdinalIgnoreCase) ? p : Path.Combine(p, "Triforce");
                        SendReveal();
                    });
                    break;
                case "dlss":
                    var d = new Dlss(ctx);
                    if (d.Chosen) { d.TurnOff(); SendReveal(); break; }
                    Ask("NVIDIA DLSS", "<p>" + H(d.LicenseText).Replace("\n\n", "</p><p>").Replace("\n", "<br>") + "</p>", "dlssanswer",
                        new[] { ("no", "Leave it out", false), ("yes", Dlss.AcceptLabel, true) });
                    break;
            }
        });
        On("lang", m => { if ((string?)m["id"] is { } code) { ctx.Settings.Language = code; SendReveal(); } });
        On("dlssanswer", m => { if ((string?)m["id"] == "yes") new Dlss(ctx).Accept(); SendReveal(); });
        On("screen", _ => { });
        On("open", m => OpenThing((string?)m["arg"] ?? ""));
        On("report", _ => Copy(Report.ForBuild(ctx, lastResult, runner?.Details ?? "", new Tools(ctx).InstalledVersion, lastFailure)));
        On("sound", m =>
        {
            ctx.Settings.MusicVolume = (int)Math.Round(((double?)m["music"] ?? .4) * 100);
            ctx.Settings.SoundVolume = (int)Math.Round(((double?)m["sfx"] ?? .7) * 100);
            ctx.Settings.Muted = (bool?)m["muted"] ?? false;
        });
        // (the page's Color scheme row: ColorArrived takes the color as the message arrives; this keeps the newest one)
        colorChoice = new LauncherColorChoice(ctx.Settings);
        On("color", _ => colorChoice.Keep());
        On("closeanswer", m => { if ((string?)m["id"] == "yes") { StopEverything(); host.Close(); } });
        On("uninstallanswer", m =>
        {
            if ((string?)m["id"] != "yes") { host.Close(); return; }
            new Uninstaller(ctx).Run();
            host.Close();
        });
        ctx.Platform.NotificationRequested += (_, n) => { Log.Write("notice: " + n.Title + " · " + n.Text); host.Attention(n.Title, n.Text); };
    }

    /// <summary>The page's 'color', called by OnMessage as it arrives, before its handler goes to a worker: only a "#rrggbb" is
    /// taken (the color state.color sends from then on, also when nothing is written); anything else is left out, with one log
    /// line a run.</summary>
    void ColorArrived(JsonNode m)
    {
        if (colorChoice.Take(m) != null || Interlocked.Exchange(ref colorRefused, 1) != 0) return;
        var what = m["hex"]?.ToJsonString() ?? "(none)";
        Log.Write("color: not a #rrggbb, left out: " + (what.Length > 40 ? what.Substring(0, 40) + "..." : what));
    }

    // ---------------- start ----------------
    void OnReady()
    {
        Send(new JsonObject { ["t"] = "hello", ["chrome"] = host.CustomChrome, ["os"] = OperatingSystem.IsWindows() ? "windows" : "linux" });
        var s = ctx.Settings;
        var state = new JsonObject { ["sound"] = new JsonObject { ["music"] = s.MusicVolume / 100.0, ["sfx"] = s.SoundVolume / 100.0, ["muted"] = s.Muted }, ["color"] = colorChoice.Current };
        AddSettingsState(state);
        SendState(state);
        if (start.SignalUpWhenShown) Startup.SignalUp(ctx);
        if (start.Screen == StartScreen.Uninstall)
        {
            var plan = new Uninstaller(ctx).Describe();
            Ask("Uninstall Triforce?", "<p><b>Removes</b> " + H(string.Join(", ", plan.Removes)) + "</p><p><b>Keeps</b> " + H(string.Join(", ", plan.Keeps)) + "</p>",
                "uninstallanswer", new[] { ("no", "Keep it", false), ("yes", "Uninstall", true) });
            return;
        }
        var game = GameDir;
        if (game != null)
        {
            UseMedia(game);
            RefreshHome();
            var running = new Game(ctx).IsRunning(game);
            Go(running ? "playing" : "home");
            if (running) WatchGame();
        }
        else
        {
            SendMedia();
            Go("welcome");
            // testing: hand over a game as if dropped (test runs only)
            if (ctx.TestRoot != null && ctx.Env("TRIFORCE_TEST_INTAKE") is { Length: > 0 } ti) _ = Intake(ti);
        }
        _ = Guarded("check", CheckRelease);
    }

    /// <summary>Asks the release server what is out (never blocks the window: home shows at once, the answer fills it in).</summary>
    async Task CheckRelease()
    {
        if (checking) return;
        checking = true;
        Exception? error = null;
        try
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(20));
            release = await new Updater(ctx).CheckAsync(http, cts.Token);
        }
        catch (Exception e) { error = e; }
        finally { checking = false; }
        var d = new Updater(ctx).Decide(start.Args, release, error);
        checkFailed = d.Failure;
        Log.Write("release check: " + d.Outcome + (release != null ? " " + release.Tag : "") + (d.Failure != null ? " · " + d.Failure.Title : ""));
        RefreshHome();
        // Started to carry on an update (--merge after the launcher swapped itself, or --update from the game): do it now.
        if (release != null && GameDir != null && (d.Outcome == CheckOutcome.Merge || start.Args.UpdateFolder != null))
            await UpdateNow(start.Args.UpdateFolder);
    }

    void RequestClose()
    {
        if (runner?.Running == true || workCts != null)
            Ask(workKind == "update" ? "Close Triforce and stop the update?" : "Close Triforce and stop the build?",
                "<p>What is done so far is kept: next time it carries on from where it stopped.</p>", "closeanswer",
                new[] { ("no", "Keep going", true), ("yes", "Stop and close", false) });
        else host.Close();
    }

    void StopEverything()
    {
        try { workCts?.Cancel(); } catch { }
        try { runner?.Stop(); } catch { }
    }

    // ---------------- first run: the game handed over ----------------
    async Task Intake(string path)
    {
        Send(new JsonObject { ["t"] = "check", ["st"] = "checking", ["name"] = Path.GetFileName(Path.TrimEndingDirectorySeparator(path)) });
        var t0 = DateTime.UtcNow;
        string? bad = null;
        LogicSource? s = null;
        await Task.Run(async () =>
        {
            try
            {
                s = GameIntake.Normalize(path, source);
                if (s == null) { bad = Path.GetFileName(Path.TrimEndingDirectorySeparator(path)) + " is not your game. Drop your .nup, or the folder with romfs and exefs in it."; return; }
                if (s.Kind == SourceKind.Folders && s.Missing is { } miss) { source = s; bad = "That folder has no " + miss + ". Drop the folder that holds both romfs and exefs, or drop the " + miss + " folder too."; return; }
                var q = GameIntake.Quick(s);
                if (!q.Ok) { bad = q.Text; return; }
                if (!q.Final && new Tools(ctx).Find() != null)
                {
                    var full = await new GameIntake(ctx).CheckAsync(s);
                    if (!full.Ok && !full.ToolsMissing) { bad = full.Text; return; }
                }
                if (!UseMedia(s.Kind == SourceKind.Nup ? s.Nup! : s.Romfs!)) Log.Write("intake: no art from " + path + " (the build can still go ahead)");
            }
            catch (Exception e) { bad = e.Message; Log.Write("intake: " + e); }
        });
        var shown = DateTime.UtcNow - t0;
        if (shown < TimeSpan.FromMilliseconds(700)) await Task.Delay(TimeSpan.FromMilliseconds(700) - shown);
        if (bad != null) { Send(new JsonObject { ["t"] = "check", ["st"] = "bad", ["msg"] = bad }); return; }
        source = s;
        outFolder ??= ctx.Settings.GameFolder ?? ctx.DefaultGameFolder;
        Send(new JsonObject { ["t"] = "check", ["st"] = "idle" });
        SendReveal();
        Sfx("success");
        Go("reveal");
    }

    void SendReveal()
    {
        if (source == null) return;
        var lang = Languages.Find(ctx.Settings.Language) ?? Languages.Default();
        var space = new GameIntake(ctx).Space(outFolder!);
        var rows = new JsonArray
        {
            new JsonObject { ["label"] = "Language", ["value"] = lang.NativeName, ["act"] = "lang" },
            new JsonObject { ["label"] = "Where it goes", ["value"] = outFolder, ["act"] = "folder", ["bad"] = space.TooLittle,
                ["small"] = space.Free is { } f ? $"{space.NeedWhileBuilding >> 30} GB while building · {f >> 30} GB free on {space.Disk}" + (space.TooLittle ? " · not enough" : "")
                                                : $"{space.NeedWhileBuilding >> 30} GB while building, about {space.NeedAfter >> 30} GB after" },
        };
        var d = new Dlss(ctx);
        if (d.Available) rows.Add(new JsonObject { ["label"] = "NVIDIA DLSS", ["value"] = d.Chosen ? "Included" : "Not included", ["act"] = "dlss", ["actLabel"] = d.Chosen ? "Leave out" : "Include" });
        var est = Estimate(false);
        var what = source.Kind == SourceKind.Nup ? string.Format("{0:0.0} GB", new FileInfo(source.Nup!).Length / 1e9) : "romfs and exefs";
        SendState(new JsonObject
        {
            ["reveal"] = new JsonObject
            {
                ["found"] = "<b>&#9670; Your game is ready to build</b> <em>· " + H(source.DisplayName) + " · " + H(what) + "</em>",
                ["rows"] = rows, ["canBuild"] = !space.TooLittle,
                ["under"] = est.Text + " on this PC, the tools included · you can keep using it meanwhile",
            },
        });
    }

    BuildEstimate Estimate(bool update) =>
        BuildEstimator.Estimate(BuildHistory.Load(ctx.WorkFolder), update, Environment.ProcessorCount, Gpus.Main()?.Vendor);

    // ---------------- the build (first run) ----------------
    async Task BuildFirst()
    {
        if (source == null) { Go("welcome"); return; }
        var space = new GameIntake(ctx).Space(outFolder!);
        if (space.TooLittle) { Toast("Not enough room on " + H(space.Disk) + ": the build needs " + (space.NeedWhileBuilding >> 30) + " GB free.", "error"); return; }
        retry = BuildFirst;
        workKind = "build";
        using var cts = workCts = new CancellationTokenSource();
        try
        {
            // 1. installed (Windows: the launcher copies itself to its folder, the Apps list, the shortcuts)
            if (ctx.IsWindows && ctx.InstalledDir == null)
            {
                var inst = new Installer(ctx);
                inst.Run(inst.DefaultDir, desktop: true, startMenu: true);
            }
            // 2. the tools
            await EnsureTools(cts.Token);
            // 3. the build
            var lang = (Languages.Find(ctx.Settings.Language) ?? Languages.Default()).Code;
            await RunBuild(BuildRequest.New(source, outFolder!, lang), cts.Token);
        }
        catch (OperationCanceledException) { WorkStopped(); }
        catch (Exception e) { WorkFailed("The build stopped", Failure.Of(e, false).Note, e); }
        finally { workCts = null; }
    }

    /// <summary>Settings: Build the game again (the folder it is in now, same language, mods on).</summary>
    public void StartRebuild()
    {
        var g = GameDir;
        if (g == null) { Go("welcome"); return; }
        if (workCts != null) { Go("work"); return; }
        _ = Guarded("rebuild", async () =>
        {
            retry = () => { StartRebuild(); return Task.CompletedTask; };
            workKind = "update";
            using var cts = workCts = new CancellationTokenSource();
            try
            {
                await EnsureTools(cts.Token);
                await RunBuild(BuildRequest.Update(g), cts.Token);
            }
            catch (OperationCanceledException) { WorkStopped(); }
            catch (Exception e) { WorkFailed("The build stopped", Failure.Of(e, false).Note, e); }
            finally { workCts = null; }
        });
    }

    async Task EnsureTools(CancellationToken token)
    {
        var ti = new ToolsInstaller(ctx);
        if (release == null)
        {
            try { release = await new Updater(ctx).CheckAsync(http, token); }
            catch (Exception) when (new Tools(ctx).Find() != null) { return; }   // offline, but tools are here: build with them
        }
        if (release == null || !ti.Needed(release) && new Tools(ctx).Find() != null) return;
        var rel = release;
        var mb = (rel.ToolsSize > 0 ? rel.ToolsSize : rel.ToolsCodeSize + rel.ToolsBaseSize) >> 20;
        var eb = workKind == "update" ? "Updating to " + rel.Tag : "Building your game";
        void Show(ToolsStep step, double f) => WorkScreen(eb, step == ToolsStep.Unpack ? "Unpacking Triforce's tools" : "Downloading Triforce's tools",
            f * 0.08, Steps((step, f, mb), null), "Keep this window open; you can minimize it.", true);
        var lastPush = DateTime.MinValue;
        ti.Progress += p => { if ((DateTime.UtcNow - lastPush).TotalMilliseconds < 200 && p.Step != ToolsStep.Done) return; lastPush = DateTime.UtcNow; Show(p.Step, p.Fraction); };
        Show(ToolsStep.Download, 0);
        Go("work");
        await ti.UpdateAsync(http, rel, token);
        Sfx("success");
    }

    async Task RunBuild(BuildRequest req, CancellationToken token)
    {
        var r = runner = new BuildRunner(ctx);
        var est = Estimate(req.IsUpdate);
        StartLog();
        r.Line += l => { lock (logOut) logOut.AppendLine(l); };
        var last = DateTime.MinValue;
        var eb = req.IsUpdate ? (release != null && workKind == "update" ? "Updating to " + release.Tag : "Building your game again") : "Building your game";
        void Push(bool force = false)
        {
            if (!force && (DateTime.UtcNow - last).TotalMilliseconds < 250) return;
            last = DateTime.UtcNow;
            var e = r.Estimate ?? est;
            var left = BuildClock.Remaining(e, r.Tracker);
            WorkScreen(eb, BuildClock.LeftText(left), 0.08 + 0.92 * Fraction(e, r.Tracker), Steps(null, (e, r.Tracker)),
                "Keep this window open; you can minimize it. You get a notice when the game is ready.", true,
                req.IsUpdate ? null : new[] { ("Your game", "done"), ("Build", "now"), ("Play", "") });
        }
        r.Event += _ => Push();
        r.StageChanged += (_, _) => Push(true);
        Push(true);
        Go("work");
        using var reg = token.Register(() => r.Stop());
        lastFailure = null;
        var res = lastResult = await r.RunAsync(req, token);
        Push(true);
        FlushLog();
        logTimer?.Dispose(); logTimer = null;
        if (res.Cancelled) { WorkStopped(); return; }
        if (!res.Success) { WorkFailed("The build stopped", res.Failure ?? "It ended without saying why.", null); return; }
        // done
        if (res.Folder != null) { ctx.Settings.GameFolder = res.Folder; UseMedia(res.Folder); }
        var took = BuildClock.Span(res.Elapsed);
        var lead = res.ShadersNotPrepared != null ? "The shaders get prepared the first time you play." : "Shaders ready for your graphics card · your controller works out of the box";
        SendState(new JsonObject { ["done"] = new JsonObject { ["eb"] = (req.IsUpdate ? "Built again in " : "Built in ") + took, ["lead"] = "In " + ShortPath(res.Folder) + " · " + lead } });
        RefreshHome();
        if (req.IsUpdate) { Go("home"); Toast("<b>&#9670;</b> " + (release != null && workKind == "update" ? "Updated to " + H(release.Tag) : "Built again") + " · your saves, mods and shaders stayed", "success"); }
        else Go("done");
    }

    static double Fraction(BuildEstimate e, BuildTracker t)
    {
        if (t.DoneFolder != null) return 1;
        double total = 0, done = 0;
        var stages = BuildStages.All.Select(s => s.Id).ToList();
        var at = t.Stage != null ? stages.IndexOf(t.Stage) : -1;
        for (var i = 0; i < stages.Count; i++)
        {
            var mid = e.Of(stages[i])?.Mid.TotalSeconds ?? 0;
            total += mid;
            if (i < at) done += mid;
            else if (i == at) done += mid * Math.Clamp(t.StageProgress, 0, 1);
        }
        return total > 0 ? Math.Clamp(done / total, 0, 1) : 0;
    }

    /// <summary>The steps list: the tools download (when it happens), then the build's stages, each done / now / to come with its time.</summary>
    JsonArray Steps((ToolsStep step, double frac, long mb)? tools, (BuildEstimate est, BuildTracker tr)? build)
    {
        var a = new JsonArray();
        if (tools is { } ts)
            a.Add(new JsonObject { ["name"] = "Downloading Triforce's tools", ["st"] = "n",
                ["right"] = ts.step == ToolsStep.Unpack ? "unpacking" : ts.mb > 0 ? $"{(long)(ts.frac * ts.mb)} of {ts.mb} MB" : "now" });
        var stages = BuildStages.All;
        var at = build?.tr.Stage is { } cur ? stages.ToList().FindIndex(s => s.Id == cur) : -1;
        var est = build?.est ?? Estimate(false);
        var gpu = Gpus.Main();
        for (var i = 0; i < stages.Count; i++)
        {
            var s = stages[i];
            if (s.Id == "finish") continue;   // (seconds: folded into Compiling for the player)
            string st, right;
            var e = est.Of(s.Id);
            if (build is not { } b) { st = ""; right = e != null ? BuildClock.About(e.Low, e.High) : ""; }
            else if (i < at || b.tr.DoneFolder != null)
            {
                st = "d";
                right = b.tr.StageSeconds.TryGetValue(s.Id, out var sec) ? BuildClock.Span(TimeSpan.FromSeconds(sec)) : "done";
            }
            else if (i == at)
            {
                st = "n";
                var p = b.tr.StageProgress;
                right = b.tr.Plan is { } pl && s.Id == "compile" ? $"{b.tr.FilesBuilt} of {pl.Changed} files" : p > 0 ? $"{(int)(p * 100)} %" : "now";
            }
            else { st = ""; right = e != null ? BuildClock.About(e.Low, e.High) : ""; }
            if (s.Id == "shaders" && st == "" && gpu != null && right.Length > 0) right += " on " + gpu.Name;
            a.Add(new JsonObject { ["name"] = s.Text, ["st"] = st, ["right"] = right });
        }
        return a;
    }

    void WorkScreen(string eb, string title, double frac, JsonArray steps, string note, bool canStop, (string name, string st)[]? path = null)
    {
        var p = new JsonArray();
        foreach (var (n, st) in path ?? Array.Empty<(string, string)>()) p.Add(new JsonObject { ["name"] = n, ["st"] = st });
        Send(new JsonObject
        {
            ["t"] = "work", ["kind"] = workKind, ["eb"] = eb, ["title"] = title, ["lead"] = "", ["frac"] = Math.Clamp(frac, 0, 1), ["steps"] = steps,
            ["note"] = note, ["canStop"] = canStop, ["error"] = null, ["path"] = p,
            ["stopTitle"] = workKind == "update" ? "Stop the update?" : "Stop the build?",
            ["stopText"] = "What is done so far is kept, so it carries on from here next time.",
        });
    }

    void WorkFailed(string title, string text, Exception? e)
    {
        if (e != null) Log.Write("work failed: " + e);
        lastFailure = title + ": " + text + (e != null ? " (" + e.GetType().Name + ": " + e.Message + ")" : "");
        lastResult = null;
        try { ctx.Log.Write("work failed: " + lastFailure); } catch (Exception) { }
        FlushLog();
        Send(new JsonObject
        {
            ["t"] = "work", ["eb"] = workKind == "update" ? "Updating" : "Building your game", ["title"] = title, ["lead"] = text,
            ["error"] = new JsonObject { ["msg"] = text }, ["retry"] = "Try again", ["canStop"] = false, ["path"] = new JsonArray(),
        });
        Go("work");
    }

    void WorkStopped()
    {
        FlushLog();
        Toast(workKind == "update" ? "Update stopped · it carries on from here next time" : "Build stopped · it carries on from here next time");
        Go(workKind == "update" || GameDir != null ? "home" : "reveal");
    }

    void StopWork()
    {
        try { workCts?.Cancel(); } catch { }
        try { runner?.Stop(); } catch { }
    }

    void StartLog()
    {
        lock (logOut) logOut.Clear();
        logTimer?.Dispose();
        logTimer = new Timer(_ => FlushLog(), null, 300, 300);
    }

    void FlushLog()
    {
        string text;
        lock (logOut) { if (logOut.Length == 0) return; text = logOut.ToString(); logOut.Clear(); }
        var lines = new JsonArray();
        foreach (var l in text.Split('\n')) if (l.Length > 0) lines.Add(l.TrimEnd('\r'));
        Send(new JsonObject { ["t"] = "log", ["lines"] = lines });
    }

    // ---------------- update ----------------
    async Task UpdateNow(string? folder)
    {
        if (workCts != null) { Go("work"); return; }
        if (release == null) await CheckRelease();
        if (release == null) { Toast("Could not reach the update server. You can still play.", "error"); return; }
        var rel = release;
        var up = new Updater(ctx);
        var plan = up.Plan(start.Args, rel);
        retry = () => UpdateNow(folder);
        workKind = "update";
        using var cts = workCts = new CancellationTokenSource();
        try
        {
            if (plan.SelfUpdate)
            {
                var eb = "Updating to " + rel.Tag;
                WorkScreen(eb, "Downloading the new launcher", 0, new JsonArray(), "Your saves, mods and shaders stay.", false);
                Go("work");
                var r = await up.SelfUpdateAsync(http, rel, start.Args, start.Lock,
                    p => WorkScreen(eb, "Downloading the new launcher", p.Fraction * 0.1, new JsonArray(), "Your saves, mods and shaders stay.", false),
                    () => host.Minimize(), cts.Token);
                if (r == SelfUpdateResult.CloseNow) { host.Close(); return; }
            }
            if (plan.Tools) await EnsureTools(cts.Token);
            var again = up.RebuildFolderNow(start.Args) ?? folder;
            if (again != null) await RunBuild(BuildRequest.Update(again), cts.Token);
            else { RefreshHome(); Go("home"); Toast("<b>&#9670;</b> Updated to " + H(rel.Tag), "success"); }
        }
        catch (NotStartedException e) { host.Activate(); WorkFailed("The new launcher did not start", e.Message, null); }
        catch (OperationCanceledException) { WorkStopped(); }
        catch (Exception e) { WorkFailed("The update stopped", Failure.Of(e, false).Note, e); }
        finally { workCts = null; }
    }

    // ---------------- home ----------------
    /// <summary>Sends the home, the pill and the files strip as they are now (after a build, an import, the release check...).</summary>
    public void RefreshHome()
    {
        var g = GameDir;
        var state = new JsonObject();
        var files = new JsonArray();
        var running = g != null && new Game(ctx).IsRunning(g);
        if (g != null)
        {
            var slots = new Saves(ctx).Slots(g).Where(s => s.Used && s.Caption != null).OrderByDescending(s => s.Written).ToList();
            for (var i = 0; i < slots.Count; i++)
            {
                var s = slots[i];
                files.Add(new JsonObject
                {
                    ["slot"] = s.FileNumber, ["label"] = "File " + s.FileNumber, ["place"] = null, ["pic"] = SavePicUrl(s), ["last"] = i == 0,
                    ["when"] = When(s.Written), ["meta"] = "Saved " + When(s.Written, true), ["time"] = "",
                });
            }
        }
        state["files"] = files;
        var built = g != null ? Game.BuiltVersion(g) : null;
        var us = release != null ? new Updater(ctx).State(release) : null;
        JsonObject home, pill;
        var links = new JsonArray();
        if (g == null)
        {
            home = new JsonObject { ["mode"] = "missing", ["eb"] = "Needs attention", ["title"] = "No game built yet", ["meta"] = "Add your game to build it", ["button"] = "Add your game", ["act"] = "go:welcome" };
            pill = new JsonObject { ["text"] = "Not built", ["dot"] = "amber" };
        }
        else if (us is { Any: true })
        {
            var mb = ((us.LauncherNewer ? release!.SetupSize : 0) + (us.ToolsNewer ? Math.Max(release!.ToolsSize, release.ToolsCodeSize) : 0)) >> 20;
            home = new JsonObject
            {
                ["mode"] = "update", ["eb"] = "An update is ready", ["title"] = "Version " + release!.Tag.TrimStart('v'),
                ["meta"] = (mb > 0 ? mb + " MB download · " : "") + Estimate(true).Text + " · your saves and mods stay", ["button"] = "Update", ["act"] = "update",
            };
            links.Add(new JsonObject { ["html"] = "<b>Play " + H(built?.TrimStart('v') ?? "it") + " first</b>", ["act"] = "playold" });
            links.Add(new JsonObject { ["html"] = "See all changes", ["act"] = "open:changes" });
            pill = new JsonObject { ["text"] = "Update ready", ["dot"] = "amber", ["act"] = "update" };
        }
        else
        {
            home = new JsonObject { ["mode"] = files.Count > 0 ? "ready" : "nosave", ["eb"] = "Ready to play", ["title"] = "Ocarina of Time", ["meta"] = "Version " + (built?.TrimStart('v') ?? "?"), ["button"] = "Play", ["act"] = "play" };
            links.Add(new JsonObject { ["html"] = "Version " + H(built?.TrimStart('v') ?? "?") + " · <b>" + (checkFailed != null ? "offline" : release != null ? "up to date" : "checking") + "</b>" });
            links.Add(new JsonObject { ["html"] = H(g), ["act"] = "open:game" });
            pill = checkFailed != null ? new JsonObject { ["text"] = "Offline", ["dot"] = "grey" }
                 : new JsonObject { ["text"] = release != null ? "Up to date · " + built?.TrimStart('v') : built?.TrimStart('v') ?? "", ["dot"] = "" };
        }
        if (running) { home["button"] = "Running"; home["off"] = true; pill = new JsonObject { ["text"] = "Playing", ["dot"] = "blue" }; }
        home["links"] = links;
        if (release?.NewIn is { Count: > 0 } ni)
        {
            var items = new JsonArray();
            foreach (var n in ni) items.Add(new JsonObject { ["b"] = n.Title, ["p"] = n.Text });
            home["news"] = new JsonObject { ["title"] = "New in " + release.Tag.TrimStart('v'), ["items"] = items, ["more"] = release.PageUrl != null };
        }
        state["home"] = home;
        state["pill"] = pill;
        AddSettingsState(state);
        SendState(state);
    }

    /// <summary>A folder as a player reads it: short ones whole, long ones as the drive, …, and the last two parts.</summary>
    public static string ShortPath(string? p)
    {
        if (string.IsNullOrEmpty(p) || p.Length <= 44) return p ?? "";
        var parts = p.Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries);
        return parts.Length < 4 ? p : parts[0] + Path.DirectorySeparatorChar + "…" + Path.DirectorySeparatorChar + parts[^2] + Path.DirectorySeparatorChar + parts[^1];
    }

    static string When(DateTime? t, bool withTime = false)
    {
        if (t is not { } w) return "";
        var d = (DateTime.Now.Date - w.Date).TotalDays;
        var day = d < 1 ? "today" : d < 2 ? "yesterday" : w.Year == DateTime.Now.Year ? w.ToString("d MMM") : w.ToString("d MMM yyyy");
        return withTime && d < 2 ? day + ", " + w.ToString("HH:mm") : day;
    }

    // ---------------- Play ----------------
    void Play()
    {
        var g = GameDir;
        if (g == null) { Go("welcome"); return; }
        var game = new Game(ctx);
        if (game.IsRunning(g)) { Go("playing"); ShowGame(); return; }
        gameProc = game.Play(g);
        if (gameProc == null) { Toast("The game would not start. Settings, Build the game again, repairs a broken game folder.", "error"); return; }
        Log.Write("play " + g);
        Go("playing");
        RefreshHome();
        WatchGame();
    }

    void WatchGame()
    {
        runningTimer?.Dispose();
        runningTimer = new Timer(_ =>
        {
            var g = GameDir;
            if (g != null && new Game(ctx).IsRunning(g)) return;
            runningTimer?.Dispose(); runningTimer = null;
            Log.Write("the game ended");
            RefreshHome();
            Go("home");
            host.Activate();
        }, null, 3000, 2000);
    }

    void ShowGame()
    {
        if (!OperatingSystem.IsWindows()) return;
        try
        {
            var p = gameProc is { HasExited: false } ? gameProc
                : new Game(ctx).ProcessNames.SelectMany(Process.GetProcessesByName).FirstOrDefault();
            if (p != null && p.MainWindowHandle != IntPtr.Zero) { ShowWindow(p.MainWindowHandle, 9); SetForegroundWindow(p.MainWindowHandle); }
        }
        catch (Exception e) { Log.Write("show game: " + e.Message); }
    }
    [System.Runtime.InteropServices.DllImport("user32")] static extern bool SetForegroundWindow(IntPtr h);
    [System.Runtime.InteropServices.DllImport("user32")] static extern bool ShowWindow(IntPtr h, int c);

    // ---------------- links ----------------
    void OpenThing(string what)
    {
        string? url = what switch
        {
            "help" => BuildInfo.Home,
            "changes" => release?.PageUrl ?? BuildInfo.Home,
            _ => what.StartsWith("https://", StringComparison.Ordinal) ? what : null,
        };
        if (url != null) { try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch { } return; }
        var folder = what switch
        {
            "game" => GameDir,
            "log" => ctx.WorkFolder,
            "backups" => ctx.SaveBackupsFolder,
            _ => null,
        };
        if (folder != null && Directory.Exists(folder)) ctx.Platform.OpenFolder(folder);
    }

    /// <summary>The picture in save file <paramref name="slot"/> (1..6) of the built game: the game's own caption.sav.</summary>
    byte[]? SavePicture(int slot)
    {
        var g = GameDir;
        if (g == null) return null;
        var s = new Saves(ctx).Slots(g).FirstOrDefault(x => x.FileNumber == slot);
        return s?.Caption is { } c ? Caption.JpegFromFile(c) : null;
    }

    /// <summary>The page's address of a slot's picture (changes when the save does, so the page loads the new one).</summary>
    public static string SavePicUrl(SaveSlot s) => "img/save" + s.FileNumber + ".jpg?v=" + (s.Written?.Ticks ?? 0);
}
