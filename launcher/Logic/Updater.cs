// Every start: ask what is newest, then decide (old Shell.cs Gather, Merge, StartNewSetup; research/02 2.3-2.4a).
//   Merge = 1. the launcher itself (it restarts and carries on), 2. the tools, 3. the game folder built again.
using System.Diagnostics;
using System.Net.Http;

namespace Triforce.Logic;

/// <summary>What the start page does once the check has answered.</summary>
public enum CheckOutcome
{
    /// <summary>Update &amp; merge now (a first start with no tools and no game, or --merge / --update).</summary>
    Merge,
    /// <summary>Home: an update may be offered there (Updater.State).</summary>
    Home,
    /// <summary>The check failed but there is something to play or build with: Home, offline.</summary>
    HomeOffline,
    /// <summary>The check failed and there is nothing: the failure screen (Try again).</summary>
    Failed,
}

public enum FailureKind { Cancelled, Offline, NoRoom, CutOff, NotStarted, NoRelease, Other }

/// <summary>A failure in the old words: a title and one plain sentence (old Shell.cs Failed).</summary>
public sealed record Failure(FailureKind Kind, string Title, string Note)
{
    public static Failure Of(Exception e, bool cancelled)
    {
        var offline = e is HttpRequestException || e is TaskCanceledException && !cancelled;
        if (cancelled) return new(FailureKind.Cancelled, "Stopped", "Nothing was changed.");
        if (offline) return new(FailureKind.Offline, "The Triforce site could not be reached", "Check your internet connection, then try again.");
        return e switch
        {
            NoRoomException => new(FailureKind.NoRoom, "Not enough room on the disk", e.Message),
            CutOffException => new(FailureKind.CutOff, "The download stopped", e.Message),
            NotStartedException => new(FailureKind.NotStarted, "The new launcher could not start", e.Message),
            NoReleaseException => new(FailureKind.NoRelease, "There is no Triforce release yet", e.Message),
            _ => new(FailureKind.Other, "Something went wrong", e.Message),
        };
    }
}

public sealed record CheckDecision(CheckOutcome Outcome, Release? Release, Failure? Failure);

/// <summary>What Home shows about updates (old Shell.Home.cs: setupNewer || toolsNewer || gameBehind).</summary>
public sealed record UpdateState(bool LauncherNewer, bool ToolsNewer, bool GameBehind, string? GameFolder, string? GameVersion, string? ToolsVersion, Release? Release)
{
    public bool Any => LauncherNewer || ToolsNewer || GameBehind;
}

/// <summary>The steps a Merge will take, in order.</summary>
public sealed record MergePlan(bool SelfUpdate, bool Tools, string? RebuildFolder, bool BuildForm);

/// <summary>After the launcher replaced itself: close (the new one is up, or left to itself).</summary>
public enum SelfUpdateResult { CloseNow }

public sealed class Updater
{
    readonly TriforceContext ctx;
    readonly Tools tools;
    readonly Game game;
    public Updater(TriforceContext ctx)
    {
        this.ctx = ctx;
        tools = new Tools(ctx);
        game = new Game(ctx);
    }

    /// <summary>How long a new launcher has to give its sign (old: 30 s).</summary>
    public TimeSpan HandoverLimit { get; set; } = TimeSpan.FromSeconds(30);
    /// <summary>Set by the App when the OS is ending the session (a new launcher ended then says nothing about it).</summary>
    public bool SessionEnding { get; set; }

    /// <summary>Ask the release server what is newest (nothing downloaded yet), and log what it said.</summary>
    public async Task<Release> CheckAsync(HttpClient http, CancellationToken token)
    {
        var r = await Release.LatestAsync(ctx, http, token).ConfigureAwait(false);
        ctx.Log.Write("latest release " + r.Tag + " (setup " + (r.SetupUrl != null) + ", tools " + (r.ToolsUrl != null) + "); this launcher " + BuildInfo.Version +
                      ", tools " + (tools.InstalledVersion ?? "none"));
        if (ctx.Settings.SetupSkip == r.Tag) ctx.Log.Write("launcher " + r.Tag + " is not fetched: it could not start on this PC (SetupSkip)");
        return r;
    }

