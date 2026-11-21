// The launcher's small settings, and the Apps-list entry that says where it is installed.
//   Windows    HKCU\Software\Triforce (REG_SZ strings) and HKCU\...\Uninstall\Triforce (old Setup.cs:219-259)
//   test run   <TRIFORCE_TEST_ROOT>\registry.txt: one line per value, "<key>\<name>=<value>", the last line for a name wins.
//              The same file the old Setup's test runs write, on every system
//   Linux      ${XDG_CONFIG_HOME:-~/.config}/triforce/builder.ini: flat key=value lines, rewritten whole (the Tk builder's file,
//              builder/linux/hookshot_builder.py load_config/save_config, which keeps keys it does not know). The names the
//              Tk builder and hookshot-launch read are written under their names there (out, language, dlss)
using System.Runtime.Versioning;
using System.Text;

namespace Triforce.Logic;

/// <summary>The values the launcher keeps between runs (old research/02 1.2 names).</summary>
public static class SettingNames
{
    public const string Desktop = "Desktop", StartMenu = "StartMenu", Language = "Language", GameFolder = "GameFolder", Dlss = "Dlss",
        SetupSkip = "SetupSkip", Sounds = "Sounds", ModsBuilt = "ModsBuilt";
    // (DESIGN 10: kept per user)
    public const string MusicVolume = "MusicVolume", SoundVolume = "SoundVolume", Muted = "Muted";
    // (the page's Color scheme, "#rrggbb": LauncherColor)
    public const string Color = "Color";
}

/// <summary>Where the settings are kept. Get/Set work on the settings; the Uninstall* calls on the Apps-list entry (Windows,
/// and a test run's registry.txt; nothing elsewhere).</summary>
public interface ISettingsStore
{
    string? Get(string name);
    void Set(string name, string value);
    /// <summary>Every setting gone (uninstall).</summary>
    void RemoveAll();
    string? GetUninstall(string name);
    void SetUninstall(string name, string value);
    void SetUninstallNumber(string name, int value);
    void RemoveUninstall();
}

/// <summary>The keys as the old Setup named them.</summary>
static class Keys
{
    public const string Uninstall = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\Triforce";
    public const string Settings = @"Software\Triforce";
}

/// <summary>HKCU, as the old Setup kept it. Only HKCU: no admin rights, nothing in HKLM.</summary>
[SupportedOSPlatform("windows")]
sealed class RegistrySettingsStore : ISettingsStore
{
    static string? Read(string key, string name)
    {
        try
        {
            using var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(key);
            return k?.GetValue(name) as string;
        }
        catch (Exception) { return null; }
    }

    static void Write(string key, string name, object value)
    {
        using var k = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(key);
        if (value is int n) k.SetValue(name, n, Microsoft.Win32.RegistryValueKind.DWord); else k.SetValue(name, value);
    }

    static void Remove(string key)
    {
        try { Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(key, false); } catch (Exception) { }
    }

    public string? Get(string name) => Read(Keys.Settings, name);
    public void Set(string name, string value) => Write(Keys.Settings, name, value);
    public void RemoveAll() => Remove(Keys.Settings);
    public string? GetUninstall(string name) => Read(Keys.Uninstall, name);
    public void SetUninstall(string name, string value) => Write(Keys.Uninstall, name, value);
    public void SetUninstallNumber(string name, int value) => Write(Keys.Uninstall, name, value);
    public void RemoveUninstall() => Remove(Keys.Uninstall);
}

/// <summary>(testing: TRIFORCE_TEST_ROOT) what would go to the registry, as a text file in the test folder (old Setup.cs:228-259):
/// "&lt;key&gt;\&lt;name&gt;=&lt;value&gt;", the last line for a name wins, a write rewrites the file without the name's older lines.</summary>
sealed class TestRootSettingsStore : ISettingsStore
{
    readonly string root, file;
    readonly object gate = new();
    public TestRootSettingsStore(string root) { this.root = root; file = Path.Combine(root, "registry.txt"); }

