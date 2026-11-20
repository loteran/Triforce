using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Text.Json;
using System.Windows;

namespace HookshotBuilder;

/// The game's update offer (host/hos_update.c) starts this app with --update <game folder> --tag <release>. When these tools are
/// not that release, the release's HookshotBuilder.zip is downloaded and unpacked beside this app, and the new app is started with
/// --fetched to build the folder; when they are, the folder is built again here (nx2/make_game.py --update: from the game files the
/// folder keeps).
static class Updater
{
    const string Latest = "https://api.github.com/repos/NotRexed/Hookshot/releases/latest";
    const string Asset = "HookshotBuilder.zip";
    const string SetupAsset = "HookshotBuilder.exe";            // (the setup, builder/setup/setup.c: a release holds it and its list)
    const string SetupList = "hookshot-setup.txt";              // (scripts/make_setup_parts.py)
    static readonly TimeSpan[] RetryDelays = { TimeSpan.FromSeconds(3), TimeSpan.FromSeconds(6), TimeSpan.FromSeconds(12), TimeSpan.FromSeconds(20) };

    public static void FromCommandLine(MainWindow w)
    {
        var a = Environment.GetCommandLineArgs();
        string? folder = null, tag = null;
        var fetched = false;
        for (var i = 1; i < a.Length; i++)
        {
            if (a[i] == "--update" && i + 1 < a.Length) folder = a[++i];
            else if (a[i] == "--tag" && i + 1 < a.Length) tag = a[++i];
            else if (a[i] == "--fetched") fetched = true;
        }
        if (folder == null) return;
        UpdateFolder(w, folder, tag, fetched);
    }