    bool HasGame => game.Current != null;

    /// <summary>What the start does with the check's answer (old Shell.cs Gather): with nothing to play yet the tools come
    /// straight away; --merge carries on; else Home. A failure shows Home all the same when there is something to play or tools
    /// to build with.</summary>
    public CheckDecision Decide(StartArgs args, Release? release, Exception? error, bool cancelled = false)
    {
        if (release != null && error == null)
        {
            var toolsHere = tools.InstalledVersion != null || !ctx.IsWindows;
            return new CheckDecision(!toolsHere && !HasGame || args.Merge ? CheckOutcome.Merge : CheckOutcome.Home, release, null);
        }
        var e = error ?? new InvalidOperationException("no answer");
        ctx.Log.Write("update check failed: " + e.GetType().Name + ": " + e.Message);
        if (!cancelled && (tools.InstalledVersion != null || HasGame)) return new CheckDecision(CheckOutcome.HomeOffline, null, Failure.Of(e, false));
        return new CheckDecision(CheckOutcome.Failed, null, Failure.Of(e, cancelled));
    }

    /// <summary>Is the release's launcher one to replace this one with? Not a dev build, not the same tag (plain string inequality:
    /// any other tag counts, even an older one), not one that could not start here (SetupSkip). Windows only: on Linux the
    /// launcher comes with the AppImage (LinuxUpdate).</summary>
    public bool LauncherNewer(Release r) =>
        ctx.IsWindows && !BuildInfo.IsDev && r.Tag != BuildInfo.Version && r.SetupUrl != null && ctx.Settings.SetupSkip != r.Tag;

    public bool ToolsNewer(Release r) => ctx.IsWindows ? tools.InstalledVersion != r.Tag : LinuxUpdate.BuilderNewer(r.Tag, tools.InstalledVersion);

    /// <summary>The game is behind the tools: built with other tools than the ones here.</summary>
    public bool GameBehind(string folder) => game.FindExe(folder) != null && Game.BuiltVersion(folder) is var built && built != tools.InstalledVersion && tools.InstalledVersion != null;

    public UpdateState State(Release? r)
    {
        var folder = game.Current;
        return new UpdateState(r != null && LauncherNewer(r), r != null && ToolsNewer(r), folder != null && GameBehind(folder), folder,
            folder != null ? Game.BuiltVersion(folder) : null, tools.InstalledVersion, r);
    }

    /// <summary>The steps of Update &amp; merge for these arguments (old Shell.cs Merge): the launcher when not just relaunched and a
    /// newer one is out; the tools when theirs is another tag; then the folder the game's offer named (else the one built last)
    /// built again when its version is not the tools' (re-asked after the tools step: RebuildFolderNow).</summary>
    public MergePlan Plan(StartArgs args, Release r)
    {
        var self = !args.Relaunched && LauncherNewer(r);
        var t = ctx.IsWindows && tools.InstalledVersion != r.Tag;
        var folder = RebuildFolderNow(args);
        return new MergePlan(self, t, folder, folder == null && game.FindExe(args.UpdateFolder ?? ctx.Settings.GameFolder) == null);
    }

    /// <summary>Merge step 3: the folder to build again now (it has a game, its version is not the tools', and there are tools), or null.</summary>
    public string? RebuildFolderNow(StartArgs args)
    {
        var folder = args.UpdateFolder ?? ctx.Settings.GameFolder;
        return folder != null && game.FindExe(folder) != null && Game.BuiltVersion(folder) != tools.InstalledVersion && tools.Find() != null ? folder : null;
    }

