using System.Diagnostics;
using System.IO.Compression;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Triforce.Logic;

/// <summary>A game folder's mods folder: list, on/off, order, remove, add from an archive, and the "Build again to apply" mark
/// (old Mods.cs and Shell.Mods.cs, with their hooks as instance settings rather than statics).</summary>
public sealed class ModManager
{
    readonly TriforceContext ctx;
    public ModManager(TriforceContext ctx)
    {
        this.ctx = ctx;
        GameRuns = folder => new Game(ctx).IsRunning(folder);
        Tar = DefaultTar;
    }

    /// <summary>The most an archive may unpack to, and the most files it may hold (the game's own RomFS is 12 GB in 18,000 files).</summary>
    public long MaxBytes { get; set; } = 32L << 30;
    public int MaxFiles { get; set; } = 200000;
    /// <summary>Does the game of this game folder run? Asked before every change (a test puts its own answer here).</summary>
    public Func<string, bool> GameRuns { get; set; }
    /// <summary>The tar that reads .7z and .rar: Windows' own System32\tar.exe (bsdtar), never whatever "tar" the PATH names; Linux
    /// bsdtar (libarchive) when installed: GNU tar reads neither. Null: none.</summary>
    public Func<string?> Tar { get; set; }

    static string? DefaultTar()
    {
        if (OperatingSystem.IsWindows())
        {
            var tar = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe");
            return File.Exists(tar) ? tar : null;
        }
        foreach (var p in new[] { "/usr/bin/bsdtar", "/bin/bsdtar", "/usr/local/bin/bsdtar" })
            if (File.Exists(p)) return p;
        return null;
    }

    static StringComparison PathCase => OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
    static bool Same(string a, string b) => string.Equals(a, b, PathCase);
    static string Hex8() => Guid.NewGuid().ToString("N").Substring(0, 8);
    static string Full(string path) => Path.GetFullPath(path).TrimEnd('\\', '/');
    static char Sep => Path.DirectorySeparatorChar;

    // ---- only inside the mods folder ----

    /// <summary>Is the path the mods folder or inside it? Every folder made, renamed or written to is asked this first.</summary>
    public static bool Inside(string mods, string path) => TriforceContext.Inside(mods, path);

    void Within(string mods, string path)
    {
        if (!Inside(mods, path)) throw new InvalidOperationException("The launcher would have written outside the mods folder (" + path + "), and did not.");
        ctx.Require(path);
    }

    void MakeFolder(string mods, string dir)
    {
        Within(mods, dir);
        Directory.CreateDirectory(dir);
    }

    void MoveFolder(string mods, string from, string to)
    {
        Within(mods, from);
        Within(mods, to);
        Directory.Move(from, to);
    }

    void Refuse(string mods)
    {
        if (GameRuns(Path.GetDirectoryName(Full(mods)) ?? mods))
            throw new ModsException("Close the game first. Mods are added, switched, ordered and removed only while it is not running.");
    }

    static string[] Folders(string dir)
    {
        try { return Directory.GetDirectories(dir); } catch (Exception) { return Array.Empty<string>(); }
    }

    // ---- the list ----

    /// <summary>The mods of a mods folder as the game finds them (host/mods.c find_mods), those that are off too, in the order the
    /// game loads them: the last one wins a file two of them carry. carried: false leaves out what each mod carries.</summary>
    public List<Mod> List(string mods, bool carried = true)
    {
        var found = new List<Mod>();
        if (Directory.Exists(mods)) Walk(mods, "", 0, null, found, carried);
        // (the game's order among the mods that are on; one that is off keeps the place it would have without its switch)
        found.Sort((a, b) =>
        {
            var c = Mods.Compare(Mods.Place(a.Rel), Mods.Place(b.Rel));
            return c != 0 ? c : string.CompareOrdinal(a.Rel, b.Rel);
        });
        return found;
    }

    /// <summary>The mods of a game folder.</summary>
    public List<Mod> ListGame(string game, bool carried = true) => List(Mods.FolderOf(game), carried);

