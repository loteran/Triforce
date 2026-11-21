// (Triforce) Add to Steam: the game as a "non-Steam game" in the player's Steam library (old setup/Steam.cs, Shell.Steam.cs).
// Steam's folder is always handed in; a test run has only <root>\Steam (Platform.SteamRoots), never the PC's real Steam.
//
// Steam keeps an account's shortcuts in <Steam>\userdata\<account>\config\shortcuts.vdf, a binary file of named values:
//   00 name 00 ... 08      a map (its values, then 08)
//   01 name 00 text 00     a text
//   02 name 00 4 bytes     a number (03, 04 and 06 are four bytes too; 07 and 0A are eight)
// The whole file is one map, "shortcuts", of one map for each shortcut, named "0", "1", ...; then one more 08 ends the file.
// This file is another program's: it is read whole, every shortcut and every value in it that is not ours goes back byte for
// byte, a file that does not read to its very end as such a file is refused and left alone, the new file is written beside the
// old one and takes its name in one step, and the file as it was is kept (shortcuts.vdf.triforce-backup: before the last change;
// shortcuts.vdf.before-triforce: before the first).
//
// Steam reads the file when it starts and writes its own list back when it closes: a shortcut written while Steam runs would not
// show and would be gone at Steam's next exit. So nothing is written while Steam runs: "Close Steam first".
//
// The pictures (config\grid\<appid><suffix>): the old Setup drew them from its crest with WPF (SteamArt.cs). That drawing is
// the App's now: it hands Add the PNG bytes for each kind of Pictures. Linux (the Steam Deck): the same file and the same appid
// algorithm (research/02 section 11); the shortcut starts the folder's "triforce" launcher script.
using System.Text;

namespace Triforce.Logic;

public sealed class SteamException : Exception
{
    public SteamException(string message) : base(message) { }
}

/// <summary>An account that signed in to Steam on this PC.</summary>
public sealed class SteamUser
{
    public string Root { get; set; } = "";
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public bool Recent { get; set; }
    public long Seen;
    public string Config => Path.Combine(Root, "userdata", Id, "config");
    public string Shortcuts => Path.Combine(Config, "shortcuts.vdf");
    public string Grid => Path.Combine(Config, "grid");
}

/// <summary>The binary shortcuts file, byte-exact (old Steam.cs, unchanged in its rules).</summary>
public static class SteamVdf
{
    /// <summary>The shortcut's name in the library.</summary>
    public const string Name = "Triforce";
    /// <summary>The game's program, by any of its names (Windows Triforce.exe / hookshot.exe; Linux the folder's triforce script).</summary>
    public static readonly string[] GameExes = { "Triforce.exe", "hookshot.exe", "triforce", "hookshot" };

    /// <summary>The pictures of a shortcut: what each is, the end of its file's name after the shortcut's number in config\grid, and
    /// the size Steam's library shows it at.</summary>
    public static readonly (string Kind, string File, int Width, int Height)[] Pictures =
    {
        ("portrait", "p.png", 600, 900), ("wide", ".png", 920, 430), ("hero", "_hero.png", 1920, 620), ("logo", "_logo.png", 640, 360), ("icon", "_icon.png", 256, 256),
    };

    sealed class Node
    {
        public byte Type;
        public string Name = "";
        public int Start, Value, End;
        public List<Node>? Values;
    }

    static SteamException Bad(string why) => new("Steam's shortcuts file is not as Steam writes it (" + why + "), so the launcher left it alone. Nothing was changed.");

    static List<Node> Read(byte[] b)
    {
        var at = 0;
        var top = Map(b, ref at, 0);
        if (at != b.Length) throw Bad("it goes on after its end");
        return top;
    }

    static List<Node> Map(byte[] b, ref int at, int depth)
    {
        if (depth > 16) throw Bad("its maps are nested too deep");
        var values = new List<Node>();
        for (;;)
        {
            if (at >= b.Length) throw Bad("it is cut off");
            var n = new Node { Start = at, Type = b[at++] };
            if (n.Type == 8) return values;
            var end = Array.IndexOf(b, (byte)0, at);
            if (end < 0) throw Bad("it is cut off");
            n.Name = Encoding.UTF8.GetString(b, at, end - at);
            n.Value = at = end + 1;
            switch (n.Type)
            {
                case 0: n.Values = Map(b, ref at, depth + 1); break;
                case 1:
                    end = Array.IndexOf(b, (byte)0, at);
                    if (end < 0) throw Bad("it is cut off");
                    at = end + 1;
                    break;
                case 2: case 3: case 4: case 6: at += 4; break;
                case 7: case 10: at += 8; break;
                default: throw Bad("it holds a value of a kind the launcher does not know, " + n.Type);
            }
            if (at > b.Length) throw Bad("it is cut off");
            n.End = at;
            values.Add(n);
        }
    }

