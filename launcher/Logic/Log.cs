namespace Triforce.Logic;

/// <summary>setup.log: what the launcher checked, downloaded and ran, for bug reports (old Setup.cs Program.Log). Windows: beside
/// the installed launcher (&lt;install&gt;\setup.log, the old place); Linux: ${XDG_STATE_HOME}/triforce/setup.log. A line is
/// "yyyy-MM-dd HH:mm:ss  &lt;text&gt;"; the file is deleted once it is over 2 MiB. Nothing is written under NoWrites.</summary>
public sealed class Log
{
    readonly TriforceContext ctx;
    readonly object gate = new();
    internal Log(TriforceContext ctx) { this.ctx = ctx; }

    /// <summary>Every line as it is written (the App may show them; a test reads them).</summary>
    public event Action<string>? Written;

    public string FilePath => Path.Combine(ctx.Platform.LogFolder(ctx.Home), "setup.log");

    public void Write(string line)
    {
        Written?.Invoke(line);
        if (ctx.NoWrites) return;
        try
        {
            lock (gate)
            {
                var file = FilePath;
                Directory.CreateDirectory(Path.GetDirectoryName(file)!);
                if (File.Exists(file) && new FileInfo(file).Length > (2 << 20)) File.Delete(file);
                File.AppendAllText(file, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "  " + line + Environment.NewLine);
            }
        }
        catch (Exception) { }
    }

    /// <summary>The whole log, "" when there is none.</summary>
    public string ReadAll()
    {
        try { lock (gate) return File.Exists(FilePath) ? File.ReadAllText(FilePath) : ""; } catch (Exception) { return ""; }
    }
}
