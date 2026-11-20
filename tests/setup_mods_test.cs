// tests/test_setup_mods.py: setup/Mods.cs (compiled with Guard.cs, no window) on a game folder this program makes in a
// temporary folder, with mods and archives it authors itself. No Setup runs, no game folder of this PC is looked at.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

namespace TriforceSetup;

static class ModsTest
{
    static int checks;
    static string temp = "", game = "", mods = "", outside = "", archives = "", tool = "";

    static void Check(bool ok, string what)
    {
        if (!ok) throw new Exception(what);
        checks++;
    }

    static void Put(string file, string text = "x")
    {
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        File.WriteAllText(file, text);
    }

    static string M(params string[] parts) => Path.Combine(new[] { mods }.Concat(parts).ToArray());

    /// Everything of the test's folder that is not in the mods folder: each folder, each file with its size and time. (Without
    /// the archives, which the test authors as it goes: that an adding leaves its archive alone is checked where one is added.)
    static string Outside()
    {
        var s = new StringBuilder();
        void Look(string dir)
        {
            foreach (var d in Directory.GetDirectories(dir).OrderBy(d => d, StringComparer.Ordinal))
            {
                // (and not the stand-in for tar.exe, which notes in its own folder that it was asked to unpack)
                if (new[] { mods, tool, archives }.Any(own => string.Equals(d, own, StringComparison.OrdinalIgnoreCase))) continue;
                s.AppendLine(d + "\\");
                Look(d);
            }
            foreach (var f in Directory.GetFiles(dir).OrderBy(f => f, StringComparer.Ordinal))
                s.AppendLine(f + "|" + new FileInfo(f).Length + "|" + File.GetLastWriteTimeUtc(f).Ticks);
        }
        Look(temp);
        return s.ToString();
    }

    /// The mods folder itself, the same way (what a refused change must leave as it was)
    static string Inside()
    {
        if (!Directory.Exists(mods)) return "";
        return string.Join("\n", Directory.GetFileSystemEntries(mods, "*", SearchOption.AllDirectories).OrderBy(p => p, StringComparer.Ordinal));
    }

    /// Runs a change of the mods and proves that nothing outside the mods folder was made, changed or removed by it
    static T Only<T>(Func<T> change)
    {
        var before = Outside();
        try { return change(); }
        finally
        {
            var after = Outside();
            if (after != before) throw new Exception("a change of the mods wrote outside the mods folder:\n" + before + "\n-- became --\n" + after);
            checks++;
        }
    }

    /// The sentence a change was refused with; the mods folder must be as it was, and no unpacking folder left
    static string Refused(Func<object> change, string what)
    {
        var before = Inside();
        try { Only(change); }
        catch (ModsException e)
        {
            Check(Inside() == before, what + ": refused, and the mods folder changed all the same (" + e.Message + ")");
            return e.Message;
        }
        throw new Exception(what + ": not refused");
    }

    static string Zip(string name, params (string Name, string Text)[] entries)
    {
        var file = Path.Combine(archives, name);
        if (File.Exists(file)) File.Delete(file);
        // (a name .NET itself will not write, one with a control character: written with a q in that place, which is then
        // put right in the file's bytes, where the name stands twice)
        var swaps = new List<(byte[] From, byte[] To)>();
        using (var z = ZipFile.Open(file, ZipArchiveMode.Create))
            foreach (var (entry, text) in entries)
            {
                var stand = new string(entry.Select(c => c < 32 || "\"<>|".IndexOf(c) >= 0 ? 'q' : c).ToArray());
                if (stand != entry) swaps.Add((Encoding.ASCII.GetBytes(stand), Encoding.ASCII.GetBytes(entry)));
                var e = z.CreateEntry(stand);
                if (entry.EndsWith("/")) continue;
                using var w = new StreamWriter(e.Open(), new UTF8Encoding(false));
                w.Write(text);
            }
        if (swaps.Count > 0)
        {
            var all = File.ReadAllBytes(file);
            foreach (var (from, to) in swaps)
            {
                var places = 0;
                for (var at = 0; (at = Find(all, from, at)) >= 0; at += to.Length, places++) to.CopyTo(all, at);
                Check(places == 2, "(the test) the name was not put into the zip's two lists");
            }
            File.WriteAllBytes(file, all);
        }
        return file;
    }

    static int Find(byte[] all, byte[] what, int from = 0)
    {
        for (var i = from; i + what.Length <= all.Length; i++)
            if (what.Select((b, k) => all[i + k] == b).All(same => same)) return i;
        return -1;
    }

    static List<Mod> Named(params string[] rels)
    {
        var all = Mods.List(mods);
        return rels.Select(r => all.FirstOrDefault(m => m.Rel == r) ?? throw new Exception("no mod " + r + " in the list: " + string.Join(" | ", all.Select(m => m.Rel)))).ToList();
    }

    static Mod One(string rel) => Named(rel)[0];

    static string Order() => string.Join(" | ", Mods.List(mods).Select(m => m.Rel));

    /// The folders of the mods folder the game does not look at: what an adding or a removing made for itself
    static string[] Dots() => Directory.GetDirectories(mods).Where(d => Path.GetFileName(d).StartsWith(".")).ToArray();

    static void Fresh()
    {
        if (Directory.Exists(mods)) Directory.Delete(mods, true);
        Directory.CreateDirectory(mods);
    }

    static int Main()
    {
        temp = Path.Combine(Path.GetTempPath(), "triforce-mods-test-" + Guid.NewGuid().ToString("N"));
        game = Path.Combine(temp, "game");
        mods = Path.Combine(game, "mods");
        outside = Path.Combine(temp, "outside");
        archives = Path.Combine(temp, "archives");
        Directory.CreateDirectory(archives);
        Put(Path.Combine(game, "Triforce.exe"), "not a program");
        Put(Path.Combine(outside, "secret.txt"), "what no mod may touch");
        Put(Path.Combine(temp, "mods2", "romfs", "decoy.bin"));   // (a folder whose name only begins like the mods folder's)
        try
        {
            Names();
            Listing();
            Switching();
            Ordering();
            Building();
            Removing();
            Archives();
            Hostile();
            Tar();
            Check(File.ReadAllText(Path.Combine(outside, "secret.txt")) == "what no mod may touch" && File.Exists(Path.Combine(temp, "mods2", "romfs", "decoy.bin")), "a file outside the mods folder is gone or changed");
        }
        finally
        {
            Mods.GameRuns = _ => false;
            try { Directory.Delete(temp, true); } catch (Exception) { }
        }
        Console.WriteLine("setup_mods_test: " + checks + " checks passed");
        return 0;
    }

