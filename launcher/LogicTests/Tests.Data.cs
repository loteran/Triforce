using System.IO.Compression;
using System.Text;
using Triforce.Logic;
using static Triforce.LogicTests.T;

namespace Triforce.LogicTests;

static partial class Tests
{
    const string LabGame = @"D:\src\lab-game-merge98";

    // ---- saves ----

    static void SavesListing()
    {
        // (READ ONLY: lab-game-merge98 holds real slots. Nothing here copies, locks or writes there)
        var saves = new Saves(Ctx(NewRoot("saves-list")));
        Equal(Path.Combine(LabGame, "user", "save", Saves.TriforceUid), saves.TriforceFolder(LabGame), "Triforce's save folder");
        var slots = saves.Slots(LabGame);
        Equal(6, slots.Count, "six files");
        Check(slots.Count(s => s.Used) >= 1, "slots in use");
        foreach (var s in slots.Where(s => s.Used))
        {
            Check(s.Caption != null && File.Exists(s.Caption), "File " + s.FileNumber + " has its caption.sav");
            var bytes = File.ReadAllBytes(s.Caption!);
            var jpeg = bytes.AsSpan().IndexOf(new byte[] { 0xFF, 0xD8, 0xFF });
            Check(jpeg > 0, "File " + s.FileNumber + "'s caption holds a JPEG (at " + jpeg + ")");
            Check(s.Written != null, "written time");
        }
        var newest = saves.Newest(LabGame)!;
        Check(slots.Where(s => s.Used).All(s => File.GetLastWriteTime(s.Caption!) <= File.GetLastWriteTime(newest.Caption!)), "the newest is the caption written last");
        var set = Saves.Describe("Triforce", saves.TriforceNewest(LabGame));
        Check(!set.Empty && set.Slots == slots.Count(s => s.Used), "described: " + set.Slots + " slots");
    }