    string? Read(string key, string name)
    {
        var start = key + "\\" + name + "=";
        lock (gate)
            return File.Exists(file) ? File.ReadAllLines(file).LastOrDefault(l => l.StartsWith(start, StringComparison.Ordinal))?.Substring(start.Length) : null;
    }

    void Write(string key, string name, string value)
    {
        lock (gate)
        {
            Directory.CreateDirectory(root);
            var start = key + "\\" + name + "=";
            var lines = File.Exists(file) ? File.ReadAllLines(file).Where(l => !l.StartsWith(start, StringComparison.Ordinal)).ToList() : new List<string>();
            lines.Add(start + value);
            File.WriteAllLines(file, lines);
        }
    }

    void Remove(string key)
    {
        lock (gate)
            if (File.Exists(file)) File.WriteAllLines(file, File.ReadAllLines(file).Where(l => !l.StartsWith(key + "\\", StringComparison.Ordinal)).ToList());
    }

    public string? Get(string name) => Read(Keys.Settings, name);
    public void Set(string name, string value) => Write(Keys.Settings, name, value);
    public void RemoveAll() => Remove(Keys.Settings);
    public string? GetUninstall(string name) => Read(Keys.Uninstall, name);
    public void SetUninstall(string name, string value) => Write(Keys.Uninstall, name, value);
    public void SetUninstallNumber(string name, int value) => Write(Keys.Uninstall, name, value.ToString());
    public void RemoveUninstall() => Remove(Keys.Uninstall);
}

/// <summary>Linux: builder.ini, the Tk builder's own file (flat key=value, split on the first =, rewritten whole). The values
/// both read share a name: GameFolder is "out" (hookshot-launch starts the last out= line's game), Language "language",
/// Dlss "dlss". Other keys are kept as they are, the Tk builder's too (kind, nup, romfs, exefs, prepare).</summary>
sealed class IniSettingsStore : ISettingsStore
{
    readonly string file;
    readonly object gate = new();
    public IniSettingsStore(string file) { this.file = file; }
    public string FilePath => file;

    static string KeyOf(string name) => name switch
    {
        SettingNames.GameFolder => "out",
        SettingNames.Language => "language",
        SettingNames.Dlss => "dlss",
        _ => name,
    };

    List<(string Key, string Value)> Load()
    {
        var all = new List<(string, string)>();
        try
        {
            foreach (var line in File.ReadAllLines(file, Encoding.UTF8))
            {
                var eq = line.IndexOf('=');
                if (eq < 0) continue;
                var key = line.Substring(0, eq);
                all.RemoveAll(kv => kv.Item1 == key);   // (a dict in the Tk builder: the last line for a key wins)
                all.Add((key, line.Substring(eq + 1)));
            }
        }
        catch (Exception) { }
        return all;
    }

    void Save(List<(string Key, string Value)> all)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        var tmp = file + ".new";
        File.WriteAllText(tmp, string.Concat(all.Select(kv => kv.Key + "=" + kv.Value + "\n")), new UTF8Encoding(false));
        File.Move(tmp, file, true);
    }

    public string? Get(string name)
    {
        lock (gate) { var key = KeyOf(name); var hit = Load().LastOrDefault(kv => kv.Key == key); return hit.Key == null ? null : hit.Value; }
    }

    public void Set(string name, string value)
    {
        if (value.IndexOfAny(new[] { '\n', '\r' }) >= 0) throw new ArgumentException("A setting cannot hold a line break.");
        lock (gate)
        {
            var key = KeyOf(name);
            var all = Load();
            var at = all.FindIndex(kv => kv.Key == key);
            if (at >= 0) all[at] = (key, value); else all.Add((key, value));
            Save(all);
        }
    }

    public void RemoveAll()
    {
        lock (gate) try { if (File.Exists(file)) File.Delete(file); } catch (Exception) { }
    }

    // (no Apps list on Linux: the launcher is the AppImage the player keeps where they like)
    public string? GetUninstall(string name) => null;
    public void SetUninstall(string name, string value) { }
    public void SetUninstallNumber(string name, int value) { }
    public void RemoveUninstall() { }
}

