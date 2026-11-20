// (Triforce) Add to Steam: the game as a "non-Steam game" in the player's Steam library, with pictures drawn from Triforce's own
// crest (SteamArt.cs). Nothing here needs the window, and Steam's folder is always handed in: tests/test_setup_steam.py compiles
// this file alone and runs it on Steam folders it makes itself. No test and no test run of Setup looks at the PC's real Steam.
//
// Steam keeps an account's shortcuts in <Steam>\userdata\<account>\config\shortcuts.vdf, a binary file of named values:
//   00 name 00 ... 08      a map (its values, then 08)
//   01 name 00 text 00     a text
//   02 name 00 4 bytes     a number (03, 04 and 06 are four bytes too; 07 and 0A are eight)
// The whole file is one map, "shortcuts", of one map for each shortcut, named "0", "1", ...; then one more 08 ends the file.
// This file is another program's: it is read whole, every shortcut and every value in it that is not ours goes back byte for
// byte (they are never taken apart and put together again: the bytes between ours are copied), a file that does not read to
// its very end as such a file is refused and left alone, the new file is written beside the old one and takes its name in one
// step, and the file as it was is kept (shortcuts.vdf.triforce-backup: before the last change; shortcuts.vdf.before-triforce:
// before the first).
//
// Steam reads the file when it starts and writes its own list back when it closes (NX2navi's launcher found the same, and
// closed Steam itself for it): a shortcut written while Steam runs would not show and would be gone at Steam's next exit. So
// nothing is written while Steam runs: "Close Steam first".
//
// The shortcut starts the game itself, Triforce.exe in its folder, not Setup. Its pictures go into config\grid under the
// shortcut's number, as Steam names them (Pictures).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace TriforceSetup;

/// What the player is told when the shortcut cannot be written or removed: a whole sentence
sealed class SteamException : Exception
{
    public SteamException(string message) : base(message) { }
}

/// An account that signed in to Steam on this PC
sealed class SteamUser
{
    public string Root = "", Id = "", Name = "";   // (Steam's folder, the account's folder under userdata, the name its owner chose)
    public bool Recent;                            // (the one Steam was last signed in with)
    public long Seen;                              // (when it last signed in, as Steam writes it; 0 when unknown)
    public string Config => Path.Combine(Root, "userdata", Id, "config");
    public string Shortcuts => Path.Combine(Config, "shortcuts.vdf");
    public string Grid => Path.Combine(Config, "grid");
}

static class Steam
{
    /// The shortcut's name in the library
    public const string Name = "Triforce";
    /// The game's program, by any of its names (Shell.cs GameExe)
    public static readonly string[] GameExes = { "Triforce.exe", "hookshot.exe" };
    public const string Backup = ".triforce-backup", First = ".before-triforce", Temp = ".triforce-tmp";
    public const string CloseFirst = "Close Steam first. Steam writes its own list of shortcuts when it closes, and would take this change away again.";

    /// The pictures of a shortcut: what each is, the end of its file's name after the shortcut's number in config\grid, and
    /// the size Steam's library shows it at (the cover, the wide tile, the background of the game's page, the logo over that
    /// background, and the icon the shortcut names)
    public static readonly (string Kind, string File, int Width, int Height)[] Pictures =
    {
        ("portrait", "p.png", 600, 900), ("wide", ".png", 920, 430), ("hero", "_hero.png", 1920, 620), ("logo", "_logo.png", 640, 360), ("icon", "_icon.png", 256, 256),
    };

    /// Is Steam running? Asked before every write. (Shell.Mods.cs puts its own look here for a test run, a test its own answer.)
    public static Func<string, bool> Running = _ =>
    {
        var found = Process.GetProcessesByName("steam");
        foreach (var p in found) p.Dispose();
        return found.Length > 0;
    };

    // ---- the binary file ----

    sealed class Node
    {
        public byte Type;
        public string Name = "";
        public int Start, Value, End;   // (its type byte; the first byte after its name; one past its last byte)
        public List<Node>? Values;      // (a map's)
    }

    static SteamException Bad(string why) => new("Steam's shortcuts file is not as Steam writes it (" + why + "), so Setup left it alone. Nothing was changed.");