    /// <summary>Merge step 1, the launcher itself (research/02 2.4a): the release's TriforceSetup.exe downloaded to
    /// &lt;install&gt;\TriforceSetup.new.exe (checked against the listed size only), swapped in under the running one's name (this one
    /// runs on as TriforceSetup.old.exe), started with the same arguments plus --relaunched (and --merge), and watched for 30 s:
    ///   Up or Unclear: CloseNow (the App closes; the new one carries on with the merge).
    ///   Failed (it ended with an error before its sign): the lock taken again, the old exe put back under the name, SetupSkip=tag,
    ///   and NotStartedException: the App shows its window again with that sentence.
    /// hide: called when the new one has been started (the App hides its window while it waits).</summary>
    public async Task<SelfUpdateResult> SelfUpdateAsync(HttpClient http, Release r, StartArgs args, InstanceLock? held, Action<DownloadProgress>? progress, Action? hide, CancellationToken token)
    {
        if (r.SetupUrl == null) throw new InvalidOperationException("The release " + r.Tag + " has no " + BuildInfo.SetupAsset + ".");
        var dir = ctx.InstalledDir ?? ctx.SelfDir;
        var fresh = Path.Combine(dir, "TriforceSetup.new.exe");
        var self = Path.Combine(dir, BuildInfo.SetupAsset);
        var old = Path.Combine(dir, "TriforceSetup.old.exe");
        ctx.Require(fresh);
        await new Downloader(ctx).ToFileAsync(http, r.SetupUrl, fresh, r.SetupSize, progress, token).ConfigureAwait(false);
        Handover.Swap(fresh, self, old);
        ctx.Log.Write("launcher updated to " + r.Tag + ", restarting");
        var next = args with { Merge = true };
        var startArgs = next.PassOn(afterSelfUpdate: true);

        EventWaitHandle? up = null;
        try { up = Handover.Expect(dir, ctx.TestRoot); } catch (Exception) { }
        var outcome = Handover.Outcome.Unclear;
        var why = "";
        using (up)
        {
            Process? started = null;
            held?.Release();   // (the launcher started here takes this install over: it must find the folder free)
            try
            {
                started = Installer.Start(self, startArgs);
                if (started == null) throw new InvalidOperationException("it did not start");
            }
            catch (Exception e) { outcome = Handover.Outcome.Failed; why = e.Message; }
            if (started != null)
            {
                using (started)
                {
                    if (up == null) return SelfUpdateResult.CloseNow;   // (no sign can be waited for here: started and left to itself)
                    hide?.Invoke();
                    try { outcome = await Task.Run(() => Handover.Watch(started, up, HandoverLimit), CancellationToken.None).ConfigureAwait(false); }
                    catch (Exception) { }
                    try { if (outcome == Handover.Outcome.Failed) why = "it ended at once, with exit code 0x" + started.ExitCode.ToString("X8"); } catch (Exception) { }
                }
            }
        }
        ctx.Log.Write("launcher " + r.Tag + " after the restart: " + (outcome == Handover.Outcome.Up ? "its window is up"
            : outcome == Handover.Outcome.Failed ? "it did not start (" + why + ")" : "no sign of its window, left to itself"));
        // (when the OS ends the session the new launcher is ended like everything else, which says nothing about it. And when another
        // launcher has the folder by now, the new one started after all: the player started it again)
        if (outcome != Handover.Outcome.Failed || SessionEnding || held != null && !held.TakeAgain()) return SelfUpdateResult.CloseNow;
        var back = await Task.Run(() => Handover.Restore(ctx.Platform, old, self), CancellationToken.None).ConfigureAwait(false);
        ctx.Log.Write(back ? "launcher " + BuildInfo.Version + " is back under its name, and " + r.Tag + " will not be fetched again"
                           : "launcher " + BuildInfo.Version + " could not be put back under its name");
        if (back) ctx.Settings.SetupSkip = r.Tag;
        throw new NotStartedException(back
            ? "Triforce " + r.Tag + " could not start on this PC (" + why + "), so " + BuildInfo.Version + " is back in its place. Try again goes on with this one."
            : "Triforce " + r.Tag + " could not start on this PC (" + why + "), and " + BuildInfo.Version + " could not be put back in its place. Keep this window open and download Triforce again from " + BuildInfo.Home + ".");
    }
}
