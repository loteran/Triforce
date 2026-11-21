// make_game.py's lines (its docstring, nx2/make_game.py:12-25; research/02 3.2, research/12 4.2), as typed events.
//   @@stage <id> <text>      a step begins            @@progress <0..1>        how far that step is (at most 4 a second)
//   @@plan <changed> <total> files the compile redoes  @@file <name> <s> <new|cached|again>   one module built
//   @@eta <seconds>          the compile's time left   @@times <total> <compile> <previous|-1>  at the end
//   @@check <ok|bad> <text>  (--check)                 @@done <folder>          finished: THE success signal, not the exit code
//   @@error <text>           stopped
// and two plain lines: "the shaders were not prepared now (<why>): ..." (NOT_PREPARED: left to the game's first start) and
// "the build's steps: before the game files 0:41, read 1:32, ..." (each stage's time, make_game.py stage_times).
using System.Globalization;
using System.Text.RegularExpressions;

namespace Triforce.Logic;

public abstract record BuildEvent
{
    public sealed record Stage(string Id, string Text) : BuildEvent;
    public sealed record Progress(double Fraction) : BuildEvent;
    public sealed record Plan(int Changed, int Total) : BuildEvent;
    public sealed record FileBuilt(string Name, double Seconds, string How) : BuildEvent;
    public sealed record Eta(int Seconds) : BuildEvent;
    public sealed record Times(int TotalSeconds, int CompileSeconds, int? PreviousSeconds) : BuildEvent;
    public sealed record Done(string Folder) : BuildEvent;
    public sealed record Error(string Text) : BuildEvent;
    public sealed record Check(bool Ok, string Text) : BuildEvent;
    /// <summary>The shaders were not prepared now: the game prepares them at its first start (the shader row says so).</summary>
    public sealed record NotPrepared(string Line) : BuildEvent;
    /// <summary>make_game's own account of each stage's time ("read 1:32, lift 2:10, ...").</summary>
    public sealed record StageTimes(IReadOnlyDictionary<string, double> Seconds) : BuildEvent;
    /// <summary>Any other line (the details drawer shows it).</summary>
    public sealed record Plain(string Line) : BuildEvent;
}

/// <summary>A step of the build, in order, with the words the build screen shows (old Shell.cs Stages; DESIGN 6 Build).</summary>
public sealed record BuildStage(string Id, string Text, double Weight);

public static class BuildStages
{
    /// <summary>read, lift, compile, finish (make_game.py:1850, 1953, 2007, 2077) and shaders (the game's own --prepare-shaders,
    /// gpu/vk/vk_main.cpp:1153, which make_game runs at its end and passes its @@stage/@@progress on). Weight: a stage's share of a
    /// build without the shaders (make_game.py STAGE_SPAN: read 0-5 %, lift 5-25, compile 25-95, finish 95-100).</summary>
    public static readonly IReadOnlyList<BuildStage> All = new BuildStage[]
    {
        new("read", "Reading your game files", 0.05),
        new("lift", "Translating the game to PC code", 0.20),
        new("compile", "Compiling", 0.70),
        new("finish", "Putting the game folder together", 0.05),
        new("shaders", "Preparing shaders for your graphics card", 0),
    };

    public static BuildStage? Find(string id) => All.FirstOrDefault(s => s.Id == id);
    public static int IndexOf(string id) => All.ToList().FindIndex(s => s.Id == id);
}

public static class BuildProtocol
{
    public const string NotPreparedStart = "the shaders were not prepared now";
    const string StepsStart = "the build's steps: ";
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    /// <summary>One line of make_game.py's output as an event (never null for a line: plain text is Text).</summary>
    public static BuildEvent Parse(string line)
    {
        if (line.StartsWith("@@", StringComparison.Ordinal))
        {
            var sp = line.IndexOf(' ');
            var word = sp < 0 ? line.Substring(2) : line.Substring(2, sp - 2);
            var rest = sp < 0 ? "" : line.Substring(sp + 1);
            var w = rest.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            switch (word)
            {
                case "stage" when w.Length >= 1:
                    return new BuildEvent.Stage(w[0], rest.Length > w[0].Length ? rest.Substring(w[0].Length).Trim() : "");
                case "progress" when w.Length >= 1 && double.TryParse(w[0], NumberStyles.Float, Inv, out var f):
                    return new BuildEvent.Progress(Math.Clamp(f, 0, 1));
                case "plan" when w.Length == 2 && int.TryParse(w[0], out var c) && int.TryParse(w[1], out var t):
                    return new BuildEvent.Plan(c, t);
                case "file" when w.Length == 3 && double.TryParse(w[1], NumberStyles.Float, Inv, out var s):
                    return new BuildEvent.FileBuilt(w[0], s, w[2]);
                case "eta" when w.Length >= 1 && double.TryParse(w[0], NumberStyles.Float, Inv, out var e):
                    return new BuildEvent.Eta((int)Math.Round(e));
                case "times" when w.Length == 3 && double.TryParse(w[0], NumberStyles.Float, Inv, out var tt) && double.TryParse(w[1], NumberStyles.Float, Inv, out var cc)
                                  && double.TryParse(w[2], NumberStyles.Float, Inv, out var pp):
                    return new BuildEvent.Times((int)tt, (int)cc, pp < 0 ? null : (int)pp);
                case "done":
                    return new BuildEvent.Done(rest.Trim());
                case "error":
                    return new BuildEvent.Error(rest);
                case "check" when w.Length >= 1 && (w[0] == "ok" || w[0] == "bad"):
                    return new BuildEvent.Check(w[0] == "ok", rest.Length > w[0].Length ? rest.Substring(w[0].Length + 1) : "");
            }
            return new BuildEvent.Plain(line);
        }
        if (line.StartsWith(NotPreparedStart, StringComparison.Ordinal)) return new BuildEvent.NotPrepared(line);
        if (line.StartsWith(StepsStart, StringComparison.Ordinal)) return new BuildEvent.StageTimes(ParseSteps(line.Substring(StepsStart.Length)));
        return new BuildEvent.Plain(line);
    }

