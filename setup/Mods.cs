// (Triforce) The mods of a game folder as they lie on the disk: what the Mods page (Shell.Mods.cs) lists and does. Nothing here
// needs the window: tests/test_setup_mods.py compiles this file with Guard.cs and runs it on folders of its own.
//
// The rules are the game's (host/mods.c). Nothing here changes them or keeps a list of its own, so a player who arranges the
// folders by hand and one who uses the page see the same thing:
//   <game>\mods\<name>\romfs\...     a mod is a folder, up to four levels down, that holds romfs, exefs or cheats
//   a name that starts with - or _    switches the folder off, and every mod in it (a name that starts with a dot is not looked at)
//   the name that sorts last wins     among the mods that carry the same file (compared without case, as the game does)
// On and off is a rename: with a "-" in front, or without. The order is a number in front of the name ("01-", "02-": what the
// readme tells players to do by hand), kept apart from the mod's own name, so "-02-Tall grass" is the mod Tall grass, second,
// switched off. RomFS files and cheats are read when the game starts; code patches (exefs) are built into the game, so a
// change of those waits for the next build (CodeState).
//
// Every change is a rename or a new folder inside the mods folder and nowhere else (Within), and none is made while the game
// runs (GameRuns). A mod that is removed goes to mods\.removed, which the game does not look at: nothing is ever deleted
// but what an archive's own unpacking left in its temporary folder.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace TriforceSetup;

/// What the Mods page tells the player when something cannot be done: a whole sentence, in plain words
sealed class ModsException : Exception
{
    public ModsException(string message) : base(message) { }
}

/// One mod as the game would find it
sealed class Mod
{
    public string Path = "";     // (its folder, in full)
    public string Rel = "";      // (its place under the mods folder, "/" between folders: what the game sorts by)
    public string Top = "";      // (the folder of the mods folder it is, or is in)
    public string Folder = "";   // (its folder's name as it is on the disk)
    public string Name = "";     // (its own name: the folder's without the switch and the number)
    public int? Number;          // (the number its folder's name starts with, "01-", when it has one)
    public bool On;
    public string? OffAbove;     // (the folder above it whose name switches it off, when it is not its own name that does)
    public List<string> Files = new();   // (its RomFS files as the game looks them up: lower case, "/" between folders)
    public long Bytes;           // (of those files)
    public int Patches, Cheats;  // (the files in its exefs and in its cheats folder)
    public bool RomfsIsLink;     // (its romfs folder is a link, which the game leaves out)

    public bool Deep => Rel.IndexOf('/') >= 0;
    /// Does it carry something that is built into the game (and so waits for the next build)?
    public bool Code => Patches > 0 || Cheats > 0;
    /// It carries code patches: what is built into the game (its cheats are read when the game starts: host/mod_cheats.c)
    public bool Built => Patches > 0;
    public override string ToString() => Rel;
}

/// A file more than one of the mods that are on carry: the last of them, in the order the game loads them, wins
sealed class Conflict
{
    public string File = "";
    public List<Mod> Mods = new();
    public Mod Winner => Mods[Mods.Count - 1];
}

static class Mods
{
    /// The game this is built for: a mod in an emulator's layout names the game it is made for by this number
    public const string TitleId = "0400486024DEC000";
    /// What the page says when a mod with code patches was added, removed or switched since the game was built
    public const string BuildAgain = "Build again to apply";
    /// Where removed mods go, and where an archive is unpacked and checked before its mod moves in. Both names start with a
    /// dot: the game does not look at such folders (host/mods.c find_mods)
    public const string Removed = ".removed", Adding = ".adding-", Moving = ".moving-";
    static readonly string[] Parts = { "romfs", "exefs", "cheats" };

    /// The most an archive may unpack to, and the most files it may hold (the game's own RomFS is 12 GB in 18,000 files)
    public static long MaxBytes = 32L << 30;
    public static int MaxFiles = 200000;
    /// How deep a path in an archive may go (the game follows a RomFS 32 folders down)
    public const int MaxDepth = 40;

    /// Does the game of this game folder run? Asked before every change. (Shell.Mods.cs puts the look at the processes here, a
    /// test its own answer.)
    public static Func<string, bool> GameRuns = _ => false;

    /// Windows' own tar.exe (bsdtar, in every Windows 10 and 11), which reads .7z and .rar; null when this PC has none.
    /// Only the one in System32 is ever started, never whatever "tar" the PATH names. (A test puts its own answer here.)
    public static Func<string?> Tar = () =>
    {
        var tar = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe");
        return File.Exists(tar) ? tar : null;
    };

    public static string FolderOf(string game) => System.IO.Path.Combine(game, "mods");

    // ---- names ----

    /// A folder's name taken apart: switched off (it starts with - or _), its number ("01-": two or three digits and a
    /// hyphen, so that "4K-grass" and "2024-pack" keep their names whole) and the mod's own name
    public static (bool Off, int? Number, string Name) Parse(string folder)
    {
        var off = folder.Length > 0 && (folder[0] == '-' || folder[0] == '_');
        var rest = folder.TrimStart('-', '_');
        var m = Regex.Match(rest, @"^(\d{2,3})-(.+)$", RegexOptions.Singleline);
        return m.Success ? (off, int.Parse(m.Groups[1].Value), m.Groups[2].Value) : (off, null, rest);
    }

