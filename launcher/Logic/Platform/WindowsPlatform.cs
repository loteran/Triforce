// Windows: what the old WPF Setup did, without its window (research/02 sections 1, 5, 11).
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace Triforce.Logic;

[SupportedOSPlatform("windows")]
public sealed class WindowsPlatform : PlatformBase
{
    public WindowsPlatform(IReadOnlyDictionary<string, string?>? env = null) : base(env)
    {
        Settings = TestRoot != null ? new TestRootSettingsStore(TestRoot) : new RegistrySettingsStore();
    }

    public override bool IsWindows => true;
    public override ISettingsStore Settings { get; }

    /// <summary>%LOCALAPPDATA%, or the test folder (old Setup.cs Install.LocalData).</summary>
    string LocalData => TestRoot ?? Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);

    public override string DefaultInstallDir => Path.Combine(LocalData, "Programs", "Triforce");
    // (make_game.py's own default --work is %LOCALAPPDATA%\Triforce\build; a test run passes --work <root>\build)
    public override string WorkFolder => TestRoot != null ? Path.Combine(TestRoot, "build") : Path.Combine(LocalData, "Triforce", "build");
    public override string DefaultGameFolder => Path.Combine(TestRoot ?? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Games", "Triforce");
    public override string LogFolder(string home) => home;
    public override string NgxSdkFolder(string home) => Path.Combine(home, "ngx-sdk");
    public override string SaveFolder(string game, string uid) => Path.Combine(UserFolder(game), "save", uid);
    public override string UserFolder(string game) => Path.Combine(game, "user");
    public override (string Roaming, string Local) EmulatorDataRoots => TestRoot != null
        ? (Path.Combine(TestRoot, "Roaming"), Path.Combine(TestRoot, "Local"))
        : (Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData));

    public override IReadOnlyList<string> SteamRoots()
    {
        if (TestRoot != null) return new[] { Path.Combine(TestRoot, "Steam") };
        // (Steam's folder as Steam wrote it into the registry (read, never written), else the usual one: old Steam.cs FromRegistry)
        string? folder = null;
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam", false);
            folder = key?.GetValue("SteamPath") as string;
        }
        catch (Exception) { }
        var list = new List<string>();
        if (!string.IsNullOrWhiteSpace(folder)) try { list.Add(Path.GetFullPath(folder.Replace('/', '\\'))); } catch (Exception) { }
        list.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam"));
        return list;
    }

    // ---- shortcuts ----

    public override string ShortcutPath(ShortcutPlace place) => place == ShortcutPlace.Desktop
        ? Path.Combine(TestRoot != null ? Path.Combine(TestRoot, "Desktop") : Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Triforce.lnk")
        : Path.Combine(StartFolder, "Triforce.lnk");

    string StartFolder => Path.Combine(TestRoot != null ? Path.Combine(TestRoot, "Start Menu") : Environment.GetFolderPath(Environment.SpecialFolder.Programs), "Triforce");

    /// <summary>A .lnk through the WScript.Shell COM object (old Setup.cs Shortcut).</summary>
    public override void CreateShortcut(ShortcutPlace place, Shortcut s)
    {
        var link = ShortcutPath(place);
        Directory.CreateDirectory(Path.GetDirectoryName(link)!);
        var type = Type.GetTypeFromProgID("WScript.Shell") ?? throw new InvalidOperationException("WScript.Shell is not on this PC.");
        var shell = Activator.CreateInstance(type)!;
        try
        {
            var lnk = type.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { link })!;
            var t = lnk.GetType();
            t.InvokeMember("TargetPath", BindingFlags.SetProperty, null, lnk, new object[] { s.Target });
            t.InvokeMember("Arguments", BindingFlags.SetProperty, null, lnk, new object[] { s.Arguments });
            t.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, lnk, new object[] { s.WorkingDirectory });
            t.InvokeMember("IconLocation", BindingFlags.SetProperty, null, lnk, new object[] { s.Icon + ",0" });
            t.InvokeMember("Description", BindingFlags.SetProperty, null, lnk, new object[] { s.Description });
            t.InvokeMember("Save", BindingFlags.InvokeMethod, null, lnk, null);
            Marshal.FinalReleaseComObject(lnk);
        }
        finally { Marshal.FinalReleaseComObject(shell); }
    }

    public override void RemoveShortcuts()
    {
        try { var f = ShortcutPath(ShortcutPlace.Desktop); if (File.Exists(f)) File.Delete(f); } catch (Exception) { }
        try { if (Directory.Exists(StartFolder)) Directory.Delete(StartFolder, true); } catch (Exception) { }
    }

    protected override void OpenFolderNow(string folder) =>
        Process.Start(new ProcessStartInfo("explorer.exe", "\"" + folder + "\"") { UseShellExecute = true })?.Dispose();

    // ---- processes ----

    public override Process StartTree(ProcessStartInfo info) => Process.Start(info) ?? throw new InvalidOperationException(info.FileName + " did not start.");

    /// <summary>taskkill /PID n /T /F, waiting up to 5 s (old Setup.cs Runner.Cancel).</summary>
    public override void KillTree(Process process)
    {
        try
        {
            if (process.HasExited) return;
            using var k = Process.Start(new ProcessStartInfo("taskkill", "/PID " + process.Id + " /T /F") { CreateNoWindow = true, UseShellExecute = false });
            k?.WaitForExit(5000);
        }
        catch (Exception)
        {
            try { process.Kill(entireProcessTree: true); } catch (Exception) { }
        }
    }

    public override IReadOnlyList<RunningProcess> FindProcesses(params string[] names)
    {
        var found = new List<RunningProcess>();
        foreach (var n in names)
            foreach (var p in Process.GetProcessesByName(n))
                using (p)
                {
                    string? exe = null;
                    try { exe = p.MainModule?.FileName; } catch (Exception) { }
                    found.Add(new RunningProcess(p.Id, n, exe));
                }
        return found;
    }

    // ---- disks ----

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool GetDiskFreeSpaceExW(string folder, out ulong forCaller, out ulong total, out ulong free);

    public override long? FreeBytes(string folder)
    {
        try
        {
            for (var d = Path.GetFullPath(folder); !string.IsNullOrEmpty(d); d = Path.GetDirectoryName(d))
                if (Directory.Exists(d))
                    return GetDiskFreeSpaceExW(d, out var mine, out _, out _) ? (long)Math.Min(mine, (ulong)long.MaxValue) : null;
        }
        catch (Exception) { }
        return null;
    }

    public override string Disk(string folder)
    {
        try { return Path.GetPathRoot(Path.GetFullPath(folder)) is { Length: > 0 } root ? root : folder; }
        catch (Exception) { return folder; }
    }

    public override bool IsDiskFull(Exception? e)
    {
        for (; e != null; e = e.InnerException)
            if (e is IOException && (uint)e.HResult >> 16 == 0x8007 && ((e.HResult & 0xFFFF) == 112 || (e.HResult & 0xFFFF) == 39))
                return true;   // (ERROR_DISK_FULL, ERROR_HANDLE_DISK_FULL)
        return false;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileExW(string from, string to, int flags);

    public override void ReplaceFile(string from, string to)
    {
        if (!MoveFileExW(from, to, 1 | 8))   // (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
            throw new IOException("Could not put " + to + " in place (Windows error " + Marshal.GetLastWin32Error() + ").");
    }

    // (BY_HANDLE_FILE_INFORMATION: its three times are pairs of 32-bit numbers, so nothing in it is aligned to 8 bytes)
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct FileInformation
    {
        public uint Attributes;
        public long Created, Accessed, Written;
        public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(Microsoft.Win32.SafeHandles.SafeFileHandle file, out FileInformation information);

    public override bool HasOtherName(string file)
    {
        try
        {
            using var f = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            return GetFileInformationByHandle(f.SafeFileHandle, out var information) && information.Links > 1;
        }
        catch (Exception) { return false; }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern uint GetShortPathNameW(string path, System.Text.StringBuilder into, uint size);

    /// <summary>A folder's short name (C:\Users\LONGNA~1), which programs print for temporary files, when it has another one.</summary>
    public static string? ShortName(string folder)
    {
        try
        {
            var into = new System.Text.StringBuilder(1024);
            var n = GetShortPathNameW(folder, into, (uint)into.Capacity);
            return n > 0 && n < into.Capacity && !string.Equals(into.ToString(), folder, StringComparison.OrdinalIgnoreCase) ? into.ToString() : null;
        }
        catch (Exception) { return null; }
    }
}