    /// The file's values, or a SteamException when it is not a file of named values from its first byte to its last
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
                default: throw Bad("it holds a value of a kind Setup does not know, " + n.Type);
            }
            if (at > b.Length) throw Bad("it is cut off");
            n.End = at;
            values.Add(n);
        }
    }

    static Node? Named(IEnumerable<Node> values, string name, int type) =>
        values.FirstOrDefault(v => v.Type == type && string.Equals(v.Name, name, StringComparison.OrdinalIgnoreCase));

    static string Text(byte[] b, Node n) => Encoding.UTF8.GetString(b, n.Value, n.End - 1 - n.Value);

    static byte[] Cut(byte[] b, int from, int to)
    {
        var part = new byte[to - from];
        Array.Copy(b, from, part, 0, part.Length);
        return part;
    }

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

    /// The number Steam knows a shortcut by, and names its pictures by: the CRC-32 of its Exe and AppName as they stand in
    /// the file, with the top bit set
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

    /// The map "shortcuts" of a file that was read
    static Node Shortcuts(List<Node> top) => Named(top, "shortcuts", 0) ?? throw Bad("it has no list of shortcuts");

    /// Is this shortcut ours? By its number (this game folder's), or by the name Triforce on a program called as the game is
    /// (a game folder that was moved): another program the player named Triforce is not
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

    /// Are the shortcuts named "0", "1", ... in their order, as Steam names them?
    static bool Counted(Node shortcuts) => shortcuts.Values!.Select((v, i) => v.Type == 0 && v.Name == i.ToString()).All(ok => ok);

    static byte[] Empty()
    {
        var o = new List<byte> { 0 };
        o.AddRange(Encoding.UTF8.GetBytes("shortcuts"));
        o.AddRange(new byte[] { 0, 8, 8 });
        return o.ToArray();
    }

    /// The file (no bytes: none yet) with our shortcut in it: added after the others, or, where one of ours is there, with its
    /// number, name, program, folder and icon set and everything else in it as it was (what the player set in Steam for it:
    /// launch options, tags, hidden). wasThere says which. Every other byte of the file is the same
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
            // (its own values in their places, set; the ones it lacks after them)
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
                // (written under the name as the file spells it, "exe" in files of older Steams; a second value of one name is dropped)
                if (put.Add(which)) Put(which, v.Name);
            }
            foreach (var s in set.Where(s => !put.Contains(s))) Put(s, s);
            o.Add(8);
            o.AddRange(Cut(file, ours.End, file.Length));
        }
        else
        {
            // (named as Steam would name the next one; in a file whose names are not Steam's count, the first number not taken)
            var key = shortcuts.Values!.Count;
            if (!Counted(shortcuts))
                for (key = 0; shortcuts.Values.Any(v => v.Name == key.ToString()); key++) { }
            o.AddRange(Cut(file, 0, shortcuts.End - 1));
            o.Add(0);
            o.AddRange(Encoding.UTF8.GetBytes(key.ToString()));
            o.Add(0);
            // (the values of a shortcut in the order Steam writes them)
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
            o.AddRange(new byte[] { 0 });
            o.AddRange(Encoding.UTF8.GetBytes("tags"));
            o.AddRange(new byte[] { 0, 8 });
            o.Add(8);
            o.AddRange(Cut(file, shortcuts.End - 1, file.Length));
        }
        return Proved(file, o.ToArray(), number, Math.Max(1, shortcuts.Values.Count(s => Ours(file, s, number))));
    }

    /// The file without our shortcuts (ids: the numbers they had, whose pictures can go). The same bytes when none of ours is
    /// in it. The shortcuts after ours are named anew where the file counted them 0, 1, 2 ... as Steam does, so that the count
    /// has no gap; their values are the same bytes
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

    /// Does the file hold a shortcut of ours? False for a file that cannot be read
    public static bool Has(byte[] file, string exe)
    {
        try { return file.Length > 0 && Shortcuts(Read(file)).Values!.Any(s => Ours(file, s, Id(exe, Name))); }
        catch (SteamException) { return false; }
    }

    /// Before a new file is written: it reads as a shortcuts file, holds as many of ours as it should, and every shortcut of
    /// the old one that is not ours is in it, in the same order, with the same bytes. Anything else is a mistake of Setup's
    /// own, and then nothing is written
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
            throw new SteamException("Setup could not put Steam's shortcuts file together again without changing other shortcuts, so it left the file alone. Nothing was changed.");
        return fresh;
    }

    // ---- Steam on this PC ----

    /// Is this a Steam folder?
    public static bool Valid(string root) => Directory.Exists(Path.Combine(root, "userdata")) || File.Exists(Path.Combine(root, "steam.exe"));

    /// Steam's folder as Steam wrote it into the registry (read, never written), else the usual one; null when Steam is not
    /// on this PC. (Never asked in a test run: Shell.Mods.cs SteamRoot.)
    public static string? FromRegistry()
    {
        string? folder = null;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam", false);
            folder = key?.GetValue("SteamPath") as string;
        }
        catch (Exception) { }
        if (string.IsNullOrWhiteSpace(folder))
            folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam");
        try { folder = Path.GetFullPath(folder!.Replace('/', '\\')); } catch (Exception) { return null; }
        return Valid(folder) ? folder : null;
    }

    /// The words and braces of one of Steam's text files ("name" "value", "name" { ... })
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

    /// The accounts that have a folder under userdata, the one to use first: the one Steam marks as its most recent
    /// (config\loginusers.vdf), then by when each last signed in, then, for accounts that file does not name, by when their
    /// folder was last written to
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
                else if (depth == 2 && user != null && i + 1 < words.Count && words[i + 1].StartsWith("\""))
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

    // ---- writing ----

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileExW(string from, string to, int flags);

    /// Only an account's own config folder is ever written to
    static void Within(SteamUser user, string path)
    {
        var config = Path.GetFullPath(user.Config).TrimEnd('\\') + "\\";
        if (!Path.GetFullPath(path).StartsWith(config, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Setup would have written outside Steam's settings folder for the account (" + path + "), and did not.");
    }

    /// A file written whole or not at all: beside its place first, then under its name in one step
    static void WriteWhole(SteamUser user, string file, byte[] bytes)
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
            if (!MoveFileExW(temp, file, 1 | 8))   // (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                throw new IOException("Windows did not let " + Path.GetFileName(file) + " be replaced (error " + Marshal.GetLastWin32Error() + ").");
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
        catch (Exception e) when (e is FileNotFoundException || e is DirectoryNotFoundException) { return new byte[0]; }
    }

    /// The new shortcuts file in place of the old one (old: what was read and worked from). The file as it was is kept
    /// first; should the file have changed since it was read, nothing is written
    static void Replace(SteamUser user, byte[] old, byte[] fresh)
    {
        var file = user.Shortcuts;
        if (old.SequenceEqual(fresh) && File.Exists(file)) return;
        if (Running(user.Root)) throw new SteamException(CloseFirst);
        if (!Bytes(file).SequenceEqual(old)) throw new SteamException("Steam's shortcuts file changed while Setup worked on it. Nothing was changed. Close Steam and try again.");
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

    /// Is our shortcut in this account's library?
    public static bool Added(SteamUser user, string gameExe)
    {
        try { return Has(Bytes(user.Shortcuts), Quoted(gameExe)); } catch (Exception) { return false; }
    }

    /// The program our shortcut in this account's library starts (its path, without the quotes around it); null when the
    /// account has no shortcut of ours, or its file cannot be read
    public static string? Where(SteamUser user, string gameExe)
    {
        try
        {
            var file = Bytes(user.Shortcuts);
            if (file.Length == 0) return null;
            var number = Id(Quoted(gameExe), Name);
            var ours = Shortcuts(Read(file)).Values!.FirstOrDefault(s => Ours(file, s, number));
            if (ours == null) return null;
            return Named(ours.Values!, "Exe", 1) is { } exe ? Text(file, exe).Trim().Trim('"') : "";
        }
        catch (Exception) { return null; }
    }

    static string Quoted(string path) => "\"" + path + "\"";

    /// Adds the game to an account's library, or sets a shortcut of ours that is there to this game folder: gameExe is the
    /// game's own program, started in its own folder. pictures: each kind of Pictures as a PNG file's bytes; where the
    /// shortcut was there already, only the pictures that are missing are written (the player may have set their own in
    /// Steam). True when the shortcut is new
    public static bool Add(SteamUser user, string gameExe, IDictionary<string, byte[]> pictures) => Guarded(() =>
    {
        if (Running(user.Root)) throw new SteamException(CloseFirst);
        if (!Directory.Exists(Path.GetDirectoryName(user.Config)))
            throw new SteamException("Steam has no folder for that account. Start Steam, sign in once, close it, then try again.");
        var old = Bytes(user.Shortcuts);
        var exe = Quoted(gameExe);
        var icon = Path.Combine(user.Grid, Id(exe, Name) + "_icon.png");
        var fresh = With(old, exe, Quoted(Path.GetDirectoryName(gameExe) ?? ""), pictures.ContainsKey("icon") ? icon : gameExe, out var id, out var wasThere);
        Within(user, user.Grid);
        Directory.CreateDirectory(user.Grid);
        // (the shortcut had another number, the game folder having moved: its pictures, the player's own among them, go along)
        var had = new List<uint>();
        if (wasThere) Without(old, exe, out had);
        foreach (var (kind, name, _, _) in Pictures)
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
        return !wasThere;
    });

    /// Takes our shortcut out of an account's library again, and its pictures out of config\grid. False when it was not there
    public static bool Remove(SteamUser user, string gameExe) => Guarded(() =>
    {
        if (Running(user.Root)) throw new SteamException(CloseFirst);
        var old = Bytes(user.Shortcuts);
        var fresh = Without(old, Quoted(gameExe), out var ids);
        if (ids.Count == 0) return false;
        Replace(user, old, fresh);
        foreach (var id in ids)
            foreach (var (_, name, _, _) in Pictures)
            {
                var file = Path.Combine(user.Grid, id + name);
                Within(user, file);
                try { if (File.Exists(file)) File.Delete(file); } catch (Exception) { }
            }
        return true;
    });
}