    /// As the game compares names (host/mods.c mod_cmp, _stricmp): byte by byte, the letters A to Z without case
    public static int Compare(string a, string b)
    {
        byte[] x = Encoding.UTF8.GetBytes(a), y = Encoding.UTF8.GetBytes(b);
        for (var i = 0; i < x.Length && i < y.Length; i++)
        {
            int p = x[i] >= 'A' && x[i] <= 'Z' ? x[i] + 32 : x[i], q = y[i] >= 'A' && y[i] <= 'Z' ? y[i] + 32 : y[i];
            if (p != q) return p - q;
        }
        return x.Length - y.Length;
    }

    /// A RomFS path as the game looks it up (host/mods.c lower_slash): "/" between folders, A to Z in lower case
    static string Key(string rel)
    {
        var s = new StringBuilder(rel.Length);
        foreach (var c in rel) s.Append(c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? (char)(c + 32) : c);
        return s.ToString();
    }

    /// A mod's place in the list: its path as the game sorts it, read without the switches, so that a mod stays where it
    /// is when it is switched off
    static string Place(string rel) => string.Join("/", rel.Split('/').Select(p => p.TrimStart('-', '_')));

    static bool Same(string a, string b) => string.Equals(a, b, StringComparison.OrdinalIgnoreCase);

    static string Hex8() => Guid.NewGuid().ToString("N").Substring(0, 8);

    // ---- only inside the mods folder ----

    static string Full(string path) => System.IO.Path.GetFullPath(path).TrimEnd('\\', '/');

