using System.Text;
using System.Text.RegularExpressions;
using Photino.NET;

namespace Triforce.Launcher.Host;

/// <summary>Linux: Photino (WebKitGTK) in a window the desktop frames (on a Steam Deck in Game Mode, gamescope shows it full
/// screen). Photino will not open a page on its own scheme as the first page, so the page is handed over as a string with
/// a base address on app://localhost/ui/, and everything it loads after that comes through the app:// scheme.
/// Dropped files: WebKitGTK puts file:// links in the drop's text/uri-list, so the page sends them in the JSON.</summary>
public sealed class LinuxHost : IHost
{
    public string Origin => "app://localhost/";
    public bool CustomChrome => false;
    public event Action<string, string[]>? Message;

    readonly Content content;
    readonly PhotinoWindow win;
    readonly List<string> early = new();
    bool pageReady;

    public LinuxHost(Content content, string dataDir, bool devTools)
    {
        this.content = content;
        win = new PhotinoWindow()
            .SetTitle("Triforce")
            .SetUseOsDefaultSize(false).SetSize(1280, 800).SetMinSize(1024, 640).Center()
            .SetResizable(true)
            .SetDevToolsEnabled(devTools).SetContextMenuEnabled(devTools)
            .SetGrantBrowserPermissions(true)
            .SetTemporaryFilesPath(dataDir)
            .RegisterCustomSchemeHandler("app", Serve)
            .RegisterWebMessageReceivedHandler((_, m) => OnMessage(m));
        var icon = Path.Combine(AppContext.BaseDirectory, "triforce.png");
        if (File.Exists(icon)) win.SetIconFile(icon);
    }

    Stream Serve(object sender, string scheme, string url, out string contentType)
    {
        var path = url.Substring(url.IndexOf("://", StringComparison.Ordinal) + 3);
        path = path.Substring(path.IndexOf('/') is var i and >= 0 ? i : path.Length);
        var got = content.Get(path);
        if (got is { } g) { contentType = g.mime; return new MemoryStream(g.data); }
        contentType = "text/plain";
        return new MemoryStream(Array.Empty<byte>());
    }

    void OnMessage(string json)
    {
        var files = new List<string>();
        foreach (Match m in Regex.Matches(json, "\"file://([^\"]+)\""))
            files.Add(Uri.UnescapeDataString(m.Groups[1].Value));
        if (json.Contains("\"t\":\"ready\"")) { pageReady = true; foreach (var p in early) win.SendWebMessage(p); early.Clear(); }
        Message?.Invoke(json, files.ToArray());
    }

    public void Run()
    {
        var page = content.Get("ui/index.html")!.Value.data;
        var html = Encoding.UTF8.GetString(page).Replace("<head>", "<head><base href=\"" + Origin + "ui/\">");
        win.LoadRawString(html);
        win.WaitForClose();
    }

    public void Post(string json) => Invoke(() =>
    {
        if (!pageReady) { early.Add(json); return; }
        win.SendWebMessage(json);
    });

    public void Invoke(Action a) => win.Invoke(a);
    public void Minimize() => Invoke(() => win.SetMinimized(true));
    public void ToggleMaximize() => Invoke(() => win.SetMaximized(!win.Maximized));
    public void Close() => Invoke(() => win.Close());
    public void Attention(string title, string text) => Invoke(() => { try { win.SendNotification(title, text); } catch { } });
    public void Activate() => Invoke(() => win.SetMinimized(false));

    public string? PickFile(string title, string filterName, string pattern)
    {
        var r = win.ShowOpenFile(title, null, false, new (string, string[])[] { (filterName, new[] { pattern }), ("All files", new[] { "*" }) });
        return r is { Length: > 0 } ? r[0] : null;
    }

    public string? PickFolder(string title)
    {
        var r = win.ShowOpenFolder(title, null, false);
        return r is { Length: > 0 } ? r[0] : null;
    }
}
