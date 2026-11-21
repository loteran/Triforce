// Triforce.Logic's tests. Run: dotnet run --project LogicTests (exit code 0 = every test passed). Each test makes its own
// TRIFORCE_TEST_ROOT folder under %TEMP%; nothing outside those folders is written.
using System.Diagnostics;
using Triforce.LogicTests;

if (args.Length >= 2 && args[0] == "-I" && args[1].EndsWith("make_game.py", StringComparison.Ordinal)) return Fakes.MakeGame(args.Skip(2).ToArray());
if (args.Contains("--romfs") && args.Contains("--user")) return Fakes.FakeGame(args);
if (Environment.GetEnvironmentVariable("LOGICTESTS_CHILD") is { Length: > 0 } mode && args.Contains("--relaunched")) return Fakes.NewSetup(mode, args);

var only = args.Length > 0 ? args[0] : null;
var tests = Tests.All().Concat(Tests.GameOptionsTests()).Where(t => only == null || t.Name.Contains(only, StringComparison.OrdinalIgnoreCase)).ToList();
int passed = 0, failed = 0;
foreach (var (name, run) in tests)
{
    var sw = Stopwatch.StartNew();
    try
    {
        await run();
        passed++;
        Console.WriteLine("PASS  " + name + "  (" + sw.ElapsedMilliseconds + " ms)");
    }
    catch (Exception e)
    {
        failed++;
        Console.WriteLine("FAIL  " + name + ": " + (e is TestFailed ? e.Message : e.ToString()));
    }
}
Console.WriteLine();
Console.WriteLine(passed + " passed, " + failed + " failed");
Environment.SetEnvironmentVariable("LOGICTESTS_CHILD", null);
if (failed == 0) T.CleanUp();
return failed == 0 ? 0 : 1;