    /// Is the path the mods folder or inside it? Every folder made, renamed or written to is asked this first
    public static bool Inside(string mods, string path)
    {
        try
        {
            string root = Full(mods), full = Full(path);
            return Same(full, root) || full.StartsWith(root + "\\", StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception) { return false; }
    }

    static void Within(string mods, string path)
    {
        if (!Inside(mods, path)) throw new InvalidOperationException("Setup would have written outside the mods folder (" + path + "), and did not.");
    }

    static void MakeFolder(string mods, string dir)
    {
        Within(mods, dir);
        Directory.CreateDirectory(dir);
    }

    static void MoveFolder(string mods, string from, string to)
    {
        Within(mods, from);
        Within(mods, to);
        Directory.Move(from, to);
    }

    static void Refuse(string mods)
    {
        if (GameRuns(System.IO.Path.GetDirectoryName(Full(mods)) ?? mods))
            throw new ModsException("Close the game first. Mods are added, switched, ordered and removed only while it is not running.");
    }

    static string[] Folders(string dir)
    {
        try { return Directory.GetDirectories(dir); } catch (Exception) { return new string[0]; }
    }

    static bool IsLink(string path)
    {
        try { return (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0; } catch (Exception) { return false; }
    }

    // ---- the list ----

    /// The mods of a mods folder as the game finds them (host/mods.c find_mods), those that are off too, in the order the
    /// game loads them: the last one wins a file two of them carry
    /// (carried: false leaves out what each mod carries: the folders are found, their files not looked at)
    public static List<Mod> List(string mods, bool carried = true)
    {
        var found = new List<Mod>();
        if (Directory.Exists(mods)) Walk(mods, "", 0, null, found, carried);
        // (the game's order among the mods that are on; one that is off keeps the place it would have without its switch)
        found.Sort((a, b) =>
        {
            var c = Compare(Place(a.Rel), Place(b.Rel));
            return c != 0 ? c : string.CompareOrdinal(a.Rel, b.Rel);
        });
        return found;
    }

    /// offAbove: the first folder above this one whose name switches off everything in it
    static void Walk(string dir, string rel, int depth, string? offAbove, List<Mod> found, bool carried)
    {
        var folder = depth > 0 ? System.IO.Path.GetFileName(dir) : "";
        var (isOff, number, name) = Parse(folder);
        // (romfs and exefs make a folder a mod for the game, whatever they are; a cheats folder for the build)
        bool Has(string part) => Directory.Exists(System.IO.Path.Combine(dir, part)) || part != "cheats" && File.Exists(System.IO.Path.Combine(dir, part));
        if (depth > 0 && Parts.Any(Has))
        {
            var m = new Mod
            {
                Path = dir, Rel = rel, Top = rel.Split('/')[0], Folder = folder, Name = name.Length > 0 ? name : folder, Number = number,
                On = offAbove == null && !isOff, OffAbove = offAbove,
            };
            if (carried) Fill(m);
            found.Add(m);
            return;
        }
        if (depth >= 4) return;
        foreach (var sub in Folders(dir))
        {
            var child = System.IO.Path.GetFileName(sub);
            if (child.Length == 0 || child[0] == '.') continue;
            Walk(sub, rel.Length > 0 ? rel + "/" + child : child, depth + 1, offAbove ?? (isOff ? folder : null), found, carried);
        }
    }

    /// The file the game reads for one of its RomFS paths ("Sound/Resource/x.bwav"): that of the last mod, among those that
    /// are on, that carries it (as host/mods.c looks it up: the path without case, a link to a folder not followed, the
    /// romfs folder itself neither when it is one), else the game's own under <game>\romfs. Null when neither has it.
    /// (For Setup's own use of the game's interface sounds, Sound.cs: the mods are found, their files not all looked at.)
    public static string? RomfsFile(string game, string rel)
    {
        var parts = rel.Replace('\\', '/').Split('/').Where(p => p.Length > 0).ToArray();
        if (parts.Length == 0 || parts.Any(p => p == "." || p == ".." || p.IndexOf(':') >= 0)) return null;
        string? In(string romfs)
        {
            var at = romfs;
            for (var i = 0; ; i++)
            {
                if (!Directory.Exists(at) || IsLink(at)) return null;
                at = System.IO.Path.Combine(at, parts[i]);
                if (i == parts.Length - 1) return File.Exists(at) ? at : null;
            }
        }
        try
        {
            foreach (var m in Enumerable.Reverse(List(FolderOf(game), false)).Where(m => m.On))
                if (In(System.IO.Path.Combine(m.Path, "romfs")) is { } theirs) return theirs;
            var own = System.IO.Path.Combine(new[] { game, "romfs" }.Concat(parts).ToArray());
            return File.Exists(own) ? own : null;
        }
        catch (Exception) { return null; }
    }

    /// What a mod carries: its RomFS files as the game indexes them (host/mods.c index_romfs: 32 folders deep, a link to a
    /// folder left out, the romfs folder itself too when it is one), and how many files its exefs and cheats folders hold
    static void Fill(Mod m)
    {
        var romfs = System.IO.Path.Combine(m.Path, "romfs");
        if (Directory.Exists(romfs))
        {
            if (IsLink(romfs)) m.RomfsIsLink = true;
            else Index(m, romfs, "", 0);
        }
        m.Files.Sort(string.CompareOrdinal);
        m.Patches = Count(System.IO.Path.Combine(m.Path, "exefs"));
        m.Cheats = Count(System.IO.Path.Combine(m.Path, "cheats"));
    }

    static void Index(Mod m, string dir, string rel, int depth)
    {
        if (depth > 32) return;
        FileSystemInfo[] entries;
        try { entries = new DirectoryInfo(dir).GetFileSystemInfos(); } catch (Exception) { return; }
        foreach (var e in entries)
        {
            var isDir = (e.Attributes & FileAttributes.Directory) != 0;
            if (isDir && (e.Attributes & FileAttributes.ReparsePoint) != 0) continue;
            var path = rel.Length > 0 ? rel + "/" + e.Name : e.Name;
            if (isDir) Index(m, e.FullName, path, depth + 1);
            else
            {
                m.Files.Add(Key(path));
                try { m.Bytes += ((FileInfo)e).Length; } catch (Exception) { }
            }
        }
    }

    static int Count(string dir)
    {
        try { return Directory.Exists(dir) && !IsLink(dir) ? Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Length : 0; }
        catch (Exception) { return 0; }
    }

    /// What a mod carries, in words: "1,204 game files, 2 code patch files, 1 cheat file"
    public static string Carries(Mod m)
    {
        string Some(int n, string one, string many) => n.ToString("N0", System.Globalization.CultureInfo.InvariantCulture) + " " + (n == 1 ? one : many);
        var parts = new List<string>();
        if (m.Files.Count > 0 || !m.Code) parts.Add(m.RomfsIsLink ? "a romfs folder that is a link (the game leaves it out)" : Some(m.Files.Count, "game file", "game files"));
        if (m.Patches > 0) parts.Add(Some(m.Patches, "code patch file", "code patch files"));
        if (m.Cheats > 0) parts.Add(Some(m.Cheats, "cheat file", "cheat files"));
        return string.Join(", ", parts);
    }

    // ---- conflicts ----

    /// The files more than one of the mods that are on carry, each with its mods in the order the game loads them (the last
    /// one wins), sorted by file
    public static List<Conflict> Conflicts(IEnumerable<Mod> mods)
    {
        var by = new Dictionary<string, Conflict>(StringComparer.Ordinal);
        foreach (var m in mods.Where(m => m.On))
            foreach (var f in m.Files)
            {
                if (!by.TryGetValue(f, out var c)) by[f] = c = new Conflict { File = f };
                if (!c.Mods.Contains(m)) c.Mods.Add(m);
            }
        return by.Values.Where(c => c.Mods.Count > 1).OrderBy(c => c.File, StringComparer.Ordinal).ToList();
    }

    /// For one mod: each other mod it shares files with, how many, and whether this one wins them (it loads later)
    public static List<(Mod Other, int Files, bool Wins)> Shared(Mod mod, IList<Mod> all, IEnumerable<Conflict> conflicts)
    {
        var count = new Dictionary<Mod, int>();
        foreach (var c in conflicts.Where(c => c.Mods.Contains(mod)))
            foreach (var other in c.Mods.Where(o => o != mod))
                count[other] = count.TryGetValue(other, out var n) ? n + 1 : 1;
        return count.OrderBy(p => all.IndexOf(p.Key)).Select(p => (p.Key, p.Value, all.IndexOf(mod) > all.IndexOf(p.Key))).ToList();
    }

    /// Every conflict as text, for the clipboard: the file, the mods that carry it, and which one the game takes
    public static string ConflictText(IEnumerable<Conflict> conflicts)
    {
        var s = new StringBuilder();
        foreach (var c in conflicts)
            s.AppendLine(c.File + "  <-  " + c.Winner.Rel + " wins over " + string.Join(", ", c.Mods.Take(c.Mods.Count - 1).Select(m => m.Rel)));
        return s.ToString();
    }

    // ---- what the game must be built again for ----

    /// The code patches of the mods that are on, as one short mark: it changes when such a mod is added, removed, switched
    /// or edited, or when their order changes, and stays when only RomFS mods or cheats change or the folders are numbered
    /// anew. "" when no mod that is on carries any. Setup keeps the mark of the last build (Shell.Mods.cs)
    public static string CodeState(IEnumerable<Mod> mods)
    {
        var lines = new List<string>();
        foreach (var m in mods.Where(m => m.On && m.Built))
        {
            lines.Add("mod " + m.Name);
            foreach (var part in new[] { "exefs" })   // (not "cheats": the game reads those itself, at its start)
            {
                var dir = System.IO.Path.Combine(m.Path, part);
                string[] files = { };
                try { if (Directory.Exists(dir) && !IsLink(dir)) files = Directory.GetFiles(dir, "*", SearchOption.AllDirectories); } catch (Exception) { }
                foreach (var f in files.OrderBy(f => f, StringComparer.OrdinalIgnoreCase))
                {
                    var info = new FileInfo(f);
                    lines.Add(part + f.Substring(dir.Length).ToLowerInvariant() + "|" + info.Length + "|" + info.LastWriteTimeUtc.Ticks);
                }
            }
        }
        if (lines.Count == 0) return "";
        var h = 14695981039346656037UL;   // (FNV-1a, 64 bits: a mark, not a secret)
        foreach (var c in string.Join("\n", lines)) { h ^= c; h *= 1099511628211UL; }
        return h.ToString("x16");
    }

    /// Is the game to be built again for its mods? built is the mark Setup kept at the last build: null when it kept none
    /// (then any code a mod that is on carries is news), and anything that is no mark ("changed": a mod with code was
    /// touched before any build was marked) never matches
    public static bool NeedsBuild(string state, string? built) => built == null ? state.Length > 0 : state != built;

    // ---- on and off ----

    /// Switches a mod on or off by its folder's name: "-" in front of it, or no - and _ there. Returns the folder as it is
    /// called now
    public static string SetOn(string mods, Mod mod, bool on)
    {
        Refuse(mods);
        if (!Directory.Exists(mod.Path)) throw new ModsException(mod.Name + " is no longer in the mods folder.");
        var (off, _, _) = Parse(mod.Folder);
        if (on && mod.OffAbove != null)
            throw new ModsException(mod.Name + " is inside the folder " + mod.OffAbove + ", whose name switches off everything in it. Take the - or _ off that folder's name.");
        if (on == !off) return mod.Path;
        var name = on ? mod.Folder.TrimStart('-', '_') : "-" + mod.Folder;
        if (name.Length == 0 || name[0] == '.') throw new ModsException("Without the " + mod.Folder[0] + " in front, the folder " + mod.Folder + " has no name the game looks at. Rename it by hand.");
        var parent = System.IO.Path.GetDirectoryName(mod.Path)!;
        var target = System.IO.Path.Combine(parent, name);
        if (Directory.Exists(target) || File.Exists(target))
            throw new ModsException("There is already a folder named " + name + " in the mods folder. Rename or remove one of the two first.");
        try { MoveFolder(mods, mod.Path, target); }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            throw new ModsException(mod.Name + " could not be switched " + (on ? "on" : "off") + ": " + e.Message + " Close any window that shows its folder and try again.");
        }
        return target;
    }

    // ---- the order ----

    /// The folders of the mods folder that are mods or hold them, in the order the game loads them
    public static List<string> Tops(IEnumerable<Mod> mods) => mods.Select(m => m.Top).Distinct(StringComparer.OrdinalIgnoreCase).ToList();

    /// Moves a mod one place: later (it then wins over the mod it passes) or earlier. The folders of the mods folder are
    /// numbered anew, 01- 02- ..., each keeping its own name and its switch; a mod deeper down (mods\contents\...) moves with
    /// the folder of the mods folder it is in. False when it is at that end already
    public static bool Move(string mods, Mod mod, bool later)
    {
        Refuse(mods);
        var tops = Tops(List(mods));
        var at = tops.FindIndex(t => Same(t, mod.Top));
        if (at < 0) throw new ModsException(mod.Name + " is no longer in the mods folder.");
        var to = at + (later ? 1 : -1);
        if (to < 0 || to >= tops.Count) return false;
        (tops[at], tops[to]) = (tops[to], tops[at]);
        Number(mods, tops);
        return true;
    }

    /// The folders, in this order, under the numbers 01- 02- ... (three digits from a hundred folders on): all of them or,
    /// when one cannot be renamed, none
    static void Number(string mods, List<string> tops)
    {
        var digits = tops.Count >= 100 ? "000" : "00";
        var plan = new List<(string From, string To)>();
        for (var i = 0; i < tops.Count; i++)
        {
            var (_, _, name) = Parse(tops[i]);
            var switches = tops[i].Substring(0, tops[i].Length - tops[i].TrimStart('-', '_').Length);
            var to = switches + (i + 1).ToString(digits) + "-" + name;
            if (!string.Equals(tops[i], to, StringComparison.Ordinal)) plan.Add((tops[i], to));
        }
        var leaving = new HashSet<string>(plan.Select(p => p.From), StringComparer.OrdinalIgnoreCase);
        foreach (var (_, to) in plan)
        {
            var path = System.IO.Path.Combine(mods, to);
            if (!leaving.Contains(to) && (Directory.Exists(path) || File.Exists(path)))
                throw new ModsException("There is already something named " + to + " in the mods folder, where a mod's folder would be renamed to. Rename or remove it first. Nothing was changed.");
        }
        // (a new name that is another folder's old one, two mods of one name changing places: each goes through a name of its
        // own first, one the game does not look at)
        var steps = new List<(string From, string To)>();
        if (plan.Any(p => leaving.Contains(p.To)))
        {
            var aside = Moving + Hex8() + "-";
            steps.AddRange(plan.Select((p, i) => (p.From, aside + i)));
            steps.AddRange(plan.Select((p, i) => (aside + i, p.To)));
        }
        else steps.AddRange(plan);
        var done = new List<(string From, string To)>();
        try
        {
            foreach (var (from, to) in steps)
            {
                MoveFolder(mods, System.IO.Path.Combine(mods, from), System.IO.Path.Combine(mods, to));
                done.Add((from, to));
            }
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            var back = true;
            for (var i = done.Count - 1; i >= 0; i--)
                try { MoveFolder(mods, System.IO.Path.Combine(mods, done[i].To), System.IO.Path.Combine(mods, done[i].From)); }
                catch (Exception) { back = false; }
            throw new ModsException("The mods could not be put in that order: " + e.Message + (back ? " Nothing was changed." : " Some folders have their new names and some their old ones.") +
                                    " Close any window that shows a mod's folder and try again.");
        }
    }

    // ---- removing ----

    /// Takes a mod out of the game's sight: its folder moves into mods\.removed (under a free name), which the game does not
    /// look at. Nothing is deleted. Returns where it is now
    public static string Remove(string mods, Mod mod)
    {
        Refuse(mods);
        if (!Directory.Exists(mod.Path)) throw new ModsException(mod.Name + " is no longer in the mods folder.");
        var bin = System.IO.Path.Combine(mods, Removed);
        try
        {
            MakeFolder(mods, bin);
            var target = Free(bin, mod.Folder.TrimStart('-', '_') is { Length: > 0 } n ? n : "mod");
            MoveFolder(mods, mod.Path, target);
            return target;
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            throw new ModsException(mod.Name + " could not be removed: " + e.Message + " Close any window that shows its folder and try again.");
        }
    }

    /// dir\name, or the first of "name (2)", "name (3)" ... that is not there
    static string Free(string dir, string name)
    {
        for (var k = 1; ; k++)
        {
            var path = System.IO.Path.Combine(dir, k == 1 ? name : name + " (" + k + ")");
            if (!Directory.Exists(path) && !File.Exists(path)) return path;
        }
    }

    // ---- adding from an archive ----

    static readonly HashSet<string> Devices = new(StringComparer.OrdinalIgnoreCase) { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };

    /// Is this a name Windows keeps for a device, with or without an extension (NUL, con.txt)? A file called so is the device
    public static bool IsDevice(string name)
    {
        var dot = name.IndexOf('.');
        var stem = (dot >= 0 ? name.Substring(0, dot) : name).TrimEnd(' ');
        // (COM1 to COM9 and LPT1 to LPT9; Windows takes the raised digits 1, 2 and 3 for digits there too)
        if (stem.Length == 4 && (stem.StartsWith("COM", StringComparison.OrdinalIgnoreCase) || stem.StartsWith("LPT", StringComparison.OrdinalIgnoreCase)))
            return stem[3] >= '1' && stem[3] <= '9' || stem[3] == (char)0xB9 || stem[3] == (char)0xB2 || stem[3] == (char)0xB3;
        return Devices.Contains(stem);
    }

    /// Why a file of an archive must not be unpacked under its name, null when it may be: a path that leaves the folder it is
    /// unpacked into (from the disk's root, with a drive's letter, through ..), a device's name, a colon (a hidden stream of
    /// another file) and, where the name is known exactly (exact: a zip's own list), what Windows would drop or refuse, so that
    /// no file lands under another name than its own
    public static string? Unsafe(string name, bool exact)
    {
        var n = name.Replace('\\', '/');
        if (n.Trim().Length == 0) return "a file without a name";
        if (n[0] == '/') return "a path that starts at the disk's root";
        if (n.IndexOf(':') >= 0) return "a name with a colon or a drive's letter";
        var parts = n.Split('/').Where(p => p.Length > 0 && p != ".").ToArray();
        if (parts.Length > MaxDepth) return "folders more than " + MaxDepth + " deep";
        foreach (var p in parts)
        {
            if (p == "..") return "a path that leaves its own folder (..)";
            if (p.All(c => c == '.' || c == ' ')) return "a name of dots and spaces only";
            if (IsDevice(p)) return "a name Windows keeps for a device (" + p + ")";
            if (!exact) continue;
            if (p[p.Length - 1] == '.' || p[p.Length - 1] == ' ') return "a name that ends in a dot or a space, which Windows drops";
            if (p.Any(c => c < 32 || "<>\"|?*".IndexOf(c) >= 0)) return "a name with a character Windows does not allow";
        }
        return null;
    }

    static string Short(string name) => name.Length > 120 ? name.Substring(0, 117) + "..." : name;

    /// Adds a mod from a .zip, .7z or .rar file. The archive is unpacked into a folder of its own inside the mods folder (one
    /// the game does not look at), what lies there is checked, the folder that holds romfs, exefs or cheats is looked for, and
    /// only then does that folder move into the mods folder under the mod's name. Anything else ends with a sentence that says
    /// why, and with nothing added. Returns the new mod
    public static Mod Add(string mods, string archive, Action<double>? progress = null)
    {
        Refuse(mods);
        var kind = System.IO.Path.GetExtension(archive).ToLowerInvariant();
        if (kind != ".zip" && kind != ".7z" && kind != ".rar") throw new ModsException("That is no .zip, .7z or .rar file.");
        if (!File.Exists(archive)) throw new ModsException("The file is not there: " + archive);
        string temp;
        try
        {
            MakeFolder(mods, mods);
            Sweep(mods);
            temp = System.IO.Path.Combine(mods, Adding + Hex8());
            MakeFolder(mods, temp);
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            throw new ModsException("The mods folder could not be written to: " + e.Message);
        }
        try
        {
            var into = System.IO.Path.Combine(temp, "x");
            if (kind == ".zip") UnpackZip(mods, archive, into, progress);
            else UnpackWithTar(mods, archive, into, kind);
            Check(into, kind != ".zip");
            var root = FindRoot(into);
            // (the mod's name: its own folder's in the archive, else the archive's)
            var given = Same(root, into) || Generic(System.IO.Path.GetFileName(root)) ? System.IO.Path.GetFileNameWithoutExtension(archive) : System.IO.Path.GetFileName(root);
            var name = Clean(given);
            // (where the folders are numbered, the new mod gets the next number: it loads last, and wins, wherever its name would sort)
            var numbers = Tops(List(mods)).Select(t => Parse(t).Number).Where(n => n != null).Select(n => n!.Value).ToList();
            if (numbers.Count > 0 && Parse(name).Number == null) name = (numbers.Max() + 1).ToString(numbers.Max() + 1 >= 100 ? "000" : "00") + "-" + name;
            var target = Free(mods, name);
            MoveFolder(mods, root, target);
            progress?.Invoke(1);
            return List(mods).FirstOrDefault(m => Same(m.Path, target)) ?? throw new ModsException("The mod was unpacked into " + target + ", but the game would not find it there.");
        }
        catch (ModsException) { throw; }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is InvalidDataException || e is NotSupportedException || e is ArgumentException)
        {
            throw new ModsException(Space.Full(e) ? "The disk filled while the archive was unpacked. Nothing was added." : "The archive could not be unpacked: " + e.Message + " Nothing was added.");
        }
        finally { Delete(mods, temp); }
    }

