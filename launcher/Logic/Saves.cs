// Saves of this game (Ocarina of Time, 0400486024DEC000), wherever they are (old setup/Saves.cs, Shell.Saves.cs): Triforce keeps
// them in <game>\user\save\<uid> (Linux: ~/.local/share/triforce/user/save/<uid>, host/hos_main.c saves_dir; older builds ~/.local/share/hookshot), emulators in their NAND
// (Eden, yuzu, suyu, sudachi, citron, torzu, EvilOOT: nand\user\save\0000000000000000\<user>\0400486024DEC000) or Ryujinx's
// bis\user\save\<index>\0, NX2navi in its game folder's profile (NaviSave). The files are the same everywhere (account.sav,
// option.sav, slot_00..05\caption.sav + progress.sav), so bringing saves in or out is a copy, with what gets replaced backed up first.
using System.Diagnostics;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Triforce.Logic;

/// <summary>A place that holds this game's saves (or, Empty, a place to copy them into).</summary>
public sealed class SaveSet
{
    public string Name { get; set; } = "";
    public string Folder { get; set; } = "";
    public int Slots { get; set; }
    public DateTime Last { get; set; }
    public bool Empty { get; set; }
    public override string ToString() => Name;
}

/// <summary>One of the six files of the game (slot_00..slot_05 = File 1..6). Caption: slot_0N\caption.sav, whose JPEG (from the first
/// FF D8 FF to the last FF D9) the App decodes for the picture (DESIGN 7); Written: when it was last saved.</summary>
public sealed record SaveSlot(int Index, string Folder, bool Used, string? Caption, DateTime? Written)
{
    /// <summary>"File 4": the game's own numbering.</summary>
    public int FileNumber => Index + 1;
}

public sealed class SavesException : Exception
{
    public SavesException(string message) : base(message) { }
}

public sealed class Saves
{
    public const string TitleId = "0400486024DEC000";
    /// <summary>Triforce's save folder name (host/nn_applet.c user_uid, host/nn_fs.c: save\&lt;uid hi&gt;&lt;uid lo&gt;).</summary>
    public const string TriforceUid = "5a3c9e1f0b7d24680000000000000001";
    public const int SlotCount = 6;
    static readonly string[] Emulators = { "eden", "EvilOOT", "yuzu", "suyu", "sudachi", "citron", "torzu", "eglantine", "Ryujinx" };

    readonly TriforceContext ctx;
    public Saves(TriforceContext ctx) { this.ctx = ctx; }

    /// <summary>Triforce's save folder for a game folder.</summary>
    public string TriforceFolder(string game) => ctx.Platform.SaveFolder(game, TriforceUid);

    /// <summary>Triforce commits its save as a transaction (host/save_tx.h): a finished commit that could not be put in place yet
    /// (&lt;user&gt;\save-tx\&lt;uid&gt;\ready-&lt;n&gt;, the highest n) waits beside the save folder and is newer. The folder of that
    /// transaction for a save folder &lt;user&gt;\save\&lt;uid&gt;, or null when it is no such save folder.</summary>
    static string? TriforceTx(string save)
    {
        var parent = Path.GetDirectoryName(save.TrimEnd('\\', '/'));
        if (parent == null || !string.Equals(Path.GetFileName(parent), "save", StringComparison.OrdinalIgnoreCase)) return null;
        var user = Path.GetDirectoryName(parent);
        return user == null ? null : Path.Combine(user, "save-tx", Path.GetFileName(save.TrimEnd('\\', '/')));
    }

    /// <summary>The save Triforce would load at its next start: a finished commit still waiting, else the save folder.</summary>
    public string TriforceNewest(string game)
    {
        var folder = TriforceFolder(game);
        try
        {
            var tx = TriforceTx(folder);
            if (tx != null && Directory.Exists(tx))
            {
                var ready = Directory.GetDirectories(tx, "ready-*")
                    .Select(d => (Dir: d, M: Regex.Match(Path.GetFileName(d), @"^ready-(\d{1,9})$")))
                    .Where(r => r.M.Success && IsSaveSet(r.Dir))
                    .OrderByDescending(r => int.Parse(r.M.Groups[1].Value)).FirstOrDefault();
                if (ready.Dir != null) return ready.Dir;
            }
        }
        catch (Exception) { }
        return folder;
    }