    static Node? Named(IEnumerable<Node> values, string name, int type) =>
        values.FirstOrDefault(v => v.Type == type && string.Equals(v.Name, name, StringComparison.OrdinalIgnoreCase));

    static string Text(byte[] b, Node n) => Encoding.UTF8.GetString(b, n.Value, n.End - 1 - n.Value);

    static byte[] Cut(byte[] b, int from, int to) => b.AsSpan(from, to - from).ToArray();

    static void PutText(List<byte> o, string name, string text)
    {
        if (text.IndexOf('\0') >= 0) throw new SteamException("A path with a zero in it cannot be written into Steam's shortcuts file.");
        o.Add(1);
        o.AddRange(Encoding.UTF8.GetBytes(name));
        o.Add(0);
        o.AddRange(Encoding.UTF8.GetBytes(text));
        o.Add(0);
    }

    static void PutNumber(List<byte> o, string name, uint number)
    {
        o.Add(2);
        o.AddRange(Encoding.UTF8.GetBytes(name));
        o.Add(0);
        o.AddRange(BitConverter.GetBytes(number));
    }

    /// <summary>The number Steam knows a shortcut by, and names its pictures by: the CRC-32 of its Exe and AppName as they stand in
    /// the file (Exe quoted), with the top bit set.</summary>
    public static uint Id(string exe, string name)
    {
        var crc = 0xFFFFFFFFu;
        foreach (var x in Encoding.UTF8.GetBytes(exe + name))
        {
            crc ^= x;
            for (var k = 0; k < 8; k++) crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
        return ~crc | 0x80000000u;
    }

    static Node Shortcuts(List<Node> top) => Named(top, "shortcuts", 0) ?? throw Bad("it has no list of shortcuts");

    /// <summary>Is this shortcut ours? By its number (this game folder's), or by the name Triforce on a program called as the game is
    /// (a game folder that was moved): another program the player named Triforce is not.</summary>
    static bool Ours(byte[] b, Node shortcut, uint id)
    {
        if (shortcut.Values == null) return false;
        if (Named(shortcut.Values, "appid", 2) is { } number && BitConverter.ToUInt32(b, number.Value) == id) return true;
        if (Named(shortcut.Values, "AppName", 1) is not { } name || Text(b, name) != Name) return false;
        if (Named(shortcut.Values, "Exe", 1) is not { } exe) return false;
        var file = Text(b, exe).Trim().Trim('"').Replace('/', '\\');
        file = file.Substring(file.LastIndexOf('\\') + 1);
        return GameExes.Any(g => string.Equals(g, file, StringComparison.OrdinalIgnoreCase));
    }

    static bool Counted(Node shortcuts) => shortcuts.Values!.Select((v, i) => v.Type == 0 && v.Name == i.ToString()).All(ok => ok);

    static byte[] Empty()
    {
        var o = new List<byte> { 0 };
        o.AddRange(Encoding.UTF8.GetBytes("shortcuts"));
        o.AddRange(new byte[] { 0, 8, 8 });
        return o.ToArray();
    }

    /// <summary>The file (no bytes: none yet) with our shortcut in it: added after the others, or, where one of ours is there, with
    /// its number, name, program, folder and icon set and everything else in it as it was (launch options, tags, hidden).</summary>
    public static byte[] With(byte[] file, string exe, string startDir, string icon, out uint id, out bool wasThere)
    {
        id = Id(exe, Name);
        if (file.Length == 0) file = Empty();
        var shortcuts = Shortcuts(Read(file));
        var number = id;
        var ours = shortcuts.Values!.FirstOrDefault(s => Ours(file, s, number));
        wasThere = ours != null;
        var o = new List<byte>();
        if (ours != null)
        {
            string[] set = { "appid", "AppName", "Exe", "StartDir", "icon" };
            void Put(string which, string spelled)
            {
                if (which == "appid") PutNumber(o, spelled, number);
                else PutText(o, spelled, which == "AppName" ? Name : which == "Exe" ? exe : which == "StartDir" ? startDir : icon);
            }
            o.AddRange(Cut(file, 0, ours.Value));
            var put = new HashSet<string>();
            foreach (var v in ours.Values!)
            {
                var which = set.FirstOrDefault(s => string.Equals(s, v.Name, StringComparison.OrdinalIgnoreCase));
                if (which == null) { o.AddRange(Cut(file, v.Start, v.End)); continue; }
                if (put.Add(which)) Put(which, v.Name);   // (under the name as the file spells it; a second value of one name dropped)
            }
            foreach (var s in set.Where(s => !put.Contains(s))) Put(s, s);
            o.Add(8);
            o.AddRange(Cut(file, ours.End, file.Length));
        }
        else
        {
            var key = shortcuts.Values!.Count;
            if (!Counted(shortcuts))
                for (key = 0; shortcuts.Values.Any(v => v.Name == key.ToString()); key++) { }
            o.AddRange(Cut(file, 0, shortcuts.End - 1));
            o.Add(0);
            o.AddRange(Encoding.UTF8.GetBytes(key.ToString()));
            o.Add(0);
            PutNumber(o, "appid", number);
            PutText(o, "AppName", Name);
            PutText(o, "Exe", exe);
            PutText(o, "StartDir", startDir);
            PutText(o, "icon", icon);
            PutText(o, "ShortcutPath", "");
            PutText(o, "LaunchOptions", "");
            PutNumber(o, "IsHidden", 0);
            PutNumber(o, "AllowDesktopConfig", 1);
            PutNumber(o, "AllowOverlay", 1);
            PutNumber(o, "OpenVR", 0);
            PutNumber(o, "Devkit", 0);
            PutText(o, "DevkitGameID", "");
            PutNumber(o, "DevkitOverrideAppID", 0);
            PutNumber(o, "LastPlayTime", 0);
            PutText(o, "FlatpakAppID", "");
            o.Add(0);
            o.AddRange(Encoding.UTF8.GetBytes("tags"));
            o.AddRange(new byte[] { 0, 8 });
            o.Add(8);
            o.AddRange(Cut(file, shortcuts.End - 1, file.Length));
        }
        return Proved(file, o.ToArray(), number, Math.Max(1, shortcuts.Values!.Count(s => Ours(file, s, number))));
    }

    /// <summary>The file without our shortcuts (ids: the numbers they had). The same bytes when none of ours is in it. The shortcuts
    /// after ours are named anew where the file counted them 0, 1, 2 ...; their values are the same bytes.</summary>
    public static byte[] Without(byte[] file, string exe, out List<uint> ids)
    {
        ids = new List<uint>();
        if (file.Length == 0) return file;
        var shortcuts = Shortcuts(Read(file));
        var number = Id(exe, Name);
        if (!shortcuts.Values!.Any(s => Ours(file, s, number))) return file;
        var counted = Counted(shortcuts);
        var o = new List<byte>();
        o.AddRange(Cut(file, 0, shortcuts.Value));
        var kept = 0;
        foreach (var s in shortcuts.Values!)
        {
            if (Ours(file, s, number))
            {
                ids.Add(Named(s.Values!, "appid", 2) is { } n ? BitConverter.ToUInt32(file, n.Value) : number);
                continue;
            }
            if (counted)
            {
                o.Add(s.Type);
                o.AddRange(Encoding.UTF8.GetBytes(kept.ToString()));
                o.Add(0);
                o.AddRange(Cut(file, s.Value, s.End));
            }
            else o.AddRange(Cut(file, s.Start, s.End));
            kept++;
        }
        o.AddRange(Cut(file, shortcuts.End - 1, file.Length));
        return Proved(file, o.ToArray(), number, 0);
    }

    /// <summary>Does the file hold a shortcut of ours? False for a file that cannot be read.</summary>
    public static bool Has(byte[] file, string exe)
    {
        try { return file.Length > 0 && Shortcuts(Read(file)).Values!.Any(s => Ours(file, s, Id(exe, Name))); }
        catch (SteamException) { return false; }
    }

    /// <summary>The program our shortcut starts (unquoted), "" when it names none, null when there is none.</summary>
    public static string? Where(byte[] file, string exe)
    {
        if (file.Length == 0) return null;
        var number = Id(exe, Name);
        var ours = Shortcuts(Read(file)).Values!.FirstOrDefault(s => Ours(file, s, number));
        if (ours == null) return null;
        return Named(ours.Values!, "Exe", 1) is { } e ? Text(file, e).Trim().Trim('"') : "";
    }

    /// <summary>Before a new file is written: it reads as a shortcuts file, holds as many of ours as it should, and every shortcut of
    /// the old one that is not ours is in it, in the same order, with the same bytes.</summary>
    static byte[] Proved(byte[] old, byte[] fresh, uint id, int ours)
    {
        List<byte[]> Others(byte[] b, out int mine)
        {
            var all = Shortcuts(Read(b)).Values!;
            mine = all.Count(s => Ours(b, s, id));
            return all.Where(s => !Ours(b, s, id)).Select(s => Cut(b, s.Value, s.End)).ToList();
        }
        var before = Others(old, out _);
        var after = Others(fresh, out var now);
        if (now != ours || before.Count != after.Count || before.Where((x, i) => !x.SequenceEqual(after[i])).Any())
            throw new SteamException("The launcher could not put Steam's shortcuts file together again without changing other shortcuts, so it left the file alone. Nothing was changed.");
        return fresh;
    }
}

/// <summary>Steam on this PC: its folder, its accounts, Add / Remove / Where for the game.</summary>
public sealed class SteamLibrary
{
    public const string Backup = ".triforce-backup", First = ".before-triforce", Temp = ".triforce-tmp";
    public const string CloseFirst = "Close Steam first. Steam writes its own list of shortcuts when it closes, and would take this change away again.";

