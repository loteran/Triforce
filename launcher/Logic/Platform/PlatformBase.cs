using System.Diagnostics;

namespace Triforce.Logic;

/// <summary>What both platforms share: the environment (with a test's own values first), TRIFORCE_TEST_ROOT, notifications.</summary>
public abstract class PlatformBase : IPlatform
{
    readonly IReadOnlyDictionary<string, string?>? env;

    protected PlatformBase(IReadOnlyDictionary<string, string?>? env)
    {
        this.env = env;
        // (old Setup.cs Install.TestRoot: TRIFORCE_TEST_ROOT=<folder> keeps a whole install inside that folder)
        TestRoot = Env("TRIFORCE_TEST_ROOT") is { Length: > 0 } t ? Path.GetFullPath(t) : null;
    }

    public string? TestRoot { get; }

    public string? Env(string name) =>
        env != null && env.TryGetValue(name, out var v) ? v : Environment.GetEnvironmentVariable(name);

    public abstract bool IsWindows { get; }
    public abstract ISettingsStore Settings { get; }
    public abstract string DefaultInstallDir { get; }
    public abstract string WorkFolder { get; }
    public abstract string DefaultGameFolder { get; }
    public abstract string LogFolder(string home);
    public abstract string NgxSdkFolder(string home);
    public abstract string SaveFolder(string game, string uid);
    public abstract string UserFolder(string game);
    public abstract (string Roaming, string Local) EmulatorDataRoots { get; }
    public abstract IReadOnlyList<string> SteamRoots();
    public abstract string ShortcutPath(ShortcutPlace place);
    public abstract void CreateShortcut(ShortcutPlace place, Shortcut shortcut);
    public abstract void RemoveShortcuts();

    public void OpenFolder(string folder)
    {
        // (a test run opens no window: the old Setup skipped Explorer under a UI script too)
        if (TestRoot != null) return;
        try { OpenFolderNow(folder); } catch (Exception) { }
    }

    protected abstract void OpenFolderNow(string folder);

    public event EventHandler<LauncherNotification>? NotificationRequested;
    public void Notify(string title, string text) => NotificationRequested?.Invoke(this, new LauncherNotification(title, text));

    public abstract Process StartTree(ProcessStartInfo info);
    public abstract void KillTree(Process process);
    public abstract IReadOnlyList<RunningProcess> FindProcesses(params string[] names);
    public abstract long? FreeBytes(string folder);
    public abstract string Disk(string folder);
    public abstract bool IsDiskFull(Exception? e);
    public abstract void ReplaceFile(string from, string to);
    public abstract bool HasOtherName(string file);

    /// <summary>The platform this program runs on.</summary>
    public static IPlatform Create(IReadOnlyDictionary<string, string?>? env = null) =>
        OperatingSystem.IsWindows() ? new WindowsPlatform(env) : new LinuxPlatform(env);
}
