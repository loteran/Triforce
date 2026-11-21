// A game the player keeps unpacked: the folder that holds romfs/ (and exefs/), or the romfs folder itself.
namespace Triforce.Core.GameFiles;

/// <summary>Reads romfs files straight from a folder. Never writes to it.</summary>
public sealed class FolderSource : IGameSource
{
    /// <summary>The romfs folder in use.</summary>
    public string Romfs { get; }

    /// <param name="path">The game folder holding romfs/, or the romfs folder itself (it is taken as one when it has the
    /// folders every romfs of this game has: Pack and Sound).</param>
    public FolderSource(string path)
    {
        var full = Path.GetFullPath(path);
        if (Directory.Exists(Path.Combine(full, "romfs")))
            Romfs = Path.Combine(full, "romfs");
        else if (Directory.Exists(Path.Combine(full, "Pack")) || Directory.Exists(Path.Combine(full, "Sound")))
            Romfs = full;
        else
            throw new DirectoryNotFoundException("No romfs folder in " + full + " (choose the game folder that holds romfs and exefs)");
    }

    public string Describe => "folder " + Romfs;

    string Full(string romfsPath) => Path.Combine(Romfs, GameSource.Clean(romfsPath).Replace('/', Path.DirectorySeparatorChar));

    public bool Exists(string romfsPath) => File.Exists(Full(romfsPath));

    public byte[] Read(string romfsPath)
    {
        var file = Full(romfsPath);
        // (FileShare.ReadWrite: another program, the game itself, may have the file open; we only ever read)
        using var f = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1, FileOptions.SequentialScan);
        var data = new byte[f.Length];
        f.ReadExactly(data);
        return data;
    }

    public IEnumerable<string> List(string dir)
    {
        var clean = GameSource.Clean(dir);
        var folder = clean.Length == 0 ? Romfs : Full(clean);
        if (!Directory.Exists(folder))
            yield break;
        var prefix = clean.Length == 0 ? "" : clean + "/";
        foreach (var d in Directory.EnumerateDirectories(folder))
            yield return prefix + Path.GetFileName(d) + "/";
        foreach (var f in Directory.EnumerateFiles(folder))
            yield return prefix + Path.GetFileName(f);
    }

    public string? Stamp(string romfsPath)
    {
        var info = new FileInfo(Full(romfsPath));
        return info.Exists ? info.Length + "-" + info.LastWriteTimeUtc.Ticks : null;
    }

    public void Dispose() { }
}
