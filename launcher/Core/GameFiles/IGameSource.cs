// The player's own game files, wherever they are: a .nup dump (NupSource) or a folder holding romfs/ and exefs/
// (FolderSource). Everything the launcher shows from the game (title art, music, sounds) is read through this, at run time,
// from the player's copy. Nothing read here is ever written into the repository or a release.
namespace Triforce.Core.GameFiles;

/// <summary>
/// A read-only view of a game's romfs. Paths are romfs paths with "/" between the parts and no leading slash, for example
/// "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.bntx.zs". All members are safe to call from several threads.
/// </summary>
public interface IGameSource : IDisposable
{
    /// <summary>Whether the romfs has this file.</summary>
    bool Exists(string romfsPath);

    /// <summary>The file's bytes, as the game sees them (a .zs file stays zstd: see <see cref="Zs"/>). Throws
    /// FileNotFoundException when there is no such file.</summary>
    byte[] Read(string romfsPath);

    /// <summary>What a romfs folder holds, one level deep, as romfs paths: files as they are, folders with a trailing "/".
    /// "" lists the romfs root. Empty for a folder that does not exist.</summary>
    IEnumerable<string> List(string dir);

    /// <summary>A short description for logs: what kind of source, and where.</summary>
    string Describe { get; }

    /// <summary>A cheap identity of one file (size and time, or where it sits in the package): it changes when the file
    /// can have changed. Used to key caches. Null when there is no such file.</summary>
    string? Stamp(string romfsPath);
}

/// <summary>Opening a source from whatever the player dropped or chose.</summary>
public static class GameSource
{
    /// <summary>A .nup file, a game folder (holding romfs/) or a romfs folder itself.</summary>
    public static IGameSource Open(string path)
    {
        if (File.Exists(path))
            return new NupSource(path);
        if (Directory.Exists(path))
            return new FolderSource(path);
        throw new FileNotFoundException("No game file or folder at " + path, path);
    }

    /// <summary>A romfs path made uniform: "/" between the parts, no leading or trailing "/".</summary>
    internal static string Clean(string romfsPath) => romfsPath.Replace('\\', '/').Trim('/');
}
