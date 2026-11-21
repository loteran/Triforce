// (Triforce) The mods of a game folder as they lie on the disk (old setup/Mods.cs, Shell.Mods.cs). The rules are the game's
// (host/mods.c). Nothing here changes them or keeps a list of its own, so a player who arranges the folders by hand and one who
// uses the launcher see the same thing:
//   <game>\mods\<name>\romfs\...     a mod is a folder, up to four levels down, that holds romfs, exefs or cheats
//   a name that starts with - or _    switches the folder off, and every mod in it (a name that starts with a dot is not looked at)
//   the name that sorts last wins     among the mods that carry the same file (compared without case, as the game does)
// On and off is a rename: with a "-" in front, or without. The order is a number in front of the name ("01-", "02-"), kept apart
// from the mod's own name, so "-02-Tall grass" is the mod Tall grass, second, switched off. RomFS files and cheats are read when the
// game starts; code patches (exefs) are built into the game, so a change of those waits for the next build (CodeState).
//
// Every change is a rename or a new folder inside the mods folder and nowhere else (Within), and none is made while the game runs
// (GameRuns). A mod that is removed goes to mods\.removed, which the game does not look at: nothing is ever deleted but what an
// archive's own unpacking left in its temporary folder.
using System.Diagnostics;
using System.IO.Compression;
using System.Text;
using System.Text.RegularExpressions;

namespace Triforce.Logic;

/// <summary>What the Mods page tells the player when something cannot be done: a whole sentence, in plain words.</summary>
public sealed class ModsException : Exception
{
    public ModsException(string message) : base(message) { }
}

/// <summary>One mod as the game would find it.</summary>
public sealed class Mod
{
    public string Path { get; set; } = "";     // (its folder, in full)
    public string Rel { get; set; } = "";      // (its place under the mods folder, "/" between folders: what the game sorts by)
    public string Top { get; set; } = "";      // (the folder of the mods folder it is, or is in)
    public string Folder { get; set; } = "";   // (its folder's name as it is on the disk)
    public string Name { get; set; } = "";     // (its own name: the folder's without the switch and the number)
    public int? Number { get; set; }           // (the number its folder's name starts with, "01-", when it has one)
    public bool On { get; set; }
    public string? OffAbove { get; set; }      // (the folder above it whose name switches it off, when it is not its own name that does)
    public List<string> Files { get; } = new();   // (its RomFS files as the game looks them up: lower case, "/" between folders)
    public long Bytes { get; set; }
    public int Patches { get; set; }            // (the files in its exefs)
    public int Cheats { get; set; }             // (the files in its cheats folder)
    public bool RomfsIsLink { get; set; }       // (its romfs folder is a link, which the game leaves out)
    /// <summary>What GameBanana said of it (triforce_mod.json beside it), when it came from there.</summary>
    public ModInfo? Info { get; set; }

    public bool Deep => Rel.IndexOf('/') >= 0;
    public bool Code => Patches > 0 || Cheats > 0;
    /// <summary>It carries code patches: what is built into the game (its cheats are read when the game starts: host/mod_cheats.c).</summary>
    public bool Built => Patches > 0;
    public override string ToString() => Rel;
}

/// <summary>triforce_mod.json (written by the game's Mods menu and the launcher's GameBanana install): name, author, page, id.</summary>
public sealed record ModInfo(string? Name, string? Author, string? Page, long GameBananaId, bool Wip, string? File, long FileDate);

/// <summary>A file more than one of the mods that are on carry: the last of them, in the order the game loads them, wins.</summary>
public sealed class Conflict
{
    public string File { get; set; } = "";
    public List<Mod> Mods { get; } = new();
    public Mod Winner => Mods[Mods.Count - 1];
}

/// <summary>The rules that need no folder: names, order, the safety of an archive's names, the build mark.</summary>
public static class Mods
{
    /// <summary>The game this is built for: a mod in an emulator's layout names the game it is made for by this number.</summary>
    public const string TitleId = "0400486024DEC000";
    public const string BuildAgain = "Build again to apply";
    /// <summary>Where removed mods go, and where an archive is unpacked and checked before its mod moves in. Both names start with a
    /// dot: the game does not look at such folders (host/mods.c find_mods).</summary>
    public const string Removed = ".removed", Adding = ".adding-", Moving = ".moving-";
    internal static readonly string[] Parts = { "romfs", "exefs", "cheats" };
    /// <summary>How deep a path in an archive may go (the game follows a RomFS 32 folders down).</summary>
    public const int MaxDepth = 40;