    static void Walk(string dir, string rel, int depth, string? offAbove, List<Mod> found, bool carried)
    {
        var folder = depth > 0 ? Path.GetFileName(dir) : "";
        var (isOff, number, name) = Mods.Parse(folder);
        // (romfs and exefs make a folder a mod for the game, whatever they are; a cheats folder for the build)
        bool Has(string part) => Directory.Exists(Path.Combine(dir, part)) || part != "cheats" && File.Exists(Path.Combine(dir, part));
        if (depth > 0 && Mods.Parts.Any(Has))
        {
            var m = new Mod
            {
                Path = dir, Rel = rel, Top = rel.Split('/')[0], Folder = folder, Name = name.Length > 0 ? name : folder, Number = number,
                On = offAbove == null && !isOff, OffAbove = offAbove, Info = ReadInfo(dir),
            };
            if (carried) Fill(m);
            found.Add(m);
            return;
        }
        if (depth >= 4) return;
        foreach (var sub in Folders(dir))
        {
            var child = Path.GetFileName(sub);
            if (child.Length == 0 || child[0] == '.') continue;
            Walk(sub, rel.Length > 0 ? rel + "/" + child : child, depth + 1, offAbove ?? (isOff ? folder : null), found, carried);
        }
    }

    /// <summary>triforce_mod.json beside a mod, when it came from GameBanana (the old Setup did not read it: vk_mods.h's mismatch).</summary>
    public static ModInfo? ReadInfo(string dir)
    {
        try
        {
            var file = Path.Combine(dir, "triforce_mod.json");
            if (!File.Exists(file)) return null;
            using var doc = JsonDocument.Parse(File.ReadAllText(file));
            var r = doc.RootElement;
            string? S(string n) => r.TryGetProperty(n, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
            long N(string n) => r.TryGetProperty(n, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out var x) ? x : 0;
            return new ModInfo(S("name"), S("author"), S("page"), N("gamebanana_id"), S("gamebanana_model") == "Wip", S("file"), N("file_date"));
        }
        catch (Exception) { return null; }
    }

    /// <summary>The file the game reads for one of its RomFS paths ("Sound/Resource/x.bwav"): that of the last mod, among those that
    /// are on, that carries it (a link to a folder not followed, the romfs folder itself neither when it is one), else the game's
    /// own under &lt;game&gt;\romfs. Null when neither has it. (For the game's own sounds and music the App plays.)</summary>
    public string? RomfsFile(string game, string rel)
    {
        var parts = rel.Replace('\\', '/').Split('/').Where(p => p.Length > 0).ToArray();
        if (parts.Length == 0 || parts.Any(p => p == "." || p == ".." || p.IndexOf(':') >= 0)) return null;
        string? In(string romfs)
        {
            var at = romfs;
            for (var i = 0; ; i++)
            {
                if (!Directory.Exists(at) || Mods.IsLink(at)) return null;
                at = Path.Combine(at, parts[i]);
                if (i == parts.Length - 1) return File.Exists(at) ? at : null;
            }
        }
        try
        {
            foreach (var m in Enumerable.Reverse(List(Mods.FolderOf(game), false)).Where(m => m.On))
                if (In(Path.Combine(m.Path, "romfs")) is { } theirs) return theirs;
            var own = Path.Combine(new[] { game, "romfs" }.Concat(parts).ToArray());
            return File.Exists(own) ? own : null;
        }
        catch (Exception) { return null; }
    }

    /// <summary>What a mod carries: its RomFS files as the game indexes them (32 folders deep, a link to a folder left out, the
    /// romfs folder itself too when it is one), and how many files its exefs and cheats folders hold.</summary>
    static void Fill(Mod m)
    {
        var romfs = Path.Combine(m.Path, "romfs");
        if (Directory.Exists(romfs))
        {
            if (Mods.IsLink(romfs)) m.RomfsIsLink = true;
            else Index(m, romfs, "", 0);
        }
        m.Files.Sort(string.CompareOrdinal);
        m.Patches = Count(Path.Combine(m.Path, "exefs"));
        m.Cheats = Count(Path.Combine(m.Path, "cheats"));
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
                m.Files.Add(Mods.Key(path));
                try { m.Bytes += ((FileInfo)e).Length; } catch (Exception) { }
            }
        }
    }

    static int Count(string dir)
    {
        try { return Directory.Exists(dir) && !Mods.IsLink(dir) ? Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Length : 0; }
        catch (Exception) { return 0; }
    }

    // ---- what the game must be built again for ----

    /// <summary>The mark of the mods' code the game folder was last built with (ModsBuilt "&lt;game&gt;|&lt;mark&gt;"), null when none is kept for it.</summary>
    public string? BuiltMark(string game) =>
        ctx.Settings.ModsBuilt is { } kept && kept.StartsWith(game + "|", StringComparison.OrdinalIgnoreCase) ? kept.Substring(game.Length + 1) : null;

    /// <summary>A build of this game folder ended well: its mods' code patches are in it as they are now.</summary>
    public void MarkBuilt(string game) => ctx.Settings.ModsBuilt = game + "|" + Mods.CodeState(ListGame(game));

    /// <summary>A mod with code patches changed before any build was marked: built in or not, the game is not as its mods are.</summary>
    public void MarkChanged(string game)
    {
        if (BuiltMark(game) == null) ctx.Settings.ModsBuilt = game + "|changed";
    }

    /// <summary>"Build again to apply" (old Shell.Mods.cs ModsRefresh): the code patches of the mods that are on are not the ones of the
    /// last build. Then BuildRunner with BuildRequest.Update(game) (refused while the game runs).</summary>
    public bool NeedsRebuild(string game) => Mods.NeedsBuild(Mods.CodeState(ListGame(game)), BuiltMark(game));

    // ---- on and off ----

    /// <summary>Switches a mod on or off by its folder's name: "-" in front of it, or no - and _ there. Returns its folder now.</summary>
    public string SetOn(string mods, Mod mod, bool on)
    {
        Refuse(mods);
        if (!Directory.Exists(mod.Path)) throw new ModsException(mod.Name + " is no longer in the mods folder.");
        var (off, _, _) = Mods.Parse(mod.Folder);
        if (on && mod.OffAbove != null)
            throw new ModsException(mod.Name + " is inside the folder " + mod.OffAbove + ", whose name switches off everything in it. Take the - or _ off that folder's name.");
        if (on == !off) return mod.Path;
        var name = on ? mod.Folder.TrimStart('-', '_') : "-" + mod.Folder;
        if (name.Length == 0 || name[0] == '.') throw new ModsException("Without the " + mod.Folder[0] + " in front, the folder " + mod.Folder + " has no name the game looks at. Rename it by hand.");
        var target = Path.Combine(Path.GetDirectoryName(mod.Path)!, name);
        if (Directory.Exists(target) || File.Exists(target))
            throw new ModsException("There is already a folder named " + name + " in the mods folder. Rename or remove one of the two first.");
        try { MoveFolder(mods, mod.Path, target); }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            throw new ModsException(mod.Name + " could not be switched " + (on ? "on" : "off") + ": " + e.Message + " Close any window that shows its folder and try again.");
        }
        if (mod.Built) MarkChanged(Path.GetDirectoryName(Full(mods))!);
        ctx.Log.Write("mods: " + mod.Name + " switched " + (on ? "on" : "off"));
        return target;
    }

    // ---- the order ----

    /// <summary>Moves a mod one place: later (it then wins over the mod it passes) or earlier. The folders of the mods folder are
    /// numbered anew, 01- 02- ..., each keeping its own name and its switch; a mod deeper down moves with the folder of the mods
    /// folder it is in. False when it is at that end already.</summary>
    public bool Move(string mods, Mod mod, bool later)
    {
        Refuse(mods);
        var tops = Mods.Tops(List(mods));
        var at = tops.FindIndex(t => Same(t, mod.Top));
        if (at < 0) throw new ModsException(mod.Name + " is no longer in the mods folder.");
        var to = at + (later ? 1 : -1);
        if (to < 0 || to >= tops.Count) return false;
        (tops[at], tops[to]) = (tops[to], tops[at]);
        Number(mods, tops);
        if (mod.Built) MarkChanged(Path.GetDirectoryName(Full(mods))!);
        return true;
    }

    /// <summary>The folders, in this order, under the numbers 01- 02- ... (three digits from a hundred folders on): all of them or,
    /// when one cannot be renamed, none.</summary>
    void Number(string mods, List<string> tops)
    {
        var digits = tops.Count >= 100 ? "000" : "00";
        var plan = new List<(string From, string To)>();
        for (var i = 0; i < tops.Count; i++)
        {
            var (_, _, name) = Mods.Parse(tops[i]);
            var switches = tops[i].Substring(0, tops[i].Length - tops[i].TrimStart('-', '_').Length);
            var to = switches + (i + 1).ToString(digits) + "-" + name;
            if (!string.Equals(tops[i], to, StringComparison.Ordinal)) plan.Add((tops[i], to));
        }
        var leaving = new HashSet<string>(plan.Select(p => p.From), StringComparer.OrdinalIgnoreCase);
        foreach (var (_, to) in plan)
        {
            var path = Path.Combine(mods, to);
            if (!leaving.Contains(to) && (Directory.Exists(path) || File.Exists(path)))
                throw new ModsException("There is already something named " + to + " in the mods folder, where a mod's folder would be renamed to. Rename or remove it first. Nothing was changed.");
        }
        // (a new name that is another folder's old one, two mods of one name changing places: each goes through a name of its own
        // first, one the game does not look at)
        var steps = new List<(string From, string To)>();
        if (plan.Any(p => leaving.Contains(p.To)))
        {
            var aside = Mods.Moving + Hex8() + "-";
            steps.AddRange(plan.Select((p, i) => (p.From, aside + i)));
            steps.AddRange(plan.Select((p, i) => (aside + i, p.To)));
        }
        else steps.AddRange(plan);
        var done = new List<(string From, string To)>();
        try
        {
            foreach (var (from, to) in steps)
            {
                MoveFolder(mods, Path.Combine(mods, from), Path.Combine(mods, to));
                done.Add((from, to));
            }
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            var back = true;
            for (var i = done.Count - 1; i >= 0; i--)
                try { MoveFolder(mods, Path.Combine(mods, done[i].To), Path.Combine(mods, done[i].From)); }
                catch (Exception) { back = false; }
            throw new ModsException("The mods could not be put in that order: " + e.Message + (back ? " Nothing was changed." : " Some folders have their new names and some their old ones.") +
                                    " Close any window that shows a mod's folder and try again.");
        }
    }

    // ---- removing ----

    /// <summary>Takes a mod out of the game's sight: its folder moves into mods\.removed (under a free name). Nothing is deleted.</summary>
    public string Remove(string mods, Mod mod)
    {
        Refuse(mods);
        if (!Directory.Exists(mod.Path)) throw new ModsException(mod.Name + " is no longer in the mods folder.");
        var bin = Path.Combine(mods, Mods.Removed);
        try
        {
            MakeFolder(mods, bin);
            var target = Free(bin, mod.Folder.TrimStart('-', '_') is { Length: > 0 } n ? n : "mod");
            MoveFolder(mods, mod.Path, target);
            if (mod.Built) MarkChanged(Path.GetDirectoryName(Full(mods))!);
            ctx.Log.Write("mods: " + mod.Name + " removed to " + target);
            return target;
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            throw new ModsException(mod.Name + " could not be removed: " + e.Message + " Close any window that shows its folder and try again.");
        }
    }

    /// <summary>dir\name, or the first of "name (2)", "name (3)" ... that is not there.</summary>
    internal static string Free(string dir, string name)
    {
        for (var k = 1; ; k++)
        {
            var path = Path.Combine(dir, k == 1 ? name : name + " (" + k + ")");
            if (!Directory.Exists(path) && !File.Exists(path)) return path;
        }
    }

    // ---- adding from an archive ----

    /// <summary>Adds a mod from a .zip, .7z or .rar file. The archive is unpacked into a folder of its own inside the mods folder
    /// (one the game does not look at), what lies there is checked, the folder that holds romfs, exefs or cheats is looked for,
    /// and only then does that folder move into the mods folder under the mod's name. Anything else ends with a sentence that says
    /// why, and with nothing added. Returns the new mod.</summary>
    public Mod Add(string mods, string archive, Action<double>? progress = null)
    {
        Refuse(mods);
        var kind = Path.GetExtension(archive).ToLowerInvariant();
        if (kind != ".zip" && kind != ".7z" && kind != ".rar") throw new ModsException("That is no .zip, .7z or .rar file.");
        if (!File.Exists(archive)) throw new ModsException("The file is not there: " + archive);
        string temp;
        try
        {
            MakeFolder(mods, mods);
            Sweep(mods);
            temp = Path.Combine(mods, Mods.Adding + Hex8());
            MakeFolder(mods, temp);
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            throw new ModsException("The mods folder could not be written to: " + e.Message);
        }
        try
        {
            var into = Path.Combine(temp, "x");
            Unpack(mods, archive, into, progress);
            var root = FindRoot(into);
            // (the mod's name: its own folder's in the archive, else the archive's)
            var given = Same(Full(root), Full(into)) || Mods.Generic(Path.GetFileName(root)) ? Path.GetFileNameWithoutExtension(archive) : Path.GetFileName(root);
            var name = Mods.Clean(given);
            // (where the folders are numbered, the new mod gets the next number: it loads last, and wins, wherever its name would sort)
            var numbers = Mods.Tops(List(mods)).Select(t => Mods.Parse(t).Number).Where(n => n != null).Select(n => n!.Value).ToList();
            if (numbers.Count > 0 && Mods.Parse(name).Number == null) name = (numbers.Max() + 1).ToString(numbers.Max() + 1 >= 100 ? "000" : "00") + "-" + name;
            var target = Free(mods, name);
            MoveFolder(mods, root, target);
            progress?.Invoke(1);
            var mod = List(mods).FirstOrDefault(m => Same(m.Path, target)) ?? throw new ModsException("The mod was unpacked into " + target + ", but the game would not find it there.");
            if (mod.Code) MarkChanged(Path.GetDirectoryName(Full(mods))!);
            ctx.Log.Write("mods: added " + mod.Rel + " from " + Path.GetFileName(archive));
            return mod;
        }
        catch (ModsException) { throw; }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is InvalidDataException || e is NotSupportedException || e is ArgumentException)
        {
            throw new ModsException(ctx.Platform.IsDiskFull(e) ? "The disk filled while the archive was unpacked. Nothing was added." : "The archive could not be unpacked: " + e.Message + " Nothing was added.");
        }
        finally { Delete(mods, temp); }
    }

    /// <summary>The archive unpacked into a folder and checked there (names, links, sizes, counts), before anything moves.</summary>
    internal void Unpack(string mods, string archive, string into, Action<double>? progress)
    {
        var kind = Path.GetExtension(archive).ToLowerInvariant();
        if (kind == ".zip") UnpackZip(mods, archive, into, progress);
        else UnpackWithTar(mods, archive, into, kind);
        Check(into, kind != ".zip");
    }

    /// <summary>What an adding that was cut off left in the mods folder a day ago or longer.</summary>
    void Sweep(string mods)
    {
        foreach (var d in Folders(mods))
            try
            {
                if (Regex.IsMatch(Path.GetFileName(d), "^" + Regex.Escape(Mods.Adding) + "[0-9a-f]{8}$") && DateTime.UtcNow - Directory.GetLastWriteTimeUtc(d) > TimeSpan.FromDays(1))
                    Delete(mods, d);
            }
            catch (Exception) { }
    }

    /// <summary>Removes the folder an archive was unpacked into, with what is left in it. A link in it is removed as a link.</summary>
    void Delete(string mods, string dir)
    {
        try
        {
            Within(mods, dir);
            if (!Same(Path.GetDirectoryName(Full(dir)) ?? "", Full(mods)) || !Path.GetFileName(Full(dir)).StartsWith(Mods.Adding, StringComparison.Ordinal)) return;
            Empty(dir);
        }
        catch (Exception) { }
    }

    internal static void Empty(string dir)
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

    /// <summary>A zip, with .NET's own reader. Its list is checked whole before the first file is written: every name, no links, no
    /// two files of one name, what it says it unpacks to against the bound and against the room on the disk. A file that turns out
    /// longer than its entry said stops the unpacking.</summary>
    void UnpackZip(string mods, string archive, string into, Action<double>? progress)
    {
        ZipArchive zip;
        try { zip = ZipFile.OpenRead(archive); }
        catch (Exception e) when (e is InvalidDataException || e is IOException || e is UnauthorizedAccessException || e is NotSupportedException)
        {
            throw new ModsException("That is not a zip file the launcher can read (" + e.Message + "). Nothing was added.");
        }
        using (zip)
        {
            System.Collections.ObjectModel.ReadOnlyCollection<ZipArchiveEntry> entries;
            try { entries = zip.Entries; }
            catch (ArgumentException) { throw new ModsException("The archive holds a name with a character Windows does not allow. Nothing was unpacked."); }
            catch (InvalidDataException e) { throw new ModsException("That is not a zip file the launcher can read (" + e.Message + "). Nothing was added."); }
            if (entries.Count > MaxFiles) throw new ModsException("The archive holds " + entries.Count + " files, more than the " + MaxFiles + " a mod may have. Nothing was added.");
            long total = 0;
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var e in entries)
            {
                if (Mods.Unsafe(e.FullName, true) is { } why) throw new ModsException("The archive holds " + why + ": \"" + Mods.Short(e.FullName) + "\". Nothing was unpacked.");
                // (how the zip marks what an entry is: Unix's file type in the high half of its attributes, Windows' in the low)
                var type = (e.ExternalAttributes >> 16) & 0xF000;
                if (type == 0xA000 || (e.ExternalAttributes & 0x400) != 0) throw new ModsException("The archive holds a link (\"" + Mods.Short(e.FullName) + "\"), which could point anywhere on this PC. Nothing was unpacked.");
                if (type != 0 && type != 0x8000 && type != 0x4000) throw new ModsException("The archive holds something that is neither a file nor a folder (\"" + Mods.Short(e.FullName) + "\"). Nothing was unpacked.");
                var plain = string.Join("/", e.FullName.Replace('\\', '/').Split('/').Where(p => p.Length > 0 && p != "."));
                if (plain.Length == 0) continue;
                if (!seen.Add(plain)) throw new ModsException("The archive holds two files of one name (\"" + Mods.Short(e.FullName) + "\"). Nothing was unpacked.");
                if (e.Length < 0 || e.Length > MaxBytes || (total += e.Length) > MaxBytes) throw new ModsException(TooBig());
            }
            if (ctx.SpaceProblem("this mod", total + entries.Count * 4096L, mods) is { } none) throw new ModsException(none);
            MakeFolder(mods, into);
            var root = Full(into) + Sep;
            long written = 0;
            var buffer = new byte[1 << 16];
            for (var i = 0; i < entries.Count; i++)
            {
                var e = entries[i];
                var name = e.FullName.Replace('\\', '/');
                var dest = Path.GetFullPath(Path.Combine(into, name.Replace('/', Sep)));
                if (!(dest + Sep).StartsWith(root, PathCase)) throw new ModsException("The archive holds a path that leaves its own folder: \"" + Mods.Short(e.FullName) + "\".");
                Within(mods, dest);
                if (name.EndsWith('/')) { Directory.CreateDirectory(dest); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                long size = 0;
                using (var from = e.Open())
                using (var to = new FileStream(dest, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    for (int n; (n = from.Read(buffer, 0, buffer.Length)) > 0;)
                    {
                        if ((size += n) > e.Length || (written += n) > MaxBytes) throw new ModsException("The archive unpacks to more than it says (\"" + Mods.Short(e.FullName) + "\"). Nothing was added.");
                        to.Write(buffer, 0, n);
                    }
                try { File.SetLastWriteTime(dest, e.LastWriteTime.DateTime); } catch (Exception) { }
                if ((i & 31) == 0) progress?.Invoke(0.95 * i / entries.Count);
            }
        }
    }

    string TooBig() => "The archive unpacks to more than " + (MaxBytes >> 30) + " GB, the most a mod may take. Nothing was added.";

    static string ByHand(string kind) => " Unpack the " + kind + " file yourself and put the mod's folder (the one that holds romfs) into the mods folder.";

    /// <summary>A .7z or .rar, with the system's bsdtar (Windows' own tar.exe). Its list is asked for first, twice: the names
    /// (checked as a zip's are) and the long form, whose first letter says what each entry is (a link is refused) and whose fifth
    /// word is its size (counted against the bound and the room on the disk). tar then unpacks into the folder, without -P: it
    /// refuses paths that leave it. What lies there afterwards is checked again, as files (Check).</summary>
    void UnpackWithTar(string mods, string archive, string into, string kind)
    {
        var tar = Tar();
        if (tar == null)
            throw new ModsException(OperatingSystem.IsWindows()
                ? "This PC has no tar.exe in its System32 folder (Windows 10 and 11 bring one), and the launcher opens " + kind + " files with nothing else." + ByHand(kind)
                : "This PC has no bsdtar (libarchive's tar; GNU tar does not read " + kind + " files)." + ByHand(kind));
        MakeFolder(mods, into);
        var (code, names, error) = Run(tar, new[] { "-tf", archive }, into, MaxFiles + 1);
        if (code != 0) throw new ModsException("This PC's tar could not read the " + kind + " file (" + Reason(error, code) + ")." + ByHand(kind));
        if (names.Count > MaxFiles) throw new ModsException("The archive holds more than the " + MaxFiles + " files a mod may have. Nothing was added.");
        foreach (var n in names)
            if (Mods.Unsafe(n, false) is { } why) throw new ModsException("The archive holds " + why + ": \"" + Mods.Short(n) + "\". Nothing was unpacked.");
        var (code2, lines, error2) = Run(tar, new[] { "-tvf", archive }, into, MaxFiles + 1);
        if (code2 != 0 || lines.Count != names.Count) throw new ModsException("This PC's tar could not list the " + kind + " file (" + Reason(error2, code2) + ")." + ByHand(kind));
        long total = 0;
        for (var i = 0; i < lines.Count; i++)
        {
            var words = lines[i].Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (words.Length < 6 || words[0].Length < 10 || !long.TryParse(words[4], out var size) || size < 0)
                throw new ModsException("This PC's tar lists the " + kind + " file in a way the launcher cannot read, so it was not unpacked." + ByHand(kind));
            if (words[0][0] == 'l' || words[0][0] == 'h') throw new ModsException("The archive holds a link (\"" + Mods.Short(names[i]) + "\"), which could point anywhere on this PC. Nothing was unpacked.");
            if (words[0][0] != '-' && words[0][0] != 'd') throw new ModsException("The archive holds something that is neither a file nor a folder (\"" + Mods.Short(names[i]) + "\"). Nothing was unpacked.");
            if (size > MaxBytes || (total += size) > MaxBytes) throw new ModsException(TooBig());
        }
        if (ctx.SpaceProblem("this mod", total + names.Count * 4096L, mods) is { } none) throw new ModsException(none);
        Within(mods, into);
        var (code3, _, error3) = Run(tar, new[] { "-xf", archive, "-C", into }, into, 0);
        if (code3 != 0) throw new ModsException("This PC's tar could not unpack the " + kind + " file (" + Reason(error3, code3) + "). Nothing was added." + ByHand(kind));
    }

    static string Reason(string error, int code)
    {
        var line = error.Replace("\r", "").Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Length > 0);
        return line != null ? Mods.Short(line) : "exit code " + code;
    }

    /// <summary>tar with these words, its lines (at most so many: one more and it is stopped) and what it said of errors.</summary>
    static (int Code, List<string> Lines, string Error) Run(string exe, string[] arguments, string dir, int most)
    {
        var si = new ProcessStartInfo(exe)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = dir,
        };
        foreach (var a in arguments) si.ArgumentList.Add(a);
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

    /// <summary>What an archive unpacked to, looked at as it lies on the disk before any of it moves into the mods folder: no link of
    /// any kind, nothing under a device's name, not more files or bytes than a mod may have.</summary>
    void Check(string into, bool links)
    {
        var files = 0;
        long bytes = 0;
        void Look(string dir, int depth)
        {
            if (depth > Mods.MaxDepth) throw new ModsException("The archive's folders go more than " + Mods.MaxDepth + " deep. Nothing was added.");
            foreach (var e in new DirectoryInfo(dir).GetFileSystemInfos())
            {
                var shown = Mods.Short(e.FullName.Substring(Full(into).Length + 1));
                if ((e.Attributes & FileAttributes.ReparsePoint) != 0 || e.LinkTarget != null) throw new ModsException("The archive holds a link (\"" + shown + "\"), which could point anywhere on this PC. Nothing was added.");
                if (Mods.IsDevice(e.Name)) throw new ModsException("The archive holds a name Windows keeps for a device (\"" + shown + "\"). Nothing was added.");
                if ((e.Attributes & FileAttributes.Directory) != 0) { Look(e.FullName, depth + 1); continue; }
                if (links && ctx.Platform.HasOtherName(e.FullName)) throw new ModsException("The archive holds a link (\"" + shown + "\"), which could point anywhere on this PC. Nothing was added.");
                if (++files > MaxFiles) throw new ModsException("The archive holds more than the " + MaxFiles + " files a mod may have. Nothing was added.");
                if ((bytes += ((FileInfo)e).Length) > MaxBytes) throw new ModsException(TooBig());
            }
        }
        Look(into, 0);
    }

    /// <summary>The folder of what was unpacked that is the mod: the one that holds romfs, exefs or cheats, looked for four folders
    /// down as the game looks. None, several (a download with versions to choose from) or one for another game are said so.</summary>
    static string FindRoot(string into)
    {
        var found = new List<string>();
        void Look(string dir, int depth)
        {
            if (Mods.Parts.Any(p => Directory.Exists(Path.Combine(dir, p)))) { found.Add(dir); return; }
            if (depth >= 4) return;
            foreach (var sub in Folders(dir)) Look(sub, depth + 1);
        }
        Look(into, 0);
        if (found.Count == 0)
            throw new ModsException("The archive holds no romfs, exefs or cheats folder, so the launcher cannot tell where the mod's files belong. Nothing was added. " +
                                    "(A mod's files go into mods\\<name>\\romfs, laid out as the game's own files are.)");
        if (found.Count > 1)
        {
            var names = found.Select(f => Path.GetFileName(f)).Take(4).ToList();
            throw new ModsException("The archive holds " + found.Count + " mods (" + string.Join(", ", names) + (found.Count > 4 ? ", ..." : "") + "), most likely versions to choose from. Nothing was added. " +
                                    "Unpack it yourself and put the one you want into the mods folder.");
        }
        // (an emulator's layout, ...\contents\<title id>\romfs: made for the game of that number)
        var title = Path.GetFileName(found[0]);
        if (!Same(Full(found[0]), Full(into)) && Regex.IsMatch(title, "^[0-9A-Fa-f]{16}$") && !string.Equals(title, Mods.TitleId, StringComparison.OrdinalIgnoreCase))
            throw new ModsException("The archive holds a mod for another game (title " + title.ToUpperInvariant() + "), not for this one (" + Mods.TitleId + "). Nothing was added.");
        return found[0];
    }
}