    static void Names()
    {
        Check(Mods.Parse("Tall grass") == (false, null, "Tall grass"), "a plain name");
        Check(Mods.Parse("-Tall grass") == (true, null, "Tall grass") && Mods.Parse("_Tall grass") == (true, null, "Tall grass"), "a name switched off");
        Check(Mods.Parse("01-Tall grass") == (false, 1, "Tall grass") && Mods.Parse("-02-Tall grass") == (true, 2, "Tall grass") && Mods.Parse("107-x") == (false, 107, "x"), "a numbered name");
        Check(Mods.Parse("4K-grass") == (false, null, "4K-grass") && Mods.Parse("2024-pack") == (false, null, "2024-pack") && Mods.Parse("1-x") == (false, null, "1-x") && Mods.Parse("01-") == (false, null, "01-"),
              "a name that only begins with digits lost them to a number");
        // (the game's order: no case for A to Z, bytes otherwise)
        Check(Mods.Compare("a", "B") < 0 && Mods.Compare("B", "a") > 0 && Mods.Compare("abc", "ABC") == 0 && Mods.Compare("01-x", "02-a") < 0 && Mods.Compare("9", "a") < 0, "the order of names is not the game's");
        Check(Mods.Compare("Z", "_") > 0 && Mods.Compare("a", "ab") < 0 && Mods.Compare("\u00e9", "z") > 0, "the order of names is not the game's (bytes)");
        Check(Mods.IsDevice("NUL") && Mods.IsDevice("con.txt") && Mods.IsDevice("Com1.tar.gz") && Mods.IsDevice("LPT9") && Mods.IsDevice("aux ") && !Mods.IsDevice("console.txt") && !Mods.IsDevice("COM10") && !Mods.IsDevice("nul2"),
              "the names Windows keeps for devices");
        Check(Mods.Clean("  -_.My: mod?*. ") == "My mod" && Mods.Clean("NUL") == "Mod" && Mods.Clean("...") == "Mod" && Mods.Clean("01-Grass") == "01-Grass" && Mods.Clean(new string('a', 200)).Length == 80, "a mod's name from an archive's");
        Check(Mods.Inside(mods, mods) && Mods.Inside(mods, M("a", "b")) && Mods.Inside(mods, mods.ToUpperInvariant() + "\\x") && !Mods.Inside(mods, mods + "2\\romfs") && !Mods.Inside(mods, M("..", "x")) &&
              !Mods.Inside(mods, game) && !Mods.Inside(mods, @"C:\Windows") && !Mods.Inside(mods, M("a", "..", "..", "x")), "what is inside the mods folder and what is not");
    }

    static void Listing()
    {
        Check(Mods.List(mods).Count == 0 && Mods.Conflicts(Mods.List(mods)).Count == 0, "a game folder without a mods folder has mods");
        Put(M("Grass", "romfs", "Pack", "a.bin"), "grass");
        Put(M("Grass", "romfs", "UI", "B.TXT"), "12345");
        Put(M("Grass", "readme.txt"));
        Put(M("-Off mod", "romfs", "Pack", "a.bin"));
        Put(M("_Under", "romfs", "y.bin"));
        Put(M("02-Second", "romfs", "Pack", "a.bin"));
        Put(M("02-Second", "romfs", "Pack", "only.bin"));
        Put(M("02-Second", "exefs", "E0F3CCFA.pchtxt"));
        Put(M("Cheater", "cheats", "E0F3CCFA.txt"));
        Put(M("Cheater", "cheats", "more", "other.txt"));
        Put(M("contents", Mods.TitleId, "Deep One", "romfs", "pack", "A.BIN"));
        Put(M(".hidden", "romfs", "z.bin"));
        Put(M("NotAMod", "readme.txt"));
        Put(M("-OffGroup", "Inner", "romfs", "q.bin"));
        Put(M("L1", "L2", "L3", "L4", "romfs", "four.bin"));
        Put(M("M1", "M2", "M3", "M4", "M5", "romfs", "five.bin"));
        Put(M("loose.zip"));
        Directory.CreateDirectory(M(Mods.Removed, "Gone", "romfs"));

        Check(Order() == "02-Second | Cheater | contents/" + Mods.TitleId + "/Deep One | Grass | L1/L2/L3/L4 | -Off mod | -OffGroup/Inner | _Under",
              "the mods found, or their order: " + Order());
        var grass = One("Grass");
        Check(grass.On && grass.Name == "Grass" && grass.Number == null && !grass.Deep && grass.Files.SequenceEqual(new[] { "pack/a.bin", "ui/b.txt" }) && grass.Bytes == 10 && !grass.Code, "what Grass carries");
        Check(Mods.Carries(grass) == "2 game files", "Grass in words: " + Mods.Carries(grass));
        var second = One("02-Second");
        Check(second.On && second.Name == "Second" && second.Number == 2 && second.Patches == 1 && second.Cheats == 0 && second.Code, "what 02-Second carries");
        Check(Mods.Carries(second) == "2 game files, 1 code patch file", "02-Second in words: " + Mods.Carries(second));
        var cheater = One("Cheater");
        Check(cheater.On && cheater.Files.Count == 0 && cheater.Cheats == 2 && cheater.Code && Mods.Carries(cheater) == "2 cheat files", "a mod of cheats only: " + Mods.Carries(cheater));
        var deep = One("contents/" + Mods.TitleId + "/Deep One");
        Check(deep.On && deep.Deep && deep.Top == "contents" && deep.Name == "Deep One" && deep.Files.SequenceEqual(new[] { "pack/a.bin" }), "a mod in an emulator's layout");
        Check(!One("-Off mod").On && One("-Off mod").Name == "Off mod" && One("-Off mod").OffAbove == null && !One("_Under").On && One("_Under").Name == "Under", "the mods that are off");
        var inner = One("-OffGroup/Inner");
        Check(!inner.On && inner.OffAbove == "-OffGroup" && inner.Top == "-OffGroup", "a mod inside a folder that is off");
        Check(One("L1/L2/L3/L4").Files.Count == 1, "a mod four folders down");

        // conflicts: among the mods that are on, the last one in the game's order wins
        var all = Mods.List(mods);
        var conflicts = Mods.Conflicts(all);
        Check(conflicts.Count == 1 && conflicts[0].File == "pack/a.bin" && conflicts[0].Winner == all.First(m => m.Rel == "Grass") &&
              string.Join(",", conflicts[0].Mods.Select(m => m.Name)) == "Second,Deep One,Grass", "the file three mods carry, and its winner");
        var shared = Mods.Shared(all.First(m => m.Rel == "Grass"), all, conflicts);
        Check(shared.Count == 2 && shared[0].Other.Name == "Second" && shared[0].Files == 1 && shared[0].Wins && shared[1].Other.Name == "Deep One" && shared[1].Wins, "what Grass shares, and that it wins");
        shared = Mods.Shared(all.First(m => m.Rel == "02-Second"), all, conflicts);
        Check(shared.Count == 2 && !shared[0].Wins && !shared[1].Wins && shared[1].Other.Name == "Grass", "what 02-Second shares, and that it loses");
        Check(Mods.Shared(all.First(m => m.Rel == "Cheater"), all, conflicts).Count == 0 && Mods.Shared(all.First(m => m.Rel == "-Off mod"), all, conflicts).Count == 0, "a mod that is off, or shares nothing, has conflicts");
        var text = Mods.ConflictText(conflicts);
        Check(text.Trim() == "pack/a.bin  <-  Grass wins over 02-Second, contents/" + Mods.TitleId + "/Deep One", "the conflicts as text: " + text);
    }