    public static string FolderOf(string game) => Path.Combine(game, "mods");

    /// <summary>A folder's name taken apart: switched off (it starts with - or _), its number ("01-": two or three digits and a
    /// hyphen, so that "4K-grass" and "2024-pack" keep their names whole) and the mod's own name.</summary>
    public static (bool Off, int? Number, string Name) Parse(string folder)
    {
        var off = folder.Length > 0 && (folder[0] == '-' || folder[0] == '_');
        var rest = folder.TrimStart('-', '_');
        var m = Regex.Match(rest, @"^(\d{2,3})-(.+)$", RegexOptions.Singleline);
        return m.Success ? (off, int.Parse(m.Groups[1].Value), m.Groups[2].Value) : (off, null, rest);
    }

    /// <summary>As the game compares names (host/mods.c mod_cmp, _stricmp): byte by byte, the letters A to Z without case.</summary>
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

    /// <summary>A RomFS path as the game looks it up (host/mods.c lower_slash): "/" between folders, A to Z in lower case.</summary>
    internal static string Key(string rel)
    {
        var s = new StringBuilder(rel.Length);
        foreach (var c in rel) s.Append(c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? (char)(c + 32) : c);
        return s.ToString();
    }

    /// <summary>A mod's place in the list: its path as the game sorts it, read without the switches.</summary>
    internal static string Place(string rel) => string.Join("/", rel.Split('/').Select(p => p.TrimStart('-', '_')));

    /// <summary>What a mod carries, in words: "1,204 game files, 2 code patch files, 1 cheat file".</summary>
    public static string Carries(Mod m)
    {
        string Some(int n, string one, string many) => n.ToString("N0", System.Globalization.CultureInfo.InvariantCulture) + " " + (n == 1 ? one : many);
        var parts = new List<string>();
        if (m.Files.Count > 0 || !m.Code) parts.Add(m.RomfsIsLink ? "a romfs folder that is a link (the game leaves it out)" : Some(m.Files.Count, "game file", "game files"));
        if (m.Patches > 0) parts.Add(Some(m.Patches, "code patch file", "code patch files"));
        if (m.Cheats > 0) parts.Add(Some(m.Cheats, "cheat file", "cheat files"));
        return string.Join(", ", parts);
    }

    /// <summary>The files more than one of the mods that are on carry, each with its mods in the order the game loads them.</summary>
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

    /// <summary>For one mod: each other mod it shares files with, how many, and whether this one wins them (it loads later).</summary>
    public static List<(Mod Other, int Files, bool Wins)> Shared(Mod mod, IList<Mod> all, IEnumerable<Conflict> conflicts)
    {
        var count = new Dictionary<Mod, int>();
        foreach (var c in conflicts.Where(c => c.Mods.Contains(mod)))
            foreach (var other in c.Mods.Where(o => o != mod))
                count[other] = count.TryGetValue(other, out var n) ? n + 1 : 1;
        return count.OrderBy(p => all.IndexOf(p.Key)).Select(p => (p.Key, p.Value, all.IndexOf(mod) > all.IndexOf(p.Key))).ToList();
    }

    /// <summary>Every conflict as text, for the clipboard: the file, the mods that carry it, and which one the game takes.</summary>
    public static string ConflictText(IEnumerable<Conflict> conflicts)
    {
        var s = new StringBuilder();
        foreach (var c in conflicts)
            s.AppendLine(c.File + "  <-  " + c.Winner.Rel + " wins over " + string.Join(", ", c.Mods.Take(c.Mods.Count - 1).Select(m => m.Rel)));
        return s.ToString();
    }