    readonly TriforceContext ctx;
    public SteamLibrary(TriforceContext ctx)
    {
        this.ctx = ctx;
        // (a test run has its own Steam folder, which the PC's Steam, running or not, has nothing to do with)
        Running = root =>
        {
            var procs = ctx.Platform.FindProcesses("steam");
            return ctx.TestRoot == null ? procs.Count > 0 : procs.Any(p => p.Exe != null && TriforceContext.Inside(ctx.TestRoot, p.Exe));
        };
    }

    /// <summary>Is Steam running? Asked before every write (a test puts its own answer here).</summary>
    public Func<string, bool> Running { get; set; }

    public static bool Valid(string root) => Directory.Exists(Path.Combine(root, "userdata")) || File.Exists(Path.Combine(root, "steam.exe")) || File.Exists(Path.Combine(root, "steam.sh"));

    /// <summary>Steam's folder on this PC, null when Steam is not there.</summary>
    public string? Root() => ctx.Platform.SteamRoots().FirstOrDefault(r => { try { return Valid(r); } catch (Exception) { return false; } });

    /// <summary>The words and braces of one of Steam's text files ("name" "value", "name" { ... }).</summary>
    static List<string> Words(string text)
    {
        var words = new List<string>();
        for (var i = 0; i < text.Length; i++)
        {
            var c = text[i];
            if (c == '{' || c == '}') words.Add(c.ToString());
            else if (c == '/' && i + 1 < text.Length && text[i + 1] == '/') { while (i < text.Length && text[i] != '\n') i++; }
            else if (c == '"')
            {
                var s = new StringBuilder("\"");
                for (i++; i < text.Length && text[i] != '"'; i++)
                {
                    if (text[i] == '\\' && i + 1 < text.Length) i++;
                    s.Append(text[i]);
                }
                words.Add(s.ToString());
            }
        }
        return words;
    }