    static void MakeSave(string dir, string marker, int slots = 2)
    {
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "option.sav"), marker);
        File.WriteAllText(Path.Combine(dir, "account.sav"), marker);
        for (var i = 0; i < slots; i++)
        {
            Directory.CreateDirectory(Path.Combine(dir, "slot_0" + i));
            File.WriteAllText(Path.Combine(dir, "slot_0" + i, "progress.sav"), marker + i);
            File.WriteAllText(Path.Combine(dir, "slot_0" + i, "caption.sav"), marker + i);
        }
    }

    static async Task SavesCopy()
    {
        var root = NewRoot("saves-copy");
        var ctx = Ctx(root);
        var saves = new Saves(ctx);
        var eden = Path.Combine(root, "Roaming", "eden", "nand", "user", "save", "0000000000000000", "ABCDEF0123456789ABCDEF0123456789", Saves.TitleId);
        MakeSave(eden, "eden", 3);
        var ryu = Path.Combine(root, "Roaming", "Ryujinx", "bis", "user", "save", "0000000000000001", "0");
        MakeSave(ryu, "ryujinx", 1);
        var navi = Path.Combine(root, "Local", "Programs", "NX2navi", "game", "profile", "save", "0123456789abcdef0123456789abcdef");
        MakeSave(navi, "navi", 1);
        var found = saves.FindEmulatorSaves();
        Check(found.Any(f => f.Name == "Eden" && f.Slots == 3), "Eden found: " + string.Join(", ", found.Select(f => f.Name)));
        Check(found.Any(f => f.Name == "Ryujinx") && found.Any(f => f.Name == "NX2navi"), "Ryujinx and NX2navi found");
        Equal(null, saves.RunningGame(), "nothing runs inside the test root");
        var game = Path.Combine(root, "Games", "Triforce");
        MakeSave(saves.TriforceFolder(game), "triforce", 1);
        Directory.CreateDirectory(Path.Combine(game, "user", "save-tx", Saves.TriforceUid, "work"));
        var backup = await saves.ImportAsync(found.First(f => f.Name == "Eden"), game);
        Check(backup != null && File.ReadAllText(Path.Combine(backup, "option.sav")) == "triforce", "Triforce's saves backed up first");
        Check(backup!.StartsWith(ctx.SaveBackupsFolder) && backup.EndsWith("_Triforce-before-Eden"), "the backup's name: " + backup);
        Equal("eden", File.ReadAllText(Path.Combine(saves.TriforceFolder(game), "option.sav")), "Eden's saves in Triforce");
        Equal(3, saves.Slots(game).Count(s => s.Used), "three files now");
        Check(!Directory.Exists(Path.Combine(game, "user", "save-tx", Saves.TriforceUid)), "the old save's transaction removed");
        Check(File.Exists(Path.Combine(game, "user", "save.lock")), "the game's lock was taken across the copy");
        Check(!Directory.Exists(saves.TriforceFolder(game) + ".partial") && !Directory.Exists(saves.TriforceFolder(game) + ".old"), "no partial left");
        // (out: a waiting commit is the newest save)
        var ready = Path.Combine(game, "user", "save-tx", Saves.TriforceUid, "ready-2");
        MakeSave(ready, "ready", 2);
        Equal(ready, saves.TriforceNewest(game), "the waiting commit");
        var target = Saves.Describe("Ryujinx", ryu);
        var b2 = await saves.ExportAsync(game, target);
        Equal("ready", File.ReadAllText(Path.Combine(ryu, "option.sav")), "Triforce's newest out to Ryujinx");
        Check(b2 != null && File.ReadAllText(Path.Combine(b2, "option.sav")) == "ryujinx", "Ryujinx's backed up");
        Equal(Path.Combine(game, "user", "save.lock"), SaveLock.PathOf(ready), "a commit's lock is its user folder's");
        Equal(Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(navi))!, "instance.lock"), SaveLock.PathOf(navi), "NX2navi's lock");
        // (only inside the test root)
        await ThrowsAsync<InvalidOperationException>(() => saves.ExportAsync(game, Saves.Describe("lab", Path.Combine(LabGame, "user", "save", Saves.TriforceUid))), "a test run writes no save outside its root");
        Equal(navi, Saves.FindIn(Path.Combine(root, "Local", "Programs", "NX2navi", "game")), "a chosen NX2navi game folder");
        Equal(eden, Saves.FindIn(Path.Combine(root, "Roaming", "eden")), "a chosen emulator folder, searched down");
    }

    // ---- mods ----

    static string MakeMod(string mods, string folder, params (string Rel, string Text)[] files)
    {
        var dir = Path.Combine(mods, folder);
        foreach (var (rel, text) in files)
        {
            var p = Path.Combine(dir, rel.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(p)!);
            File.WriteAllText(p, text);
        }
        return dir;
    }

    static void ModsFolder()
    {
        var root = NewRoot("mods");
        var ctx = Ctx(root);
        var game = Path.Combine(root, "Games", "Triforce");
        var mods = Mods.FolderOf(game);
        var mm = new ModManager(ctx) { GameRuns = _ => false };
        MakeMod(mods, "Tall grass", ("romfs/Model/Grass.bfres", "a"), ("romfs/UI/x.bntx", "b"));
        MakeMod(mods, "Better HUD", ("romfs/UI/X.bntx", "c"));
        MakeMod(mods, "-Off one", ("romfs/Sound/a.bwav", "d"));
        MakeMod(mods, "_pack/Inner", ("romfs/Event/e.bin", "e"));
        MakeMod(mods, "Code mod", ("exefs/main.ips", "PATCH"), ("cheats/c.txt", "cheat"));
        MakeMod(mods, ".hidden", ("romfs/a", "x"));
        var list = mm.List(mods);
        Equal("Better HUD,Code mod,Off one,Inner,Tall grass", string.Join(",", list.Select(m => m.Name)), "the game's order, switches ignored, dot folders left out");
        Check(!list.First(m => m.Name == "Off one").On && list.First(m => m.Name == "Inner").OffAbove == "_pack", "off, and off by its folder");
        var conflicts = Mods.Conflicts(list);
        Equal(1, conflicts.Count, "one shared file");
        Equal("ui/x.bntx", conflicts[0].File, "keyed in lower case with /");
        Equal("Tall grass", conflicts[0].Winner.Name, "the last one wins");
        Equal("2 game files", Mods.Carries(list.First(m => m.Name == "Tall grass")), "carries");
        Equal("1 code patch file, 1 cheat file", Mods.Carries(list.First(m => m.Name == "Code mod")), "code");
        // (the build mark)
        Check(mm.NeedsRebuild(game), "code patches never built: build again");
        mm.MarkBuilt(game);
        Check(!mm.NeedsRebuild(game), "marked");
        var state = Mods.CodeState(list);
        Equal(16, state.Length, "16 hex digits");
        // (on/off, order, remove)
        var hud = list.First(m => m.Name == "Better HUD");
        var off = mm.SetOn(mods, hud, false);
        Equal(Path.Combine(mods, "-Better HUD"), off, "off is a - in front");
        Throws<ModsException>(() => mm.SetOn(mods, mm.List(mods).First(m => m.Name == "Inner"), true), "a folder above switches it off");
        var code = mm.List(mods).First(m => m.Name == "Code mod");
        mm.SetOn(mods, code, false);
        Check(mm.NeedsRebuild(game), "a code mod switched off: build again");
        Check(mm.Move(mods, mm.List(mods).First(m => m.Name == "Tall grass"), later: false), "moved earlier");
        var tops = Mods.Tops(mm.List(mods));
        Check(tops.All(t => Mods.Parse(t).Number != null), "every folder numbered: " + string.Join(",", tops));
        Check(tops.Contains("-01-Better HUD") || tops.Any(t => t.StartsWith("-0") && t.EndsWith("Better HUD")), "the switch kept in front of the number: " + string.Join(",", tops));
        var order = mm.List(mods).Select(m => m.Name).ToList();
        Check(order.IndexOf("Tall grass") < order.IndexOf("Inner"), "Tall grass passed Inner: " + string.Join(",", order));
        Check(!mm.Move(mods, mm.List(mods)[0], later: false), "the first cannot go earlier");
        var removed = mm.Remove(mods, mm.List(mods).First(m => m.Name == "Tall grass"));
        Check(removed.StartsWith(Path.Combine(mods, Mods.Removed)) && Directory.Exists(removed), "removed to .removed, nothing deleted");
        Check(mm.List(mods).All(m => m.Name != "Tall grass"), "out of the list");
        var busy = new ModManager(ctx) { GameRuns = _ => true };
        Equal("Close the game first. Mods are added, switched, ordered and removed only while it is not running.",
            Throws<ModsException>(() => busy.Remove(mods, busy.List(mods)[0]), "refused while the game runs").Message, "said so");
        // (the romfs file the game reads)
        MakeMod(mods, "99-Sound", ("romfs/Sound/Resource/Stream/SE_Sys_Do00.bwav", "mod's"));
        Directory.CreateDirectory(Path.Combine(game, "romfs", "Sound", "Resource", "Stream"));
        File.WriteAllText(Path.Combine(game, "romfs", "Sound", "Resource", "Stream", "SE_Sys_Cancel00.bwav"), "game's");
        Equal("mod's", File.ReadAllText(mm.RomfsFile(game, "Sound/Resource/Stream/SE_Sys_Do00.bwav")!), "a mod's file wins");
        Equal("game's", File.ReadAllText(mm.RomfsFile(game, "Sound/Resource/Stream/SE_Sys_Cancel00.bwav")!), "else the game's");
        Equal(null, mm.RomfsFile(game, "../secret"), "no way out");
        Equal<(bool, int?, string)>((true, 2, "Grass"), Mods.Parse("-02-Grass"), "parse");
        Equal((false, (int?)null, "4K-grass"), Mods.Parse("4K-grass"), "4K- is a name");
        Check(Mods.Compare("a", "B") < 0 && Mods.Compare("_", "a") < 0, "_stricmp order");
    }

    static byte[] ZipBytes(Action<ZipArchive> fill)
    {
        using var m = new MemoryStream();
        using (var z = new ZipArchive(m, ZipArchiveMode.Create, true)) fill(z);
        return m.ToArray();
    }

    static void Put(ZipArchive z, string name, string text)
    {
        using var w = new StreamWriter(z.CreateEntry(name).Open());
        w.Write(text);
    }

    static void ModsArchives()
    {
        var root = NewRoot("modzip");
        var ctx = Ctx(root);
        var game = Path.Combine(root, "Games", "Triforce");
        var mods = Mods.FolderOf(game);
        var mm = new ModManager(ctx) { GameRuns = _ => false };
        var good = Path.Combine(root, "Cool Link.zip");
        File.WriteAllBytes(good, ZipBytes(z => { Put(z, "contents/0400486024DEC000/romfs/Model/Link.bfres", "link"); Put(z, "readme.txt", "hi"); }));
        var m = mm.Add(mods, good);
        Equal("Cool Link", m.Name, "named after the archive when its folder is generic");
        Equal("model/link.bfres", m.Files.Single(), "its file");
        Check(!Directory.EnumerateDirectories(mods, ".adding-*").Any(), "the temporary folder removed");
        File.WriteAllBytes(good, ZipBytes(z => Put(z, "Hat/romfs/UI/hat.bntx", "hat")));
        Equal("Hat", mm.Add(mods, good).Name, "named after its own folder");
        void Refused(string name, Action<ZipArchive> fill, string words)
        {
            var f = Path.Combine(root, name);
            File.WriteAllBytes(f, ZipBytes(fill));
            var e = Throws<ModsException>(() => mm.Add(mods, f), name);
            Check(e.Message.Contains(words), name + ": " + e.Message);
        }
        Refused("escape.zip", z => Put(z, "../../evil/romfs/x", "x"), "leaves its own folder");
        Refused("device.zip", z => Put(z, "m/romfs/CON.txt", "x"), "device");
        Refused("two.zip", z => { Put(z, "a/romfs/x", "x"); Put(z, "b/romfs/y", "y"); }, "versions to choose from");
        Refused("none.zip", z => Put(z, "just/a/file.txt", "x"), "no romfs, exefs or cheats");
        Refused("other.zip", z => Put(z, "contents/0100000000010000/romfs/x", "x"), "another game");
        Refused("link.zip", z => { var e = z.CreateEntry("m/romfs/link"); e.ExternalAttributes = unchecked((int)(0xA1FFu << 16)); using var w = new StreamWriter(e.Open()); w.Write("/etc/passwd"); }, "link");
        Refused("dup.zip", z => { Put(z, "m/romfs/a", "1"); Put(z, "m/romfs/a", "2"); }, "two files of one name");
        var big = new ModManager(ctx) { GameRuns = _ => false, MaxBytes = 10 };
        File.WriteAllBytes(good, ZipBytes(z => Put(z, "m/romfs/big", new string('x', 100))));
        Check(Throws<ModsException>(() => big.Add(mods, good), "too big").Message.Contains("more than"), "the size bound");
        Throws<ModsException>(() => mm.Add(mods, Path.Combine(root, "x.txt")), "not an archive");
        Equal("Mod", Mods.Clean("..."), "clean: nothing left");
        Equal("Mod", Mods.Clean("NUL"), "clean: a device");
        Equal("Tall grass", Mods.Clean("-_Tall grass. "), "clean: switches and dots");
        Equal(2, mm.List(mods).Count, "only the two good ones added");
    }

    // ---- GameBanana ----

    const string PageJson = """
        {"_aMetadata":{"_nRecordCount":3,"_bIsComplete":true},"_aRecords":[
          {"_idRow":101,"_sModelName":"Mod","_sName":"Hat","_sProfileUrl":"https://gamebanana.com/mods/101","_nLikeCount":5,"_nViewCount":50,
           "_tsDateModified":1700000000,"_aSubmitter":{"_sName":"ann"},"_aRootCategory":{"_sName":"Models"},"_aGame":{"_idRow":26242},
           "_aPreviewMedia":{"_aImages":[{"_sBaseUrl":"https://images.gamebanana.com/img/ss/mods","_sFile":"a.jpg","_sFile530":"530-90_a.jpg"}]}},
          {"_idRow":105323,"_sModelName":"Wip","_sName":"Squadala!","_bHasFiles":true,"_aSubmitter":{"_sName":"daurosk"},"_aRootCategory":{"_sName":"Other"}},
          {"_idRow":7,"_sModelName":"Wip","_sName":"Showcase","_bHasFiles":false},
          {"_idRow":8,"_sModelName":"Request","_sName":"Please"},
          {"_idRow":9,"_sModelName":"Mod","_sName":"Other game","_aGame":{"_idRow":1}}
        ]}
        """;

    static void GameBananaParse()
    {
        var p = GameBanana.ParsePage(PageJson, null, 1, false);
        Equal(2, p.Mods.Count, "a mod and a WiP with files; no showcase, request or other game");
        Equal("https://images.gamebanana.com/img/ss/mods/530-90_a.jpg", p.Mods[0].ThumbUrl, "the 530 picture");
        Check(p.Mods[1].IsKit && p.Mods[1].Wip, "Squadala is the kit");
        Check(p.Complete && p.Total == 3, "metadata");
        Equal("https://gamebanana.com/apiv11/Game/26242/Subfeed?_sSort=default&_csvModelInclusions=Mod,Wip&_nPage=2", GameBanana.ListUrl(2), "the section's URL");
        Equal("https://gamebanana.com/apiv11/Util/Search/Results?_sModelName=Wip&_idGameRow=26242&_sOrder=best_match&_nPage=1&_sSearchString=link%20%26%20zelda%C3%A9",
            GameBanana.SearchUrl(true, 1, "link & zeldaé"), "search URL, UTF-8 escaped");
        // (as the game's own: </p> and <br> each a line, at most one blank line; <li> opens "- " where it stands)
        Equal("Line one\n\nLine two- item\nA & B", GameBanana.Plain("<p>Line one</p><br>Line  two<ul><li>item</li></ul>A &amp; B"), "plain text");
        using var d = System.Text.Json.JsonDocument.Parse("""{"_sText":"<b>Hi</b>","_nDownloadCount":12,"_aFiles":[{"_idRow":1,"_sFile":"x-linux.zip","_sDownloadUrl":"https://gamebanana.com/dl/1","_nFilesize":10,"_tsDateAdded":5},{"_idRow":2,"_sFile":"x-windows.zip","_sDownloadUrl":"https://gamebanana.com/dl/2","_nFilesize":11,"_tsDateAdded":6},{"_idRow":3,"_sFile":"nolink"}]}""");
        var det = GameBanana.ParseDetails(d.RootElement, 105323, true);
        Equal(2, det.Files.Count, "files with a link");
        Equal(1, det.DefaultFile(true), "a kit's Windows file first");
        Equal("Hi", det.Text, "description");
    }

    static async Task GameBananaLive()
    {
        // (one read-only list call to the real API)
        var gb = new GameBanana();
        var page = await gb.ListAsync(1);
        Check(page.Mods.Count > 0 && page.Total > 0, "the section lists mods: " + page.Mods.Count + " of " + page.Total);
        Check(page.Mods.All(m => m.Id > 0 && m.Name.Length > 0), "cards have ids and names");
    }

    static async Task GameBananaInstall()
    {
        using var s = new FakeServer();
        var root = NewRoot("gbinstall");
        var ctx = Ctx(root);
        var game = Path.Combine(root, "Games", "Triforce");
        var mods = Mods.FolderOf(game);
        MakeMod(mods, "01-Old one", ("romfs/a", "a"));
        var mm = new ModManager(ctx) { GameRuns = _ => false };
        var inst = new GameBananaInstaller(ctx, mm);
        s.Add("/dl/1", ZipBytes(z => { Put(z, "Fancy Hat v2/romfs/UI/hat.bntx", "hat"); Put(z, "readme.txt", "r"); }));
        var mod = new GbMod(101, false, "Fancy Hat", "ann", "Models", "https://gamebanana.com/mods/101", 1, 1, 1, null);
        var file = new GbFile(1, "hat.zip", "", s.Url + "/dl/1", 0, 1700000000);
        using var http = Downloader.CreateHttp();
        var msg = await inst.InstallAsync(http, mod, file, game, null, CancellationToken.None);
        Check(msg.Contains("02-Fancy Hat"), "installed after the highest number: " + msg);
        var info = ModManager.ReadInfo(Path.Combine(mods, "02-Fancy Hat"))!;
        Check(info.GameBananaId == 101 && info.Author == "ann" && info.FileDate == 1700000000, "triforce_mod.json");
        Check(inst.Installed(game).Any(i => i.Id == 101), "listed as installed");
        Check(!Directory.Exists(Path.Combine(mods, ".triforce-download")), "the download folder removed");
        // (switched off, then updated: the new one takes its place and its switch)
        mm.SetOn(mods, mm.List(mods).First(m => m.Name == "Fancy Hat"), false);
        s.Add("/dl/2", ZipBytes(z => Put(z, "romfs/UI/hat2.bntx", "hat2")));
        await inst.InstallAsync(http, mod, file with { Url = s.Url + "/dl/2", Added = 1800000000 }, game, null, CancellationToken.None);
        var now = mm.List(mods).Where(m => m.Info?.GameBananaId == 101).ToList();
        Equal(1, now.Count, "one folder for the mod after its update");
        Check(now[0].Folder.StartsWith("_02-") && !now[0].On, "its place and its off kept: " + now[0].Folder);
        Equal("ui/hat2.bntx", now[0].Files.Single(), "the new files");
        s.Add("/dl/3", ZipBytes(z => Put(z, "docs/readme.txt", "x")));
        await ThrowsAsync<ModsException>(() => inst.InstallAsync(http, mod with { Id = 5 }, file with { Url = s.Url + "/dl/3" }, game, null, CancellationToken.None), "no romfs: refused");
        // (bare game folders, no romfs above them: wrapped)
        s.Add("/dl/4", ZipBytes(z => Put(z, "MyPack/Model/x.bfres", "m")));
        await inst.InstallAsync(http, mod with { Id = 6, Name = "Bare" }, file with { Url = s.Url + "/dl/4" }, game, null, CancellationToken.None);
        Check(mm.List(mods).Any(m => m.Name == "Bare" && m.Files.Contains("model/x.bfres")), "wrapped in a romfs");
    }

    // ---- Linux update logic (tests/test_linux_update.py's cases) ----

    static void LinuxLogic()
    {
        string M(string? g, string t, string? l, bool p, bool k) => LinuxUpdate.ChooseMode(g, t, l, p, k);
        Equal("update-game", M("v1", "v2", "v2", false, true), "1");
        Equal("up-to-date", M("v2", "v2", "v2", false, true), "2");
        Equal("up-to-date", M("v1", "v1", null, false, true), "3");
        Equal("update-game", M("v1", "v1", "v2", false, true), "4");
        Equal("update-builder", M(null, "v1", "v2", false, false), "5");
        Equal("build", M(null, "v2", "v2", false, false), "6");
        Equal("build", M("v2", "v2", "v2", true, true), "7");
        var ci = "v2.0.5-3-g1a2b3c4";
        Equal("build", M(null, ci, "v2.0.5", false, false), "an Actions build is not taken back");
        Equal("build", M(null, "1a2b3c4", "v2.0.5", false, false), "a bare commit");
        Equal("build", M(null, "dev", "v2.0.5", false, false), "dev");
        Equal("build", M(null, "v2.0.6", "v2.0.5", false, false), "newer");
        Equal("build", M(null, "v2.1.0-rc.1", "v2.0.5", false, false), "an rc after");
        Equal("up-to-date", M(ci, ci, "v2.0.5", false, true), "its own game");
        Equal("update-game", M("v2.0.4", ci, "v2.0.5", false, true), "an older game");
        Equal(ci, LinuxUpdate.UpdateTarget("v2.0.5", ci), "target stays");
        Equal("update-game", M("v2.0.5", ci, "v2.0.5", false, true), "the release's game is older than this builder");
        Equal("update-builder", M(null, "v2.0.4", "v2.0.5", false, false), "an older release");
        Equal("update-builder", M(null, "v2.1.0-rc.1", "v2.1.0", false, false), "rc before its release");
        Equal("build", M(null, "v2.0.10", "v2.0.9", false, false), "numbers, not text");
        Equal("update-builder", M(null, "v2.0.4-3-g1a2b3c4", "v2.0.5", false, false), "after an older release");
        Equal("build", M(null, "v2.0.5-3-g1a2b3c4-dirty", "v2.0.5", false, false), "dirty after");
        Equal("update-builder", M(null, "v2.1.0-rc.1-2-gabc1234", "v2.1.0", false, false), "rc commits before the release");
        var sha = new string('a', 64);
        var list = LinuxUpdate.ParseList("# x\ntag v0.5.2\npart " + sha + " 10 usr/share/hookshot https://x/a.zip\ntree\npart " + sha + " 5 usr/lib/hookshot https://x/b.zip\nfile hookshot_vk.so\nfile libSDL2-2.0.so.0\n", local: false);
        Equal("v0.5.2", list.Tag, "tag");
        Check(list.Parts[0].Files == null && list.Parts[1].Files!.Count == 2, "tree and files");
        Throws<FormatException>(() => LinuxUpdate.ParseList("tag v1\npart " + sha + " 1 a file:///x\ntree\n", local: false), "file:// only in a local list");
        Throws<FormatException>(() => LinuxUpdate.ParseList("tag ../x\npart " + sha + " 1 a https://x\ntree\n"), "a tag that is a path");
        Throws<FormatException>(() => LinuxUpdate.ParseList("tag v1\npart " + sha + " 1 ../a https://x\ntree\n"), "a part out of its folder");
        Throws<FormatException>(() => LinuxUpdate.ParseList("tag v1\nwhat\n"), "unknown line");
        Equal("\"/home/a/100%% Games/\\\\\\\\x\\\\$y\"", LinuxPlatform.DesktopExec("/home/a/100% Games/\\x$y"), "desktop Exec escaping");
    }

    // ---- Steam ----

    static void SteamShortcut()
    {
        var root = NewRoot("steam");
        var ctx = Ctx(root);
        var steamRoot = Path.Combine(root, "Steam");
        Directory.CreateDirectory(Path.Combine(steamRoot, "userdata", "1001", "config"));
        Directory.CreateDirectory(Path.Combine(steamRoot, "userdata", "2002", "config"));
        Directory.CreateDirectory(Path.Combine(steamRoot, "config"));
        File.WriteAllText(Path.Combine(steamRoot, "config", "loginusers.vdf"),
            "\"users\"\n{\n\t\"" + (76561197960265728L + 2002) + "\"\n\t{\n\t\t\"PersonaName\"\t\t\"Link\"\n\t\t\"MostRecent\"\t\t\"1\"\n\t}\n}\n");
        var lib = new SteamLibrary(ctx) { Running = _ => false };
        Equal(steamRoot, lib.Root(), "the test root's Steam only");
        var user = lib.User()!;
        Check(user.Id == "2002" && user.Name == "Link", "the most recent account");
        // (another shortcut there first, kept byte for byte)
        var other = SteamVdf.With(Array.Empty<byte>(), "\"C:\\Other\\other.exe\"", "\"C:\\Other\"", "", out _, out _);
        var otherBytes = other.ToArray();
        // (named Triforce but not ours: its program is no game exe, and its number is another)
        File.WriteAllBytes(user.Shortcuts, otherBytes);
        var before = File.ReadAllBytes(user.Shortcuts);
        var game = Path.Combine(root, "Games", "Triforce");
        Directory.CreateDirectory(game);
        var exe = Path.Combine(game, "Triforce.exe");
        File.WriteAllText(exe, "");
        var pics = SteamVdf.Pictures.ToDictionary(p => p.Kind, p => new byte[] { 0x89, (byte)'P', (byte)'N', (byte)'G' });
        Check(lib.Add(user, exe, pics), "added");
        Check(lib.Added(user, exe), "there");
        Equal(exe, lib.Where(user, exe), "it starts the game");
        var id = SteamVdf.Id("\"" + exe + "\"", "Triforce");
        Check(id >= 0x80000000u, "the top bit set");
        Check(File.Exists(Path.Combine(user.Grid, id + "p.png")) && File.Exists(Path.Combine(user.Grid, id + "_icon.png")), "pictures under the id");
        Check(File.ReadAllBytes(user.Shortcuts + SteamLibrary.First).SequenceEqual(before), "the file before the first change kept");
        // (the game folder moved: the shortcut set to it, its pictures moved along)
        var moved = Path.Combine(root, "Games", "Moved");
        Directory.CreateDirectory(moved);
        var exe2 = Path.Combine(moved, "Triforce.exe");
        File.WriteAllText(exe2, "");
        Equal(exe, lib.Where(user, exe2), "the old shortcut found by its name and program");
        Check(!lib.Add(user, exe2, pics), "set, not added");
        var id2 = SteamVdf.Id("\"" + exe2 + "\"", "Triforce");
        Check(File.Exists(Path.Combine(user.Grid, id2 + "p.png")) && !File.Exists(Path.Combine(user.Grid, id + "p.png")), "pictures moved to the new id");
        var busy = new SteamLibrary(ctx) { Running = _ => true };
        Equal(SteamLibrary.CloseFirst, Throws<SteamException>(() => busy.Remove(user, exe2), "Steam running").Message, "close Steam first");
        Check(lib.Remove(user, exe2), "removed");
        Check(!lib.Added(user, exe2), "gone");
        Check(File.ReadAllBytes(user.Shortcuts).SequenceEqual(before), "the file as it was before");
        File.WriteAllBytes(user.Shortcuts, new byte[] { 0, 1, 2 });
        Check(Throws<SteamException>(() => lib.Add(user, exe, pics), "a broken file").Message.Contains("left it alone"), "left alone");
    }
}