    /// <summary>The code patches of the mods that are on, as one short mark: it changes when such a mod is added, removed, switched
    /// or edited, or when their order changes, and stays when only RomFS mods or cheats change or the folders are numbered anew.
    /// "" when no mod that is on carries any. FNV-1a 64 over the UTF-16 units of the lines joined by \n, as 16 hex digits.</summary>
    public static string CodeState(IEnumerable<Mod> mods)
    {
        var lines = new List<string>();
        foreach (var m in mods.Where(m => m.On && m.Built))
        {
            lines.Add("mod " + m.Name);
            foreach (var part in new[] { "exefs" })   // (not "cheats": the game reads those itself, at its start)
            {
                var dir = Path.Combine(m.Path, part);
                string[] files = { };
                try { if (Directory.Exists(dir) && !IsLink(dir)) files = Directory.GetFiles(dir, "*", SearchOption.AllDirectories); } catch (Exception) { }
                foreach (var f in files.OrderBy(f => f, StringComparer.OrdinalIgnoreCase))
                {
                    var info = new FileInfo(f);
                    // (the relative path as the old Setup wrote it, with \ on Windows: a mark kept by an old Setup still matches)
                    lines.Add(part + f.Substring(dir.Length).ToLowerInvariant() + "|" + info.Length + "|" + info.LastWriteTimeUtc.Ticks);
                }
            }
        }
        if (lines.Count == 0) return "";
        return InstanceLock.Fnv1a64(string.Join("\n", lines)).ToString("x16");
    }

    /// <summary>Is the game to be built again for its mods? built is the mark kept at the last build: null when none (then any code
    /// a mod that is on carries is news), and anything that is no mark ("changed") never matches.</summary>
    public static bool NeedsBuild(string state, string? built) => built == null ? state.Length > 0 : state != built;

    /// <summary>The folders of the mods folder that are mods or hold them, in the order the game loads them.</summary>
    public static List<string> Tops(IEnumerable<Mod> mods) => mods.Select(m => m.Top).Distinct(StringComparer.OrdinalIgnoreCase).ToList();

    internal static bool IsLink(string path)
    {
        try { return (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0; } catch (Exception) { return false; }
    }

    static readonly HashSet<string> Devices = new(StringComparer.OrdinalIgnoreCase) { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };

    /// <summary>Is this a name Windows keeps for a device, with or without an extension (NUL, con.txt)? Kept on Linux too: mods and
    /// saves move between systems.</summary>
    public static bool IsDevice(string name)
    {
        var dot = name.IndexOf('.');
        var stem = (dot >= 0 ? name.Substring(0, dot) : name).TrimEnd(' ');
        // (COM1 to COM9 and LPT1 to LPT9; Windows takes the raised digits 1, 2 and 3 for digits there too)
        if (stem.Length == 4 && (stem.StartsWith("COM", StringComparison.OrdinalIgnoreCase) || stem.StartsWith("LPT", StringComparison.OrdinalIgnoreCase)))
            return stem[3] >= '1' && stem[3] <= '9' || stem[3] == (char)0xB9 || stem[3] == (char)0xB2 || stem[3] == (char)0xB3;
        return Devices.Contains(stem);
    }

    /// <summary>Why a file of an archive must not be unpacked under its name, null when it may be: a path that leaves the folder
    /// it is unpacked into (from the disk's root, with a drive's letter, through ..), a device's name, a colon (a hidden stream of
    /// another file) and, where the name is known exactly (exact: a zip's own list), what Windows would drop or refuse.</summary>
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

    /// <summary>A name for the mod's folder from an archive's or a folder's name: what Windows allows, and nothing in front that
    /// would switch the mod off or hide it from the game.</summary>
    public static string Clean(string name)
    {
        var s = new string(name.Where(c => c >= 32 && "<>:\"/\\|?*".IndexOf(c) < 0).ToArray()).Trim();
        s = s.TrimStart('-', '_', '.', ' ').TrimEnd('.', ' ');
        if (s.Length > 80) s = s.Substring(0, 80).TrimEnd('.', ' ');
        return s.Length == 0 || IsDevice(s) ? "Mod" : s;
    }

    /// <summary>A folder's name in an archive that says nothing about the mod: an emulator's layout, or the part itself.</summary>
    internal static bool Generic(string name) =>
        Regex.IsMatch(name, "^[0-9A-Fa-f]{16}$") || new[] { "contents", "atmosphere", "mods", "mod" }.Any(g => string.Equals(g, name, StringComparison.OrdinalIgnoreCase));

    internal static string Short(string name) => name.Length > 120 ? name.Substring(0, 117) + "..." : name;
}
