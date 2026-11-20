// tests/test_setup_steam.py: setup/Steam.cs (compiled alone, no window) on Steam folders this program makes in a temporary
// folder, with shortcuts.vdf files it authors byte by byte. No Setup runs; the PC's real Steam and the registry are not
// looked at (Steam.FromRegistry is never called here).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace TriforceSetup;

static class SteamTest
{
    static int checks;
    static string temp = "";

    static void Check(bool ok, string what)
    {
        if (!ok) throw new Exception(what);
        checks++;
    }

    // ---- shortcuts files, authored here (not with Steam.cs's own writer) ----

    static byte[] Join(params byte[][] parts) => parts.SelectMany(p => p).ToArray();
    static byte[] Z(string text) => Join(Encoding.UTF8.GetBytes(text), new byte[] { 0 });
    static byte[] Text(string name, string text) => Join(new byte[] { 1 }, Z(name), Z(text));
    static byte[] Number(string name, uint number) => Join(new byte[] { 2 }, Z(name), BitConverter.GetBytes(number));
    static byte[] Map(string name, params byte[][] values) => Join(new byte[] { 0 }, Z(name), Join(values), new byte[] { 8 });
    /// A shortcuts file of these shortcuts' values, named 0, 1, 2 ... (or as names says)
    static byte[] File_(byte[][] shortcuts, string[]? names = null) =>
        Join(Map("shortcuts", shortcuts.Select((s, i) => Map(names != null ? names[i] : i.ToString(), s)).ToArray()), new byte[] { 8 });

    const string Game = @"C:\Games\Triforce\Triforce.exe", Exe = "\"" + Game + "\"", Dir = @"""C:\Games\Triforce""", Icon = @"C:\Steam\userdata\1\config\grid\icon.png";
    const uint GameId = 0xBCEDCD22;   // (zlib's CRC-32 of "C:\Games\Triforce\Triforce.exe"Triforce, quotes included, with the top bit set)

    /// A new shortcut as Steam writes one, with our values
    static byte[] Ours(string exe = Exe, string dir = Dir, string icon = Icon, uint? id = null) => Join(
        Number("appid", id ?? Steam.Id(exe, "Triforce")), Text("AppName", "Triforce"), Text("Exe", exe), Text("StartDir", dir), Text("icon", icon), Text("ShortcutPath", ""),
        Text("LaunchOptions", ""), Number("IsHidden", 0), Number("AllowDesktopConfig", 1), Number("AllowOverlay", 1), Number("OpenVR", 0), Number("Devkit", 0),
        Text("DevkitGameID", ""), Number("DevkitOverrideAppID", 0), Number("LastPlayTime", 0), Text("FlatpakAppID", ""), Map("tags"));

    /// Somebody else's shortcuts, with what a file may hold besides the usual: a 64-bit number, a float, a value of a name
    /// Setup does not know, bytes that are no UTF-8, a map in a map, names in the lower case of older Steams
    static readonly byte[] Other0 = Join(
        Number("appid", 0x9ABCDEF0), Text("AppName", "Another game"), Text("Exe", "\"D:\\Games\\Another\\game.exe\""), Text("StartDir", "\"D:\\Games\\Another\\\""), Text("icon", ""),
        Text("LaunchOptions", "-windowed \"quoted\""), Number("IsHidden", 1), new byte[] { 7 }, Z("SixtyFour"), BitConverter.GetBytes(0x1122334455667788UL),
        new byte[] { 3 }, Z("AFloat"), BitConverter.GetBytes(1.5f), Join(new byte[] { 1 }, Z("NotUtf8"), new byte[] { 0xFF, 0xFE, 0x80, 0 }),
        Map("tags", Text("0", "Favorite"), Text("1", "Zelda"), Map("deeper", Number("x", 7))));
    static readonly byte[] Other1 = Join(
        Text("appname", "Old Steam's spelling"), Text("exe", "\"C:\\Emu\\emu.exe\""), Text("StartDir", "\"C:\\Emu\""), new byte[] { 10 }, Z("Signed64"), BitConverter.GetBytes(-5L),
        new byte[] { 6 }, Z("AColour"), new byte[] { 1, 2, 3, 4 }, new byte[] { 4 }, Z("APointer"), new byte[] { 9, 9, 9, 9 }, Map("tags"));
    /// A shortcut the player made and called Triforce, to Setup: not ours
    static readonly byte[] Namesake = Join(Number("appid", 0x80001234), Text("AppName", "Triforce"), Text("Exe", "\"C:\\Programs\\Triforce\\TriforceSetup.exe\""), Text("StartDir", ""), Map("tags"));

