// The start of a launcher run: its arguments, the one-launcher-per-install lock, the clean-up after a self-update, and which
// screen the App opens. This is the contract old installs update onto (research/02 2.4 and 2.8; research/12 1.3):
//   Windows   TriforceSetup.exe                                      (shortcut: the update check, then Home or the build)
//             TriforceSetup.exe --relaunched --merge                 (an old Setup replaced itself with this exe)
//             TriforceSetup.exe --update "<dir>" --relaunched        (the same, when the game had started the update)
//             TriforceSetup.exe --update "<dir>" --tag "<tag>"       (the game's own update offer, host/hos_update.c)
//             TriforceSetup.exe --merge                              (an install relaunching the installed copy)
//             TriforceSetup.exe --uninstall [--quiet]                (the Apps list)
//             TriforceSetup.exe --install-to <dir> [--no-desktop] [--no-startmenu]
//   Linux     AppRun --launch <dir>                                  (the game folder's "triforce" script)
//             AppRun --update <dir> [--tag T] [--fetched]            (the game, host/hos_update.c execl)
//             AppRun                                                 (the menu entry)
//   (AppRun keeps --cli, --play and "<dir>" for itself: they never start a window)
// Unknown arguments are ignored, never fatal.
namespace Triforce.Logic;

/// <summary>The arguments of a start, as the old Setup, the game and AppRun pass them.</summary>
public sealed record StartArgs
{
    public bool Relaunched { get; init; }
    /// <summary>Update &amp; merge straight away: Setup itself, then the tools, then the game folder built again.</summary>
    public bool Merge { get; init; }
    /// <summary>--update &lt;dir&gt;: the game folder to build again (the game's offer; it also sets Merge).</summary>
    public string? UpdateFolder { get; init; }
    /// <summary>--tag &lt;tag&gt;: the release the game saw (the old Setup ignored it).</summary>
    public string? Tag { get; init; }
    public bool Uninstall { get; init; }
    public bool Quiet { get; init; }
    public string? InstallTo { get; init; }
    public bool NoDesktop { get; init; }
    public bool NoStartMenu { get; init; }
    /// <summary>Linux --launch &lt;dir&gt;: the game folder's launcher page (Play, and Update when a newer release is out).</summary>
    public string? LaunchFolder { get; init; }
    /// <summary>Linux --fetched: the builder re-executed itself after its own update.</summary>
    public bool Fetched { get; init; }
    /// <summary>Linux --play [dir] when it reaches the launcher at all (AppRun normally handles it): start the game, no window.</summary>
    public bool Play { get; init; }
    public string? PlayFolder { get; init; }
    public IReadOnlyList<string> Unknown { get; init; } = Array.Empty<string>();

    /// <summary>Reads the arguments. A value with a quote in it is repaired: a folder that ends in a backslash, passed as
    /// --update "C:\Games\Triforce\" by a command line, reaches a program as `C:\Games\Triforce" --tag v0.5.2` (the \" is an
    /// escaped quote under the MSVC rules); the part after the quote is read as the arguments it was meant to be.</summary>
    public static StartArgs Parse(IReadOnlyList<string> raw)
    {
        var args = Repair(raw);
        var a = new StartArgs();
        var unknown = new List<string>();
        string? Next(ref int i) => i + 1 < args.Count && !args[i + 1].StartsWith("--", StringComparison.Ordinal) ? args[++i] : null;
        for (var i = 0; i < args.Count; i++)
        {
            switch (args[i])
            {
                case "--update":
                    if (Next(ref i) is { } u) a = a with { UpdateFolder = u, Merge = true };
                    break;
                case "--tag": a = a with { Tag = Next(ref i) }; break;
                case "--merge": a = a with { Merge = true }; break;
                case "--relaunched": a = a with { Relaunched = true }; break;
                case "--uninstall": a = a with { Uninstall = true }; break;
                case "--quiet": a = a with { Quiet = true }; break;
                case "--install-to": a = a with { InstallTo = Next(ref i) }; break;
                case "--no-desktop": a = a with { NoDesktop = true }; break;
                case "--no-startmenu": a = a with { NoStartMenu = true }; break;
                case "--launch": a = a with { LaunchFolder = Next(ref i) }; break;
                case "--fetched": a = a with { Fetched = true }; break;
                case "--play": a = a with { Play = true, PlayFolder = Next(ref i) }; break;
                default: unknown.Add(args[i]); break;
            }
        }
        return a with { Unknown = unknown };
    }

