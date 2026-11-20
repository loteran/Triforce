// tests/test_setup_instance.py: setup/Guard.cs's Instance between real processes (this program started again as the second Setup),
// with the names of folders that need not exist. No Setup runs and no install is touched.
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Threading;

namespace TriforceSetup;

static class InstanceTest
{
    static int checks;
    static void Check(bool ok, string what)
    {
        if (!ok) throw new Exception(what);
        checks++;
    }

    static Process Start(string mode, string folder, string? root = null)
    {
        var a = mode + " \"" + folder + "\"" + (root != null ? " \"" + root + "\"" : "");
        return Process.Start(new ProcessStartInfo(Assembly.GetExecutingAssembly().Location, a)
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true })!;
    }

    static int Second(string folder, string? root = null)
    {
        using var p = Start("second", folder, root);
        if (!p.WaitForExit(20000)) { p.Kill(); throw new Exception("the second start did not end"); }
        return p.ExitCode;
    }

    const int Refused = 10, RefusedNobodyToCall = 11, GotIt = 20;

    static int Main(string[] args)
    {
        if (args.Length >= 2)
        {
            var root = args.Length > 2 ? args[2] : null;
            if (args[0] == "second")   // (what a second start does)
                return Instance.Take(args[1], root) ? GotIt : Instance.CallForward(args[1], root) ? Refused : RefusedNobodyToCall;
            if (args[0] == "hold")     // (a Setup that holds its folder until it is killed)
            {
                if (!Instance.Take(args[1], root)) return 21;
                Console.WriteLine("held");
                Console.Out.Flush();
                Thread.Sleep(Timeout.Infinite);
            }
            return 2;
        }

        // the names
        var a = @"C:\Users\you\AppData\Local\Programs\Triforce";
        var name = Instance.Name(a, null);
        Check(name.StartsWith(@"Local\TriforceSetup-") && name.IndexOf('\\', 6) < 0 && name.Length < 200, "not a name Windows takes for a lock: " + name);
        Check(name == Instance.Name(a.ToLowerInvariant() + @"\", null), "the same folder typed in other letters is another lock");
        Check(name == Instance.Name(@"C:\Users\you\AppData\Local\Programs\Other\..\Triforce", null), "the same folder by another way is another lock");
        Check(name == Instance.Name(a.Replace('\\', '/'), null), "the same folder with other slashes is another lock");
        Check(name != Instance.Name(@"D:\Triforce", null), "two folders share a lock");
        Check(name != Instance.Name(a, @"E:\scratch\root"), "a test run shares the lock of the real install in the same folder");
        Check(Instance.Name(a, @"E:\scratch\one") != Instance.Name(a, @"E:\scratch\two"), "two test folders share a lock");
        Check(Instance.Name(@"C:\" + new string('x', 400), null).Length < 200, "a long folder makes a name Windows refuses");

        var folder = Path.Combine(Path.GetTempPath(), "triforce-instance-test-" + Guid.NewGuid().ToString("N"));
        var called = new AutoResetEvent(false);
        var calls = 0;

        // the first Setup, and what a second start finds
        Check(Instance.Take(folder, null), "the first Setup did not get its folder");
        Check(Instance.Take(folder, null), "the Setup that holds the folder was refused it");
        Check(Second(folder) == Refused, "a second start before the window listens was not refused, or could not leave its call");
        Instance.Listen(() => { Interlocked.Increment(ref calls); called.Set(); });
        Check(called.WaitOne(10000), "a call made before the window listened was lost");
        Check(Second(folder) == Refused, "a second start for the same folder was not refused");
        Check(called.WaitOne(10000), "the first Setup was not asked to the front");
        Check(Second(folder.ToUpperInvariant().Replace('\\', '/') + "/") == Refused, "a second start for the same folder, typed otherwise, was not refused");
        Check(called.WaitOne(10000), "the first Setup was not asked to the front (the folder typed otherwise)");
        Check(Second(folder + "-other") == GotIt, "a Setup of another folder was refused");
        Check(Second(folder, Path.Combine(folder, "root")) == GotIt, "a test run (TRIFORCE_TEST_ROOT) was refused because of the real install's Setup");
        Check(!called.WaitOne(300) && calls == 3, "the first Setup was called by a start for another folder");

        // handing over: the folder is let go before the next Setup starts, and taken again when that one did not start
        Instance.Release();
        Check(Second(folder) == GotIt, "after Release the folder was not free");
        Check(!called.WaitOne(300), "a Setup that let go was still called");
        Check(Instance.TakeAgain(), "the folder could not be taken again");
        Check(Second(folder) == Refused && called.WaitOne(10000), "after taking the folder again a second start was not refused, or its call not heard");
        Instance.Release();
        Instance.Release();

        // a Setup that ends without letting go (killed, crashed): Windows lets go for it
        using (var holder = Start("hold", folder))
        {
            Check(holder.StandardOutput.ReadLine() == "held", "the other process did not get the free folder");
            Check(!Instance.Take(folder, null), "the folder of a running Setup was taken");
            Check(Second(folder) == Refused, "a call to a Setup that has no window yet could not be left");
            // (a third process still has the lock open while its holder dies: Windows then hands on an abandoned mutex)
            using var open = Mutex.OpenExisting(Instance.Name(folder, null));
            holder.Kill();
            holder.WaitForExit();
            Check(Instance.Take(folder, null), "the folder of a Setup that was killed stayed locked");
            Check(Second(folder) == Refused, "the folder taken over from a killed Setup is not held");
            Instance.Release();
        }
        using (var holder = Start("hold", folder))
        {
            Check(holder.StandardOutput.ReadLine() == "held", "the other process did not get the folder after Release");
            holder.Kill();
            holder.WaitForExit();
            Check(Second(folder) == GotIt, "the folder of a Setup that was killed stayed locked for the next start");
        }

        Console.WriteLine("setup_instance_test: " + checks + " checks passed");
        return 0;
    }
}
