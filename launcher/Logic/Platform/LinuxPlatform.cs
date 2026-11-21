// Linux (the AppImage, the Steam Deck): the Tk builder's layout (research/12 section 2) and the equivalents research/02
// section 11 names for each Windows mechanism. NOT run on this PC (Windows): every Linux path here is ported from the
// Python/shell it replaces and should be checked on a real Linux machine or a Deck before release.
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace Triforce.Logic;

public sealed class LinuxPlatform : PlatformBase
{
    public LinuxPlatform(IReadOnlyDictionary<string, string?>? env = null) : base(env)
    {
        Settings = TestRoot != null ? new TestRootSettingsStore(TestRoot) : new IniSettingsStore(Path.Combine(ConfigHome, "triforce", "builder.ini"));
    }

    public override bool IsWindows => false;
    public override ISettingsStore Settings { get; }

    string HomeDir => TestRoot ?? Env("HOME") ?? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);

    // (XDG, honoured as the Tk builder does: config, data, cache; the game's own saves_dir takes XDG_DATA_HOME only when absolute)
    string Xdg(string name, string fallback, string testSub)
    {
        if (TestRoot != null) return Path.Combine(TestRoot, testSub);
        return Env(name) is { Length: > 0 } v && v.StartsWith('/') ? v : Path.Combine(HomeDir, fallback);
    }

    public string ConfigHome => Xdg("XDG_CONFIG_HOME", ".config", "config");
    public string DataHome => Xdg("XDG_DATA_HOME", Path.Combine(".local", "share"), "data");
    string CacheHome => Xdg("XDG_CACHE_HOME", ".cache", "cache");
    string StateHome => Xdg("XDG_STATE_HOME", Path.Combine(".local", "state"), "state");

    /// <summary>linux_update.data_root(): ${HOOKSHOT_BUILDER_HOME:-${XDG_DATA_HOME:-~/.local/share}}/triforce/builder: the self-update
    /// root (current, versions/&lt;tag&gt;), read by the game (host/hos_update.c) and the folder launcher (hookshot-folder).</summary>
    public override string DefaultInstallDir =>
        Path.Combine(TestRoot == null && Env("HOOKSHOT_BUILDER_HOME") is { Length: > 0 } b ? b : DataHome, "triforce", "builder");

    public override string WorkFolder => Path.Combine(CacheHome, "triforce-build");
    public override string DefaultGameFolder => Path.Combine(HomeDir, "Games", "Triforce");
    public override string LogFolder(string home) => Path.Combine(StateHome, "triforce");
    // (hookshot_builder.py ngx_sdk_dir: beside the builder's versions, kept across updates)
    public override string NgxSdkFolder(string home) => Path.Combine(DefaultInstallDir, "ngx-sdk");
    // (host/hos_main.c saves_dir: $XDG_DATA_HOME/triforce/user, outside the game folder so a rebuild keeps them)
    public override string SaveFolder(string game, string uid) => Path.Combine(UserFolder(game), "save", uid);
    /// <summary>((Triforce) 11-19) $XDG_DATA_HOME/triforce/user. Until the game has started once on v0.5.3 or later, the saves are
    /// still where an older build kept them: $XDG_DATA_HOME/hookshot (the game renames that to triforce/user at its next
    /// start), or the game folder's user/ when the game could not move them out of it. The first of these with a save folder.</summary>
    public override string UserFolder(string game)
    {
        var now = Path.Combine(DataHome, "triforce", "user");
        foreach (var d in new[] { now, Path.Combine(DataHome, "hookshot"), Path.Combine(game, "user") })
            if (Directory.Exists(Path.Combine(d, "save"))) return d;
        return now;
    }
    public override (string Roaming, string Local) EmulatorDataRoots => (DataHome, ConfigHome);

    public override IReadOnlyList<string> SteamRoots()
    {
        if (TestRoot != null) return new[] { Path.Combine(TestRoot, "Steam") };
        // (research/12 section 6, guess-level: verify on a Deck. ~/.steam/steam is a link to the real folder)
        return new[]
        {
            Path.Combine(HomeDir, ".steam", "steam"), Path.Combine(HomeDir, ".local", "share", "Steam"),
            Path.Combine(HomeDir, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam"),
        };
    }

    // ---- shortcuts: .desktop files ----

    public override string ShortcutPath(ShortcutPlace place) => place == ShortcutPlace.Desktop
        ? Path.Combine(HomeDir, "Desktop", "triforce-launcher.desktop")
        : Path.Combine(DataHome, "applications", "triforce-launcher.desktop");

    public override void CreateShortcut(ShortcutPlace place, Shortcut s)
    {
        var file = ShortcutPath(place);
        if (place == ShortcutPlace.Desktop && !Directory.Exists(Path.GetDirectoryName(file))) return;   // (no desktop folder: none made)
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        File.WriteAllText(file, DesktopEntry(s.Name, s.Description, s.Target, s.Arguments, s.WorkingDirectory, s.Icon), new UTF8Encoding(false));
        if (!OperatingSystem.IsWindows()) try { File.SetUnixFileMode(file, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute | UnixFileMode.GroupRead | UnixFileMode.OtherRead); } catch (Exception) { }
    }

    /// <summary>A desktop entry. Refuses CR, LF and NUL in any value (a line break would start another key: hookshot_builder.py add_to_menu).</summary>
    public static string DesktopEntry(string name, string comment, string target, string arguments, string workDir, string icon, string? wmClass = null)
    {
        foreach (var v in new[] { name, comment, target, arguments, workDir, icon, wmClass ?? "" })
            if (v.IndexOfAny(new[] { '\r', '\n', '\0' }) >= 0) throw new ArgumentException("A path or name with a line break cannot go into a menu entry.");
        var exec = DesktopExec(target) + (arguments.Length > 0 ? " " + arguments : "");
        return "[Desktop Entry]\nType=Application\nName=" + name + "\nComment=" + comment + "\nExec=" + exec + "\nPath=" + workDir + "\nIcon=" + icon +
               "\nCategories=Game;\nTerminal=false\n" + (wmClass != null ? "StartupWMClass=" + wmClass + "\n" : "");
    }

    /// <summary>One Exec argument (hookshot_builder.py desktop_exec): a % is a field code, so it is doubled; inside the quotes the
    /// Desktop Entry spec wants " ` $ \ each preceded by a backslash, and every backslash written twice because the file's string
    /// escaping is applied first (so a literal backslash is four, and a dollar two backslashes and the dollar).</summary>
    public static string DesktopExec(string path)
    {
        var escaped = path.Replace("\\", "\\\\\\\\");
        foreach (var c in new[] { "\"", "`", "$" }) escaped = escaped.Replace(c, "\\\\" + c);
        return "\"" + escaped.Replace("%", "%%") + "\"";
    }

    public override void RemoveShortcuts()
    {
        foreach (var p in new[] { ShortcutPlace.Desktop, ShortcutPlace.StartMenu })
            try { var f = ShortcutPath(p); if (File.Exists(f)) File.Delete(f); } catch (Exception) { }
    }

    protected override void OpenFolderNow(string folder) =>
        Process.Start(new ProcessStartInfo("xdg-open") { ArgumentList = { folder }, UseShellExecute = false })?.Dispose();

    // ---- processes ----

    [DllImport("libc", SetLastError = true)]
    static extern int kill(int pid, int sig);
    const int SIGTERM = 15, SIGKILL = 9;

    static string? Which(string name)
    {
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "/usr/bin:/bin").Split(':'))
            if (dir.Length > 0 && File.Exists(Path.Combine(dir, name))) return Path.Combine(dir, name);
        return null;
    }

    /// <summary>Through setsid, so the program leads a process group of its own and Stop reaches the clang and lift workers too
    /// (research/12 4.6: Process cannot start a child in a new session). A child .NET starts is no group leader, so setsid
    /// calls setsid() and execs in place: the process id is the group's id. -w: should it have to fork, it waits.</summary>
    public override Process StartTree(ProcessStartInfo info)
    {
        if (Which("setsid") is { } setsid)
        {
            var wrapped = new ProcessStartInfo(setsid)
            {
                UseShellExecute = false, WorkingDirectory = info.WorkingDirectory, CreateNoWindow = info.CreateNoWindow,
                RedirectStandardOutput = info.RedirectStandardOutput, RedirectStandardError = info.RedirectStandardError,
                RedirectStandardInput = info.RedirectStandardInput,
                StandardOutputEncoding = info.StandardOutputEncoding, StandardErrorEncoding = info.StandardErrorEncoding,
            };
            wrapped.ArgumentList.Add("-w");
            wrapped.ArgumentList.Add(info.FileName);
            foreach (var a in info.ArgumentList) wrapped.ArgumentList.Add(a);
            wrapped.Environment.Clear();
            foreach (var kv in info.Environment) wrapped.Environment[kv.Key] = kv.Value;
            return Process.Start(wrapped) ?? throw new InvalidOperationException("setsid did not start.");
        }
        return Process.Start(info) ?? throw new InvalidOperationException(info.FileName + " did not start.");
    }

    /// <summary>kill(-pgid, SIGTERM), then SIGKILL after 5 s (the Tk builder's killpg; make_game reports an outside SIGTERM as
    /// "out of memory", but a Stop is told apart by the runner, which knows it asked).</summary>
    public override void KillTree(Process process)
    {
        try
        {
            if (process.HasExited) return;
            if (kill(-process.Id, SIGTERM) != 0) kill(process.Id, SIGTERM);
            if (!process.WaitForExit(5000))
            {
                kill(-process.Id, SIGKILL);
                try { process.Kill(entireProcessTree: true); } catch (Exception) { }
            }
        }
        catch (Exception)
        {
            try { process.Kill(entireProcessTree: true); } catch (Exception) { }
        }
    }

    /// <summary>/proc/&lt;pid&gt;/comm (the first 15 characters of the name) and /proc/&lt;pid&gt;/exe.</summary>
    public override IReadOnlyList<RunningProcess> FindProcesses(params string[] names)
    {
        var found = new List<RunningProcess>();
        string[] dirs;
        try { dirs = Directory.GetDirectories("/proc"); } catch (Exception) { return found; }
        foreach (var d in dirs)
        {
            if (!int.TryParse(Path.GetFileName(d), out var pid)) continue;
            string comm;
            try { comm = File.ReadAllText(Path.Combine(d, "comm")).Trim(); } catch (Exception) { continue; }
            string? exe = null;
            try { exe = new FileInfo(Path.Combine(d, "exe")).LinkTarget; } catch (Exception) { }
            var exeName = exe != null ? Path.GetFileName(exe) : null;
            foreach (var n in names)
                if (string.Equals(comm, n.Length > 15 ? n.Substring(0, 15) : n, StringComparison.OrdinalIgnoreCase) ||
                    exeName != null && string.Equals(exeName, n, StringComparison.OrdinalIgnoreCase))
                {
                    found.Add(new RunningProcess(pid, n, exe));
                    break;
                }
        }
        return found;
    }

    // ---- disks ----

    // (struct statvfs, x86_64 and aarch64 glibc: eleven unsigned longs then six ints of padding)
    [StructLayout(LayoutKind.Sequential)]
    struct StatVfs
    {
        public ulong bsize, frsize, blocks, bfree, bavail, files, ffree, favail, fsid, flag, namemax;
        public int s0, s1, s2, s3, s4, s5;
    }

    [DllImport("libc", SetLastError = true, CharSet = CharSet.Ansi)]
    static extern int statvfs(string path, out StatVfs buf);

    public override long? FreeBytes(string folder)
    {
        try
        {
            for (var d = Path.GetFullPath(folder); !string.IsNullOrEmpty(d); d = Path.GetDirectoryName(d))
                if (Directory.Exists(d))
                    return statvfs(d, out var s) == 0 ? (long)Math.Min(s.bavail * s.frsize, (ulong)long.MaxValue) : null;
        }
        catch (Exception) { }
        return null;
    }

    /// <summary>The mount point that holds the folder (the longest one that is a prefix of it).</summary>
    public override string Disk(string folder)
    {
        try
        {
            var full = Path.GetFullPath(folder);
            var best = "/";
            foreach (var d in DriveInfo.GetDrives())
            {
                var root = d.RootDirectory.FullName.TrimEnd('/') + "/";
                if ((full + "/").StartsWith(root, StringComparison.Ordinal) && root.Length > best.Length) best = d.RootDirectory.FullName;
            }
            return best;
        }
        catch (Exception) { return folder; }
    }

    public override bool IsDiskFull(Exception? e)
    {
        for (; e != null; e = e.InnerException)
            if (e is IOException)
            {
                var code = e.HResult & 0xFFFF;
                if (code == 28 || code == 122 || code == 112 || code == 39) return true;   // (ENOSPC, EDQUOT, and the Windows codes .NET may map them to)
                if (e.Message.Contains("No space left", StringComparison.OrdinalIgnoreCase) || e.Message.Contains("quota", StringComparison.OrdinalIgnoreCase)) return true;
            }
        return false;
    }

    public override void ReplaceFile(string from, string to) => File.Move(from, to, true);   // (rename(2): one step)

    [DllImport("libc", SetLastError = true, CharSet = CharSet.Ansi)]
    static extern int lstat(string path, byte[] buf);

    /// <summary>st_nlink &gt; 1 (x86_64 glibc layout: st_dev 8, st_ino 8, st_nlink 8 at offset 16; the AppImage is x86_64 only).
    /// Elsewhere unknown: false.</summary>
    public override bool HasOtherName(string file)
    {
        if (RuntimeInformation.ProcessArchitecture != Architecture.X64) return false;
        try
        {
            var buf = new byte[256];
            return lstat(file, buf) == 0 && BitConverter.ToUInt64(buf, 16) > 1;
        }
        catch (Exception) { return false; }
    }
}
