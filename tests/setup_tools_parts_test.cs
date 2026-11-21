// tests/test_setup_tools_parts.py: setup/Setup.cs's Tools (cut out of it, the production code) with the tools in two parts:
// the base list read, a base held checked, the tools made again from the code part and the base kept or fetched, and every
// failure leaving the tools that were there whole. Authored files in a temporary folder; no Setup runs, nothing is fetched.
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

static class PartsTest
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
    static string Sha(string text) => Sha(Encoding.UTF8.GetBytes(text));
    static string Sha(byte[] b)
    {
        using var s = System.Security.Cryptography.SHA256.Create();
        return string.Concat(s.ComputeHash(b).Select(x => x.ToString("x2")));
    }
    // (the base's files, as setup/pack_tools.ps1 lists them)
    static readonly (string Path, string Text)[] BaseFiles =
    {
        ("llvm-mingw/bin/clang.exe", "the compiler"), ("llvm-mingw/lib/libc.a", "its library"),
        ("python/python.exe", "the python"), ("python/Lib/os.py", "a module"),
    };
    static string List((string Path, string Text)[] files, string? header = null)
    {
        var rows = files.Select(f => "file\t" + Encoding.UTF8.GetByteCount(f.Text) + "\t" + Sha(f.Text) + "\t" + f.Path)
                        .OrderBy(x => x, StringComparer.Ordinal);
        var body = string.Join("\n", new[] { "dir\tllvm-mingw", "dir\tpython" }.Concat(rows)) + "\n";
        return (header ?? "triforce-tools-base\t1") + "\nid\t" + Sha(body) + "\n" + body;
    }
    static void Zip(string file, params (string Name, string Text)[] entries)
    {
        if (File.Exists(file)) File.Delete(file);
        using var z = ZipFile.Open(file, ZipArchiveMode.Create);
        foreach (var (name, text) in entries)
        {
            using var w = new StreamWriter(z.CreateEntry(name).Open());
            w.Write(text);
        }
    }
    static string Read(string file) => File.Exists(file) ? File.ReadAllText(file) : "";

    static int Main()
    {
        var root = Path.Combine(Path.GetTempPath(), "triforce-parts-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            Install.SelfDir = Path.Combine(root, "setup");
            var tools = Tools.Folder;
            var list = List(BaseFiles);

            // ---- the list: read as written, refused when it is not one
            var b = Tools.ParseBaseList(list);
            Check(b != null && b.Dirs.SequenceEqual(new[] { "llvm-mingw", "python" }) && b.Files.Count == 4 && b.Id.Length == 64, "the base list was not read");
            Check(Tools.ParseBaseList(List(BaseFiles, "triforce-tools-base\t2")) == null, "a list of another format was taken");
            Check(Tools.ParseBaseList(List(new[] { ("../outside.txt", "x") })) == null, "a list with a path out of the folder was taken");
            Check(Tools.ParseBaseList(list + "something\telse\n") == null, "a list with a line it does not know was taken");

            // ---- tools there now: v1 with the base
            void Tools1()
            {
                if (Directory.Exists(tools)) Directory.Delete(tools, true);
                foreach (var (p, t) in BaseFiles) Put(Path.Combine(tools, p), t);
                Put(Path.Combine(tools, "nx2", "make_game.py"), "v1 builder");
                Put(Path.Combine(tools, "nx2", "old_only.py"), "gone in v2");
                Put(Path.Combine(tools, "version.txt"), "v1\n");
            }
            Tools1();
            Check(Tools.HasBase(b!, tools, _ => { }), "the base there was not taken for the list's");
            Check(Read(Path.Combine(tools, Tools.BaseHave)).Trim() == b!.Id, "a base read whole was not remembered (base.have)");
            Check(Tools.HasBase(b, tools, _ => throw new Exception("read again")), "a remembered base was read again");
            File.WriteAllText(Path.Combine(tools, "python", "python.exe"), "the pythoX");   // (same size, other bytes)
            File.Delete(Path.Combine(tools, Tools.BaseHave));
            Check(!Tools.HasBase(b, tools, _ => { }), "a base file with other bytes was taken");
            Tools1();
            File.WriteAllText(Path.Combine(tools, "python", "python.exe"), "longer than it was");
            Check(!Tools.HasBase(b, tools, _ => { }), "a base file of another size was taken");
            Tools1();
            File.Delete(Path.Combine(tools, "llvm-mingw", "lib", "libc.a"));
            Check(!Tools.HasBase(b, tools, _ => { }), "a base with a file gone was taken");

            // ---- the code part alone, the base kept: v2's code, v1's base moved over, v1's other files gone
            Tools1();
            Put(Path.Combine(tools, "python", "Lib", "__pycache__", "os.pyc"), "made by a run");   // (a file the list does not name)
            var code = Path.Combine(root, "code.zip");
            Zip(code, ("tools/nx2/make_game.py", "v2 builder"), ("tools/gpu/new.cpp", "v2 renderer"), ("tools/base.list", list), ("tools/version.txt", "v2\n"));
            Check(Tools.BaseListIn(code)?.Id == b.Id, "the code part's base list was not read");
            Tools.UnpackParts(code, null, b, "v2", _ => { });
            Check(Tools.InstalledVersion == "v2" && Read(Path.Combine(tools, "nx2", "make_game.py")) == "v2 builder" &&
                  Read(Path.Combine(tools, "gpu", "new.cpp")) == "v2 renderer", "the code part was not unpacked");
            Check(!File.Exists(Path.Combine(tools, "nx2", "old_only.py")), "a file of the old code stayed");
            Check(BaseFiles.All(f => Read(Path.Combine(tools, f.Path)) == f.Text) && File.Exists(Path.Combine(tools, "python", "Lib", "__pycache__", "os.pyc")),
                  "the base was not kept whole");
            Check(Read(Path.Combine(tools, Tools.BaseHave)).Trim() == b.Id && !Directory.Exists(tools + ".part") && !Directory.Exists(tools + ".old"),
                  "the new tools do not remember their base, or a partial or old folder was left");

            // ---- the base fetched: tools whose base is another
            Tools1();
            File.WriteAllText(Path.Combine(tools, "python", "python.exe"), "an older python");
            var baseZip = Path.Combine(root, "base.zip");
            Zip(baseZip, BaseFiles.Select(f => ("tools/" + f.Path, f.Text)).ToArray());
            Tools.UnpackParts(code, baseZip, b, "v2", _ => { });
            Check(Tools.InstalledVersion == "v2" && Read(Path.Combine(tools, "python", "python.exe")) == "the python", "the fetched base was not unpacked");

            // ---- failures leave the tools that were there
            Tools1();
            var bad = Path.Combine(root, "bad.zip");
            Zip(bad, ("tools/nx2/make_game.py", "v3"), ("tools/con.txt", "a device's name"));
            Check(Throws<InvalidDataException>(() => Tools.UnpackParts(bad, null, b, "v3", _ => { })) != null, "a code part with a device's name was unpacked");
            Check(Tools.InstalledVersion == "v1" && BaseFiles.All(f => Read(Path.Combine(tools, f.Path)) == f.Text) && !Directory.Exists(tools + ".part"),
                  "a refused code part changed the tools");
            // (the base moved over, then found short of a file the list names: the base folders go back)
            var longer = Tools.ParseBaseList(List(BaseFiles.Concat(new[] { ("python/missing.py", "not there") }).ToArray()))!;
            Check(Throws<InvalidDataException>(() => Tools.UnpackParts(code, null, longer, "v2", _ => { })) != null, "a base short of a listed file was taken");
            Check(Tools.InstalledVersion == "v1" && BaseFiles.All(f => Read(Path.Combine(tools, f.Path)) == f.Text) &&
                  Read(Path.Combine(tools, "nx2", "make_game.py")) == "v1 builder" && !Directory.Exists(tools + ".part"),
                  "the base folders moved over did not go back after a failure");

            // ---- the whole zip (a first install, an older release's way): the base it unpacks is remembered
            if (Directory.Exists(tools)) Directory.Delete(tools, true);
            var whole = Path.Combine(root, "whole.zip");
            Zip(whole, BaseFiles.Select(f => ("tools/" + f.Path, f.Text)).Concat(new[] { ("tools/nx2/make_game.py", "v2 builder"), ("tools/base.list", list) }).ToArray());
            Tools.Unpack(whole, "v2", _ => { });
            Check(Read(Path.Combine(tools, Tools.BaseHave)).Trim() == b.Id, "the whole zip's base was not remembered");
            Check(Tools.HasBase(b, tools, _ => throw new Exception("read again")), "the whole zip's base was read again at the next update");

            Console.WriteLine("PASS: " + checks + " checks: the tools in parts, the base kept or fetched, failures leave the tools there");
            return 0;
        }
        catch (Exception e)
        {
            Console.WriteLine("FAIL: " + e.Message);
            return 1;
        }
        finally
        {
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }
}