/// <summary>The settings with their names and meanings. Writes are dropped while the context says NoWrites (old --snapshot).</summary>
public sealed class Settings
{
    readonly ISettingsStore store;
    readonly Func<bool> noWrites;
    internal Settings(ISettingsStore store, Func<bool> noWrites) { this.store = store; this.noWrites = noWrites; }

    public ISettingsStore Store => store;
    public string? Get(string name) => store.Get(name);
    public void Set(string name, string value) { if (!noWrites()) store.Set(name, value); }

    /// <summary>The game folder the last good build wrote (its @@done line).</summary>
    public string? GameFolder { get => Get(SettingNames.GameFolder) is { Length: > 0 } g ? g : null; set => Set(SettingNames.GameFolder, value ?? ""); }
    /// <summary>The game's language code (Languages.All), null when never chosen.</summary>
    public string? Language { get => Get(SettingNames.Language); set => Set(SettingNames.Language, value ?? ""); }
    /// <summary>NVIDIA DLSS chosen, its licence accepted ("1").</summary>
    public bool Dlss { get => Get(SettingNames.Dlss) == "1"; set => Set(SettingNames.Dlss, value ? "1" : "0"); }
    /// <summary>A release tag whose launcher could not start on this PC: never fetched again (research/02 2.4a).</summary>
    public string? SetupSkip { get => Get(SettingNames.SetupSkip); set => Set(SettingNames.SetupSkip, value ?? ""); }
    /// <summary>Shortcut choices, re-used when a downloaded copy reinstalls over an install (default on).</summary>
    public bool Desktop { get => Get(SettingNames.Desktop) != "0"; set => Set(SettingNames.Desktop, value ? "1" : "0"); }
    public bool StartMenu { get => Get(SettingNames.StartMenu) != "0"; set => Set(SettingNames.StartMenu, value ? "1" : "0"); }
    /// <summary>"&lt;gameFolder&gt;|&lt;16 hex CodeState&gt;" or "&lt;gameFolder&gt;|changed" (Mods.CodeState).</summary>
    public string? ModsBuilt { get => Get(SettingNames.ModsBuilt); set => Set(SettingNames.ModsBuilt, value ?? ""); }
    /// <summary>The old sound level: off / low / medium / high.</summary>
    public string? Sounds { get => Get(SettingNames.Sounds); set => Set(SettingNames.Sounds, value ?? ""); }
    /// <summary>DESIGN 10: 0..100, default 40.</summary>
    public int MusicVolume { get => Percent(SettingNames.MusicVolume, 40); set => Set(SettingNames.MusicVolume, Math.Clamp(value, 0, 100).ToString()); }
    /// <summary>DESIGN 10: 0..100, default 70.</summary>
    public int SoundVolume { get => Percent(SettingNames.SoundVolume, 70); set => Set(SettingNames.SoundVolume, Math.Clamp(value, 0, 100).ToString()); }
    public bool Muted { get => Get(SettingNames.Muted) == "1"; set => Set(SettingNames.Muted, value ? "1" : "0"); }
    /// <summary>The page's color scheme, "#rrggbb" in lower case: Triforce gold (LauncherColor.Default) when never chosen, as in
    /// every settings file from before it, or when what is kept does not read as one. Only a color LauncherColor.Parse takes is kept.</summary>
    public string Color
    {
        get => LauncherColor.Parse(Get(SettingNames.Color)) ?? LauncherColor.Default;
        set => Set(SettingNames.Color, LauncherColor.Parse(value) ?? throw new ArgumentException("Not a #rrggbb color: " + value));
    }

    int Percent(string name, int fallback) => int.TryParse(Get(name), out var v) && v >= 0 && v <= 100 ? v : fallback;
}