    /// <summary>The accounts that have a folder under userdata, the one to use first: the one Steam marks as its most recent
    /// (config\loginusers.vdf), then by when each last signed in, then by when their folder was last written to.</summary>
    public static List<SteamUser> Users(string root)
    {
        var users = new List<SteamUser>();
        string[] folders = { };
        try { folders = Directory.GetDirectories(Path.Combine(root, "userdata")); } catch (Exception) { }
        foreach (var f in folders)
        {
            var id = Path.GetFileName(f);
            if (id != "0" && id.Length <= 10 && id.All(c => c >= '0' && c <= '9')) users.Add(new SteamUser { Root = root, Id = id });
        }
        try
        {
            var words = Words(File.ReadAllText(Path.Combine(root, "config", "loginusers.vdf")));
            const long first = 76561197960265728;   // (an account's number in that file is this much more than its folder's name)
            SteamUser? user = null;
            var depth = 0;
            for (var i = 0; i < words.Count; i++)
            {
                if (words[i] == "{") { depth++; continue; }
                if (words[i] == "}") { depth--; if (depth < 2) user = null; continue; }
                var word = words[i].Substring(1);
                if (depth == 1 && i + 1 < words.Count && words[i + 1] == "{")
                    user = long.TryParse(word, out var id64) && id64 > first ? users.FirstOrDefault(u => u.Id == (id64 - first).ToString()) : null;
                else if (depth == 2 && user != null && i + 1 < words.Count && words[i + 1].StartsWith('"'))
                {
                    var value = words[++i].Substring(1);
                    if (string.Equals(word, "PersonaName", StringComparison.OrdinalIgnoreCase)) user.Name = value;
                    else if (string.Equals(word, "MostRecent", StringComparison.OrdinalIgnoreCase)) user.Recent = value == "1";
                    else if (string.Equals(word, "Timestamp", StringComparison.OrdinalIgnoreCase)) long.TryParse(value, out user.Seen);
                }
            }
        }
        catch (Exception) { }
        DateTime Written(SteamUser u)
        {
            try { return Directory.GetLastWriteTimeUtc(Directory.Exists(u.Config) ? u.Config : Path.GetDirectoryName(u.Config)!); } catch (Exception) { return DateTime.MinValue; }
        }
        return users.OrderByDescending(u => u.Recent).ThenByDescending(u => u.Seen).ThenByDescending(Written).ThenBy(u => u.Id, StringComparer.Ordinal).ToList();
    }