    static List<string> Repair(IReadOnlyList<string> raw)
    {
        var o = new List<string>();
        foreach (var arg in raw)
        {
            var q = arg.IndexOf('"');
            if (q < 0) { o.Add(arg); continue; }
            // (what was meant: the value up to the quote, a backslash given back; the rest split as a command line would be)
            var value = arg.Substring(0, q);
            if (value.Length > 0 && !value.EndsWith('\\') && !value.EndsWith('/') && value.Length >= 2 && value[1] == ':') value += "\\";
            if (value.Length > 0) o.Add(value);
            foreach (var w in arg.Substring(q + 1).Split(' ', StringSplitOptions.RemoveEmptyEntries)) o.Add(w.Trim('"'));
        }
        return o;
    }

    /// <summary>What a launcher passes on when it hands over to another copy (old Shell.cs Relaunch): --update "&lt;dir&gt;" when it had
    /// it, --relaunched after a self-update, --merge when merging without a folder named.</summary>
    public IReadOnlyList<string> PassOn(bool afterSelfUpdate)
    {
        var a = new List<string>();
        if (UpdateFolder != null) { a.Add("--update"); a.Add(UpdateFolder); }
        if (Tag != null && UpdateFolder != null) { a.Add("--tag"); a.Add(Tag); }
        if (afterSelfUpdate) a.Add("--relaunched");
        if (Merge && UpdateFolder == null) a.Add("--merge");
        return a;
    }
}

/// <summary>What the App opens.</summary>
public enum StartScreen
{
    /// <summary>Nothing: the process ends with ExitCode (another launcher is open, a silent install, the hand-over to the installed copy).</summary>
    Exit,
    /// <summary>The first run's Install page (Windows, not installed yet).</summary>
    Install,
    /// <summary>Ask the release server (Updater.CheckAsync), then Updater.Decide says Home, Merge or Failed.</summary>
    Check,
    /// <summary>Linux --launch &lt;dir&gt;: Home for that game folder (the check runs too, quietly).</summary>
    Launch,
    /// <summary>--uninstall: ask (unless Quiet), then Uninstaller.Run, then exit 0 (1 when the player says no).</summary>
    Uninstall,
}

public sealed class StartupResult
{
    public required StartArgs Args { get; init; }
    public StartScreen Screen { get; init; }
    /// <summary>Set when Screen is Exit: the process's exit code.</summary>
    public int ExitCode { get; init; }
    /// <summary>A sentence for the player (only for a message box the App shows before it exits; never with --quiet).</summary>
    public string? Message { get; init; }
    /// <summary>The install's lock, held for as long as the launcher runs: Release it when the window has closed.</summary>
    public InstanceLock? Lock { get; init; }
    /// <summary>The game folder this start is about (--update or --launch).</summary>
    public string? GameFolder { get; init; }
    /// <summary>Call Startup.SignalUp once the window is up (started by an old Setup that waits for that sign).</summary>
    public bool SignalUpWhenShown { get; init; }
}