    /// What an adding that was cut off (Setup closed in the middle of it) left in the mods folder a day ago or longer
    static void Sweep(string mods)
    {
        foreach (var d in Folders(mods))
            try
            {
                if (Regex.IsMatch(System.IO.Path.GetFileName(d), "^" + Regex.Escape(Adding) + "[0-9a-f]{8}$") && DateTime.UtcNow - Directory.GetLastWriteTimeUtc(d) > TimeSpan.FromDays(1))
                    Delete(mods, d);
            }
            catch (Exception) { }
    }

    /// Removes the folder an archive was unpacked into, with what is left in it. A link in it is removed as a link: what it
    /// points to is never followed into
    static void Delete(string mods, string dir)
    {
        try
        {
            Within(mods, dir);
            if (!Same(System.IO.Path.GetDirectoryName(Full(dir)) ?? "", Full(mods)) || !System.IO.Path.GetFileName(Full(dir)).StartsWith(Adding, StringComparison.Ordinal)) return;
            Empty(dir);
        }
        catch (Exception) { }
    }

    static void Empty(string dir)
    {
        foreach (var e in new DirectoryInfo(dir).GetFileSystemInfos())
        {
            try
            {
                if ((e.Attributes & FileAttributes.ReadOnly) != 0) e.Attributes &= ~FileAttributes.ReadOnly;
                if ((e.Attributes & FileAttributes.Directory) != 0 && (e.Attributes & FileAttributes.ReparsePoint) == 0) Empty(e.FullName);
                else if ((e.Attributes & FileAttributes.Directory) != 0) Directory.Delete(e.FullName, false);
                else File.Delete(e.FullName);
            }
            catch (Exception) { }
        }
        try { Directory.Delete(dir, false); } catch (Exception) { }
    }

