// tests/test_setup_report.py: setup/Guard.cs's Report over authored texts (no Setup runs, no log of this PC is read, and the
// clipboard is not touched).
using System;
using System.IO;
using System.Linq;

namespace TriforceSetup;

static class ReportTest
{
    static int checks;
    static void Check(bool ok, string what)
    {
        if (!ok) throw new Exception(what);
        checks++;
    }

    static void Same(string got, string want, string what) => Check(got == want, what + ": \"" + got + "\", not \"" + want + "\"");

    static int Main()
    {
        const string home = @"C:\Users\Zelda";
        var nl = Environment.NewLine;

        // the last lines
        Same(Report.Tail("a\nb\nc\n", 2), "b" + nl + "c", "the last two lines");
        Same(Report.Tail("a\r\nb\r\nc\r\n\r\n", 5), "a" + nl + "b" + nl + "c", "fewer lines than asked for, from a file with Windows' line ends");
        Same(Report.Tail("", 5), "", "no text");
        Same(Report.Tail("one line", 1), "one line", "one line without an end");
        var long_ = string.Join("\n", Enumerable.Range(1, 500).Select(i => "line " + i + ".")) + "\n";
        var tail = Report.Tail(long_, Report.Lines);
        Check(Report.Lines == 200 && tail.StartsWith("line 301." + nl) && tail.EndsWith(nl + "line 500.") && tail.Split('\n').Length == 200, "not the last 200 of 500 lines");

        // the account's folder, however a tool printed it
        Same(Report.NoName(@"wrote C:\Users\Zelda\AppData\Local\Programs\Triforce\setup.log", home), @"wrote %USERPROFILE%\AppData\Local\Programs\Triforce\setup.log", "a plain path");
        Same(Report.NoName("reading C:/Users/Zelda/Games/OcarinaOfTime.nup", home), "reading %USERPROFILE%/Games/OcarinaOfTime.nup", "forward slashes");
        Same(Report.NoName(@"OSError: [Errno 22] 'C:\\Users\\Zelda\\AppData\\Local\\Triforce\\build\\sa\\gen\\a64_fpx.h'", home),
             @"OSError: [Errno 22] '%USERPROFILE%\\AppData\\Local\\Triforce\\build\\sa\\gen\\a64_fpx.h'", "doubled backslashes, as Python prints a path in an error");
        Same(Report.NoName(@"c:\users\ZELDA\Desktop and C:\Users/Zelda\Desktop", home), @"%USERPROFILE%\Desktop and %USERPROFILE%\Desktop", "other letters' case, mixed slashes");
        Same(Report.NoName(@"--out ""C:\Users\Zelda"" (C:\Users\Zelda)", home), @"--out ""%USERPROFILE%"" (%USERPROFILE%)", "the folder itself, in quotes and at the end of a line");
        Same(Report.NoName(@"C:\Users\Zeldana\Games C:\Users\Zelda2\Games C:\Users\Zel\Games", home), @"C:\Users\Zeldana\Games C:\Users\Zelda2\Games C:\Users\Zel\Games",
             "another account whose name only begins the same");
        Same(Report.NoName(@"D:\Games\Triforce and C:\Windows\Temp", home), @"D:\Games\Triforce and C:\Windows\Temp", "paths outside the account's folder");
        // (the short name programs print for temporary files, when the account's name is long or has a space)
        const string spaced = @"C:\Users\Princess Zelda", brief = @"C:\Users\PRINCE~1";
        Same(Report.NoName(@"clang: error: C:\Users\PRINCE~1\AppData\Local\Temp\cc-1f.o, from C:\Users\Princess Zelda\Games", spaced, brief),
             @"clang: error: %USERPROFILE%\AppData\Local\Temp\cc-1f.o, from %USERPROFILE%\Games", "the folder's short name");
        // (a name with characters that mean something in a pattern is that name and no other)
        const string odd = @"C:\Users\a.b (c)+[d]$";
        Same(Report.NoName(@"C:\Users\a.b (c)+[d]$\x and C:\Users\axb (c)+[d]$\x", odd), @"%USERPROFILE%\x and C:\Users\axb (c)+[d]$\x", "a name with a dot, brackets and a plus");
        Same(Report.NoName(@"\\server\home\zelda\Games and //server/home/zelda/Games", @"\\server\home\zelda"), @"%USERPROFILE%\Games and %USERPROFILE%/Games", "an account's folder on a server");
        Same(Report.NoName(@"C:\Users\Zelda\x", null, "", "  "), @"C:\Users\Zelda\x", "no folder known: nothing to take out, and nothing broken");
        Same(Report.NoName(@"C:\Games\x and C:\Users\Zelda", @"C:\"), @"C:\Games\x and C:\Users\Zelda", "a whole drive as the account's folder would take every path out");

        // the report
        var build = string.Join("\n", Enumerable.Range(1, 300).Select(i => "[" + i + "/760] clang -O2 -c C:\\Users\\Zelda\\AppData\\Local\\Triforce\\build\\sa\\gen\\code_" + i + ".c")) +
                    "\n@@error The compiler stopped at sa/gen/code_300.c (exit code 1).\n";
        var log = string.Join("\r\n", Enumerable.Range(1, 250).Select(i => "2026-11-15 10:00:00  log line " + i + " in C:/Users/Zelda/AppData/Local/Programs/Triforce")) + "\r\n";
        var known = new[] { "Triforce Setup v9.9.9, tools v9.9.9, Microsoft Windows NT 10.0.26200.0", "The build stopped at \"Compiling\" after 12 min: The compiler stopped.",
                            @"make_game.py --nup C:\Users\Zelda\Games\OcarinaOfTime.nup --out D:\Games\Triforce --language en-US --fsr (tools C:\Users\Zelda\AppData\Local\Programs\Triforce\tools)",
                            @"Game folder: D:\Games\Triforce (D:\ has 12 GB free)", @"Setup: C:\Users\Zelda\AppData\Local\Programs\Triforce (C:\ has 80 GB free)" };
        var report = Report.Compose(known, build, log, home, null);
        Check(report.IndexOf("zelda", StringComparison.OrdinalIgnoreCase) < 0, "the account's name is in the report");
        Check(report.StartsWith("Triforce Setup v9.9.9, tools v9.9.9, Microsoft Windows NT 10.0.26200.0" + nl + "The build stopped at \"Compiling\" after 12 min: The compiler stopped." + nl +
                                @"make_game.py --nup %USERPROFILE%\Games\OcarinaOfTime.nup --out D:\Games\Triforce --language en-US --fsr (tools %USERPROFILE%\AppData\Local\Programs\Triforce\tools)" + nl),
              "the report does not begin with what Setup knows of the build");
        Check(report.Contains(@"Game folder: D:\Games\Triforce (D:\ has 12 GB free)" + nl + @"Setup: %USERPROFILE%\AppData\Local\Programs\Triforce (C:\ has 80 GB free)" + nl), "the folders and their room are not in the report");
        int b = report.IndexOf("---- the build's last lines ----"), l = report.IndexOf("---- setup.log's last lines ----");
        Check(b > 0 && l > b, "the two logs are not in the report under their names, the build's first");
        var built = report.Substring(b, l - b);
        Check(!built.Contains("[101/760]") && built.Contains("[102/760] clang -O2 -c %USERPROFILE%\\AppData") && built.Contains("@@error The compiler stopped at sa/gen/code_300.c (exit code 1)."),
              "not the build's last 200 lines");
        var logged = report.Substring(l);
        Check(!logged.Contains("log line 50 ") && logged.Contains("log line 51 in %USERPROFILE%/AppData") && logged.Contains("log line 250 in "), "not setup.log's last 200 lines");
        Check(Report.Compose(known, "", "", home).Contains("---- setup.log's last lines ----"), "a report without logs is not put together");

        // a folder's short name: none, or another way to the same folder
        var dir = Path.Combine(Path.GetTempPath(), "triforce report test " + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var s = Report.Short(dir);
            Check(s == null || (!string.Equals(s, dir, StringComparison.OrdinalIgnoreCase) && Directory.Exists(s)), "the short name of a folder is not a folder: " + s);
            Check(Report.Short(Path.Combine(dir, "not there")) == null, "a folder that is not there has a short name");
        }
        finally { Directory.Delete(dir, true); }

        Console.WriteLine("setup_report_test: " + checks + " checks passed");
        return 0;
    }
}