    /// <summary>The six files of a game folder's save, as the game would load them (a waiting commit first).</summary>
    public IReadOnlyList<SaveSlot> Slots(string game) => SlotsIn(TriforceNewest(game));

    /// <summary>The six files of any save folder.</summary>
    public static IReadOnlyList<SaveSlot> SlotsIn(string saveFolder)
    {
        var list = new List<SaveSlot>();
        for (var i = 0; i < SlotCount; i++)
        {
            var dir = Path.Combine(saveFolder, "slot_" + i.ToString("00"));
            var progress = Path.Combine(dir, "progress.sav");
            var caption = Path.Combine(dir, "caption.sav");
            var used = File.Exists(progress);
            DateTime? written = null;
            try
            {
                var times = new[] { progress, caption }.Where(File.Exists).Select(File.GetLastWriteTime).ToList();
                if (times.Count > 0) written = times.Max();
            }
            catch (Exception) { }
            list.Add(new SaveSlot(i, dir, used, File.Exists(caption) ? caption : null, written));
        }
        return list;
    }

    /// <summary>The file saved last: the one whose caption.sav was written last (DESIGN 7 "Newest save"), null when none is used.</summary>
    public SaveSlot? Newest(string game) =>
        Slots(game).Where(s => s.Used).OrderByDescending(s => s.Caption != null ? File.GetLastWriteTime(s.Caption) : s.Written ?? DateTime.MinValue).FirstOrDefault();

    /// <summary>A folder holding this game's saves.</summary>
    public static bool IsSaveSet(string dir) =>
        Directory.Exists(dir) && (File.Exists(Path.Combine(dir, "option.sav")) || File.Exists(Path.Combine(dir, "slot_00", "progress.sav"))
                                  || File.Exists(Path.Combine(dir, "account.sav")));

    public static SaveSet Describe(string name, string dir)
    {
        var s = new SaveSet { Name = name, Folder = dir };
        if (!Directory.Exists(dir)) { s.Empty = true; return s; }
        s.Slots = Directory.GetDirectories(dir, "slot_*").Count(d => File.Exists(Path.Combine(d, "progress.sav")));
        var files = Directory.GetFiles(dir, "*.sav", SearchOption.AllDirectories);
        s.Empty = files.Length == 0;
        s.Last = files.Length > 0 ? files.Max(f => File.GetLastWriteTime(f)) : DateTime.MinValue;
        return s;
    }

    /// <summary>The save an NX2navi game would load, given its game folder or that folder's profile folder. NX2navi keeps the save in
    /// &lt;game&gt;\profile\save\&lt;account&gt; (32 hex digits) and commits through &lt;game&gt;\profile\save-tx: a finished commit there
    /// (ready-&lt;n&gt;, the highest n) is newer than the account folder until the game's next start swaps it in.</summary>
    public static string? NaviSave(string dir)
    {
        try
        {
            foreach (var profile in new[] { Path.Combine(dir, "profile"), dir })
            {
                var saves = Path.Combine(profile, "save");
                if (!Directory.Exists(saves)) continue;
                var tx = Path.Combine(profile, "save-tx");
                if (Directory.Exists(tx))
                {
                    // (the game names them with ten digits, ready-0000000001: its runtime/fs_host.c "ready-%010u", an unsigned 32-bit n)
                    var ready = Directory.GetDirectories(tx, "ready-*")
                        .Select(d => (Dir: d, M: Regex.Match(Path.GetFileName(d), @"^ready-(\d{1,10})$")))
                        .Where(r => r.M.Success && IsSaveSet(r.Dir))
                        .OrderByDescending(r => long.Parse(r.M.Groups[1].Value)).FirstOrDefault();
                    if (ready.Dir != null) return ready.Dir;
                }
                var account = Directory.GetDirectories(saves)
                    .Where(d => Regex.IsMatch(Path.GetFileName(d), "^[0-9a-fA-F]{32}$") && IsSaveSet(d))
                    .OrderByDescending(d => Describe("", d).Last).FirstOrDefault();
                if (account != null) return account;
            }
        }
        catch (Exception) { }
        return null;
    }