    /// A zip, with .NET's own reader (what unpacks Setup's tools, Setup.cs Tools.Unpack). Its list is checked whole before the
    /// first file is written: every name, no links, no two files of one name, what it says it unpacks to against the bound and
    /// against the room on the disk. A file that turns out longer than its entry said stops the unpacking
    static void UnpackZip(string mods, string archive, string into, Action<double>? progress)
    {
        ZipArchive zip;
        try { zip = ZipFile.OpenRead(archive); }
        catch (Exception e) when (e is InvalidDataException || e is IOException || e is UnauthorizedAccessException || e is NotSupportedException)
        {
            throw new ModsException("That is not a zip file Setup can read (" + e.Message + "). Nothing was added.");
        }
        using (zip)
        {
            System.Collections.ObjectModel.ReadOnlyCollection<ZipArchiveEntry> entries;
            try { entries = zip.Entries; }
            // (.NET reads the list of names only now, and gives up on a name with a character no path may have)
            catch (ArgumentException) { throw new ModsException("The archive holds a name with a character Windows does not allow. Nothing was unpacked."); }
            catch (InvalidDataException e) { throw new ModsException("That is not a zip file Setup can read (" + e.Message + "). Nothing was added."); }
            if (entries.Count > MaxFiles) throw new ModsException("The archive holds " + entries.Count + " files, more than the " + MaxFiles + " a mod may have. Nothing was added.");
            long total = 0;
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var e in entries)
            {
                if (Unsafe(e.FullName, true) is { } why) throw new ModsException("The archive holds " + why + ": \"" + Short(e.FullName) + "\". Nothing was unpacked.");
                // (how the zip marks what an entry is: Unix's file type in the high half of its attributes, Windows' in the low)
                var type = (e.ExternalAttributes >> 16) & 0xF000;
                if (type == 0xA000 || (e.ExternalAttributes & 0x400) != 0) throw new ModsException("The archive holds a link (\"" + Short(e.FullName) + "\"), which could point anywhere on this PC. Nothing was unpacked.");
                if (type != 0 && type != 0x8000 && type != 0x4000) throw new ModsException("The archive holds something that is neither a file nor a folder (\"" + Short(e.FullName) + "\"). Nothing was unpacked.");
                var plain = string.Join("/", e.FullName.Replace('\\', '/').Split('/').Where(p => p.Length > 0 && p != "."));
                if (plain.Length == 0) continue;
                if (!seen.Add(plain)) throw new ModsException("The archive holds two files of one name (\"" + Short(e.FullName) + "\"). Nothing was unpacked.");
                if (e.Length < 0 || e.Length > MaxBytes || (total += e.Length) > MaxBytes) throw new ModsException(TooBig());
            }
            if (Space.Problem("this mod", total + entries.Count * 4096L, Space.Free(mods), Space.Disk(mods)) is { } none) throw new ModsException(none);
            MakeFolder(mods, into);
            var root = Full(into) + "\\";
            long written = 0;
            var buffer = new byte[1 << 16];
            for (var i = 0; i < entries.Count; i++)
            {
                var e = entries[i];
                var name = e.FullName.Replace('\\', '/');
                var dest = System.IO.Path.GetFullPath(System.IO.Path.Combine(into, name.Replace('/', '\\')));
                if (!(dest + "\\").StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new ModsException("The archive holds a path that leaves its own folder: \"" + Short(e.FullName) + "\".");
                Within(mods, dest);
                if (name.EndsWith("/")) { Directory.CreateDirectory(dest); continue; }
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(dest)!);
                long size = 0;
                using (var from = e.Open())
                using (var to = new FileStream(dest, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    for (int n; (n = from.Read(buffer, 0, buffer.Length)) > 0;)
                    {
                        if ((size += n) > e.Length || (written += n) > MaxBytes) throw new ModsException("The archive unpacks to more than it says (\"" + Short(e.FullName) + "\"). Nothing was added.");
                        to.Write(buffer, 0, n);
                    }
                try { File.SetLastWriteTime(dest, e.LastWriteTime.DateTime); } catch (Exception) { }
                if ((i & 31) == 0) progress?.Invoke(0.95 * i / entries.Count);
            }
        }
    }

    static string TooBig() => "The archive unpacks to more than " + (MaxBytes >> 30) + " GB, the most a mod may take. Nothing was added.";

    static string ByHand(string kind) => " Unpack the " + kind + " file yourself and put the mod's folder (the one that holds romfs) into the mods folder.";

    /// A .7z or .rar, with Windows' own tar.exe: nothing that reads them is in Setup or in the repository. Its list is asked
    /// for first, twice: the names (checked as a zip's are, as far as tar's spelling of them can be trusted) and the long form,
    /// whose first letter says what each entry is (a link is refused) and whose fifth word is its size (counted against the
    /// bound and the room on the disk). tar then unpacks into the folder, without -P: it refuses paths that leave it. What
    /// lies there afterwards is checked again, as files (Check)
    static void UnpackWithTar(string mods, string archive, string into, string kind)
    {
        var tar = Tar();
        if (tar == null)
            throw new ModsException("This PC has no tar.exe in its System32 folder (Windows 10 and 11 bring one), and Setup opens " + kind + " files with nothing else." + ByHand(kind));
        MakeFolder(mods, into);
        var (code, names, error) = Run(tar, "-tf \"" + archive + "\"", into, MaxFiles + 1);
        if (code != 0) throw new ModsException("This PC's tar.exe could not read the " + kind + " file (" + Reason(error, code) + ")." + ByHand(kind));
        if (names.Count > MaxFiles) throw new ModsException("The archive holds more than the " + MaxFiles + " files a mod may have. Nothing was added.");
        foreach (var n in names)
            if (Unsafe(n, false) is { } why) throw new ModsException("The archive holds " + why + ": \"" + Short(n) + "\". Nothing was unpacked.");
        var (code2, lines, error2) = Run(tar, "-tvf \"" + archive + "\"", into, MaxFiles + 1);
        if (code2 != 0 || lines.Count != names.Count) throw new ModsException("This PC's tar.exe could not list the " + kind + " file (" + Reason(error2, code2) + ")." + ByHand(kind));
        long total = 0;
        for (var i = 0; i < lines.Count; i++)
        {
            var words = lines[i].Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (words.Length < 6 || words[0].Length < 10 || !long.TryParse(words[4], out var size) || size < 0)
                throw new ModsException("This PC's tar.exe lists the " + kind + " file in a way Setup cannot read, so it was not unpacked." + ByHand(kind));
            if (words[0][0] == 'l' || words[0][0] == 'h') throw new ModsException("The archive holds a link (\"" + Short(names[i]) + "\"), which could point anywhere on this PC. Nothing was unpacked.");
            if (words[0][0] != '-' && words[0][0] != 'd') throw new ModsException("The archive holds something that is neither a file nor a folder (\"" + Short(names[i]) + "\"). Nothing was unpacked.");
            if (size > MaxBytes || (total += size) > MaxBytes) throw new ModsException(TooBig());
        }
        if (Space.Problem("this mod", total + names.Count * 4096L, Space.Free(mods), Space.Disk(mods)) is { } none) throw new ModsException(none);
        Within(mods, into);
        var (code3, _, error3) = Run(tar, "-xf \"" + archive + "\" -C \"" + into + "\"", into, 0);
        if (code3 != 0) throw new ModsException("This PC's tar.exe could not unpack the " + kind + " file (" + Reason(error3, code3) + "). Nothing was added." + ByHand(kind));
    }

    static string Reason(string error, int code)
    {
        var line = error.Replace("\r", "").Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Length > 0);
        return line != null ? Short(line) : "exit code " + code;
    }