    static T? Throws<T>(Action a) where T : Exception
    {
        try { a(); } catch (T e) { return e; }
        return null;
    }

    static string Hex(byte[] b) => BitConverter.ToString(b);

    static int Main()
    {
        temp = Path.Combine(Path.GetTempPath(), "triforce-steam-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        var running = Steam.Running;
        Steam.Running = _ => false;
        try
        {
            Bytes();
            Refusals();
            Accounts();
            OnDisk();
        }
        finally
        {
            Steam.Running = running;
            try { Directory.Delete(temp, true); } catch (Exception) { }
        }
        Console.WriteLine("setup_steam_test: " + checks + " checks passed");
        return 0;
    }

    static void Bytes()
    {
        // the shortcut's number
        Check(Steam.Id("123456789", "") == 0xCBF43926 && Steam.Id("1234", "56789") == 0xCBF43926, "the CRC-32 is not the usual one");
        Check(Steam.Id(Exe, "Triforce") == GameId && Steam.Id("\"D:\\Old place\\Triforce.exe\"", "Triforce") == 0xCDD663C6, "the shortcut's number: " + Steam.Id(Exe, "Triforce").ToString("X8"));
        Check((Steam.Id("a", "b") & 0x80000000) != 0, "the shortcut's number has no top bit");

        // the pictures Steam's library shows, by the names it looks for
        var pictures = string.Join(" ", Steam.Pictures.Select(p => p.Kind + "=" + p.File + ":" + p.Width + "x" + p.Height));
        Check(pictures == "portrait=p.png:600x900 wide=.png:920x430 hero=_hero.png:1920x620 logo=_logo.png:640x360 icon=_icon.png:256x256", "the pictures: " + pictures);

        // no file yet, an empty one, and one with no shortcuts: the file Steam would write with one shortcut
        var one = File_(new[] { Ours() });
        foreach (var none in new[] { new byte[0], File_(new byte[0][]) })
        {
            var made = Steam.With(none, Exe, Dir, Icon, out var id, out var wasThere);
            Check(made.SequenceEqual(one) && id == GameId && !wasThere, "the first shortcut of a file:\n" + Hex(made) + "\nnot\n" + Hex(one));
        }
        Check(Steam.Has(one, Exe) && !Steam.Has(new byte[0], Exe) && !Steam.Has(File_(new[] { Other0, Namesake }), Exe), "which file holds a shortcut of ours");

        // several shortcuts: ours goes after them, and every byte before it stays
        var others = File_(new[] { Other0, Other1, Namesake });
        var added = Steam.With(others, Exe, Dir, Icon, out _, out var there);
        Check(added.SequenceEqual(File_(new[] { Other0, Other1, Namesake, Ours() })) && !there, "a shortcut added after three others changed them, or is not as Steam writes one");
        Check(added.Take(others.Length - 2).SequenceEqual(others.Take(others.Length - 2)), "the bytes before the new shortcut are not the file's own");
        // (and again: it is there, nothing changes)
        var again = Steam.With(added, Exe, Dir, Icon, out _, out there);
        Check(again.SequenceEqual(added) && there, "adding what is there changed the file");

        // ours is there, from a game folder that has moved, with what the player set in Steam: only its number, program,
        // folder and icon change, in their places
        byte[] Set(string exe, string dir, string icon, uint id) => Join(
            Text("LaunchOptions", "-fullscreen"), Number("appid", id), Text("AppName", "Triforce"), Number("IsHidden", 1), Text("Exe", exe), Text("StartDir", dir),
            Text("icon", icon), new byte[] { 7 }, Z("Unknown64"), BitConverter.GetBytes(42UL), Map("tags", Text("0", "Favorite")), Number("LastPlayTime", 1731672000));
        var moved = File_(new[] { Other0, Set("\"D:\\Old place\\Triforce.exe\"", "\"D:\\Old place\"", "old.png", 0xCDD663C6), Other1 });
        var set = Steam.With(moved, Exe, Dir, Icon, out var newId, out there);
        Check(set.SequenceEqual(File_(new[] { Other0, Set(Exe, Dir, Icon, GameId), Other1 })) && there && newId == GameId, "a shortcut of ours set to a new game folder lost what the player set, or changed its neighbours");
        // (found by its number alone, whatever it is called now; values it lacks are added; a name spelled as older Steams did stays so)
        var renamed = File_(new[] { Join(Number("AppId", GameId), Text("appname", "My Zelda"), Text("exe", "x"), Map("tags")), Other0 });
        set = Steam.With(renamed, Exe, Dir, Icon, out _, out there);
        Check(set.SequenceEqual(File_(new[] { Join(Number("AppId", GameId), Text("appname", "Triforce"), Text("exe", Exe), Map("tags"), Text("StartDir", Dir), Text("icon", Icon)), Other0 })) && there,
              "a shortcut found by its number: " + Hex(set));
        // (the game under its older name)
        Check(Steam.Has(File_(new[] { Join(Text("AppName", "Triforce"), Text("Exe", "\"E:/Games/Old/HOOKSHOT.EXE\"")) }), Exe), "a shortcut to the game under its older name is not ours");

        // names that are not Steam's count: the new one takes the first number that is free, the others keep theirs
        var odd = File_(new[] { Other0, Other1, Namesake }, new[] { "1", "zero", "0" });
        added = Steam.With(odd, Exe, Dir, Icon, out _, out _);
        Check(added.SequenceEqual(File_(new[] { Other0, Other1, Namesake, Ours() }, new[] { "1", "zero", "0", "2" })), "a shortcut added to a file with names of its own");

        // taking ours out again: the file as it was before, to the byte
        var back = Steam.Without(File_(new[] { Other0, Other1, Namesake, Ours() }), Exe, out var ids);
        Check(back.SequenceEqual(others) && ids.SequenceEqual(new[] { GameId }), "removing the shortcut that was added does not give the file back as it was");
        // (from the middle: the ones after it are counted anew, as Steam counts, and are the same bytes otherwise)
        back = Steam.Without(moved, Exe, out ids);
        Check(back.SequenceEqual(File_(new[] { Other0, Other1 })) && ids.SequenceEqual(new[] { 0xCDD663C6 }), "removing a shortcut from the middle");
        back = Steam.Without(File_(new[] { Other0, Ours(), Other1 }, new[] { "a", "b", "c" }), Exe, out _);
        Check(back.SequenceEqual(File_(new[] { Other0, Other1 }, new[] { "a", "c" })), "removing from a file with names of its own renamed the others");
        back = Steam.Without(one, Exe, out _);
        Check(back.SequenceEqual(File_(new byte[0][])), "removing the only shortcut does not leave an empty list");
        back = Steam.Without(File_(new[] { Ours(), Other0, Ours("\"D:\\Old place\\Triforce.exe\"") }), Exe, out ids);
        Check(back.SequenceEqual(File_(new[] { Other0 })) && ids.Count == 2, "two shortcuts of ours were not both removed");
        // (none of ours in it: the same bytes, and nothing to write)
        back = Steam.Without(others, Exe, out ids);
        Check(ReferenceEquals(back, others) && ids.Count == 0 && Steam.Without(new byte[0], Exe, out _).Length == 0, "removing where nothing of ours is made a new file");
    }

    static void Refusals()
    {
        var good = File_(new[] { Other0, Ours(), Other1 });
        // cut off anywhere: every beginning of a good file but the whole of it is refused (no bytes at all is no file yet)
        for (var length = 1; length < good.Length; length++)
        {
            var cut = good.Take(length).ToArray();
            Check(Throws<SteamException>(() => Steam.With(cut, Exe, Dir, Icon, out _, out _)) != null, "a file cut off after " + length + " of " + good.Length + " bytes was taken for a whole one");
            Check(Throws<SteamException>(() => Steam.Without(cut, Exe, out _)) != null, "a file cut off after " + length + " bytes was taken for a whole one (removing)");
            Check(!Steam.Has(cut, Exe), "a file cut off after " + length + " bytes holds a shortcut of ours");
        }
        var said = Throws<SteamException>(() => Steam.With(good.Take(good.Length / 2).ToArray(), Exe, Dir, Icon, out _, out _))!.Message;
        Check(said.Contains("cut off") && said.Contains("left it alone") && said.Contains("Nothing was changed"), "what is said of a file that is cut off: " + said);
        foreach (var (bad, why) in new[]
                 {
                     (Join(good, new byte[] { 0 }), "goes on after its end"), (Join(good, good), "goes on after its end"),
                     (File_(new[] { Join(Text("AppName", "x"), new byte[] { 5 }, Z("Wide"), new byte[] { 65, 0, 0, 0 }) }), "a kind Setup does not know"),
                     (File_(new[] { Join(new byte[] { 9 }, Z("Nine"), new byte[] { 1, 2, 3, 4 }) }), "a kind Setup does not know"),
                     (Join(Map("something else", Map("0", Other0)), new byte[] { 8 }), "no list of shortcuts"),
                     (Join(Text("shortcuts", "a text, not a list"), new byte[] { 8 }), "no list of shortcuts"),
                     (new byte[] { 8 }, "no list of shortcuts"),
                     (Join(Enumerable.Repeat(Join(new byte[] { 0 }, Z("m")), 40).ToArray()), "nested too deep"),
                     (Encoding.ASCII.GetBytes("\"shortcuts\"\n{\n}\n"), ""),
                 })
        {
            var e = Throws<SteamException>(() => Steam.With(bad, Exe, Dir, Icon, out _, out _));
            Check(e != null && e.Message.Contains(why) && e.Message.Contains("left it alone"), "a file that " + why + " was not refused for it: " + e?.Message);
            Check(Throws<SteamException>(() => Steam.Without(bad, Exe, out _)) != null && !Steam.Has(bad, Exe), "a file that " + why + " was not refused (removing)");
        }
    }

    static string Root(string name)
    {
        var root = Path.Combine(temp, name);
        Directory.CreateDirectory(Path.Combine(root, "userdata"));
        return root;
    }

    static void Accounts()
    {
        Check(!Steam.Valid(Path.Combine(temp, "nothing here")) && Steam.Users(Path.Combine(temp, "nothing here")).Count == 0, "a folder that is not there is Steam's");
        var root = Root("accounts");
        Check(Steam.Valid(root) && Steam.Users(root).Count == 0, "a Steam nobody signed in to has accounts");
        foreach (var id in new[] { "111", "222", "444", "0", "anonymous", "12345678901" }) Directory.CreateDirectory(Path.Combine(root, "userdata", id, "config"));
        Directory.CreateDirectory(Path.Combine(root, "config"));
        File.WriteAllText(Path.Combine(root, "userdata", "555"), "a file, no account's folder");
        // without Steam's list of who signed in: by when each folder was last written to
        Directory.SetLastWriteTimeUtc(Path.Combine(root, "userdata", "111", "config"), new DateTime(2026, 11, 1, 0, 0, 0, DateTimeKind.Utc));
        Directory.SetLastWriteTimeUtc(Path.Combine(root, "userdata", "222", "config"), new DateTime(2026, 11, 3, 0, 0, 0, DateTimeKind.Utc));
        Directory.SetLastWriteTimeUtc(Path.Combine(root, "userdata", "444", "config"), new DateTime(2026, 11, 2, 0, 0, 0, DateTimeKind.Utc));
        Check(string.Join(",", Steam.Users(root).Select(u => u.Id)) == "222,444,111", "the accounts without Steam's list: " + string.Join(",", Steam.Users(root).Select(u => u.Id)));
        // with it: the one Steam marks as its most recent first, then by when each signed in; an account without a folder is
        // none, and one the list does not name comes last
        File.WriteAllText(Path.Combine(root, "config", "loginusers.vdf"),
            "\"users\"\n{\n\t// a comment\n" +
            "\t\"76561197960265839\"\n\t{\n\t\t\"AccountName\"\t\t\"first\"\n\t\t\"PersonaName\"\t\t\"The \\\"First\\\" One\"\n\t\t\"MostRecent\"\t\t\"0\"\n\t\t\"Timestamp\"\t\t\"1731000000\"\n\t}\n" +
            "\t\"76561197960265950\"\n\t{\n\t\t\"AccountName\"\t\t\"second\"\n\t\t\"PersonaName\"\t\t\"Second {braces}\"\n\t\t\"Nested\"\n\t\t{\n\t\t\t\"PersonaName\"\t\t\"not this\"\n\t\t}\n\t\t\"mostrecent\"\t\t\"1\"\n\t\t\"Timestamp\"\t\t\"1730000000\"\n\t}\n" +
            "\t\"76561197960266061\"\n\t{\n\t\t\"PersonaName\"\t\t\"No folder\"\n\t\t\"MostRecent\"\t\t\"0\"\n\t\t\"Timestamp\"\t\t\"1739999999\"\n\t}\n}\n");
        var users = Steam.Users(root);
        Check(string.Join(",", users.Select(u => u.Id)) == "222,111,444", "the accounts with Steam's list: " + string.Join(",", users.Select(u => u.Id)));
        Check(users[0].Recent && users[0].Name == "Second {braces}" && users[0].Seen == 1730000000 && users[1].Name == "The \"First\" One" && !users[1].Recent && users[2].Name == "", "what Steam's list says of each account: " + users[0].Name + " / " + users[1].Name);
        Check(users[0].Shortcuts == Path.Combine(root, "userdata", "222", "config", "shortcuts.vdf") && users[0].Grid == Path.Combine(root, "userdata", "222", "config", "grid"), "an account's files");
        File.WriteAllText(Path.Combine(root, "config", "loginusers.vdf"), "\"users\" { \"7656119796026\" { \"PersonaName\" \"cut");
        Check(Steam.Users(root).Count == 3, "a damaged list of accounts loses the accounts");
    }

    /// Everything under a folder: each folder, each file with its bytes
    static string Tree(string dir, string? without = null)
    {
        var s = new StringBuilder();
        foreach (var p in Directory.GetFileSystemEntries(dir, "*", SearchOption.AllDirectories).OrderBy(p => p, StringComparer.Ordinal))
        {
            if (without != null && (p + "\\").StartsWith(without + "\\", StringComparison.OrdinalIgnoreCase)) continue;
            s.AppendLine(Directory.Exists(p) ? p + "\\" : p + " " + Hex(File.ReadAllBytes(p)));
        }
        return s.ToString();
    }

    static void OnDisk()
    {
        var root = Root("steam");
        Directory.CreateDirectory(Path.Combine(root, "config"));
        File.WriteAllText(Path.Combine(root, "config", "config.vdf"), "Steam's own settings");
        File.WriteAllText(Path.Combine(root, "steam.exe"), "not a program");
        var fresh = new SteamUser { Root = root, Id = "111" };
        var busy = new SteamUser { Root = root, Id = "222" };
        Directory.CreateDirectory(Path.GetDirectoryName(fresh.Config)!);   // (an account that never opened its library: no config folder yet)
        Directory.CreateDirectory(busy.Grid);
        File.WriteAllText(Path.Combine(busy.Config, "localconfig.vdf"), "the account's own settings");
        File.WriteAllText(Path.Combine(busy.Grid, "2596069104p.png"), "another game's cover");
        var others = File_(new[] { Other0, Other1, Namesake });
        File.WriteAllBytes(busy.Shortcuts, others);
        var pictures = Steam.Pictures.ToDictionary(p => p.Kind, p => Encoding.ASCII.GetBytes("picture: " + p.Kind));
        var game = Path.Combine(temp, "Games", "Triforce", "Triforce.exe");
        string exe = "\"" + game + "\"", dir = "\"" + Path.GetDirectoryName(game) + "\"";
        var id = Steam.Id(exe, "Triforce");
        string Picture(SteamUser u, string kind) => Path.Combine(u.Grid, id + Steam.Pictures.First(p => p.Kind == kind).File);

        // Only an account's config folder is written to: everything else under the test's folder is the same after each change
        T Only<T>(SteamUser user, Func<T> change)
        {
            var before = Tree(temp, user.Config);
            try { return change(); }
            finally
            {
                Check(Tree(temp, user.Config) == before, "a change of an account's shortcuts wrote outside its config folder");
                Check(Directory.GetFiles(user.Config, "*" + Steam.Temp, SearchOption.AllDirectories).Length == 0, "a temporary file was left behind");
            }
        }

        // no shortcuts file yet: made, with the pictures; nothing to keep a copy of
        Check(!Steam.Added(fresh, game), "a shortcut is there before it was added");
        Check(Only(fresh, () => Steam.Add(fresh, game, pictures)), "the shortcut was not new");
        Check(File.ReadAllBytes(fresh.Shortcuts).SequenceEqual(File_(new[] { Ours(exe, dir, Picture(fresh, "icon")) })), "the first shortcuts file of an account is not as Steam writes it");
        Check(Steam.Pictures.All(p => File.ReadAllText(Picture(fresh, p.Kind)) == "picture: " + p.Kind) && Directory.GetFiles(fresh.Grid).Length == 5, "the pictures are not in the grid folder under the shortcut's number");
        Check(!File.Exists(fresh.Shortcuts + Steam.Backup) && !File.Exists(fresh.Shortcuts + Steam.First), "a copy was kept of a file that was not there");
        Check(Steam.Added(fresh, game) && !Steam.Added(busy, game), "which account has the shortcut");
        Check(Steam.Where(fresh, game) == game && Steam.Where(busy, game) == null && Steam.Where(new SteamUser { Root = root, Id = "999" }, game) == null, "what the account's shortcut starts: " + Steam.Where(fresh, game));

        // other shortcuts there: they stay, and the file as it was is kept
        Check(Only(busy, () => Steam.Add(busy, game, pictures)), "the shortcut was not new");
        var with = File_(new[] { Other0, Other1, Namesake, Ours(exe, dir, Picture(busy, "icon")) });
        Check(File.ReadAllBytes(busy.Shortcuts).SequenceEqual(with), "a shortcut added on the disk changed the others");
        Check(File.ReadAllBytes(busy.Shortcuts + Steam.Backup).SequenceEqual(others) && File.ReadAllBytes(busy.Shortcuts + Steam.First).SequenceEqual(others), "the file as it was is not kept");
        Check(File.ReadAllText(Path.Combine(busy.Grid, "2596069104p.png")) == "another game's cover" && File.ReadAllText(Path.Combine(busy.Config, "localconfig.vdf")) == "the account's own settings", "another file of the account changed");

        // again: nothing to do. A picture the player replaced in Steam stays theirs; one that is gone comes back
        File.WriteAllText(Picture(busy, "portrait"), "the player's own cover");
        File.Delete(Picture(busy, "hero"));
        var stamp = File.GetLastWriteTimeUtc(busy.Shortcuts);
        Check(!Only(busy, () => Steam.Add(busy, game, pictures)), "a shortcut that was there counts as new");
        Check(File.ReadAllBytes(busy.Shortcuts).SequenceEqual(with) && File.GetLastWriteTimeUtc(busy.Shortcuts) == stamp, "adding what is there wrote the file again");
        Check(File.ReadAllText(Picture(busy, "portrait")) == "the player's own cover" && File.ReadAllText(Picture(busy, "hero")) == "picture: hero", "the pictures of a shortcut that was there");

        // the game folder moved: the shortcut is set to it; the copy is the file before this change, the first copy the first
        var moved = Path.Combine(temp, "Elsewhere", "Triforce.exe");
        Check(!Only(busy, () => Steam.Add(busy, moved, pictures)), "a shortcut set to a new folder counts as new");
        var movedId = Steam.Id("\"" + moved + "\"", "Triforce");
        Check(File.ReadAllBytes(busy.Shortcuts).SequenceEqual(File_(new[] { Other0, Other1, Namesake, Ours("\"" + moved + "\"", "\"" + Path.GetDirectoryName(moved) + "\"", Path.Combine(busy.Grid, movedId + "_icon.png")) })),
              "the shortcut was not set to the game's new folder");
        Check(File.ReadAllBytes(busy.Shortcuts + Steam.Backup).SequenceEqual(with) && File.ReadAllBytes(busy.Shortcuts + Steam.First).SequenceEqual(others), "the copies after a second change");
        Check(Steam.Where(busy, game) == moved && Steam.Where(busy, moved) == moved, "what a shortcut set to another folder starts: " + Steam.Where(busy, game));
        // (its pictures went along to its new number, the player's own cover too, and none is left under the old one)
        Check(File.ReadAllText(Path.Combine(busy.Grid, movedId + "p.png")) == "the player's own cover" && File.ReadAllText(Path.Combine(busy.Grid, movedId + "_logo.png")) == "picture: logo" &&
              !Steam.Pictures.Any(p => File.Exists(Picture(busy, p.Kind))) && Directory.GetFiles(busy.Grid).Length == 6, "the pictures of a shortcut whose number changed: " + string.Join(" ", Directory.GetFiles(busy.Grid).Select(Path.GetFileName)));

        // while Steam runs: nothing is written, and it says to close Steam
        var asked = "";
        Steam.Running = folder => { asked = folder; return true; };
        foreach (var change in new Func<bool>[] { () => Steam.Add(busy, game, pictures), () => Steam.Remove(busy, moved), () => Steam.Add(fresh, moved, pictures) })
        {
            var before = Tree(temp);
            var e = Throws<SteamException>(() => change());
            Check(e != null && e.Message.StartsWith("Close Steam first") && Tree(temp) == before, "Steam's files were written while Steam runs: " + e?.Message);
        }
        Check(asked == root, "the Steam asked about is not the one written to: " + asked);
        // (Steam started while Setup worked, and wrote its file: Setup's is not put over it)
        var steams = File_(new[] { Other1 });
        var asks = 0;
        Steam.Running = _ => { if (++asks == 2) File.WriteAllBytes(busy.Shortcuts, steams); return false; };
        var late = Throws<SteamException>(() => Steam.Remove(busy, moved));
        Check(late != null && late.Message.Contains("changed while Setup worked") && File.ReadAllBytes(busy.Shortcuts).SequenceEqual(steams), "a file Steam wrote meanwhile was written over: " + late?.Message);
        Steam.Running = _ => false;
        File.WriteAllBytes(busy.Shortcuts, File_(new[] { Other0, Other1, Namesake, Ours("\"" + moved + "\"") }));

        // a file that cannot be read whole: refused, and nothing of the account's folder changes (no picture, no copy)
        foreach (var user in new[] { busy, fresh })
        {
            var good = File.ReadAllBytes(user.Shortcuts);
            File.WriteAllBytes(user.Shortcuts, good.Take(good.Length - 9).ToArray());
            var before = Tree(temp);
            Check(Throws<SteamException>(() => Steam.Add(user, game, pictures)) is { } cut && cut.Message.Contains("left it alone") && Tree(temp) == before, "a file that is cut off was written to (adding)");
            Check(Throws<SteamException>(() => Steam.Remove(user, game)) != null && Tree(temp) == before && !Steam.Added(user, game), "a file that is cut off was written to (removing)");
            File.WriteAllBytes(user.Shortcuts, good);
        }

        // removing: the others' file to the byte, our pictures gone, the others' pictures and the copies there
        File.WriteAllText(Path.Combine(busy.Grid, movedId + "p.png"), "ours, by its number");
        Check(Only(busy, () => Steam.Remove(busy, game)), "the shortcut was not removed");
        Check(File.ReadAllBytes(busy.Shortcuts).SequenceEqual(others), "after adding and removing the shortcuts file is not what it was");
        Check(!File.Exists(Path.Combine(busy.Grid, movedId + "p.png")) && File.Exists(Path.Combine(busy.Grid, "2596069104p.png")) && File.Exists(busy.Shortcuts + Steam.First), "the pictures after removing");
        stamp = File.GetLastWriteTimeUtc(busy.Shortcuts);
        Check(!Only(busy, () => Steam.Remove(busy, game)) && File.GetLastWriteTimeUtc(busy.Shortcuts) == stamp && !Steam.Added(busy, game), "removing what is not there wrote the file");
        Check(Only(fresh, () => Steam.Remove(fresh, game)) && File.ReadAllBytes(fresh.Shortcuts).SequenceEqual(File_(new byte[0][])) && Directory.GetFiles(fresh.Grid).Length == 0, "removing the only shortcut");

        // an account without a folder: said so, nothing made
        var nobody = new SteamUser { Root = root, Id = "999" };
        var tree = Tree(temp);
        Check(Throws<SteamException>(() => Steam.Add(nobody, game, pictures)) != null && !Steam.Remove(nobody, game) && Tree(temp) == tree, "an account without a folder got one");
        // without pictures the shortcut shows the game's own icon
        Steam.Add(fresh, game, new Dictionary<string, byte[]>());
        Check(File.ReadAllBytes(fresh.Shortcuts).SequenceEqual(File_(new[] { Ours(exe, dir, game) })) && Directory.GetFiles(fresh.Grid).Length == 0, "a shortcut without pictures");
        Check(File.ReadAllText(Path.Combine(root, "config", "config.vdf")) == "Steam's own settings" && File.ReadAllText(Path.Combine(root, "steam.exe")) == "not a program", "a file of Steam's own changed");
    }
}