    /// <summary>The account Steam was last signed in with, null when there is no Steam or nobody signed in.</summary>
    public SteamUser? User() => Root() is { } r ? Users(r).FirstOrDefault() : null;

    /// <summary>What the game's shortcut starts: Windows the game's own Triforce.exe; Linux the folder's "triforce" launcher script
    /// (research/12 6: it shows Update; the AppImage's --play would go straight to the game).</summary>
    public string ShortcutTarget(string gameFolder) => ctx.IsWindows
        ? new Game(ctx).FindExe(gameFolder) ?? Path.Combine(gameFolder, "Triforce.exe")
        : Path.Combine(gameFolder, "triforce");

    /// <summary>Only an account's own config folder is ever written to (and, in a test run, only inside the test folder).</summary>
    void Within(SteamUser user, string path)
    {
        if (!TriforceContext.Inside(user.Config, path) || TriforceContext.SamePath(user.Config, path))
            throw new InvalidOperationException("The launcher would have written outside Steam's settings folder for the account (" + path + "), and did not.");
        ctx.Require(path);
    }

    /// <summary>A file written whole or not at all: beside its place first, then under its name in one step.</summary>
    void WriteWhole(SteamUser user, string file, byte[] bytes)
    {
        Within(user, file);
        var temp = file + Temp;
        try
        {
            using (var f = new FileStream(temp, FileMode.Create, FileAccess.Write, FileShare.None))
            {
                f.Write(bytes, 0, bytes.Length);
                f.Flush(true);
            }
            ctx.Platform.ReplaceFile(temp, file);
        }
        catch (Exception)
        {
            try { File.Delete(temp); } catch (Exception) { }
            throw;
        }
    }

    static byte[] Bytes(string file)
    {
        try { return File.ReadAllBytes(file); }
        catch (Exception e) when (e is FileNotFoundException || e is DirectoryNotFoundException) { return Array.Empty<byte>(); }
    }

    /// <summary>The new shortcuts file in place of the old one: the file as it was is kept first; should the file have changed since
    /// it was read, nothing is written.</summary>
    void Replace(SteamUser user, byte[] old, byte[] fresh)
    {
        var file = user.Shortcuts;
        if (old.SequenceEqual(fresh) && File.Exists(file)) return;
        if (Running(user.Root)) throw new SteamException(CloseFirst);
        if (!Bytes(file).SequenceEqual(old)) throw new SteamException("Steam's shortcuts file changed while the launcher worked on it. Nothing was changed. Close Steam and try again.");
        if (File.Exists(file))
        {
            if (!File.Exists(file + First)) WriteWhole(user, file + First, old);
            WriteWhole(user, file + Backup, old);
        }
        WriteWhole(user, file, fresh);
    }