    /// tar with these words, its lines (at most so many: one more and it is stopped) and what it said of errors
    static (int Code, List<string> Lines, string Error) Run(string exe, string arguments, string dir, int most)
    {
        var si = new ProcessStartInfo(exe, arguments)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = dir,
        };
        using var p = Process.Start(si)!;
        var error = p.StandardError.ReadToEndAsync();
        var lines = new List<string>();
        for (string? line; (line = p.StandardOutput.ReadLine()) != null;)
        {
            if (line.Length == 0) continue;
            lines.Add(line);
            if (lines.Count > most && most > 0)
            {
                try { p.Kill(); } catch (Exception) { }
                break;
            }
        }
        p.WaitForExit();
        string said;
        try { said = error.Result; } catch (Exception) { said = ""; }
        return (lines.Count > most && most > 0 ? 0 : p.ExitCode, lines, said);
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

    /// Has the file a second name somewhere (a hard link)? Writing to one of them writes to the other
    static bool HasOtherName(string file)
    {
        try
        {
            using var f = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            return GetFileInformationByHandle(f.SafeFileHandle, out var information) && information.Links > 1;
        }
        catch (Exception) { return false; }
    }

    /// What an archive unpacked to, looked at as it lies on the disk before any of it moves into the mods folder: no link of
    /// any kind, nothing under a device's name, not more files or bytes than a mod may have
    static void Check(string into, bool links)
    {
        var files = 0;
        long bytes = 0;
        void Look(string dir, int depth)
        {
            if (depth > MaxDepth) throw new ModsException("The archive's folders go more than " + MaxDepth + " deep. Nothing was added.");
            foreach (var e in new DirectoryInfo(dir).GetFileSystemInfos())
            {
                var shown = Short(e.FullName.Substring(Full(into).Length + 1));
                if ((e.Attributes & FileAttributes.ReparsePoint) != 0) throw new ModsException("The archive holds a link (\"" + shown + "\"), which could point anywhere on this PC. Nothing was added.");
                if (IsDevice(e.Name)) throw new ModsException("The archive holds a name Windows keeps for a device (\"" + shown + "\"). Nothing was added.");
                if ((e.Attributes & FileAttributes.Directory) != 0) { Look(e.FullName, depth + 1); continue; }
                if (links && HasOtherName(e.FullName)) throw new ModsException("The archive holds a link (\"" + shown + "\"), which could point anywhere on this PC. Nothing was added.");
                if (++files > MaxFiles) throw new ModsException("The archive holds more than the " + MaxFiles + " files a mod may have. Nothing was added.");
                if ((bytes += ((FileInfo)e).Length) > MaxBytes) throw new ModsException(TooBig());
            }
        }
        Look(into, 0);
    }

