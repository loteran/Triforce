// A built game folder: its program, its version, Play, and whether it runs (old Shell.cs GameExe/BuiltVersion/Play,
// builder/linux/linux_update.py game_exe/game_version, hookshot_builder.py play/add_to_menu, hookshot-launch).
using System.Diagnostics;

namespace Triforce.Logic;

/// <summary>What the launcher knows of a game folder.</summary>
public sealed record GameInfo(
    string Folder,
    /// <summary>The game's program, null when there is none (no game here, or the folder is gone).</summary>
    string? Exe,
    /// <summary>version= of triforce_version.txt (hookshot_version.txt in an older folder): the tools it was built with.</summary>
    string? Version,
    /// <summary>builder= of that file: the launcher the game starts again for its own update offer.</summary>
    string? Builder,
    /// <summary>The code line of language.txt.</summary>
    string? Language,
    /// <summary>It keeps the game files an update builds from (.triforce_exefs and romfs).</summary>
    bool Kept,
    /// <summary>It was built with NVIDIA DLSS (an update keeps it).</summary>
    bool Dlss,
    bool Exists);

public sealed class Game
{
    readonly TriforceContext ctx;
    public Game(TriforceContext ctx) { this.ctx = ctx; }

    /// <summary>The names of the game's program (Hookshot's after Triforce's, for a folder that still has those).</summary>
    public static readonly string[] WindowsExes = { "Triforce.exe", "hookshot.exe" };
    public static readonly string[] LinuxNames = { "triforce", "hookshot" };
    /// <summary>Process names that are this game (Windows: without .exe).</summary>
    public string[] ProcessNames => ctx.IsWindows ? new[] { "Triforce", "hookshot" } : LinuxNames;

    /// <summary>The game's program in a folder. Windows: Triforce.exe, else hookshot.exe (make_game puts the game straight in the
    /// folder). Linux: only with image.bin there; bin/triforce under the folder's launcher script first, then triforce in a folder
    /// built without one (beside image.bin that name is the launcher itself) (linux_update.game_exe).</summary>
    public string? FindExe(string? folder)
    {
        if (string.IsNullOrEmpty(folder)) return null;
        if (ctx.IsWindows)
        {
            foreach (var n in WindowsExes)
            {
                var p = Path.Combine(folder, n);
                if (File.Exists(p)) return p;
            }
            return null;
        }
        if (!File.Exists(Path.Combine(folder, "image.bin"))) return null;
        foreach (var n in LinuxNames)
            if (File.Exists(Path.Combine(folder, "bin", n))) return Path.Combine(folder, "bin", n);
        foreach (var n in LinuxNames)
            if (File.Exists(Path.Combine(folder, n))) return Path.Combine(folder, n);
        return null;
    }

    /// <summary>"name=value" from a small text file, null when it is not there.</summary>
    public static string? ReadValue(string file, string name)
    {
        try
        {
            foreach (var line in File.ReadAllLines(file))
                if (line.StartsWith(name + "=", StringComparison.Ordinal)) return line.Substring(name.Length + 1).Trim();
        }
        catch (Exception) { }
        return null;
    }

    static string? VersionValue(string folder, string name) =>
        ReadValue(Path.Combine(folder, "triforce_version.txt"), name) ?? ReadValue(Path.Combine(folder, "hookshot_version.txt"), name);

    /// <summary>The release a folder was built with (version= of triforce_version.txt, else hookshot_version.txt).</summary>
    public static string? BuiltVersion(string folder) => VersionValue(folder, "version");

    /// <summary>The language a folder was built in: the code line of its language.txt (make_game.py folder_language).</summary>
    public static string? FolderLanguage(string folder)
    {
        try
        {
            foreach (var raw in File.ReadLines(Path.Combine(folder, "language.txt")))
            {
                var line = raw.Trim();
                if (line.Length > 0 && !line.StartsWith('#') && Languages.Find(line) != null) return line;
            }
        }
        catch (Exception) { }
        return null;
    }

    /// <summary>Does the folder keep the game files an update builds from? (linux_update.game_version: .triforce_exefs or
    /// .hookshot_exefs, and romfs)</summary>
    public static bool KeptFiles(string folder) =>
        (Directory.Exists(Path.Combine(folder, ".triforce_exefs")) || Directory.Exists(Path.Combine(folder, ".hookshot_exefs"))) && Directory.Exists(Path.Combine(folder, "romfs"));

    /// <summary>Was the folder built with NVIDIA DLSS? Windows: ngx\nvngx_dlss.dll. Linux: ngx/libnvidia-ngx-dlss.so* and NVIDIA's
    /// licence in the launcher's ngx-sdk (hookshot_builder.py folder_has_dlss).</summary>
    public bool HasDlss(string folder)
    {
        if (ctx.IsWindows) return File.Exists(Path.Combine(folder, "ngx", "nvngx_dlss.dll"));
        try
        {
            return Directory.EnumerateFiles(Path.Combine(folder, "ngx"), "libnvidia-ngx-dlss.so*").Any() && File.Exists(Path.Combine(ctx.NgxSdkFolder, "LICENSE.txt"));
        }
        catch (Exception) { return false; }
    }