    static void Switching()
    {
        // off: a "-" in front; the mod keeps its place in the list, and the file it won goes to the next one
        var before = Order();
        var path = Only(() => Mods.SetOn(mods, One("Grass"), false));
        Check(path == M("-Grass") && Directory.Exists(M("-Grass", "romfs", "UI")) && !Directory.Exists(M("Grass")), "Grass was not switched off by its folder's name");
        Check(Order() == before.Replace("| Grass |", "| -Grass |") && !One("-Grass").On, "a mod that was switched off moved in the list: " + Order());
        Check(Mods.Conflicts(Mods.List(mods)).Single().Winner.Name == "Deep One", "with Grass off the file's winner is not the next mod");
        Check(Only(() => Mods.SetOn(mods, One("-Grass"), false)) == M("-Grass"), "switching off what is off renamed it");
        Only(() => Mods.SetOn(mods, One("-Grass"), true));
        Check(Directory.Exists(M("Grass")) && One("Grass").On && Order() == before, "Grass was not switched on again");
        Only(() => Mods.SetOn(mods, One("_Under"), true));
        Check(Directory.Exists(M("Under")) && One("Under").On, "a mod switched off with _ was not switched on");
        Only(() => Mods.SetOn(mods, One("Under"), false));
        Check(Directory.Exists(M("-Under")), "a mod is not switched off with -");
        Only(() => Mods.SetOn(mods, One("02-Second"), false));
        Check(Directory.Exists(M("-02-Second")) && One("-02-Second").Number == 2 && One("-02-Second").Name == "Second", "a numbered mod switched off lost its number or its name");
        Only(() => Mods.SetOn(mods, One("-02-Second"), true));

        // a name that is taken, a folder above that is off, a mod that is gone
        Put(M("Off mod", "romfs", "other.bin"));
        var said = Refused(() => Mods.SetOn(mods, One("-Off mod"), true), "switching on over a folder of that name");
        Check(said.Contains("already a folder named Off mod"), "the refusal does not name the folder in the way: " + said);
        Directory.Delete(M("Off mod"), true);
        said = Refused(() => Mods.SetOn(mods, One("-OffGroup/Inner"), true), "switching on inside a folder that is off");
        Check(said.Contains("-OffGroup"), "the refusal does not name the folder that is off: " + said);
        var gone = One("Cheater");
        Directory.Move(M("Cheater"), M("Cheater moved"));
        Refused(() => Mods.SetOn(mods, gone, false), "switching a mod whose folder is gone");
        Directory.Move(M("Cheater moved"), M("Cheater"));

        // never while the game runs: nothing is renamed, added or removed
        var asked = "";
        Mods.GameRuns = folder => { asked = folder; return true; };
        var zip = Zip("While running.zip", ("romfs/a.bin", "a"));
        foreach (var (what, change) in new (string, Func<object>)[]
                 {
                     ("switched", () => Mods.SetOn(mods, One("Grass"), false)), ("moved", () => Mods.Move(mods, One("Grass"), false)),
                     ("removed", () => Mods.Remove(mods, One("Grass"))), ("added", () => Mods.Add(mods, zip)),
                 })
            Check(Refused(change, "a mod " + what + " while the game runs").StartsWith("Close the game first"), "the refusal while the game runs does not say to close it");
        Check(string.Equals(asked, game, StringComparison.OrdinalIgnoreCase), "the game asked about is not the one of this mods folder: " + asked);
        Mods.GameRuns = _ => false;
    }