    /// The latest release's tag, quietly (null: GitHub did not answer); a few tries, 20 s at most
    public static async Task<string?> LatestTag()
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(8) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("HookshotBuilder");
        for (var attempt = 0; attempt < 3; attempt++)
        {
            try
            {
                using var request = new HttpRequestMessage(HttpMethod.Get, Latest);
                request.Headers.Accept.ParseAdd("application/vnd.github+json");
                using var response = await http.SendAsync(request);
                if (response.IsSuccessStatusCode)
                {
                    using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                    return doc.RootElement.GetProperty("tag_name").GetString();
                }
                if ((int)response.StatusCode < 500) break;   // (the API's limit, 60 an hour a PC: the list below instead)
            }
            catch (Exception) { }
            await Task.Delay(TimeSpan.FromSeconds(2 << attempt));
        }
        // (v2.0.0: when the API did not answer, the builder said a game was up to date. The release's setup list names its tag in
        // its first line, from GitHub's download service, not the API)
        for (var attempt = 0; attempt < 2; attempt++)
        {
            try
            {
                var list = await http.GetStringAsync("https://github.com/NotRexed/Hookshot/releases/latest/download/" + SetupList);
                foreach (var line in list.Split('\n'))
                    if (line.StartsWith("tag ")) return line[4..].Trim();
            }
            catch (Exception) { }
            await Task.Delay(TimeSpan.FromSeconds(2));
        }
        return null;
    }

    /// The latest release's setup (HookshotBuilder.exe) downloaded and started with args: it installs the new builder (only the
    /// parts that changed) and starts it with the same args; this one closes. (no args: the new builder opens as it does)
    public static async Task RunLatestSetup(MainWindow w, IEnumerable<string> args)
    {
        var token = w.DownloadToken;
        using var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("HookshotBuilder");
        w.Note("Asking GitHub for the latest release");
        string? setupUrl = null, setupApiUrl = null;
        long setupSize = 0;
        using (var response = await GetWithRetries(w, http, Latest, token, "application/vnd.github+json", TimeSpan.FromSeconds(30)))
        using (var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync(token)))
            foreach (var asset in doc.RootElement.GetProperty("assets").EnumerateArray())
                if (asset.GetProperty("name").GetString() == SetupAsset)
                {
                    setupUrl = asset.GetProperty("browser_download_url").GetString();
                    setupApiUrl = asset.GetProperty("url").GetString();
                    setupSize = asset.GetProperty("size").GetInt64();
                }
        if (setupUrl == null) throw new InvalidOperationException("The latest release has no " + SetupAsset + ".");
        var cache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HookshotBuilder", "update");
        Directory.CreateDirectory(cache);
        var setup = Path.Combine(cache, SetupAsset);
        w.Note("Downloading the new Hookshot Builder");
        await DownloadWithRetries(w, http, setupUrl, setupApiUrl, setup, setupSize, token);
        var si = new ProcessStartInfo(setup) { UseShellExecute = false, WorkingDirectory = cache };
        foreach (var arg in args) si.ArgumentList.Add(arg);
        Process.Start(si);
        Application.Current.Shutdown();
    }

    /// The game in folder built again with the latest release: here when these tools are it (or fetched), else after the setup
    /// installed it (it starts the new builder with --update folder --tag t --fetched)
    public static async void UpdateFolder(MainWindow w, string folder, string? tag, bool fetched)
    {
        w.BeginUpdate(folder, tag);
        var tools = Tools.Find();
        if (tools == null)
        {
            w.DownloadFailed("The Hookshot tools were not found next to this app.");
            return;
        }
        // (just downloaded, but another release than asked for: the setup installed the release before it (GitHub's latest link lags
        // right after a publish), and building here kept players on the old version while the builder said it updated)
        if (fetched && tag != null && tools.Version != null && tools.Version != tag)
        {
            w.DownloadFailed($"The update installed {tools.Version}, not {tag}: GitHub's download links can lag right after a release. Try Update again in a few minutes.");
            return;
        }
        // (already the new release, or just downloaded: no second download)
        if (fetched || (tag != null && tools.Version == tag))
        {
            w.UpdateBuild(tools, folder);
            return;
        }
        try
        {
            var token = w.DownloadToken;
            using var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
            http.DefaultRequestHeaders.UserAgent.ParseAdd("HookshotBuilder");
            w.Note("Asking GitHub for the latest release");
            string? newTag = null, url = null, setupUrl = null;
            string? apiUrl = null, setupApiUrl = null;
            long size = 0, setupSize = 0;
            var hasList = false;
            using (var response = await GetWithRetries(w, http, Latest, token, "application/vnd.github+json", TimeSpan.FromSeconds(30)))
            using (var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync(token)))
            {
                newTag = doc.RootElement.GetProperty("tag_name").GetString();
                foreach (var asset in doc.RootElement.GetProperty("assets").EnumerateArray())
                {
                    var assetName = asset.GetProperty("name").GetString();
                    if (assetName == Asset)
                    {
                        url = asset.GetProperty("browser_download_url").GetString();
                        apiUrl = asset.GetProperty("url").GetString();
                        size = asset.GetProperty("size").GetInt64();
                    }
                    else if (assetName == SetupAsset)
                    {
                        setupUrl = asset.GetProperty("browser_download_url").GetString();
                        setupApiUrl = asset.GetProperty("url").GetString();
                        setupSize = asset.GetProperty("size").GetInt64();
                    }
                    else if (assetName == SetupList) hasList = true;
                }
            }
            if (newTag == null || (url == null && setupUrl == null)) throw new InvalidOperationException("The latest release has no " + Asset + ".");
            if (newTag == tools.Version)
            {
                w.UpdateBuild(tools, folder);
                return;
            }
            var name = string.Concat(newTag.Where(c => char.IsLetterOrDigit(c) || c == '-' || c == '_' || c == '.'));
            // (hookshot) the name is a folder under builders\ that the update replaces: "..", "." or a name with a leading dot
            // would be the HookshotBuilder folder itself or not a version (the same rule as the setup's and Linux's lists)
            if (name.Length == 0 || name.Length > 128 || name[0] == '.')
                throw new InvalidOperationException("The latest release's tag is not a version name.");
            var cache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HookshotBuilder", "update");
            Directory.CreateDirectory(cache);
            // (a release with the setup and its list: the setup downloads only the parts that changed since the version installed (a few MB
            // when only the builder's code did), checks them and starts the new builder with these arguments. The whole zip is the fallback)
            if (setupUrl != null && hasList)
            {
                w.Note($"Downloading the update for {newTag}");
                var setup = Path.Combine(cache, SetupAsset);
                await DownloadWithRetries(w, http, setupUrl, setupApiUrl, setup, setupSize, token);
                var ssi = new ProcessStartInfo(setup) { UseShellExecute = false, WorkingDirectory = cache };
                foreach (var arg in new[] { "--update", folder, "--tag", newTag, "--fetched" }) ssi.ArgumentList.Add(arg);
                Process.Start(ssi);
                Application.Current.Shutdown();
                return;
            }
            if (url == null) throw new InvalidOperationException("The latest release has no " + Asset + ".");
            var zip = Path.Combine(cache, "HookshotBuilder-" + name + ".zip");
            w.Note($"Downloading {Asset} of {newTag} ({size >> 20} MB)");
            await DownloadWithRetries(w, http, url, apiUrl, zip, size, token, w.DownloadProgress);
            w.Note("Unpacking the new version");
            // (beside this app when that folder takes writing, else in the app data folder)
            var parent = Directory.GetParent(AppContext.BaseDirectory.TrimEnd('\\', '/'))!.FullName;
            string target;
            try
            {
                target = await Unpack(zip, Path.Combine(parent, "HookshotBuilder " + name));
            }
            catch (Exception e) when (e is UnauthorizedAccessException or IOException)
            {
                target = await Unpack(zip, Path.Combine(Path.GetDirectoryName(cache)!, "builders", name));
            }
            try { File.Delete(zip); } catch (Exception) { }
            var exe = Path.Combine(target, "HookshotBuilder.exe");
            if (!File.Exists(exe)) throw new FileNotFoundException("The download has no HookshotBuilder.exe (your antivirus may have removed it).");
            var si = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = target };
            foreach (var arg in new[] { "--update", folder, "--tag", newTag, "--fetched" }) si.ArgumentList.Add(arg);
            Process.Start(si);
            Application.Current.Shutdown();
        }
        catch (OperationCanceledException)
        {
            w.DownloadFailed("The download was cancelled. Your game folder is unchanged.");
        }
        catch (Exception e)
        {
            w.DownloadFailed(e.Message + " Your game folder is unchanged; you can also download the new version from GitHub yourself.");
        }
    }

    static async Task<HttpResponseMessage> GetWithRetries(MainWindow w, HttpClient http, string url, CancellationToken token,
        string? accept = null, TimeSpan? timeout = null)
    {
        Exception? last = null;
        for (var attempt = 0; attempt <= RetryDelays.Length; attempt++)
        {
            try
            {
                using var request = new HttpRequestMessage(HttpMethod.Get, url);
                if (accept != null) request.Headers.Accept.ParseAdd(accept);
                using var ask = timeout is null ? null : new CancellationTokenSource(timeout.Value);
                using var both = ask is null ? null : CancellationTokenSource.CreateLinkedTokenSource(token, ask.Token);
                var response = await http.SendAsync(request, HttpCompletionOption.ResponseContentRead, both?.Token ?? token);
                if (response.IsSuccessStatusCode) return response;

                var failure = HttpFailure(response);
                response.Dispose();
                if (!IsTransient(failure)) throw failure;
                last = failure;
            }
            catch (OperationCanceledException e) when (!token.IsCancellationRequested && timeout is not null)
            {
                last = e;
            }
            catch (HttpRequestException e) when (IsTransient(e))
            {
                last = e;
            }

            if (attempt == RetryDelays.Length) break;
            w.Note($"GitHub did not answer{StatusDetail(last)}. Retrying in {(int)RetryDelays[attempt].TotalSeconds} s...");
            await Task.Delay(RetryDelays[attempt], token);
        }
        throw new HttpRequestException($"GitHub's release API did not respond after {RetryDelays.Length + 1} attempts: {last?.Message}",
            last, (last as HttpRequestException)?.StatusCode);
    }

    static async Task DownloadWithRetries(MainWindow w, HttpClient http, string url, string? apiUrl, string path, long size,
        CancellationToken token, Action<double>? progress = null)
    {
        Exception? last = null;
        for (var attempt = 0; attempt <= RetryDelays.Length; attempt++)
        {
            try
            {
                await DownloadOnce(http, url, path, size, token, progress, apiAsset: false);
                return;
            }
            catch (Exception e) when (IsTransient(e))
            {
                last = e;
                if (apiUrl != null) w.Note($"Release download failed{StatusDetail(e)}; trying GitHub's API asset URL.");
            }

            if (apiUrl != null)
            {
                try
                {
                    await DownloadOnce(http, apiUrl, path, size, token, progress, apiAsset: true);
                    return;
                }
                catch (Exception e) when (IsTransient(e))
                {
                    last = e;
                }
            }

            if (attempt == RetryDelays.Length) break;
            w.Note($"GitHub's download servers did not answer{StatusDetail(last)}. Retrying in {(int)RetryDelays[attempt].TotalSeconds} s...");
            await Task.Delay(RetryDelays[attempt], token);
        }
        throw new HttpRequestException($"GitHub's asset download failed after {RetryDelays.Length + 1} attempts: {last?.Message}",
            last, (last as HttpRequestException)?.StatusCode);
    }

    static async Task DownloadOnce(HttpClient http, string url, string path, long size, CancellationToken token,
        Action<double>? progress, bool apiAsset)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, url);
        if (apiAsset) request.Headers.Accept.ParseAdd("application/octet-stream");
        using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, token);
        if (!response.IsSuccessStatusCode) throw HttpFailure(response);

        // ((Triforce) the size the release lists, which the server must not contradict; the server's only where none is listed (the
        // server's went first, and a short or other file passed as whole). Room for it asked before the first byte, and the bytes
        // written to <file>.part, which becomes the file only once complete: a failure removes it)
        var told = response.Content.Headers.ContentLength;
        if (size > 0 && told is { } other && other != size)
            throw new IOException($"The server offers {other} bytes as {Path.GetFileName(path)}, and the release lists {size}. Nothing was downloaded.");
        var total = size > 0 ? size : told ?? 0;
        if (total > 0 && NoRoom(path, total) is { } none) throw new IOException(none);
        var part = path + ".part";
        try
        {
            await using (var from = await response.Content.ReadAsStreamAsync(token))
            await using (var to = File.Create(part))
            {
                var buffer = new byte[1 << 20];
                long have = 0;
                var shown = DateTime.MinValue;
                int n;
                while (true)
                {
                    try { n = await from.ReadAsync(buffer, token); }
                    catch (IOException e) { throw new HttpRequestException("The download connection ended unexpectedly.", e); }
                    if (n == 0) break;
                    await to.WriteAsync(buffer.AsMemory(0, n), token);
                    have += n;
                    if (total > 0 && have > total) throw new IOException($"The server sent more than the {total} bytes of {Path.GetFileName(path)}.");
                    if (progress != null && total > 0 && DateTime.UtcNow - shown > TimeSpan.FromMilliseconds(200))
                    {
                        shown = DateTime.UtcNow;
                        progress(Math.Min(1.0, (double)have / total));
                    }
                }
                if (total > 0 && have != total) throw new HttpRequestException($"The download ended early ({have >> 20} of {total >> 20} MB).");
            }
            File.Move(part, path, true);
        }
        catch (Exception)
        {
            try { File.Delete(part); } catch (Exception) { }
            throw;
        }
    }

    /// ((Triforce) setup/Guard.cs Space, the same rule) Null when the disk of path has room for need bytes and 32 MB more (or
    /// cannot be asked); else the sentence that says how much it takes and how much is free
    static string? NoRoom(string path, long need)
    {
        long free;
        try { free = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(path))!).AvailableFreeSpace; }
        catch (Exception) { return null; }
        need += 32L << 20;
        return free >= need ? null
            : $"There is no room for {Path.GetFileName(path)} on {Path.GetPathRoot(Path.GetFullPath(path))}: it takes {(need + (1 << 20) - 1) >> 20} MB there, and {free >> 20} MB are free. Make room and try again.";
    }

    static readonly HashSet<string> Devices = new(StringComparer.OrdinalIgnoreCase) { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };

    /// ((Triforce) setup/Mods.cs Unsafe, the same rules) Why a zip's member must not be unpacked under its name, null when it may:
    /// a path from the disk's root or through "..", a colon (a drive's letter, a hidden stream), a name Windows keeps for a device
    /// (with or without an extension), a name that ends in a dot or a space (Windows drops them), a character Windows refuses
    internal static string? UnsafeName(string name)
    {
        var n = name.Replace('\\', '/');
        if (n.Trim().Length == 0) return "a file without a name";
        if (n[0] == '/') return "a path that starts at the disk's root";
        if (n.IndexOf(':') >= 0) return "a name with a colon or a drive's letter";
        foreach (var p in n.Split('/'))
        {
            if (p.Length == 0 || p == ".") continue;
            if (p == "..") return "a path that leaves its own folder (..)";
            if (p[p.Length - 1] == '.' || p[p.Length - 1] == ' ') return "a name that ends in a dot or a space, which Windows drops";
            var dot = p.IndexOf('.');
            var stem = (dot >= 0 ? p.Substring(0, dot) : p).TrimEnd(' ');
            if (Devices.Contains(stem) || stem.Length == 4 && (stem.StartsWith("COM", StringComparison.OrdinalIgnoreCase) || stem.StartsWith("LPT", StringComparison.OrdinalIgnoreCase)) &&
                (stem[3] is >= '1' and <= '9' || stem[3] == (char)0xB9 || stem[3] == (char)0xB2 || stem[3] == (char)0xB3))
                return "a name Windows keeps for a device (" + p + ")";
            if (p.Any(c => c < 32 || "<>\"|?*".IndexOf(c) >= 0)) return "a name with a character Windows does not allow";
        }
        return null;
    }

    static HttpRequestException HttpFailure(HttpResponseMessage response) =>
        new($"GitHub returned HTTP {(int)response.StatusCode} ({response.ReasonPhrase}).", null, response.StatusCode);

    static bool IsTransient(Exception error) =>
        error is HttpRequestException request && (request.StatusCode is null || (int)request.StatusCode >= 500);

    static string StatusDetail(Exception? error) =>
        error is HttpRequestException { StatusCode: { } status } ? $" (HTTP {(int)status})" : "";

    /// The zip's HookshotBuilder folder, as target (an older one there replaced)
    /// ((Triforce) every name checked first (UnsafeName: such a zip is refused whole), a failure removes the part, and an older
    /// target is moved aside and back when the new one cannot take its place: before, it was deleted first, and a move that
    /// failed then left neither)
    static async Task<string> Unpack(string zip, string target)
    {
        var part = target + ".part";
        if (Directory.Exists(part)) Directory.Delete(part, true);
        try
        {
            await Task.Run(() =>
            {
                using (var z = ZipFile.OpenRead(zip))
                    foreach (var e in z.Entries)
                        if (UnsafeName(e.FullName) is { } why)
                            throw new InvalidDataException($"The download holds {why} (\"{e.FullName}\"): it was not unpacked.");
                ZipFile.ExtractToDirectory(zip, part);
            });
        }
        catch (Exception)
        {
            try { if (Directory.Exists(part)) Directory.Delete(part, true); } catch (Exception) { }
            throw;
        }
        var inner = Path.Combine(part, "HookshotBuilder");
        var old = target + ".old";
        if (Directory.Exists(old)) Directory.Delete(old, true);
        if (Directory.Exists(target)) Directory.Move(target, old);
        try { Directory.Move(Directory.Exists(inner) ? inner : part, target); }
        catch (Exception)
        {
            try { if (!Directory.Exists(target) && Directory.Exists(old)) Directory.Move(old, target); } catch (Exception) { }
            try { if (Directory.Exists(part)) Directory.Delete(part, true); } catch (Exception) { }
            throw;
        }
        try { if (Directory.Exists(old)) Directory.Delete(old, true); } catch (Exception) { }
        if (Directory.Exists(part)) Directory.Delete(part, true);
        return target;
    }
}
