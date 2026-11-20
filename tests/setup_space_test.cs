// tests/test_setup_space.py: setup/Guard.cs's Space, and setup/Setup.cs's Tools (cut out of it, the production code) unpacking
// authored zips into a temporary folder. No Setup runs and no install is touched.
using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

namespace TriforceSetup;

// (stand-in for Setup.cs's Install: the folder the tools go beside)
static class Install
{
    public static string? InstalledDir = null, TestRoot = null;
    public static string SelfDir = "";
}

static class SpaceTest
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

    static void Zip(string file, params (string Name, string Text)[] entries)
    {
        if (File.Exists(file)) File.Delete(file);
        using var z = ZipFile.Open(file, ZipArchiveMode.Create);
        foreach (var (name, text) in entries)
        {
            var e = z.CreateEntry(name);
            if (name.EndsWith("/")) continue;
            using var w = new StreamWriter(e.Open(), new UTF8Encoding(false));
            w.Write(text);
        }
    }

    static int Find(byte[] all, byte[] what, int from)
    {
        for (var i = from; i + what.Length <= all.Length; i++)
            if (what.Select((b, k) => all[i + k] == b).All(same => same)) return i;
        return -1;
    }

    static int Main()
    {
        const long MB = 1L << 20;

        // the question itself
        Check(Space.Problem("the tools", 100 * MB, 100 * MB + Space.Margin, @"C:\") == null, "room for the file and the margin was refused");
        Check(Space.Problem("the tools", 100 * MB, null, @"C:\") == null, "a disk that cannot be asked was taken for full");
        Check(Space.Problem("the tools", -5, Space.Margin, @"C:\") == null, "nothing more to write, and still no room");
        var said = Space.Problem("the tools", 100 * MB, 100 * MB + Space.Margin - 1, @"D:\");
        Check(said != null && said.Contains("the tools") && said.Contains(@"D:\"), "the sentence does not say what there is no room for, or where: " + said);
        Check(said!.Contains("132 MB") && said.Contains("131 MB"), "the sentence does not say how much it takes and how much is free: " + said);
        said = Space.Problem("x", 1, 0, @"C:\");
        Check(said != null && said.Contains("33 MB") && said.Contains(" 0 MB"), "a full disk: " + said);

        // the disk asked
        var temp = Path.Combine(Path.GetTempPath(), "triforce-space-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        try
        {
            Check(Space.Free(temp) is > 0, "the free space of a folder that is there was not found");
            Check(Space.Free(Path.Combine(temp, "not", "made", "yet")) is > 0, "the free space for a folder still to make was not found");
            Check(Space.Free("") == null, "no folder at all has free space");
            var unused = "ZYXWVUTSRQPONMLKJIHGFED".First(c => !DriveInfo.GetDrives().Any(d => char.ToUpperInvariant(d.Name[0]) == c));
            Check(Space.Free(unused + @":\Games\Triforce") == null, "a drive that is not there has free space");
            Check(Space.Disk(@"c:\Users\you\Games\Triforce") == @"c:\" && Space.Disk(@"\\server\share\games") == @"\\server\share", "the disk's name");

            // a disk that filled all the same
            Check(Space.Full(new IOException("full", unchecked((int)0x80070070))) && Space.Full(new IOException("full", unchecked((int)0x80070027))), "a full disk's error is not known");
            Check(Space.Full(new InvalidOperationException("outer", new IOException("full", unchecked((int)0x80070070)))), "a full disk's error inside another is not known");
            Check(!Space.Full(new IOException("in use", unchecked((int)0x80070020))) && !Space.Full(new Exception("x")) && !Space.Full(null), "another error is taken for a full disk");

            // the tools unpacked (Setup.cs's Tools): whole, or not at all
            Install.InstalledDir = temp;
            var tools = Path.Combine(temp, "tools");
            var zip = Path.Combine(temp, "tools.zip");
            // ((Triforce) a name that leaves the folder: the zip is refused whole before its first file (it was skipped, the rest written))
            Zip(zip, ("tools/nx2/make_game.py", "print('one')"), ("tools/../../outside.txt", "must not be written"));
            Check(Throws<InvalidDataException>(() => Tools.Unpack(zip, "v0.9.0", _ => { })) != null && !Directory.Exists(tools) && !Directory.Exists(tools + ".part"),
                  "a zip with a name out of the folder was unpacked");
            Zip(zip, ("tools/nx2/make_game.py", "print('one')"), ("tools/python/python.exe", "not a program"), ("tools/empty/", ""),
                ("tools/deep/er/file.txt", "deep"));
            double last = -1;
            Tools.Unpack(zip, "v1.0.0", f => last = f);
            Check(File.ReadAllText(Path.Combine(tools, "nx2", "make_game.py")) == "print('one')" && File.ReadAllText(Path.Combine(tools, "deep", "er", "file.txt")) == "deep" &&
                  Directory.Exists(Path.Combine(tools, "empty")), "the tools were not unpacked whole");
            Check(Tools.InstalledVersion == "v1.0.0" && Tools.Find()?.Python == Path.Combine(tools, "python", "python.exe") && last == 1, "the unpacked tools are not found, or not as that release's");
            Check(!File.Exists(Path.Combine(Path.GetDirectoryName(temp)!, "outside.txt")) && !File.Exists(Path.Combine(temp, "outside.txt")), "a name in the zip wrote outside the tools folder");
            Check(!Directory.Exists(tools + ".part") && !Directory.Exists(tools + ".old"), "an unpacking that ended well left a folder behind");

            // no room: said before the first file, and the tools there stay
            Zip(zip, ("tools/nx2/make_game.py", "print('two')"), ("tools/big.bin", new string('x', 3 << 20)));
            var ask = Space.Free;
            var asked = "";
            Space.Free = folder => { asked = folder; return 2 * MB + Space.Margin; };
            var none = Throws<NoRoomException>(() => Tools.Unpack(zip, "v2.0.0", _ => { }));
            Check(none != null && none.Message.Contains("the Triforce tools") && none.Message.Contains("36 MB") && none.Message.Contains("34 MB"),
                  "three megabytes were unpacked into two, or the refusal does not say what it takes and what is free: " + none?.Message);
            Check(asked.StartsWith(temp, StringComparison.OrdinalIgnoreCase), "the room was asked of another folder than the tools go to: " + asked);
            Check(Tools.InstalledVersion == "v1.0.0" && File.ReadAllText(Path.Combine(tools, "nx2", "make_game.py")) == "print('one')", "a refused unpacking changed the tools that were there");
            Check(!Directory.Exists(tools + ".part"), "a refused unpacking left a folder behind");
            // (and with the room, three megabytes and a cluster for each file, it goes through)
            Space.Free = _ => (3 << 20) + 20 + 2 * 4096 + Space.Margin;
            Tools.Unpack(zip, "v2.0.0", _ => { });
            Check(Tools.InstalledVersion == "v2.0.0" && File.ReadAllText(Path.Combine(tools, "nx2", "make_game.py")) == "print('two')", "with just the room it takes, the tools were not unpacked");
            Space.Free = ask;

            // a zip that is damaged halfway: the half that was unpacked goes, the tools there stay
            Zip(zip, ("tools/nx2/make_game.py", "print('three')"), ("tools/second.txt", string.Concat(Enumerable.Repeat("the second file. ", 400))));
            var all = File.ReadAllBytes(zip);
            var header = Find(all, new byte[] { (byte)'P', (byte)'K', 3, 4 }, 4);
            Check(header > 0 && Encoding.ASCII.GetString(all, header + 30, 16) == "tools/second.txt", "(the test) the second file's place in the zip was not found");
            var data = header + 30 + BitConverter.ToUInt16(all, header + 26) + BitConverter.ToUInt16(all, header + 28);
            for (var i = 0; i < 4; i++) all[data + i] = 0xFF;   // (no deflate block starts like this)
            File.WriteAllBytes(zip, all);
            Check(Throws<Exception>(() => Tools.Unpack(zip, "v3.0.0", _ => { })) is { } bad && bad is not NoRoomException, "a damaged zip was unpacked without a word");
            Check(!Directory.Exists(tools + ".part"), "half a tools folder was left behind by an unpacking that failed");
            Check(Tools.InstalledVersion == "v2.0.0" && File.ReadAllText(Path.Combine(tools, "nx2", "make_game.py")) == "print('two')", "an unpacking that failed changed the tools that were there");

            // what an unpacking that was cut off left behind
            Directory.CreateDirectory(Path.Combine(tools + ".part", "nx2"));
            File.WriteAllText(Path.Combine(tools + ".part", "nx2", "half.py"), "half");
            Directory.CreateDirectory(tools + ".old");
            Tools.Sweep();
            Check(!Directory.Exists(tools + ".part") && !Directory.Exists(tools + ".old") && Tools.InstalledVersion == "v2.0.0", "the leavings of a cut unpacking stay, or the tools went with them");
            Directory.Move(tools, tools + ".old");   // (cut between the two renames: the old folder holds the only tools)
            Tools.Sweep();
            // ((Triforce) and they are put back: the rollback point of an unpacking cut off)
            Check(!Directory.Exists(tools + ".old") && Tools.InstalledVersion == "v2.0.0", "the only tools there are were swept away, or not put back");
        }
        finally
        {
            try { Directory.Delete(temp, true); } catch (Exception) { }
        }

        Console.WriteLine("setup_space_test: " + checks + " checks passed");
        return 0;
    }
}