    static void Ordering()
    {
        Fresh();
        Put(M("Alpha", "romfs", "f.bin"), "alpha");
        Put(M("Beta", "romfs", "f.bin"), "beta");
        Put(M("Gamma", "romfs", "f.bin"), "gamma");
        Put(M("Gamma", "exefs", "p.ips"));
        Put(M("contents", Mods.TitleId, "Deep", "romfs", "f.bin"), "deep");
        Put(M("NotAMod", "readme.txt"));
        Check(Order() == "Alpha | Beta | contents/" + Mods.TitleId + "/Deep | Gamma" && Mods.Conflicts(Mods.List(mods)).Single().Winner.Name == "Gamma", "before any move: " + Order());
        var code = Mods.CodeState(Mods.List(mods));

        // later: past the next one; every folder of a mod gets its number, each keeps its own name; what is no mod keeps its name
        Check(Only(() => Mods.Move(mods, One("Beta"), true)), "Beta was not moved");
        Check(Order() == "01-Alpha | 02-contents/" + Mods.TitleId + "/Deep | 03-Beta | 04-Gamma", "after Beta moved later: " + Order());
        Check(Directory.Exists(M("NotAMod")) && One("03-Beta").Name == "Beta" && One("03-Beta").Number == 3, "numbering touched what is no mod, or a mod's own name");
        Check(Only(() => Mods.Move(mods, One("04-Gamma"), false)) && Order() == "01-Alpha | 02-contents/" + Mods.TitleId + "/Deep | 03-Gamma | 04-Beta", "after Gamma moved earlier: " + Order());
        Check(Mods.Conflicts(Mods.List(mods)).Single().Winner.Name == "Beta" && File.ReadAllText(M("04-Beta", "romfs", "f.bin")) == "beta", "the mod moved last does not win, or is not the same mod");
        Check(Mods.CodeState(Mods.List(mods)) == code, "numbering the folders anew changed the mark of the mods' code");
        // the ends
        var before = Inside();
        Check(!Only(() => Mods.Move(mods, One("01-Alpha"), false)) && !Only(() => Mods.Move(mods, One("04-Beta"), true)) && Inside() == before, "a mod moved past the end of the list");
        // a mod deeper down moves with the folder of the mods folder it is in
        Check(Only(() => Mods.Move(mods, One("02-contents/" + Mods.TitleId + "/Deep"), false)) && Order() == "01-contents/" + Mods.TitleId + "/Deep | 02-Alpha | 03-Gamma | 04-Beta", "a deep mod moved: " + Order());
        // a mod that is off keeps its switch, and its place is counted
        Only(() => Mods.SetOn(mods, One("03-Gamma"), false));
        Check(Only(() => Mods.Move(mods, One("-03-Gamma"), true)) && Order() == "01-contents/" + Mods.TitleId + "/Deep | 02-Alpha | 03-Beta | -04-Gamma", "a mod that is off moved: " + Order());

        // something in the way of a new name: nothing is renamed
        Put(M("-03-Gamma"));
        var said = Refused(() => Mods.Move(mods, One("-04-Gamma"), false), "a move whose new name is taken");
        Check(said.Contains("-03-Gamma") && said.Contains("Nothing was changed"), "the refusal does not name what is in the way: " + said);
        File.Delete(M("-03-Gamma"));

        // a folder that cannot be renamed (a file in it is open): the folders renamed before it get their old names back
        Fresh();
        Put(M("Alpha", "romfs", "f.bin"));
        Put(M("Beta", "romfs", "f.bin"));
        Put(M("Gamma", "romfs", "f.bin"));
        using (new FileStream(M("Gamma", "romfs", "f.bin"), FileMode.Open, FileAccess.Read, FileShare.None))
        {
            said = Refused(() => Mods.Move(mods, One("Alpha"), true), "a move with a folder that cannot be renamed");
            Check(said.Contains("Nothing was changed") && Order() == "Alpha | Beta | Gamma", "a move that failed halfway left folders renamed: " + Order() + " / " + said);
        }

        // two mods of one name change places: each keeps its files
        Fresh();
        Put(M("01-Same", "romfs", "first.bin"));
        Put(M("02-Same", "romfs", "second.bin"));
        Check(Only(() => Mods.Move(mods, One("01-Same"), true)) && File.Exists(M("01-Same", "romfs", "second.bin")) && File.Exists(M("02-Same", "romfs", "first.bin")), "two mods of one name did not change places");
        Check(Directory.GetDirectories(mods).Length == 2, "changing places left a folder behind: " + Inside());

        // a hundred folders: three digits, and the order holds
        Fresh();
        for (var i = 0; i < 101; i++) Put(M("m" + i.ToString("000"), "romfs", "f.bin"));
        Only(() => Mods.Move(mods, One("m000"), true));
        var order = Mods.List(mods).Select(m => m.Rel).ToList();
        Check(order[0] == "001-m001" && order[1] == "002-m000" && order[100] == "101-m100" && Mods.Parse(order[100]) == (false, 101, "m100"), "a hundred and one mods numbered: " + order[0] + " " + order[1] + " " + order[100]);
    }

    static void Building()
    {
        Fresh();
        Put(M("Grass", "romfs", "a.bin"));
        Put(M("Patch", "exefs", "a.pchtxt"), "one");
        Put(M("Trainer", "cheats", "b.txt"), "one");
        var none = Mods.CodeState(Named("Grass"));
        var both = Mods.CodeState(Mods.List(mods));
        Check(none == "" && both.Length == 16, "the mark of the mods' code: [" + none + "] [" + both + "]");
        Check(!Mods.NeedsBuild("", null) && Mods.NeedsBuild(both, null) && !Mods.NeedsBuild(both, both) && Mods.NeedsBuild("", both) && Mods.NeedsBuild(both, "changed") && Mods.NeedsBuild("", "changed") && !Mods.NeedsBuild("", ""),
              "when the game is to be built again");
        Check(Mods.BuildAgain == "Build again to apply", "the page's words for it");
        // a RomFS mod switched: no build. A mod with code switched, edited, removed or added: a build
        Mods.SetOn(mods, One("Grass"), false);
        Check(Mods.CodeState(Mods.List(mods)) == both, "switching a RomFS mod asks for a build");
        // (cheats are read when the game starts, not built in: a mod of cheats alone asks for no build, switched or edited)
        Check(Mods.CodeState(Named("Trainer")) == "" && Mods.CodeState(Named("Patch")) == both, "a mod of cheats alone is in the mark");
        Mods.SetOn(mods, One("Trainer"), false);
        Check(Mods.CodeState(Mods.List(mods)) == both, "switching off a mod with cheats alone asks for a build");
        Mods.SetOn(mods, One("-Trainer"), true);
        File.WriteAllText(M("Trainer", "cheats", "b.txt"), "two!");
        Check(Mods.CodeState(Mods.List(mods)) == both, "an edited cheat file asks for a build");
        Mods.SetOn(mods, One("Patch"), false);
        Check(Mods.CodeState(Mods.List(mods)) == "", "with the mod with patches off there is still a mark");
        Mods.SetOn(mods, One("-Patch"), true);
        Check(Mods.CodeState(Mods.List(mods)) == both, "the mark is not the same again when the mod is on again");
        File.WriteAllText(M("Patch", "exefs", "a.pchtxt"), "two!");
        Check(Mods.CodeState(Mods.List(mods)) != both, "an edited patch does not ask for a build");
        both = Mods.CodeState(Mods.List(mods));
        Mods.Remove(mods, One("Patch"));
        Check(Mods.CodeState(Mods.List(mods)) != both, "removing a mod with patches does not ask for a build");
        Check(Mods.CodeState(Mods.List(mods)) == "", "with no mod with patches left there is still a mark");
    }

