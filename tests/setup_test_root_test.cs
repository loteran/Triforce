// tests/test_setup_test_root.py: setup/Setup.cs's Runner and Install.Allowed (cut out of it, the production code) started with a
// stand-in for the tools' Python: this program itself, which writes down what it was started with and builds nothing. No Setup
// runs, no make_game.py, and no install is touched.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading;

namespace TriforceSetup;

// (stand-ins for what Runner reads of the rest of Setup.cs; Install's Allowed and TestOnly are the real ones, cut in beside these)
static class Program { public const string SetupAsset = "TriforceSetup.exe"; }
sealed class Tools { public string Root = "", Python = ""; }
static partial class Install
{
    public static string? TestRoot = null, InstalledDir = null;
    public static string Self = "";
}

static class TestRootTest
{
    static int checks;
    static void Check(bool ok, string what)
    {
        if (!ok) throw new Exception(what);
        checks++;
    }

    static string dir = "", said = "";

    /// One build as the window starts it: how it ended ("" for well), and the arguments the tools' Python got, null when it was never started
    static (string? Ended, string[]? Got) Build(params string[] args)
    {
        if (File.Exists(said)) File.Delete(said);
        var runner = new Runner(new Tools { Root = dir, Python = Assembly.GetExecutingAssembly().Location }, args.ToList());
        var done = new ManualResetEvent(false);
        string? ended = "never ended";
        runner.Finished += failure => { ended = failure ?? ""; done.Set(); };
        runner.Start();
        if (!done.WaitOne(60000)) throw new Exception("the build's stand-in did not end");
        return (ended, File.Exists(said) ? File.ReadAllLines(said) : null);
    }

    static int Main(string[] args)
    {
        // (the stand-in for python.exe -I nx2\make_game.py ...: what it was started with, one argument a line, and a build's last word)
        if (args.Length > 0 && args[0] == "-I")
        {
            File.WriteAllLines(Environment.GetEnvironmentVariable("SETUP_TEST_SAID")!, args);
            Console.WriteLine("@@done nowhere");
            return 0;
        }

        dir = Path.Combine(Path.GetTempPath(), "triforce-test-root-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        said = Path.Combine(dir, "said.txt");
        Environment.SetEnvironmentVariable("SETUP_TEST_SAID", said);
        Install.Self = Path.Combine(dir, "TriforceSetup.exe");
        var root = Path.Combine(dir, "root");
        try
        {
            // what a test run may write to
            Install.TestRoot = root;
            Check(Install.Allowed(Path.Combine(root, "Games", "Triforce")) && Install.Allowed(root) && Install.Allowed(root.ToUpperInvariant() + @"\x"), "a test run may not write inside its own folder");
            Check(!Install.Allowed(dir) && !Install.Allowed(root + "-beside") && !Install.Allowed(Path.Combine(root, "..", "elsewhere")) && !Install.Allowed(@"C:\Users\you\Games\Triforce"),
                  "a test run may write outside its folder");

            // a test run's build: inside its folder, and in its own build folder
            var (ended, got) = Build("--romfs", @"D:\dump\romfs", "--exefs", @"D:\dump\exefs", "--out", Path.Combine(root, "Games", "Triforce"), "--language", "en-US");
            Check(ended == "" && got != null && got[0] == "-I" && got[1] == Path.Combine(dir, "nx2", "make_game.py"), "(the test) the stand-in for the tools was not started as make_game.py is: " + ended);
            var started = got!;
            var work = Array.IndexOf(started, "--work");
            Check(work > 1 && started[work + 1] == Path.Combine(root, "build") && started.Count(a => a == "--work") == 1,
                  "a test run's build was not given its own build folder: make_game.py would work in the PC's real one and stop the build running there");
            Check(started.Contains("--out") && started[Array.IndexOf(started, "--out") + 1] == Path.Combine(root, "Games", "Triforce"), "the build's own arguments were lost");
            (ended, got) = Build("--update", Path.Combine(root, "Games", "Triforce"));
            Check(ended == "" && got != null && got[Array.IndexOf(got, "--work") + 1] == Path.Combine(root, "build"), "a test run's update was not given its own build folder");
            (ended, got) = Build("--update", Path.Combine(root, "Games", "Triforce"), "--work", Path.Combine(root, "other"));
            Check(got != null && got.Count(a => a == "--work") == 1 && got[Array.IndexOf(got, "--work") + 1] == Path.Combine(root, "other"), "a build folder that was named was replaced or doubled");

            // outside its folder: refused before anything is started
            (ended, got) = Build("--romfs", @"D:\dump\romfs", "--exefs", @"D:\dump\exefs", "--out", @"C:\Users\you\Games\Triforce");
            Check(ended == Install.TestOnly && got == null, "a test run's build into a folder outside its own was started: " + ended);
            (ended, got) = Build("--update", Path.Combine(dir, "game"));
            Check(ended == Install.TestOnly && got == null, "a test run's update of a folder outside its own was started: " + ended);

            // a player's build (no test root): nothing is added
            Install.TestRoot = null;
            (ended, got) = Build("--update", Path.Combine(dir, "game"));
            Check(ended == "" && got != null && !got.Contains("--work"), "a player's build was given a build folder: make_game.py chooses it");
        }
        finally
        {
            Install.TestRoot = null;
            try { Directory.Delete(dir, true); } catch (Exception) { }
        }

        Console.WriteLine("setup_test_root_test: " + checks + " checks passed");
        return 0;
    }
}
