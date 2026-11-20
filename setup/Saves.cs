// Saves of this game (Ocarina of Time, 0400486024DEC000), wherever they are: Triforce keeps them in <game>\user\save\<uid>, emulators
// in their NAND (Eden, yuzu, suyu, sudachi, citron, torzu, EvilOOT: nand\user\save\0000000000000000\<user>\0400486024DEC000) or
// Ryujinx's bis\user\save\<index>\0, NX2navi in its game folder's profile (NaviSave). The files are the same everywhere
// (account.sav, option.sav, slot_00..05\caption.sav + progress.sav), so converting is a copy, with what gets replaced backed up first.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace TriforceSetup;

sealed class SaveSet
{
    public string Name = "", Folder = "";
    public int Slots;
    public DateTime Last;
    public bool Empty;   // (a place with no save yet: only a destination)

    public override string ToString() => Name;
}

static class Saves
{
    public const string TitleId = "0400486024DEC000";
    /// Triforce's save folder (host/nn_applet.c user_uid, host/nn_fs.c: save\<uid hi><uid lo>)
    public const string TriforceUid = "5a3c9e1f0b7d24680000000000000001";
    static readonly string[] Emulators = { "eden", "EvilOOT", "yuzu", "suyu", "sudachi", "citron", "torzu", "Ryujinx" };

    public static string TriforceFolder(string game) => Path.Combine(game, "user", "save", TriforceUid);

    /// Triforce commits its save as a transaction (host/save_tx.h): the game's save mount is a working copy in
    /// <user>\save-tx\<uid>\work, and a finished commit that could not be put in place yet (ready-<n>, the highest n) waits beside
    /// it and is newer than the save folder. The folder of that transaction for a save folder <user>\save\<uid>, or null when
    /// the folder is not such a save folder
    static string? TriforceTx(string save)
    {
        var parent = Path.GetDirectoryName(save.TrimEnd('\\'));
        if (parent == null || !string.Equals(Path.GetFileName(parent), "save", StringComparison.OrdinalIgnoreCase)) return null;
        var user = Path.GetDirectoryName(parent);
        return user == null ? null : Path.Combine(user, "save-tx", Path.GetFileName(save.TrimEnd('\\')));
    }

    /// The save Triforce would load at its next start: a finished commit still waiting, else the save folder
    public static string TriforceNewest(string game)
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

    /// A folder holding this game's saves
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

    /// The save an NX2navi game would load, given its game folder or that folder's profile folder. NX2navi keeps the save in
    /// <game>\profile\save\<account> (32 hex digits) and commits through <game>\profile\save-tx: a finished commit there
    /// (ready-<n>, the highest n) is newer than the account folder until the game's next start swaps it in. Null when the folder is
    /// no NX2navi game or holds no save
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

    /// The game folder NX2navi's launcher built into, from its settings (<launcher>\data\launcher.json, "game_dir"); null when
    /// the launcher is not there or never built a game
    static string? NaviGameDir(string launcher)
    {
        try
        {
            var file = Path.Combine(launcher, "data", "launcher.json");
            if (!File.Exists(file)) return null;
            var json = File.ReadAllText(file).TrimStart('\uFEFF');
            var parsed = new System.Web.Script.Serialization.JavaScriptSerializer().DeserializeObject(json) as Dictionary<string, object>;
            return parsed != null && parsed.TryGetValue("game_dir", out var g) && g is string { Length: > 0 } game ? game : null;
        }
        catch (Exception) { return null; }
    }

