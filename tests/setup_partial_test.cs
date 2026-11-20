// tests/test_setup_partial.py: setup/Guard.cs's Partial and SaveLock, setup/Saves.cs's Copy and setup/Setup.cs's Tools (cut out
// of it, the production code) on authored files in a temporary folder. No Setup runs, no game runs, no install is touched.
using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

namespace TriforceSetup;

// (stand-in for Setup.cs's Install: everything is allowed here, the tools go beside the test folder)
static class Install
{
    public static string? InstalledDir = null, TestRoot = null;
    public static string SelfDir = "";
    public static string TestOnly => "a test run";
    public static bool Allowed(string dir) => true;
}

static class PartialTest
{
    static int checks;
    static void Check(bool ok, string what)
    {
        if (!ok) throw new Exception(what);
        checks++;
    }

    static T? Throws<T>(Action a) where T : Exception
    {
        try { a(); } catch (T e) { return e; }
        return null;
    }

    static void Put(string file, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        File.WriteAllText(file, text);
    }

    static void Save(string dir, string tag)
    {
        Put(Path.Combine(dir, "account.sav"), "account " + tag);
        Put(Path.Combine(dir, "option.sav"), "option " + tag);
        Put(Path.Combine(dir, "slot_00", "progress.sav"), "progress " + tag);
    }

    static bool Is(string dir, string tag) =>
        File.Exists(Path.Combine(dir, "account.sav")) && File.ReadAllText(Path.Combine(dir, "account.sav")) == "account " + tag &&
        File.ReadAllText(Path.Combine(dir, "slot_00", "progress.sav")) == "progress " + tag && Directory.GetFileSystemEntries(dir).Length == 3;

    static bool Locked(string file)
    {
        try { using var f = new FileStream(file, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); return false; }
        catch (IOException) { return true; }
    }

    static void Zip(string file, params string[] names)
    {
        if (File.Exists(file)) File.Delete(file);
        using var z = ZipFile.Open(file, ZipArchiveMode.Create);
        foreach (var name in names)
        {
            var e = z.CreateEntry(name);
            using var w = new StreamWriter(e.Open(), new UTF8Encoding(false));
            w.Write("content of " + name);
        }
    }

