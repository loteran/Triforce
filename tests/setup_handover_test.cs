// tests/test_setup_handover.py: setup/Guard.cs's Handover with real processes (this program, started again as "the new Setup") and
// real files in a temporary folder, one of them a running program: renaming it aside, putting it back over the one that did not
// start. No Setup runs and no install is touched.
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading;

namespace TriforceSetup;

static class HandoverTest
{
    static int checks;
    static void Check(bool ok, string what)
    {
        if (!ok) throw new Exception(what);
        checks++;
    }

    static Process Start(string exe, string args) =>
        Process.Start(new ProcessStartInfo(exe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true })!;

    static bool Throws(Action a)
    {
        try { a(); } catch (Exception) { return true; }
        return false;
    }

    static int Main(string[] args)
    {
        if (args.Length > 0)
        {
            // (a new Setup, as the old one sees it)
            switch (args[0])
            {
                case "up": Handover.Up(args[1], args.Length > 2 ? args[2] : null); Thread.Sleep(1500); return 0;        // (its window comes up)
                case "up-then-crash": Handover.Up(args[1], null); return 7;                                             // (and it fails later)
                case "crash": return int.Parse(args[1]);                                                                 // (it ends at once)
                case "run": Console.WriteLine("running"); Console.Out.Flush(); Thread.Sleep(Timeout.Infinite); return 0; // (it runs, and gives no sign)
            }
            return 2;
        }

        var me = Assembly.GetExecutingAssembly().Location;
        var dir = Path.Combine(Path.GetTempPath(), "triforce-handover-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var folder = Path.Combine(dir, "install");
        try
        {
            // what became of the new Setup
            Handover.Outcome Watched(string run, double seconds = 20, string? root = null)
            {
                using var up = Handover.Expect(folder, root);
                using var p = Start(me, run);
                var o = Handover.Watch(p, up, TimeSpan.FromSeconds(seconds));
                if (!p.HasExited) p.Kill();
                p.WaitForExit();
                return o;
            }
            Check(Watched("up \"" + folder + "\"") == Handover.Outcome.Up, "a new Setup whose window came up was not heard");
            Check(Watched("up-then-crash \"" + folder + "\"") == Handover.Outcome.Up, "a new Setup that came up and failed later was taken back");
            var watch = Stopwatch.StartNew();
            Check(Watched("crash 7") == Handover.Outcome.Failed && watch.ElapsedMilliseconds < 10000, "a new Setup that ended at once with an error was not noticed, or not at once");
            Check(Watched("crash -532462766") == Handover.Outcome.Failed, "a new Setup that crashed (.NET's exit code for an unhandled error) was not noticed");
            Check(Watched("crash 0") == Handover.Outcome.Unclear, "a Setup that ended without an error (a second one that found another open) was taken for a failed one");
            watch.Restart();
            Check(Watched("run", 1.5) == Handover.Outcome.Unclear && watch.ElapsedMilliseconds >= 1400, "a new Setup that runs without the sign was not left alone, or not waited for");
            Check(Watched("up \"" + folder + "-other\"", 3) == Handover.Outcome.Unclear, "the sign of another install's Setup was taken for this one's");
            Check(Watched("up \"" + folder + "\"", 3, Path.Combine(dir, "root")) == Handover.Outcome.Unclear, "the sign of the real install's Setup was heard by a test run's (TRIFORCE_TEST_ROOT)");
            Check(Watched("up \"" + folder + "\" \"" + Path.Combine(dir, "root") + "\"", 20, Path.Combine(dir, "root")) == Handover.Outcome.Up, "a test run's new Setup was not heard by its old one");
            Handover.Up(folder, null);   // (nobody waits: an older Setup started this one)

            // the swap, with plain files
            // (stand-ins for TriforceSetup.exe, .new.exe and .old.exe: no file here is Setup, the running one is this test program)
            string self = Path.Combine(dir, "stand-in.exe"), fresh = Path.Combine(dir, "stand-in.new.exe"), previous = Path.Combine(dir, "stand-in.old.exe");
            File.WriteAllText(self, "the running Setup");
            File.WriteAllText(fresh, "the new Setup");
            File.WriteAllText(previous, "left by an update before");
            Handover.Swap(fresh, self, previous);
            Check(File.ReadAllText(self) == "the new Setup" && File.ReadAllText(previous) == "the running Setup" && !File.Exists(fresh), "the swap did not put the new Setup under the name and keep the one before");
            // (the second step fails: the first is undone, and there is still a Setup under the name)
            Check(Throws(() => Handover.Swap(fresh, self, previous)) && File.ReadAllText(self) == "the new Setup" && !File.Exists(previous),
                  "a swap whose second step failed left no Setup under the name, or said nothing");
            // (the one before cannot be removed, still in use: nothing is changed)
            File.WriteAllText(fresh, "newer still");
            File.WriteAllText(previous, "in use");
            using (new FileStream(previous, FileMode.Open, FileAccess.Read, FileShare.None))
                Check(Throws(() => Handover.Swap(fresh, self, previous)) && File.ReadAllText(self) == "the new Setup" && File.Exists(fresh), "a swap that could not begin changed something");
            Check(File.ReadAllText(previous) == "in use", "(the test) the file in use");

            // going back, with plain files: in one step, or not at all
            File.WriteAllText(previous, "the Setup before");
            File.WriteAllText(self, "a new Setup that does not start");
            using (new FileStream(self, FileMode.Open, FileAccess.Read, FileShare.Read))   // (a virus scanner still reading it)
                Check(!Handover.Restore(previous, self, 2) && File.ReadAllText(previous) == "the Setup before", "a new Setup that is held open was written over, or the one before lost");
            Check(Handover.Restore(previous, self) && File.ReadAllText(self) == "the Setup before" && !File.Exists(previous), "the Setup before was not put back under its name");
            Check(!Handover.Restore(previous, self, 1) && File.ReadAllText(self) == "the Setup before", "going back with no Setup before to go back to did something");

            // the same with a program that runs, as Setup does while it replaces itself
            foreach (var f in new[] { self, fresh, previous }) File.Delete(f);
            File.Copy(me, self);
            var program = File.ReadAllBytes(self);
            using (var old = Start(self, "run"))
            {
                Check(old.StandardOutput.ReadLine() == "running", "(the test) the copy of this program did not start");
                File.WriteAllText(fresh, "a new Setup that is no program at all");
                Handover.Swap(fresh, self, previous);   // (Windows lets a running program be renamed)
                Check(File.ReadAllText(self) == "a new Setup that is no program at all" && File.ReadAllBytes(previous).SequenceEqual(program) && !old.HasExited, "a running Setup could not step aside for the new one");
                Check(Throws(() => Process.Start(new ProcessStartInfo(self) { UseShellExecute = false })?.Dispose()), "(the test) a file that is no program was started");
                Check(Handover.Restore(previous, self) && File.ReadAllBytes(self).SequenceEqual(program) && !File.Exists(previous) && !old.HasExited,
                      "the running Setup could not take its name back from a new one that did not start");
                // (and it is a Setup under that name again: it starts)
                using (var again = Start(self, "crash 0"))
                    Check(again.WaitForExit(20000) && again.ExitCode == 0, "the Setup that was put back does not start");
                // (a new Setup that does run is never written over: Windows refuses, and nothing is changed)
                File.Copy(me, previous);
                Check(!Handover.Restore(previous, self, 2) && File.Exists(previous) && !old.HasExited, "a Setup that runs was written over by the one before");
                old.Kill();
                old.WaitForExit();
            }
        }
        finally
        {
            for (var i = 0; i < 20; i++)
            {
                try { Directory.Delete(dir, true); break; } catch (Exception) { Thread.Sleep(100); }
            }
        }

        Console.WriteLine("setup_handover_test: " + checks + " checks passed");
        return 0;
    }
}
