namespace Triforce.Logic;

/// <summary>(Triforce) A copy that is never seen half done (old Guard.cs Partial). The old Setup copied itself into the install
/// folder and a save over the one there file by file, in place: a full disk, a file held open or Setup closed in the middle left
/// a Setup that does not start under the name every shortcut starts, or half a save. Now the copy is written as
/// &lt;name&gt;.partial beside its place and renamed into it once complete; what was there is replaced in one step (a file) or
/// moved aside as &lt;name&gt;.old and removed after (a folder), and moved back when the new one cannot go in.</summary>
public sealed class Partial
{
    public const string Suffix = ".partial", Aside = ".old";
    readonly IPlatform platform;
    public Partial(IPlatform platform) { this.platform = platform; }

    /// <summary>(testing) called with the partial once it is complete, before it goes into its place: a test checks what holds
    /// then, or throws to stand for a failure there.</summary>
    public Action<string>? Copied { get; set; }

    /// <summary>A file or a folder and what is in it, gone; quietly when it cannot be.</summary>
    public static void Remove(string path)
    {
        try
        {
            if (File.Exists(path)) { File.SetAttributes(path, FileAttributes.Normal); File.Delete(path); }
            else if (Directory.Exists(path)) Directory.Delete(path, true);
        }
        catch (Exception) { }
    }

    /// <summary>from copied to to, through to.partial: an older to is replaced in one step only once the copy is complete.</summary>
    public void CopyFile(string from, string to)
    {
        var partial = to + Suffix;
        try
        {
            Remove(partial);
            File.Copy(from, partial, false);
            Copied?.Invoke(partial);
            platform.ReplaceFile(partial, to);
        }
        catch (Exception)
        {
            Remove(partial);
            throw;
        }
    }

    /// <summary>The folder from copied as to.partial (one there from a copy that was cut off is removed first); its path.</summary>
    public string Stage(string from, string to)
    {
        var partial = to + Suffix;
        try
        {
            Remove(partial);
            Tree(from, partial);
            return partial;
        }
        catch (Exception)
        {
            Remove(partial);
            throw;
        }
    }

    static void Tree(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var f in Directory.GetFiles(from)) File.Copy(f, Path.Combine(to, Path.GetFileName(f)), false);
        foreach (var d in Directory.GetDirectories(from)) Tree(d, Path.Combine(to, Path.GetFileName(d)));
    }

    /// <summary>A complete partial folder put in to's place: the folder there moved aside as to.old first and removed after, or
    /// moved back when the new one cannot go in (the partial is then removed).</summary>
    public void SwapIn(string partial, string to)
    {
        var old = to + Aside;
        try
        {
            Copied?.Invoke(partial);
            Remove(old);
            if (Directory.Exists(to)) Directory.Move(to, old);
            try { Directory.Move(partial, to); }
            catch (Exception)
            {
                try { if (!Directory.Exists(to) && Directory.Exists(old)) Directory.Move(old, to); } catch (Exception) { }
                throw;
            }
        }
        catch (Exception)
        {
            Remove(partial);
            throw;
        }
        Remove(old);
    }

    /// <summary>What a copy that was cut off left beside to: the folder moved aside goes back when to is gone, and a partial goes.</summary>
    public static void Recover(string to)
    {
        try { if (!Directory.Exists(to) && Directory.Exists(to + Aside)) Directory.Move(to + Aside, to); } catch (Exception) { }
        if (Directory.Exists(to)) Remove(to + Aside);
        Remove(to + Suffix);
    }
}
