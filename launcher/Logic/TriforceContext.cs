// One launcher run's view of the PC: the platform, the settings, the log and where Triforce is installed. Every service in
// Triforce.Logic takes one; nothing keeps state in statics (a test makes as many contexts, with as many test folders, as it
// likes, side by side).
namespace Triforce.Logic;

public sealed class TriforceContext
{
    public IPlatform Platform { get; }
    public Settings Settings { get; }
    public Log Log { get; }
    /// <summary>(old --snapshot) nothing is written to the settings or the log.</summary>
    public bool NoWrites { get; set; }
    /// <summary>The running launcher's executable (Environment.ProcessPath); a test may name another.</summary>
    public string SelfPath { get; }
    public string SelfDir => Path.GetDirectoryName(SelfPath)!;
    /// <summary>Free bytes of the disk a folder is on (the platform's answer; a test puts its own here).</summary>
    public Func<string, long?> FreeBytes { get; set; }

    public TriforceContext(IPlatform platform, string? selfPath = null)
    {
        Platform = platform;
        SelfPath = Path.GetFullPath(selfPath ?? Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, BuildInfo.SetupAsset));
        Settings = new Settings(platform.Settings, () => NoWrites);
        FreeBytes = platform.FreeBytes;
        Log = new Log(this);
    }

    /// <summary>The context of this process: its platform, with the environment (or a test's own values over it).</summary>
    public static TriforceContext Create(IReadOnlyDictionary<string, string?>? env = null, string? selfPath = null) =>
        new(PlatformBase.Create(env), selfPath);

    public string? TestRoot => Platform.TestRoot;
    public string? Env(string name) => Platform.Env(name);
    public bool IsWindows => Platform.IsWindows;

    static StringComparison PathCase => OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;

    /// <summary>The install folder from the Apps-list entry, when Triforce is installed (only when its TriforceSetup.exe is there:
    /// old Setup.cs InstalledDir). Linux: none (the AppImage is wherever the player keeps it).</summary>
    public string? InstalledDir
    {
        get
        {
            var d = Platform.Settings.GetUninstall("InstallLocation");
            return d != null && File.Exists(Path.Combine(d, BuildInfo.SetupAsset)) ? d : null;
        }
    }

    /// <summary>The folder this launcher works in: tools\, downloads\, save-backups\, setup.log, TriforceSetup.old.exe. The install
    /// folder, else the folder it runs from (Windows); the self-update root ~/.local/share/triforce/builder (Linux).</summary>
    public string Home => InstalledDir ?? (Platform.IsWindows ? SelfDir : Platform.DefaultInstallDir);

    /// <summary>Is this run the installed launcher (its folder is the install folder)?</summary>
    public bool RunningInstalled => InstalledDir is { } d && SamePath(d, SelfDir);

    public static bool SamePath(string a, string b)
    {
        try { return string.Equals(Path.GetFullPath(a).TrimEnd('\\', '/'), Path.GetFullPath(b).TrimEnd('\\', '/'), PathCase); }
        catch (Exception) { return false; }
    }

    /// <summary>Is path the folder or inside it?</summary>
    public static bool Inside(string folder, string path)
    {
        try
        {
            string root = Path.GetFullPath(folder).TrimEnd('\\', '/'), full = Path.GetFullPath(path).TrimEnd('\\', '/');
            return string.Equals(full, root, PathCase) || full.StartsWith(root + Path.DirectorySeparatorChar, PathCase) ||
                   OperatingSystem.IsWindows() && full.StartsWith(root + "/", PathCase);
        }
        catch (Exception) { return false; }
    }

    /// <summary>May this run write there? Always, except in a test run (TRIFORCE_TEST_ROOT), which installs, builds, copies saves
    /// and starts games only inside its own folder, whatever folder is typed or picked: so a look at the launcher cannot change
    /// the PC's real install, real game or real saves (old Setup.cs Install.Allowed).</summary>
    public bool Allowed(string path) => TestRoot == null || Inside(TestRoot, path);

    /// <summary>Playing and the game's own settings file: as Allowed, and in a test run also inside the one game folder
    /// TRIFORCE_TEST_GAME names (the owner's try-it run: Play, the benchmark and the game's options file on his real game,
    /// while saves, mods, the install and everything else stay inside the test folder).</summary>
    public bool AllowedForGame(string path) => Allowed(path) || Env("TRIFORCE_TEST_GAME") is { Length: > 0 } g && Inside(Path.GetFullPath(g), path);

    public string TestOnly => "This is a test run of Triforce: it only writes inside its own folder, " + TestRoot + ".";

    /// <summary>Throws the TestOnly sentence when a path is not allowed.</summary>
    public void Require(string path)
    {
        if (!Allowed(path)) throw new InvalidOperationException(TestOnly);
    }

    // ---- folders the launcher keeps (old research/02 1.1) ----
    public string ToolsFolder => Path.Combine(Home, "tools");
    public string DownloadsFolder => Path.Combine(Home, "downloads");
    public string SaveBackupsFolder => Path.Combine(Home, "save-backups");
    public string NgxSdkFolder => Platform.NgxSdkFolder(Home);
    public string WorkFolder => Platform.WorkFolder;
    public string DefaultGameFolder => Platform.DefaultGameFolder;
    /// <summary>The launcher's exe in the install folder: what HOOKSHOT_BUILDER_EXE names, so every game folder's builder= line
    /// points back here (old Setup.cs Runner.Info). Linux: AppRun sets HOOKSHOT_BUILDER_EXE itself ($APPIMAGE or AppRun).</summary>
    public string BuilderExe => Platform.IsWindows
        ? (InstalledDir is { } d ? Path.Combine(d, BuildInfo.SetupAsset) : SelfPath)
        : Env("HOOKSHOT_BUILDER_EXE") ?? Env("APPIMAGE") ?? SelfPath;

    /// <summary>The space a disk lacks for something, in the old words; null when there is room (or it is not known).</summary>
    public string? SpaceProblem(string what, long need, string folder) => Space.Problem(what, need, FreeBytes(folder), Platform.Disk(folder));
}