    /// The folder of what was unpacked that is the mod: the one that holds romfs, exefs or cheats, looked for four folders
    /// down as the game looks. None, several (a download with versions to choose from) or one for another game are said so
    static string FindRoot(string into)
    {
        var found = new List<string>();
        void Look(string dir, int depth)
        {
            if (Parts.Any(p => Directory.Exists(System.IO.Path.Combine(dir, p)))) { found.Add(dir); return; }
            if (depth >= 4) return;
            foreach (var sub in Folders(dir)) Look(sub, depth + 1);
        }
        Look(into, 0);
        if (found.Count == 0)
            throw new ModsException("The archive holds no romfs, exefs or cheats folder, so Setup cannot tell where the mod's files belong. Nothing was added. " +
                                    "(A mod's files go into mods\\<name>\\romfs, laid out as the game's own files are.)");
        if (found.Count > 1)
        {
            var names = found.Select(f => System.IO.Path.GetFileName(f)).Take(4).ToList();
            throw new ModsException("The archive holds " + found.Count + " mods (" + string.Join(", ", names) + (found.Count > 4 ? ", ..." : "") + "), most likely versions to choose from. Nothing was added. " +
                                    "Unpack it yourself and put the one you want into the mods folder.");
        }
        // (an emulator's layout, ...\contents\<title id>\romfs: made for the game of that number)
        var title = System.IO.Path.GetFileName(found[0]);
        if (!Same(found[0], into) && Regex.IsMatch(title, "^[0-9A-Fa-f]{16}$") && !Same(title, TitleId))
            throw new ModsException("The archive holds a mod for another game (title " + title.ToUpperInvariant() + "), not for this one (" + TitleId + "). Nothing was added.");
        return found[0];
    }

    /// A folder's name in an archive that says nothing about the mod: an emulator's layout, or the part itself
    static bool Generic(string name) =>
        Regex.IsMatch(name, "^[0-9A-Fa-f]{16}$") || new[] { "contents", "atmosphere", "mods", "mod" }.Any(g => Same(g, name));

    /// A name for the mod's folder from an archive's or a folder's name: what Windows allows, and nothing in front that would
    /// switch the mod off or hide it from the game
    public static string Clean(string name)
    {
        var s = new string(name.Where(c => c >= 32 && "<>:\"/\\|?*".IndexOf(c) < 0).ToArray()).Trim();
        s = s.TrimStart('-', '_', '.', ' ').TrimEnd('.', ' ');
        if (s.Length > 80) s = s.Substring(0, 80).TrimEnd('.', ' ');
        return s.Length == 0 || IsDevice(s) ? "Mod" : s;
    }
}
