namespace Triforce.Launcher;

/// <summary>A plain text log beside the launcher's data (launcher.log). Never throws.</summary>
public static class Log
{
    static readonly object gate = new();
    public static string? FilePath;

    public static void Write(string line)
    {
        try
        {
            lock (gate)
            {
                var s = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff") + "  " + line + Environment.NewLine;
                if (FilePath != null) File.AppendAllText(FilePath, s);
                else Console.Error.Write(s);
            }
        }
        catch { }
    }
}