public static class Startup
{
    /// <summary>The start: what runs before any window. Ordered as the old Program.Main and Shell.Begin.</summary>
    public static StartupResult Run(TriforceContext ctx, IReadOnlyList<string> args)
    {
        var a = StartArgs.Parse(args);
        var home = ctx.IsWindows ? ctx.InstalledDir ?? ctx.SelfDir : ctx.Home;
        if (a.Unknown.Count > 0) ctx.Log.Write("start: arguments not known, ignored: " + string.Join(" ", a.Unknown));

        // (the Setup this one replaced still runs for a moment, until it has heard that this window is up: again once it is gone)
        RemoveOld(ctx);
        if (a.Relaunched) _ = Task.Delay(5000).ContinueWith(_ => RemoveOld(ctx));

        if (a.Uninstall)
        {
            // (not under a launcher that is open: its tools and its build's work folder would go while it uses them)
            var l = InstanceLock.TryTake(home, ctx.TestRoot);
            if (l == null)
            {
                ctx.Log.Write("not uninstalled: Triforce is open for " + home);
                InstanceLock.CallForward(home, ctx.TestRoot);
                return new StartupResult { Args = a, Screen = StartScreen.Exit, ExitCode = 1, Message = a.Quiet ? null : "Triforce is open. Close it, then uninstall again." };
            }
            return new StartupResult { Args = a, Screen = StartScreen.Uninstall, Lock = l };
        }

        // (silent install: --install-to <folder> [--no-desktop] [--no-startmenu]; the installed launcher then starts as usual)
        if (a.InstallTo != null)
        {
            var exe = new Installer(ctx).Run(Path.GetFullPath(a.InstallTo), !a.NoDesktop, !a.NoStartMenu);
            ctx.Log.Write("installed " + BuildInfo.Version + " to " + Path.GetDirectoryName(exe) + " (silent)");
            if (ctx.Allowed(exe)) Installer.Start(exe, Array.Empty<string>())?.Dispose();
            return new StartupResult { Args = a, Screen = StartScreen.Exit, ExitCode = 0 };
        }

        // (a second start for the same install must not fetch, build, update or convert saves beside the first: it asks the first
        // one's window to the front and ends; nobody to ask yet: the first is only just starting or closing, tried 30 times)
        InstanceLock? held = null;
        for (var tries = 0; (held = InstanceLock.TryTake(home, ctx.TestRoot)) == null; tries++)
        {
            if (InstanceLock.CallForward(home, ctx.TestRoot))
            {
                ctx.Log.Write("Triforce is already open for " + home + ": its window was asked to the front, and this start ends");
                return new StartupResult { Args = a, Screen = StartScreen.Exit, ExitCode = 0 };
            }
            if (tries >= 30)
            {
                ctx.Log.Write("Triforce is already running for " + home + " and does not answer: this start ends");
                return new StartupResult
                {
                    Args = a, Screen = StartScreen.Exit, ExitCode = 1,
                    Message = "Triforce is already running, but its window could not be brought to the front.\n\nClose it (in Task Manager: TriforceSetup.exe) and start Triforce again.",
                };
            }
            Thread.Sleep(100);
        }

        var result = new StartupResult { Args = a, Screen = StartScreen.Check, Lock = held, GameFolder = a.UpdateFolder ?? a.LaunchFolder, SignalUpWhenShown = a.Relaunched };
        if (!ctx.IsWindows)
            return a.LaunchFolder != null && a.UpdateFolder == null ? Same(result, StartScreen.Launch) : result;

        // (TRIFORCE_DEV=1: run in place, without installing, for testing)
        if (ctx.RunningInstalled || ctx.Env("TRIFORCE_DEV") == "1") return result;
        if (ctx.InstalledDir is { } dir)
        {
            // (installed already: this download becomes the installed launcher, which then carries on with what it was asked)
            try
            {
                var exe = new Installer(ctx).Run(dir, ctx.Settings.Desktop, ctx.Settings.StartMenu);
                held.Release();   // (the launcher started here takes this install over: it must find the folder free)
                if (!ctx.Allowed(exe)) throw new InvalidOperationException(ctx.TestOnly);
                Installer.Start(exe, a.PassOn(afterSelfUpdate: false))?.Dispose();
                return new StartupResult { Args = a, Screen = StartScreen.Exit, ExitCode = 0 };
            }
            catch (Exception e)
            {
                ctx.Log.Write("the installed copy could not be replaced (" + e.Message + "): this one carries on");
                held.TakeAgain();
                return result;
            }
        }
        return Same(result, StartScreen.Install);
    }

    static StartupResult Same(StartupResult r, StartScreen screen) => new()
    {
        Args = r.Args, Screen = screen, Lock = r.Lock, GameFolder = r.GameFolder, SignalUpWhenShown = r.SignalUpWhenShown,
    };

    /// <summary>TriforceSetup.old.exe beside the install: the launcher a self-update replaced (it may still run for a few seconds).</summary>
    public static void RemoveOld(TriforceContext ctx)
    {
        var before = Path.Combine(ctx.IsWindows ? ctx.InstalledDir ?? ctx.SelfDir : ctx.Home, "TriforceSetup.old.exe");
        try { if (File.Exists(before) && ctx.Allowed(before)) File.Delete(before); } catch (Exception) { }
    }

    /// <summary>The sign an old Setup waits for after it replaced itself with this one: call once the window is up. Without it
    /// the old one waits 30 s hidden, then quits.</summary>
    public static void SignalUp(TriforceContext ctx) =>
        Handover.Up(ctx.IsWindows ? ctx.InstalledDir ?? ctx.SelfDir : ctx.Home, ctx.TestRoot);
}