    /// Every save of this game found on the PC: the emulators' usual folders, the folders of programs the desktop and Start menu start
    /// (portable emulators keep a "user" folder beside them), Ryujinx's numbered saves that hold this game's files, and NX2navi's
    /// games (its launcher's own game folder and the one its settings name, <drive>:\Games\NX2navi, and beside a shortcut's program)
    public static List<SaveSet> FindEmulatorSaves()
    {
        var found = new List<SaveSet>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        void Add(string name, string dir)
        {
            try
            {
                if (!IsSaveSet(dir)) return;
                var full = Path.GetFullPath(dir).TrimEnd('\\');
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
        void Ryujinx(string name, string root)   // (root\bis\user\save\<index>\0)
        {
            var saves = Path.Combine(root, "bis", "user", "save");
            if (!Directory.Exists(saves)) return;
            foreach (var s in SafeDirs(saves)) Add(name, Path.Combine(s, "0"));
        }
        void Navi(string game)   // (an NX2navi game folder)
        {
            if (NaviSave(game) is { } save) Add("NX2navi", save);
        }
        void NaviLauncher(string launcher)   // (the folder of NX2navi's launcher: its own game folder, and the one it was told to use)
        {
            Navi(Path.Combine(launcher, "game"));
            if (NaviGameDir(launcher) is { } game) Navi(game);
        }
        var appData = Install.TestRoot != null ? Path.Combine(Install.TestRoot, "Roaming") : Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        var local = Install.TestRoot != null ? Path.Combine(Install.TestRoot, "Local") : Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        NaviLauncher(Path.Combine(local, "Programs", "NX2navi"));
        if (Install.TestRoot == null)
            foreach (var drive in DriveInfo.GetDrives())
            {
                try { if (drive.DriveType == DriveType.Fixed && drive.IsReady) Navi(Path.Combine(drive.RootDirectory.FullName, "Games", "NX2navi")); }
                catch (Exception) { }
            }
        foreach (var e in Emulators)
        {
            var root = Path.Combine(appData, e);
            if (e == "Ryujinx") Ryujinx("Ryujinx", root); else Nand(Pretty(e), root);
        }
        // (portable copies: the programs the shortcuts start)
        foreach (var exe in Install.TestRoot != null ? Enumerable.Empty<string>() : ShortcutTargets())
        {
            var dir = Path.GetDirectoryName(exe);
            if (dir == null) continue;
            var name = Path.GetFileNameWithoutExtension(exe) + " (" + dir + ")";
            Nand(name, Path.Combine(dir, "user"));
            Ryujinx(name, Path.Combine(dir, "portable"));
            Navi(dir);           // (a shortcut to the game itself)
            NaviLauncher(dir);   // (a shortcut to NX2navi's launcher)
        }
        return found.OrderByDescending(s => s.Last).ToList();
    }

    static string Pretty(string e) => e switch
    {
        "eden" => "Eden", "yuzu" => "yuzu", "suyu" => "suyu", "sudachi" => "Sudachi", "citron" => "Citron", "torzu" => "torzu", _ => e,
    };

    static IEnumerable<string> SafeDirs(string d)
    {
        try { return Directory.GetDirectories(d); } catch (Exception) { return Array.Empty<string>(); }
    }

    /// The .exe files the desktop and Start menu shortcuts start
    static IEnumerable<string> ShortcutTargets()
    {
        var places = new[]
        {
            Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory),
            Environment.GetFolderPath(Environment.SpecialFolder.Programs), Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms),
        };
        var exes = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Type? type = Type.GetTypeFromProgID("WScript.Shell");
        if (type == null) return exes;
        object? shell = null;
        try
        {
            shell = Activator.CreateInstance(type);
            foreach (var p in places.Where(Directory.Exists))
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

    /// A chosen folder: the save set it is, or the first one inside it (a few levels down)
    public static string? FindIn(string dir)
    {
        if (IsSaveSet(dir)) return dir;
        // (an NX2navi game folder or its profile: the save the game would load, which a search by depth would not pick when a
        // finished commit waits beside the account folder)
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

    /// A game or emulator that has these saves open: it would write over the copy when it closes
    public static string? RunningGame()
    {
        // ("ocarina" is the program NX2navi builds)
        foreach (var n in new[] { "Triforce", "hookshot", "EvilOOT", "eden", "yuzu", "suyu", "sudachi", "citron", "torzu", "Ryujinx", "ocarina" })
            foreach (var p in Process.GetProcessesByName(n))
                if (Install.TestRoot == null || InTestRoot(p)) return n == "ocarina" ? "NX2navi" : n;
        return null;
    }

    /// (testing, TRIFORCE_TEST_ROOT: only a game started from inside the test folder counts. The PC's own games have their own
    /// saves, which a test neither reads nor touches)
    static bool InTestRoot(Process p)
    {
        try { return p.MainModule!.FileName.StartsWith(Install.TestRoot!, StringComparison.OrdinalIgnoreCase); }
        catch (Exception) { return false; }
    }

    /// Copies one save set over another: the destination's old files are moved to the backup folder first (nothing is merged slot by
    /// slot: the copy is exactly the source). Returns the backup folder, or null when there was nothing to back up.
    /// ((Triforce) The games' save locks are held across all of it (Guard.cs SaveLock: a game that runs refuses it, one started
    /// meanwhile waits), and the copy is made as <save>.partial and swapped in once complete (Guard.cs Partial): a copy that fails
    /// or is cut off leaves the destination as it was, never half a save. TRIFORCE_SAVE_LOCK=0: no lock, as before)
    public static string? Copy(string from, string to, string backupRoot, string label)
    {
        var src = Path.GetFullPath(from).TrimEnd('\\');
        var dst = Path.GetFullPath(to).TrimEnd('\\');
        if (!Install.Allowed(dst)) throw new InvalidOperationException(Install.TestOnly);
        if (string.Equals(src, dst, StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("That is the same save folder.");
        var locking = Environment.GetEnvironmentVariable("TRIFORCE_SAVE_LOCK") != "0";
        using var dstLock = locking ? SaveLock.Take(dst) : null;
        using var srcLock = locking && !string.Equals(SaveLock.PathOf(src), SaveLock.PathOf(dst), StringComparison.OrdinalIgnoreCase) ? SaveLock.Take(src) : null;
        Partial.Recover(dst);
        var partial = Partial.Stage(src, dst);
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
            Partial.Remove(partial);
            throw;
        }
        Partial.SwapIn(partial, dst);
        // (into Triforce: its working copy and any finished commit still waiting belong to the save that was replaced, and the game
        // would put such a commit over the copy at its next start)
        if (TriforceTx(dst) is { } tx && Directory.Exists(tx))
        {
            try { Directory.Delete(tx, true); } catch (Exception) { }
        }
        return backup;
    }

    static void CopyTree(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var f in Directory.GetFiles(from)) File.Copy(f, Path.Combine(to, Path.GetFileName(f)), true);
        foreach (var d in Directory.GetDirectories(from)) CopyTree(d, Path.Combine(to, Path.GetFileName(d)));
    }
}
