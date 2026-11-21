using Triforce.Launcher.Host;

namespace Triforce.Launcher;

static class Program
{
    /// <summary>Exit codes the old Setup understands when it swaps itself for this exe (research/02 §2): nonzero before the
    /// "up" event = roll back to the old one. 3 = no web view on this PC.</summary>
    [STAThread]
    static int Main(string[] args)
    {
        var dataDir = DataDir();
        Directory.CreateDirectory(dataDir);
        Log.FilePath = Path.Combine(dataDir, "launcher.log");
        Log.Write("start " + string.Join(' ', args));
        var dev = args.Contains("--devtools") || Environment.GetEnvironmentVariable("TRIFORCE_DEVTOOLS") == "1";

        // The old Setup's start contract (research/02): arguments, one instance, the "up" event, the old exe removed.
        var ctx = Triforce.Logic.TriforceContext.Create();
        var start = Triforce.Logic.Startup.Run(ctx, args);
        Log.Write("startup: " + start.Screen + (start.Message != null ? " (" + start.Message + ")" : ""));
        if (start.Screen == Triforce.Logic.StartScreen.Exit) return start.ExitCode;

        var content = new Content();
        IHost host;
#if WINDOWS
        if (WinHost.RuntimeVersion() == null)
        {
            Log.Write("no WebView2 runtime");
            return 3;
        }
        host = new WinHost(content, Path.Combine(dataDir, "webview"), dev);
#else
        // What Photino.Native links (GTK 3, WebKitGTK 4.1, libnotify). One missing: exit 4, and AppRun opens the classic
        // builder window instead.
        foreach (var lib in new[] { "libgtk-3.so.0", "libwebkit2gtk-4.1.so.0", "libnotify.so.4" })
            if (!System.Runtime.InteropServices.NativeLibrary.TryLoad(lib, out _))
            {
                Log.Write("missing " + lib + ": the classic builder window opens instead");
                return 4;
            }
        host = new LinuxHost(content, Path.Combine(dataDir, "webview"), dev);
#endif
        var app = new Backend(host, content, ctx, start, dataDir);
        host.Message += app.OnMessage;
        host.Run();
        app.Shutdown();
        Log.Write("end");
        return 0;
    }

    /// <summary>The launcher's own working data (web view cache, log, decoded game media). Under TRIFORCE_TEST_ROOT
    /// everything stays inside that folder.</summary>
    static string DataDir()
    {
        var test = Environment.GetEnvironmentVariable("TRIFORCE_TEST_ROOT");
        if (!string.IsNullOrEmpty(test)) return Path.Combine(Path.GetFullPath(test), "launcher");
        if (OperatingSystem.IsWindows())
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Triforce", "launcher");
        var xdg = Environment.GetEnvironmentVariable("XDG_CACHE_HOME");
        return Path.Combine(string.IsNullOrEmpty(xdg) ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".cache") : xdg, "triforce-launcher");
    }
}
