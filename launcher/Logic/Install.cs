// A per-user install, as the old Setup made it (research/02 1.1-1.5): the exe copied into %LOCALAPPDATA%\Programs\Triforce,
// the Apps-list entry under HKCU, the shortcuts; and the uninstall that takes it away again (game folders and saves stay).
using System.Diagnostics;

namespace Triforce.Logic;

public sealed class Installer
{
    readonly TriforceContext ctx;
    public Installer(TriforceContext ctx) { this.ctx = ctx; }

    /// <summary>The folder an install goes to when the player changes nothing.</summary>
    public string DefaultDir => ctx.Platform.DefaultInstallDir;

    /// <summary>The line under the install folder (old Shell.cs InstallSpaceText): "about 2 GB", red at 3 GiB free or less.</summary>
    public (string Text, bool Low) SpaceLine(string dir)
    {
        try
        {
            var disk = ctx.Platform.Disk(dir);
            if (ctx.FreeBytes(dir) is not { } free) return ("Triforce and its tools take about 2 GB.", false);
            return ("Triforce and its tools take about 2 GB. " + disk + " has " + (free >> 30) + " GB free.", free <= (3L << 30));
        }
        catch (Exception) { return ("Triforce and its tools take about 2 GB.", false); }
    }

    /// <summary>Copies this exe into the folder, registers it in the Apps list and makes the shortcuts. Returns the installed exe.
    /// ("Triforce" opens the launcher, not the game: every start goes through the update question first, then Play.)</summary>
    public string Run(string dir, bool desktop, bool startMenu)
    {
        dir = Path.GetFullPath(dir);
        ctx.Require(dir);
        Directory.CreateDirectory(dir);
        var exe = Path.Combine(dir, BuildInfo.SetupAsset);
        if (ctx.IsWindows)
        {
            // ((Triforce) through <exe>.partial, renamed into place once complete: a copy cut off in the middle left a Setup that
            // does not start under the name every shortcut starts. Partial)
            if (!TriforceContext.SamePath(exe, ctx.SelfPath)) new Partial(ctx.Platform).CopyFile(ctx.SelfPath, exe);
            var s = ctx.Platform.Settings;
            if (!ctx.NoWrites)
            {
                s.SetUninstall("DisplayName", BuildInfo.AppName);
                s.SetUninstall("DisplayIcon", exe + ",0");
                s.SetUninstall("DisplayVersion", BuildInfo.Version);
                s.SetUninstall("Publisher", "Triforce");
                s.SetUninstall("InstallLocation", dir);
                s.SetUninstall("UninstallString", "\"" + exe + "\" --uninstall");
                s.SetUninstall("QuietUninstallString", "\"" + exe + "\" --uninstall --quiet");
                s.SetUninstall("URLInfoAbout", BuildInfo.Home);
                s.SetUninstallNumber("NoModify", 1);
                s.SetUninstallNumber("NoRepair", 1);
            }
        }
        else exe = ctx.Env("APPIMAGE") ?? ctx.SelfPath;   // (Linux: the AppImage stays where the player keeps it; only the menu entries are made)
        ctx.Settings.Desktop = desktop;
        ctx.Settings.StartMenu = startMenu;
        try
        {
            var link = new Shortcut(exe, "", ctx.IsWindows ? dir : Path.GetDirectoryName(exe)!, exe, "Play Triforce");
            if (startMenu) ctx.Platform.CreateShortcut(ShortcutPlace.StartMenu, link);
            if (desktop) ctx.Platform.CreateShortcut(ShortcutPlace.Desktop, link);
        }
        catch (Exception e) { ctx.Log.Write("shortcuts: " + e.Message); }
        ctx.Log.Write("installed " + BuildInfo.Version + " to " + dir);
        return exe;
    }

    /// <summary>Starts a launcher exe with arguments (each one as is; quoted by .NET with the MSVC rules), in its own folder.</summary>
    public static Process? Start(string exe, IEnumerable<string> args)
    {
        var si = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! };
        foreach (var a in args) si.ArgumentList.Add(a);
        return Process.Start(si);
    }
}

/// <summary>What an uninstall removes and what it keeps, said before it is done (DESIGN 6 Settings: "says what goes and what stays").</summary>
public sealed record UninstallPlan(IReadOnlyList<string> Removes, IReadOnlyList<string> Keeps);

/// <summary>Removes the launcher, its tools and the shortcuts (old Setup.cs Install.Uninstall). Game folders and saves are the
/// player's and stay. The question ("Remove Triforce and its tools from this PC?") is the App's; --quiet asks nothing.</summary>
public sealed class Uninstaller
{
    readonly TriforceContext ctx;
    public Uninstaller(TriforceContext ctx) { this.ctx = ctx; }

    public UninstallPlan Describe()
    {
        var removes = new List<string>
        {
            "Triforce and its tools (" + ctx.Home + ")",
            "the shortcuts",
            "the build's work folder and compile cache (" + ctx.WorkFolder + ", made again by a build)",
            "the launcher's settings",
        };
        if (!ctx.IsWindows) removes[0] = "the launcher's updates and NVIDIA's SDK (" + ctx.Platform.DefaultInstallDir + ")";
        var keeps = new List<string> { "your game folders" + (ctx.Settings.GameFolder is { } g ? " (" + g + ")" : ""), "your saves" };
        return new UninstallPlan(removes, keeps);
    }

    /// <summary>Does it. The launcher's own folder goes once this process is gone (Windows: a short-lived cmd waits 2 s, then
    /// rmdir; Linux: removed now, a running binary can be unlinked).</summary>
    public void Run()
    {
        var dir = ctx.InstalledDir ?? ctx.Home;
        ctx.Log.Write("uninstall: " + dir);
        ctx.Platform.RemoveShortcuts();
        // (the build's work folder and compile cache, nx2/make_game.py --work: several GB, made again by a build)
        var work = ctx.WorkFolder;
        try { if (Directory.Exists(work) && ctx.Allowed(work)) Directory.Delete(work, true); } catch (Exception) { }
        ctx.Platform.Settings.RemoveUninstall();
        ctx.Platform.Settings.RemoveAll();
        if (ctx.IsWindows)
        {
            // (this exe is running from the folder: a short-lived cmd removes it once we are gone)
            if (File.Exists(Path.Combine(dir, BuildInfo.SetupAsset)) && ctx.Allowed(dir))
                Process.Start(new ProcessStartInfo("cmd.exe", "/c timeout /t 2 /nobreak >nul & rmdir /s /q \"" + dir + "\"")
                    { CreateNoWindow = true, UseShellExecute = false, WorkingDirectory = Path.GetTempPath() })?.Dispose();
        }
        else
        {
            var root = ctx.Platform.DefaultInstallDir;
            try { if (Directory.Exists(root) && ctx.Allowed(root)) Directory.Delete(root, true); } catch (Exception) { }
        }
    }
}
