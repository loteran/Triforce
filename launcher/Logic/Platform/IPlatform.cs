// What differs between Windows and Linux, behind one face (DESIGN.md 8: "the Windows-only parts behind an IPlatform";
// research/02 section 11 lists each Windows mechanism and its Linux equivalent). Everything else in Triforce.Logic is
// plain .NET and the same on both.
//
// TRIFORCE_TEST_ROOT=<folder> (old Setup.cs Install.TestRoot) keeps a whole install inside one folder on both systems: the
// settings are <root>\registry.txt, the shortcuts, the build's work folder, the default game folder, the emulators' saves
// and Steam are folders inside it, and nothing outside it is written (TriforceContext.Allowed). A test run never reads or
// writes the real registry, the real shortcuts, the real Steam or the real game.
using System.Diagnostics;

namespace Triforce.Logic;

/// <summary>Where a shortcut to the launcher goes.</summary>
public enum ShortcutPlace
{
    /// <summary>Windows: the desktop's Triforce.lnk. Linux: ~/Desktop/triforce-launcher.desktop when there is a desktop folder.</summary>
    Desktop,
    /// <summary>Windows: Start menu\Programs\Triforce\Triforce.lnk. Linux: ~/.local/share/applications/triforce-launcher.desktop.</summary>
    StartMenu,
}

/// <summary>A shortcut's contents.</summary>
public sealed record Shortcut(string Target, string Arguments, string WorkingDirectory, string Icon, string Description, string Name = "Triforce");

/// <summary>Something the App shows as a system notification (a build finished while the window was not in front, DESIGN 6).</summary>
public sealed record LauncherNotification(string Title, string Text);

/// <summary>A running program: its process id, name and, when it can be read, the full path of its executable.</summary>
public sealed record RunningProcess(int Id, string Name, string? Exe);

public interface IPlatform
{
    bool IsWindows { get; }
    /// <summary>TRIFORCE_TEST_ROOT, as a full path, or null.</summary>
    string? TestRoot { get; }
    /// <summary>An environment variable as this context sees it (a test's own values first).</summary>
    string? Env(string name);
    /// <summary>The settings: HKCU\Software\Triforce (Windows), ~/.config/triforce/builder.ini (Linux), registry.txt (test run).</summary>
    ISettingsStore Settings { get; }

    // ---- paths ----
    /// <summary>Windows %LOCALAPPDATA%\Programs\Triforce; Linux the self-update root ${XDG_DATA_HOME}/triforce/builder.</summary>
    string DefaultInstallDir { get; }
    /// <summary>make_game.py's work folder (its default --work): build_times.json, objcache, sa... (research/02 1.1, 12 section 2).</summary>
    string WorkFolder { get; }
    string DefaultGameFolder { get; }
    /// <summary>Where setup.log is written for an install folder (Windows: the folder itself; Linux ${XDG_STATE_HOME}/triforce).</summary>
    string LogFolder(string home);
    /// <summary>Where NVIDIA's DLSS SDK is fetched to: beside the tools, never inside them (an update replaces those whole).</summary>
    string NgxSdkFolder(string home);
    /// <summary>The save folder of a game folder: Windows &lt;game&gt;\user\save\&lt;uid&gt;; Linux &lt;UserFolder&gt;/save/&lt;uid&gt;.</summary>
    string SaveFolder(string game, string uid);
    /// <summary>The game's user folder (its saves, their commits, its options): what --user names. Windows &lt;game&gt;\user;
    /// Linux ${XDG_DATA_HOME}/triforce/user, or the folder an older build used while the game has not moved it yet.</summary>
    string UserFolder(string game);
    /// <summary>The folders emulators keep their data in: (Roaming, Local) on Windows; (data, config) on Linux.</summary>
    (string Roaming, string Local) EmulatorDataRoots { get; }
    /// <summary>Steam folders to look in, in order (test run: &lt;root&gt;\Steam only).</summary>
    IReadOnlyList<string> SteamRoots();

    // ---- shortcuts and the shell ----
    string ShortcutPath(ShortcutPlace place);
    void CreateShortcut(ShortcutPlace place, Shortcut shortcut);
    /// <summary>Removes the launcher's shortcuts (both places); quietly when one cannot be.</summary>
    void RemoveShortcuts();
    /// <summary>Explorer / xdg-open on a folder. Not in a test run (nothing opens a window there).</summary>
    void OpenFolder(string folder);
    /// <summary>The App listens and shows it (a toast, a desktop notification).</summary>
    event EventHandler<LauncherNotification>? NotificationRequested;
    void Notify(string title, string text);

    // ---- processes ----
    /// <summary>Starts a program so that it and every program it starts can be stopped together (KillTree). Linux: setsid, its
    /// own process group; Windows: as is (taskkill /T walks the tree).</summary>
    Process StartTree(ProcessStartInfo info);
    /// <summary>Stops a program and its children, waiting up to 5 s: taskkill /PID n /T /F; kill(-pgid, SIGTERM) then SIGKILL.</summary>
    void KillTree(Process process);
    /// <summary>The running programs of these names (case-insensitive), with their executables where they can be read.</summary>
    IReadOnlyList<RunningProcess> FindProcesses(params string[] names);

    // ---- disks ----
    /// <summary>Free bytes for this user on the disk of a folder (the nearest existing folder above it); null when unknown.</summary>
    long? FreeBytes(string folder);
    /// <summary>The disk as the player knows it: C:\ on Windows, the mount point on Linux.</summary>
    string Disk(string folder);
    /// <summary>Is this the error of a disk that filled while it was written to? (ERROR_DISK_FULL / ENOSPC)</summary>
    bool IsDiskFull(Exception? e);
    /// <summary>from over to in one step (MoveFileExW REPLACE_EXISTING|WRITE_THROUGH; rename(2)).</summary>
    void ReplaceFile(string from, string to);
    /// <summary>Does a file have a second name (a hard link)? Writing one writes the other.</summary>
    bool HasOtherName(string file);
}