    static void Removing()
    {
        Fresh();
        Put(M("Grass", "romfs", "Pack", "a.bin"), "kept");
        Put(M("-Grass", "romfs", "b.bin"));
        Put(M("contents", Mods.TitleId, "Deep", "romfs", "c.bin"));
        var to = Only(() => Mods.Remove(mods, One("Grass")));
        Check(to == M(Mods.Removed, "Grass") && File.ReadAllText(M(Mods.Removed, "Grass", "romfs", "Pack", "a.bin")) == "kept" && !Directory.Exists(M("Grass")), "a removed mod is not kept in " + Mods.Removed);
        to = Only(() => Mods.Remove(mods, One("-Grass")));
        Check(to == M(Mods.Removed, "Grass (2)") && File.Exists(M(Mods.Removed, "Grass (2)", "romfs", "b.bin")), "a second removed mod of that name went over the first: " + to);
        Only(() => Mods.Remove(mods, One("contents/" + Mods.TitleId + "/Deep")));
        Check(File.Exists(M(Mods.Removed, "Deep", "romfs", "c.bin")) && Mods.List(mods).Count == 0, "what is removed is still listed (the game would load it): " + Order());
    }

    static void Archives()
    {
        Fresh();
        // the mod's parts at the archive's top: its name is the archive's
        var cool = Zip("Cool Mod.zip", ("romfs/Pack/x.bin", "pack"), ("romfs/empty/", ""), ("readme.txt", "read me"));
        var (bytes, time) = (File.ReadAllBytes(cool), File.GetLastWriteTimeUtc(cool));
        var m = Only(() => Mods.Add(mods, cool));
        Check(File.ReadAllBytes(cool).SequenceEqual(bytes) && File.GetLastWriteTimeUtc(cool) == time, "adding a mod changed its archive");
        Check(m.Rel == "Cool Mod" && m.On && m.Files.SequenceEqual(new[] { "pack/x.bin" }) && File.ReadAllText(M("Cool Mod", "romfs", "Pack", "x.bin")) == "pack" &&
              File.Exists(M("Cool Mod", "readme.txt")) && Directory.Exists(M("Cool Mod", "romfs", "empty")), "a zip with romfs at its top");
        // again: a free name, nothing written over
        m = Only(() => Mods.Add(mods, Zip("Cool Mod.zip", ("romfs/Pack/x.bin", "second"))));
        Check(m.Rel == "Cool Mod (2)" && File.ReadAllText(M("Cool Mod", "romfs", "Pack", "x.bin")) == "pack", "the same archive added twice: " + m.Rel);
        // the mod in a folder of its own: that folder's name, and what lies beside it stays out
        m = Only(() => Mods.Add(mods, Zip("download-1234.zip", ("My Mod v2/romfs/a.bin", "a"), ("My Mod v2/exefs/p.ips", "p"), ("My Mod v2/cheats/c.txt", "c"), ("credits.txt", "x"))));
        Check(m.Rel == "My Mod v2" && m.Code && m.Patches == 1 && m.Cheats == 1 && !File.Exists(M("My Mod v2", "credits.txt")) && !File.Exists(M("credits.txt")), "a zip with the mod in its own folder: " + m.Rel);
        // an emulator's layout, with Windows' slashes: the folder that holds romfs is the mod, named after the archive
        m = Only(() => Mods.Add(mods, Zip("Emu layout.zip", ("atmosphere\\contents\\" + Mods.TitleId + "\\romfs\\a.bin", "a"))));
        Check(m.Rel == "Emu layout" && File.Exists(M("Emu layout", "romfs", "a.bin")), "a zip in an emulator's layout: " + m.Rel);
        // cheats alone are a mod
        m = Only(() => Mods.Add(mods, Zip("-_Trainer.zip", ("cheats/E0F3CCFA.txt", "c"))));
        Check(m.Rel == "Trainer" && m.On && m.Cheats == 1, "a zip of cheats, whose name would have switched it off: " + m.Rel);
        Check(Dots().Length == 0, "an adding left its unpacking folder behind: " + Inside());

        // what is no mod, said so, and nothing added
        var before = Inside();
        var said = Refused(() => Mods.Add(mods, Zip("Loose.zip", ("Pack/a.bin", "a"), ("UI/b.bin", "b"))), "a zip without romfs, exefs or cheats");
        Check(said.Contains("no romfs, exefs or cheats folder"), "why a zip of loose files was refused: " + said);
        said = Refused(() => Mods.Add(mods, Zip("Choice.zip", ("Version A/romfs/a.bin", "a"), ("Version B/romfs/a.bin", "b"))), "a zip with two mods");
        Check(said.Contains("2 mods") && said.Contains("Version A") && said.Contains("Version B"), "why a zip of two mods was refused: " + said);
        said = Refused(() => Mods.Add(mods, Zip("Other game.zip", ("contents/0100000000010000/romfs/a.bin", "a"))), "a zip for another game");
        Check(said.Contains("another game") && said.Contains("0100000000010000"), "why a zip for another game was refused: " + said);
        Put(Path.Combine(archives, "notes.txt"), "no archive");
        Check(Refused(() => Mods.Add(mods, Path.Combine(archives, "notes.txt")), "a text file").Contains(".zip, .7z or .rar"), "why a text file was refused");
        Check(Refused(() => Mods.Add(mods, Path.Combine(archives, "missing.zip")), "a file that is not there").Contains("not there"), "why a missing file was refused");
        Put(Path.Combine(archives, "garbage.zip"), "this is no zip file at all, only its name says so");
        Check(Refused(() => Mods.Add(mods, Path.Combine(archives, "garbage.zip")), "a file that is no zip").Contains("not a zip file"), "why a damaged zip was refused");
        Check(Inside() == before, "the refusals left something in the mods folder");

        // where the folders are numbered, a new mod gets the next number and loads last
        Mods.Move(mods, One("Cool Mod"), true);
        var last = Mods.List(mods).Count;
        m = Only(() => Mods.Add(mods, Zip("Aardvark.zip", ("romfs/Pack/x.bin", "wins"))));
        Check(m.Rel == (last + 1).ToString("00") + "-Aardvark" && Mods.List(mods).Last().Rel == m.Rel && Mods.Conflicts(Mods.List(mods)).First(c => c.File == "pack/x.bin").Winner.Name == "Aardvark", "a mod added to numbered folders: " + m.Rel);
        m = Only(() => Mods.Add(mods, Zip("01-First.zip", ("romfs/q.bin", "q"))));
        Check(m.Rel == "01-First", "an archive that brings its own number got a second one: " + m.Rel);

        // an adding that was cut off a day ago left its folder: the next adding takes it away. A newer one stays (it may be at
        // work), and so does a folder that only looks like one
        Put(M(Mods.Adding + "0123abcd", "x", "romfs", "half.bin"));
        Directory.SetLastWriteTimeUtc(M(Mods.Adding + "0123abcd"), DateTime.UtcNow.AddDays(-2));
        Put(M(Mods.Adding + "89abcdef", "x", "half.bin"));
        Put(M(Mods.Adding + "my own folder", "keep.txt"));
        Directory.SetLastWriteTimeUtc(M(Mods.Adding + "my own folder"), DateTime.UtcNow.AddDays(-9));
        Only(() => Mods.Add(mods, Zip("Sweeper.zip", ("romfs/s.bin", "s"))));
        Check(!Directory.Exists(M(Mods.Adding + "0123abcd")) && Directory.Exists(M(Mods.Adding + "89abcdef")) && File.Exists(M(Mods.Adding + "my own folder", "keep.txt")), "the leavings of a cut adding: " + string.Join(" ", Dots()));
    }