    static T Guarded<T>(Func<T> work)
    {
        try { return work(); }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            throw new SteamException("Steam's files could not be written: " + e.Message);
        }
    }

    static string Quoted(string path) => "\"" + path + "\"";

    /// <summary>Is our shortcut in this account's library?</summary>
    public bool Added(SteamUser user, string gameExe)
    {
        try { return SteamVdf.Has(Bytes(user.Shortcuts), Quoted(gameExe)); } catch (Exception) { return false; }
    }

    /// <summary>The program our shortcut in this account's library starts, null when there is none (the "Set to this folder" case
    /// when it is another folder's).</summary>
    public string? Where(SteamUser user, string gameExe)
    {
        try { return SteamVdf.Where(Bytes(user.Shortcuts), Quoted(gameExe)); } catch (Exception) { return null; }
    }

    /// <summary>Adds the game to an account's library, or sets a shortcut of ours that is there to this game folder. pictures: each
    /// kind of SteamVdf.Pictures as a PNG file's bytes (the App draws them); where the shortcut was there already, only the pictures
    /// that are missing are written, and the pictures of its old number move to the new one. True when the shortcut is new.</summary>
    public bool Add(SteamUser user, string gameExe, IReadOnlyDictionary<string, byte[]> pictures) => Guarded(() =>
    {
        if (Running(user.Root)) throw new SteamException(CloseFirst);
        if (!Directory.Exists(Path.GetDirectoryName(user.Config)))
            throw new SteamException("Steam has no folder for that account. Start Steam, sign in once, close it, then try again.");
        var old = Bytes(user.Shortcuts);
        var exe = Quoted(gameExe);
        var icon = Path.Combine(user.Grid, SteamVdf.Id(exe, SteamVdf.Name) + "_icon.png");
        var fresh = SteamVdf.With(old, exe, Quoted(Path.GetDirectoryName(gameExe) ?? ""), pictures.ContainsKey("icon") ? icon : gameExe, out var id, out var wasThere);
        Within(user, Path.Combine(user.Grid, "x"));
        Directory.CreateDirectory(user.Grid);
        var had = new List<uint>();
        if (wasThere) SteamVdf.Without(old, exe, out had);
        foreach (var (kind, name, _, _) in SteamVdf.Pictures)
        {
            var file = Path.Combine(user.Grid, id + name);
            foreach (var before in had.Where(h => h != id).Select(h => Path.Combine(user.Grid, h + name)).Where(File.Exists))
            {
                Within(user, before);
                if (File.Exists(file)) File.Delete(before); else File.Move(before, file);
            }
            if (pictures.TryGetValue(kind, out var png) && (!wasThere || !File.Exists(file))) WriteWhole(user, file, png);
        }
        Replace(user, old, fresh);
        // (no account's name or number in the log: it is pasted where help is asked for)
        ctx.Log.Write("steam: the shortcut was " + (wasThere ? "set to " + gameExe : "added") + " for the account Steam was last signed in with");
        return !wasThere;
    });

    /// <summary>Takes our shortcut out of an account's library again, and its pictures out of config\grid. False when it was not there.</summary>
    public bool Remove(SteamUser user, string gameExe) => Guarded(() =>
    {
        if (Running(user.Root)) throw new SteamException(CloseFirst);
        var old = Bytes(user.Shortcuts);
        var fresh = SteamVdf.Without(old, Quoted(gameExe), out var ids);
        if (ids.Count == 0) return false;
        Replace(user, old, fresh);
        foreach (var id in ids)
            foreach (var (_, name, _, _) in SteamVdf.Pictures)
            {
                var file = Path.Combine(user.Grid, id + name);
                Within(user, file);
                try { if (File.Exists(file)) File.Delete(file); } catch (Exception) { }
            }
        ctx.Log.Write("steam: the shortcut was removed for the account Steam was last signed in with");
        return true;
    });
}