    /// <summary>The game folder NX2navi's launcher built into (&lt;launcher&gt;\data\launcher.json "game_dir").</summary>
    static string? NaviGameDir(string launcher)
    {
        try
        {
            var file = Path.Combine(launcher, "data", "launcher.json");
            if (!File.Exists(file)) return null;
            using var doc = JsonDocument.Parse(File.ReadAllText(file).TrimStart('﻿'));
            return doc.RootElement.TryGetProperty("game_dir", out var g) && g.ValueKind == JsonValueKind.String && g.GetString() is { Length: > 0 } game ? game : null;
        }
        catch (Exception) { return null; }
    }

    /// <summary>Every save of this game found on this PC, newest first: the emulators' usual folders, the folders of programs the
    /// desktop and Start menu start (portable emulators keep a "user" folder beside them), Ryujinx's numbered saves, and NX2navi's
    /// games. A test run looks only in its own Roaming and Local folders (no drives, no shortcuts).</summary>
    public List<SaveSet> FindEmulatorSaves()
    {
        var found = new List<SaveSet>();
        var seen = new HashSet<string>(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
        void Add(string name, string dir)
        {
            try
            {
                if (!IsSaveSet(dir)) return;
                var full = Path.GetFullPath(dir).TrimEnd('\\', '/');
                if (seen.Add(full)) found.Add(Describe(name, full));
            }
            catch (Exception) { }
        }
        void Nand(string name, string root)   // (root\nand\user\save\0000000000000000\<user>\<title>)
        {
            var users = Path.Combine(root, "nand", "user", "save", "0000000000000000");
            if (!Directory.Exists(users)) return;
            foreach (var u in SafeDirs(users)) Add(name, Path.Combine(u, TitleId));
        }
        void Ryujinx(string name, string root)   // (root\bis\user\save\<index>\0; the title is not checked)
        {
            var saves = Path.Combine(root, "bis", "user", "save");
            if (!Directory.Exists(saves)) return;
            foreach (var s in SafeDirs(saves)) Add(name, Path.Combine(s, "0"));
        }
        void Navi(string game) { if (NaviSave(game) is { } save) Add("NX2navi", save); }
        void NaviLauncher(string launcher)
        {
            Navi(Path.Combine(launcher, "game"));
            if (NaviGameDir(launcher) is { } game) Navi(game);
        }
        var (roaming, local) = ctx.Platform.EmulatorDataRoots;
        if (ctx.IsWindows)
        {
            NaviLauncher(Path.Combine(local, "Programs", "NX2navi"));
            if (ctx.TestRoot == null)
                foreach (var drive in DriveInfo.GetDrives())
                    try { if (drive.DriveType == DriveType.Fixed && drive.IsReady) Navi(Path.Combine(drive.RootDirectory.FullName, "Games", "NX2navi")); }
                    catch (Exception) { }
            foreach (var e in Emulators)
            {
                var root = Path.Combine(roaming, e);
                if (e == "Ryujinx") Ryujinx("Ryujinx", root); else Nand(Pretty(e), root);
            }
            // (portable copies: the programs the shortcuts start)
            foreach (var exe in ctx.TestRoot != null ? Enumerable.Empty<string>() : ShortcutTargets())
            {
                var dir = Path.GetDirectoryName(exe);
                if (dir == null) continue;
                var name = Path.GetFileNameWithoutExtension(exe) + " (" + dir + ")";
                Nand(name, Path.Combine(dir, "user"));
                Ryujinx(name, Path.Combine(dir, "portable"));
                Navi(dir);
                NaviLauncher(dir);
            }
        }
        else
        {
            // (Linux, docs/saves.md: ~/.local/share/<emu>, ~/.config/Ryujinx, the Flatpaks' own folders; NX2navi's AppImage builds
            // into ~/Games/Ocarina. Guess-level beyond docs/saves.md: check on a Deck)
            var home = ctx.TestRoot ?? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            foreach (var e in Emulators.Where(e => e != "Ryujinx"))
            {
                Nand(Pretty(e), Path.Combine(roaming, e));
                Nand(Pretty(e), Path.Combine(roaming, e.ToLowerInvariant()));
            }
            Ryujinx("Ryujinx", Path.Combine(local, "Ryujinx"));
            if (ctx.TestRoot == null)
            {
                var flat = Path.Combine(home, ".var", "app");
                foreach (var (id, emu) in new[] { ("dev.eden_emu.eden", "eden"), ("org.yuzu_emu.yuzu", "yuzu"), ("org.suyu_emu.suyu", "suyu"), ("org.citron_emu.citron", "citron") })
                    Nand(Pretty(emu) + " (Flatpak)", Path.Combine(flat, id, "data", emu));
                Ryujinx("Ryujinx (Flatpak)", Path.Combine(flat, "org.ryujinx.Ryujinx", "config", "Ryujinx"));
            }
            Navi(Path.Combine(home, "Games", "Ocarina"));
        }
        return found.OrderByDescending(s => s.Last).ToList();
    }

    static string Pretty(string e) => e.ToLowerInvariant() switch
    {
        "eden" => "Eden", "yuzu" => "yuzu", "suyu" => "suyu", "sudachi" => "Sudachi", "citron" => "Citron", "torzu" => "torzu", "eglantine" => "Eglantine",
        "eviloot" => "EvilOOT", _ => e,
    };

    static IEnumerable<string> SafeDirs(string d)
    {
        try { return Directory.GetDirectories(d); } catch (Exception) { return Array.Empty<string>(); }
    }

    /// <summary>The .exe files the desktop and Start menu shortcuts start (WScript.Shell reads the .lnk), at most 400 per folder.</summary>
    static IEnumerable<string> ShortcutTargets()
    {
        var exes = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (!OperatingSystem.IsWindows()) return exes;
        var places = new[]
        {
            Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory),
            Environment.GetFolderPath(Environment.SpecialFolder.Programs), Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms),
        };
        var type = Type.GetTypeFromProgID("WScript.Shell");
        if (type == null) return exes;
        object? shell = null;
        try
        {
            shell = Activator.CreateInstance(type);
            foreach (var p in places.Where(p => p.Length > 0 && Directory.Exists(p)))
            {
                IEnumerable<string> links;
                try { links = Directory.GetFiles(p, "*.lnk", p.EndsWith("Programs", StringComparison.OrdinalIgnoreCase) ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly); }
                catch (Exception) { continue; }
                foreach (var l in links.Take(400))
                {
                    try
                    {
                        var lnk = type.InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, shell, new object[] { l })!;
                        var target = lnk.GetType().InvokeMember("TargetPath", System.Reflection.BindingFlags.GetProperty, null, lnk, null) as string;
                        System.Runtime.InteropServices.Marshal.FinalReleaseComObject(lnk);
                        if (target != null && target.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) && File.Exists(target)) exes.Add(target);
                    }
                    catch (Exception) { }
                }
            }
        }
        catch (Exception) { }
        finally { if (shell != null) System.Runtime.InteropServices.Marshal.FinalReleaseComObject(shell); }
        return exes;
    }

    /// <summary>A chosen folder: the save set it is, an NX2navi save inside it, or the first one inside it (breadth first, 7 levels,
    /// at most 4000 folders).</summary>
    public static string? FindIn(string dir)
    {
        if (IsSaveSet(dir)) return dir;
        if (NaviSave(dir) is { } navi) return navi;
        var queue = new Queue<(string, int)>();
        queue.Enqueue((dir, 0));
        var looked = 0;
        while (queue.Count > 0 && looked++ < 4000)
        {
            var (d, depth) = queue.Dequeue();
            foreach (var s in SafeDirs(d))
            {
                if (IsSaveSet(s)) return s;
                if (depth < 7) queue.Enqueue((s, depth + 1));
            }
        }
        return null;
    }

    /// <summary>A game or emulator that has these saves open (it would write over the copy when it closes): its name, or null.
    /// In a test run only a program started from inside the test folder counts.</summary>
    public string? RunningGame()
    {
        // ("ocarina" is the program NX2navi builds)
        var names = new[] { "Triforce", "hookshot", "EvilOOT", "eden", "yuzu", "suyu", "sudachi", "citron", "torzu", "Ryujinx", "ocarina" };
        foreach (var p in ctx.Platform.FindProcesses(names))
            if (ctx.TestRoot == null || p.Exe != null && TriforceContext.Inside(ctx.TestRoot, p.Exe))
                return p.Name == "ocarina" ? "NX2navi" : p.Name;
        return null;
    }

    /// <summary>Copies one save set over another: the destination's old files are copied to the backup folder first (nothing is
    /// merged slot by slot: the copy is exactly the source). Returns the backup folder, or null when there was nothing to back up.
    /// The games' save locks are held across all of it (SaveLock), and the copy is made as &lt;save&gt;.partial and swapped in once
    /// complete (Partial): a copy that fails or is cut off leaves the destination as it was. TRIFORCE_SAVE_LOCK=0: no lock.</summary>
    public string? Copy(string from, string to, string backupRoot, string label)
    {
        var src = Path.GetFullPath(from).TrimEnd('\\', '/');
        var dst = Path.GetFullPath(to).TrimEnd('\\', '/');
        ctx.Require(dst);
        if (TriforceContext.SamePath(src, dst)) throw new InvalidOperationException("That is the same save folder.");
        var locking = ctx.Env("TRIFORCE_SAVE_LOCK") != "0";
        using var dstLock = locking ? SaveLock.Take(dst) : null;
        using var srcLock = locking && !string.Equals(SaveLock.PathOf(src), SaveLock.PathOf(dst), StringComparison.OrdinalIgnoreCase) ? SaveLock.Take(src) : null;
        var partial = new Partial(ctx.Platform);
        Partial.Recover(dst);
        var staged = partial.Stage(src, dst);
        string? backup = null;
        try
        {
            if (Directory.Exists(dst) && Directory.EnumerateFileSystemEntries(dst).Any())
            {
                var safe = string.Concat(label.Select(c => Path.GetInvalidFileNameChars().Contains(c) || c == ' ' ? '-' : c));
                backup = Path.Combine(backupRoot, DateTime.Now.ToString("yyyy-MM-dd_HH-mm-ss") + "_" + safe);
                Directory.CreateDirectory(backup);
                CopyTree(dst, backup);
            }
        }
        catch (Exception)
        {
            Partial.Remove(staged);
            throw;
        }
        partial.SwapIn(staged, dst);
        // (into Triforce: its working copy and any finished commit still waiting belong to the save that was replaced, and the game
        // would put such a commit over the copy at its next start)
        if (TriforceTx(dst) is { } tx && Directory.Exists(tx))
            try { Directory.Delete(tx, true); } catch (Exception) { }
        return backup;
    }

    static void CopyTree(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var f in Directory.GetFiles(from)) File.Copy(f, Path.Combine(to, Path.GetFileName(f)), true);
        foreach (var d in Directory.GetDirectories(from)) CopyTree(d, Path.Combine(to, Path.GetFileName(d)));
    }

    void Refuse()
    {
        if (RunningGame() is { } running)
            throw new SavesException("Close " + running + " first: it would write its own saves back over the copy when it closes.");
    }

    /// <summary>Bring saves in: a found (or chosen) save set becomes Triforce's; Triforce's own are backed up first, into
    /// &lt;install&gt;\save-backups\&lt;date&gt;_Triforce-before-&lt;name&gt;. Refused while a game or emulator runs. Returns the backup folder.</summary>
    public Task<string?> ImportAsync(SaveSet from, string game) => Task.Run(() =>
    {
        Refuse();
        if (from.Empty || !IsSaveSet(from.Folder)) throw new SavesException("That folder has no saves to bring in.");
        var backup = Copy(from.Folder, TriforceFolder(game), ctx.SaveBackupsFolder, "Triforce before " + from.Name);
        ctx.Log.Write("saves: " + from.Folder + " -> Triforce" + (backup != null ? " (backup " + backup + ")" : ""));
        return backup;
    });

    /// <summary>Copy saves out: Triforce's newest save becomes the target's; the target's are backed up first. Returns the backup folder.</summary>
    public Task<string?> ExportAsync(string game, SaveSet to) => Task.Run(() =>
    {
        Refuse();
        var source = TriforceNewest(game);
        if (!IsSaveSet(source)) throw new SavesException("Triforce has no saves yet to take out.");
        var backup = Copy(source, to.Folder, ctx.SaveBackupsFolder, to.Name + " before Triforce");
        ctx.Log.Write("saves: Triforce -> " + to.Folder + (backup != null ? " (backup " + backup + ")" : ""));
        return backup;
    });
}