    static void Hostile()
    {
        Fresh();
        Put(M("Grass", "romfs", "a.bin"));
        // names that leave the folder, are no file's name, or are another file's stream
        foreach (var (name, why) in new[]
                 {
                     ("../evil.txt", ".."), ("..\\evil.txt", ".."), ("romfs/../../../outside/secret.txt", ".."), ("romfs/a/../../../../evil.txt", ".."), ("/evil.txt", "disk's root"), ("\\evil.txt", "disk's root"),
                     ("\\\\server\\share\\evil.txt", "disk's root"), ("C:/evil.txt", "colon"), ("C:evil.txt", "colon"), ("romfs/a.bin:hidden", "colon"), ("romfs/NUL", "device"), ("romfs/con.txt", "device"),
                     ("COM1/romfs/a.bin", "device"), ("romfs/ends in a dot.", "dot or a space"), ("romfs/ends in a space ", "dot or a space"), ("romfs/a?b.bin", "does not allow"), ("romfs/a\u0001b.bin", "does not allow"),
                     ("romfs/.../x.bin", "dots and spaces"), ("romfs/ . /x.bin", "dots and spaces"),
                 })
        {
            var said = Refused(() => Mods.Add(mods, Zip("hostile.zip", ("romfs/good.bin", "good"), (name, "evil"))), "a zip with the name " + name);
            Check(said.Contains(why) && said.Contains("Nothing was unpacked"), "why a zip with the name " + name + " was refused: " + said);
        }
        Check(Mods.Unsafe("romfs/Pack/a.bin", true) == null && Mods.Unsafe("./romfs/a.bin", true) == null && Mods.Unsafe("My Mod (v1.2)/romfs/\u30e2\u30c3\u30c9.bin", true) == null && Mods.Unsafe("romfs/a?b", false) == null,
              "an honest name was refused");
        Check(Mods.Unsafe(string.Join("/", Enumerable.Repeat("d", Mods.MaxDepth + 1)), true) != null && Mods.Unsafe("", true) != null, "a path too deep, or no name at all, was let through");
        Check(!File.Exists(Path.Combine(temp, "evil.txt")) && !File.Exists(Path.Combine(game, "evil.txt")) && !File.Exists(Path.Combine(Path.GetDirectoryName(temp)!, "evil.txt")) && !File.Exists(M("evil.txt")), "a hostile name was written");

        // a link (as zips made on Linux and macOS mark one), and what is neither file nor folder
        foreach (var (attributes, why) in new[] { (unchecked((int)0xA1FF0000), "a link"), (0x400, "a link"), (0x21A40000, "neither a file nor a folder"), (0x61A40000, "neither a file nor a folder") })
        {
            var file = Path.Combine(archives, "link.zip");
            if (File.Exists(file)) File.Delete(file);
            using (var z = ZipFile.Open(file, ZipArchiveMode.Create))
            {
                using (var w = new StreamWriter(z.CreateEntry("romfs/good.bin").Open())) w.Write("good");
                var e = z.CreateEntry("romfs/link");
                e.ExternalAttributes = attributes;
                using (var w = new StreamWriter(e.Open())) w.Write("../../../outside/secret.txt");
            }
            var said = Refused(() => Mods.Add(mods, file), "a zip with an entry marked " + attributes.ToString("X8"));
            Check(said.Contains(why), "why a zip with an entry marked " + attributes.ToString("X8") + " was refused: " + said);
        }
        // (an ordinary file and folder as Linux marks them pass)
        var unix = Path.Combine(archives, "unix.zip");
        using (var z = ZipFile.Open(unix, ZipArchiveMode.Create))
        {
            z.CreateEntry("romfs/").ExternalAttributes = 0x41ED0010;
            var e = z.CreateEntry("romfs/file.bin");
            e.ExternalAttributes = unchecked((int)0x81A40000);
            using var w = new StreamWriter(e.Open());
            w.Write("fine");
        }
        Check(Only(() => Mods.Add(mods, unix)).Files.Count == 1, "a zip made on Linux was refused");

        // two files of one name (the second would be written over the first, or over a file the list did not show)
        Check(Refused(() => Mods.Add(mods, Zip("twice.zip", ("romfs/a.bin", "1"), ("romfs/A.BIN", "2"))), "a zip with two files of one name").Contains("two files of one name"), "why two files of one name were refused");
        Check(Refused(() => Mods.Add(mods, Zip("twice.zip", ("romfs/a", "1"), ("romfs/a/", ""))), "a zip with a file and a folder of one name").Contains("two files of one name"), "why a file and a folder of one name were refused");
        Refused(() => Mods.Add(mods, Zip("under.zip", ("romfs/a", "1"), ("romfs/a/b.bin", "2"))), "a zip with a file under a file");

        // the bounds: what it says it unpacks to, how many files, and what it really unpacks to
        long bytes = Mods.MaxBytes;
        var files = Mods.MaxFiles;
        Mods.MaxBytes = 1000;
        var big = Zip("big.zip", ("romfs/a.bin", new string('a', 600)), ("romfs/b.bin", new string('b', 600)));
        Check(Refused(() => Mods.Add(mods, big), "a zip that unpacks to more than a mod may take").Contains("unpacks to more than"), "why a zip too large was refused");
        Mods.MaxBytes = 1200;
        Check(Only(() => Mods.Add(mods, big)).Bytes == 1200, "a zip of just the size allowed was refused");
        Mods.MaxBytes = bytes;
        Mods.MaxFiles = 3;
        Check(Refused(() => Mods.Add(mods, Zip("many.zip", ("romfs/1", "1"), ("romfs/2", "2"), ("romfs/3", "3"), ("romfs/4", "4"))), "a zip with more files than a mod may have").Contains("more than the 3"), "why a zip of too many files was refused");
        Mods.MaxFiles = files;
        // (a zip that says 10 bytes and unpacks to 5,000: its list passes, its unpacking is stopped)
        var lying = Zip("lying.zip", ("romfs/bomb.bin", new string('x', 5000)));
        var all = File.ReadAllBytes(lying);
        var central = Find(all, new byte[] { (byte)'P', (byte)'K', 1, 2 });
        Check(central > 0 && BitConverter.ToUInt32(all, central + 24) == 5000, "(the test) the zip's list was not found");
        BitConverter.GetBytes(10u).CopyTo(all, central + 24);
        File.WriteAllBytes(lying, all);
        using (var z = ZipFile.OpenRead(lying)) Check(z.Entries[0].Length == 10, "(the test) the zip does not lie about its size");
        Check(Refused(() => Mods.Add(mods, lying), "a zip that unpacks to more than it says").Contains("more than it says"), "why a zip that lies about its size was refused");

        // no room on the disk: said before the first file, with what it takes and what is free
        var ask = Space.Free;
        var asked = "";
        Space.Free = folder => { asked = folder; return 1 << 20; };
        var none = Refused(() => Mods.Add(mods, big), "a zip there is no room for");
        Check(none.Contains("no room for this mod") && none.Contains("33 MB") && none.Contains(" 1 MB") && Mods.Inside(mods, asked), "the refusal for want of room: " + none + " (asked of " + asked + ")");
        Space.Free = ask;

        Check(Dots().Length == 0, "a refused archive left its unpacking folder behind: " + string.Join(" ", Dots()));
        Check(Order() == "big | Grass | unix", "the refused archives left a mod: " + Order());
    }