    public GameInfo Describe(string folder) => new(folder, FindExe(folder), BuiltVersion(folder), VersionValue(folder, "builder"), FolderLanguage(folder),
        KeptFiles(folder), HasDlss(folder), Directory.Exists(folder));

    /// <summary>The game folder the launcher built last (GameFolder), when it still holds a game.</summary>
    public string? Current => ctx.Settings.GameFolder is { } g && FindExe(g) != null ? g : null;

    /// <summary>Does this folder's game run? A program of the game's names whose exe is inside the folder (another folder's game is
    /// another game). One whose exe cannot be read counts, except in a test run, where only programs inside the test folder do
    /// (old Shell.Mods.cs GameRuns).</summary>
    public bool IsRunning(string folder)
    {
        foreach (var p in ctx.Platform.FindProcesses(ProcessNames))
        {
            if (p.Exe == null) { if (ctx.TestRoot == null) return true; continue; }
            if (TriforceContext.Inside(folder, p.Exe) && (ctx.TestRoot == null || TriforceContext.Inside(ctx.TestRoot, p.Exe))) return true;
        }
        return false;
    }

    /// <summary>Is any Triforce game running (any folder)? The App mutes its sounds then (old Sound.cs).</summary>
    public bool AnyRunning() =>
        ctx.Platform.FindProcesses(ProcessNames).Any(p => ctx.TestRoot == null || p.Exe != null && TriforceContext.Inside(ctx.TestRoot, p.Exe));

    /// <summary>The variables the game must not see (make_game.py GAME_ENV_DROP + LD_LIBRARY_PATH, hookshot-launch), and what the
    /// launcher's own .NET and WebKit add (research/12 6).</summary>
    static readonly string[] DropPrefixes = { "HOOKSHOT_", "PYTHON", "APPDIR", "APPIMAGE", "ARGV0", "OWD", "DOTNET_", "WEBKIT_", "COREHOST_" };
    static readonly string[] DropNames = { "LD_LIBRARY_PATH", "GDK_BACKEND" };

    /// <summary>Starts the game in its folder (old Shell.cs Play; Linux hookshot_builder.py play / hookshot-launch), with
    /// HOOKSHOT_NO_UPDATE_CHECK=1: the launcher just asked about updates, and the game's own offer would ask again. Linux: the
    /// AppImage's variables dropped, a session of its own, its output nowhere. Refused in a test run outside the test folder.</summary>
    public Process? Play(string folder)
    {
        var exe = FindExe(folder);
        if (exe == null) return null;
        if (!ctx.AllowedForGame(exe)) { ctx.Log.Write("not started (a test run starts only what is in its own folder): " + exe); return null; }
        var si = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = folder };
        if (!ctx.IsWindows)
        {
            foreach (var key in si.Environment.Keys.ToList())
                if (DropPrefixes.Any(p => key.StartsWith(p, StringComparison.Ordinal)) || DropNames.Contains(key)) si.Environment.Remove(key);
            si.RedirectStandardOutput = si.RedirectStandardError = true;
            si.RedirectStandardInput = true;
        }
        si.Environment["HOOKSHOT_NO_UPDATE_CHECK"] = "1";
        var p = ctx.IsWindows ? Process.Start(si) : ctx.Platform.StartTree(si);
        if (p != null && !ctx.IsWindows)
        {
            // (stdio to nowhere: read and dropped, so a full pipe never blocks the game)
            p.OutputDataReceived += (_, _) => { };
            p.ErrorDataReceived += (_, _) => { };
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            try { p.StandardInput.Close(); } catch (Exception) { }
        }
        ctx.Log.Write("started the game: " + exe);
        return p;
    }

    /// <summary>The window class the game names itself with on Linux (host/posix/display_sdl.c HOS_APP_ID).</summary>
    public const string LinuxAppId = "io.github.NotRexed.Hookshot";

    /// <summary>Linux: an applications-menu entry that starts the game folder's launcher script (hookshot_builder.py add_to_menu),
    /// written after every good build. Its icon: the crest copied into the folder as hookshot.png.</summary>
    public void AddToMenu(string folder, string? iconSource)
    {
        if (ctx.IsWindows) return;
        if (folder.IndexOfAny(new[] { '\r', '\n', '\0' }) >= 0) throw new ArgumentException("the game folder's name has a line break");
        var icon = Path.Combine(folder, "hookshot.png");
        if (iconSource != null && File.Exists(iconSource)) File.Copy(iconSource, icon, true);
        var apps = Path.Combine(((LinuxPlatform)ctx.Platform).DataHome, "applications");
        ctx.Require(apps);
        Directory.CreateDirectory(apps);
        File.WriteAllText(Path.Combine(apps, "triforce.desktop"),
            LinuxPlatform.DesktopEntry("Triforce", "The Legend of Zelda: Ocarina of Time, built from your copy", Path.Combine(folder, "triforce"), "", folder, icon, LinuxAppId));
    }
}