    static int Main()
    {
        var root = Path.Combine(Path.GetTempPath(), "triforce-partial-" + Guid.NewGuid().ToString("N").Substring(0, 8));
        Directory.CreateDirectory(root);
        try
        {
            // ---- Partial.CopyFile: Setup copying itself into the install folder
            var self = Path.Combine(root, "new-setup.exe");
            var exe = Path.Combine(root, "install", "TriforceSetup.exe");
            Put(self, "the new Setup");
            Put(exe, "the old Setup");
            Partial.Copied = p => { Check(File.Exists(p) && p == exe + Partial.Suffix && File.ReadAllText(exe) == "the old Setup", "the copy was not whole beside the old one before it went in"); throw new IOException("cut off"); };
            Check(Throws<IOException>(() => Partial.CopyFile(self, exe)) != null, "a copy cut off was not said");
            Check(File.ReadAllText(exe) == "the old Setup" && !File.Exists(exe + Partial.Suffix), "a copy cut off left half a Setup, or its partial");
            Partial.Copied = null;
            Partial.CopyFile(self, exe);
            Check(File.ReadAllText(exe) == "the new Setup" && !File.Exists(exe + Partial.Suffix), "the copy did not go into its place");
            Put(exe + Partial.Suffix, "left by a copy before");
            Partial.CopyFile(self, exe);
            Check(File.ReadAllText(exe) == "the new Setup" && !File.Exists(exe + Partial.Suffix), "a partial left before stood in the way");

            // ---- SaveLock.PathOf
            var game = Path.Combine(root, "game");
            var user = Path.Combine(game, "user");
            var tri = Path.Combine(user, "save", Saves.TriforceUid);
            var lockFile = Path.Combine(user, "save.lock");
            Check(SaveLock.PathOf(tri) == lockFile, "Triforce's save folder has another lock than <user>\\save.lock: " + SaveLock.PathOf(tri));
            Check(SaveLock.PathOf(Path.Combine(user, "save-tx", Saves.TriforceUid, "ready-3")) == lockFile, "a commit waiting is not under the save's lock");
            Check(SaveLock.PathOf(Path.Combine(root, "navi", "profile", "save", "00112233445566778899aabbccddeeff")) == Path.Combine(root, "navi", "profile", "instance.lock"), "NX2navi's profile lock is not the one its game holds");
            Check(SaveLock.PathOf(Path.Combine(root, "nand", "user", "save", "0000000000000000", "ABCD", Saves.TitleId)) == null, "an emulator's save folder was given a lock");

            // ---- Saves.Copy: an emulator's save into Triforce's
            var emu = Path.Combine(root, "nand", "user", "save", "0000000000000000", "ABCD", Saves.TitleId);
            var backups = Path.Combine(root, "backups");
            Save(emu, "emulator");
            Save(tri, "triforce");
            // a game that runs holds the lock: refused, nothing changed
            using (var game1 = new FileStream(lockFile, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
            {
                var no = Throws<InvalidOperationException>(() => Saves.Copy(emu, tri, backups, "test"));
                Check(no != null && no.Message == SaveLock.Running, "a copy went on with the game's save lock held");
                Check(Is(tri, "triforce") && !Directory.Exists(tri + Partial.Suffix) && !Directory.Exists(backups), "a refused copy changed something");
                // (TRIFORCE_SAVE_LOCK=0: the look at the processes alone, as before)
                Environment.SetEnvironmentVariable("TRIFORCE_SAVE_LOCK", "0");
                Saves.Copy(emu, tri, backups, "unlocked");
                Environment.SetEnvironmentVariable("TRIFORCE_SAVE_LOCK", null);
                Check(Is(tri, "emulator"), "TRIFORCE_SAVE_LOCK=0 still asked for the lock");
                Save(tri, "triforce");
            }
            // the lock held across the copy, and a copy that fails in the middle leaves the save that was there
            var during = 0;
            Partial.Copied = p =>
            {
                during++;
                Check(Locked(lockFile), "the save's lock is not held during the copy: a game started now would mount half a save");
                Check(Is(p, "emulator") && Is(tri, "triforce"), "the copy was not whole beside the save before it went in");
                throw new IOException("the disk filled");
            };
            Check(Throws<IOException>(() => Saves.Copy(emu, tri, backups, "test")) != null && during == 1, "a copy that failed was not said");
            Check(Is(tri, "triforce") && !Directory.Exists(tri + Partial.Suffix) && !Directory.Exists(tri + Partial.Aside), "a copy that failed left half a save, its partial or the old one aside");
            Check(!Locked(lockFile), "the save's lock was kept after the copy");
            // the new save cannot go into its place (a file in it held open: the folder cannot be moved): the one moved aside comes back
            FileStream? held = null;
            Partial.Copied = p => held = new FileStream(Path.Combine(p, "account.sav"), FileMode.Open, FileAccess.Read, FileShare.Read);
            Check(Throws<Exception>(() => Saves.Copy(emu, tri, backups, "test")) != null, "a save that could not go into its place was not said");
            held?.Dispose();
            Check(Is(tri, "triforce") && !Directory.Exists(tri + Partial.Aside), "the save moved aside did not come back when the new one could not take its place");
            Partial.Remove(tri + Partial.Suffix);
            Partial.Copied = p => Check(Locked(lockFile), "the save's lock is not held during the copy");
            var backup = Saves.Copy(emu, tri, backups, "test");
            Partial.Copied = null;
            Check(Is(tri, "emulator") && backup != null && Is(backup, "triforce"), "the copy, or the backup of the save it replaced, is wrong");
            Check(!Directory.Exists(tri + Partial.Suffix) && !Directory.Exists(tri + Partial.Aside) && !Locked(lockFile), "a copy left its partial, the old save aside or the lock");
            // Setup closed between the two moves: the save moved aside comes back at the next copy's start
            Directory.Move(tri, tri + Partial.Aside);
            Partial.Recover(tri);
            Check(Is(tri, "emulator") && !Directory.Exists(tri + Partial.Aside), "the save moved aside by a copy cut off did not come back");
            // Triforce's save out to the emulator: the game's lock is held for the source too
            Partial.Copied = p => Check(Locked(lockFile), "the game's lock is not held while its save is read");
            Saves.Copy(tri, emu, backups, "out");
            Partial.Copied = null;
            Check(Is(emu, "emulator"), "the save was not copied out");

            // ---- Tools.Unpack: a name Windows would not keep as it is is refused before the first file
            Install.SelfDir = Path.Combine(root, "setup");
            var tools = Tools.Folder;
            var zip = Path.Combine(root, "tools.zip");
            Zip(zip, "tools/nx2/make_game.py", "tools/python/python.exe");
            Tools.Unpack(zip, "v1", _ => { });
            Check(Tools.InstalledVersion == "v1", "the tools were not unpacked");
            foreach (var bad in new[] { "tools/python/NUL", "tools/python/con.txt", "tools/lib/COM3.dll", "tools/x/lpt1", "tools/a/name.", "tools/a/name ", "tools/a:b", "tools/../evil.txt", "tools/a/AUX.h" })
            {
                Zip(zip, "tools/nx2/make_game.py", bad, "tools/z.txt");
                var e = Throws<InvalidDataException>(() => Tools.Unpack(zip, "v2", _ => { }));
                Check(e != null, "a zip with " + bad + " was unpacked");
                Check(Tools.InstalledVersion == "v1" && !Directory.Exists(tools + ".part") && !File.Exists(Path.Combine(tools, "z.txt")), "a refused zip (" + bad + ") changed the tools, or left its part");
            }
            // Setup closed between Unpack's two moves: the tools that were there come back
            Directory.Move(tools, tools + ".old");
            Tools.Sweep();
            Check(Tools.InstalledVersion == "v1" && !Directory.Exists(tools + ".old"), "the tools moved aside by an unpacking cut off did not come back");

            Console.WriteLine("PASS: " + checks + " checks: copies through a partial, the save's lock across a copy, the tools' names");
            return 0;
        }
        catch (Exception e)
        {
            Console.WriteLine("FAIL: " + e.Message);
            return 1;
        }
        finally
        {
            Partial.Copied = null;
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }
}