    /// <summary>"before the game files 0:41, read 1:32, lift 2:10, compile 7:42, finish 1:17" (clock: m:ss or h:mm:ss).</summary>
    public static IReadOnlyDictionary<string, double> ParseSteps(string text)
    {
        var d = new Dictionary<string, double>();
        foreach (var part in text.Split(", "))
        {
            var m = Regex.Match(part.Trim(), @"^(.*\S)\s+(\d+(?::\d{1,2}){1,2})$");
            if (!m.Success) continue;
            var secs = m.Groups[2].Value.Split(':').Aggregate(0.0, (acc, x) => acc * 60 + int.Parse(x));
            d[m.Groups[1].Value] = secs;
        }
        return d;
    }
}

/// <summary>The build's state as its lines come (old Shell.cs Line/Ended; the Tk builder's parser): the stage it is in, that
/// stage's progress, when each stage began, the plan, the time left the compile said, the folder @@done named, the last
/// @@error. Fed line by line by BuildRunner, or by a test from a transcript.</summary>
public sealed class BuildTracker
{
    readonly Func<DateTime> now;
    public BuildTracker(Func<DateTime>? clock = null) { now = clock ?? (() => DateTime.UtcNow); Started = now(); }

    public DateTime Started { get; private set; }
    public string? Stage { get; private set; }
    public double StageProgress { get; private set; }
    public DateTime StageStarted { get; private set; }
    /// <summary>Each stage's seconds once the next began (the last one's at the end).</summary>
    public Dictionary<string, double> StageSeconds { get; } = new();
    public (int Changed, int Total)? Plan { get; private set; }
    public int FilesBuilt { get; private set; }
    public int? EtaSeconds { get; private set; }
    public BuildEvent.Times? Times { get; private set; }
    public string? DoneFolder { get; private set; }
    public string? Error { get; private set; }
    public string? NotPrepared { get; private set; }
    public IReadOnlyDictionary<string, double>? ReportedSteps { get; private set; }

    public event Action<BuildEvent>? Event;
    /// <summary>A stage ended (its id) and the next began.</summary>
    public event Action<string?, string>? StageChanged;

    public BuildEvent Feed(string line)
    {
        var e = BuildProtocol.Parse(line);
        switch (e)
        {
            case BuildEvent.Stage s:
                var t = now();
                if (Stage != null && !ended) StageSeconds[Stage] = StageSeconds.GetValueOrDefault(Stage) + (t - StageStarted).TotalSeconds;
                var was = Stage;
                Stage = s.Id;
                StageStarted = t;
                StageProgress = 0;
                if (s.Id != "compile") EtaSeconds = null;
                StageChanged?.Invoke(was, s.Id);
                break;
            case BuildEvent.Progress p: StageProgress = p.Fraction; break;
            case BuildEvent.Plan p: Plan = (p.Changed, p.Total); break;
            case BuildEvent.FileBuilt f when f.How is "new" or "again": FilesBuilt++; break;
            case BuildEvent.Eta x: EtaSeconds = x.Seconds; break;
            case BuildEvent.Times x: Times = x; break;
            case BuildEvent.Done d: DoneFolder = d.Folder; break;
            case BuildEvent.Error x: Error = x.Text; break;
            case BuildEvent.NotPrepared n: NotPrepared = n.Line; break;
            case BuildEvent.StageTimes st: ReportedSteps = st.Seconds; break;
        }
        Event?.Invoke(e);
        return e;
    }

    /// <summary>The last stage's time, at the build's end.</summary>
    public void End()
    {
        if (Stage != null && !ended) StageSeconds[Stage] = StageSeconds.GetValueOrDefault(Stage) + (now() - StageStarted).TotalSeconds;
        ended = true;
    }

    bool ended;

    public TimeSpan Elapsed => now() - Started;
    public TimeSpan InStage => Stage == null ? TimeSpan.Zero : now() - StageStarted;
}