    static int RunTool(string exe, string arguments)
    {
        using var p = Process.Start(new ProcessStartInfo(exe, arguments) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true })!;
        p.StandardOutput.ReadToEndAsync();
        p.StandardError.ReadToEndAsync();
        p.WaitForExit();
        return p.ExitCode;
    }

    /// .7z and .rar go through Windows' own tar.exe. Here a stand-in for it (a .cmd file that prints an authored list and
    /// "unpacks" what the test tells it to) shows what Setup makes of tar's answers; then the real one unpacks a .7z it made
    /// itself, where this PC has it
    static void Tar()
    {
        Fresh();
        var real = Mods.Tar;
        tool = Path.Combine(temp, "tool");
        Directory.CreateDirectory(tool);
        var fake = Path.Combine(tool, "tar.cmd");
        File.WriteAllText(fake, "@echo off\r\nif \"%1\"==\"-tf\" type \"%~dp0names.txt\"\r\nif \"%1\"==\"-tvf\" type \"%~dp0long.txt\"\r\nif \"%1\"==\"-xf\" call \"%~dp0unpack.cmd\" %4\r\nexit /b 0\r\n");
        var unpacked = Path.Combine(tool, "unpacked.txt");
        void Answers(string names, string longForm, string unpack)
        {
            File.WriteAllText(Path.Combine(tool, "names.txt"), names.Replace("\n", "\r\n"));
            File.WriteAllText(Path.Combine(tool, "long.txt"), longForm.Replace("\n", "\r\n"));
            File.WriteAllText(Path.Combine(tool, "unpack.cmd"), "@echo off\r\necho x> \"%~dp0unpacked.txt\"\r\n" + unpack.Replace("\n", "\r\n") + "\r\n");
            if (File.Exists(unpacked)) File.Delete(unpacked);
        }
        Put(Path.Combine(archives, "Rar mod.rar"), "not a rar file: the stand-in does not read it");
        Put(Path.Combine(archives, "Seven.7z"), "not a 7z file either");
        string rar = Path.Combine(archives, "Rar mod.rar"), seven = Path.Combine(archives, "Seven.7z");
        const string honestNames = "romfs/\nromfs/Pack/\nromfs/Pack/a.bin\n";
        const string honestLong = "drwxrwxrwx  0 0      0           0 Nov 15 12:00 romfs/\ndrwxrwxrwx  0 0      0           0 Nov 15 12:00 romfs/Pack/\n-rw-rw-rw-  0 0      0           4 Nov 15 12:00 romfs/Pack/a.bin\n";
        const string honestUnpack = "mkdir \"%~1\\romfs\\Pack\"\necho data> \"%~1\\romfs\\Pack\\a.bin\"";

        // no tar on this PC: said plainly, nothing made
        Mods.Tar = () => null;
        foreach (var archive in new[] { rar, seven })
        {
            var said = Refused(() => Mods.Add(mods, archive), "a " + Path.GetExtension(archive) + " file without tar.exe");
            Check(said.Contains("no tar.exe") && said.Contains(Path.GetExtension(archive) + " file yourself"), "what is said without tar.exe: " + said);
        }
        Check(Only(() => Mods.Add(mods, Zip("No tar needed.zip", ("romfs/a.bin", "a")))).Rel == "No tar needed", "a zip needs tar.exe");

        Mods.Tar = () => fake;
        // an honest list: unpacked through tar, checked, moved in
        Answers(honestNames, honestLong, honestUnpack);
        var m = Only(() => Mods.Add(mods, rar));
        Check(m.Rel == "Rar mod" && m.Files.SequenceEqual(new[] { "pack/a.bin" }) && File.Exists(unpacked), "a .rar file through tar: " + m.Rel);
        m = Only(() => Mods.Add(mods, seven));
        Check(m.Rel == "Seven", "a .7z file through tar: " + m.Rel);

        // a list that names a path out of the folder, a device, a link, something else, too much, or cannot be read: tar is
        // never asked to unpack
        foreach (var (names, longForm, why) in new[]
                 {
                     ("romfs/a.bin\n../evil.txt\n", honestLong, ".."),
                     ("romfs/a.bin\nC:\\Windows\\evil.txt\n", honestLong, "colon"),
                     ("romfs/a.bin\n/evil.txt\n", honestLong, "disk's root"),
                     ("romfs/a.bin\nromfs/NUL\n", honestLong, "device"),
                     (honestNames, honestLong.Replace("-rw-rw-rw-", "lrwxrwxrwx"), "a link"),
                     (honestNames, honestLong.Replace("-rw-rw-rw-", "hrw-rw-rw-"), "a link"),
                     (honestNames, honestLong.Replace("-rw-rw-rw-", "crw-rw-rw-"), "neither a file nor a folder"),
                     (honestNames, honestLong.Replace("           4 Nov", " 99999999999999 Nov"), "unpacks to more than"),
                     (honestNames, honestLong.Replace("           4 Nov", "        four Nov"), "cannot read"),
                     (honestNames, "romfs/\nromfs/Pack/\nromfs/Pack/a.bin\n", "cannot read"),
                     (honestNames, honestLong + honestLong, "could not list"),
                 })
        {
            Answers(names, longForm, honestUnpack);
            var said = Refused(() => Mods.Add(mods, seven), "a .7z file whose list says " + why);
            Check(said.Contains(why) && !File.Exists(unpacked), "a .7z file whose list says " + why + ": " + said + (File.Exists(unpacked) ? " (and it was unpacked)" : ""));
        }
        var most = Mods.MaxFiles;
        Mods.MaxFiles = 2;
        Answers(honestNames, honestLong, honestUnpack);
        Check(Refused(() => Mods.Add(mods, seven), "a .7z file with more files than a mod may have").Contains("more than the 2") && !File.Exists(unpacked), "a .7z file of too many files");
        Mods.MaxFiles = most;

        // a list that looked honest and an unpacking that was not: what lies on the disk is checked before any of it moves in.
        // A link to a folder outside (a junction) and a second name of a file outside (a hard link) are refused, and what
        // they point to is still there afterwards, whole
        Put(Path.Combine(outside, "folder", "inner.txt"), "inside the folder a link points to");
        Answers(honestNames, honestLong, honestUnpack + "\nmklink /J \"%~1\\romfs\\way out\" \"" + Path.Combine(outside, "folder") + "\" >nul");
        var link = Refused(() => Mods.Add(mods, seven), "an unpacking that left a link to a folder");
        Check(link.Contains("a link") && link.Contains("way out") && File.Exists(unpacked), "an unpacked link to a folder: " + link);
        Check(File.ReadAllText(Path.Combine(outside, "folder", "inner.txt")) == "inside the folder a link points to", "removing what was unpacked followed a link out of the mods folder");
        Answers(honestNames, honestLong, honestUnpack + "\nmklink /H \"%~1\\romfs\\second name.txt\" \"" + Path.Combine(outside, "secret.txt") + "\" >nul");
        link = Refused(() => Mods.Add(mods, seven), "an unpacking that left a second name of a file outside");
        Check(link.Contains("a link") && link.Contains("second name.txt"), "an unpacked hard link: " + link);
        Check(File.ReadAllText(Path.Combine(outside, "secret.txt")) == "what no mod may touch", "a file outside the mods folder was reached through its second name");
        // more than the list said, a device's name, nothing of a mod
        var bound = Mods.MaxBytes;
        Mods.MaxBytes = 100;
        Answers(honestNames, honestLong, honestUnpack + "\nfor /L %%i in (1,1,40) do echo 0123456789>> \"%~1\\romfs\\large.bin\"");
        Check(Refused(() => Mods.Add(mods, seven), "an unpacking larger than its list").Contains("unpacks to more than"), "an unpacking larger than its list");
        Mods.MaxBytes = bound;
        Answers("Pack/\nPack/a.bin\n", "drwxrwxrwx  0 0      0           0 Nov 15 12:00 Pack/\n-rw-rw-rw-  0 0      0           4 Nov 15 12:00 Pack/a.bin\n", "mkdir \"%~1\\Pack\"\necho data> \"%~1\\Pack\\a.bin\"");
        Check(Refused(() => Mods.Add(mods, seven), "a .7z file without romfs").Contains("no romfs, exefs or cheats folder"), "a .7z file without romfs");
        // tar itself failing
        File.WriteAllText(fake, "@echo off\r\necho tar: Error opening archive: Unrecognized archive format 1>&2\r\nexit /b 1\r\n");
        var failed = Refused(() => Mods.Add(mods, rar), "a .rar file tar cannot read");
        Check(failed.Contains("could not read the .rar file") && failed.Contains("Unrecognized archive format") && failed.Contains("yourself"), "what is said when tar cannot read the file: " + failed);
        Check(Dots().Length == 0, "an archive through tar left its unpacking folder behind");

        // the real tar.exe of this PC, on a .7z it makes itself from authored files
        Mods.Tar = real;
        var tar = real();
        if (tar == null) { Console.WriteLine("NOT RUN: this PC has no System32\\tar.exe: a real .7z file was not unpacked"); return; }
        var source = Path.Combine(temp, "source");
        Put(Path.Combine(source, "Seven Mod", "romfs", "Pack", "real.bin"), "from a real 7z");
        // (plain names only: this PC's tar.exe, bsdtar 3.8.9, ends with an access violation when it is to write a .7z file that
        // holds a name outside ASCII, so no such archive can be authored here)
        Put(Path.Combine(source, "Seven Mod", "romfs", "UI", "second file.txt"), "two");
        Put(Path.Combine(source, "Seven Mod", "exefs", "p.ips"), "p");
        var made = Path.Combine(archives, "real.7z");
        if (RunTool(tar, "--format 7zip -cf \"" + made + "\" -C \"" + source + "\" \"Seven Mod\"") != 0 || !File.Exists(made))
        {
            Console.WriteLine("NOT RUN: this PC's tar.exe writes no .7z files: a real .7z file was not unpacked");
            return;
        }
        var head = File.ReadAllBytes(made).Take(6).ToArray();
        Check(head.SequenceEqual(new byte[] { (byte)'7', (byte)'z', 0xBC, 0xAF, 0x27, 0x1C }), "(the test) tar did not write a 7z file");
        var whole = File.ReadAllBytes(made);
        m = Only(() => Mods.Add(mods, made));
        Check(File.ReadAllBytes(made).SequenceEqual(whole), "adding a mod changed its archive");
        Check(m.Rel == "Seven Mod" && m.Files.Count == 2 && m.Files[0] == "pack/real.bin" && m.Patches == 1 && File.ReadAllText(M("Seven Mod", "romfs", "Pack", "real.bin")) == "from a real 7z",
              "a real .7z file through this PC's tar.exe: " + m.Rel + ", " + Mods.Carries(m));
        Console.WriteLine("a real .7z file was unpacked with " + tar);
    }
}
